# Welcome to PicOS Development!


Welcome to the documentation for PicOS, a lightweight app platform for the [ClockworkPi PicoCalc](https://www.clockworkpi.com/picocalc). If you're excited about creating retro-style games and utilities with modern tools, you're in the right place.

PicOS is built on a simple yet powerful idea: a small, fast C-language kernel lives on the device, providing a safe, sandboxed Lua 5.4 environment for your applications. All your apps reside on a swappable SD card, making development as easy as editing a text file.

## The Core Philosophy

The OS handles the low-level hardware details so you can focus on your app's logic. The architecture is clean and robust:

- Resident OS (Flash): The core kernel, drivers, Lua runtime, and the main app launcher are stored on the device's internal flash.
- Your Apps (SD Card): Each app is a simple directory on the SD card containing an `app.json` metadata file and a `main.lua` script. This means you can swap apps just by swapping cards!