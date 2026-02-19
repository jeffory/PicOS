cmake_minimum_required(VERSION 3.13)

# ── Target board ──────────────────────────────────────────────────────────────
# Change to "pico2" for standard Pico 2, "pico_w" for Pico W (original),
# or "pico" for original Pico (note: very little RAM for Lua + framebuffer)
set(PICO_BOARD pimoroni_pico_plus2_w_rp2350 CACHE STRING "Target board")

# Pull in Pico SDK (must be done before project())
include(pico_sdk_import.cmake)

project(picocalc_os C CXX ASM)
set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# ── Lua 5.4 ──────────────────────────────────────────────────────────────────
# Lua is added as a git submodule or downloaded into third_party/lua-5.4/
# See README.md for setup instructions.

set(LUA_DIR ${CMAKE_SOURCE_DIR}/third_party/lua-5.4/src)

set(LUA_SOURCES
    ${LUA_DIR}/lapi.c
    ${LUA_DIR}/lcode.c
    ${LUA_DIR}/lctype.c
    ${LUA_DIR}/ldebug.c
    ${LUA_DIR}/ldo.c
    ${LUA_DIR}/ldump.c
    ${LUA_DIR}/lfunc.c
    ${LUA_DIR}/lgc.c
    ${LUA_DIR}/linit.c
    ${LUA_DIR}/llex.c
    ${LUA_DIR}/lmem.c
    ${LUA_DIR}/lobject.c
    ${LUA_DIR}/lopcodes.c
    ${LUA_DIR}/lparser.c
    ${LUA_DIR}/lstate.c
    ${LUA_DIR}/lstring.c
    ${LUA_DIR}/ltable.c
    ${LUA_DIR}/ltm.c
    ${LUA_DIR}/lundump.c
    ${LUA_DIR}/lvm.c
    ${LUA_DIR}/lzio.c
    # Standard libraries (selective — skip io/os/package for sandboxing)
    ${LUA_DIR}/lauxlib.c
    ${LUA_DIR}/lbaselib.c
    ${LUA_DIR}/lmathlib.c
    ${LUA_DIR}/lstrlib.c
    ${LUA_DIR}/ltablib.c
    ${LUA_DIR}/lutf8lib.c
    # Include corolib if you want coroutine support in apps
    ${LUA_DIR}/lcorolib.c
)

add_library(lua STATIC ${LUA_SOURCES})
target_include_directories(lua PUBLIC ${LUA_DIR})

# Lua config overrides for embedded target
target_compile_definitions(lua PRIVATE
    LUA_32BITS=1           # Use 32-bit integers (saves memory on RP2350)
    LUA_USE_LONGJMP=1      # Use setjmp/longjmp instead of C++ exceptions
    LUAI_MAXSTACK=500      # Reduce stack depth from 1000000 to 500
    LUA_IDSIZE=60          # Smaller error source IDs
)
# No signals, no OS-level stuff
target_compile_options(lua PRIVATE -Os)

# ── FatFS ─────────────────────────────────────────────────────────────────────
# Option A: Use pico-extras (recommended):
#   Add to your ~/.gitconfig or clone pico-extras and add to PICO_SDK_PATH
#   See: https://github.com/raspberrypi/pico-extras
#
# Option B: Vendor FatFS directly (what this CMake assumes by default):
#   Download FatFS from http://elm-chan.org/fsw/ff/
#   Place source in third_party/fatfs/
#   Provide your own diskio.c for SPI-based SD (see README for a template)

set(FATFS_DIR ${CMAKE_SOURCE_DIR}/third_party/fatfs)

add_library(fatfs STATIC
    ${FATFS_DIR}/ff.c
    ${FATFS_DIR}/ffsystem.c
    ${FATFS_DIR}/ffunicode.c
    ${FATFS_DIR}/port/diskio_spi.c    # our SPI port layer
)
# diskio_spi.c uses hardware.h and Pico SDK SPI — expose both include paths
target_include_directories(fatfs PUBLIC
    ${FATFS_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(fatfs pico_stdlib hardware_spi hardware_gpio)

# ── Main firmware target ───────────────────────────────────────────────────────

add_executable(picocalc_os
    src/main.c
    src/os/launcher.c
    src/os/lua_bridge.c
    src/os/system_menu.c
    src/drivers/display.c
    src/drivers/keyboard.c
    src/drivers/sdcard.c
)

target_include_directories(picocalc_os PRIVATE
    src/
    src/os/
    src/drivers/
)

target_link_libraries(picocalc_os
    pico_stdlib
    pico_multicore
    hardware_spi
    hardware_i2c
    hardware_gpio
    hardware_pwm
    hardware_dma
    hardware_clocks
    lua
    fatfs
)

# Enable USB serial for stdio (printf goes to USB-C serial — handy for debug)
pico_enable_stdio_usb(picocalc_os 1)
pico_enable_stdio_uart(picocalc_os 0)

# Generate UF2, map, and disassembly
pico_add_extra_outputs(picocalc_os)

# ── Compiler flags ────────────────────────────────────────────────────────────

target_compile_options(picocalc_os PRIVATE
    -Wall -Wextra
    -Wno-unused-parameter
    -Os                    # Optimise for size (Lua + framebuffer is large)
)

# Put the framebuffer in PSRAM section (only effective on Pimoroni boards
# with a linker script that defines .psram → PSRAM address space)
# On standard Pico 2 the attribute is silently ignored and FB goes in SRAM.
target_compile_definitions(picocalc_os PRIVATE
    PICO_RP2350=1
)

# ── Print firmware size after build ───────────────────────────────────────────

add_custom_command(TARGET picocalc_os POST_BUILD
    COMMAND arm-none-eabi-size $<TARGET_FILE:picocalc_os>
    COMMENT "Firmware size:"
)
