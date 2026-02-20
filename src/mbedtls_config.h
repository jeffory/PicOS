#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

// System support
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_FREE_MACRO        free
#define MBEDTLS_PLATFORM_MALLOC_MACRO      malloc
#define MBEDTLS_PLATFORM_CALLOC_MACRO      calloc
#define MBEDTLS_PLATFORM_EXIT_ALT
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

// ── THE HACK ────────────────────────────────────────────────────────────────
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
// ────────────────────────────────────────────────────────────────────────────

// TLS features
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE

// Key exchange
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED

// Cipher suites & algorithms
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_MD5_C
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_MD_C

// RSA / PKCS1
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_BIGNUM_C

// Required for lwIP altcp compatibility
#define MBEDTLS_SSL_RENEGOTIATION

// Time support (required by pico_lwip_mbedtls for session tracking)
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_PLATFORM_MS_TIME_ALT   // provided in wifi.c via time_us_64()

// Memory tuning
#define MBEDTLS_SSL_IN_CONTENT_LEN  16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN  4096
#define MBEDTLS_SSL_MAX_CONTENT_LEN 16384

#endif /* MBEDTLS_CONFIG_H */
