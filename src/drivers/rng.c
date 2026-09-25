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

#include <limits.h>
#include <stdio.h>
#include <string.h>

// rng_clk cycles between ring-oscillator samples.  Sampling faster than the
// ring decorrelates makes the autocorrelation/CRNGT tests fail: measured on a
// PicoCalc (RP2350, 200 MHz, 20 blocks per setting, Sep 2026), 32 cycles passed
// 1/20 on the shortest chain and 0/20 on the others, 128 passed 20/20 on chains
// 0-1 but only 14/20 on chains 2-3, and 256 and 1024 passed 20/20 on all four.
// 32 made every Core 0 boot seed fail (and a8015f6, which panicked on that
// failure, boot-looped).  So: 256 (~1 ms per 192-bit block), and retries after
// a health-test failure use 1024 (~4.5 ms).  The TRNG is only read to seed the
// DRBGs at boot (reseeding is disabled), so the cost is a few ms, once.
#define TRNG_SAMPLE_CNT        256u
#define TRNG_SAMPLE_CNT_RETRY 1024u
#define TRNG_TIMEOUT_US   20000u  // per 192-bit block (>4x the slow setting)
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

// Leave the TRNG as the SDK found it: source off, software-reset (which also
// clears a latched autocorrelation halt and restores the reset-default
// configuration), status cleared.  pico_rand's capture_additional_trng_samples
// waits on TRNG_BUSY with no timeout and IRQs masked, so it must never find a
// halted block.  Called with the TRNG lock held, after EVERY use.
static void trng_park(void) {
  trng_hw->rnd_source_enable = 0;
  trng_hw->trng_sw_reset = 1;
  busy_wait_us_32(2);
  trng_hw->rng_icr = 0xFFFFFFFFu;
}

// One 192-bit EHR block (6 words) with VNC, CRNGT and autocorrelation on.
// Called with the TRNG lock held; always parks the block before returning.
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
    trng_hw->sample_cnt1 = attempt == 0 ? TRNG_SAMPLE_CNT : TRNG_SAMPLE_CNT_RETRY;
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
    trng_park();
    return true;
  }
  trng_park();  // failure exit: a halted TRNG would hang pico_rand
  return false;
}

bool rng_trng_read(uint8_t *out, size_t len) {
  // The SDK's runtime init takes the TRNG out of reset (pico_rand uses it
  // from boot).  If it is somehow still held, release it with a bounded
  // wait rather than unreset_block_wait's unbounded one.
  if (resets_hw->reset & RESETS_RESET_TRNG_BITS) {
    unreset_block(RESETS_RESET_TRNG_BITS);
    uint32_t t0 = time_us_32();
    while (!(resets_hw->reset_done & RESETS_RESET_TRNG_BITS))
      if (time_us_32() - t0 > 1000u) return false;
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

// Pre-generated DRBG output.  Core 1 draws its randomness from deep inside
// mbedTLS (ECDHE key generation and blinding in mg_mgr_poll), where even a
// plain CTR_DRBG request (~300 bytes of stack) eats into the 4 KB stack;
// wifi_poll refills the pool at the top of the loop (rng_refill) so those
// draws are a memcpy.  One-shot: bytes are zeroed as they are handed out.
#define RNG_POOL_BYTES 512

typedef struct {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  bool failed;                     // a DRBG request failed: never used again
  uint16_t pool_avail;             // unread bytes at the END of pool[]
  uint8_t pool[RNG_POOL_BYTES];
} rng_state_t;

static rng_state_t *s_state[2];  // indexed by core; umm-allocated

bool rng_ready(void) {
  rng_state_t *st = s_state[get_core_num()];
  return st != NULL && !st->failed;
}

bool rng_init_this_core(void) {
  unsigned core = get_core_num();
  if (s_state[core]) return true;

  uint32_t t0 = time_us_32();
  rng_state_t *st = (rng_state_t *)umm_calloc(1, sizeof(rng_state_t));
  if (!st) {
    printf("[RNG] core %u: out of memory\n", core);
    return false;
  }
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
    printf("[RNG] core %u: TRNG/DRBG seed FAILED (-0x%04x) — no CSPRNG, "
           "TLS disabled on this core\n", core, (unsigned)-rc);
    mbedtls_ctr_drbg_free(&st->drbg);
    mbedtls_entropy_free(&st->entropy);
    umm_free(st);
    return false;
  }
  // Never reseed from inside a (deep) request: see rng.h.
  mbedtls_ctr_drbg_set_reseed_interval(&st->drbg, INT32_MAX);
  s_state[core] = st;
  rng_refill();
  printf("[RNG] core %u: CTR_DRBG seeded from the TRNG in %lu us\n", core,
         (unsigned long)(time_us_32() - t0));
  return true;
}

static int drbg_fill(rng_state_t *st, unsigned char *out, size_t len) {
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

int rng_mbedtls_random(void *p_rng, unsigned char *out, size_t len) {
  (void)p_rng;
  rng_state_t *st = s_state[get_core_num()];
  if (!st || st->failed) return MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
  if (len <= st->pool_avail) {
    uint8_t *src = st->pool + (RNG_POOL_BYTES - st->pool_avail);
    memcpy(out, src, len);
    mbedtls_platform_zeroize(src, len);
    st->pool_avail = (uint16_t)(st->pool_avail - len);
    return 0;
  }
  int rc = drbg_fill(st, out, len);
  if (rc != 0) {
    // Fail closed: rng_ready() turns false, so wifi.c refuses new TLS and
    // picocalc.crypto.randomBytes raises.
    st->failed = true;
    st->pool_avail = 0;
    mbedtls_platform_zeroize(st->pool, sizeof(st->pool));
    printf("[RNG] core %u: DRBG request failed (-0x%04x) — RNG disabled\n",
           get_core_num(), (unsigned)-rc);
  }
  return rc;
}

void rng_refill(void) {
  rng_state_t *st = s_state[get_core_num()];
  if (!st || st->failed || st->pool_avail == RNG_POOL_BYTES) return;
  if (drbg_fill(st, st->pool, RNG_POOL_BYTES) == 0) {
    st->pool_avail = RNG_POOL_BYTES;
  } else {
    st->failed = true;
    st->pool_avail = 0;
    mbedtls_platform_zeroize(st->pool, sizeof(st->pool));
  }
}

bool rng_bytes(void *buf, size_t len) {
  if (rng_mbedtls_random(NULL, (unsigned char *)buf, len) == 0) return true;
  mbedtls_platform_zeroize(buf, len);
  return false;
}

// ── Mongoose (MG_ENABLE_CUSTOM_RANDOM) ──────────────────────────────────────
// Fails hard: without a working DRBG the buffer is zeroed and false is
// returned — never a get_rand_* fallback (pico_rand can spin on the TRNG with
// IRQs masked, and its output is not a CSPRNG).  TLS never reaches here in
// that state, because wifi.c refuses to start TLS unless rng_ready().  The
// non-TLS users (mg_tcpip_init's ephemeral port, DHCP/DNS ids) tolerate
// zeros.  No panic: that would turn a TRNG fault into a boot loop, since
// mg_tcpip_init calls this during wifi_init.
bool mg_random(void *buf, size_t len) {
  return rng_bytes(buf, len);
}
