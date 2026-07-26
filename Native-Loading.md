# Native App Development

PicOS supports running native ARM Cortex-M33 (RP2350) applications in addition to Lua scripts. Native apps are Position-Independent ELF32 (PIE) binaries loaded from the SD card into PSRAM at runtime.

## Application Structure

A native application typically consists of:
- `main.elf`: The compiled binary.
- `app.json`: Metadata for the launcher (name, description, requirements).

Place these files in `/apps/<your_app_name>/` on the SD card.

### app.json

```json
{
  "id": "com.example.myapp",
  "name": "My App",
  "description": "A native PicOS app",
  "version": "1.0",
  "author": "Your Name",
  "requirements": ["audio", "http"]
}
```

See [[Global Variables and Permissions]] for available requirements (`filesystem`, `root-filesystem`, `http`, `audio`).

## Development Environment

### Prerequisites
- **GNU Arm Embedded Toolchain**: `arm-none-eabi-gcc`
- **PicOS SDK Headers**: `app_abi.h` and `os.h` (found in `sdk/native/`)
- **Linker Script**: `linker.ld` (found in `sdk/native/`)

## Creating a Native App

### Entry Point

Your application must define an entry point named `picos_main`. The OS passes a pointer to the `PicoCalcAPI` struct, which provides access to all OS services.

```c
#include "app_abi.h"
#include "os.h"

void picos_main(const PicoCalcAPI *api,
                const char *app_dir,
                const char *app_id,
                const char *app_name)
{
    const picocalc_display_t *d = api->display;
    const picocalc_input_t   *i = api->input;
    const picocalc_sys_t     *s = api->sys;

    while (true) {
        s->poll();  // REQUIRED: polls keyboard, fires pending callbacks,
                    // handles system menu (Sym key)

        if (s->shouldExit())  // User selected "Exit App" from system menu
            return;

        if (i->getButtonsPressed() & BTN_ESC)
            return;

        d->clear(0x0000);
        d->drawText(10, 10, "Hello from C!", 0xFFFF, 0x0000);
        d->flush();
    }
}
```

**Important**: You must call `s->poll()` each frame and check `s->shouldExit()` to properly handle the system menu exit. Returning from `picos_main` returns control to the launcher.

### The API Surface

The `PicoCalcAPI` struct contains pointers to all OS subsystems. The full type definitions are in `sdk/native/os.h`.

#### Core subsystems (always available)

| Pointer | Type | Description |
|---------|------|-------------|
| `api->input` | `picocalc_input_t` | Button state, edge detection, character input |
| `api->display` | `picocalc_display_t` | Drawing primitives, framebuffer flush, effects |
| `api->fs` | `picocalc_fs_t` | SD card file I/O (open, read, write, list) |
| `api->sys` | `picocalc_sys_t` | Time, battery, reboot, menu items, logging, `poll()`, `shouldExit()` |
| `api->audio` | `picocalc_audio_t` | Tone generation, PCM streaming |
| `api->wifi` | `picocalc_wifi_t` | WiFi connect/disconnect/status (Pico 2W only) |
| `api->tcp` | `picocalc_tcp_t` | Raw TCP/TLS sockets (non-blocking) |
| `api->ui` | `picocalc_ui_t` | Modal dialogs: text input, confirmation |
| `api->psram` | `picocalc_psram_t` | PIO PSRAM and QMI PSRAM allocation |
| `api->perf` | `picocalc_perf_t` | FPS counting, frame timing |
| `api->terminal` | `picocalc_terminal_t` | Terminal emulator widget |

#### Phase 1 additions (`api->version >= 1`)

| Pointer | Type | Description |
|---------|------|-------------|
| `api->http` | `picocalc_http_t` | HTTP/HTTPS client (pool of 8 connections) |
| `api->soundplayer` | `picocalc_soundplayer_t` | Sample playback, file player, MP3 player |
| `api->appconfig` | `picocalc_appconfig_t` | Per-app key/value config (`/data/<APP_ID>/config.json`) |
| `api->crypto` | `picocalc_crypto_t` | SHA-256, SHA-1, HMAC, AES-CTR, ECDH, signature verification |

#### Phase 2 additions (`api->version >= 2`)

