# PicOS GBC Emulator Native App Build
#
# Produces main.elf — a Position-Independent ELF32 ARM Thumb-2 binary.
# Copy main.elf + app.json to /apps/gbc/ on the SD card.
#
# Requirements:
#   arm-none-eabi-gcc (any modern version; Fedora: gcc-arm-none-eabi-cs)

CC      = arm-none-eabi-gcc
CFLAGS  = -mcpu=cortex-m33 -mthumb \
          -fpie -fno-plt -ffunction-sections -fdata-sections \
          -O3 -flto -Wall -Wextra -Wno-unused-parameter \
          -I. \
          -IRP2350-GBC/inc \
          -IRP2350-GBC/ext/minigb_apu \
          -DPEANUT_GB_HIGH_LCD_ACCURACY=0 \
          -DPEANUT_GB_IS_LITTLE_ENDIAN=1 \
          -DPEANUT_FULL_GBC_SUPPORT=1 \
          -DPEANUT_GB_12_COLOUR=1 \
          -DENABLE_LCD=1 \
          -DENABLE_SOUND=1 \
          -DENABLE_SDCARD=1 \
          -DENABLE_RTC=0
LDFLAGS = -T linker.ld \
          -Wl,--entry=picos_main \
          -Wl,-pie \
          -Wl,--gc-sections \
          -Wl,--no-warn-rwx-segments \
          -nostartfiles -nodefaultlibs -lc -lm

# Test with display.c only
SRCS    = main.c display.c fs.c input.c RP2350-GBC/ext/minigb_apu/minigb_apu.c

TARGET  = main.elf

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS) linker.ld RP2350-GBC/inc/peanut_gb.h RP2350-GBC/ext/minigb_apu/minigb_apu.h os.h app_abi.h
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $@
	arm-none-eabi-size $@

clean:
	rm -f $(TARGET)
