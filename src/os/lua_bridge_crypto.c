#include "lua_bridge_internal.h"

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

// ── Module-level RNG ────────────────────────────────────────────────────────
static mbedtls_ctr_drbg_context s_ctr_drbg;
static mbedtls_entropy_context  s_entropy;
static bool s_rng_seeded = false;

static int ensure_rng(void) {
    if (s_rng_seeded) return 0;
    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_ctr_drbg);
    int ret = mbedtls_ctr_drbg_seed(&s_ctr_drbg, mbedtls_entropy_func,
                                     &s_entropy, (const unsigned char *)"picos_crypto", 12);
    if (ret == 0) s_rng_seeded = true;
    return ret;
}

// ── AES-CTR cipher userdata ─────────────────────────────────────────────────
#define AES_CTR_MT "picocalc.crypto.aes_ctr"

typedef struct {
    mbedtls_aes_context aes;
    uint8_t nonce_counter[16];
    uint8_t stream_block[16];
    size_t  nc_off;
    bool    valid;
} aes_ctr_ud_t;

static aes_ctr_ud_t *check_aes_ctr(lua_State *L, int idx) {
    aes_ctr_ud_t *ud = (aes_ctr_ud_t *)luaL_checkudata(L, idx, AES_CTR_MT);
    if (!ud->valid) luaL_error(L, "aes_ctr: cipher has been freed");
    return ud;
}

static int l_aes_ctr_update(lua_State *L) {
    aes_ctr_ud_t *ud = check_aes_ctr(L, 1);
    size_t len;
    const char *input = luaL_checklstring(L, 2, &len);

    uint8_t *output = umm_malloc(len);
    if (!output) return luaL_error(L, "aes_ctr: out of memory");

    int ret = mbedtls_aes_crypt_ctr(&ud->aes, len,
                                     &ud->nc_off, ud->nonce_counter,
                                     ud->stream_block,
                                     (const unsigned char *)input, output);
    if (ret != 0) {
        umm_free(output);
        return luaL_error(L, "aes_ctr: encrypt/decrypt failed (%d)", ret);
    }

    lua_pushlstring(L, (const char *)output, len);
    umm_free(output);
    return 1;
}

static int l_aes_ctr_free(lua_State *L) {
    aes_ctr_ud_t *ud = (aes_ctr_ud_t *)luaL_checkudata(L, 1, AES_CTR_MT);
    if (ud->valid) {
        mbedtls_aes_free(&ud->aes);
        ud->valid = false;
    }
    return 0;
}

static const luaL_Reg l_aes_ctr_methods[] = {
    {"update", l_aes_ctr_update},
    {"free",   l_aes_ctr_free},
    {NULL, NULL}
};

// ── ECDH userdata (X25519 and P-256) ────────────────────────────────────────
#define ECDH_MT "picocalc.crypto.ecdh"

typedef struct {
    mbedtls_ecdh_context ctx;
    mbedtls_ecp_group_id grp_id;
    bool has_keypair;
    bool valid;
} ecdh_ud_t;

static ecdh_ud_t *check_ecdh(lua_State *L, int idx) {
    ecdh_ud_t *ud = (ecdh_ud_t *)luaL_checkudata(L, idx, ECDH_MT);
    if (!ud->valid) luaL_error(L, "ecdh: context has been freed");
    return ud;
}

