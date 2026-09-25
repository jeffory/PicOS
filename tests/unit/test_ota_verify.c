// Host unit tests for src/os/ota_verify.c: the OTA image authentication that
// decides whether /system/update.bin may be flashed (review: Network Critical
// "sys.applyUpdate flashes unsigned firmware").  Fixtures in fixtures/ota/
// were made with tools/sign_update.py: image.sig by the TEST key the build
// embeds, image.other.sig by a second, untrusted key.
#include "check.h"
#include "fakes/sdcard_fake.h"
#include "ota_verify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIX PICOS_ROOT "/tests/unit/fixtures/ota/"
#define KEYS PICOS_ROOT "/tests/keys/"

#define BIN  "/system/update.bin"
#define HASH "/system/update.sha256"
#define SIG  "/system/update.sig"

static char *slurp(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) { printf("missing fixture %s\n", path); exit(2); }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)n + 1);
  if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) exit(2);
  buf[n] = '\0';
  fclose(f);
  *len = (size_t)n;
  return buf;
}

static char *s_img, *s_sig, *s_other_sig, *s_hash;
static size_t s_img_len, s_sig_len, s_other_len, s_hash_len;

// Stage image + .sha256 + .sig on the fake SD card.
static void stage(const char *img, size_t img_len, const char *hash,
                  size_t hash_len, const char *sig, size_t sig_len) {
  sdfake_reset();
  if (img) sdfake_put(BIN, img, img_len);
  if (hash) sdfake_put(HASH, hash, hash_len);
  if (sig) sdfake_put(SIG, sig, sig_len);
}

static bool verify(const char **err) {
  *err = NULL;
  return ota_verify_file(BIN, HASH, SIG, err);
}

static void test_good_signature(void) {
  const char *err;
  stage(s_img, s_img_len, s_hash, s_hash_len, s_sig, s_sig_len);
  CHECK(verify(&err));
  CHECK(err == NULL);
}

static void test_tampered_image(void) {
  const char *err;
  char *bad = malloc(s_img_len);
  memcpy(bad, s_img, s_img_len);
  bad[1000] ^= 0x01;
  // The attacker also rewrites the .sha256 to match: only the sig stops it.
  uint8_t d[32];
  sdfake_reset();
  sdfake_put(BIN, bad, s_img_len);
  CHECK(ota_hash_file(BIN, d));
  char hex[66];
  for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", d[i]);
  hex[64] = '\n';
  stage(bad, s_img_len, hex, 65, s_sig, s_sig_len);
  CHECK(!verify(&err));
  CHECK(err && strstr(err, "signature"));
  // ...and with the original .sha256 the cheap pre-check catches it first.
  stage(bad, s_img_len, s_hash, s_hash_len, s_sig, s_sig_len);
  CHECK(!verify(&err));
  CHECK(err && strstr(err, "Checksum mismatch"));
  free(bad);
}

static void test_truncated_or_padded_sig(void) {
  const char *err;
  for (size_t cut = 0; cut < s_sig_len; cut += 7) {
    stage(s_img, s_img_len, s_hash, s_hash_len, s_sig, cut);
    CHECK(!verify(&err));
  }
  stage(s_img, s_img_len, s_hash, s_hash_len, s_sig, s_sig_len - 1);
  CHECK(!verify(&err));
  // Trailing garbage after a valid DER signature.
  char padded[OTA_SIG_MAX_LEN];
  memcpy(padded, s_sig, s_sig_len);
  padded[s_sig_len] = 0;
  stage(s_img, s_img_len, s_hash, s_hash_len, padded, s_sig_len + 1);
  CHECK(!verify(&err));
  // Oversized file.
  char big[OTA_SIG_MAX_LEN + 8];
  memset(big, 0x30, sizeof(big));
  stage(s_img, s_img_len, s_hash, s_hash_len, big, sizeof(big));
  CHECK(!verify(&err));
  // A bit flipped inside r.
  char flip[OTA_SIG_MAX_LEN];
  memcpy(flip, s_sig, s_sig_len);
  flip[10] ^= 0x40;
  stage(s_img, s_img_len, s_hash, s_hash_len, flip, s_sig_len);
  CHECK(!verify(&err));
}

