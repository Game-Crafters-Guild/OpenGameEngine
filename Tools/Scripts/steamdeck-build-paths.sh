#!/usr/bin/env bash
# Shared default paths for Steam Deck Docker build and deploy scripts.

ge_steamdeck_default_host_dist_dir() {
  # Avoid spaces in the path — Docker bind mounts on macOS are unreliable under
  # ~/Library/Application Support/ for long container builds.
  if [[ "$(uname -s)" == Darwin ]]; then
    echo "${HOME}/Documents/GameEngine/Build/SteamDeck"
  elif [[ -n "${XDG_DATA_HOME:-}" ]]; then
    echo "${XDG_DATA_HOME}/GameEngine/Build/SteamDeck"
  else
    echo "${HOME}/.local/share/GameEngine/Build/SteamDeck"
  fi
}
