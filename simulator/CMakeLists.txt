cmake_minimum_required(VERSION 3.13)

# PicOS PC Simulator
# Build with: cmake -DPICOS_SIMULATOR=ON ..

project(picos_simulator C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Parent directory (PicOS root)
set(PICOS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Find required packages
find_package(SDL2 REQUIRED)
find_package(SDL2_image REQUIRED)
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)

# Version from git (same as main project)
execute_process(
    COMMAND git describe --tags --always
    WORKING_DIRECTORY ${PICOS_ROOT}
    OUTPUT_VARIABLE GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(GIT_VERSION)
    string(REGEX REPLACE "^v" "" PICOS_VERSION "${GIT_VERSION}")
else()
    set(PICOS_VERSION "0.0.0-sim")
endif()
message(STATUS "PicOS Simulator version: ${PICOS_VERSION}")

# ── Lua 5.4 ─────────────────────────────────────────────────────────────────
set(LUA_DIR ${PICOS_ROOT}/third_party/lua-5.4/src)

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
    ${LUA_DIR}/lauxlib.c
    ${LUA_DIR}/lbaselib.c
    ${LUA_DIR}/lmathlib.c
    ${LUA_DIR}/lstrlib.c
    ${LUA_DIR}/ltablib.c
    ${LUA_DIR}/lutf8lib.c
    ${LUA_DIR}/lcorolib.c
)

add_library(lua_sim STATIC ${LUA_SOURCES})
target_include_directories(lua_sim PUBLIC ${LUA_DIR})
target_compile_definitions(lua_sim PRIVATE
    LUA_32BITS=1
    LUA_USE_LONGJMP=1
    LUAI_MAXSTACK=500
    PICOS_SIMULATOR=1
)
target_compile_options(lua_sim PRIVATE -O2 -Wall)

# ── libmad (MP3 decoder) ──────────────────────────────────────────────────
set(LIBMAD_DIR ${PICOS_ROOT}/third_party/libmad)

add_library(libmad_sim STATIC
    ${LIBMAD_DIR}/bit.c
    ${LIBMAD_DIR}/fixed.c
    ${LIBMAD_DIR}/frame.c
    ${LIBMAD_DIR}/huffman.c
    ${LIBMAD_DIR}/layer3.c
    ${LIBMAD_DIR}/stream.c
    ${LIBMAD_DIR}/synth.c
    ${LIBMAD_DIR}/timer.c
    ${LIBMAD_DIR}/version.c
)
target_include_directories(libmad_sim PUBLIC ${LIBMAD_DIR})
target_compile_definitions(libmad_sim PRIVATE
    FPM_64BIT
    HAVE_STDINT_H=1
    SIZEOF_INT=4
    SIZEOF_LONG=8
    SIZEOF_LONG_LONG=8
    NDEBUG=1
    OPT_SPEED=1
    OPT_SSO=1
    PROGMEM=             # empty — no flash storage on simulator
)
target_compile_options(libmad_sim PRIVATE -O2 -Wall -Wno-unused-parameter -Wno-unused-result)

# ── Image Decoders (same as main) ───────────────────────────────────────────
set(TGX_DIR ${PICOS_ROOT}/third_party/tgx)
set(JPEGDEC_DIR ${PICOS_ROOT}/third_party/JPEGDEC)
set(PNGDEC_DIR ${PICOS_ROOT}/third_party/PNGdec)
set(GIF_DIR ${PICOS_ROOT}/third_party/AnimatedGIF)

add_library(image_decoders_sim STATIC
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

target_include_directories(image_decoders_sim PUBLIC
    ${TGX_DIR}/src
    ${JPEGDEC_DIR}/src
    ${PNGDEC_DIR}/src
    ${GIF_DIR}/src
)
target_compile_definitions(image_decoders_sim PUBLIC __LINUX__=1 PICOS_SIMULATOR=1)
target_compile_options(image_decoders_sim PRIVATE -O2)

# ── Simulator HAL ───────────────────────────────────────────────────────────
add_library(sim_hal STATIC
    hal/hal_display.c
    hal/hal_input.c
    hal/hal_sdcard.c
    hal/hal_psram.c
    hal/hal_timing.c
    hal/hal_audio.c
    hal/hal_threading.c
    sim_audio.c
    stubs/pico_sdk_stubs.c
    stubs/hardware_stubs.c
    stubs/driver_stubs.c
    stubs/keyboard_stub.c
)

target_include_directories(sim_hal PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/hal
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${PICOS_ROOT}/src
    ${PICOS_ROOT}/src/os
    ${PICOS_ROOT}/src/fonts
    ${PICOS_ROOT}/src/drivers
    ${PICOS_ROOT}/third_party/umm_malloc/src
    ${LIBMAD_DIR}
)

target_link_libraries(sim_hal PUBLIC
    SDL2::SDL2
    Threads::Threads
    libmad_sim
)

target_compile_definitions(sim_hal PUBLIC
    PICOS_SIMULATOR=1
    PICOS_VERSION="${PICOS_VERSION}"
    DISPLAY_SCALE=3
    SIMULATOR_BUILD=1
)

# ── Simulator Network Library (libcurl-based) ──────────────────────────────
add_library(sim_network STATIC
    sim_wifi.c
    sim_http.c
    sim_tcp.c
)
target_include_directories(sim_network PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${PICOS_ROOT}/src
    ${PICOS_ROOT}/src/os
    ${PICOS_ROOT}/src/drivers
    ${PICOS_ROOT}/third_party/umm_malloc/src
)
target_link_libraries(sim_network PUBLIC CURL::libcurl Threads::Threads)
target_compile_definitions(sim_network PUBLIC PICOS_SIMULATOR=1 WIFI_ENABLED=1)

# ── Unicorn Engine (ARM emulation for native apps) ─────────────────────────
include(FetchContent)
FetchContent_Declare(
    unicorn
    GIT_REPOSITORY https://github.com/unicorn-engine/unicorn.git
    GIT_TAG        2.1.1
    GIT_SHALLOW    TRUE
)
# Build Unicorn as static library, disable unneeded architectures
set(UNICORN_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(UNICORN_ARCH "arm" CACHE STRING "" FORCE)
set(UNICORN_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(unicorn)

# ── Main Simulator Executable ───────────────────────────────────────────────
set(PICOS_CORE_SOURCES
    ${PICOS_ROOT}/src/os/launcher.c
    ${PICOS_ROOT}/src/os/lua_runner.c
    ${PICOS_ROOT}/src/os/native_loader.c
    ${PICOS_ROOT}/src/os/lua_bridge.c
    ${PICOS_ROOT}/src/os/lua_bridge_display.c
    ${PICOS_ROOT}/src/os/lua_bridge_input.c
    ${PICOS_ROOT}/src/os/lua_bridge_sys.c
    ${PICOS_ROOT}/src/os/lua_bridge_fs.c
    ${PICOS_ROOT}/src/os/lua_bridge_config.c
    ${PICOS_ROOT}/src/os/appconfig.c
    ${PICOS_ROOT}/src/os/lua_bridge_appconfig.c
    ${PICOS_ROOT}/src/os/lua_bridge_perf.c
    ${PICOS_ROOT}/src/os/perf.c
    ${PICOS_ROOT}/src/os/lua_bridge_graphics.c
    ${PICOS_ROOT}/src/os/lua_bridge_3d.c
    ${PICOS_ROOT}/src/os/lua_bridge_ui.c
    ${PICOS_ROOT}/src/os/lua_bridge_audio.c
    ${PICOS_ROOT}/src/os/lua_bridge_sound.c
    ${PICOS_ROOT}/src/os/lua_bridge_repl.c
    ${PICOS_ROOT}/src/os/lua_bridge_network.c
    ${PICOS_ROOT}/src/os/lua_bridge_tcp.c
    ${PICOS_ROOT}/src/os/lua_bridge_video.c
    ${PICOS_ROOT}/src/os/lua_bridge_game.c
    ${PICOS_ROOT}/src/os/lua_bridge_game_camera.c
    ${PICOS_ROOT}/src/os/lua_bridge_game_scene.c
    ${PICOS_ROOT}/src/os/lua_bridge_game_save.c
    ${PICOS_ROOT}/src/os/lua_psram_alloc.c
    ${PICOS_ROOT}/src/os/system_menu.c
    ${PICOS_ROOT}/src/os/screenshot.c
    ${PICOS_ROOT}/src/os/terminal.c
    ${PICOS_ROOT}/src/os/terminal_parser.c
    ${PICOS_ROOT}/src/os/terminal_render.c
    ${PICOS_ROOT}/src/os/lua_bridge_terminal.c
    ${PICOS_ROOT}/src/os/lua_bridge_crypto.c
    ${PICOS_ROOT}/src/os/text_input.c
    ${PICOS_ROOT}/src/os/file_browser.c
    ${PICOS_ROOT}/src/os/config.c
    ${PICOS_ROOT}/src/os/clock.c
    ${PICOS_ROOT}/src/os/tz_picker.c
    ${PICOS_ROOT}/src/os/ui.c
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs/dev_commands_stub.c
    ${PICOS_ROOT}/src/fonts/font_6x11.c
    ${PICOS_ROOT}/src/fonts/font_scientifica.c
    ${PICOS_ROOT}/src/os/image_decoders.cpp
    ${PICOS_ROOT}/src/os/text_wrap.c
    ${PICOS_ROOT}/src/os/ui_widgets.c
)

# Filter out crypto (not yet implemented in simulator)
# Network and TCP are now provided by sim_network + real lua_bridge files
list(REMOVE_ITEM PICOS_CORE_SOURCES
    ${PICOS_ROOT}/src/os/lua_bridge_crypto.c
)

add_executable(picos_simulator
    main.c
    sim_socket.c
    sim_socket_handler.c
    unicorn_runner.c
    unicorn_trampolines.c
    ${PICOS_CORE_SOURCES}
)

target_include_directories(picos_simulator PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${PICOS_ROOT}/src
    ${PICOS_ROOT}/src/os
    ${PICOS_ROOT}/src/fonts
    ${PICOS_ROOT}/src/drivers
    ${PICOS_ROOT}/src/usb
    ${PICOS_ROOT}/third_party/umm_malloc/src
    ${unicorn_SOURCE_DIR}/include
)

target_link_libraries(picos_simulator PRIVATE
    sim_hal
    sim_network
    lua_sim
    image_decoders_sim
    unicorn
    SDL2::SDL2
    SDL2_image::SDL2_image
    Threads::Threads
    CURL::libcurl
)

target_compile_definitions(picos_simulator PRIVATE
    PICOS_SIMULATOR=1
    PICOS_VERSION="${PICOS_VERSION}"
    DISPLAY_SCALE=3
    SIMULATOR_BUILD=1
    WIFI_ENABLED=1
)

target_compile_options(picos_simulator PRIVATE
    -Wall -Wextra
    -Wno-unused-parameter
    -O2
    -g
)

# Unicorn Engine's crc32c.c and PNGdec's crc32.c both define crc32 —
# they are unrelated implementations; allow the linker to pick the first one.
target_link_options(picos_simulator PRIVATE -Wl,--allow-multiple-definition)

# ── Install / Run ───────────────────────────────────────────────────────────
set_target_properties(picos_simulator PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# Copy SDL2 DLL on Windows (if needed)
if(WIN32)
    add_custom_command(TARGET picos_simulator POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:SDL2::SDL2>
        $<TARGET_FILE_DIR:picos_simulator>
    )
endif()

message(STATUS "Simulator configured:")
message(STATUS "  - Display: 960x960 (3x scale)")
message(STATUS "  - Audio: SDL2 enabled")
message(STATUS "  - Threading: Dual-core simulation")
message(STATUS "  - SD Card: Host filesystem")
message(STATUS "  - WiFi: Mock (always connected)")
message(STATUS "  - HTTP/TCP: libcurl")
