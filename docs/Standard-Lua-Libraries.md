# Standard Lua Libraries

The following Lua 5.4 standard libraries are available:

| Library | Description |
|---------|-------------|
| `base` | Core functions (`print`, `type`, `pairs`, `ipairs`, `tonumber`, `tostring`, etc.) |
| `table` | Table manipulation (`table.insert`, `table.remove`, `table.sort`, etc.) |
| `string` | String manipulation (`string.sub`, `string.format`, `string.match`, etc.) |
| `math` | Math functions (`math.sin`, `math.random`, `math.floor`, etc.) |
| `coroutine` | Coroutines (`coroutine.create`, `resume`, `yield`, `wrap`, …) |
| `utf8` | UTF-8 helpers (`utf8.char`, `utf8.codes`, `utf8.len`, …) |

**Not available** (for sandboxing): `io`, `os`, `package`, `debug`. The base
functions `dofile` and `loadfile` are removed. `load` accepts text chunks only
(bytecode is rejected), as do app `main.lua` files and `sys.loadlib`.

## Numbers

PicOS Lua uses **32-bit integers and single-precision floats** (`LUA_32BITS`):

- Integers wrap at ±2^31 (`0xDEADBEEF` is negative; `%x` still prints the
  32-bit pattern). `sys.getTimeMs()` goes negative after ~24.8 days of uptime
  (differences still wrap correctly).
- Floats have ~7 significant digits; integers are exact only up to 2^24;
  `1e39 == math.huge`. JSON numbers above 2^24 lose precision the same way.
- Float formatting (`tostring`, `..`, `string.format`, JSON, REPL) is the same
  on the device and the simulator and matches glibc: `tostring(51.0)` is
  `"51.0"`, NaN prints `nan`. Subnormals (< 1.2e-38) print `0.0` on the device.
- Arguments that are quantities (coordinates, sizes, durations, volumes,
  rates, frame indices) round to nearest, so `199.99998` counts as `200`.
  NaN, inf and values beyond ±2^24 are argument errors. Volumes, colour
  components, effect factors and brightness clamp to their range. Handles,
  enum values, colours, masks, byte counts and ports must be exact integers.

## Limits

- `LUAI_MAXSTACK = 1000` slots (a frame costs 10-13 slots: recursion tops out
  around 80-100 Lua levels).
- `LUAI_MAXCCALLS = 60`: Lua→C→Lua nesting (e.g. `string.gsub` callbacks
  calling `gsub`) stops at ~58 levels with a catchable `"C stack overflow"`.
- The Lua VM runs on its own 64 KB stack in PSRAM.
