/*
 * apake_bench.c
 * ------------------------------------------------------------------
 * 제안 2-Round PQC aPAKE 성능 분석 벤치마크 (Fig. 5 그대로 구현)
 *   KEM = ML-KEM-768 (liboqs)
 *   AE  = AES-256-GCM (OpenSSL)   : ψ(hk), τ(π) 에만 사용
 *   IC1, IC2 = 이상암호(Ideal Cipher)  : C1, C2 에 사용, hp 로 키잉
 *              → 길이 보존 키드 퍼뮤테이션. 여기서는 벤치마크용으로
 *                AES-256-CTR 키스트림(길이 보존, 완전 가역)으로 인스턴스화.
 *                (실배포용으로는 large-domain PRP 권장; 성능 특성은 동일)
 *
 * 프리미티브 대응 (이미지 상단 정의):
 *   hp = H0(pw)        : IC1/IC2 키
 *   π  = H1(pw)        : τ 의 AE 키
 *   K0 = G(C‖S‖p̂k‖ĉ‖K̂)
 *   hk = H2(K0)        : ψ 의 AE 키
 *   ssk= H3(C‖S‖tr‖K0‖K),  tr = (C1, C2, ψ)
 *
 * 빌드:
 *   gcc -O2 -Wall -Wextra -Wno-deprecated-declarations \
 *       -I/usr/local/include -L/usr/local/lib \
 *       apake_bench.c -o apake_bench -loqs -lcrypto -lm
 *
 * 실행 (서버 먼저, runs/warmup 은 서버 값이 기준):
 *   서버:       ./apake_bench server [port] [measured_runs] [warmup_runs]
 *   클라이언트: ./apake_bench client [server_ip] [port] [measured_runs] [warmup_runs]
 *
 * 예시:
 *   ./apake_bench server 8080 1000 100
 *   ./apake_bench client 192.168.116.10 8080 1000 100
 * ------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>   /* TCP_NODELAY */
#include <sys/socket.h>

#include <openssl/sha.h>
#include <openssl/rand.h>
#include <openssl/evp.h>
#include <oqs/oqs.h>

/* =========================================================
 * 1. 상수 및 타입 정의
 * ========================================================= */
#define KEM_ALG              "ML-KEM-768"

/* ML-KEM-768 크기 (바이트) */
#define KEM_PK_BYTES         1184
#define KEM_SK_BYTES         2400
#define KEM_CT_BYTES         1088
#define KEM_SS_BYTES         32

/* AES-256-GCM 파라미터 */
#define AE_KEY_BYTES         32
#define AE_NONCE_BYTES       12
#define AE_TAG_BYTES         16

/* 해시 출력 크기 (SHA-256 = λ) */
#define LAMBDA_BYTES         32

/*
 * 메시지 크기
 *   C1  = IC1.Enc_hp(p̂k)          → 길이 보존 = KEM_PK  = 1184
 *   C2  = IC2.Enc_hp(ĉ)           → 길이 보존 = KEM_CT  = 1088
 *   τ   = AE.Enc_π(sk)            = nonce+SK+tag        = 2428
 *   ψ   = AE.Enc_hk(c ‖ τ)        = nonce+(CT+τ)+tag
 *         psi_plain = CT(1088) + τ(2428)               = 3516
 *         ψ         = 12 + 3516 + 16                    = 3544
 *   로그인 1회 총 전송량 = C1 + C2 + ψ                  = 5816
 */
#define TAU_BYTES            (AE_NONCE_BYTES + KEM_SK_BYTES + AE_TAG_BYTES)
#define C1_BYTES             (KEM_PK_BYTES)
#define C2_BYTES             (KEM_CT_BYTES)
#define PSI_PLAIN_BYTES      (KEM_CT_BYTES + TAU_BYTES)
#define PSI_BYTES            (AE_NONCE_BYTES + PSI_PLAIN_BYTES + AE_TAG_BYTES)
#define TOTAL_LOGIN_BYTES    (C1_BYTES + C2_BYTES + PSI_BYTES)

/*
 * BenchParams: 서버 → 클라이언트로 최초 전달되는 실험 설정
 * (클라이언트는 이 값(서버 기준)을 그대로 사용 → 양쪽 runs 자동 동기화)
 */
