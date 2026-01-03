#!/usr/bin/env sh
set -e

if [ ! -x build/sip_gateway ]; then
  cmake -S . -B build
  cmake --build build
fi

./build/sip_gateway
