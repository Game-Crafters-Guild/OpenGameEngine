# AMD FidelityFX Denoiser (reflection denoiser)

- Source: https://github.com/GPUOpen-Effects/FidelityFX-Denoiser
- Upstream revision: `d7dfecbabe7b9523b14e7b067216e06b86e8d189` ("FidelityFX Denoiser v1.2", 2021-12-07)
- License: MIT, Copyright (c) 2021 Advanced Micro Devices, Inc. (see `LICENSE.txt`)
- Adapted components. In `Engine/Modules/Rendering/Shaders/sssr_prefilter.comp`,
  derived from `ffx-reflection-dnsr/ffx_denoiser_reflections_prefilter.h`
  (`FFX_DNSR_Reflections_Resolve`, `FFX_DNSR_Reflections_InitializeGroupSharedMemory`):
  the groupshared neighbourhood tile loaded cooperatively once and sampled from
  LDS in place of per-tap texture fetches, the block-average radiance reference
  used as a firefly gate, and variance-driven neighbour weighting with a bias
  floor. In `Engine/Modules/Rendering/Shaders/sssr_reproject.comp`, derived from
  the average-radiance reduction at the tail of `FFX_DNSR_Reflections_Reproject`
  in `ffx_denoiser_reflections_reproject.h`: the workgroup LDS reduction that
  produces that block-average reference, one texel per 8x8 block — reweighted by
  trace confidence and stored premultiplied, where upstream weights by
  `exp(-luminance)` and stores the divided mean. The constants
  `RADIANCE_WEIGHT_BIAS` (0.6) and `PREFILTER_VARIANCE_BIAS` (0.1) are upstream
  values.
- Deliberately not adopted, each because it was measured or reasoned to regress
  this engine: upstream's **sparse 15-tap Halton footprint** (a subsampled kernel
  re-samples a static geometric alias rather than low-passing it — measured worse
  than the dense filter it would replace, both with the tap set rotated per pixel
  and with it rotated per frame, so this port keeps the LDS tile and uses a dense
  footprint); its depth edge stop (`exp(-|dz|*z*4)`, whose sigma shrinks as 1/z
  and would close the filter at grazing incidence); its normal exponent of 512
  (which assumes geometrically smooth G-buffer normals rather than normal-mapped
  ones); its temporal-only variance drive; and its absolute-distance radiance
  weight (this pipeline carries physical light units, in which that expression
  saturates to its floor).
- No upstream source files are vendored. The resolve-temporal and shadow
  denoiser headers are not adapted; of the reproject header, only the
  average-radiance reduction named above is.
- `ffx_denoiser_reflections_common.h` carries a second, inline MIT notice —
  Copyright (c) [2015] [Playdead], from "Temporal Reprojection Anti-Aliasing" —
  covering `FFX_DNSR_Reflections_ClipAABB`. That function is not adapted, so that
  notice does not travel with this tree; it must be added alongside this one if
  the RGB AABB history clip is ever ported.