typedef struct {
    int measured_runs;
    int warmup_runs;
    int c1_bytes;
    int c2_bytes;
    int psi_bytes;
    int total_login_bytes;
} BenchParams;

/*
 * 서버가 등록(Registration) 단계에서 저장하는 레코드: Server(hp, pk, τ)
 *   hp  : H0(pw)                — IC 키
 *   pk  : 클라이언트 장기 공개키
 *   tau : AE.Enc_π(sk)          — π = H1(pw)
 */
typedef struct {
    uint8_t hp[LAMBDA_BYTES];
    uint8_t pk[KEM_PK_BYTES];
    uint8_t tau[TAU_BYTES];
} ServerRecord;

/*
 * 클라이언트 로그인 상태 (장기 sk 는 보관하지 않음 — τ 에서 복원)
 */
typedef struct {
    uint8_t eph_pk[KEM_PK_BYTES];   /* p̂k */
    uint8_t eph_sk[KEM_SK_BYTES];   /* ŝk */
    uint8_t hp[LAMBDA_BYTES];       /* H0(pw) */
    uint8_t K0[LAMBDA_BYTES];
    uint8_t C1[C1_BYTES];           /* transcript 용 */
} ClientState;

/* 전역 KEM 컨텍스트 */
static OQS_KEM *g_kem = NULL;

/* =========================================================
 * 2. 유틸리티: 시간 측정 / 소켓 I/O / 난수
 * ========================================================= */
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int readn(int fd, void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    size_t left = n;
    while (left > 0) {
        ssize_t r = read(fd, p, left);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return -1;
        p += r; left -= (size_t)r;
    }
    return 0;
}

static int writen(int fd, const void *buf, size_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t left = n;
    while (left > 0) {
        ssize_t w = write(fd, p, left);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        if (w == 0) return -1;
        p += w; left -= (size_t)w;
    }
    return 0;
}

