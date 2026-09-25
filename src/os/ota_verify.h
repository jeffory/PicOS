#pragma once

// OTA image authentication: the checks that decide whether a staged firmware
// image may be flashed.  Shared by the firmware (ota_update.c), the simulator
// (its ota_prepare_update stub) and the host unit tests, so all three run the
// same code.  Needs only mbedTLS (pk/ecdsa/sha256) and the sdcard_* file API.
//
// Scheme: ECDSA P-256 over SHA-256 of the raw image.  The signature file holds
// the DER ECDSA-Sig-Value (what `openssl dgst -sha256 -sign` and
// tools/sign_update.py write).  The public key is embedded at build time from
// the CMake option PICOS_UPDATE_PUBKEY_PEM (see cmake/picos_update_key.cmake).
// The .sha256 file is a cheap pre-check (it catches a truncated download
// before any public-key maths); only the signature authenticates the image.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OTA_SIG_MAX_LEN 128  // a P-256 DER signature is at most 72 bytes
#define OTA_MAX_SIZE (2u * 1024u * 1024u)  // flash is 4 MB; leave headroom
#define OTA_MIN_SIZE 256u                  // at least the vector table

// The PEM public key the firmware trusts (generated: ota_pubkey.c).
extern const char g_ota_update_pubkey_pem[];

// Strict SHA-256 hex parse: exactly 64 hex digits, optionally followed by a
// single "\n" or "\r\n" and nothing else (no spaces, signs, "0x", or a
// filename as `sha256sum` prints).  Upper- or lower-case digits.
bool ota_parse_sha256_hex(const char *text, size_t len, uint8_t out[32]);

// Verify a DER ECDSA signature over a SHA-256 digest with a PEM P-256 public
// key.  False for a malformed key or signature, a non-P-256 key, or a
// signature that does not verify.
bool ota_sig_verify_pem(const char *pem, const uint8_t digest[32],
                        const uint8_t *sig, size_t sig_len);

// Check an image digest against hash_path (strict hex) and sig_path (the
// signature, verified with g_ota_update_pubkey_pem).  On failure sets *err
// to a static message and returns false.
bool ota_check_digest(const uint8_t digest[32], const char *hash_path,
                      const char *sig_path, const char **err);

// SHA-256 of a file on the SD card (streamed; feeds the watchdog on
// firmware).  False if the file cannot be read completely.
bool ota_hash_file(const char *path, uint8_t out[32]);

// ota_hash_file(bin_path) + ota_check_digest.
bool ota_verify_file(const char *bin_path, const char *hash_path,
                     const char *sig_path, const char **err);

// RP2350 vector table sanity check on the first bytes of an image: initial
// SP in SRAM, reset vector in flash, not a UF2 file.
bool ota_image_header_ok(const uint8_t *data, size_t len);

// Everything sys.applyUpdate checks before it asks the user: size limits,
// vector table, checksum and signature.  The firmware's ota_prepare_update
// and the simulator's both call this.
bool ota_prepare_check(const char *bin_path, const char *hash_path,
                       const char *sig_path, const char **err);
