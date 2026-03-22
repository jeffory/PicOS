#pragma once

#include <stdint.h>
#include <stdbool.h>

// =============================================================================
// Crypto — mbedTLS wrappers
//
// These are the implementations behind picocalc_crypto_t in os.h.
// The Lua bridge and native app loader both call through g_api.crypto.
// Internally, mbedTLS is used. Context objects are allocated via umm_malloc
// (QMI PSRAM heap).
// =============================================================================

// --- Hashing / MAC -----------------------------------------------------------

void crypto_sha256(const uint8_t *data, uint32_t len, uint8_t out[32]);
void crypto_sha1(const uint8_t *data, uint32_t len, uint8_t out[20]);
void crypto_hmac_sha256(const uint8_t *key, uint32_t klen,
                        const uint8_t *data, uint32_t dlen,
                        uint8_t out[32]);
void crypto_hmac_sha1(const uint8_t *key, uint32_t klen,
                      const uint8_t *data, uint32_t dlen,
                      uint8_t out[20]);
void crypto_random_bytes(uint8_t *buf, uint32_t len);

// SSH session-key derivation (RFC 4253 §7.2). letter = 'A'–'F'.
void crypto_derive_key(char letter,
                       const uint8_t *K, uint32_t k_len,
                       const uint8_t *H, uint32_t h_len,
                       const uint8_t *session_id, uint32_t sid_len,
                       uint8_t *out, uint32_t out_len);

// --- AES-CTR -----------------------------------------------------------------

typedef struct crypto_aes_s crypto_aes_t;  // opaque

// Create AES-CTR context. klen = 16/24/32. nonce = 16 bytes.
// Returns NULL on OOM or bad key size.
crypto_aes_t *crypto_aes_new(const uint8_t *key, uint32_t klen,
                              const uint8_t *nonce);
// Encrypt/decrypt (in-place or in→out). Returns 0 on success.
int  crypto_aes_update(crypto_aes_t *ctx,
                       const uint8_t *in, uint8_t *out, uint32_t len);
void crypto_aes_free(crypto_aes_t *ctx);

// --- ECDH --------------------------------------------------------------------

typedef struct crypto_ecdh_s crypto_ecdh_t;  // opaque

// Create X25519 or P-256 keypair context. Returns NULL on failure.
crypto_ecdh_t *crypto_ecdh_x25519(void);
crypto_ecdh_t *crypto_ecdh_p256(void);
// Write public key bytes into out; sets *out_len. Generates keypair lazily.
void crypto_ecdh_get_public_key(crypto_ecdh_t *ctx,
                                uint8_t *out, uint32_t *out_len);
// Compute shared secret. Returns 0 on success.
int  crypto_ecdh_compute_shared(crypto_ecdh_t *ctx,
                                const uint8_t *remote, uint32_t rlen,
                                uint8_t *out, uint32_t *out_len);
void crypto_ecdh_free(crypto_ecdh_t *ctx);

// --- Signature verification --------------------------------------------------

// SSH wire-format public key blob + raw signature bytes + SHA-256 hash.
bool crypto_rsa_verify(const uint8_t *pubkey, uint32_t pklen,
                       const uint8_t *sig, uint32_t slen,
                       const uint8_t *hash, uint32_t hlen);
// SSH wire-format public key blob + SSH-format sig + SHA-256 hash.
bool crypto_ecdsa_p256_verify(const uint8_t *pubkey, uint32_t pklen,
                               const uint8_t *sig, uint32_t slen,
                               const uint8_t *hash, uint32_t hlen);
