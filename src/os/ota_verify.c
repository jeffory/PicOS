// ota_verify.c — OTA image authentication (see ota_verify.h).
//
// Host-portable on purpose: the firmware, the simulator and tests/unit all
// compile this file, so the check that guards the flash is the one the tests
// exercise.  Allocations go through mbedTLS's calloc (umm_malloc on firmware).

#include "ota_verify.h"

#include "../drivers/sdcard.h"

#include "mbedtls/ecp.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"

#include <stdio.h>
#include <string.h>

#if !defined(PICOS_SIMULATOR) && !defined(PICOS_HOST_TEST)
#include "hardware/watchdog.h"
#define OTA_FEED_WATCHDOG() watchdog_update()
#else
#define OTA_FEED_WATCHDOG() ((void)0)
#endif

// ── Strict hex ───────────────────────────────────────────────────────────────

static int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool ota_parse_sha256_hex(const char *text, size_t len, uint8_t out[32]) {
  if (!text) return false;
  // 64 digits, then nothing, "\n" or "\r\n".
  if (len == 65) {
    if (text[64] != '\n') return false;
  } else if (len == 66) {
    if (text[64] != '\r' || text[65] != '\n') return false;
  } else if (len != 64) {
    return false;
  }
  uint8_t tmp[32];
  for (int i = 0; i < 32; i++) {
    int hi = hex_nibble(text[2 * i]);
    int lo = hex_nibble(text[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    tmp[i] = (uint8_t)((hi << 4) | lo);
  }
  memcpy(out, tmp, 32);
  return true;
}

// Read a whole small file (at most cap bytes).  Returns its length, or -1 if
// it is missing, unreadable or larger than cap.
static int read_small_file(const char *path, uint8_t *buf, size_t cap) {
  int size = sdcard_fsize(path);
  if (size < 0 || (size_t)size > cap) return -1;
  sdfile_t f = sdcard_fopen(path, "r");
  if (!f) return -1;
  int got = 0;
  while (got < size) {
    int n = sdcard_fread(f, buf + got, size - got);
    if (n <= 0) break;
    got += n;
  }
  sdcard_fclose(f);
  return got == size ? size : -1;
}

// ── Signature ────────────────────────────────────────────────────────────────

bool ota_sig_verify_pem(const char *pem, const uint8_t digest[32],
                        const uint8_t *sig, size_t sig_len) {
  if (!pem || !digest || !sig || sig_len == 0 || sig_len > OTA_SIG_MAX_LEN)
    return false;

  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  bool ok = false;
  // PEM input must include the terminating NUL in its length.
  if (mbedtls_pk_parse_public_key(&pk, (const unsigned char *)pem,
                                  strlen(pem) + 1) != 0)
    goto out;
  if (mbedtls_pk_get_type(&pk) != MBEDTLS_PK_ECKEY ||
      mbedtls_pk_ec(pk)->MBEDTLS_PRIVATE(grp).id != MBEDTLS_ECP_DP_SECP256R1)
    goto out;
  // Parses the DER ECDSA-Sig-Value and rejects trailing bytes.
  ok = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, digest, 32, sig,
                         sig_len) == 0;
out:
  mbedtls_pk_free(&pk);
  return ok;
}

bool ota_check_digest(const uint8_t digest[32], const char *hash_path,
                      const char *sig_path, const char **err) {
  // 1. Cheap pre-check: the .sha256 (catches a truncated/corrupt download).
  uint8_t buf[OTA_SIG_MAX_LEN];
  int n = read_small_file(hash_path, buf, 66);
  if (n < 0) {
    *err = sdcard_fsize(hash_path) < 0 ? "Missing checksum file"
                                        : "Malformed checksum file";
    return false;
  }
  uint8_t expected[32];
  if (!ota_parse_sha256_hex((const char *)buf, (size_t)n, expected)) {
    *err = "Malformed checksum file";
    return false;
  }
  if (memcmp(expected, digest, 32) != 0) {
    *err = "Checksum mismatch";
    return false;
  }

  // 2. The real control: the signature over the digest.
  n = read_small_file(sig_path, buf, sizeof(buf));
  if (n <= 0) {
    *err = (n == 0 || sdcard_fsize(sig_path) < 0)
               ? "Missing signature file"
               : "Invalid signature file";
    return false;
  }
  if (!ota_sig_verify_pem(g_ota_update_pubkey_pem, digest, buf, (size_t)n)) {
    *err = "Bad signature: image not signed by the PicOS update key";
    return false;
  }
  return true;
}

bool ota_hash_file(const char *path, uint8_t out[32]) {
  int size = sdcard_fsize(path);
  if (size < 0) return false;
  sdfile_t f = sdcard_fopen(path, "r");
  if (!f) return false;

  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  uint8_t buf[512];
  int total = 0, n;
  while ((n = sdcard_fread(f, buf, sizeof(buf))) > 0) {
    mbedtls_sha256_update(&ctx, buf, (size_t)n);
    total += n;
    OTA_FEED_WATCHDOG();
  }
  sdcard_fclose(f);
  mbedtls_sha256_finish(&ctx, out);
  mbedtls_sha256_free(&ctx);
  return total == size;
}

bool ota_verify_file(const char *bin_path, const char *hash_path,
                     const char *sig_path, const char **err) {
  uint8_t digest[32];
  if (!ota_hash_file(bin_path, digest)) {
    *err = "Cannot read firmware file";
    return false;
  }
  return ota_check_digest(digest, hash_path, sig_path, err);
}

// ── Image header ─────────────────────────────────────────────────────────────

#define OTA_SRAM_BASE  0x20000000u
#define OTA_SRAM_END   0x20082000u
#define OTA_FLASH_BASE 0x10000000u
#define OTA_FLASH_END  0x10400000u

bool ota_image_header_ok(const uint8_t *data, size_t len) {
  if (len < 8) return false;
  if (memcmp(data, "UF2\n", 4) == 0) {
    printf("[OTA] Error: File is UF2 format, not .bin!\n");
    return false;
  }
  uint32_t sp = (uint32_t)data[0] | (uint32_t)data[1] << 8 |
                (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
  uint32_t rv = (uint32_t)data[4] | (uint32_t)data[5] << 8 |
                (uint32_t)data[6] << 16 | (uint32_t)data[7] << 24;
  if (sp < OTA_SRAM_BASE || sp > OTA_SRAM_END) {
    printf("[OTA] Invalid SP: 0x%08lx (expected 0x20000000-0x20082000)\n",
           (unsigned long)sp);
    return false;
  }
  rv &= ~1u;
  if (rv < OTA_FLASH_BASE || rv >= OTA_FLASH_END) {
    printf("[OTA] Invalid reset vector: 0x%08lx (expected "
           "0x10000000-0x10400000)\n", (unsigned long)rv);
    return false;
  }
  return true;
}

bool ota_prepare_check(const char *bin_path, const char *hash_path,
                       const char *sig_path, const char **err) {
  int size = sdcard_fsize(bin_path);
  printf("[OTA] Checking update %s, size=%d\n", bin_path, size);
  if (size < 0) {
    *err = "Firmware file not found";
    return false;
  }
  if ((uint32_t)size < OTA_MIN_SIZE) {
    *err = "Firmware file too small";
    return false;
  }
  if ((uint32_t)size > OTA_MAX_SIZE) {
    *err = "Firmware file too large (max 2MB)";
    return false;
  }
  uint8_t header[OTA_MIN_SIZE];
  sdfile_t f = sdcard_fopen(bin_path, "r");
  if (!f) {
    *err = "Cannot open firmware file";
    return false;
  }
  int n = sdcard_fread(f, header, sizeof(header));
  sdcard_fclose(f);
  if (n < (int)sizeof(header)) {
    *err = "Cannot read firmware header";
    return false;
  }
  if (!ota_image_header_ok(header, (size_t)n)) {
    *err = "Invalid firmware (bad vector table)";
    return false;
  }
  if (!ota_verify_file(bin_path, hash_path, sig_path, err)) {
    printf("[OTA] Refusing %s: %s\n", bin_path, *err);
    return false;
  }
  return true;
}
