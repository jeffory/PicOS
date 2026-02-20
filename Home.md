# Welcome to PicOS Development!


Welcome to the documentation for PicOS, a lightweight app platform for the [ClockworkPi PicoCalc](https://www.clockworkpi.com/picocalc). If you're excited about creating retro-style games and utilities with modern tools, you're in the right place.

PicOS is built on a simple yet powerful idea: a small, fast C-language kernel lives on the device, providing a safe, sandboxed Lua 5.4 environment for your applications. All your apps reside on a swappable SD card, making development as easy as editing a text file.

## The Core Philosophy

The OS handles the low-level hardware details so you can focus on your app's logic. The architecture is clean and robust:

- Resident OS (Flash): The core kernel, drivers, Lua runtime, and the main app launcher are stored on the device's internal flash.
- Your Apps (SD Card): Each app is a simple directory on the SD card containing an `app.json` metadata file and a `main.lua` script. This means you can swap apps just by swapping cards!

## Screenshots

<img width="640" height="640" alt="shot001" src="https://github.com/user-attachments/assets/2ab5ab5a-3921-44ba-b841-4ee365bb9fcb" />
<img width="640" height="640" alt="shot002" src="https://github.com/user-attachments/assets/7020256b-880f-47cd-885e-ba07634fa417" />
<img width="640" height="640" alt="shot003" src="https://github.com/user-attachments/assets/522b2361-821c-4470-98a0-bf9c50fae690" />
<img width="640" height="640" alt="shot004" src="https://github.com/user-attachments/assets/3606afbe-d585-47f7-99e9-747f5347af56" />
<img width="640" height="640" alt="shot005" src="https://github.com/user-attachments/assets/211e66c3-1de9-4aea-b887-e4ed78e25f2b" />
<img width="640" height="640" alt="shot006" src="https://github.com/user-attachments/assets/2793bf6c-afc1-4671-a5da-044b937039d1" />
