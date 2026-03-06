# Native App Development

PicOS supports running native ARM Cortex-M33 (RP2350) applications in addition to Lua scripts. Native apps are Position-Independent ELF32 (PIE) binaries loaded from the SD card into PSRAM at runtime.

## Application Structure

A native application typically consists of:
- `main.elf`: The compiled binary.
- `app.json`: Metadata for the launcher (icon, name, description).

Place these files in `/apps/<your_app_name>/` on the SD card.

## Development Environment

### Prerequisites
- **GNU Arm Embedded Toolchain**: `arm-none-eabi-gcc`
- **PicOS SDK Headers**: `app_abi.h` and `os.h` (found in `sdk/native/`)
- **Linker Script**: `linker.ld` (found in `sdk/native/`)

## Creating a Native App

### Entry Point

Your application must define an entry point named `picos_main`. The OS passes a pointer to the `PicoCalcAPI` struct, which provides access to all OS services (Display, Input, FS, etc.).

```c
#include "app_abi.h"
#include "os.h"

void picos_main(const PicoCalcAPI *api,
                const char *app_dir,
                const char *app_id,
                const char *app_name)
{
    const picocalc_display_t *d = api->display;
    const picocalc_sys_t     *s = api->sys;

    while (true) {
        d->clear(0x0000);
        d->drawText(10, 10, "Hello from C!", 0xFFFF, 0x0000);
        d->flush();

        s->poll(); // Poll system events
        if (api->input->getButtonsPressed())
            break;
    }
}
```

### The API Surface

The `PicoCalcAPI` struct contains pointers to various subsystems:
- `api->input`: Keyboard and button state.
- `api->display`: Drawing primitives (pixels, lines, rects, text) and screen flush.
- `api->fs`: SD card file operations.
- `api->sys`: System time, reboot, battery status, menu items, and logging.
- `api->audio`: Tone generation and volume control.
- `api->wifi`: Network connectivity (on supported hardware).

See [Lua SDK Reference](Lua-SDK-Reference.md) for detailed descriptions of these functions, as the C API maps directly to the Lua modules.

## Compilation

Native apps MUST be compiled as **Position-Independent Executables (PIE)**. This allows the OS to load them anywhere in memory.

### Required CFLAGS
- `-fpie`: Generate position-independent code.
- `-fno-plt`: Avoid Procedure Linkage Table.
- `-mcpu=cortex-m33 -mthumb`: Target the RP2350 processor.

### Required LDFLAGS
- `-T linker.ld`: Use the provided linker script.
- `-Wl,--entry=picos_main`: Set the entry point.
- `-Wl,-pie`: Final link as PIE.
- `-nostartfiles -nodefaultlibs`: Native apps do not use standard C runtime startup (CRT0).

### Example Makefile
A complete working example can be found in `sdk/native/Makefile`.

```makefile
CC      = arm-none-eabi-gcc
CFLAGS  = -mcpu=cortex-m33 -mthumb -fpie -fno-plt -Os -I.
LDFLAGS = -T linker.ld -Wl,--entry=picos_main -Wl,-pie -nostartfiles -nodefaultlibs

all: main.elf

main.elf: main.c
	$(CC) $(CFLAGS) main.c $(LDFLAGS) -o $@
```

## Binary Loading Process

When the PicOS launcher starts a native app:
1. It reads the `main.elf` file from the SD card.
2. It parses the ELF program headers.
3. It allocates space in PSRAM for the `PT_LOAD` segments.
4. It copies the code and data into PSRAM.
5. It performs base-relocation if necessary (though PIE usually handles this via PC-relative addressing).
6. It jumps to the `picos_main` address.

The application runs in the same privilege level as the OS but is expected to return control to the OS by exiting `picos_main`.

## TinyGo Support

TinyGo can also produce PIE binaries for ARM. Use the following flags:
- `-target=pico`: Target the RP2040/RP2350.
- `-gc=none`: Recommended for real-time performance.
- `-panic=trap`: Minimize binary size.
- Export `picos_main` using `//export picos_main`.
