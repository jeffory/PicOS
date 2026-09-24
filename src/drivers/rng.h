#pragma once

// Cryptographic random numbers for mbedTLS, Mongoose and picocalc.crypto.
//
// Entropy comes from the RP2350 hardware TRNG with its von Neumann corrector
// and the CRNGT/autocorrelation health tests switched on, fed into an mbedTLS
// entropy pool (SHA-512 conditioning) that seeds a CTR_DRBG.  Each core gets
// its own DRBG (allocated from the umm heap on first use), so Core 0 (Lua
// crypto) and Core 1 (Mongoose TLS) never share generator state or need a
// lock; TRNG register access is serialised with pico_rand's spinlock.
//
// Replaces the previous sources: pico_mbedtls's mbedtls_hardware_poll and
// Mongoose's default mg_random, both built on get_rand_* (xoroshiro128**).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Seed the calling core's DRBG from the TRNG.  Seeding is deep (~2 KB of
// stack: SHA-512 conditioning + the CTR_DRBG derivation function), so it runs
// only at known-shallow points: Core 0 from main() on the 32 KB OS stack
// before wifi_init, Core 1 at the top of core1_entry.  It is never done
// lazily inside a Mongoose/TLS call chain, and the DRBGs never reseed
// automatically (interval INT32_MAX; well inside CTR_DRBG's 2^48 limit), so
// later requests stay shallow (~300 bytes).  Returns false (logged) if the
// TRNG failed its health tests; this core then has no CSPRNG and TLS is
// refused (wifi.c) instead of running on weak randomness.
bool rng_init_this_core(void);

// True once rng_init_this_core() succeeded on the calling core.
bool rng_ready(void);

// Fill buf with len random bytes from this core's DRBG.  False if this core
// has no seeded DRBG (see rng_init_this_core); buf is then zeroed and must
// not be used.
bool rng_bytes(void *buf, size_t len);

// Top up this core's pool of pre-generated DRBG output (512 bytes) so later
// draws from deep call chains are a memcpy.  Call from a shallow point:
// wifi_poll does, before mg_mgr_poll.  No-op when full or unseeded.
void rng_refill(void);

// mbedTLS f_rng adapter (p_rng unused).  Returns 0 or an mbedTLS error.
int rng_mbedtls_random(void *p_rng, unsigned char *out, size_t len);

// Raw TRNG output with health tests on (for diagnostics / entropy source).
// False if the TRNG did not deliver within its timeout or kept failing.
bool rng_trng_read(uint8_t *out, size_t len);
