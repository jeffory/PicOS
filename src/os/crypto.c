#include "crypto.h"
#include "lua_psram_alloc.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/sha256.h"
#include "mbedtls/sha1.h"
#include "mbedtls/md.h"
#include "mbedtls/aes.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecp.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/bignum.h"
#include "mbedtls/asn1.h"
#include "mbedtls/platform_util.h"

#include <string.h>

// ── Module-level RNG ─────────────────────────────────────────────────────────
static mbedtls_ctr_drbg_context s_ctr_drbg;
static mbedtls_entropy_context  s_entropy;
static bool s_rng_seeded = false;

static int ensure_rng(void) {
    if (s_rng_seeded) return 0;
    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_ctr_drbg);
    int ret = mbedtls_ctr_drbg_seed(&s_ctr_drbg, mbedtls_entropy_func,
                                     &s_entropy, (const unsigned char *)"picos_crypto", 12);
    if (ret != 0) {
        mbedtls_ctr_drbg_free(&s_ctr_drbg);
        mbedtls_entropy_free(&s_entropy);
        return ret;
    }
    s_rng_seeded = true;
    return 0;
}

// ── Opaque struct definitions ─────────────────────────────────────────────────

struct crypto_aes_s {
    mbedtls_aes_context aes;
    uint8_t nonce_counter[16];
    uint8_t stream_block[16];
    size_t  nc_off;
    bool    valid;
};

struct crypto_ecdh_s {
    mbedtls_ecdh_context ctx;
    mbedtls_ecp_group_id grp_id;
    bool has_keypair;
    bool valid;
    uint8_t  pub_key[65];    // cached public key bytes (32=X25519, 65=P-256)
    uint32_t pub_key_len;    // 0 = not yet generated
};

// ── Hashing / MAC ─────────────────────────────────────────────────────────────

void crypto_sha256(const uint8_t *data, uint32_t len, uint8_t out[32]) {
    mbedtls_sha256(data, len, out, 0);
}

void crypto_sha1(const uint8_t *data, uint32_t len, uint8_t out[20]) {
    mbedtls_sha1(data, len, out);
}

void crypto_hmac_sha256(const uint8_t *key, uint32_t klen,
                        const uint8_t *data, uint32_t dlen,
                        uint8_t out[32]) {
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md_info) return;
    int ret = mbedtls_md_hmac(md_info, key, (size_t)klen, data, (size_t)dlen, out);
    if (ret != 0) memset(out, 0, 32);
}

void crypto_hmac_sha1(const uint8_t *key, uint32_t klen,
                      const uint8_t *data, uint32_t dlen,
                      uint8_t out[20]) {
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (!md_info) return;
    int ret = mbedtls_md_hmac(md_info, key, (size_t)klen, data, (size_t)dlen, out);
    if (ret != 0) memset(out, 0, 20);
}

void crypto_random_bytes(uint8_t *buf, uint32_t len) {
    if (ensure_rng() != 0) return;
    mbedtls_ctr_drbg_random(&s_ctr_drbg, buf, len);
}

// ── SSH session-key derivation (RFC 4253 §7.2) ───────────────────────────────

void crypto_derive_key(char letter,
                       const uint8_t *K, uint32_t k_len,
                       const uint8_t *H, uint32_t h_len,
                       const uint8_t *session_id, uint32_t sid_len,
                       uint8_t *out, uint32_t out_len) {
    if (out_len == 0 || out_len > 256) return;

    // First round: SHA256(K || H || letter || session_id)
    uint8_t result[256];
    uint32_t have = 0;

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, K, k_len);
    mbedtls_sha256_update(&ctx, H, h_len);
    mbedtls_sha256_update(&ctx, (const uint8_t *)&letter, 1);
    mbedtls_sha256_update(&ctx, session_id, sid_len);
    mbedtls_sha256_finish(&ctx, result);
    mbedtls_sha256_free(&ctx);
    have = 32;

    // Additional rounds if needed: SHA256(K || H || K1 || ... || Kn-1)
    while (have < (int)out_len) {
        if (have + 32 > 256) break;  // safety: never write past result[]
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts(&ctx, 0);
        mbedtls_sha256_update(&ctx, K, k_len);
        mbedtls_sha256_update(&ctx, H, h_len);
        mbedtls_sha256_update(&ctx, result, have);
        mbedtls_sha256_finish(&ctx, result + have);
        mbedtls_sha256_free(&ctx);
        have += 32;
    }

    memcpy(out, result, out_len);
    mbedtls_platform_zeroize(result, sizeof(result));
}

// ── AES-CTR ───────────────────────────────────────────────────────────────────

