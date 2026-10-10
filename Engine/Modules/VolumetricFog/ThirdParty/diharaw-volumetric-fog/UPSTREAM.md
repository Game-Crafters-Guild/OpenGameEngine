# diharaw/volumetric-fog Provenance

This module is based on standard froxel volumetric fog techniques and uses
`diharaw/volumetric-fog` as an MIT-licensed reference implementation.

- Upstream: https://github.com/diharaw/volumetric-fog
- Commit: `4e6418ca40281ebd5d865f56c4e0e837f3b17d48`
- License: MIT (staged from vcpkg package `diharaw-volumetric-fog-upstream`)
- Copyright: Copyright (c) 2021 Dihara Wijetunga

No upstream source files or assets are vendored into this module. The local
implementation is adapted to GameEngine's RenderGraph, resource binding, shadow
data, post-process, and render-pipeline-node APIs.

Reference techniques used:

- Frustum-aligned volumetric grid/froxels
- Stochastic per-froxel jitter
- Directional-light phase scattering with shadow visibility
- Front-to-back Beer-Lambert transmittance integration
- Temporal reprojection into a persistent history volume
