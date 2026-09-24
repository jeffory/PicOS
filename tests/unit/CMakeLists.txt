# Host unit tests for PicOS pure-logic modules (no Pico SDK, no SDL).
#
#   make test-unit          # configure build_unit/, build, ctest
#   make fuzz               # libFuzzer targets (clang), see tests/fuzz/
#
# Each test is one executable (tests/unit/test_<module>.c + the module's
# source).  Assertions come from check.h; a test exits non-zero on failure.
# Built with ASan+UBSan when the compiler supports them
# (-DPICOS_UNIT_SANITIZE=OFF to disable).
cmake_minimum_required(VERSION 3.16)
project(picos_unit C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

get_filename_component(PICOS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

option(PICOS_UNIT_SANITIZE "Build unit tests with ASan+UBSan" ON)
option(PICOS_FUZZ "Also build the libFuzzer targets (tests/fuzz)" OFF)

include(CheckCSourceCompiles)

add_compile_options(-Wall -Wextra -Werror -g -O1)

set(PICOS_UNIT_SAN_FLAGS "")
if(PICOS_UNIT_SANITIZE)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
    check_c_source_compiles("int main(void){return 0;}" PICOS_HAVE_ASAN_UBSAN)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(PICOS_HAVE_ASAN_UBSAN)
        set(PICOS_UNIT_SAN_FLAGS -fsanitize=address,undefined
            -fno-sanitize-recover=all -fno-omit-frame-pointer)
    else()
        message(WARNING "ASan/UBSan not available with ${CMAKE_C_COMPILER}; "
                        "unit tests run without sanitizers")
    endif()
endif()

enable_testing()

# picos_unit_test(<name> SOURCES <files...> [INCLUDES <dirs...>] [DEFINES ...])
function(picos_unit_test name)
    cmake_parse_arguments(T "" "" "SOURCES;INCLUDES;DEFINES" ${ARGN})
    add_executable(${name} ${CMAKE_CURRENT_SOURCE_DIR}/${name}.c ${T_SOURCES})
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR} ${PICOS_ROOT}/src/os ${T_INCLUDES})
    target_compile_definitions(${name} PRIVATE
        PICOS_ROOT="${PICOS_ROOT}" ${T_DEFINES})
    target_compile_options(${name} PRIVATE ${PICOS_UNIT_SAN_FLAGS})
    target_link_options(${name} PRIVATE ${PICOS_UNIT_SAN_FLAGS})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:abort_on_error=0;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
endfunction()

set(SRC ${PICOS_ROOT}/src)

picos_unit_test(test_font SOURCES ${SRC}/fonts/font.c INCLUDES ${SRC}/fonts)
picos_unit_test(test_elf_plan SOURCES ${SRC}/os/elf_plan.c)
picos_unit_test(test_fs_path SOURCES ${SRC}/os/fs_path.c)

if(PICOS_FUZZ)
    add_subdirectory(${PICOS_ROOT}/tests/fuzz ${CMAKE_CURRENT_BINARY_DIR}/fuzz)
endif()