static int l_ecdh_get_public_key(lua_State *L) {
    ecdh_ud_t *ud = check_ecdh(L, 1);

    if (!ud->has_keypair) {
        if (ensure_rng() != 0)
            return luaL_error(L, "ecdh: RNG init failed");

        // mbedtls_ecdh_make_public generates keypair and outputs public key
        // in TLS format: [1-byte length][point data]
        uint8_t tls_buf[128];
        size_t olen = 0;
        int ret = mbedtls_ecdh_make_public(&ud->ctx, &olen,
                                            tls_buf, sizeof(tls_buf),
                                            mbedtls_ctr_drbg_random, &s_ctr_drbg);
        if (ret != 0)
            return luaL_error(L, "ecdh: make_public failed (%d)", ret);
        ud->has_keypair = true;

        // Strip the TLS length byte — return raw point data
        // tls_buf[0] = length, tls_buf[1..olen-1] = point data
        lua_pushlstring(L, (const char *)tls_buf + 1, olen - 1);
    } else {
        // Already generated — re-export by calling make_public again
        // (it's idempotent once keypair exists)
        uint8_t tls_buf[128];
        size_t olen = 0;
        int ret = mbedtls_ecdh_make_public(&ud->ctx, &olen,
                                            tls_buf, sizeof(tls_buf),
                                            mbedtls_ctr_drbg_random, &s_ctr_drbg);
        if (ret != 0)
            return luaL_error(L, "ecdh: export pubkey failed (%d)", ret);
        lua_pushlstring(L, (const char *)tls_buf + 1, olen - 1);
    }
    return 1;
}

static int l_ecdh_compute_shared(lua_State *L) {
    ecdh_ud_t *ud = check_ecdh(L, 1);
    size_t peer_len;
    const char *peer_pub = luaL_checklstring(L, 2, &peer_len);

    if (ensure_rng() != 0)
        return luaL_error(L, "ecdh: RNG init failed");

    // Wrap peer public key in TLS format: [1-byte length][point data]
    uint8_t tls_peer[128];
    if (peer_len + 1 > sizeof(tls_peer))
        return luaL_error(L, "ecdh: peer key too long");
    tls_peer[0] = (uint8_t)peer_len;
    memcpy(tls_peer + 1, peer_pub, peer_len);

    int ret = mbedtls_ecdh_read_public(&ud->ctx, tls_peer, peer_len + 1);
    if (ret != 0)
        return luaL_error(L, "ecdh: read_public failed (%d)", ret);

    // Compute shared secret
    uint8_t secret[32];
    size_t olen = 0;
    ret = mbedtls_ecdh_calc_secret(&ud->ctx, &olen, secret, sizeof(secret),
                                    mbedtls_ctr_drbg_random, &s_ctr_drbg);
    if (ret != 0)
        return luaL_error(L, "ecdh: calc_secret failed (%d)", ret);

    lua_pushlstring(L, (const char *)secret, olen);
    return 1;
}

static int l_ecdh_free(lua_State *L) {
    ecdh_ud_t *ud = (ecdh_ud_t *)luaL_checkudata(L, 1, ECDH_MT);
    if (ud->valid) {
        mbedtls_ecdh_free(&ud->ctx);
        ud->valid = false;
    }
    return 0;
}

static const luaL_Reg l_ecdh_methods[] = {
    {"getPublicKey",  l_ecdh_get_public_key},
    {"computeShared", l_ecdh_compute_shared},
    {"free",          l_ecdh_free},
    {NULL, NULL}
};

// ── Top-level crypto functions ──────────────────────────────────────────────

static int l_crypto_random_bytes(lua_State *L) {
    int n = (int)luaL_checkinteger(L, 1);
    if (n <= 0 || n > 4096)
        return luaL_error(L, "randomBytes: n must be 1-4096");

    if (ensure_rng() != 0)
        return luaL_error(L, "randomBytes: RNG init failed");

    uint8_t *buf = umm_malloc((size_t)n);
    if (!buf) return luaL_error(L, "randomBytes: out of memory");

    int ret = mbedtls_ctr_drbg_random(&s_ctr_drbg, buf, (size_t)n);
    if (ret != 0) {
        umm_free(buf);
        return luaL_error(L, "randomBytes: generation failed (%d)", ret);
    }

    lua_pushlstring(L, (const char *)buf, (size_t)n);
    umm_free(buf);
    return 1;
}

static int l_crypto_sha256(lua_State *L) {
    size_t len;
    const char *data = luaL_checklstring(L, 1, &len);
    uint8_t hash[32];

    int ret = mbedtls_sha256((const unsigned char *)data, len, hash, 0);
    if (ret != 0)
        return luaL_error(L, "sha256 failed (%d)", ret);

    lua_pushlstring(L, (const char *)hash, 32);
    return 1;
}