static void test_wrong_key(void) {
  const char *err;
  stage(s_img, s_img_len, s_hash, s_hash_len, s_other_sig, s_other_len);
  CHECK(!verify(&err));
  CHECK(err && strstr(err, "signature"));
  // The same signature does verify under the key that made it.
  size_t pl;
  char *other_pub = slurp(KEYS "picos-update-TEST-other-public.pem", &pl);
  uint8_t d[32];
  CHECK(ota_hash_file(BIN, d));
  CHECK(ota_sig_verify_pem(other_pub, d, (const uint8_t *)s_other_sig,
                           s_other_len));
  CHECK(!ota_sig_verify_pem(other_pub, d, (const uint8_t *)s_sig, s_sig_len));
  // Garbage or empty key text never verifies.
  CHECK(!ota_sig_verify_pem("not a key", d, (const uint8_t *)s_sig, s_sig_len));
  CHECK(!ota_sig_verify_pem("", d, (const uint8_t *)s_sig, s_sig_len));
  free(other_pub);
}

static void test_missing_files(void) {
  const char *err;
  stage(s_img, s_img_len, s_hash, s_hash_len, NULL, 0);
  CHECK(!verify(&err));
  CHECK(err && strstr(err, "signature file"));
  stage(s_img, s_img_len, NULL, 0, s_sig, s_sig_len);
  CHECK(!verify(&err));
  CHECK(err && strstr(err, "checksum file"));
  stage(NULL, 0, s_hash, s_hash_len, s_sig, s_sig_len);
  CHECK(!verify(&err));
  // An empty .sig is missing, not a signature.
  stage(s_img, s_img_len, s_hash, s_hash_len, "", 0);
  CHECK(!verify(&err));
}

static void test_strict_hex(void) {
  uint8_t out[32];
  const char *good = "00112233445566778899aabbccddeeff"
                     "00112233445566778899AABBCCDDEEFF";
  CHECK(ota_parse_sha256_hex(good, 64, out));
  CHECK_EQ_U32(out[0], 0x00);
  CHECK_EQ_U32(out[5], 0x55);
  CHECK_EQ_U32(out[31], 0xFF);
  char buf[80];
  snprintf(buf, sizeof(buf), "%s\n", good);
  CHECK(ota_parse_sha256_hex(buf, 65, out));
  snprintf(buf, sizeof(buf), "%s\r\n", good);
  CHECK(ota_parse_sha256_hex(buf, 66, out));
  // Rejected: short, long, trailing junk, whitespace, sign, prefix, name.
  CHECK(!ota_parse_sha256_hex(good, 63, out));
  snprintf(buf, sizeof(buf), "%s0", good);
  CHECK(!ota_parse_sha256_hex(buf, 65, out));
  snprintf(buf, sizeof(buf), "%s  picocalc_os.bin\n", good);
  CHECK(!ota_parse_sha256_hex(buf, strlen(buf), out));
  snprintf(buf, sizeof(buf), " %.63s", good);
  CHECK(!ota_parse_sha256_hex(buf, 64, out));
  snprintf(buf, sizeof(buf), "+%.63s", good);
  CHECK(!ota_parse_sha256_hex(buf, 64, out));
  snprintf(buf, sizeof(buf), "0x%.62s", good);
  CHECK(!ota_parse_sha256_hex(buf, 64, out));
  snprintf(buf, sizeof(buf), "%.62s g", good);  // sscanf("%02x") accepted " g"-style pairs
  CHECK(!ota_parse_sha256_hex(buf, 64, out));
  snprintf(buf, sizeof(buf), "%.63sg", good);
  CHECK(!ota_parse_sha256_hex(buf, 64, out));
  snprintf(buf, sizeof(buf), "%s\n\n", good);
  CHECK(!ota_parse_sha256_hex(buf, 66, out));
  // A NUL inside the counted length is not a digit.
  memcpy(buf, good, 64);
  buf[20] = '\0';
  CHECK(!ota_parse_sha256_hex(buf, 64, out));
}

int main(void) {
  s_img = slurp(FIX "image.bin", &s_img_len);
  s_sig = slurp(FIX "image.sig", &s_sig_len);
  s_other_sig = slurp(FIX "image.other.sig", &s_other_len);
  s_hash = slurp(FIX "image.sha256", &s_hash_len);
  test_good_signature();
  test_tampered_image();
  test_truncated_or_padded_sig();
  test_wrong_key();
  test_missing_files();
  test_strict_hex();
  free(s_img); free(s_sig); free(s_other_sig); free(s_hash);
  return check_report("test_ota_verify");
}
