// hardware/vreg.h stub
#ifndef HARDWARE_VREG_H
#define HARDWARE_VREG_H

#include <stdint.h>

// Voltage regulator voltage levels
enum vreg_voltage {
    VREG_VOLTAGE_0_85 = 0,
    VREG_VOLTAGE_0_90,
    VREG_VOLTAGE_0_95,
    VREG_VOLTAGE_1_00,
    VREG_VOLTAGE_1_05,
    VREG_VOLTAGE_1_10,
    VREG_VOLTAGE_1_15,
    VREG_VOLTAGE_1_20,
    VREG_VOLTAGE_1_25,
    VREG_VOLTAGE_1_30,
    VREG_VOLTAGE_DEFAULT = VREG_VOLTAGE_1_15
};

// Voltage regulator functions (no-op on simulator)
static inline void vreg_set_voltage(enum vreg_voltage voltage) {
    (void)voltage;
}

static inline enum vreg_voltage vreg_get_voltage(void) {
    return VREG_VOLTAGE_DEFAULT;
}

#endif // HARDWARE_VREG_H
