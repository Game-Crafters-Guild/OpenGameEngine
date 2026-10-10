# MirzaBeig/GPU-Fog-Particles Provenance

This module adapts the textureless particle-fog shader technique from
`MirzaBeig/GPU-Fog-Particles` to GameEngine's material surface-shader pipeline.

- Upstream: https://github.com/MirzaBeig/GPU-Fog-Particles
- Commit: `324e6ce9444b997293644d3b049f891365999743`
- License: Unlicense (staged from vcpkg package `mirzabeig-gpu-fog-particles-upstream`)

No Unity project assets are vendored into this module. The local implementation
ports the shader's layered value noise, simplex noise, Voronoi attenuation, tint,
vertex-color modulation, and radial particle mask to GLSL. Unity-specific
soft-particle depth fading is not included because the current material adapter
does not expose scene-depth sampling to surface shaders.
