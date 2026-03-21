#include "lua_bridge_internal.h"

// ── AES-CTR cipher userdata ─────────────────────────────────────────────────
#define AES_CTR_MT "picocalc.crypto.aes_ctr"

typedef struct {
    pccrypto_aes_t ctx; // opaque AES-CTR context handle (NULL once freed)
} aes_ctr_ud_t;

static aes_ctr_ud_t *check_aes_ctr(lua_State *L, int idx) {
    aes_ctr_ud_t *ud = (aes_ctr_ud_t *)luaL_checkudata(L, idx, AES_CTR_MT);
    if (!ud->ctx) luaL_error(L, "aes_ctr: cipher has been freed");
    return ud;
}

static int l_aes_ctr_update(lua_State *L) {
    aes_ctr_ud_t *ud = check_aes_ctr(L, 1);
    size_t len;
    const char *input = luaL_checklstring(L, 2, &len);

    uint8_t *output = umm_malloc(len);
    if (!output) return luaL_error(L, "aes_ctr: out of memory");

    int ret = g_api.crypto->aesUpdate(ud->ctx,
                                      (const uint8_t *)input, output,
                                      (uint32_t)len);
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
    if (ud->ctx) {
        g_api.crypto->aesFree(ud->ctx);
        ud->ctx = NULL;
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
    pccrypto_ecdh_t ctx; // opaque ECDH context handle (NULL once freed)
} ecdh_ud_t;

static ecdh_ud_t *check_ecdh(lua_State *L, int idx) {
    ecdh_ud_t *ud = (ecdh_ud_t *)luaL_checkudata(L, idx, ECDH_MT);
    if (!ud->ctx) luaL_error(L, "ecdh: context has been freed");
    return ud;
}

static int l_ecdh_get_public_key(lua_State *L) {
    ecdh_ud_t *ud = check_ecdh(L, 1);

    uint8_t buf[128];
    uint32_t olen = 0;
    g_api.crypto->ecdhGetPublicKey(ud->ctx, buf, &olen);
    if (olen == 0)
        return luaL_error(L, "ecdh: failed to get public key");

    lua_pushlstring(L, (const char *)buf, (size_t)olen);
    return 1;
}

static int l_ecdh_compute_shared(lua_State *L) {
    ecdh_ud_t *ud = check_ecdh(L, 1);
    size_t peer_len;
    const char *peer_pub = luaL_checklstring(L, 2, &peer_len);

    uint8_t secret[66]; // up to P-256 (65 bytes) or X25519 (32 bytes)
    uint32_t olen = 0;
    int ret = g_api.crypto->ecdhComputeShared(ud->ctx,
                                               (const uint8_t *)peer_pub,
                                               (uint32_t)peer_len,
                                               secret, &olen);
    if (ret != 0)
        return luaL_error(L, "ecdh: compute_shared failed (%d)", ret);

    lua_pushlstring(L, (const char *)secret, (size_t)olen);
    return 1;
}

static int l_ecdh_free(lua_State *L) {
    ecdh_ud_t *ud = (ecdh_ud_t *)luaL_checkudata(L, 1, ECDH_MT);
    if (ud->ctx) {
        g_api.crypto->ecdhFree(ud->ctx);
        ud->ctx = NULL;
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

    uint8_t *buf = umm_malloc((size_t)n);
    if (!buf) return luaL_error(L, "randomBytes: out of memory");

    g_api.crypto->randomBytes(buf, (uint32_t)n);

    lua_pushlstring(L, (const char *)buf, (size_t)n);
    umm_free(buf);
    return 1;
}

static int l_crypto_sha256(lua_State *L) {
    size_t len;
    const char *data = luaL_checklstring(L, 1, &len);
    uint8_t hash[32];

    g_api.crypto->sha256((const uint8_t *)data, (uint32_t)len, hash);

    lua_pushlstring(L, (const char *)hash, 32);
    return 1;
}

static int l_crypto_sha1(lua_State *L) {
    size_t len;
    const char *data = luaL_checklstring(L, 1, &len);
    uint8_t hash[20];

    g_api.crypto->sha1((const uint8_t *)data, (uint32_t)len, hash);

    lua_pushlstring(L, (const char *)hash, 20);
    return 1;
}

static int l_crypto_hmac_sha256(lua_State *L) {
    size_t key_len, data_len;
    const char *key = luaL_checklstring(L, 1, &key_len);
    const char *data = luaL_checklstring(L, 2, &data_len);
    uint8_t hash[32];

    g_api.crypto->hmacSha256((const uint8_t *)key, (uint32_t)key_len,
                              (const uint8_t *)data, (uint32_t)data_len,
                              hash);

    lua_pushlstring(L, (const char *)hash, 32);
    return 1;
}

static int l_crypto_hmac_sha1(lua_State *L) {
    size_t key_len, data_len;
    const char *key = luaL_checklstring(L, 1, &key_len);
    const char *data = luaL_checklstring(L, 2, &data_len);
    uint8_t hash[20];

    g_api.crypto->hmacSha1((const uint8_t *)key, (uint32_t)key_len,
                            (const uint8_t *)data, (uint32_t)data_len,
                            hash);

    lua_pushlstring(L, (const char *)hash, 20);
    return 1;
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

    uint8_t result[256];
    g_api.crypto->deriveKey(letter,
                             (const uint8_t *)K, (uint32_t)k_len,
                             (const uint8_t *)H, (uint32_t)h_len,
                             (const uint8_t *)sid, (uint32_t)sid_len,
                             result, (uint32_t)needed);

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
    ud->ctx = g_api.crypto->aesNew((const uint8_t *)key, (uint32_t)key_len,
                                    (const uint8_t *)iv);
    if (!ud->ctx)
        return luaL_error(L, "aes_ctr_new: failed to create AES context");

    luaL_getmetatable(L, AES_CTR_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static int l_crypto_ecdh_x25519_new(lua_State *L) {
    ecdh_ud_t *ud = (ecdh_ud_t *)lua_newuserdata(L, sizeof(ecdh_ud_t));
    ud->ctx = g_api.crypto->ecdhX25519();
    if (!ud->ctx)
        return luaL_error(L, "ecdh_x25519_new: failed to create ECDH context");

    luaL_getmetatable(L, ECDH_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static int l_crypto_ecdh_p256_new(lua_State *L) {
    ecdh_ud_t *ud = (ecdh_ud_t *)lua_newuserdata(L, sizeof(ecdh_ud_t));
    ud->ctx = g_api.crypto->ecdhP256();
    if (!ud->ctx)
        return luaL_error(L, "ecdh_p256_new: failed to create ECDH context");

    luaL_getmetatable(L, ECDH_MT);
    lua_setmetatable(L, -2);
    return 1;
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

    bool ok = g_api.crypto->rsaVerify(blob, (uint32_t)blob_len,
                                       sig, (uint32_t)sig_len,
                                       hash, (uint32_t)hash_len);
    lua_pushboolean(L, ok);
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

    bool ok = g_api.crypto->ecdsaP256Verify(blob, (uint32_t)blob_len,
                                              sig_data, (uint32_t)sig_len,
                                              hash, (uint32_t)hash_len);
    lua_pushboolean(L, ok);
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
