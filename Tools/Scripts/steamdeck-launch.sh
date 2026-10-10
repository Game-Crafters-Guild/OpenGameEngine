#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

# Steam provides its own session environment. SSH launches need the local
# desktop session defaults; derive the user ID instead of assuming UID 1000.
if [[ -n "${SSH_CONNECTION:-}" ]]; then
  runtime_dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
  export XDG_RUNTIME_DIR="$runtime_dir"
  export DISPLAY="${DISPLAY:-:0}"
  if [[ -z "${XAUTHORITY:-}" ]]; then
    for candidate in "$runtime_dir"/xauth_*; do
      if [[ -f "$candidate" ]]; then
        export XAUTHORITY="$candidate"
        break
      fi
    done
  fi
  if [[ -z "${DBUS_SESSION_BUS_ADDRESS:-}" && -S "$runtime_dir/bus" ]]; then
    export DBUS_SESSION_BUS_ADDRESS="unix:path=$runtime_dir/bus"
  fi
fi

export GE_PACKAGED_PLAYER=1
# Retain known working Deck fallbacks until tested on the target driver. Both
# are overridable for device qualification and performance comparisons.
export GE_VK_USE_DESCRIPTOR_BUFFER="${GE_VK_USE_DESCRIPTOR_BUFFER:-0}"
if [[ "${GE_DECK_WSI_BYPASS:-1}" == 1 ]]; then
  unset ENABLE_GAMESCOPE_WSI
  export DISABLE_GAMESCOPE_WSI="${DISABLE_GAMESCOPE_WSI:-1}"
  export GAMESCOPE_WSI_FORCE_BYPASS="${GAMESCOPE_WSI_FORCE_BYPASS:-1}"
fi
export LD_LIBRARY_PATH="$PWD${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$PWD/Player" "$@"
