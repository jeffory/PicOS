// Pico SDK Stubs Implementation

#include "pico_sdk_stubs.h"
#include "hal_psram.h"

// PSRAM stubs
void* psram_malloc(size_t size) {
    return hal_psram_malloc(size);
}

void psram_free(void* ptr) {
    hal_psram_free(ptr);
}

// I2C instances (dummies)
struct i2c_inst { int dummy; };
struct i2c_inst i2c0_instance = {0};
struct i2c_inst i2c1_instance = {0};
i2c_inst_t* i2c0 = &i2c0_instance;
i2c_inst_t* i2c1 = &i2c1_instance;

// SPI instances (dummies)
struct spi_inst { int dummy; };
struct spi_inst spi0_instance = {0};
struct spi_inst spi1_instance = {0};
spi_inst_t* spi0 = &spi0_instance;
spi_inst_t* spi1 = &spi1_instance;