/* 작은 요청-응답에서 Nagle 지연 제거 (정확한 RTT 측정에 필수) */
static void set_tcp_nodelay(int fd)
{
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

static void random_bytes(uint8_t *out, size_t len)
{
    if (RAND_bytes(out, (int)len) != 1) {
        fprintf(stderr, "RAND_bytes failed\n");
        exit(1);
    }
}

/* =========================================================
 * 3. 해시 / KDF  (H0, H1, H2, H3, G 를 SHA-256 + 도메인 레이블로 구현)
 * ========================================================= */

/* hp = H0(pw)  : IC1/IC2 키 */
static void H0_hp(const char *pw, uint8_t hp[LAMBDA_BYTES])
{
    SHA256_CTX c;
    SHA256_Init(&c);
    SHA256_Update(&c, "H0-hp", 5);
    SHA256_Update(&c, pw, strlen(pw));
    SHA256_Final(hp, &c);
}

/* π = H1(pw)  : τ 의 AE 키 */
static void H1_pi(const char *pw, uint8_t pi[LAMBDA_BYTES])
{
    SHA256_CTX c;
    SHA256_Init(&c);
    SHA256_Update(&c, "H1-pi", 5);
    SHA256_Update(&c, pw, strlen(pw));
    SHA256_Final(pi, &c);
}

/* K0 = G(C ‖ S ‖ p̂k ‖ ĉ ‖ K̂) */
static void G_K0(const char *C, const char *S,
                 const uint8_t eph_pk[KEM_PK_BYTES],
                 const uint8_t eph_ct[KEM_CT_BYTES],
                 const uint8_t eph_ss[KEM_SS_BYTES],
                 uint8_t K0[LAMBDA_BYTES])
{
    SHA256_CTX c;
    SHA256_Init(&c);
    SHA256_Update(&c, "G-K0", 4);
    SHA256_Update(&c, C, strlen(C));
    SHA256_Update(&c, S, strlen(S));
    SHA256_Update(&c, eph_pk, KEM_PK_BYTES);
    SHA256_Update(&c, eph_ct, KEM_CT_BYTES);
    SHA256_Update(&c, eph_ss, KEM_SS_BYTES);
    SHA256_Final(K0, &c);
}

/* hk = H2(K0)  : ψ 의 AE 키 */
static void H2_hk(const uint8_t K0[LAMBDA_BYTES], uint8_t hk[LAMBDA_BYTES])
{
    SHA256_CTX c;
    SHA256_Init(&c);
    SHA256_Update(&c, "H2-hk", 5);
    SHA256_Update(&c, K0, LAMBDA_BYTES);
    SHA256_Final(hk, &c);
}

/* ssk = H3(C ‖ S ‖ tr ‖ K0 ‖ K),  tr = (C1, C2, ψ) */
static void H3_ssk(const char *C, const char *S,
                   const uint8_t C1[C1_BYTES],
                   const uint8_t C2[C2_BYTES],
                   const uint8_t psi[PSI_BYTES],
                   const uint8_t K0[LAMBDA_BYTES],
                   const uint8_t K[KEM_SS_BYTES],
                   uint8_t ssk[LAMBDA_BYTES])
{
    SHA256_CTX c;
    SHA256_Init(&c);
    SHA256_Update(&c, "H3-ssk", 6);
    SHA256_Update(&c, C, strlen(C));
    SHA256_Update(&c, S, strlen(S));
    SHA256_Update(&c, C1,  C1_BYTES);   /* tr = (C1, C2, ψ) */
    SHA256_Update(&c, C2,  C2_BYTES);
    SHA256_Update(&c, psi, PSI_BYTES);
    SHA256_Update(&c, K0,  LAMBDA_BYTES);
    SHA256_Update(&c, K,   KEM_SS_BYTES);
    SHA256_Final(ssk, &c);
}

/* =========================================================
 * 4. 이상암호 IC1 / IC2  (hp 로 키잉된 길이 보존 변환)
 *    벤치마크용: AES-256-CTR 키스트림. Enc 와 Dec 가 동일 연산(가역).
 *    label 로 IC1("IC1")과 IC2("IC2")를 도메인 분리.
 * ========================================================= */
static int IC_crypt(const uint8_t hp[LAMBDA_BYTES], const char *label,
                    const uint8_t *in, uint8_t *out, size_t len)
{
    /* key = SHA256(label ‖ hp) */
    uint8_t key[AE_KEY_BYTES];
    SHA256_CTX c;
    SHA256_Init(&c);
    SHA256_Update(&c, label, strlen(label));
    SHA256_Update(&c, hp, LAMBDA_BYTES);
    SHA256_Final(key, &c);

    uint8_t iv[16] = {0};   /* 키가 hp 로 결정되는 결정적 퍼뮤테이션 */

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int ok = 0, outl = 0, total = 0;
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_ctr(), NULL, key, iv) != 1) goto done;
    if (EVP_EncryptUpdate(ctx, out, &outl, in, (int)len) != 1) goto done;
    total = outl;
    if (EVP_EncryptFinal_ex(ctx, out + total, &outl) != 1) goto done;
    total += outl;
    if ((size_t)total != len) goto done;
    ok = 1;
done:
    EVP_CIPHER_CTX_free(ctx);
    return ok ? 0 : -1;
}

/* =========================================================
 * 5. AES-256-GCM (AE)  — ψ, τ 에만 사용
 *    포맷: [ nonce(12) | ciphertext(pt_len) | tag(16) ]
 * ========================================================= */
static int AE_Enc(const uint8_t key[AE_KEY_BYTES],
                  const uint8_t *pt, size_t pt_len,
                  uint8_t *out, size_t out_len)
{
    if (out_len != AE_NONCE_BYTES + pt_len + AE_TAG_BYTES) return -1;

    uint8_t *nonce = out;
    uint8_t *ct    = out + AE_NONCE_BYTES;
    uint8_t *tag   = out + AE_NONCE_BYTES + pt_len;

    random_bytes(nonce, AE_NONCE_BYTES);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int ok = 0, len = 0, ct_len = 0;
    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto done;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, AE_NONCE_BYTES, NULL) != 1) goto done;
    if (EVP_EncryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) goto done;
    if (EVP_EncryptUpdate(ctx, ct, &len, pt, (int)pt_len) != 1) goto done;
    ct_len = len;
    if (EVP_EncryptFinal_ex(ctx, ct + ct_len, &len) != 1) goto done;
    ct_len += len;
    if ((size_t)ct_len != pt_len) goto done;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, AE_TAG_BYTES, tag) != 1) goto done;
    ok = 1;
done:
    EVP_CIPHER_CTX_free(ctx);
    return ok ? 0 : -1;
}

