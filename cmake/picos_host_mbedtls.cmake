# Host (simulator / unit-test) build of the mbedTLS subset that
# src/os/ota_verify.c needs, so the OTA signature check that the firmware runs
# is the same code the simulator and the unit tests run.
#
# Source: PICOS_MBEDTLS_DIR if set (cache var or env), else the Pico SDK's
# copy ($PICO_SDK_PATH/lib/mbedtls, the version the firmware links), else the
# pinned 3.6.2 release tarball fetched at configure time.
#
#   include(${PICOS_ROOT}/cmake/picos_host_mbedtls.cmake)
#   target_link_libraries(<target> PRIVATE picos_host_mbedtls)
include_guard(GLOBAL)

set(PICOS_MBEDTLS_VERSION 3.6.2)
set(PICOS_MBEDTLS_URL
    "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-${PICOS_MBEDTLS_VERSION}/mbedtls-${PICOS_MBEDTLS_VERSION}.tar.bz2")
set(PICOS_MBEDTLS_SHA256
    8b54fb9bcf4d5a7078028e0520acddefb7900b3e66fec7f7175bb5b7d85ccdca)

if(NOT PICOS_MBEDTLS_DIR AND DEFINED ENV{PICOS_MBEDTLS_DIR})
    set(PICOS_MBEDTLS_DIR "$ENV{PICOS_MBEDTLS_DIR}")
endif()
if(NOT PICOS_MBEDTLS_DIR AND DEFINED ENV{PICO_SDK_PATH}
   AND EXISTS "$ENV{PICO_SDK_PATH}/lib/mbedtls/library/ecdsa.c")
    set(PICOS_MBEDTLS_DIR "$ENV{PICO_SDK_PATH}/lib/mbedtls")
endif()
if(NOT PICOS_MBEDTLS_DIR)
    include(FetchContent)
    # SOURCE_SUBDIR names a directory with no CMakeLists.txt, so
    # MakeAvailable only downloads (mbedTLS's own build is not used).
    FetchContent_Declare(picos_mbedtls
        URL ${PICOS_MBEDTLS_URL}
        URL_HASH SHA256=${PICOS_MBEDTLS_SHA256}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR picos-no-cmake)
    FetchContent_MakeAvailable(picos_mbedtls)
    set(PICOS_MBEDTLS_DIR "${picos_mbedtls_SOURCE_DIR}")
endif()
if(NOT EXISTS "${PICOS_MBEDTLS_DIR}/library/ecdsa.c")
    message(FATAL_ERROR "mbedTLS sources not found at '${PICOS_MBEDTLS_DIR}'")
endif()
message(STATUS "Host mbedTLS: ${PICOS_MBEDTLS_DIR}")

get_filename_component(_picos_cmake_dir "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
file(GLOB _picos_mbedtls_srcs "${PICOS_MBEDTLS_DIR}/library/*.c")
add_library(picos_host_mbedtls STATIC ${_picos_mbedtls_srcs})
target_include_directories(picos_host_mbedtls SYSTEM PUBLIC
    "${PICOS_MBEDTLS_DIR}/include")
target_compile_definitions(picos_host_mbedtls PUBLIC
    MBEDTLS_CONFIG_FILE="${_picos_cmake_dir}/mbedtls_host_config.h")
# Third-party code: no project warnings / -Werror.
target_compile_options(picos_host_mbedtls PRIVATE -w)
set_target_properties(picos_host_mbedtls PROPERTIES POSITION_INDEPENDENT_CODE ON)
