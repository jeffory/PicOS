// mbedTLS configuration for HOST builds (simulator and tests/unit): only what
// src/os/ota_verify.c needs — PEM public-key parsing, ECDSA P-256 verify and
// SHA-256.  The firmware uses src/mbedtls_config.h instead.
#ifndef PICOS_MBEDTLS_HOST_CONFIG_H
#define PICOS_MBEDTLS_HOST_CONFIG_H

#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C

#endif
