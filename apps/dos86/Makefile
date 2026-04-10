# DOS/86 — fake86 8086 PC emulator for PicOS

CC      = arm-none-eabi-gcc
CFLAGS  = -mcpu=cortex-m33 -mthumb -std=gnu11 \
          -fpie -fno-plt -ffunction-sections -fdata-sections \
          -O2 -Wall -Wextra -Wno-unused-parameter \
          -I. -I../../sdk/native
LDFLAGS = -T linker.ld \
          -Wl,--entry=picos_main \
          -Wl,-pie \
          -Wl,--gc-sections \
          -Wl,--no-warn-rwx-segments \
          -nostartfiles -nodefaultlibs -lc -lgcc

SRCS    = main.c backend.c scancode.c stubs.c fake86/cpu.c fake86/i8259.c fake86/i8253.c fake86/ports.c fake86/disk.c fake86/video.c fake86/render.c fake86/bios.c speaker.c opl.c opl2/opl2_core.c audio_mix.c
TARGET  = main.elf

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS) linker.ld os.h app_abi.h
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $@
	arm-none-eabi-size $@

clean:
	rm -f $(TARGET)
