// PicOS PC Simulator - Main Entry Point
// Cross-platform SDL2 implementation for desktop debugging

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "hal/hal_display.h"
#include "hal/hal_input.h"
#include "hal/hal_sdcard.h"
#include "hal/hal_psram.h"
#include "hal/hal_timing.h"
#include "hal/hal_audio.h"
#include "hal/hal_threading.h"
#include "sim_socket.h"

// PicOS includes (adapted for simulator)
#include "os.h"
#include "terminal.h"
#include "launcher.h"
#include "lua_psram_alloc.h"
#include "splash_logo.h"
#include "drivers/display.h"

// Simulator configuration
#define SIM_WINDOW_TITLE "PicOS Simulator"
#define SIM_DEFAULT_SD_CARD "."

// Global state
static volatile sig_atomic_t g_running = 1;
static char g_sd_card_path[512] = SIM_DEFAULT_SD_CARD;
static char g_launch_app[128] = "";  // App to auto-launch
static int g_auto_launch_done = 0;   // Flag to track if auto-launch was attempted
static int g_show_splash = 0;        // Show boot splash screen

// External function from keyboard stub
extern void set_simulator_exit_flag(volatile int *flag);

// Function to check if we should auto-launch
const char* simulator_get_auto_launch_app(void) {
    if (!g_auto_launch_done && g_launch_app[0] != '\0') {
        g_auto_launch_done = 1;
        return g_launch_app;
    }
    return NULL;
}

// Signal handler for clean shutdown
static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
    extern void dev_commands_set_exit(void);
    dev_commands_set_exit();
}

// Print usage
static void print_usage(const char* program) {
    printf("Usage: %s [options]\n", program);
    printf("Options:\n");
    printf("  --sd-card PATH    Path to simulated SD card (default: %s)\n", SIM_DEFAULT_SD_CARD);
    printf("  --launch APP      Auto-launch app on startup\n");
    printf("  --show-splash     Show boot splash screen with delays\n");
    printf("  --debug          Enable debug logging\n");
    printf("  --help           Show this help\n");
}

// Parse command line arguments
static void parse_args(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else if (strcmp(argv[i], "--sd-card") == 0 && i + 1 < argc) {
            strncpy(g_sd_card_path, argv[i + 1], sizeof(g_sd_card_path) - 1);
            g_sd_card_path[sizeof(g_sd_card_path) - 1] = '\0';
            i++;
        } else if (strcmp(argv[i], "--launch") == 0 && i + 1 < argc) {
            strncpy(g_launch_app, argv[i + 1], sizeof(g_launch_app) - 1);
            g_launch_app[sizeof(g_launch_app) - 1] = '\0';
            i++;
        } else if (strcmp(argv[i], "--show-splash") == 0) {
            g_show_splash = 1;
        } else if (strcmp(argv[i], "--debug") == 0) {
            hal_set_debug_mode(1);
        } else {
            printf("Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            exit(1);
        }
    }
}

// Draw splash screen with status message
static void draw_splash_screen(const char* status, const char* subtext) {
    // Clear to black
    uint16_t* fb = hal_display_get_framebuffer();
    for (int i = 0; i < 320 * 320; i++) {
        fb[i] = 0x0000;  // Black in RGB565
    }

#if LOGO_W > 0 && LOGO_H > 0
    // Draw logo centered
    int lx = (320 - LOGO_W) / 2;
    int ly = (320 - LOGO_H) / 2 - 16;

    // Draw the logo pixel by pixel
    for (int dy = 0; dy < LOGO_H && ly + dy < 320; dy++) {
        for (int dx = 0; dx < LOGO_W && lx + dx < 320; dx++) {
            if (lx + dx >= 0 && ly + dy >= 0) {
                fb[(ly + dy) * 320 + (lx + dx)] = logo_data[dy * LOGO_W + dx];
            }
        }
    }

    // Draw status text (using simple font approximation)
    if (status && status[0]) {
        // Draw status at bottom of screen
        // For now, we'll skip text drawing since we don't have a simple text renderer here
        // The visual logo is sufficient for the splash screen
        (void)status;
        (void)subtext;
    }
#endif

    // Present to screen
    hal_display_present();
}

