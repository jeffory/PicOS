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

// Fill buf with len random bytes from this core's DRBG.  False if the TRNG
// failed its health tests or the DRBG could not be seeded/allocated; buf is
// then zeroed and must not be used.
bool rng_bytes(void *buf, size_t len);

// mbedTLS f_rng adapter (p_rng unused).  Returns 0 or an mbedTLS error.
int rng_mbedtls_random(void *p_rng, unsigned char *out, size_t len);

// Raw TRNG output with health tests on (for diagnostics / entropy source).
// False if the TRNG did not deliver within its timeout or kept failing.
bool rng_trng_read(uint8_t *out, size_t len);