static int l_crypto_sha1(lua_State *L) {
    size_t len;
    const char *data = luaL_checklstring(L, 1, &len);
    uint8_t hash[20];

    int ret = mbedtls_sha1((const unsigned char *)data, len, hash);
    if (ret != 0)
        return luaL_error(L, "sha1 failed (%d)", ret);

    lua_pushlstring(L, (const char *)hash, 20);
    return 1;
}

static int l_crypto_hmac(lua_State *L, mbedtls_md_type_t md_type, int hash_len) {
    size_t key_len, data_len;
    const char *key = luaL_checklstring(L, 1, &key_len);
    const char *data = luaL_checklstring(L, 2, &data_len);
    uint8_t hash[32]; // max output size (SHA-256)

    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(md_type);
    if (!md_info)
        return luaL_error(L, "hmac: unsupported hash type");

    int ret = mbedtls_md_hmac(md_info,
                               (const unsigned char *)key, key_len,
                               (const unsigned char *)data, data_len,
                               hash);
    if (ret != 0)
        return luaL_error(L, "hmac failed (%d)", ret);

    lua_pushlstring(L, (const char *)hash, (size_t)hash_len);
    return 1;
}

static int l_crypto_hmac_sha256(lua_State *L) {
    return l_crypto_hmac(L, MBEDTLS_MD_SHA256, 32);
}

static int l_crypto_hmac_sha1(lua_State *L) {
    return l_crypto_hmac(L, MBEDTLS_MD_SHA1, 20);
}

// SSH key derivation function (RFC 4253 §7.2)
// deriveKey(K_mpint, H, session_id, letter, needed_len) → string
static int l_crypto_derive_key(lua_State *L) {
    size_t k_len, h_len, sid_len;
    const char *K = luaL_checklstring(L, 1, &k_len);       // K as mpint (already encoded)
    const char *H = luaL_checklstring(L, 2, &h_len);       // exchange hash
    const char *sid = luaL_checklstring(L, 3, &sid_len);   // session ID
    const char *letter_str = luaL_checkstring(L, 4);        // single char A-F
    int needed = (int)luaL_checkinteger(L, 5);

    if (needed <= 0 || needed > 256)
        return luaL_error(L, "deriveKey: needed must be 1-256");

    char letter = letter_str[0];

    // First round: SHA256(K || H || letter || session_id)
    uint8_t result[256];
    int have = 0;

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, (const unsigned char *)K, k_len);
    mbedtls_sha256_update(&ctx, (const unsigned char *)H, h_len);
    mbedtls_sha256_update(&ctx, (const unsigned char *)&letter, 1);
    mbedtls_sha256_update(&ctx, (const unsigned char *)sid, sid_len);
    mbedtls_sha256_finish(&ctx, result);
    mbedtls_sha256_free(&ctx);
    have = 32;

    // Additional rounds if needed: SHA256(K || H || K1 || ... || Kn-1)
    while (have < needed) {
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts(&ctx, 0);
        mbedtls_sha256_update(&ctx, (const unsigned char *)K, k_len);
        mbedtls_sha256_update(&ctx, (const unsigned char *)H, h_len);
        mbedtls_sha256_update(&ctx, result, (size_t)have);
        mbedtls_sha256_finish(&ctx, result + have);
        mbedtls_sha256_free(&ctx);
        have += 32;
    }

    lua_pushlstring(L, (const char *)result, (size_t)needed);
    return 1;
}

