#ifndef LUA_NUMFMT_H
#define LUA_NUMFMT_H

// Float formatting for Lua that does not depend on the C library.
//
// The firmware links the Pico SDK's pico_printf (snprintf is --wrap'ed), whose
// %g keeps trailing zeros ("51.00000"), whose %f drops digits past precision 9
// and switches to exponent form at 1e9, and which has no %a. The simulator
// uses glibc. Lua's l_sprintf (patched in by cmake/picodeck_lua.cmake) routes
// every float conversion through here on both targets, so tostring,
// string.format and lua_pushfstring print identically everywhere.
//
// The output matches glibc for every float32 value: the exact decimal value
// is computed with a small bignum and rounded half-to-even. One deliberate
// difference: NaN prints as "nan" whatever its sign bit (x86 makes 0/0
// negative and glibc shows "-nan"; ARM does not).

#include <stddef.h>

// Formats v with one printf conversion spec "%[-+ #0][width][.prec]<conv>",
// conv one of a A e E f F g G. Precision is capped at 150. snprintf
// semantics: writes at most size-1 characters plus a NUL and returns the full
// length; returns -1 for a malformed spec.
int picodeck_numfmt_float(char *buf, size_t size, const char *spec, float v);

// l_sprintf replacement (all Lua uses have exactly one conversion). Float
// conversions (the argument is a double holding a lua_Number) go through
// picodeck_numfmt_float; anything else is passed to vsnprintf.
int picodeck_lua_sprintf(char *buf, size_t size, const char *fmt, ...);

#endif
