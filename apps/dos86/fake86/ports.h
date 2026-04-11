#pragma once
#include "config.h"

void ports_reset(void);
uint8_t  portin8(uint16_t port);
uint16_t portin16(uint16_t port);
void     portout8(uint16_t port, uint8_t val);
void     portout16(uint16_t port, uint16_t val);

/* Aliases used by cpu.c */
uint8_t  portin(uint16_t port);
void     portout(uint16_t port, uint8_t val);