static int AE_Dec(const uint8_t key[AE_KEY_BYTES],
                  const uint8_t *in, size_t in_len,
                  uint8_t *pt, size_t pt_len)
{
    if (in_len != AE_NONCE_BYTES + pt_len + AE_TAG_BYTES) return -1;

    const uint8_t *nonce = in;
    const uint8_t *ct    = in + AE_NONCE_BYTES;
    const uint8_t *tag   = in + AE_NONCE_BYTES + pt_len;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return -1;

    int ok = 0, len = 0, pt_out = 0;
    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) goto done;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, AE_NONCE_BYTES, NULL) != 1) goto done;
    if (EVP_DecryptInit_ex(ctx, NULL, NULL, key, nonce) != 1) goto done;
    if (EVP_DecryptUpdate(ctx, pt, &len, ct, (int)pt_len) != 1) goto done;
    pt_out = len;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, AE_TAG_BYTES, (void *)tag) != 1) goto done;
    if (EVP_DecryptFinal_ex(ctx, pt + pt_out, &len) != 1) goto done;  /* 태그 실패 시 -1 */
    pt_out += len;
    if ((size_t)pt_out != pt_len) goto done;
    ok = 1;
done:
    EVP_CIPHER_CTX_free(ctx);
    return ok ? 0 : -1;
}

/* =========================================================
 * 6. ML-KEM-768 래퍼
 * ========================================================= */
static int KEM_KeyGen(uint8_t pk[KEM_PK_BYTES], uint8_t sk[KEM_SK_BYTES])
{
    return OQS_KEM_keypair(g_kem, pk, sk) == OQS_SUCCESS ? 0 : -1;
}
static int KEM_Encap(const uint8_t pk[KEM_PK_BYTES],
                     uint8_t ct[KEM_CT_BYTES], uint8_t ss[KEM_SS_BYTES])
{
    return OQS_KEM_encaps(g_kem, ct, ss, pk) == OQS_SUCCESS ? 0 : -1;
}
static int KEM_Decap(const uint8_t sk[KEM_SK_BYTES],
                     const uint8_t ct[KEM_CT_BYTES], uint8_t ss[KEM_SS_BYTES])
{
    return OQS_KEM_decaps(g_kem, ss, ct, sk) == OQS_SUCCESS ? 0 : -1;
}

/* =========================================================
 * 7. aPAKE 프로토콜 (Fig. 5)
 * ========================================================= */

/*
 * Registration
 *   (pk, sk) ← KEM.KeyGen ; hp = H0(pw) ; π = H1(pw) ; τ = AE.Enc_π(sk)
 *   서버 저장: Server(hp, pk, τ).  (클라이언트는 sk 를 버림)
 */
static int Registration(const char *pw, ServerRecord *rec)
{
    memset(rec, 0, sizeof(*rec));

    uint8_t sk[KEM_SK_BYTES];
    if (KEM_KeyGen(rec->pk, sk) != 0) return -1;

    H0_hp(pw, rec->hp);

    uint8_t pi[AE_KEY_BYTES];
    H1_pi(pw, pi);
    if (AE_Enc(pi, sk, KEM_SK_BYTES, rec->tau, TAU_BYTES) != 0) return -1;

    return 0;
}

/*
 * Client Round 1
 *   (p̂k, ŝk) ← KEM.KeyGen ; hp = H0(pw) ; C1 = IC1.Enc_hp(p̂k)
 */
static int Client_Round1(const char *pw, ClientState *st, uint8_t C1[C1_BYTES])
{
    memset(st, 0, sizeof(*st));

    if (KEM_KeyGen(st->eph_pk, st->eph_sk) != 0) return -1;
    H0_hp(pw, st->hp);

    if (IC_crypt(st->hp, "IC1", st->eph_pk, C1, C1_BYTES) != 0) return -1;
    memcpy(st->C1, C1, C1_BYTES);
    return 0;
}

/*
 * Server Round 2
 *   p̂k = IC1.Dec_hp(C1) ; (ĉ, K̂) ← KEM.Encap(p̂k)
 *   K0 = G(C‖S‖p̂k‖ĉ‖K̂) ; C2 = IC2.Enc_hp(ĉ)
 *   (c, K) ← KEM.Encap(pk) ; hk = H2(K0) ; ψ = AE.Enc_hk(c‖τ)
 *   tr = (C1,C2,ψ) ; ssk = H3(C‖S‖tr‖K0‖K)
 */
