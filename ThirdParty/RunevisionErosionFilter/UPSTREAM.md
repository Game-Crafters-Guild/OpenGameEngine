# Runevision Erosion Filter Provenance

The terrain noise effect's erosion block implements the "Fast and Gorgeous
Erosion Filter" technique: a point-local pure filter over a height function with
an analytic gradient, producing gradient-aligned stripe gullies that are rotated
per cell, blended across neighbouring cells, and stacked with fading amplitude.

- Technique: "Fast and Gorgeous Erosion Filter", Rune Skovbo Johansen, 2026-03
- Source: https://blog.runevision.com/2026/03/fast-and-gorgeous-erosion-filter.html
- Reference shaders: https://www.shadertoy.com/view/wXcfWn (advanced),
  https://www.shadertoy.com/view/33cXW8 (clean), https://www.shadertoy.com/view/sf23W1
- License: MPL v2 — the author states "I've released my code under the Mozilla
  Public License v2 in order to encourage further sharing of improvements."
  Canonical text in `LICENSE-MPL-2.0.txt` next to this file.
- Lineage the author credits: Clay John (2018), Felix Westin / Fewes (2023),
  with the author's own modifications 2025-2026.

## What is vendored

No upstream source file is vendored. The blog post publishes the technique as
prose plus two helper functions; the runnable listings live on Shadertoy, which
was not reachable from the implementation environment. The engine-side code was
therefore written from the published *description* of the technique rather than
transcribed from the author's source.

MPL v2 is applied to the derived files anyway, as the conservative reading: the
technique and its published expression are the author's, and file-scope copyleft
costs the engine nothing because the code is confined to files that contain
nothing else.

## Where the derived code lives

MPL v2 applies to exactly these two files, which carry the SPDX identifier:

- `Engine/Modules/TerrainECS/Include/TerrainECS/Erosion/ErosionFilter.h`
- `Engine/Modules/TerrainECS/Source/Erosion/ErosionFilter.cpp`

Every modification to the technique stays inside them; callers include the
header freely and are unaffected by the licence. A GLSL twin
(`erosion_filter.glsl`) will join this list when the GPU bake path gains the
kernel — it carries the same header.

## Modifications made here

- **Additive integration.** Upstream the filter *is* the terrain height. Here it
  is an offset added to the engine's existing `FBMNoise2D` result, scaled by
  `ErosionStrength`, so that strength 0 reproduces the pre-erosion bake
  byte-for-byte and the filter can be dropped into an existing authored scene
  without re-tuning the base noise.
- **Analytic gradient from the engine's own basis.** The gradient comes from a
  derivative-carrying evaluation of the engine's existing `GradientNoise2D`
  (smoothstep interpolation, four diagonal gradient directions), not from
  upstream's noise. The value path is arranged to match `GradientNoise2D`
  expression-for-expression so the eroded and non-eroded bakes agree at the base.
- **Edge rounding as a continuous knob.** Upstream's "straight gullies" is a
  discrete choice between `sign(sin)` and `sin`. Here `ErosionEdgeRounding`
  interpolates between them so the crease sharpness is authorable.
- **Named constants.** The cell-jitter, normalisation gain, slope reference,
  and per-octave lacunarity/persistence are named constants rather than authored
  fields, per the design doc's exposure decision.
- **Scale-free masking.** The slope mask and altitude fade target are normalised
  by the base fBM's analytic amplitude envelope so the same erosion parameters
  behave consistently across terrain height scales and world sizes.

Design record: "Terrain World-Editing UX", §5.3 (c), in the support repository.
