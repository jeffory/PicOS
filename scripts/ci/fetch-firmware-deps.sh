#!/usr/bin/env bash
# Toolchain and third-party sources for a firmware build on Ubuntu CI
# runners (mirrors the steps in release.yml). Pico SDK goes to $HOME/pico-sdk.
set -euo pipefail
sudo apt-get update -q
sudo apt-get install -y cmake curl unzip gcc-arm-none-eabi libnewlib-arm-none-eabi \
  libstdc++-arm-none-eabi-newlib

# Lua 5.4.7, patched to honour the CMake Lua config (see cmake/picodeck_lua.cmake).
make download-lua

# FatFS R0.15 upstream sources only; ffconf.h and port/diskio_spi.c are tracked.
curl -fL https://elm-chan.org/fsw/ff/arc/ff15.zip -o /tmp/ff15.zip
unzip -o -j /tmp/ff15.zip \
  'source/ff.c' 'source/ff.h' 'source/diskio.h' \
  'source/ffsystem.c' 'source/ffunicode.c' \
  -d third_party/fatfs/

if [ ! -d "$HOME/pico-sdk" ]; then
  git clone --depth 1 --branch 2.2.0 https://github.com/raspberrypi/pico-sdk.git "$HOME/pico-sdk"
  (cd "$HOME/pico-sdk" && git submodule update --init --depth 1)
fi
