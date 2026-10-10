#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=steamdeck-build-paths.sh
source "$script_dir/steamdeck-build-paths.sh"

repo_root="$(cd "$script_dir/../.." && pwd)"

default_deploy_source="$(ge_steamdeck_default_host_dist_dir)/steamdeck-player"
source_dir="${GE_STEAMDECK_DEPLOY_SOURCE:-$default_deploy_source}"
host="${GE_STEAMDECK_HOST:-}"
user="${GE_STEAMDECK_SSH_USER:-deck}"
ssh_key="${GE_STEAMDECK_SSH_KEY:-}"
remote_dir="${GE_STEAMDECK_REMOTE_DIR:-/home/deck/devkit-game/GameEnginePlayer_Linux}"
app_name="${GE_STEAMDECK_APP_NAME:-Open Engine Player}"
register_shortcut=1
launch_after=0

usage() {
  cat <<EOF
Usage: $0 --host HOST [options]

Deploy an already-built GameEngine Steam Deck package over SSH.

Options:
  --source DIR          Local package folder (default: GameEngine/Build/SteamDeck/steamdeck-player)
  --host HOST           Steam Deck hostname or IP address
  --user USER           SSH user (default: deck)
  --ssh-key PATH        SSH private key. If omitted, the script tries common keys.
  --remote-dir DIR      Install folder on Deck (default: /home/deck/devkit-game/GameEnginePlayer_Linux)
  --app-name NAME       Steam shortcut name (default: Open Engine Player)
  --no-shortcut         Skip Steam shortcut registration
  --launch              Launch after deploy when devkit utilities are present
  -h, --help            Show this help

Environment mirrors the options:
  GE_STEAMDECK_DEPLOY_SOURCE, GE_STEAMDECK_HOST, GE_STEAMDECK_SSH_USER,
  GE_STEAMDECK_SSH_KEY, GE_STEAMDECK_REMOTE_DIR, GE_STEAMDECK_APP_NAME
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --source)
      source_dir="${2:-}"
      shift 2
      ;;
    --host)
      host="${2:-}"
      shift 2
      ;;
    --user)
      user="${2:-}"
      shift 2
      ;;
    --ssh-key)
      ssh_key="${2:-}"
      shift 2
      ;;
    --remote-dir)
      remote_dir="${2:-}"
      shift 2
      ;;
    --app-name)
      app_name="${2:-}"
      shift 2
      ;;
    --no-shortcut)
      register_shortcut=0
      shift
      ;;
    --launch)
      launch_after=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "ERROR: Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "$host" ]]; then
  echo "ERROR: Steam Deck host is required. Set it in Build Settings or pass --host deck@ip/hostname." >&2
  exit 2
fi

if [[ "$host" == *@* ]]; then
  user="${host%@*}"
  host="${host#*@}"
fi

if [[ -z "$source_dir" || ! -d "$source_dir" ]]; then
  echo "ERROR: Deploy source folder not found: $source_dir" >&2
  exit 1
fi

if [[ ! -f "$source_dir/launch.sh" && ! -f "$source_dir/Player" ]]; then
  echo "ERROR: Deploy source does not look like a Steam Deck Player package: $source_dir" >&2
  exit 1
fi

expand_home() {
  local value="$1"
  if [[ "$value" == "~/"* ]]; then
    printf '%s/%s' "$HOME" "${value#~/}"
  else
    printf '%s' "$value"
  fi
}

if [[ -z "$ssh_key" ]]; then
  candidates=(
    "$HOME/.ssh/gameengine_steamdeck"
    "$HOME/.config/steamos-devkit/devkit_rsa"
    "$HOME/.local/share/steamos-devkit/steamos-devkit/devkit_rsa"
    "$HOME/Library/Application Support/SteamOS Devkit/devkit_rsa"
  )
  for candidate in "${candidates[@]}"; do
    if [[ -f "$candidate" ]]; then
      ssh_key="$candidate"
      break
    fi
  done
else
  ssh_key="$(expand_home "$ssh_key")"
fi

ssh_args=(-o BatchMode=yes -o ConnectTimeout=10)
if [[ -n "$ssh_key" ]]; then
  if [[ ! -f "$ssh_key" ]]; then
    echo "ERROR: SSH key path does not exist: $ssh_key" >&2
    exit 1
  fi
  ssh_args+=(-i "$ssh_key")
fi

ssh_target="$user@$host"
rsync_remote_shell="ssh"
for arg in "${ssh_args[@]}"; do
  escaped="${arg//\'/\'\\\'\'}"
  rsync_remote_shell+=" '$escaped'"
done

