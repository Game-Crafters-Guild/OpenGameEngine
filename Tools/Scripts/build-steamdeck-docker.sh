#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=steamdeck-build-paths.sh
source "$script_dir/steamdeck-build-paths.sh"

repo_root="$(cd "$script_dir/../.." && pwd)"
cd "$repo_root"

project_root="${GE_STEAMDECK_PROJECT_ROOT:-$repo_root}"
project_root="$(cd "$project_root" && pwd)"
image_tag="${IMAGE_TAG:-gameengine-steamdeck-runtime4}"
# The release SDK targets Deck x86_64, including on Apple Silicon via emulation.
docker_platform="${DOCKER_PLATFORM:-linux/amd64}"
linux_cross_compile_x64=0
if [[ "$docker_platform" == "linux/arm64" ]]; then
  linux_cross_compile_x64=1
fi
preset="${GE_PRESET:-steamdeck-runtime4}"
build_preset="${GE_BUILD_PRESET:-player-steamdeck-runtime4}"
jobs="${JOBS:-}"
host_cpus=""
build_cpus=""
if [[ -n "${VCPKG_MAX_CONCURRENCY:-}" ]]; then
  vcpkg_max_concurrency="$VCPKG_MAX_CONCURRENCY"
elif [[ "$docker_platform" == "linux/arm64" ]]; then
  # Native arm64 container (default on Apple Silicon): parallel vcpkg is safe and much faster.
  host_cpus="$(sysctl -n hw.ncpu 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
  build_cpus="$host_cpus"
  colima_cpus=""
  if command -v colima >/dev/null 2>&1; then
    colima_cpus="$(colima list 2>/dev/null | awk '$1=="default" && $2=="Running" { print $4; exit }')"
  fi
  if [[ "$colima_cpus" =~ ^[0-9]+$ && "$colima_cpus" -lt "$build_cpus" ]]; then
    build_cpus="$colima_cpus"
  fi
  vcpkg_max_concurrency="$build_cpus"
  if [[ "$vcpkg_max_concurrency" -lt 2 ]]; then
    vcpkg_max_concurrency=2
  fi
else
  # linux/amd64 under Rosetta/QEMU: keep serial installs for stability.
  vcpkg_max_concurrency=1
fi
if [[ -n "$jobs" ]]; then
  jobs_expr="$jobs"
elif [[ -n "$build_cpus" ]]; then
  jobs_expr="$build_cpus"
else
  jobs_expr='$(nproc)'
fi
host_dist_dir="${GE_STEAMDECK_HOST_DIST_DIR:-}"
if [[ -z "$host_dist_dir" ]]; then
  host_dist_dir="${GE_STEAMDECK_DIST_DIR:-$(ge_steamdeck_default_host_dist_dir)}"
fi
container_dist_dir="${GE_STEAMDECK_CONTAINER_DIST_DIR:-}"
skip_image_build=0
check_only=0

