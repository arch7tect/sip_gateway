#!/usr/bin/env sh
set -e

cmake -S . -B build
cmake --build build
