// simulator/net/include/pico/multicore.h — SIM_FIRMWARE_NET only.
//
// The pieces of the Pico SDK's multicore API that src/drivers/wifi.c uses.
// Core 0 is the simulator's main thread and Core 1 its network/audio thread
// (simulator/main.c core1_thread); get_core_num() tells them apart.
// Implemented in simulator/net/sim_net_shim.c.

#ifndef PICODECK_SIM_NET_MULTICORE_H
#define PICODECK_SIM_NET_MULTICORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 1 on the thread that called sim_net_core1_init() (the sim's Core 1
// thread), 0 everywhere else.
unsigned int get_core_num(void);

// Doorbells: the firmware rings one so Core 1 wakes before its next tick.
// The simulator's Core 1 polls every 5 ms, so claiming is bookkeeping and
// ringing only counts (sim_net_doorbell_count()).
void multicore_doorbell_claim(unsigned int doorbell_num, unsigned int core_mask);
void multicore_doorbell_set_other_core(unsigned int doorbell_num);

#ifdef __cplusplus
}
#endif

#endif  // PICODECK_SIM_NET_MULTICORE_H
