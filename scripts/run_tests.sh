#!/bin/sh
set -e

cmake --build build --target sip_gateway_tests
ctest --test-dir build