echo "== GameEngine Steam Deck deploy =="
echo "Source:    $source_dir"
echo "Target:    $ssh_target:$remote_dir"
if [[ -n "$ssh_key" ]]; then
  echo "SSH key:   $ssh_key"
else
  echo "SSH key:   default ssh-agent / ~/.ssh config"
fi

if ! command -v rsync >/dev/null 2>&1; then
  echo "ERROR: rsync is required for deployment." >&2
  exit 1
fi

ssh "${ssh_args[@]}" "$ssh_target" "mkdir -p '$remote_dir'"
rsync -az --delete --chmod=Du=rwx,Dgo=rx,Fu=rw,Fgo=r --exclude='.DS_Store' \
  -e "$rsync_remote_shell" \
  "$source_dir"/ "$ssh_target:$remote_dir"/

ssh "${ssh_args[@]}" "$ssh_target" "chmod +x '$remote_dir/Player' '$remote_dir/launch.sh' 2>/dev/null || true; if [[ ! -x '$remote_dir/$app_name' ]]; then printf '%s\\n' '#!/usr/bin/env bash' 'exec \"\$(dirname \"\$0\")/launch.sh\" \"\$@\"' > '$remote_dir/$app_name' && chmod +x '$remote_dir/$app_name'; fi"

# In the desktop entries below, Version= is the Desktop Entry Specification version the file
# follows, not the game's or the engine's version.
install_desktop_cmd=$(cat <<EOF
set -euo pipefail
remote_dir='$remote_dir'
app_name='$app_name'
desktop_src="\$remote_dir/gameengine-player.desktop"
desktop_dst="\$HOME/.local/share/applications/gameengine-player.desktop"
mkdir -p "\$HOME/.local/share/applications"
if [[ -f "\$desktop_src" ]]; then
  sed "s|__INSTALL_DIR__|\$remote_dir|g; s|__APP_NAME__|\$app_name|g" "\$desktop_src" > "\$desktop_dst"
elif [[ -f "\$remote_dir/icon.png" ]]; then
  cat > "\$desktop_dst" <<DESKTOP
[Desktop Entry]
Type=Application
Version=1.0
Name=\$app_name
Comment=Open Engine Player
Exec="\$remote_dir/\$app_name"
Path=\$remote_dir
Icon=\$remote_dir/icon.png
Terminal=false
Categories=Game;
StartupWMClass=Player
DESKTOP
else
  echo 'No icon.png in package; skipping .desktop install.'
  exit 0
fi
chmod 644 "\$desktop_dst"
if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database "\$HOME/.local/share/applications" || true
fi
echo "Installed desktop entry: \$desktop_dst"
EOF
)
ssh "${ssh_args[@]}" "$ssh_target" "$install_desktop_cmd"

if [[ "$register_shortcut" -eq 1 ]]; then
  shortcut_cmd="launcher='$remote_dir/$app_name'; if command -v steam-client-create-shortcut >/dev/null 2>&1; then steam-client-create-shortcut --name '$app_name' --directory '$remote_dir' --command \"\$launcher\" --icon '$remote_dir/icon.png' 2>/dev/null || steam-client-create-shortcut --name '$app_name' --directory '$remote_dir' --command \"\$launcher\" || echo 'Steam shortcut registration failed; continuing.'; elif [[ -x \"\$HOME/devkit-utils/steam-client-create-shortcut\" ]]; then \"\$HOME/devkit-utils/steam-client-create-shortcut\" --name '$app_name' --directory '$remote_dir' --command \"\$launcher\" --icon '$remote_dir/icon.png' 2>/dev/null || \"\$HOME/devkit-utils/steam-client-create-shortcut\" --name '$app_name' --directory '$remote_dir' --command \"\$launcher\" || echo 'Steam shortcut registration failed; continuing.'; else echo 'Steam shortcut tool not found. In Steam: remove the launch.sh entry, then Add Non-Steam Game and pick \"$app_name\" (not launch.sh).'; fi"
  ssh "${ssh_args[@]}" "$ssh_target" "$shortcut_cmd"
fi

if [[ "$launch_after" -eq 1 ]]; then
  launch_cmd="if command -v steam-client-launch >/dev/null 2>&1; then steam-client-launch '$app_name' || nohup '$remote_dir/launch.sh' >/tmp/gameengine-player.log 2>&1 & elif [[ -x \"\$HOME/devkit-utils/steam-client-launch\" ]]; then \"\$HOME/devkit-utils/steam-client-launch\" '$app_name' || nohup '$remote_dir/launch.sh' >/tmp/gameengine-player.log 2>&1 & else nohup '$remote_dir/launch.sh' >/tmp/gameengine-player.log 2>&1 & fi"
  ssh "${ssh_args[@]}" "$ssh_target" "$launch_cmd"
fi

echo "Deploy complete."
