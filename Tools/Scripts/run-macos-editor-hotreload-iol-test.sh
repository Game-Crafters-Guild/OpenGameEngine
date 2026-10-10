#!/usr/bin/env bash
set -euo pipefail

# macOS Editor IoL + HotReload test harness (parity with Windows/Linux).
# Runs the Editor, verifies an initial InitializeOnLoad Boot() (v1),
# edits TestInit.cs, and verifies a second Boot() (v2) after hot reload.

CONFIG="${CONFIG:-Debug}"
BUILD_DIR="${BUILD_DIR:-build-macos}"
INITIAL_TIMEOUT_SECONDS="${INITIAL_TIMEOUT_SECONDS:-60}"
HOTRELOAD_TIMEOUT_SECONDS="${HOTRELOAD_TIMEOUT_SECONDS:-180}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

if [[ "$(uname)" != "Darwin" ]]; then
  echo "This script is intended to run on macOS (Darwin)." >&2
  exit 1
fi

BUILD_PATH="${REPO_ROOT}/${BUILD_DIR}"
BIN_DIR="${BUILD_PATH}/bin/${CONFIG}"
EDITOR_PATH="${BIN_DIR}/Editor"

if [[ ! -x "${EDITOR_PATH}" ]]; then
  echo "Editor executable not found at '${EDITOR_PATH}'. Build the Editor (${CONFIG}) before running this test." >&2
  exit 1
fi

RUNTIME_ASSETS_DIR="${BIN_DIR}/Assets"
mkdir -p "${RUNTIME_ASSETS_DIR}"
RUNTIME_TESTINIT_PATH="${RUNTIME_ASSETS_DIR}/TestInit.cs"

RUNTIME_ORIGINAL_CONTENT=""
RUNTIME_HAS_ORIGINAL=0
if [[ -f "${RUNTIME_TESTINIT_PATH}" ]]; then
  RUNTIME_ORIGINAL_CONTENT="$(cat "${RUNTIME_TESTINIT_PATH}")"
  RUNTIME_HAS_ORIGINAL=1
fi

LOG_DIR="${REPO_ROOT}/Logs"
mkdir -p "${LOG_DIR}"
EDITOR_LOG="${LOG_DIR}/Editor-macOS-IoL-HotReload.log"
IOL_TRACE="${LOG_DIR}/Editor-macOS-IoL-HotReload-ioltrace.log"

rm -f "${EDITOR_LOG}" "${IOL_TRACE}"

read -r -d '' BEFORE_SOURCE <<'EOF'
using System;
using System.IO;
using GameEngine.Scripting;

namespace GameScripts
{
    public static class SampleInit
    {
        [InitializeOnLoad]
        public static void Boot()
        {
            string tracePath = Environment.GetEnvironmentVariable("GE_HOTRELOAD_IOL_TRACE_FILE") ?? "<null>";
            string marker = $"[Test] InitializeOnLoad Boot() called v1 (GE_HOTRELOAD_IOL_TRACE_FILE='{tracePath}')";
            Console.WriteLine(marker);

            if (!string.IsNullOrEmpty(tracePath) && !string.Equals(tracePath, "<null>", StringComparison.Ordinal))
            {
                try
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(tracePath)!);
                    File.AppendAllText(tracePath, marker + Environment.NewLine);
                }
                catch
                {
                }
            }
        }
    }
}
EOF

read -r -d '' AFTER_SOURCE <<'EOF'
using System;
using System.IO;
using GameEngine.Scripting;

namespace GameScripts
{
    public static class SampleInit
    {
        [InitializeOnLoad]
        public static void Boot()
        {
            string tracePath = Environment.GetEnvironmentVariable("GE_HOTRELOAD_IOL_TRACE_FILE") ?? "<null>";
            string marker = $"[Test] InitializeOnLoad Boot() called v2 (GE_HOTRELOAD_IOL_TRACE_FILE='{tracePath}')";
            Console.WriteLine(marker);

            if (!string.IsNullOrEmpty(tracePath) && !string.Equals(tracePath, "<null>", StringComparison.Ordinal))
            {
                try
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(tracePath)!);
                    File.AppendAllText(tracePath, marker + Environment.NewLine);
                }
                catch
                {
                }
            }
        }
    }
}
EOF

wait_for_log_line() {
  local log_path="$1" pattern="$2" timeout="$3" label="$4" start_ts now_ts

  start_ts="$(date +%s)"
  while true; do
    now_ts="$(date +%s)"
    if (( now_ts - start_ts > timeout )); then
      echo "Timed out after ${timeout}s waiting for '${pattern}' in ${log_path} (${label})." >&2
      return 1
    fi

    if [[ -f "${log_path}" ]]; then
      if grep -Fq "${pattern}" "${log_path}"; then
        echo "Observed '${pattern}' in ${log_path} (${label})."
        return 0
      fi
    fi

    if ! kill -0 "${EDITOR_PID}" 2>/dev/null; then
      echo "Editor process exited while waiting for '${pattern}' (${label})." >&2
      return 1
    fi

    sleep 1
  done
}

cleanup() {
  # Restore original runtime TestInit.cs
  if (( RUNTIME_HAS_ORIGINAL == 1 )); then
    printf '%s' "${RUNTIME_ORIGINAL_CONTENT}" > "${RUNTIME_TESTINIT_PATH}"
  elif [[ -f "${RUNTIME_TESTINIT_PATH}" ]]; then
    rm -f "${RUNTIME_TESTINIT_PATH}"
  fi

  if [[ -n "${EDITOR_PID:-}" ]]; then
    if kill -0 "${EDITOR_PID}" 2>/dev/null; then
      kill "${EDITOR_PID}" 2>/dev/null || true
      wait "${EDITOR_PID}" 2>/dev/null || true
    fi
  fi
}

trap cleanup EXIT

# Stage initial version into the runtime Assets folder used by the Editor
printf '%s
' "${BEFORE_SOURCE}" > "${RUNTIME_TESTINIT_PATH}"

# Configure IoL/HotReload environment
export GE_HOTRELOAD_IOL_TRACE_FILE="${IOL_TRACE}"
export GE_HOTRELOAD_VERBOSE="1"

echo "== macOS Editor IoL HotReload test =="
echo "Editor: ${EDITOR_PATH}"
echo "Logfile: ${EDITOR_LOG}"

"${EDITOR_PATH}" -logfile "${EDITOR_LOG}" &
EDITOR_PID=$!

# Initial IoL Boot (v1)
wait_for_log_line "${IOL_TRACE}" "InitializeOnLoad Boot() called v1" "${INITIAL_TIMEOUT_SECONDS}" "initial IoL Boot v1"

# Apply hot-reload change (v2)
printf '%s
' "${AFTER_SOURCE}" > "${RUNTIME_TESTINIT_PATH}"

# Hot-reload IoL Boot (v2)
wait_for_log_line "${IOL_TRACE}" "InitializeOnLoad Boot() called v2" "${HOTRELOAD_TIMEOUT_SECONDS}" "hot-reload IoL Boot v2"

echo "macOS Editor IoL hot-reload test completed successfully."
