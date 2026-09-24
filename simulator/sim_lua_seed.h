// Force-included into the simulator's lstate.c (simulator/CMakeLists.txt):
// the string hash seed comes from sim_lua_makeseed() in sim_test_control.c,
// fixed under --test-mode so pairs() order over string keys repeats run to
// run. (A function-like macro cannot be passed as a CMake compile
// definition, hence the header.)
#pragma once
unsigned int sim_lua_makeseed(void *L);
#define luai_makeseed(L) sim_lua_makeseed(L)
