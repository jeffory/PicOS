# libFuzzer targets, built from tests/unit/CMakeLists.txt with -DPICOS_FUZZ=ON
# (see `make fuzz`).  Clang only: skipped with a warning when the compiler
# cannot link -fsanitize=fuzzer.
set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=fuzzer")
check_c_source_compiles(
    "#include <stdint.h>
     #include <stddef.h>
     int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n) { (void)d; (void)n; return 0; }"
    PICOS_HAVE_LIBFUZZER)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)

if(NOT PICOS_HAVE_LIBFUZZER)
    message(WARNING "PICOS_FUZZ=ON but ${CMAKE_C_COMPILER} has no libFuzzer "
                    "(-fsanitize=fuzzer); fuzz targets not built. Use clang.")
    return()
endif()

set(PICOS_FUZZ_FLAGS -fsanitize=fuzzer,address,undefined
    -fno-sanitize-recover=all -fno-omit-frame-pointer)

# picos_fuzz_target(<name> SOURCES <files...> [INCLUDES <dirs...>])
function(picos_fuzz_target name)
    cmake_parse_arguments(F "" "" "SOURCES;INCLUDES" ${ARGN})
    add_executable(${name} ${CMAKE_CURRENT_SOURCE_DIR}/${name}.c ${F_SOURCES})
    target_include_directories(${name} PRIVATE ${PICOS_ROOT}/src/os ${F_INCLUDES})
    target_compile_options(${name} PRIVATE ${PICOS_FUZZ_FLAGS})
    target_link_options(${name} PRIVATE ${PICOS_FUZZ_FLAGS})
endfunction()

picos_fuzz_target(fuzz_elf_plan SOURCES ${PICOS_ROOT}/src/os/elf_plan.c)
picos_fuzz_target(fuzz_app_manifest SOURCES ${PICOS_ROOT}/src/os/app_manifest.c)