| Pointer | Type | Description |
|---------|------|-------------|
| `api->graphics` | `picocalc_graphics_t` | Image loading (BMP/JPEG/PNG/GIF), drawing, scaling |
| `api->video` | `picocalc_video_t` | MJPEG video playback with audio |
| `api->modplayer` | `picocalc_modplayer_t` | MOD tracker music playback |
| `api->zip` | `picocalc_zip_t` | ZIP archive extraction |

#### Version detection

The `api->version` field indicates which additions are present:

- `1` — Phase 1 additions
- `2` — Phase 2 additions
- `3` — `api->fs->browse` (modal file-browser overlay for native apps)
- `4` — display clip rect (`setClipRect`/`getClipRect`/`clearClipRect`), mode-7 `drawPlane`, and native parity for `fillHLine`/`fillTriangle`/`setScrollArea`/`setScrollOffset`

```c
if (api->version >= 2) {
    // Phase 2 APIs are available
    pcimage_t img = api->graphics->load("/apps/myapp/logo.bmp");
}
```

`sdk/native/os.h` is the source of truth for the complete type definitions, the exact struct layout, and the latest `version` values. The C API maps directly to the `picocalc.*` Lua modules documented in the [[Lua SDK Reference]].

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
- `-Wl,--no-warn-rwx-segments`: Suppress linker warnings about RWX segments.
- `-nostartfiles -nodefaultlibs`: Native apps do not use standard C runtime startup (CRT0).

### Example Makefile
A complete working example can be found in `sdk/native/Makefile`.

```makefile
CC      = arm-none-eabi-gcc
CFLAGS  = -mcpu=cortex-m33 -mthumb -fpie -fno-plt -Os -I.
LDFLAGS = -T linker.ld -Wl,--entry=picos_main -Wl,-pie \
          -Wl,--no-warn-rwx-segments -nostartfiles -nodefaultlibs

all: main.elf

main.elf: main.c
	$(CC) $(CFLAGS) main.c $(LDFLAGS) -o $@
```

## Binary Loading Process

When the PicOS launcher starts a native app:
1. Core 1 is paused to prevent PSRAM heap contention during loading.
2. The `main.elf` file is read and the ELF header and program headers are validated.
3. The virtual address range of all `PT_LOAD` segments is computed.
4. **Split-mode loading**: If the code segment (`PF_X`) fits in SRAM, it is placed there for faster execution. Data/BSS segments go into PSRAM via `umm_malloc`. If SRAM is insufficient, everything goes into PSRAM.
5. Code written to PSRAM uses the uncached alias (`0x15xxxxxx`) to bypass write-back cache, then XIP cache is invalidated. Execution uses the cached alias (`0x11xxxxxx`) so the 16KB XIP cache serves most instruction fetches.
6. `R_ARM_RELATIVE` relocations are applied with dual bias (code bias for SRAM, data bias for PSRAM).
7. The app runs on the **Process Stack Pointer (PSP)** via an 8KB static SRAM stack buffer, keeping the main stack (MSP) available for interrupt handlers.
8. Core 1 is resumed after the app exits and resources are freed.

The application runs in the same privilege level as the OS but is expected to return control to the OS by returning from `picos_main`.

## Memory

- **Stack**: 8KB (static SRAM buffer, runs on PSP). Do not use large stack allocations.
- **SRAM heap**: ~28.8KB available via `malloc()`/`free()`. Very limited — free promptly.
- **PSRAM heap**: 8MB available via `api->psram->qmiAlloc()`/`api->psram->qmiFree()`. Use for large allocations.
- **PIO PSRAM**: 8MB secondary PSRAM available via `api->psram->pioRead()`/`api->psram->pioWrite()` for bulk data.

**Important**: Do not mix `malloc`/`free` (SRAM) with PSRAM allocation functions. They use separate heaps.

## Debugging

- Native apps can log via `api->sys->log("message: %d", value)`. Output appears on USB serial at 115200 baud as `[APP] message`.
- If the app crashes (HardFault), the fault handler displays register state, CFSR flags, and stack pointer info on the LCD. It detects whether the crash was in the app (PSP) or OS (MSP).
- Stack overflow signature: OVFL with SP below `__StackBottom` (0x20081000) and BFAR at 0x35xxxxxx.

## See also

- [[Lua SDK Reference]] — Lua API documentation (C API mirrors these modules)
- [[Global Variables and Permissions]] — App requirements
- [[Crash Logging and Watchdog]] — Fault recovery and reboot behavior
