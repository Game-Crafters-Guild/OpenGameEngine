#!/usr/bin/env bash
set -euo pipefail

# Simple macOS harness for CoreCLR + compile-server smoke tests.
# This is intended to mirror the Windows/Linux smoke tests once
# macOS CoreCLR hosting and the compile-server transport are wired.

CONFIG="${CONFIG:-Debug}"
BUILD_DIR="${BUILD_DIR:-build-macos}"
GENERATOR="${CMAKE_GENERATOR:-Ninja}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

if [[ "$(uname)" != "Darwin" ]]; then
  echo "This script is intended to run on macOS (Darwin)." >&2
  exit 1
fi

BUILD_PATH="${REPO_ROOT}/${BUILD_DIR}"

echo "== macOS scripting smoke tests =="
echo "Repo root: ${REPO_ROOT}"
echo "Build dir: ${BUILD_PATH} (config: ${CONFIG}, generator: ${GENERATOR})"

cmake -S "${REPO_ROOT}" -B "${BUILD_PATH}" -G "${GENERATOR}" \
  -DCMAKE_BUILD_TYPE="${CONFIG}" \
  -DENABLE_SCRIPTING=ON \
  -DBUILD_TESTING=ON

cmake --build "${BUILD_PATH}" --config "${CONFIG}" \
  --target EngineCoreClrSmokeTests EngineCompileServerSmokeTests -- -j8

ctest --test-dir "${BUILD_PATH}" -C "${CONFIG}" \
  -R "Engine(CoreClr|CompileServer)SmokeTests" --output-on-failure

echo "macOS scripting smoke tests completed."
