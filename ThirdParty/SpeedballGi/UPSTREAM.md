# speedball-gi DDGI library

- Source: https://github.com/cl0nazepamm/speedball
- Revision: `09217fa172ba86f312572a95ba83ac5b2062f817` (release 0.7.0)
- Upstream files: `js/gi_probes.js`, `js/gi_settings.js` and their WGSL kernels
- License: MIT (see `LICENSE.md`)
- Copyright: clone.software

The engine's DDGI probe field (`Engine/Modules/Rendering/Shaders/ddgi_*.comp`,
`Shaders/Includes/ddgi_*.glsl`, `Engine/Source/Engine/Rendering/DDGI*.cpp`) is
a GLSL/C++ port of this library's probe solve: octahedral atlas addressing,
the trace/blend/upload split, relocation and classification, the per-texel
temporal policy, and the dual reflection lobes. The port adapts the WGSL
kernels to Vulkan ray query and to the engine's render graph, adds a
software trace lane, cascades, skinned geometry, and hit-point NEE.

The Sponza verification asset that ships with the same repository is
documented separately in `ThirdParty/SpeedballSponzaTestScene/UPSTREAM.md`;
it is not covered by this MIT license.