usage() {
  cat <<EOF
Usage: $0 [--unity] [--skip-image-build] [--check] [-j JOBS]

Build the Steam Deck Player from macOS/Linux using Docker.

Environment overrides:
  IMAGE_TAG          Docker image tag (default: gameengine-steamdeck-runtime4)
  DOCKER_PLATFORM   Docker platform (default: linux/amd64; matches the release SDK)
  GE_PRESET         CMake configure preset (default: steamdeck-runtime4)
  GE_BUILD_PRESET   CMake build preset (default: player-steamdeck-runtime4)
  GE_STEAMDECK_DIST_DIR
                    Deprecated alias for GE_STEAMDECK_HOST_DIST_DIR
  GE_STEAMDECK_HOST_DIST_DIR
                    Host output directory (default: outside repo; see steamdeck-build-paths.sh)
  GE_STEAMDECK_CONTAINER_DIST_DIR
                    Container output directory when host output is mounted
  GE_STEAMDECK_PROJECT_ROOT
                    Host project root containing Assets/ (default: repo root)
  GE_STEAMDECK_ASSET_ROOT
                    Container asset root to package (default: /src/Assets, or /project/Assets when GE_STEAMDECK_PROJECT_ROOT is outside the repo)
  GE_STEAMDECK_STARTUP_SCENE
                    Asset-relative startup scene, e.g. Scenes/Main.scene
  GE_STEAMDECK_RENDER_PIPELINE
                    Asset-relative render graph path
  GE_STEAMDECK_GAME_NAME
                    Name written to game.config
  GE_STEAMDECK_LOG_FILE
                    Build log path (default: <dist>/build-steamdeck.log)
  VCPKG_MAX_CONCURRENCY
                    vcpkg dependency build jobs (default: host CPU count on linux/arm64,
                    1 on linux/amd64 emulation)
  JOBS              Parallel Ninja jobs (default: host CPU count on linux/arm64, else nproc)
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --unity)
      preset="steamdeck-runtime4-unity"
      build_preset="player-steamdeck-runtime4-unity"
      shift
      ;;
    --skip-image-build)
      skip_image_build=1
      shift
      ;;
    --check)
      check_only=1
      shift
      ;;
    -j|--jobs)
      jobs="${2:-}"
      if [[ -z "$jobs" ]]; then
        echo "ERROR: $1 requires a value." >&2
        exit 2
      fi
      shift 2
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