static int Server_Round2(const char *C, const char *S,
                         const ServerRecord *rec,
                         const uint8_t C1[C1_BYTES],
                         uint8_t C2[C2_BYTES],
                         uint8_t psi[PSI_BYTES],
                         uint8_t server_ssk[LAMBDA_BYTES])
{
    /* 1. p̂k = IC1.Dec_hp(C1)   (IC 는 가역: Enc 와 동일 연산) */
    uint8_t eph_pk[KEM_PK_BYTES];
    if (IC_crypt(rec->hp, "IC1", C1, eph_pk, C1_BYTES) != 0) return -1;

    /* 2. (ĉ, K̂) ← KEM.Encap(p̂k) */
    uint8_t eph_ct[KEM_CT_BYTES];
    uint8_t eph_ss[KEM_SS_BYTES];
    if (KEM_Encap(eph_pk, eph_ct, eph_ss) != 0) return -1;

    /* 3. K0 = G(...) */
    uint8_t K0[LAMBDA_BYTES];
    G_K0(C, S, eph_pk, eph_ct, eph_ss, K0);

    /* 4. C2 = IC2.Enc_hp(ĉ) */
    if (IC_crypt(rec->hp, "IC2", eph_ct, C2, C2_BYTES) != 0) return -1;

    /* 5. (c, K) ← KEM.Encap(pk) */
    uint8_t hid_ct[KEM_CT_BYTES];
    uint8_t hid_ss[KEM_SS_BYTES];
    if (KEM_Encap(rec->pk, hid_ct, hid_ss) != 0) return -1;

    /* 6. hk = H2(K0) ; ψ = AE.Enc_hk(c ‖ τ) */
    uint8_t psi_plain[PSI_PLAIN_BYTES];
    memcpy(psi_plain,                hid_ct,   KEM_CT_BYTES);
    memcpy(psi_plain + KEM_CT_BYTES, rec->tau, TAU_BYTES);

    uint8_t hk[AE_KEY_BYTES];
    H2_hk(K0, hk);
    if (AE_Enc(hk, psi_plain, PSI_PLAIN_BYTES, psi, PSI_BYTES) != 0) return -1;

    /* 7. ssk = H3(C‖S‖tr‖K0‖K),  tr=(C1,C2,ψ),  K=hid_ss */
    H3_ssk(C, S, C1, C2, psi, K0, hid_ss, server_ssk);
    return 0;
}

/*
 * Client Finish
 *   ĉ = IC2.Dec_hp(C2) ; K̂ ← KEM.Decap(ŝk, ĉ) ; K0 = G(...)
 *   hk = H2(K0) ; (c‖τ) = AE.Dec_hk(ψ)
 *   π = H1(pw) ; sk = AE.Dec_π(τ) ; K ← KEM.Decap(sk, c)
 *   ssk = H3(C‖S‖tr‖K0‖K)
 *   반환: 0=성공, -1=인증/복호 실패(잘못된 pw 등)
 */
static int Client_Finish(const char *C, const char *S, const char *pw,
                         ClientState *st,
                         const uint8_t C2[C2_BYTES],
                         const uint8_t psi[PSI_BYTES],
                         uint8_t client_ssk[LAMBDA_BYTES])
{
    /* 1. ĉ = IC2.Dec_hp(C2) */
    uint8_t eph_ct[KEM_CT_BYTES];
    if (IC_crypt(st->hp, "IC2", C2, eph_ct, C2_BYTES) != 0) return -1;

    /* 2. K̂ ← KEM.Decap(ŝk, ĉ) */
    uint8_t eph_ss[KEM_SS_BYTES];
    if (KEM_Decap(st->eph_sk, eph_ct, eph_ss) != 0) return -1;

    /* 3. K0 = G(...) */
    uint8_t K0[LAMBDA_BYTES];
    G_K0(C, S, st->eph_pk, eph_ct, eph_ss, K0);
    memcpy(st->K0, K0, LAMBDA_BYTES);

    /* 4. hk = H2(K0) ; (c ‖ τ) = AE.Dec_hk(ψ) */
    uint8_t hk[AE_KEY_BYTES];
    H2_hk(K0, hk);

    uint8_t psi_plain[PSI_PLAIN_BYTES];
    if (AE_Dec(hk, psi, PSI_BYTES, psi_plain, PSI_PLAIN_BYTES) != 0) return -1;

    uint8_t hid_ct[KEM_CT_BYTES];
    uint8_t tau[TAU_BYTES];
    memcpy(hid_ct, psi_plain,                KEM_CT_BYTES);
    memcpy(tau,    psi_plain + KEM_CT_BYTES,  TAU_BYTES);

    /* 5. π = H1(pw) ; sk = AE.Dec_π(τ) */
    uint8_t pi[AE_KEY_BYTES];
    H1_pi(pw, pi);
    uint8_t sk[KEM_SK_BYTES];
    if (AE_Dec(pi, tau, TAU_BYTES, sk, KEM_SK_BYTES) != 0) return -1;

    /* 6. K ← KEM.Decap(sk, c) */
    uint8_t hid_ss[KEM_SS_BYTES];
    if (KEM_Decap(sk, hid_ct, hid_ss) != 0) return -1;

    /* 7. ssk = H3(...) */
    H3_ssk(C, S, st->C1, C2, psi, K0, hid_ss, client_ssk);
    return 0;
}

