# Terrain topology on WebGPU

Build `CBTTerrainBrowserSmoke` in a configured web editor build, serve its `bin`
directory with `Tools/Web/serve.py`, and open `CBTTerrainBrowserSmoke/index.html`
in a WebGPU browser. The page reports PASS or FAIL, also exposed as the document's
`data-result` attribute. Add `?mode=depth` for the depth-19 refinement stress case.

The default camera matches the native screen-space edge-conformity test. Forty
refinement updates followed by forty coarsening updates dispatch the production cooked kernels through split, propagation,
simplification, reduction, and validation. Every update checks reciprocal
neighbor links, allocation budget, freed-slot references, and compact-list
integrity. This reproduces lost concurrent neighbor-component writes that pass
native Vulkan tests, even with the same narrow heap. The target copies the actual
WebEditor shader cook; it contains no test replacement shaders.
