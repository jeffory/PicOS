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
#     Idempotent: a marker comment records that the file is already patched.
#     Fails the build if the upstream text is not found (version drift).
#
# Script mode, for places that extract the tarball outside CMake (Makefile
# download-lua, CI workflows):
#   cmake -DLUA_SRC_DIR=third_party/lua-5.4/src -P cmake/picos_lua.cmake

set(PICOS_LUA_DEFINITIONS
    LUA_32BITS=1           # 32-bit lua_Integer, single-precision lua_Number
    LUA_USE_LONGJMP=1      # setjmp/longjmp error handling (no C++ exceptions)
    LUAI_MAXSTACK=1000     # Lua stack slots per coroutine (upstream: 1000000)
    LUA_IDSIZE=60          # Size of source ids in error messages
)

set(_PICOS_LUACONF_MARKER "/* PicOS: luaconf.h patched by cmake/picos_lua.cmake */")

# Replace exactly one occurrence of _from with _to in the variable named _var.
function(_picos_luaconf_replace _var _from _to _what)
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

    string(FIND "${_text}" "${_PICOS_LUACONF_MARKER}" _marker_pos)
    if(NOT _marker_pos EQUAL -1)
        return()  # already patched
    endif()

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

    file(WRITE "${_file}" "${_PICOS_LUACONF_MARKER}\n${_text}")
    message(STATUS "Patched ${_file} to honour PICOS_LUA_DEFINITIONS")
endfunction()

if(CMAKE_SCRIPT_MODE_FILE)
    if(NOT LUA_SRC_DIR)
        message(FATAL_ERROR "usage: cmake -DLUA_SRC_DIR=<lua>/src -P cmake/picos_lua.cmake")
    endif()
    picos_patch_luaconf("${LUA_SRC_DIR}")
endif()