// Show boot splash sequence with artificial delays
static void show_boot_splash(void) {
    printf("[Core0] Showing boot splash screen...\n");
    fflush(stdout);

    // Initial splash - "PicOS Simulator"
    draw_splash_screen("PicOS Simulator", "Starting up...");
    hal_sleep_ms(800);

    // Show initialization steps like hardware does
    draw_splash_screen("Initialising keyboard...", NULL);
    hal_sleep_ms(600);

    draw_splash_screen("Mounting SD card...", NULL);
    hal_sleep_ms(800);

    draw_splash_screen("Initialising WiFi...", NULL);
    hal_sleep_ms(600);

    draw_splash_screen("Loading...", NULL);
    hal_sleep_ms(500);

    printf("[Core0] Splash sequence complete\n");
    fflush(stdout);
}

// Core 1 entry point (simulates the second core)
static void* core1_thread(void* arg) {
    (void)arg;
    printf("[Core1] Started (network/audio thread)\n");
    
    // Initialize audio
    hal_audio_init();
    
    // Core 1 main loop
    while (g_running) {
        // Update audio
        hal_audio_update();

        // Network polling — drain IPC queue, run curl_multi, poll TCP sockets
        extern void wifi_poll(void);
        wifi_poll();
        extern void http_fire_c_pending(void);
        http_fire_c_pending();

        // 5ms delay (same as hardware)
        hal_sleep_ms(5);
    }
    
    printf("[Core1] Shutting down\n");
    hal_audio_shutdown();
    return NULL;
}

int main(int argc, char** argv) {
    printf("PicOS Simulator v%s\n", PICOS_VERSION);
    printf("=====================\n\n");
    
    // Parse arguments
    parse_args(argc, argv);
    
    printf("Configuration:\n");
    printf("  SD Card: %s\n", g_sd_card_path);
    printf("  Display: 960x960 (3x scale)\n");
    printf("  Audio: SDL2\n");
    printf("  Threading: Dual-core simulation\n\n");
    
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    
    // Initialize subsystems
    if (!hal_display_init(SIM_WINDOW_TITLE)) {
        fprintf(stderr, "Display initialization failed\n");
        SDL_Quit();
        return 1;
    }

    // Show boot splash if requested (before other init for visibility)
    if (g_show_splash) {
        show_boot_splash();
    }

    if (!hal_input_init()) {
        fprintf(stderr, "Input initialization failed\n");
        hal_display_shutdown();
        SDL_Quit();
        return 1;
    }
    
    if (!hal_sdcard_init(g_sd_card_path)) {
        fprintf(stderr, "SD card initialization failed\n");
        hal_input_shutdown();
        hal_display_shutdown();
        SDL_Quit();
        return 1;
    }
    
    if (!hal_psram_init()) {
        fprintf(stderr, "PSRAM initialization failed\n");
        hal_sdcard_shutdown();
        hal_input_shutdown();
        hal_display_shutdown();
        SDL_Quit();
        return 1;
    }
    
    hal_timing_init();

    // Initialize networking (before Core 1 starts)
    extern void http_init(void);
    extern void tcp_init(void);
    extern void wifi_init(void);
    http_init();
    tcp_init();
    wifi_init();

    // Start Core 1 thread (simulates second core)
    thread_t core1;
    if (!hal_thread_create(&core1, core1_thread, NULL)) {
        fprintf(stderr, "Failed to create Core 1 thread\n");
        hal_psram_shutdown();
        hal_sdcard_shutdown();
        hal_input_shutdown();
        hal_display_shutdown();
        SDL_Quit();
        return 1;
    }
    
    printf("[Core0] Starting main loop...\n");
    fflush(stdout);
    
    // Set up exit flag for keyboard stub
    set_simulator_exit_flag(&g_running);
    
    // Initialize Lua heap (required for Lua apps)
    printf("[Core0] Initializing Lua heap...\n");
    fflush(stdout);
    lua_psram_alloc_init();
    printf("[Core0] Lua heap initialized\n");
    fflush(stdout);
    
    // Run the app launcher
    // If --launch was specified, the launcher will handle it after scanning apps
    printf("[Core0] About to start launcher...\n");
    fflush(stdout);
    printf("[Core0] Starting launcher...\n");
    fflush(stdout);

    extern void dev_commands_init(void);
    dev_commands_init();

    sim_socket_init();

    launcher_run();
    printf("[Core0] Launcher returned\n");
    fflush(stdout);
    printf("[Core0] Launcher exited, setting g_running=0\n");
    fflush(stdout);
    g_running = 0;
    
    printf("\n[Core0] Shutting down...\n");
    
    // Wait for Core 1 to finish
    hal_thread_join(&core1);
    
    // Cleanup
    sim_socket_close();
    hal_psram_shutdown();
    hal_sdcard_shutdown();
    hal_input_shutdown();
    hal_display_shutdown();
    SDL_Quit();
    
    printf("Simulator exited cleanly.\n");
    return 0;
}