host_dist_abs="$host_dist_dir"
if [[ "$host_dist_abs" != /* ]]; then
  host_dist_abs="$repo_root/$host_dist_abs"
fi
mkdir -p "$host_dist_abs"

output_mount_args=()
if [[ "$host_dist_abs" != "$repo_root" && "$host_dist_abs" != "$repo_root/"* ]]; then
  if [[ -z "$container_dist_dir" ]]; then
    container_dist_dir="/output"
  fi
  output_mount_args=(-v "$host_dist_abs:$container_dist_dir")
elif [[ -z "$container_dist_dir" ]]; then
  container_dist_dir="${host_dist_abs#"$repo_root"/}"
fi

log_file="${GE_STEAMDECK_LOG_FILE:-$host_dist_abs/build-steamdeck.log}"
mkdir -p "$(dirname "$log_file")"
exec > >(tee "$log_file") 2>&1

echo "== GameEngine Steam Deck Docker build =="
echo "Repo:      $repo_root"
echo "Project:   $project_root"
echo "Platform:  $docker_platform"
if [[ "$linux_cross_compile_x64" -eq 1 ]]; then
  echo "Target:    x86_64 Linux via native arm64 container cross-compiler"
fi
echo "Preset:    $preset"
echo "Build:     $build_preset"
echo "vcpkg:     max concurrency $vcpkg_max_concurrency"
if [[ -n "$host_cpus" ]]; then
  if [[ -n "$build_cpus" && "$build_cpus" != "$host_cpus" ]]; then
    echo "Jobs:      $jobs_expr (host $host_cpus CPUs, Colima VM $build_cpus)"
  else
    echo "Jobs:      $jobs_expr (CPUs: $host_cpus)"
  fi
else
  echo "Jobs:      $jobs_expr"
fi
echo "Output:    $host_dist_abs"
echo "Log:       $log_file"

if ! command -v docker >/dev/null 2>&1; then
  echo "ERROR: Docker is not installed or not on PATH. Install Docker Desktop or 'brew install docker colima', then rerun this script." >&2
  exit 1
fi

if ! docker info >/dev/null 2>&1; then
  if command -v colima >/dev/null 2>&1; then
    echo "Docker daemon is not running; starting Colima..."
    # Defaults for ~10-core / 32GB unified-memory Macs: 16GiB VM leaves headroom for
    # macOS, Editor, and GPU. Override: COLIMA_START_ARGS='--cpu 8 --memory 12 ...'
    # shellcheck disable=SC2086
    colima start ${COLIMA_START_ARGS:---cpu 10 --memory 16 --disk 100 --vm-type vz --vz-rosetta}
  else
    echo "ERROR: Docker daemon is not running. Start Docker Desktop, or install/start Colima." >&2
    exit 1
  fi
fi

echo "Docker:    $(docker --version)"

if [[ "$check_only" -eq 1 ]]; then
  echo "Preflight OK."
  exit 0
fi

# A preflight or failed compile must never erase the last deployable game.
# Serialize access to the shared CMake tree and use a private package candidate.
lock_dir="$repo_root/build/.steamdeck-build.lock"
mkdir -p "$repo_root/build"
if ! mkdir "$lock_dir" 2>/dev/null; then
  echo "ERROR: Another Deck build owns $lock_dir. If a build was interrupted, remove this lock after checking it has stopped." >&2
  exit 1
fi
candidate_root="$(mktemp -d "$host_dist_abs/.building.XXXXXX")"
trap 'rm -rf "$candidate_root"; rmdir "$lock_dir"' EXIT
source_identity="$(python3 "$script_dir/deck_template.py" fingerprint --repo "$repo_root")"

build_docker_image() {
  local context_dir
  context_dir="$(mktemp -d "${TMPDIR:-/tmp}/gameengine-linux-dev-context.XXXXXX")"
  cp Tools/Docker/Dockerfile.steamdeck-dev "$context_dir/Dockerfile.steamdeck-dev"
  if docker build --platform "$docker_platform" -f "$context_dir/Dockerfile.steamdeck-dev" -t "$image_tag" "$context_dir"; then
    rm -rf "$context_dir"
  else
    local status=$?
    rm -rf "$context_dir"
    return "$status"
  fi
}

if [[ "$skip_image_build" -eq 0 ]]; then
  echo "[1/3] Building Docker image '$image_tag' for $docker_platform"
  build_docker_image
else
  if docker image inspect "$image_tag" >/dev/null 2>&1; then
    echo "[1/3] Reusing Docker image '$image_tag'"
  else
    echo "[1/3] Docker image '$image_tag' was not found; building it now"
    build_docker_image
  fi
fi

container_asset_root="${GE_STEAMDECK_ASSET_ROOT:-/src/Assets}"
project_mount_args=()
if [[ "$project_root" != "$repo_root" ]]; then
  project_mount_args=(-v "$project_root:/project:ro")
  if [[ -z "${GE_STEAMDECK_ASSET_ROOT:-}" ]]; then
    container_asset_root="/project/Assets"
  fi
fi
dist_dir="$container_dist_dir/$(basename "$candidate_root")"

container_script=$(cat <<EOF
set -euo pipefail

echo "Container tools:"
cmake --version | head -n 1
ninja --version
c++ --version | head -n 1
export VCPKG_MAX_CONCURRENCY="${vcpkg_max_concurrency}"
echo "vcpkg max concurrency: \$VCPKG_MAX_CONCURRENCY"
if [[ "${linux_cross_compile_x64}" -eq 1 ]]; then
  export GE_LINUX_CROSS_COMPILE_X64=1
  export LD_LIBRARY_PATH="/usr/x86_64-linux-gnu/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
  echo "Target cross compiler: /usr/bin/x86_64-linux-gnu-gcc / /usr/bin/x86_64-linux-gnu-g++"
fi

linux_vcpkg_root="/tmp/gameengine-vcpkg"
copy_vcpkg_failure_logs() {
  local status=\$?
  if [[ "\$status" -ne 0 && -d "\$linux_vcpkg_root/buildtrees" ]]; then
    local failure_log_dir="$container_dist_dir/vcpkg-failure-logs"
    echo "Copying vcpkg failure logs to /src/\$failure_log_dir"
    rm -rf "\$failure_log_dir"
    mkdir -p "\$failure_log_dir"
    find "\$linux_vcpkg_root/buildtrees" -type f \\( -name '*.log' -o -name 'CMakeCache.txt' -o -name 'issue_body.md' \\) | while IFS= read -r log_path; do
      local rel_path="\${log_path#"\$linux_vcpkg_root/buildtrees/"}"
      mkdir -p "\$failure_log_dir/\$(dirname "\$rel_path")"
      cp "\$log_path" "\$failure_log_dir/\$rel_path"
    done
  fi
  return "\$status"
}
trap copy_vcpkg_failure_logs EXIT

echo "Preparing Linux vcpkg checkout at \$linux_vcpkg_root"
rm -rf "\$linux_vcpkg_root"
mkdir -p "\$linux_vcpkg_root" /src/.vcpkg/downloads
tar -C /src/dependencies/vcpkg \
  --exclude='./buildtrees' \
  --exclude='./downloads' \
  --exclude='./installed' \
  --exclude='./packages' \
  --exclude='./vcpkg' \
  -cf - . | tar -C "\$linux_vcpkg_root" -xf -
export VCPKG_DOWNLOADS=/src/.vcpkg/downloads
export VCPKG_DEFAULT_BINARY_CACHE=/vcpkg-cache
if [[ "\${GE_LINUX_CROSS_COMPILE_X64:-0}" == "1" ]]; then
  rm -rf /src/.vcpkg/downloads/tools/cmake-3.31.10-linux
  rm -rf /src/.vcpkg/downloads/tools/ninja-1.13.2-linux
fi
echo "Bootstrapping Linux vcpkg executable"
"\$linux_vcpkg_root/bootstrap-vcpkg.sh" -disableMetrics

host_sr_exe=""
if [[ "\${GE_LINUX_CROSS_COMPILE_X64:-0}" == "1" ]]; then
  host_sr_dir="build/shaderreflect-host-linux-arm64"
  host_sr_exe="\${host_sr_dir}/bin/RelWithDebInfo/Tools/ShaderReflect"
  host_sr_src="Tools/ShaderReflectHost"
    echo "Updating native host ShaderReflect (Tools/ShaderReflectHost)..."
    cmake -S "\$host_sr_src" -B "\$host_sr_dir" -G Ninja \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
      -DCMAKE_TOOLCHAIN_FILE="\$linux_vcpkg_root/scripts/buildsystems/vcpkg.cmake" \
      -DVCPKG_ROOT="\$linux_vcpkg_root" \
      -DVCPKG_TARGET_TRIPLET=arm64-linux
    cmake --build "\$host_sr_dir" --target ShaderReflect -j "$jobs_expr"
  if [[ ! -x "\$host_sr_exe" ]]; then
    echo "ERROR: Host ShaderReflect was not built at \$host_sr_exe" >&2
    exit 1
  fi
fi

# Keep CMake's dependency graph: configure detects changed inputs and the
# container's vcpkg checkout uses the same path on every invocation.

cmake_configure_args=(
  --preset "$preset"
  -DCMAKE_TOOLCHAIN_FILE="\$linux_vcpkg_root/scripts/buildsystems/vcpkg.cmake"
  -DVCPKG_ROOT="\$linux_vcpkg_root"
  -DGE_SKIP_VCPKG_VERIFY=ON
)
if [[ "\${GE_LINUX_CROSS_COMPILE_X64:-0}" == "1" ]]; then
  cmake_configure_args+=(
    -DCMAKE_SYSTEM_NAME=Linux
    -DCMAKE_C_COMPILER=/usr/bin/x86_64-linux-gnu-gcc
    -DCMAKE_CXX_COMPILER=/usr/bin/x86_64-linux-gnu-g++
    -DCMAKE_SYSTEM_PROCESSOR=x86_64
  )
  if [[ -n "\$host_sr_exe" ]]; then
    cmake_configure_args+=(-DGE_SHADERREFLECT_EXECUTABLE="\$host_sr_exe")
  fi
fi
cmake "\${cmake_configure_args[@]}"
cmake --build --preset "$build_preset" -j "$jobs_expr"
rm -rf "$dist_dir/steamdeck-player"
mkdir -p "$dist_dir/steamdeck-player"
cp -a build/$preset/bin/RelWithDebInfo/Apps/Player/. "$dist_dir/steamdeck-player/"
if [[ "\${GE_LINUX_CROSS_COMPILE_X64:-0}" == "1" ]] && compgen -G "/usr/x86_64-linux-gnu/lib/libgomp.so.1*" >/dev/null; then
  cp -a /usr/x86_64-linux-gnu/lib/libgomp.so.1* "$dist_dir/steamdeck-player/"
fi
rm -f "$dist_dir/steamdeck-player/ld-linux-x86-64.so.2"
rm -f "$dist_dir/steamdeck-player/libanl.so."* \
      "$dist_dir/steamdeck-player/libc.so."* \
      "$dist_dir/steamdeck-player/libdl.so."* \
      "$dist_dir/steamdeck-player/libm.so."* \
      "$dist_dir/steamdeck-player/libpthread.so."* \
      "$dist_dir/steamdeck-player/libresolv.so."* \
      "$dist_dir/steamdeck-player/librt.so."* \
      "$dist_dir/steamdeck-player/libutil.so."*

asset_root="\${GE_STEAMDECK_ASSET_ROOT:-/src/Assets}"
if [[ -z "\$asset_root" ]]; then
  asset_root="/src/Assets"
fi
engine_asset_root="/src/Assets"
game_name="\${GE_STEAMDECK_GAME_NAME:-Open Engine Player}"
startup_scene="\${GE_STEAMDECK_STARTUP_SCENE:-}"
render_pipeline="\${GE_STEAMDECK_RENDER_PIPELINE:-RenderPipelines/ForwardPlus.rendergraph}"

find_first_asset() {
  local root="\$1"
  local pattern="\$2"
  if [[ ! -d "\$root" ]]; then
    return 1
  fi
  find "\$root" -type f -name "\$pattern" | sort | head -n 1
}

if [[ -z "\$startup_scene" ]]; then
  first_scene="\$(find_first_asset "\$asset_root" '*.scene' || true)"
  if [[ -n "\$first_scene" ]]; then
    startup_scene="\${first_scene#"\$asset_root/"}"
  fi
fi

if [[ -z "\$render_pipeline" || ! -f "\$asset_root/\$render_pipeline" ]]; then
  if [[ -f "\$asset_root/RenderPipelines/ForwardPlus.rendergraph" ]]; then
    render_pipeline="RenderPipelines/ForwardPlus.rendergraph"
  elif [[ -f "\$engine_asset_root/RenderPipelines/ForwardPlus.rendergraph" ]]; then
    render_pipeline="RenderPipelines/ForwardPlus.rendergraph"
  fi
fi

if [[ -d "\$asset_root" ]]; then
  echo "Copying game assets from \$asset_root"
  mkdir -p "$dist_dir/steamdeck-player/Assets"
  cp -R --no-preserve=mode,ownership,timestamps,xattr "\$asset_root"/. "$dist_dir/steamdeck-player/Assets/"
else
  echo "WARNING: Asset root not found: \$asset_root"
fi

# Ship the authoritative GUID identity as a flat, read-only .assetmanifest at the
# asset root — the Player mounts it via a read-only package mount (no scan, no
# SQLite). The editor's AssetDatabase.assetdb (sibling of Assets/, same JSONL
# format) is the source; copy it in as Assets/.assetmanifest so the Player's
# package mount finds it. (Matches BuildPipeline::StageAssetManifest on Win/macOS.)
project_workspace="\${asset_root%/Assets}"
if [[ "\$project_workspace" == "\$asset_root" ]]; then
  project_workspace="/src"
fi
project_asset_db="\$project_workspace/AssetDatabase.assetdb"
if [[ -f "\$project_asset_db" ]]; then
  echo "Shipping asset identity from \$project_asset_db -> Assets/.assetmanifest"
  cp --no-preserve=mode,ownership,timestamps,xattr "\$project_asset_db" "$dist_dir/steamdeck-player/Assets/.assetmanifest"
else
  echo "WARNING: No AssetDatabase.assetdb at \$project_asset_db — imported-asset GUID references may not resolve"
fi

if [[ -n "\$render_pipeline" && ! -f "$dist_dir/steamdeck-player/Assets/\$render_pipeline" && -f "\$engine_asset_root/\$render_pipeline" ]]; then
  echo "Copying default render pipeline \$render_pipeline from \$engine_asset_root"
  mkdir -p "$dist_dir/steamdeck-player/Assets/\$(dirname "\$render_pipeline")"
  cp --no-preserve=mode,ownership,timestamps,xattr "\$engine_asset_root/\$render_pipeline" "$dist_dir/steamdeck-player/Assets/\$render_pipeline"
fi
if ! find "$dist_dir/steamdeck-player/Assets/RenderPipelines" -type f -name '*.rendergraph' -print -quit 2>/dev/null | grep -q .; then
  if [[ -d "\$engine_asset_root/RenderPipelines" ]]; then
    echo "Copying default render pipelines from \$engine_asset_root"
    mkdir -p "$dist_dir/steamdeck-player/Assets/RenderPipelines"
    cp -R --no-preserve=mode,ownership,timestamps,xattr "\$engine_asset_root/RenderPipelines"/. "$dist_dir/steamdeck-player/Assets/RenderPipelines/"
  fi
fi
shader_pkg_src="build/$preset/Shaders"
if [[ -d "\$shader_pkg_src" ]] && compgen -G "\$shader_pkg_src/*.shaderpkg" >/dev/null; then
  if ! find "$dist_dir/steamdeck-player/Assets/Shaders" -maxdepth 1 -type f -name '*.shaderpkg' -print -quit 2>/dev/null | grep -q .; then
    echo "Copying compiled shader packages from \$shader_pkg_src"
    mkdir -p "$dist_dir/steamdeck-player/Assets/Shaders"
    cp --no-preserve=mode,ownership,timestamps,xattr "\$shader_pkg_src"/*.shaderpkg "$dist_dir/steamdeck-player/Assets/Shaders/"
  fi
fi

if [[ -z "\$startup_scene" ]]; then
  echo "WARNING: No startup scene was provided or discovered; the Deck player will open to an empty window."
fi
if [[ -z "\$render_pipeline" ]]; then
  echo "WARNING: No render pipeline was provided or discovered."
fi

export GE_GAME_CONFIG_OUT="$dist_dir/steamdeck-player/game.config"
export GE_GAME_CONFIG_GAME_NAME="\$game_name"
export GE_GAME_CONFIG_STARTUP_SCENE="\$startup_scene"
export GE_GAME_CONFIG_RENDER_PIPELINE="\$render_pipeline"
python3 - <<'PY'
import json
import os
from pathlib import Path

cfg = {
    "gameName": os.environ.get("GE_GAME_CONFIG_GAME_NAME", "Open Engine Player"),
    "startupScene": os.environ.get("GE_GAME_CONFIG_STARTUP_SCENE", ""),
    "renderPipeline": os.environ.get("GE_GAME_CONFIG_RENDER_PIPELINE", ""),
    "scriptAssemblyPath": "",
    "window": {
        "width": 1280,
        "height": 800,
        "mode": "windowed",
        "vsync": True,
    },
    "hdr": {
        "enabled": False,
        "mode": "hdr10_pq",
        "targetDisplay": -1,
        "metadata": {
            "maxMasteringLuminance": 0.0,
            "minMasteringLuminance": 0.0,
            "maxContentLightLevel": 0.0,
            "maxFrameAverageLightLevel": 0.0,
            "paperWhiteNits": 0.0,
        },
    },
}

out = Path(os.environ["GE_GAME_CONFIG_OUT"])
out.write_text(json.dumps(cfg, indent=4) + "\n", encoding="utf-8")
PY
echo "Wrote game.config (scene='\$startup_scene', pipeline='\$render_pipeline')"

cp /src/Tools/Scripts/steamdeck-launch.sh "$dist_dir/steamdeck-player/launch.sh"
chmod +x "$dist_dir/steamdeck-player/Player" "$dist_dir/steamdeck-player/launch.sh"

# Use a stable filename; a game named "Player" must not overwrite the binary.
steam_launcher="$dist_dir/steamdeck-player/launch-game.sh"
cat > "\$steam_launcher" <<'EOS'
#!/usr/bin/env bash
exec "\$(dirname "\$0")/launch.sh" "\$@"
EOS
chmod +x "\$steam_launcher"
echo "Packaged Steam launcher: \$game_name"

player_icon_src="/src/Apps/Editor/Icon/AppIcon.png"
if [[ -f "\$player_icon_src" ]]; then
  cp --no-preserve=mode,ownership,timestamps,xattr "\$player_icon_src" "$dist_dir/steamdeck-player/icon.png"
  echo "Packaged Steam library icon: icon.png"
else
  echo "WARNING: Player icon not found at \$player_icon_src"
fi

# Version= is the Desktop Entry Specification version the file follows, not the game's or the
# engine's version.
cat > "$dist_dir/steamdeck-player/gameengine-player.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Version=1.0
Name=__APP_NAME__
Comment=Open Engine Player
Exec="__INSTALL_DIR__/__APP_NAME__"
Path=__INSTALL_DIR__
Icon=__INSTALL_DIR__/icon.png
Terminal=false
Categories=Game;
StartupWMClass=Player
DESKTOP

tar -czf "$dist_dir/steamdeck-player.tar.gz" -C "$dist_dir" steamdeck-player
echo "Packaged $dist_dir/steamdeck-player.tar.gz"
EOF
)

echo "[2/3] Building Steam Deck Player inside container"
docker_run_args=(
  --rm
  --platform "$docker_platform"
  -e VCPKG_MAX_CONCURRENCY="$vcpkg_max_concurrency"
  -e GE_STEAMDECK_ASSET_ROOT="$container_asset_root"
  -e GE_STEAMDECK_STARTUP_SCENE="${GE_STEAMDECK_STARTUP_SCENE:-}"
  -e GE_STEAMDECK_RENDER_PIPELINE="${GE_STEAMDECK_RENDER_PIPELINE:-}"
  -e GE_STEAMDECK_GAME_NAME="${GE_STEAMDECK_GAME_NAME:-}"
  -v "gameengine-steamrt4-vcpkg-${docker_platform//\//-}:/vcpkg-cache"
  -v "$repo_root:/src"
  -w /src
)
if ((${#project_mount_args[@]} > 0)); then
  docker_run_args+=("${project_mount_args[@]}")
fi
if ((${#output_mount_args[@]} > 0)); then
  docker_run_args+=("${output_mount_args[@]}")
fi
docker run "${docker_run_args[@]}" "$image_tag" bash -lc "$container_script"
python3 "$script_dir/deck_template.py" seal --repo "$repo_root" \
  --package "$candidate_root/steamdeck-player" --identity "$source_identity"
# Include the identity in the distribution archive too.
tar -czf "$candidate_root/steamdeck-player.tar.gz" -C "$candidate_root" steamdeck-player

# Promote only the completed candidate. Roll back both outputs if promotion
# fails; the old release remains available throughout compilation and staging.
python3 "$script_dir/build_deck.py" --promote "$candidate_root" --output "$host_dist_abs"

echo "[3/3] Done"
echo "Output folder: $host_dist_abs/steamdeck-player"
echo "Archive:       $host_dist_abs/steamdeck-player.tar.gz"
