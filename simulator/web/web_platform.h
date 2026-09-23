#ifndef WEB_PLATFORM_H
#define WEB_PLATFORM_H

#include <stdint.h>

// Run the simulated Core 1 service loop if its 5 ms period has elapsed.
void web_core1_tick(void);

// Hand the tab back to the browser for at least `ms` (0 = one event-loop turn).
void web_yield(uint32_t ms);

// Yield only if the OS has been running for longer than a frame without one.
void web_yield_if_due(void);

#endif
