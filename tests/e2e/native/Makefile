# Builds the E2E native fixture apps against the public SDK (sdk/native):
# native_api_probe (api_probe.c) and native_leaky (leaky.c).
# The outputs are committed so the E2E job needs no ARM toolchain; rebuild
# and commit them whenever sdk/native/os.h changes:
#
#   make -C tests/e2e/native
#
# CI's native-sdk job runs this too (into a scratch dir), so the SDK header
# always compiles.

SDK     ?= ../../../sdk/native
OUT     ?= ../apps/native_api_probe
LEAKY   ?= ../apps/native_leaky
CC       = arm-none-eabi-gcc
CFLAGS   = -mcpu=cortex-m33 -mthumb -fpie -fno-plt \
           -ffunction-sections -fdata-sections -Os \
           -Wall -Wextra -Werror -I$(SDK)
LDFLAGS  = -T $(SDK)/linker.ld -Wl,--entry=picodeck_main -Wl,-pie \
           -Wl,--gc-sections -Wl,--no-warn-rwx-segments \
           -nostartfiles -nodefaultlibs -Wl,--build-id=none

.PHONY: all clean
all: $(OUT)/main.elf $(LEAKY)/main.elf

$(OUT)/main.elf: api_probe.c $(SDK)/os.h $(SDK)/app_abi.h $(SDK)/linker.ld
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) api_probe.c $(LDFLAGS) -o $@

$(LEAKY)/main.elf: leaky.c $(SDK)/os.h $(SDK)/app_abi.h $(SDK)/linker.ld
	@mkdir -p $(LEAKY)
	$(CC) $(CFLAGS) leaky.c $(LDFLAGS) -o $@

clean:
	rm -f $(OUT)/main.elf $(LEAKY)/main.elf
