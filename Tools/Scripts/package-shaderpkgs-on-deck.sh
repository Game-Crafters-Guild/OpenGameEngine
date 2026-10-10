#!/usr/bin/env bash
# Run ON the Steam Deck (x86_64) to create .shaderpkg files from staged .spv inputs.
set -euo pipefail

TOOLS_DIR="${1:-/home/deck/games/GameEnginePlayer_Linux/shader-tools}"
SPV_DIR="${2:-/home/deck/games/GameEnginePlayer_Linux/shader-spv}"
OUT_DIR="${3:-/home/deck/games/GameEnginePlayer_Linux/Assets/Shaders}"

export LD_LIBRARY_PATH="${TOOLS_DIR}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
SR="${TOOLS_DIR}/ShaderReflect"

if [[ ! -x "$SR" ]]; then
  echo "ERROR: ShaderReflect not found at $SR" >&2
  exit 1
fi

mkdir -p "$OUT_DIR"

package_graphics() {
  local name="$1" vs="$2" fs="$3"
  "$SR" --vs "${SPV_DIR}/${vs}.spv" --fs "${SPV_DIR}/${fs}.spv" --out "${OUT_DIR}/${name}.shaderpkg"
  echo "  ${name}.shaderpkg"
}

package_compute() {
  local name="$1" cs="$2"
  "$SR" --cs "${SPV_DIR}/${cs}.spv" --out "${OUT_DIR}/${name}.shaderpkg"
  echo "  ${name}.shaderpkg"
}

echo "Packaging shaders into ${OUT_DIR}"
package_graphics copy fullscreen_noinput.vert copy.frag
package_graphics tonemap fullscreen_noinput.vert tonemap.frag
package_graphics forwardplus_lighting fullscreen_noinput.vert forwardplus_lighting.frag
package_graphics world_debug_forwardplus world_debug_forwardplus.vert world_debug_forwardplus.frag
package_graphics world_debug world_debug.vert world_debug.frag
package_graphics sky_render fullscreen_noinput.vert sky_render.frag
package_compute frustum_culling frustum_culling.comp
package_compute clustered_light_cull clustered_light_cull.comp
package_compute cluster_depth_minmax cluster_depth_minmax.comp
package_compute visibility_union visibility_union.comp
package_graphics bloom_threshold fullscreen_noinput.vert bloom_threshold.frag
package_graphics bloom_combine fullscreen_noinput.vert bloom_combine.frag
package_graphics hdr_color_fx fullscreen_noinput.vert hdr_color_fx.frag
package_graphics post_fx_ldr_stack fullscreen_noinput.vert post_fx_ldr_stack.frag
package_graphics vhs fullscreen_noinput.vert vhs.frag
package_graphics chromatic_aberration fullscreen_noinput.vert chromatic_aberration.frag
package_graphics film_simulation_grain fullscreen_noinput.vert film_simulation_grain.frag
package_graphics film_simulation_artifacts fullscreen_noinput.vert film_simulation_artifacts.frag
package_compute ffx_dof_prepare ffx_dof_prepare.comp
package_compute ffx_dof_tile ffx_dof_tile.comp
package_compute ffx_dof_dilate ffx_dof_dilate.comp
package_compute ffx_dof_blur ffx_dof_blur.comp
package_compute ffx_dof_composite ffx_dof_composite.comp
package_compute depth_reduce depth_reduce.comp
package_compute depth_reduce_ms depth_reduce_ms.comp
package_graphics msm_write fullscreen_noinput.vert msm_write.frag
echo "Done. $(ls -1 "${OUT_DIR}"/*.shaderpkg 2>/dev/null | wc -l | tr -d ' ') shaderpkg files."
