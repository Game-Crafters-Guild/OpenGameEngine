# BakingLab ACES fit (Stephen Hill)

- Source: https://github.com/TheRealMJP/BakingLab
- Upstream revision: `c3868af50d72afc13cdfe513a1e0c6a4fafdac8c` (repository
  `master`, 2024-05-10; fetched 2026-08-04)
- License: MIT, Copyright (c) 2016 MJP (see `LICENSE.txt`). The fit itself was
  authored by Stephen Hill (@self_shadow), as upstream's `BakingLab/ACES.hlsl`
  header records — but the copyright holder of record for the MIT notice is MJP,
  so the notice, not just the author credit, is what must travel with the code.
- Adapted components: `TonemapACES` in
  `Engine/Modules/Rendering/Shaders/tonemap.frag` is `ACESFitted` +
  `RRTAndODTFit` from `BakingLab/ACES.hlsl`, transcribed
  coefficient-for-coefficient apart from the one deviation below:
  `ACESInputMat` / `ACESOutputMat` (transposed from HLSL row-major to GLSL
  column-major) and the rational-fit constants `0.0245786`, `0.983729`,
  `0.4329510`, `0.238081`, with the same final [0, 1] clamp and no input
  pre-scale.
- Local modifications: upstream's numerator constant **`0.000090537` is
  deliberately dropped** (`num = v * (v + 0.0245786)`, 2026-08-16). It is the
  only term that sets the curve's sign at zero, and with it the fit is negative
  for working-space values below `0.003253` — which the final clamp turns into a
  flat black floor, so the darkest span stops resolving and a channel that enters
  it while its neighbours have not is deleted rather than darkened. Dropping it
  puts the curve through the origin, strictly increasing and linear in the limit.
  Its influence decays as 1/v², so the rest of the curve is nearly untouched:
  mid-grey +0.25% (scene 0.18 still lands at display-linear ~0.106, upstream's
  own anchor), a neutral 0..4 ramp moves at most 2 codes at 8 bits, and saturated
  colours move by ≤3.7e-4 in float. Mechanism and measurements:
  the tone-mapping design record (near-black item); pinned by
  `TonemapHdrHeadroomTests` tests 9–11 with an M8/M9 mutation study.
- Nothing else from BakingLab is vendored.
