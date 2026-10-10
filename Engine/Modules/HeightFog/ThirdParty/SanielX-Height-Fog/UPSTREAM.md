# SanielX/Height-Fog Provenance

This module adapts the analytical screen-space height fog math from
`SanielX/Height-Fog` to GameEngine's fullscreen post-FX render graph path.

- Upstream: https://github.com/SanielX/Height-Fog
- Commit: `ab6f8278b58d77a6bc9cea4d3c9dcd3f34291fe7`
- License: MIT (staged from vcpkg package `sanielx-height-fog-upstream`)

Unity-specific Post Processing V2 plumbing and project assets are not vendored.
The local implementation ports the HLSL optical-depth, smooth-distance ramp,
phase function, and sky handling into GLSL using GameEngine's `ViewParams` UBO
and depth texture inputs.
