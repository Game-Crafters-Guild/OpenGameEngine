# FidelityFX Depth of Field package

This package contains GameEngine's compute implementation of the AMD
FidelityFX Depth of Field 1.1 pipeline: half-resolution CoC-aware reduction,
tile classification and dilation, separate near/far ring gathers, foreground
hole filling, and full-resolution composition.

The implementation is adapted to GameEngine's render graph and camera model.
Its custom ring sample transform preserves the engine's polygonal aperture,
aperture roundness, and anamorphic bokeh controls. The focus debug visualizer
is retained in the composite pass.

Algorithm reference: https://gpuopen.com/manuals/fidelityfx_sdk/techniques/depth-of-field/
Upstream source: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v1.1.4

AMD FidelityFX is licensed under MIT; see `LICENSE.txt`.
