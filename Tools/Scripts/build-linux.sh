#!/usr/bin/env bash
set -euo pipefail

# Simple helper script to configure and build the C++ engine on Linux.
# Configures a Ninja Debug tree with examples and tests, scripting off.

BUILD_DIR=${1:-build}

cmake -S . -B "${BUILD_DIR}" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_EXAMPLES=ON \
  -DBUILD_TESTING=ON \
  -DENABLE_SCRIPTING=OFF

cmake --build "${BUILD_DIR}" -- -j"$(nproc)"

# Optionally run tests (uncomment if you want this by default):
# ctest --test-dir "${BUILD_DIR}" --output-on-failure

