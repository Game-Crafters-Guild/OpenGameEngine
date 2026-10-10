# SE Natural Bloom and Dirty Lens

The engine adapts the lens dirt of Sonic Ether's MIT-licensed SE Natural Bloom
and Dirty Lens effect: the supplied `lensDirt1` texture
(`Assets/Textures/Bloom/lensDirt1.png`) and its composite. Bloom itself (the
three-to-eight-octave HDR pyramid, the normalized octave reconstruction and the
combine) belongs to the engine's core bloom, not to this port.

Lens dirt is part of the Bloom Effect on a Post Process Volume. The `Lens Dirt`
toggle enables it; `Lens Dirt Intensity` sets its strength on an exponential
scale, and `Lens Dirt Spread` biases its illumination toward narrow or broad
bloom octaves. The illumination is gathered from all eight pyramid levels into
a low-resolution field (`bloom_lens_dirt_gather.frag`), independently of the
effect's `Octaves` and `Radius`, so small HDR sources such as the sun can
reveal dirt across the lens without widening the normal bloom. The composite
(`bloom_lens_dirt.frag`) adds that light through the dirt texture in HDR, so it
never darkens bright parts of the scene. An optional `Lens Dirt Vignette` keeps
the center of the lens clean.

The `Lens Dirt Texture` slot in the Bloom Effect inspector lists texture assets
whose name or path contains `dirt`; left empty, it uses the supplied
`lensDirt1` texture.

Upstream: https://github.com/sonicether/SE-Natural-Bloom-Dirty-Lens
Ported from commit `bc15de63773354ca99d2a24abe065814e8ba60cf`.
