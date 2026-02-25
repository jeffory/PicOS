cmake_minimum_required(VERSION 3.13)

# ── Target board ──────────────────────────────────────────────────────────────
# Change to "pico2" for standard Pico 2, "pico_w" for Pico W (original),
# or "pico" for original Pico (note: very little RAM for Lua + framebuffer)
set(PICO_BOARD pimoroni_pico_plus2_w_rp2350 CACHE STRING "Target board")

# ── Custom mbedtls config (must be set before pico_sdk_init) ──────────────────
# Routes mbedtls allocations to PSRAM via umm_malloc instead of SRAM
set(PICO_MBEDTLS_CONFIG_FILE "${CMAKE_SOURCE_DIR}/src/mbedtls_config.h" CACHE STRING "Custom mbedtls config" FORCE)

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

# ── Image Decoders ────────────────────────────────────────────────────────────
set(TGX_DIR ${CMAKE_SOURCE_DIR}/third_party/tgx)
set(JPEGDEC_DIR ${CMAKE_SOURCE_DIR}/third_party/JPEGDEC)
set(PNGDEC_DIR ${CMAKE_SOURCE_DIR}/third_party/PNGdec)
set(GIF_DIR ${CMAKE_SOURCE_DIR}/third_party/AnimatedGIF)

add_library(image_decoders STATIC
    ${TGX_DIR}/src/Color.cpp
    ${TGX_DIR}/src/Fonts.cpp
    ${TGX_DIR}/src/Renderer3D.cpp
    ${JPEGDEC_DIR}/src/JPEGDEC.cpp
    ${PNGDEC_DIR}/src/PNGdec.cpp
    ${PNGDEC_DIR}/src/adler32.c
    ${PNGDEC_DIR}/src/crc32.c
    ${PNGDEC_DIR}/src/infback.c
    ${PNGDEC_DIR}/src/inffast.c
    ${PNGDEC_DIR}/src/inflate.c
    ${PNGDEC_DIR}/src/inftrees.c
    ${PNGDEC_DIR}/src/zutil.c
    ${GIF_DIR}/src/AnimatedGIF.cpp
)

target_include_directories(image_decoders PUBLIC
    ${TGX_DIR}/src
    ${JPEGDEC_DIR}/src
    ${PNGDEC_DIR}/src
    ${GIF_DIR}/src
)

target_compile_definitions(image_decoders PUBLIC __LINUX__=1)


# ── Main firmware target ───────────────────────────────────────────────────────

add_executable(picocalc_os
    src/main.c
    src/os/launcher.c
    src/os/lua_runner.c
    src/os/native_loader.c
    src/os/lua_bridge.c
    src/os/lua_bridge_display.c
    src/os/lua_bridge_input.c
    src/os/lua_bridge_sys.c
    src/os/lua_bridge_fs.c
    src/os/lua_bridge_network.c
    src/os/lua_bridge_config.c
    src/os/lua_bridge_perf.c
    src/os/lua_bridge_graphics.c
    src/os/lua_bridge_ui.c
    src/os/lua_bridge_audio.c
    src/os/lua_bridge_sound.c
    src/os/lua_psram_alloc.c
    src/os/system_menu.c
    src/os/screenshot.c
    src/os/text_input.c
    src/os/file_browser.c
    src/os/config.c
    src/os/clock.c
    src/os/tz_picker.c
    src/os/ui.c
    src/usb/usb_descriptors.c
    src/usb/usb_msc.c
    src/os/image_decoders.cpp
    src/drivers/display.c
    src/drivers/audio.c
    src/drivers/sound.c
    src/drivers/fileplayer.c
    src/drivers/mp3_player.c
    src/drivers/keyboard.c
    src/drivers/sdcard.c
    src/drivers/wifi.c
    src/drivers/http.c
    third_party/mongoose/mongoose.c
    third_party/picomp3lib/src/bitstream.c
    third_party/picomp3lib/src/buffers.c
    third_party/picomp3lib/src/dct32.c
    third_party/picomp3lib/src/dequant.c
    third_party/picomp3lib/src/dqchan.c
    third_party/picomp3lib/src/huffman.c
    third_party/picomp3lib/src/hufftabs.c
    third_party/picomp3lib/src/imdct.c
    third_party/picomp3lib/src/mp3dec.c
    third_party/picomp3lib/src/mp3tabs.c
    third_party/picomp3lib/src/polyphase.c
    third_party/picomp3lib/src/scalfact.c
    third_party/picomp3lib/src/stproc.c
    third_party/picomp3lib/src/subband.c
    third_party/picomp3lib/src/trigtabs.c
    third_party/umm_malloc/src/umm_malloc.c
)

target_include_directories(picocalc_os PRIVATE
    src/usb/
    src/
    src/os/
    src/drivers/
    third_party/mongoose/
    third_party/umm_malloc/src/
    third_party/picomp3lib/src/
    third_party/picomp3lib/interface/
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
    hardware_pio
    hardware_xip_cache
    tinyusb_device
    lua
    fatfs
    image_decoders
)

# ── WiFi support (Pico W / Pimoroni Pico Plus 2W boards) ─────────────────────
if(PICO_BOARD STREQUAL "pimoroni_pico_plus2_w_rp2350" OR
   PICO_BOARD STREQUAL "pico_w" OR
   PICO_BOARD STREQUAL "pico2_w")
    target_link_libraries(picocalc_os 
        pico_cyw43_arch_none
        pico_mbedtls
        pico_rand
    )
    target_compile_definitions(picocalc_os PRIVATE 
        WIFI_ENABLED=1
        MG_ENABLE_TCPIP=1
        MG_ENABLE_DRIVER_PICO_W=1
        MG_ENABLE_TCPIP_DRIVER_INIT=0
        CYW43_LWIP=0
    )
    message(STATUS "WiFi enabled for board: ${PICO_BOARD} (Mongoose TCPIP)")
else()
    message(STATUS "WiFi disabled for board: ${PICO_BOARD}")
endif()

# Enable USB serial for stdio (printf goes to USB-C serial — handy for debug)
pico_enable_stdio_usb(picocalc_os 1)
pico_enable_stdio_uart(picocalc_os 1)

pico_generate_pio_header(picocalc_os ${CMAKE_CURRENT_SOURCE_DIR}/src/drivers/lcd_spi.pio)

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
    PICO_STACK_SIZE=4096  # Reduced to 4KB to fit in SCRATCH_X/Y
    MG_ENABLE_MBEDTLS=1
    __ARMEL__=1            # For picomp3lib MP3 decoder
    # Re-enable pico_stdio_usb features that get auto-disabled when
    # tinyusb_device is linked directly. We provide our own descriptors
    # (tusb_config.h + usb_descriptors.c with CDC+MSC) but still want
    # the SDK to handle tusb_init() and the background tud_task() IRQ.
    PICO_STDIO_USB_ENABLE_TINYUSB_INIT=1
    PICO_STDIO_USB_ENABLE_IRQ_BACKGROUND_TASK=1
    PICO_STDIO_USB_USE_DEFAULT_DESCRIPTORS=0
    PICO_STDIO_USB_ENABLE_RESET_VIA_BAUD_RATE=0
    PICO_STDIO_USB_ENABLE_RESET_VIA_VENDOR_INTERFACE=0
)

# ── Print firmware size after build ───────────────────────────────────────────

add_custom_command(TARGET picocalc_os POST_BUILD
    COMMAND arm-none-eabi-size $<TARGET_FILE:picocalc_os>
    COMMENT "Firmware size:"
)