/* =========================================================
 * 8. 등록 레코드 직렬화
 * ========================================================= */
static int send_record(int fd, const ServerRecord *r)
{
    if (writen(fd, r->hp,  LAMBDA_BYTES) != 0) return -1;
    if (writen(fd, r->pk,  KEM_PK_BYTES) != 0) return -1;
    if (writen(fd, r->tau, TAU_BYTES)    != 0) return -1;
    return 0;
}
static int recv_record(int fd, ServerRecord *r)
{
    if (readn(fd, r->hp,  LAMBDA_BYTES) != 0) return -1;
    if (readn(fd, r->pk,  KEM_PK_BYTES) != 0) return -1;
    if (readn(fd, r->tau, TAU_BYTES)    != 0) return -1;
    return 0;
}

/* =========================================================
 * 9. 서버 메인 루프
 * ========================================================= */
static int run_server(int port, int measured_runs, int warmup_runs)
{
    const char *C = "Client-C";
    const char *S = "Server-S";
    int total_runs = measured_runs + warmup_runs;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(listen_fd); return 1;
    }
    if (listen(listen_fd, 1) < 0) {
        perror("listen"); close(listen_fd); return 1;
    }

    printf("[SERVER] 포트 %d 에서 대기 중...\n", port);
    fflush(stdout);

    struct sockaddr_in cli_addr;
    socklen_t cli_len = sizeof(cli_addr);
    int fd = accept(listen_fd, (struct sockaddr *)&cli_addr, &cli_len);
    if (fd < 0) { perror("accept"); close(listen_fd); return 1; }
    set_tcp_nodelay(fd);

    char ipbuf[64];
    inet_ntop(AF_INET, &cli_addr.sin_addr, ipbuf, sizeof(ipbuf));
    printf("[SERVER] 클라이언트 연결: %s\n", ipbuf);
    fflush(stdout);

    /* 실험 설정 전송 (클라이언트가 이 값을 그대로 사용) */
    BenchParams bp;
    bp.measured_runs     = measured_runs;
    bp.warmup_runs       = warmup_runs;
    bp.c1_bytes          = C1_BYTES;
    bp.c2_bytes          = C2_BYTES;
    bp.psi_bytes         = PSI_BYTES;
    bp.total_login_bytes = TOTAL_LOGIN_BYTES;
    if (writen(fd, &bp, sizeof(bp)) != 0) {
        fprintf(stderr, "BenchParams 전송 실패\n");
        close(fd); close(listen_fd); return 1;
    }

    /* 등록 레코드 수신 */
    ServerRecord rec;
    if (recv_record(fd, &rec) != 0) {
        fprintf(stderr, "등록 레코드 수신 실패\n");
        close(fd); close(listen_fd); return 1;
    }
    printf("[SERVER] 등록 레코드 수신 완료 (프로토콜 시작)\n");
    fflush(stdout);

    printf("run,server_round2_ns\n");
    fflush(stdout);

    int mismatch = 0;

    for (int i = 0; i < total_runs; i++) {
        uint8_t C1[C1_BYTES];
        uint8_t C2[C2_BYTES];
        uint8_t psi[PSI_BYTES];
        uint8_t server_ssk[LAMBDA_BYTES];
        uint8_t client_ssk[LAMBDA_BYTES];

        if (readn(fd, C1, C1_BYTES) != 0) {
            fprintf(stderr, "C1 수신 실패 (run=%d)\n", i); break;
        }

        /* ===== 측정 구간 ===== */
        uint64_t t0 = now_ns();
        if (Server_Round2(C, S, &rec, C1, C2, psi, server_ssk) != 0) {
            fprintf(stderr, "Server_Round2 실패 (run=%d)\n", i); break;
        }
        uint64_t t1 = now_ns();
        /* ===================== */

        if (writen(fd, C2,  C2_BYTES)  != 0) break;
        if (writen(fd, psi, PSI_BYTES) != 0) break;

        /* 정합성 검증: 클라이언트 ssk 수신 후 비교 */
        if (readn(fd, client_ssk, LAMBDA_BYTES) != 0) break;
        if (memcmp(server_ssk, client_ssk, LAMBDA_BYTES) != 0) mismatch++;

        if (i >= warmup_runs) {
            printf("%d,%lu\n", i - warmup_runs, (unsigned long)(t1 - t0));
            fflush(stdout);
        }
    }

    if (mismatch)
        fprintf(stderr, "[경고] ssk 불일치 %d 회 (프로토콜/설정 확인 필요)\n", mismatch);
    else
        fprintf(stderr, "[OK] 모든 run 에서 ssk 일치\n");

    close(fd);
    close(listen_fd);
    printf("[SERVER] 실험 완료\n");
    return 0;
}