crypto_aes_t *crypto_aes_new(const uint8_t *key, uint32_t klen,
                              const uint8_t *nonce) {
    if (klen != 16 && klen != 24 && klen != 32) return NULL;

    crypto_aes_t *ctx = (crypto_aes_t *)umm_malloc(sizeof(crypto_aes_t));
    if (!ctx) return NULL;
    memset(ctx, 0, sizeof(crypto_aes_t));

    mbedtls_aes_init(&ctx->aes);
    int ret = mbedtls_aes_setkey_enc(&ctx->aes, key, (unsigned int)(klen * 8));
    if (ret != 0) {
        mbedtls_aes_free(&ctx->aes);
        umm_free(ctx);
        return NULL;
    }

    memcpy(ctx->nonce_counter, nonce, 16);
    ctx->nc_off = 0;
    ctx->valid = true;
    return ctx;
}

int crypto_aes_update(crypto_aes_t *ctx,
                      const uint8_t *in, uint8_t *out, uint32_t len) {
    if (!ctx || !ctx->valid) return -1;
    return mbedtls_aes_crypt_ctr(&ctx->aes, len,
                                  &ctx->nc_off, ctx->nonce_counter,
                                  ctx->stream_block, in, out);
}

void crypto_aes_free(crypto_aes_t *ctx) {
    if (!ctx) return;
    mbedtls_aes_free(&ctx->aes);
    umm_free(ctx);
}

// ── ECDH ──────────────────────────────────────────────────────────────────────

static crypto_ecdh_t *crypto_ecdh_new(mbedtls_ecp_group_id grp_id) {
    crypto_ecdh_t *ctx = (crypto_ecdh_t *)umm_malloc(sizeof(crypto_ecdh_t));
    if (!ctx) return NULL;
    memset(ctx, 0, sizeof(crypto_ecdh_t));

    mbedtls_ecdh_init(&ctx->ctx);
    int ret = mbedtls_ecdh_setup(&ctx->ctx, grp_id);
    if (ret != 0) {
        mbedtls_ecdh_free(&ctx->ctx);
        umm_free(ctx);
        return NULL;
    }

    ctx->grp_id = grp_id;
    ctx->has_keypair = false;
    ctx->valid = true;
    return ctx;
}

crypto_ecdh_t *crypto_ecdh_x25519(void) {
    return crypto_ecdh_new(MBEDTLS_ECP_DP_CURVE25519);
}

crypto_ecdh_t *crypto_ecdh_p256(void) {
    return crypto_ecdh_new(MBEDTLS_ECP_DP_SECP256R1);
}

void crypto_ecdh_get_public_key(crypto_ecdh_t *ctx, uint8_t *out, uint32_t *out_len) {
    if (!ctx || !ctx->valid) { *out_len = 0; return; }

    if (!ctx->has_keypair) {
        // Generate keypair (first call only)
        if (ensure_rng() != 0) { *out_len = 0; return; }
        uint8_t tls_buf[128];
        size_t olen = 0;
        int ret = mbedtls_ecdh_make_public(&ctx->ctx, &olen, tls_buf, sizeof(tls_buf),
                                            mbedtls_ctr_drbg_random, &s_ctr_drbg);
        if (ret != 0 || olen < 2) { *out_len = 0; return; }
        // Strip TLS 1-byte length prefix; save raw public key bytes
        uint32_t data_len = (uint32_t)(olen - 1);
        if (data_len > sizeof(ctx->pub_key)) { *out_len = 0; return; }
        memcpy(ctx->pub_key, tls_buf + 1, data_len);
        ctx->pub_key_len = data_len;
        ctx->has_keypair = true;
    }

    // Return cached public key
    uint32_t copy_len = ctx->pub_key_len < *out_len ? ctx->pub_key_len : *out_len;
    memcpy(out, ctx->pub_key, copy_len);
    *out_len = ctx->pub_key_len;
}

int crypto_ecdh_compute_shared(crypto_ecdh_t *ctx,
                                const uint8_t *remote, uint32_t rlen,
                                uint8_t *out, uint32_t *out_len) {
    if (!ctx || !ctx->valid) return -1;
    if (ensure_rng() != 0) return -1;

    // Wrap peer public key in TLS format: [1-byte length][point data]
    uint8_t tls_peer[128];
    if (rlen + 1 > sizeof(tls_peer)) return -1;
    tls_peer[0] = (uint8_t)rlen;
    memcpy(tls_peer + 1, remote, rlen);

    int ret = mbedtls_ecdh_read_public(&ctx->ctx, tls_peer, rlen + 1);
    if (ret != 0) return ret;

    uint8_t secret[32];
    size_t olen = 0;
    ret = mbedtls_ecdh_calc_secret(&ctx->ctx, &olen, secret, sizeof(secret),
                                    mbedtls_ctr_drbg_random, &s_ctr_drbg);
    if (ret != 0) return ret;

    uint32_t copy_len = (uint32_t)olen;
    if (out_len && *out_len < copy_len) copy_len = *out_len;
    if (out) memcpy(out, secret, copy_len);
    if (out_len) *out_len = (uint32_t)olen;
    return 0;
}

