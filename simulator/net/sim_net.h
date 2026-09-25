// simulator/net/sim_net.h — hooks the simulator calls in a SIM_FIRMWARE_NET
// build (firmware src/drivers/wifi.c + http.c + tcp.c on Mongoose/POSIX).

#ifndef PICOS_SIM_NET_H
#define PICOS_SIM_NET_H

#include <stdint.h>

// Mark the calling thread as Core 1 (get_core_num() == 1). wifi_poll() is a
// no-op on any other core, exactly as on the device. Call first thing on the
// simulator's Core 1 thread.
void sim_net_core1_init(void);

// Doorbell rings so far (Core 0 → Core 1 IPC wake-ups; diagnostics).
uint32_t sim_net_doorbell_count(void);

#endif  // PICOS_SIM_NET_H