/* =========================================================
 * 10. 클라이언트 메인 루프
 * ========================================================= */
static int run_client(const char *server_ip, int port,
                      int measured_runs, int warmup_runs)
{
    const char *C  = "Client-C";
    const char *S  = "Server-S";
    const char *pw = "correct horse battery staple";

    (void)measured_runs; (void)warmup_runs;  /* 서버 값(bp)을 사용 */

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, server_ip, &serv.sin_addr) != 1) {
        fprintf(stderr, "잘못된 서버 IP: %s\n", server_ip);
        close(fd); return 1;
    }
    if (connect(fd, (struct sockaddr *)&serv, sizeof(serv)) < 0) {
        perror("connect"); close(fd); return 1;
    }
    set_tcp_nodelay(fd);
    printf("[CLIENT] 서버 %s:%d 연결 완료\n", server_ip, port);

    /* 실험 설정 수신 → 서버 값 채택 */
    BenchParams bp;
    if (readn(fd, &bp, sizeof(bp)) != 0) {
        fprintf(stderr, "BenchParams 수신 실패\n"); close(fd); return 1;
    }
    int m_runs = bp.measured_runs;
    int w_runs = bp.warmup_runs;
    int total_runs = m_runs + w_runs;
    printf("[CLIENT] 설정: measured=%d warmup=%d\n", m_runs, w_runs);
    printf("[CLIENT] 메시지 크기: C1=%d C2=%d PSI=%d 총=%d bytes\n",
           bp.c1_bytes, bp.c2_bytes, bp.psi_bytes, bp.total_login_bytes);
    fflush(stdout);

    /* 등록(1회) 후 레코드 전송 */
    ServerRecord rec;
    if (Registration(pw, &rec) != 0) {
        fprintf(stderr, "Registration 실패\n"); close(fd); return 1;
    }
    if (send_record(fd, &rec) != 0) {
        fprintf(stderr, "등록 레코드 전송 실패\n"); close(fd); return 1;
    }
    printf("[CLIENT] 등록 레코드 전송 완료 (프로토콜 시작)\n");

    printf("run,client_round1_ns,net_rtt_ns,client_finish_ns,total_login_ns\n");
    fflush(stdout);

    int fail = 0;

    for (int i = 0; i < total_runs; i++) {
        ClientState st;
        uint8_t C1[C1_BYTES];
        uint8_t C2[C2_BYTES];
        uint8_t psi[PSI_BYTES];
        uint8_t client_ssk[LAMBDA_BYTES];

        /* --- Round 1 --- */
        uint64_t t0 = now_ns();
        if (Client_Round1(pw, &st, C1) != 0) {
            fprintf(stderr, "Client_Round1 실패 (run=%d)\n", i); break;
        }
        uint64_t t1 = now_ns();

        if (writen(fd, C1, C1_BYTES) != 0) {
            fprintf(stderr, "C1 전송 실패 (run=%d)\n", i); break;
        }

        /* --- 서버 응답 수신 (네트워크 RTT + 서버 연산 포함) --- */
        if (readn(fd, C2,  C2_BYTES)  != 0) { fprintf(stderr, "C2 수신 실패\n"); break; }
        if (readn(fd, psi, PSI_BYTES) != 0) { fprintf(stderr, "PSI 수신 실패\n"); break; }
        uint64_t t2 = now_ns();

        /* --- Finish --- */
        if (Client_Finish(C, S, pw, &st, C2, psi, client_ssk) != 0) {
            fprintf(stderr, "Client_Finish 실패 (run=%d)\n", i);
            fail++;
            break;
        }
        uint64_t t3 = now_ns();

        /* ssk 를 서버로 전송(정합성 검증 및 동기화) */
        if (writen(fd, client_ssk, LAMBDA_BYTES) != 0) break;

        if (i >= w_runs) {
            printf("%d,%lu,%lu,%lu,%lu\n",
                   i - w_runs,
                   (unsigned long)(t1 - t0),
                   (unsigned long)(t2 - t1),
                   (unsigned long)(t3 - t2),
                   (unsigned long)(t3 - t0));
            fflush(stdout);
        }
    }

    if (fail) fprintf(stderr, "[경고] Client_Finish 실패 %d 회\n", fail);

    close(fd);
    printf("[CLIENT] 실험 완료\n");
    return 0;
}