void crypto_ecdh_free(crypto_ecdh_t *ctx) {
    if (!ctx) return;
    mbedtls_ecdh_free(&ctx->ctx);
    umm_free(ctx);
}

// ── Signature verification ────────────────────────────────────────────────────

bool crypto_rsa_verify(const uint8_t *pubkey, uint32_t pklen,
                       const uint8_t *sig, uint32_t slen,
                       const uint8_t *hash, uint32_t hlen) {
    if (hlen != 32) return false;

    const unsigned char *p = pubkey;
    const unsigned char *end = pubkey + pklen;

    // Skip key type string ("ssh-rsa")
    if (p + 4 > end) return false;
    uint32_t str_len = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    if (str_len > (uint32_t)(end - p - 4)) return false;
    p += 4 + str_len;

    // Read e (public exponent)
    if (p + 4 > end) return false;
    uint32_t e_len = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    if (e_len > (uint32_t)(end - p - 4)) return false;
    p += 4;
    if (p + e_len > end) return false;
    const unsigned char *e_data = p;
    p += e_len;

    // Read n (modulus)
    if (p + 4 > end) return false;
    uint32_t n_len = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    if (n_len > (uint32_t)(end - p - 4)) return false;
    p += 4;
    if (p + n_len > end) return false;
    const unsigned char *n_data = p;

    mbedtls_rsa_context rsa;
    mbedtls_rsa_init(&rsa);

    int ret = mbedtls_rsa_import_raw(&rsa,
                                      n_data, n_len,
                                      NULL, 0,
                                      NULL, 0,
                                      NULL, 0,
                                      e_data, e_len);
    if (ret != 0) { mbedtls_rsa_free(&rsa); return false; }

    ret = mbedtls_rsa_complete(&rsa);
    if (ret != 0) { mbedtls_rsa_free(&rsa); return false; }

    // rsa-sha2-256: PKCS#1 v1.5 with SHA-256
    ret = mbedtls_rsa_pkcs1_verify(&rsa, MBEDTLS_MD_SHA256, 32, hash, sig);
    mbedtls_rsa_free(&rsa);
    return ret == 0;
}

bool crypto_ecdsa_p256_verify(const uint8_t *pubkey, uint32_t pklen,
                               const uint8_t *sig, uint32_t slen,
                               const uint8_t *hash, uint32_t hlen) {
    if (hlen != 32) return false;

    const unsigned char *p = pubkey;
    const unsigned char *end = pubkey + pklen;

    // Skip key type string
    if (p + 4 > end) return false;
    uint32_t str_len = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    if (str_len > (uint32_t)(end - p - 4)) return false;
    p += 4 + str_len;

    // Skip curve identifier string
    if (p + 4 > end) return false;
    str_len = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    if (str_len > (uint32_t)(end - p - 4)) return false;
    p += 4 + str_len;

    // Read Q (uncompressed point)
    if (p + 4 > end) return false;
    uint32_t q_len = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    if (q_len > (uint32_t)(end - p - 4)) return false;
    p += 4;
    if (p + q_len > end) return false;
    const unsigned char *q_data = p;

    mbedtls_ecp_group grp;
    mbedtls_ecp_point Q;
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&Q);

    bool result = false;
    int ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret != 0) goto cleanup_ec;

    ret = mbedtls_ecp_point_read_binary(&grp, &Q, q_data, q_len);
    if (ret != 0) goto cleanup_ec;

    {
        // Parse SSH signature: mpint(r) + mpint(s)
        const unsigned char *sp = sig;
        const unsigned char *send = sig + slen;

        if (sp + 4 > send) goto cleanup_ec;
        uint32_t r_len = ((uint32_t)sp[0] << 24) | ((uint32_t)sp[1] << 16) | ((uint32_t)sp[2] << 8) | sp[3];
        sp += 4;
        if (sp + r_len > send) goto cleanup_ec;
        const unsigned char *r_data = sp;
        sp += r_len;

        if (sp + 4 > send) goto cleanup_ec;
        uint32_t s_len_val = ((uint32_t)sp[0] << 24) | ((uint32_t)sp[1] << 16) | ((uint32_t)sp[2] << 8) | sp[3];
        sp += 4;
        if (sp + s_len_val > send) goto cleanup_ec;
        const unsigned char *s_data = sp;

        mbedtls_mpi r, s;
        mbedtls_mpi_init(&r);
        mbedtls_mpi_init(&s);

        ret = mbedtls_mpi_read_binary(&r, r_data, r_len);
        if (ret == 0) ret = mbedtls_mpi_read_binary(&s, s_data, s_len_val);
        if (ret == 0) ret = mbedtls_ecdsa_verify(&grp, hash, hlen, &Q, &r, &s);

        mbedtls_mpi_free(&r);
        mbedtls_mpi_free(&s);

        result = (ret == 0);
    }

cleanup_ec:
    mbedtls_ecp_point_free(&Q);
    mbedtls_ecp_group_free(&grp);
    return result;
}
