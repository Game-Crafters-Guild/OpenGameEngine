# SebLague/Clouds Provenance

This module ports the raymarched volumetric clouds from Sebastian Lague's
`Clouds` project (the "Coding Adventure: Clouds" video) to GameEngine's
render-graph post-FX path.

- Upstream: https://github.com/SebLague/Clouds
- Commit: `fcc997c40d36c7bedf95a294cd2136b8c5127009`
- License: MIT (see `LICENSE` in this directory)

Ported faithfully: the container-box raymarch (`Clouds.shader` — ray/box
intersection, `sampleDensity` shape+detail erosion, height gradient and edge
falloff, light march with Beer's law, two-lobe Henyey-Greenstein phase,
depth-limited march, scene composite) and the layered inverted Worley noise
bake with per-channel min/max normalization (`NoiseGenCompute.compute`).

Deliberate deviations:

- **Hash-derived Worley points.** Upstream uploads CPU-generated per-cell point
  buffers; this port derives each cell's feature point from a hash of the
  wrapped cell coordinate, which tiles identically and lets the bake run as
  two `runOnce` blueprint compute nodes with no CPU involvement. Octave cell
  counts/persistence are shader constants (upstream ships pre-saved textures
  authored from per-channel `WorleyNoiseSettings` assets).
- **Temporal accumulation** (not in upstream): the march start is jittered with
  interleaved gradient noise and the cloud-only output converges through a
  reprojected exponential history blend (`TemporalFullscreenShader` node).
- **Interleaved gradient noise** replaces upstream's blue-noise texture for the
  ray start offset.
- **Weather map**: not ported — it is commented out in the upstream shader.
- **Sun tracking**: light direction/color resolve from the scene's primary
  directional light (upstream reads Unity's `_WorldSpaceLightPos0`).
- The `container` transform becomes the owning PostProcessVolume entity's world
  transform (translation = center, scale = box size).
