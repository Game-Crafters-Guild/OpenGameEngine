# Film Simulation package

Self-contained MIT-licensed film grain and physical-film artifact shaders.
It preserves the existing grain response, colored grain, hair, scratch, dust,
gate weave, and gate mask controls.

Grain defaults to a lightweight mode adapted from AMD FidelityFX Lens 1.1.
The Filmic mode retains discrete overlapping grain particles for more explicit
shape and density art direction. Both modes use bounded display-range
compositing and the package's shadow, midtone, and highlight response controls.

FidelityFX source: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v1.1.3
The adapted FidelityFX code is MIT-licensed; see `LICENSE.txt`.

The render pipeline and extraction path require this package to be mounted.
Disabling it in a project's Package Manager neutralizes grain, artifacts, and
HDR halation after that project is reopened, while retaining scene settings
for a later re-enable.
