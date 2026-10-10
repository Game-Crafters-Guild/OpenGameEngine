#!/usr/bin/env bash
# Provision the engine's pinned Emscripten SDK for wasm builds.
#
# Installs emsdk into Tools/emsdk/emsdk/ (gitignored) and activates the pinned
# version. Idempotent: re-running verifies the pin and exits fast.
#
# Usage:
#   ./Tools/emsdk/setup.sh            # install + activate the pin
#   source ./Tools/emsdk/setup.sh     # same, then exports EMSDK/PATH into the
#                                     # calling shell for the wasm-* presets
#
# Pin rationale: 6.0.5 is the engine's probed-good version (emdawnwebgpu +
# contrib.glfw3 + pthreads + ASYNCIFY all verified). Phase 5 embeds Mono via
# .NET 11 wasm-tools, whose dotnet/emsdk pin is 5.0.6->6.0.2; re-aligning the
# engine pin to the .NET-shipped emscripten is a Phase 5 concern, not this
# script's.

GE_EMSDK_VERSION="6.0.5"
GE_EMSDK_REPO="https://github.com/emscripten-core/emsdk.git"

_ge_emsdk_main() {
    local scriptDir
    scriptDir="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
    local installDir="${scriptDir}/emsdk"

    if [[ ! -d "${installDir}/.git" ]]; then
        echo "emsdk: cloning ${GE_EMSDK_REPO} -> ${installDir}"
        git clone --depth 1 "${GE_EMSDK_REPO}" "${installDir}" || return 1
    fi

    local emcc="${installDir}/upstream/emscripten/emcc"
    local haveVersion=""
    if [[ -x "${emcc}" ]]; then
        haveVersion="$("${emcc}" --version 2>/dev/null | sed -n 's/^emcc[^0-9]*\([0-9][0-9.]*\).*/\1/p' | head -1)"
    fi

    if [[ "${haveVersion}" != "${GE_EMSDK_VERSION}" ]]; then
        echo "emsdk: installing + activating ${GE_EMSDK_VERSION} (have: '${haveVersion:-none}')"
        # The pinned release tag must be visible to a shallow clone.
        git -C "${installDir}" fetch --depth 1 origin main && git -C "${installDir}" checkout FETCH_HEAD -- . 2>/dev/null
        (cd "${installDir}" && ./emsdk install "${GE_EMSDK_VERSION}" && ./emsdk activate "${GE_EMSDK_VERSION}") || return 1
    else
        # Activation state (.emscripten config) can lag the installed toolchain
        # when several versions are installed side by side; re-activate cheaply.
        (cd "${installDir}" && ./emsdk activate "${GE_EMSDK_VERSION}" >/dev/null) || return 1
    fi

    echo "emsdk: ${GE_EMSDK_VERSION} ready at ${installDir}"
    export EMSDK="${installDir}"
    export PATH="${installDir}:${installDir}/upstream/emscripten:${PATH}"
    "${emcc}" --version | head -1
}

_ge_emsdk_main
_ge_emsdk_status=$?
# Sourced: keep the exports, don't kill the caller's shell on failure.
(return 0 2>/dev/null) || exit ${_ge_emsdk_status}
