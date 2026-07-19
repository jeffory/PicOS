#pragma once

#include <stdbool.h>
#include <stdint.h>

// Run a native ARM ELF app inside the Unicorn Engine emulator.
// elf_path  — host filesystem path to the main.elf file
// app_dir   — PicOS path (e.g. "/apps/hello_c")
// app_id    — from app.json (e.g. "com.example.hello")
// app_name  — from app.json (e.g. "Hello C")
// Returns true if the app ran and exited normally, false on error.
bool unicorn_run_app(const char *elf_path, const char *app_dir,
                     const char *app_id, const char *app_name);
