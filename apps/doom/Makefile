# PicOS DOOM (doomgeneric) Native App Build

CC      = arm-none-eabi-gcc
CFLAGS  = -mcpu=cortex-m33 -mthumb -std=gnu99 \
          -fpie -fno-plt -ffunction-sections -fdata-sections \
          -O3 -g -Wall -Wextra -Wno-unused-parameter \
          -I. \
          -I../../sdk/native \
          -Isrc/doomgeneric \
          -DDOOMGENERIC_RESX=320 \
          -DDOOMGENERIC_RESY=200
LDFLAGS = -T linker.ld \
          -Wl,--entry=picos_main \
          -Wl,-pie \
          -Wl,--gc-sections \
          -Wl,--no-warn-rwx-segments \
          -nostartfiles -nodefaultlibs -lc -lm -lgcc

# All .c files in doomgeneric EXCEPT the other platform files and sound drivers we don't have
EXCLUDE_SRCS = src/doomgeneric/doomgeneric_%.c \
               src/doomgeneric/dummy.c \
               src/doomgeneric/icon.c \
               src/doomgeneric/i_allegro%.c \
               src/doomgeneric/i_sdl%.c \
               src/doomgeneric/i_cdmus.c \
               src/doomgeneric/i_joystick.c

DOOM_SRCS = $(filter-out $(EXCLUDE_SRCS), $(wildcard src/doomgeneric/*.c))
SRCS    = dg_picos.c stubs.c i_picos_sound.c opl.c mus_player.c $(DOOM_SRCS)

TARGET  = main.elf

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS) linker.ld ../../sdk/native/os.h ../../sdk/native/app_abi.h
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $@
	arm-none-eabi-size $@

clean:
	rm -f $(TARGET)
