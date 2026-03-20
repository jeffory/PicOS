// hardware/structs/xip.h stub
#ifndef HARDWARE_STRUCTS_XIP_H
#define HARDWARE_STRUCTS_XIP_H

#include <stdint.h>

// XIP (Execute-In-Place) control register
typedef struct {
    uint32_t ctrl;
    uint32_t ctr_hit;
    uint32_t ctr_acc;
    uint32_t reserved[6];
} xip_ctrl_hw_t;

static xip_ctrl_hw_t xip_ctrl_hw_instance = {0};
static xip_ctrl_hw_t* xip_ctrl_hw = &xip_ctrl_hw_instance;

// No-op for performance counter
static inline uint32_t xip_ctrl_get_status(void) { return 0; }
static inline void xip_ctrl_set_status(uint32_t val) { (void)val; }

#endif // HARDWARE_STRUCTS_XIP_H
