// rng.c — TRNG-seeded CTR_DRBG per core (see rng.h).

#include "rng.h"

#include "hardware/resets.h"
#include "hardware/structs/trng.h"
#include "hardware/sync.h"
#include "pico/platform.h"
#include "pico/time.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/platform_util.h"

#include "umm_malloc.h"

#include <stdio.h>
#include <string.h>

// rng_clk cycles between ring-oscillator samples.  The von Neumann corrector
// discards ~3/4 of the bits, so one 192-bit EHR block costs roughly
// 192 * 4 * TRNG_SAMPLE_CNT cycles (~25 us at 150 MHz).  Must be >= 17 when
// the corrector is bypassed; it is not here, but stay well clear anyway.
#define TRNG_SAMPLE_CNT   32u
#define TRNG_TIMEOUT_US   20000u  // per 192-bit block
#define TRNG_MAX_RETRIES  4       // health-test failures tolerated per block

// The entropy pool demands this many TRNG bytes before it will seed the DRBG
// (twice the 256-bit security level the DRBG targets).
#define RNG_ENTROPY_THRESHOLD 64

#define TRNG_ERR_BITS (TRNG_RNG_ISR_AUTOCORR_ERR_BITS | \
                       TRNG_RNG_ISR_CRNGT_ERR_BITS | TRNG_RNG_ISR_VN_ERR_BITS)

// pico_rand drives the same TRNG (raw mode) under this lock; we reconfigure
// the block fully each time we take it, so the two never see half a setup.
static spin_lock_t *trng_lock(void) {
  return spin_lock_instance(PICO_SPINLOCK_ID_RAND);
}

// One 192-bit EHR block (6 words) with VNC, CRNGT and autocorrelation on.
// Called with the TRNG lock held.
static bool trng_block(uint32_t words[6]) {
  for (int attempt = 0; attempt <= TRNG_MAX_RETRIES; attempt++) {
    trng_hw->rnd_source_enable = 0;
    if (attempt > 0) {
      // An autocorrelation failure stops the TRNG until a software reset.
      trng_hw->trng_sw_reset = 1;
      busy_wait_us_32(2);
    }
    trng_hw->rng_imr = 0xFu;              // no interrupts; we poll the ISR
    trng_hw->rng_icr = 0xFFFFFFFFu;       // clear EHR_VALID and errors
    trng_hw->trng_config = 0;             // shortest ROSC chain
    trng_hw->sample_cnt1 = TRNG_SAMPLE_CNT;
    trng_hw->trng_debug_control = 0;      // VNC + CRNGT + autocorr all on
    trng_hw->rnd_source_enable = 1;

    uint32_t start = time_us_32();
    uint32_t isr;
    while (((isr = trng_hw->rng_isr) &
            (TRNG_RNG_ISR_EHR_VALID_BITS | TRNG_ERR_BITS)) == 0) {
      if (time_us_32() - start > TRNG_TIMEOUT_US) {
        isr = 0;
        break;
      }
    }
    if ((isr & TRNG_ERR_BITS) || !(isr & TRNG_RNG_ISR_EHR_VALID_BITS))
      continue;
    for (int i = 0; i < 6; i++) words[i] = trng_hw->ehr_data[i];
    trng_hw->rnd_source_enable = 0;
    trng_hw->rng_icr = 0xFFFFFFFFu;
    return true;
  }
  trng_hw->rnd_source_enable = 0;
  trng_hw->rng_icr = 0xFFFFFFFFu;
  return false;
}

bool rng_trng_read(uint8_t *out, size_t len) {
  static bool s_unreset;  // one byte of .bss; both cores may set it
  if (!s_unreset) {
    unreset_block_wait(RESETS_RESET_TRNG_BITS);  // no-op if already out
    s_unreset = true;
  }
  while (len > 0) {
    uint32_t words[6];
    uint32_t save = spin_lock_blocking(trng_lock());
    bool ok = trng_block(words);
    spin_unlock(trng_lock(), save);
    if (!ok) {
      mbedtls_platform_zeroize(words, sizeof(words));
      return false;
    }
    size_t n = len < sizeof(words) ? len : sizeof(words);
    memcpy(out, words, n);
    mbedtls_platform_zeroize(words, sizeof(words));
    out += n;
    len -= n;
  }
  return true;
}

// mbedTLS entropy source callback.
static int trng_entropy_source(void *data, unsigned char *out, size_t len,
                               size_t *olen) {
  (void)data;
  *olen = 0;
  if (!rng_trng_read(out, len)) return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
  *olen = len;
  return 0;
}

// ── Per-core DRBG ────────────────────────────────────────────────────────────

typedef struct {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
} rng_state_t;

static rng_state_t *s_state[2];  // indexed by core; umm-allocated

static rng_state_t *rng_state_for_core(void) {
  unsigned core = get_core_num();
  if (s_state[core]) return s_state[core];

  rng_state_t *st = (rng_state_t *)umm_calloc(1, sizeof(rng_state_t));
  if (!st) return NULL;
  mbedtls_entropy_init(&st->entropy);
  mbedtls_ctr_drbg_init(&st->drbg);
  int rc = mbedtls_entropy_add_source(&st->entropy, trng_entropy_source, NULL,
                                      RNG_ENTROPY_THRESHOLD,
                                      MBEDTLS_ENTROPY_SOURCE_STRONG);
  if (rc == 0) {
    static const char pers[] = "picos-rng";
    unsigned char custom[sizeof(pers) + 1];
    memcpy(custom, pers, sizeof(pers));
    custom[sizeof(pers)] = (unsigned char)core;  // distinct per core
    rc = mbedtls_ctr_drbg_seed(&st->drbg, mbedtls_entropy_func, &st->entropy,
                               custom, sizeof(custom));
  }
  if (rc != 0) {
    printf("[RNG] core %u DRBG seed failed: -0x%04x\n", core, (unsigned)-rc);
    mbedtls_ctr_drbg_free(&st->drbg);
    mbedtls_entropy_free(&st->entropy);
    umm_free(st);
    return NULL;
  }
  s_state[core] = st;
  return st;
}

int rng_mbedtls_random(void *p_rng, unsigned char *out, size_t len) {
  (void)p_rng;
  rng_state_t *st = rng_state_for_core();
  if (!st) return MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
  while (len > 0) {
    size_t n = len < MBEDTLS_CTR_DRBG_MAX_REQUEST ? len
                                                  : MBEDTLS_CTR_DRBG_MAX_REQUEST;
    int rc = mbedtls_ctr_drbg_random(&st->drbg, out, n);
    if (rc != 0) return rc;
    out += n;
    len -= n;
  }
  return 0;
}

bool rng_bytes(void *buf, size_t len) {
  if (rng_mbedtls_random(NULL, (unsigned char *)buf, len) == 0) return true;
  mbedtls_platform_zeroize(buf, len);
  return false;
}

// ── Mongoose (MG_ENABLE_CUSTOM_RANDOM) ──────────────────────────────────────
// Mongoose's mbedTLS glue passes mg_random to mbedtls_ssl_conf_rng and
// ignores its return value, so a failure here would hand TLS predictable
// bytes.  A TRNG that fails its health tests is a hardware fault: stop.
#include "pico/platform/panic.h"

bool mg_random(void *buf, size_t len) {
  if (rng_bytes(buf, len)) return true;
  panic("RNG: TRNG/DRBG failure (refusing to run TLS without entropy)");
  return false;
}
