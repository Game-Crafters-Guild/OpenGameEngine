# OpenColorIO (generated ACES 2 output transform)

- Source: https://github.com/AcademySoftwareFoundation/OpenColorIO
- Generator version: OpenColorIO 2.5.2 (pinned in
  `Tools/ShaderGen/requirements.txt`; asserted at generation time)
- License: BSD-3-Clause, Copyright Contributors to the OpenColorIO Project
  (see `LICENSE.txt`, fetched from the upstream repository 2026-08-04)
- Generated content: `Engine/Modules/Rendering/Shaders/Includes/tonemap_aces2.glsl`
  and `tonemap_aces2_tables.glsl` carry the GPU shader text emitted by
  OpenColorIO's official ACES 2 fixed-function implementation for the ACES 2.0
  Rec.709 Output Transform at six peak luminances (100·2^k nits, k = 0..5) —
  the sampled tables (`aces2_reach_m_tables`, `aces2_gamut_cusp_tables`, hue
  arrays) and the helper functions (`aces2_tonescale_fwd0`,
  `aces2_gamut_compress0`, table samplers, …), tier-parameterized for runtime
  blending by `Tools/ShaderGen/gen_aces2_tables.py`. The emitted function
  bodies originate in OpenColorIO's source templates, so this is redistributed
  OpenColorIO-derived source, not mere tool output: the BSD-3-Clause notice,
  conditions, and disclaimer travel with it via this directory.
- No OpenColorIO library code is vendored, linked, or loaded at runtime;
  generation was offline and the shader has no LUT asset or descriptor
  dependency.
