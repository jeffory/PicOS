// Host unit test for src/drivers/ca_bundle.c: the bundle parses as a whole
// (the firmware parses it ONCE at boot into one mbedtls_x509_crt chain that
// every TLS connection shares, wifi.c) and the chains PicOS depends on verify
// against it, with host-name checking.  Fixtures in fixtures/tls/ are the
// chains served by the real hosts on 2026-09-24 (`openssl s_client
// -showcerts`); the host build has no MBEDTLS_HAVE_TIME_DATE, so leaf expiry
// does not rot this test — only trust paths and names are checked.
#include "check.h"
#include "ca_bundle.h"

#include "mbedtls/x509_crt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIX PICOS_ROOT "/tests/unit/fixtures/tls/"

static mbedtls_x509_crt s_ca;

static uint32_t verify_chain(const char *file, const char *host) {
  FILE *f = fopen(file, "rb");
  if (!f) { printf("missing fixture %s\n", file); exit(2); }
  static char pem[16384];
  size_t n = fread(pem, 1, sizeof(pem) - 1, f);
  fclose(f);
  pem[n] = '\0';
  mbedtls_x509_crt chain;
  mbedtls_x509_crt_init(&chain);
  int rc = mbedtls_x509_crt_parse(&chain, (const unsigned char *)pem, n + 1);
  CHECK_EQ_INT(rc, 0);
  uint32_t flags = 0;
  rc = mbedtls_x509_crt_verify(&chain, &s_ca, NULL, host, &flags, NULL, NULL);
  mbedtls_x509_crt_free(&chain);
  return rc == 0 ? 0 : (flags ? flags : 0xFFFFFFFFu);
}

int main(void) {
  mbedtls_x509_crt_init(&s_ca);
  // Exactly how wifi.c parses it: the whole bundle, NUL included.
  int rc = mbedtls_x509_crt_parse(&s_ca, (const unsigned char *)g_ca_bundle_pem,
                                  g_ca_bundle_pem_len + 1);
  CHECK_EQ_INT(rc, 0);  // >0 would mean some roots failed to parse
  int count = 0;
  for (mbedtls_x509_crt *c = &s_ca; c && c->raw.len; c = c->next) count++;
  CHECK_EQ_INT(count, 11);

  // WE1 -> GTS Root R4 (in the bundle directly).
  CHECK_EQ_U32(verify_chain(FIX "picos.jeffory.dev.chain.pem",
                            "picos.jeffory.dev"), 0);
  // DV E36 -> Sectigo E46 (P-384).
  CHECK_EQ_U32(verify_chain(FIX "github.com.chain.pem", "github.com"), 0);
  // YR1 -> ISRG Root YR, which is NOT in the bundle: only its cross-sign by
  // ISRG Root X1 makes this verify.
  CHECK_EQ_U32(verify_chain(FIX "release-assets.githubusercontent.com.chain.pem",
                            "release-assets.githubusercontent.com"), 0);

  // Wrong host name for a trusted chain.
  CHECK((verify_chain(FIX "github.com.chain.pem", "evil.example") &
         MBEDTLS_X509_BADCERT_CN_MISMATCH) != 0);
  // A chain whose root is not in the bundle.
  CHECK((verify_chain(FIX "selfsigned.example.chain.pem",
                      "selfsigned.example") &
         MBEDTLS_X509_BADCERT_NOT_TRUSTED) != 0);

  mbedtls_x509_crt_free(&s_ca);
  return check_report("test_ca_bundle");
}
