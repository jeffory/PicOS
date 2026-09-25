# PicOS Lua VM configuration — shared by the firmware (CMakeLists.txt) and the
# simulator (simulator/CMakeLists.txt) so both VMs have identical semantics.
#
# Two jobs:
#
#  1. PICOS_LUA_DEFINITIONS: the one list of Lua compile-time settings. Both
#     builds apply it PUBLIC on their Lua library so the bridge code and the VM
#     agree on lua_Integer / lua_Number / luaL_Buffer layout.
#
#  2. picos_patch_luaconf(<lua src dir>): upstream Lua 5.4.7's luaconf.h
#     hard-codes LUA_32BITS, LUAI_MAXSTACK and LUA_IDSIZE, so -D flags for them
#     are silently overridden (LUA_USER_H cannot help: lua.h includes it after
#     luaconf.h has already chosen the number types). This wraps each of those
#     definitions in #if !defined(...) so the values above take effect.
#     (LUAI_MAXCCALLS needs no patch: llimits.h already guards it.)
#     It also routes l_sprintf (every float Lua prints: tostring,
#     string.format %a/%e/%f/%g, lua_pushfstring %f) to picos_lua_sprintf
#     when PICOS_LUA_SPRINTF is defined, so number formatting does not depend
#     on the C library: the firmware's pico_printf %g keeps trailing zeros
#     ("51.00000") and has no %a, while the simulator uses glibc. The patch
#     also defines PICOS_LUA_SPRINTF_PATCHED, which lua_numfmt.c requires
#     whenever PICOS_LUA_SPRINTF is set (a build that skipped the patch
#     would otherwise silently print floats through the C library).
#     Idempotent: each edit is skipped if already applied (a checkout patched
#     by an older version of this file picks up only the new edits).
#     Fails the build if the upstream text is not found (version drift).
#
#  3. PICOS_LUA_SOURCES: PicOS sources that belong in the Lua library itself
#     (the formatter behind picos_lua_sprintf).
#
# Script mode, for places that extract the tarball outside CMake (Makefile
# download-lua, CI workflows):
#   cmake -DLUA_SRC_DIR=third_party/lua-5.4/src -P cmake/picos_lua.cmake

set(PICOS_LUA_DEFINITIONS
    LUA_32BITS=1           # 32-bit lua_Integer, single-precision lua_Number
    LUA_USE_LONGJMP=1      # setjmp/longjmp error handling (no C++ exceptions)
    LUAI_MAXSTACK=1000     # Lua stack slots per coroutine (upstream: 1000000)
    LUAI_MAXCCALLS=60      # nested C calls + parser levels (upstream: 200);
                           # sized with the 64 KB Lua VM stack (lua_runner.c)
    LUA_IDSIZE=60          # Size of source ids in error messages
    PICOS_LUA_SPRINTF=1    # l_sprintf -> picos_lua_sprintf (src/os/lua_numfmt.c)
)

set(PICOS_LUA_SOURCES "${CMAKE_CURRENT_LIST_DIR}/../src/os/lua_numfmt.c")

set(_PICOS_LUACONF_MARKER "/* PicOS: luaconf.h patched by cmake/picos_lua.cmake */")

# Replace _from with _to in the variable named _var, unless _to is already
# there (applied by an earlier run).
function(_picos_luaconf_replace _var _from _to _what)
    string(FIND "${${_var}}" "${_to}" _done)
    if(NOT _done EQUAL -1)
        return()
    endif()
    string(FIND "${${_var}}" "${_from}" _pos)
    if(_pos EQUAL -1)
        message(FATAL_ERROR
            "picos_patch_luaconf: upstream ${_what} definition not found in "
            "luaconf.h — has the Lua version changed? Update cmake/picos_lua.cmake.")
    endif()
    string(REPLACE "${_from}" "${_to}" _out "${${_var}}")
    set(${_var} "${_out}" PARENT_SCOPE)
endfunction()

function(picos_patch_luaconf _lua_src_dir)
    set(_file "${_lua_src_dir}/luaconf.h")
    if(NOT EXISTS "${_file}")
        message(FATAL_ERROR "picos_patch_luaconf: ${_file} not found (run `make setup`)")
    endif()
    file(READ "${_file}" _text)
    set(_orig "${_text}")

    _picos_luaconf_replace(_text
        "#define LUA_32BITS\t0\n"
        "#if !defined(LUA_32BITS)\n#define LUA_32BITS\t0\n#endif\n"
        "LUA_32BITS")
    _picos_luaconf_replace(_text
        "#if LUAI_IS32INT\n#define LUAI_MAXSTACK\t\t1000000\n#else\n#define LUAI_MAXSTACK\t\t15000\n#endif\n"
        "#if !defined(LUAI_MAXSTACK)\n#if LUAI_IS32INT\n#define LUAI_MAXSTACK\t\t1000000\n#else\n#define LUAI_MAXSTACK\t\t15000\n#endif\n#endif\n"
        "LUAI_MAXSTACK")
    _picos_luaconf_replace(_text
        "#define LUA_IDSIZE\t60\n"
        "#if !defined(LUA_IDSIZE)\n#define LUA_IDSIZE\t60\n#endif\n"
        "LUA_IDSIZE")
    # A tree patched before PICOS_LUA_SPRINTF_PATCHED existed: add it.
    set(_old_sprintf "#define l_sprintf(s,sz,f,i)\tpicos_lua_sprintf(s,sz,f,i)\n#elif")
    string(FIND "${_text}" "${_old_sprintf}" _old_pos)
    if(NOT _old_pos EQUAL -1)
        string(REPLACE "${_old_sprintf}"
            "#define l_sprintf(s,sz,f,i)\tpicos_lua_sprintf(s,sz,f,i)\n#define PICOS_LUA_SPRINTF_PATCHED 1\n#elif"
            _text "${_text}")
    endif()
    _picos_luaconf_replace(_text
        "#if !defined(LUA_USE_C89)\n#define l_sprintf(s,sz,f,i)\tsnprintf(s,sz,f,i)\n"
        "#if defined(PICOS_LUA_SPRINTF)\n/* PicOS: floats bypass the C library's printf (src/os/lua_numfmt.c) */\nint picos_lua_sprintf(char *s, size_t sz, const char *fmt, ...);\n#define l_sprintf(s,sz,f,i)\tpicos_lua_sprintf(s,sz,f,i)\n#define PICOS_LUA_SPRINTF_PATCHED 1\n#elif !defined(LUA_USE_C89)\n#define l_sprintf(s,sz,f,i)\tsnprintf(s,sz,f,i)\n"
        "l_sprintf")

    if(_text STREQUAL _orig)
        return()  # already patched
    endif()
    string(FIND "${_text}" "${_PICOS_LUACONF_MARKER}" _marker_pos)
    if(_marker_pos EQUAL -1)
        set(_text "${_PICOS_LUACONF_MARKER}\n${_text}")
    endif()
    file(WRITE "${_file}" "${_text}")
    message(STATUS "Patched ${_file} to honour PICOS_LUA_DEFINITIONS")
endfunction()

if(CMAKE_SCRIPT_MODE_FILE)
    if(NOT LUA_SRC_DIR)
        message(FATAL_ERROR "usage: cmake -DLUA_SRC_DIR=<lua>/src -P cmake/picos_lua.cmake")
    endif()
    picos_patch_luaconf("${LUA_SRC_DIR}")
endif()
