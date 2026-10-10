# Khronos PBR Neutral tone mapper

- Source: https://github.com/KhronosGroup/ToneMapping
- Upstream revision: `b5a2eed5ddf6c2227090449399de9c7affb9e4c9` (repository `main`,
  2024-06-17; fetched 2026-08-04)
- License: Apache-2.0, Copyright 2024 The Khronos Group, Inc. (see `LICENSE.txt`,
  copied from upstream `LICENSES/Apache-2.0.txt`). Upstream's `.reuse/dep5` names
  the borrowed file explicitly:
  `Files: … PBR_Neutral/pbrNeutral.glsl / Copyright: 2024 The Khronos Group, Inc. / License: Apache-2.0`.
- Adapted components: `TonemapNeutral` in
  `Engine/Modules/Rendering/Shaders/tonemap.frag` is `PBRNeutralToneMapping` from
  `PBR_Neutral/pbrNeutral.glsl`, transcribed whole — same constants
  (`startCompression = 0.8 - 0.04`, `desaturation = 0.15`, the `x < 0.08` toe
  offset), same compression and desaturation math. The single textual deviation:
  the early return is `max(v, vec3(0.0))` where upstream returns `color`
  unchanged; upstream documents its input as non-negative and the call site
  clamps, so the two are value-identical over the operator's input contract.
- The upstream `config.ocio`, `.cube` LUT, and LUT writer are not vendored.
- Upstream ships no `NOTICE` file, so Apache-2.0 §4(d) imposes no further
  redistribution text beyond this license copy and the shader header notice.
