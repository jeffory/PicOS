# Host (simulator / unit-test) build of the mbedTLS subset that
# src/os/ota_verify.c needs, so the OTA signature check that the firmware runs
# is the same code the simulator and the unit tests run.
#
# Source: PICODECK_MBEDTLS_DIR if set (cache var or env), else the Pico SDK's
# copy ($PICO_SDK_PATH/lib/mbedtls, the version the firmware links), else the
# pinned 3.6.2 release tarball fetched at configure time.
#
#   include(${PICODECK_ROOT}/cmake/picodeck_host_mbedtls.cmake)
#   target_link_libraries(<target> PRIVATE picodeck_host_mbedtls)
include_guard(GLOBAL)

set(PICODECK_MBEDTLS_VERSION 3.6.2)
set(PICODECK_MBEDTLS_URL
    "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${PICODECK_MBEDTLS_VERSION}/mbedtls-${PICODECK_MBEDTLS_VERSION}.tar.bz2")
set(PICODECK_MBEDTLS_SHA256
    8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca)

if(NOT PICODECK_MBEDTLS_DIR AND DEFINED ENV{PICODECK_MBEDTLS_DIR})
    set(PICODECK_MBEDTLS_DIR "$ENV{PICODECK_MBEDTLS_DIR}")
endif()
if(NOT PICODECK_MBEDTLS_DIR AND DEFINED ENV{PICO_SDK_PATH}
   AND EXISTS "$ENV{PICO_SDK_PATH}/lib/mbedtls/library/ecdsa.c")
    set(PICODECK_MBEDTLS_DIR "$ENV{PICO_SDK_PATH}/lib/mbedtls")
endif()
if(NOT PICODECK_MBEDTLS_DIR)
    include(FetchContent)
    # SOURCE_SUBDIR names a directory with no CMakeLists.txt, so
    # MakeAvailable only downloads (mbedTLS's own build is not used).
    FetchContent_Declare(picodeck_mbedtls
        URL ${PICODECK_MBEDTLS_URL}
        URL_HASH SHA256=${PICODECK_MBEDTLS_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR picodeck-no-cmake)
    FetchContent_MakeAvailable(picodeck_mbedtls)
    set(PICODECK_MBEDTLS_DIR "${picodeck_mbedtls_SOURCE_DIR}")
endif()
if(NOT EXISTS "${PICODECK_MBEDTLS_DIR}/library/ecdsa.c")
    message(FATAL_ERROR "mbedTLS sources not found at '${PICODECK_MBEDTLS_DIR}'")
endif()
message(STATUS "Host mbedTLS: ${PICODECK_MBEDTLS_DIR}")

get_filename_component(_picodeck_cmake_dir "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
file(GLOB _picodeck_mbedtls_srcs "${PICODECK_MBEDTLS_DIR}/library/*.c")
add_library(picodeck_host_mbedtls STATIC ${_picodeck_mbedtls_srcs})
target_include_directories(picodeck_host_mbedtls SYSTEM PUBLIC
    "${PICODECK_MBEDTLS_DIR}/include")
target_compile_definitions(picodeck_host_mbedtls PUBLIC
    MBEDTLS_CONFIG_FILE="${_picodeck_cmake_dir}/mbedtls_host_config.h")
# Third-party code: no project warnings / -Werror.
target_compile_options(picodeck_host_mbedtls PRIVATE -w)
set_target_properties(picodeck_host_mbedtls PROPERTIES POSITION_INDEPENDENT_CODE ON)
