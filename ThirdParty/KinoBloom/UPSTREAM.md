# KinoBloom

- Source: https://github.com/keijiro/KinoBloom
- Upstream revision: `0a04c43fa47de12bb52edb325c8d3c0125d0b4ca`
- License: MIT (see `LICENSE.md`)
- Adapted components: the resolution-independent mapping from Radius and
  render height to pyramid depth, and the fractional sample scale that keeps
  Radius continuous between pyramid levels. Bloom uses both
  (`PostProcessSettings::ResolveBloomPyramid`, `bloom_octave_gather.frag`);
  fog glow uses the depth mapping (`PostProcessSettings::ResolveFogGlowPyramid`).
- Nothing else is adapted: the upstream prefilters, downsample and upsample
  filters, RGBM path, additive reconstruction and shaders are not vendored.
  The engine's bloom pyramid, filters and energy-normalized reconstruction
  are its own.
