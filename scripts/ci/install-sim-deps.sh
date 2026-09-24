#!/usr/bin/env bash
# System packages for building and testing the PC simulator on Ubuntu CI
# runners. The single list shared by build-sim.yml and e2e.yml (they used to
# keep separate copies that drifted apart). clang: make simulator-asan /
# simulator-tsan build with it (its compiler-rt carries the sanitizer runtimes).
set -euo pipefail
sudo apt-get update -q
sudo apt-get install -y \
  cmake \
  build-essential \
  clang \
  curl \
  libsdl2-dev \
  libsdl2-image-dev \
  libcurl4-openssl-dev \
  libpthread-stubs0-dev \
  python3-pip \
  python3-venv