/* =========================================================
 * 11. KEM 초기화 및 main
 * ========================================================= */
static int init_kem(void)
{
    g_kem = OQS_KEM_new(KEM_ALG);
    if (!g_kem) {
        fprintf(stderr, "OQS_KEM_new 실패: %s (liboqs 에 ML-KEM-768 미포함?)\n", KEM_ALG);
        return -1;
    }
    if (g_kem->length_public_key   != KEM_PK_BYTES ||
        g_kem->length_secret_key   != KEM_SK_BYTES ||
        g_kem->length_ciphertext   != KEM_CT_BYTES ||
        g_kem->length_shared_secret != KEM_SS_BYTES) {
        fprintf(stderr,
            "ML-KEM-768 크기 불일치:\n"
            "  pk=%zu(기대 %d) sk=%zu(기대 %d) ct=%zu(기대 %d) ss=%zu(기대 %d)\n",
            g_kem->length_public_key,   KEM_PK_BYTES,
            g_kem->length_secret_key,   KEM_SK_BYTES,
            g_kem->length_ciphertext,   KEM_CT_BYTES,
            g_kem->length_shared_secret, KEM_SS_BYTES);
        OQS_KEM_free(g_kem); g_kem = NULL;
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (init_kem() != 0) return 1;

    int ret = 0;
    if (argc < 2) {
        printf("사용법:\n");
        printf("  서버:       %s server [port] [measured_runs] [warmup_runs]\n", argv[0]);
        printf("  클라이언트: %s client [server_ip] [port] [measured_runs] [warmup_runs]\n", argv[0]);
        printf("\n예시:\n");
        printf("  %s server 8080 1000 100\n", argv[0]);
        printf("  %s client 192.168.116.10 8080 1000 100\n", argv[0]);
        printf("\n메시지 크기(bytes): C1=%d C2=%d TAU=%d PSI=%d  총 전송=%d\n",
               C1_BYTES, C2_BYTES, TAU_BYTES, PSI_BYTES, TOTAL_LOGIN_BYTES);
    } else if (strcmp(argv[1], "server") == 0) {
        int port     = argc > 2 ? atoi(argv[2]) : 8080;
        int m        = argc > 3 ? atoi(argv[3]) : 1000;
        int w        = argc > 4 ? atoi(argv[4]) : 100;
        ret = run_server(port, m, w);
    } else if (strcmp(argv[1], "client") == 0) {
        const char *ip = argc > 2 ? argv[2] : "127.0.0.1";
        int port       = argc > 3 ? atoi(argv[3]) : 8080;
        int m          = argc > 4 ? atoi(argv[4]) : 1000;
        int w          = argc > 5 ? atoi(argv[5]) : 100;
        ret = run_client(ip, port, m, w);
    } else {
        fprintf(stderr, "알 수 없는 모드: %s\n", argv[1]);
        ret = 1;
    }

    if (g_kem) OQS_KEM_free(g_kem);
    return ret;
}
