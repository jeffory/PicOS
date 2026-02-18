#pragma once

// =============================================================================
// PicoCalc Hardware Pin Definitions
// Target: Pimoroni Pico Plus 2 W + ClockworkPi PicoCalc v2.0 mainboard
//
// Sources: clockworkpi/PicoCalc schematic, zenodante driver, community reports
// Verify against clockwork_Mainboard_V2.0_Schematic.pdf if anything is wrong.
// =============================================================================

// --- Display: ST7365P (320x320 IPS), SPI1 -----------------------------------
// SPI1 is shared with the WiFi chip on Pico 2W. The OS must coordinate access.
// Pins confirmed from working constellation-pico Rust project (official PicoCalc repo).
// Note: backlight is controlled by the STM32 keyboard MCU, not the RP2350.
#define LCD_SPI_PORT    spi1
#define LCD_PIN_MOSI    11      // GP11 / SPI1 TX
#define LCD_PIN_SCK     10      // GP10 / SPI1 SCK
#define LCD_PIN_CS      13      // GP13 / SPI1 CS
#define LCD_PIN_DC      14      // GP14 / Data/Command
#define LCD_PIN_RST     15      // GP15 / Reset
#define LCD_WIDTH       320
#define LCD_HEIGHT      320
#define LCD_SPI_BAUD    (100 * 1000 * 1000)  // 100 MHz — achievable at 200 MHz sysclk (÷2)

// --- SD Card: FatFS, SPI0 ---------------------------------------------------
#define SD_SPI_PORT     spi0
#define SD_PIN_MISO     16      // GP16 / SPI0 RX
#define SD_PIN_CS       17      // GP17 / SPI0 CS
#define SD_PIN_SCK      18      // GP18 / SPI0 SCK
#define SD_PIN_MOSI     19      // GP19 / SPI0 TX
#define SD_SPI_BAUD     (10 * 1000 * 1000)   // 10 MHz

// --- Keyboard: STM32F103 via I2C1 -------------------------------------------
// Pins confirmed from working constellation-pico Rust project (official PicoCalc repo).
#define KBD_I2C_PORT    i2c1
#define KBD_PIN_SDA      6      // GP6 / I2C1 SDA
#define KBD_PIN_SCL      7      // GP7 / I2C1 SCL
#define KBD_I2C_ADDR    0x1F   // STM32 keyboard controller default address
#define KBD_I2C_BAUD    (100 * 1000)  // 100 kHz standard I2C — 10× faster than 10 kHz
                                      // Revert to (10 * 1000) if keyboard reliability regresses

// STM32 register map (from clockworkpi/PicoCalc picocalc_keyboard firmware)
// Read protocol:  send reg address (1 byte, nostop=false i.e. STOP), wait, then read N bytes
// Write protocol: send { reg | KBD_WRITE_MASK, value } as 2 bytes
#define KBD_WRITE_MASK  0x80   // OR with register address when writing
#define KBD_REG_KEY     0x04   // FIFO count: bits[4:0]=pending events, bit5=capslock, bit6=numlock
#define KBD_REG_FIF     0x09   // FIFO read: 2 bytes per event — [state, keycode]
#define KBD_REG_BL      0x05   // LCD backlight brightness (0-255)
#define KBD_REG_BAT     0x0B   // Battery percent (bit7=charging flag, bits[6:0]=0-100)

// --- Audio: PWM -------------------------------------------------------------
#define AUDIO_PIN_L     26      // GP26 / Left speaker PWM
#define AUDIO_PIN_R     27      // GP27 / Right speaker PWM

// --- UART Debug -------------------------------------------------------------
#define DBG_UART_TX      0      // GP0 / UART0 TX → USB serial on Pico
#define DBG_UART_RX      1      // GP1 / UART0 RX
