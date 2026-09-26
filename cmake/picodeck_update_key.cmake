# Embeds the OTA update PUBLIC key (a P-256 PEM) as a C string, the key
# src/os/ota_verify.c checks /system/update.sig against.
#
#   picodeck_generate_update_pubkey(<out.c> <key.pem>)
#
# Firmware: CMake option PICODECK_UPDATE_PUBKEY_PEM (default: the TEST key in
# tests/keys, whose PRIVATE half is in the repo — dev builds only).  Release
# builds pass the real public key and PICODECK_REQUIRE_RELEASE_UPDATE_KEY=ON,
# which refuses the test key (see .github/workflows/release.yml).
include_guard(GLOBAL)

get_filename_component(PICODECK_TEST_UPDATE_PUBKEY
    "${CMAKE_CURRENT_LIST_DIR}/../tests/keys/picodeck-update-TEST-public.pem" ABSOLUTE)

function(picodeck_generate_update_pubkey out_c pem)
    get_filename_component(pem "${pem}" ABSOLUTE)
    if(NOT EXISTS "${pem}")
        message(FATAL_ERROR "OTA update public key not found: ${pem}")
    endif()
    file(READ "${pem}" _pem)
    string(STRIP "${_pem}" _pem)
    if(NOT _pem MATCHES "^-----BEGIN PUBLIC KEY-----\n[A-Za-z0-9+/=\n]+\n-----END PUBLIC KEY-----$")
        message(FATAL_ERROR "${pem} is not a PEM SubjectPublicKeyInfo "
                            "(openssl pkey -pubout) file")
    endif()
    string(REPLACE "\n" "\\n\"\n    \"" PICODECK_UPDATE_PUBKEY_C "${_pem}")
    set(PICODECK_UPDATE_PUBKEY_SOURCE "${pem}")
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ota_pubkey.c.in"
                   "${out_c}" @ONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${pem}")
endfunction()
