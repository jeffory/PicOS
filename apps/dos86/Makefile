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

SRCS    = main.c fake86/cpu.c stubs.c
TARGET  = main.elf

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS) linker.ld os.h app_abi.h
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $@
	arm-none-eabi-size $@

clean:
	rm -f $(TARGET)