static int l_crypto_aes_ctr_new(lua_State *L) {
    size_t key_len, iv_len;
    const char *key = luaL_checklstring(L, 1, &key_len);
    const char *iv = luaL_checklstring(L, 2, &iv_len);

    if (key_len != 16 && key_len != 32)
        return luaL_error(L, "aes_ctr_new: key must be 16 or 32 bytes");
    if (iv_len != 16)
        return luaL_error(L, "aes_ctr_new: iv must be 16 bytes");

    aes_ctr_ud_t *ud = (aes_ctr_ud_t *)lua_newuserdata(L, sizeof(aes_ctr_ud_t));
    memset(ud, 0, sizeof(aes_ctr_ud_t));

    mbedtls_aes_init(&ud->aes);
    int ret = mbedtls_aes_setkey_enc(&ud->aes, (const unsigned char *)key, (unsigned int)(key_len * 8));
    if (ret != 0) {
        mbedtls_aes_free(&ud->aes);
        return luaL_error(L, "aes_ctr_new: setkey failed (%d)", ret);
    }

    memcpy(ud->nonce_counter, iv, 16);
    ud->nc_off = 0;
    ud->valid = true;

    luaL_getmetatable(L, AES_CTR_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static int l_crypto_ecdh_new(lua_State *L, mbedtls_ecp_group_id grp_id) {
    ecdh_ud_t *ud = (ecdh_ud_t *)lua_newuserdata(L, sizeof(ecdh_ud_t));
    memset(ud, 0, sizeof(ecdh_ud_t));

    mbedtls_ecdh_init(&ud->ctx);
    int ret = mbedtls_ecdh_setup(&ud->ctx, grp_id);
    if (ret != 0) {
        mbedtls_ecdh_free(&ud->ctx);
        return luaL_error(L, "ecdh_new: setup failed (%d)", ret);
    }

    ud->grp_id = grp_id;
    ud->has_keypair = false;
    ud->valid = true;

    luaL_getmetatable(L, ECDH_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static int l_crypto_ecdh_x25519_new(lua_State *L) {
    return l_crypto_ecdh_new(L, MBEDTLS_ECP_DP_CURVE25519);
}

static int l_crypto_ecdh_p256_new(lua_State *L) {
    return l_crypto_ecdh_new(L, MBEDTLS_ECP_DP_SECP256R1);
}

// rsaVerify(pubkey_blob, sig_blob, hash) → bool
// pubkey_blob is SSH wire format: string("ssh-rsa") + mpint(e) + mpint(n)
// sig_blob is the raw signature bytes (PKCS#1 v1.5)
static int l_crypto_rsa_verify(lua_State *L) {
    size_t blob_len, sig_len, hash_len;
    const unsigned char *blob = (const unsigned char *)luaL_checklstring(L, 1, &blob_len);
    const unsigned char *sig = (const unsigned char *)luaL_checklstring(L, 2, &sig_len);
    const unsigned char *hash = (const unsigned char *)luaL_checklstring(L, 3, &hash_len);

    if (hash_len != 32)
        return luaL_error(L, "rsaVerify: hash must be 32 bytes (SHA-256)");

    // Parse SSH public key blob
    const unsigned char *p = blob;
    const unsigned char *end = blob + blob_len;

    // Skip key type string ("ssh-rsa")
    if (p + 4 > end) goto fail;
    uint32_t slen = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    p += 4 + slen;

    // Read e (public exponent)
    if (p + 4 > end) goto fail;
    uint32_t e_len = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    p += 4;
    if (p + e_len > end) goto fail;
    const unsigned char *e_data = p;
    p += e_len;

    // Read n (modulus)
    if (p + 4 > end) goto fail;
    uint32_t n_len = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    p += 4;
    if (p + n_len > end) goto fail;
    const unsigned char *n_data = p;

    {
        mbedtls_rsa_context rsa;
        mbedtls_rsa_init(&rsa);

        int ret = mbedtls_rsa_import_raw(&rsa,
                                          n_data, n_len,   // N
                                          NULL, 0,          // P
                                          NULL, 0,          // Q
                                          NULL, 0,          // D
                                          e_data, e_len);   // E
        if (ret != 0) {
            mbedtls_rsa_free(&rsa);
            goto fail;
        }

        ret = mbedtls_rsa_complete(&rsa);
        if (ret != 0) {
            mbedtls_rsa_free(&rsa);
            goto fail;
        }

        // rsa-sha2-256: PKCS#1 v1.5 with SHA-256
        ret = mbedtls_rsa_pkcs1_verify(&rsa, MBEDTLS_MD_SHA256,
                                        32, hash, sig);
        mbedtls_rsa_free(&rsa);

        lua_pushboolean(L, ret == 0);
        return 1;
    }

fail:
    lua_pushboolean(L, 0);
    return 1;
}

// ecdsaP256Verify(pubkey_blob, sig_blob, hash) → bool
// pubkey_blob is SSH wire format: string("ecdsa-sha2-nistp256") + string("nistp256") + string(Q)
// sig_blob is SSH wire format: mpint(r) + mpint(s)
static int l_crypto_ecdsa_p256_verify(lua_State *L) {
    size_t blob_len, sig_len, hash_len;
    const unsigned char *blob = (const unsigned char *)luaL_checklstring(L, 1, &blob_len);
    const unsigned char *sig_data = (const unsigned char *)luaL_checklstring(L, 2, &sig_len);
    const unsigned char *hash = (const unsigned char *)luaL_checklstring(L, 3, &hash_len);

    if (hash_len != 32)
        return luaL_error(L, "ecdsaP256Verify: hash must be 32 bytes (SHA-256)");

    // Parse SSH public key blob to extract the EC point Q
    const unsigned char *p = blob;
    const unsigned char *end = blob + blob_len;

    // Skip key type string
    if (p + 4 > end) goto fail;
    uint32_t slen = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    p += 4 + slen;

    // Skip curve identifier string
    if (p + 4 > end) goto fail;
    slen = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    p += 4 + slen;

    // Read Q (uncompressed point)
    if (p + 4 > end) goto fail;
    uint32_t q_len = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    p += 4;
    if (p + q_len > end) goto fail;
    const unsigned char *q_data = p;

    {
        // Load the EC point
        mbedtls_ecp_group grp;
        mbedtls_ecp_point Q;
        mbedtls_ecp_group_init(&grp);
        mbedtls_ecp_point_init(&Q);

        int ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
        if (ret != 0) goto cleanup_ec;

        ret = mbedtls_ecp_point_read_binary(&grp, &Q, q_data, q_len);
        if (ret != 0) goto cleanup_ec;

        // Parse SSH signature: mpint(r) + mpint(s)
        const unsigned char *sp = sig_data;
        const unsigned char *send = sig_data + sig_len;

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
        if (ret == 0) ret = mbedtls_ecdsa_verify(&grp, hash, hash_len, &Q, &r, &s);

        mbedtls_mpi_free(&r);
        mbedtls_mpi_free(&s);

cleanup_ec:
        mbedtls_ecp_point_free(&Q);
        mbedtls_ecp_group_free(&grp);

        lua_pushboolean(L, ret == 0);
        return 1;
    }

fail:
    lua_pushboolean(L, 0);
    return 1;
}

// ── Registration ────────────────────────────────────────────────────────────

static const luaL_Reg l_crypto_lib[] = {
    {"randomBytes",     l_crypto_random_bytes},
    {"sha256",          l_crypto_sha256},
    {"sha1",            l_crypto_sha1},
    {"hmacSHA256",      l_crypto_hmac_sha256},
    {"hmacSHA1",        l_crypto_hmac_sha1},
    {"deriveKey",       l_crypto_derive_key},
    {"aes_ctr_new",     l_crypto_aes_ctr_new},
    {"ecdh_x25519_new", l_crypto_ecdh_x25519_new},
    {"ecdh_p256_new",   l_crypto_ecdh_p256_new},
    {"rsaVerify",       l_crypto_rsa_verify},
    {"ecdsaP256Verify", l_crypto_ecdsa_p256_verify},
    {NULL, NULL}
};

void lua_bridge_crypto_init(lua_State *L) {
    // AES-CTR metatable
    luaL_newmetatable(L, AES_CTR_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, l_aes_ctr_methods, 0);
    lua_pushcfunction(L, l_aes_ctr_free);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    // ECDH metatable
    luaL_newmetatable(L, ECDH_MT);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, l_ecdh_methods, 0);
    lua_pushcfunction(L, l_ecdh_free);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);

    // Register crypto subtable
    register_subtable(L, "crypto", l_crypto_lib);
}
