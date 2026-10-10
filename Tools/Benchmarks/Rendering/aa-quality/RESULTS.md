# AA quality results — 2026-09-06

These are historical measurements; commands below describe the scripts at the
time of capture. Automated SSAA correctness is maintained by `SsaaResolveTests`;
the lab's live filter scripts now report metrics without `--assert-quality`.

Baseline: `origin/main` at `3a257efdb`. Worktree: `feat/aa-quality-benchmark`.

Apple M1 Max, Metal, Release, 1531 × 1033 display pixels, fixed EV100 16.9.
The baseline and updated runs use the same generated chart and isolated editor
profile. The baseline includes only the editor-data isolation change, with main's AA shaders. Off is byte-identical. Measurements use 16 static captures, 16 sampled
camera positions, and 20 profiler samples per mode. Updated runs also capture
an occluder reveal. Capture frame indices are retained; these are not consecutive
frame recordings or fixed-speed motion benchmarks.

## Quality

| Measurement | Main | Updated | Improvement |
|---|---:|---:|---:|
| TAA edge standard deviation (display codes) | 14.011 | 1.314 | 90.6% lower |
| TAAU edge standard deviation (display codes) | 18.145 | 1.753 | 90.3% lower |
| MSAA2 out-of-triangle red pixels | 1000 | 0 | Eliminated in chart |
| MSAA4 out-of-triangle red pixels | 2257 | 0 | Eliminated in chart |
| SSAA 125% mean HDR area error | 1.011488 | 0.015692 | 98.4% lower |
| SSAA 150% mean HDR area error | 0.512627 | 0.016089 | 96.9% lower |
| SSAA 200% mean HDR area error | 0.000259 | 0.000208 | 19.6% lower |

SSAA error compares live FP16 pass input/output against independent CPU texel-area
integration. Fractional scales use four bilinear samples; aligned 200% uses one.
Residual error includes FP16 output and hardware interpolation precision. MSAA 8×
was requested and correctly clamped to the device maximum of 4×; it is not an 8× result.

TAA uses unjittered clipping neighborhoods, depth-footprint history retention at rest,
static-background disocclusion rejection, and bounded current reconstruction to avoid
dark ringing trails. TAAU phase correction uses the display grid. The sampled reveal
restores the fence without the persistent dark outline found during development.
Static detail remains visible, but a lower flicker statistic alone does not prove fidelity.

## Cost

Median reported GPU encoder spans, milliseconds (20 samples). Shared spans are not
additive pass costs. These are single-machine observations, not controlled throughput
claims; other GPU work, thermals, and driver scheduling can affect them.

| Pass | Main | Updated |
|---|---:|---:|
| TAA | 0.494 | 0.635 |
| TAAU | 0.486 | 0.636 |
| MSAA2 | 0.065 | 0.064 |
| MSAA4 | 0.116 | 0.116 |
| SSAA125 | 0.065 | 0.101 |
| SSAA150 | 0.074 | 0.103 |
| SSAA200 | 0.067 | 0.064 |

The stronger temporal resolve costs approximately 0.14–0.15 ms more in this scene.
Fractional SSAA filtering costs about 0.03 ms more; the 200% fast path and MSAA
shading cost are roughly unchanged. SSAA still pays for its larger internal image.

## Verification and limits

- Release Editor and shader package builds succeeded; no shader compilation errors in the final editor log.
- All 12 selected TAA, MSAA, and render-scale pipeline tests passed.
- The comparison gates passed: unchanged Off control, MSAA border diagnostics, at least 80% lower TAA/TAAU static edge variation, and SSAA area accuracy.
- All nine final AA configurations report zero render-graph errors and warnings.
- Python syntax checks and `git diff --check` passed.

This is a tested improvement, not evidence of industry-leading quality. Production
qualification still needs frame-locked animation, camera cuts, alpha-tested foliage,
transparency/reactive shading, changing exposure/lighting, multiple frame rates,
and other GPU backends. The deliberately unfiltered checker remains a shading
aliasing stress case; MSAA does not supersample interior shading.

## Local artifacts

- Baseline: `Artifacts/AAQuality/before-isolated/`
- Updated matrix: `Artifacts/AAQuality/final2/`
- `comparison.png`: static matrix; `taa-stability.gif`: slowed sampled comparison.
- Per-mode manifests retain applied AA settings, resource extents, camera, frame indices, and timings.
- `area.json`: HDR integration errors; `validation.json`: final render-graph checks.
- Generated scene: `Artifacts/AAQuality/Project/Assets/Scenes/AAQuality.scene`.

Artifacts are ignored by git. Recreate the project and captures using the adjacent README.
The final TAA/TAAU captures were refreshed after temporal-only shader changes;
the other modes retain their validated captures from the same final MSAA/SSAA code.

## Sponza cutout correction

A subsequent Sponza check exposed a regression in the initial centroid change:
alpha-tested foliage showed bright sky-colored specks with MSAA. Disabling sky
IBL did not remove them; disabling the backdrop or MSAA did. Restoring main's
interpolation removed the specks, isolating the regression to the adapter change.

The corrected adapters use pixel-center interpolation for every surface varying
when `ALPHA_TEST` is active, keeping opacity evaluation consistent between the
depth and color variants. Opaque materials retain centroid interpolation. This
also covers procedural opacity that depends on position or vertex color rather
than only UV0. No depth bias, lighting adjustment, or MSAA sample reduction is used.

The same live Sponza camera at 1531 × 1033 with MSAA 4× and sky enabled was captured
before and after shader reload. The shadowed foliage crop x=190..379, y=730..969
contained 35 pixels above display code 100 before the fix and zero afterward.
The source scene's unsaved edits were preserved; AA and sky settings were restored
after the live check. Render-graph validation reported no errors or warnings.

Local evidence is under `Artifacts/AAQuality/SponzaMSAA/`. The original failure
capture lived on the capturing machine and is not in the repo. This is a
scene-specific regression check. An exploratory procedural/textured cutout chart
did not reproduce the original failure and is not counted as a regression gate for it.

The opaque chart was rechecked at MSAA 2× and 4×: both still report zero red
out-of-triangle diagnostic pixels and zero render-graph validation errors.

## Wider SSAA reconstruction

The final SSAA filter is a separable, positive tent with a radius of one output
pixel, evaluated using four bilinear fetches for the supported 1–2× scale range.
It replaces the box/area filter measured above; those area-error numbers remain
historical and are not the accuracy criterion for the final kernel. Upscaling
below native resolution retains Catmull–Rom.

A separate Release editor reproduced the upward Sponza camera at 1531×1033,
using the same frozen scene, profile and 2× internal scale before and after.
Inspection of full images and enlarged cloth crops shows reduced fine-pattern
contrast and smoother edges, with modest softening. Eight sampled camera poses
were captured per version; these do not establish frame-locked temporal stability.
The existing bright architectural shading remains visible in both versions.
The new resolve's median reported Metal encoder span was 0.1213 ms (20 samples).
The before capture did not have profiling enabled, so no matched timing delta is
claimed. The Sponza render graph reported no errors or warnings.

`verify_tent.py --port 10020 --output <path>/tent.json --assert-quality` passed
on the stationary AA chart, comparing GPU FP16 output against independently
weighted CPU source samples (clamped at texture borders):

| Linear scale | Normalized mean error | Normalized p99 error |
| --- | ---: | ---: |
| 1.25× | 2.85e-7 | 1.08e-5 |
| 1.5× | 2.32e-7 | 7.63e-6 |
| 2× | 7.93e-8 | 2.86e-6 |

Normalization uses the source HDR peak. These are kernel correctness checks,
not a perceptual metric. Release shader compilation and packaging passed.
Captures, sampled motion GIF and timing data are under the ignored
`Artifacts/AAQuality/FilterStudy` directory.

## PR #1463 candidate evaluation

Compared three isolated ideas from `fix/taa-rest-stability` against #1466 at
`c93a8410a` in the private Release AA chart editor. Each build used the same
profile, 1531×1033 viewport, native TAA and 67% TAAU, 16 static samples,
16 camera-pan poses, and an occluder reveal with eight sampled captures.

- Depth candidate: weight neighborhood moments by
  `1 - smoothstep(0.04, 0.20, relativeNeighborDepthDifference)` and normalize by
  total weight. Retain our reconstruction thresholds and history handling.
- Threshold candidate: use 0.04/0.20 instead of 0.002/0.02 for the reconstruction
  silhouette transition. Retain our neighborhood and history handling.
- Clipped candidate: always use variance-clipped history color instead of our
  stationary-depth-footprint exception. Retain our other rejection logic.

| Variant | Native TAA static edge deviation | TAAU static edge deviation |
| --- | ---: | ---: |
| Current #1466 | 1.264 | 1.599 |
| Depth-weighted moments | 1.312 | 1.705 |
| Looser silhouette thresholds | 1.370 | 1.772 |
| Always-clipped history | 6.531 | 4.962 |

Values are standard deviation in display code values; lower means less static
variation, not necessarily better spatial fidelity. The small differences in
the first three rows are not statistically qualified: jitter phases and capture
intervals are sampled. Inspection of pan/reveal crops did not establish a clear
benefit from depth weighting or looser thresholds. Always-clipped history caused
a large stability regression (5.2× native / 3.1× TAAU).

Decision: adopt none of these isolated changes. Restore the original shader
sources and rebuild their packages. All candidate builds and all eight
render-graph validation checks passed. The experiment did not test the complete
#1463 implementation as a unit, its additional current-color AABB clamp, or
changing-light response; it does not establish that every possible combination
is inferior. Frame-locked motion and broader scene qualification remain open.

The experiment runner, exact baseline sources, manifests, images and sampled
reveal GIFs are retained locally under ignored `Artifacts/AAQuality/Taa1463`.

## Sun-disc temporal history bounds (2026-09-09)

An additional live Debug/Metal check used the balanced-lighting editor's unsaved sky scene at 1531 × 1033. Native TAA produced a dark dotted ring inside the sun while AA-off did not. Current-sample undershoot protection alone does not bound reprojected history. The initial fix clamped history RGB to the current source neighborhood minimum and maximum before Karis blending. That removed the visible ring, but the unconditional clamp also discarded valid subpixel history; the follow-up below replaces it for validated stationary history.

The PR's updated shaders compiled and were loaded into that editor without closing the unsaved scene. Native TAA and 75% TAAU captures showed no dark sun ring; toggling balanced lighting off and back on returned to a clean sky without the white/blue speckled history artifact. The original render scale and balanced-lighting state were restored. These are sampled live checks, not a consecutive-frame benchmark or a rerun of the full AA quality matrix.


## Stationary TAA history regression fixes (2026-09-09)

Native TAA and TAAU require stored history depth to match an actual sample in
the current 3x3 footprint, within FP16 tolerance. Being between the nearest and
farthest surface is insufficient. Valid stationary history bypasses the current
frame's RGB bounds, preserving bright and dark detail missed by one raster phase.

Stationary coverage blends in linear light so an old dark sample cannot dominate
an HDR emitter through Karis weighting. Disjoint current/history neighborhood
color bounds reject abrupt shading changes. Moving/clipped history retains
variance clipping, source bounds, and Karis weighting.

Twelve direct GPU cases exercise both shipped resolve kernels: missed bright and
dark detail, absent intermediate depth, a real footprint-depth match, HDR sky
coverage, and uniform HDR lighting changes. Eight of the first ten cases failed
against `801f8d33`; all twelve pass with the fix. Twelve existing TAA/render-scale/
MSAA pipeline tests and 32 AA settings/jitter tests also pass (56 total). The Editor rebuild and deep code-signature verification succeed.

An isolated 1531x1033 lab run captured 16 stationary, 16 pan, and eight reveal
samples per mode, with zero render-graph errors at native and 0.67 internal scale.
In the fixed checker ROI (x=850..1139, y=445..644), sampled mean pixel standard
deviation fell from 5.581 to 1.063 display codes for native TAA and from 5.472 to
1.153 for TAAU. The diagonal ROI was approximately unchanged (0.083 to 0.094 and
0.139 to 0.147). Linear coverage integration also changes mean brightness; these
statistics establish reduced sampled checker flicker, not universal fidelity or
performance improvements. Captures are timestamped samples, not consecutive
frames. A stable editor hover outline lies outside both measured ROIs.

Artifacts: `Artifacts/AAQuality/taa-history-fix/` (captures, region-comparison.json,
and regression logs). The user's open scene was not replaced. The complete
MSAA/SSAA matrix was not repeated for this TAA-only follow-up.

## Grazing sphere rim — 2026-09-09

The native-scale TAA sphere repro exposed false depth rejection beyond the
earlier single-resolve gates: jitter samples different depths on a steep facet,
and that facet can disappear from the current point-depth footprint altogether.
Matching only FP16 rounding error repeatedly discarded valid rim history.

The resolve uses bounded one-sided depth slopes, then (only near a sky/surface
silhouette) looks for a surviving nearby history sample on the same depth layer.
It does not replace surface matching with a global depth min/max interval.
Shading-change rejection accounts for the full silhouette reconstruction
footprint, expressed in input pixels for TAAU. Flat-depth replacement, complete
foreground removal, and uniform HDR lighting changes still reject stale history.
The base history weight and moving-history path are unchanged.

The new GPU sequence evaluates an analytic sloped silhouette over eight jitter
phases for 96 resolves, feeding the actual FP16 color/depth history into each
subsequent dispatch, then replacing the foreground. It runs for native TAA and
TAAU. Against the pre-fix packages both cases fail: the settled rim falls back to
0.25 on missed phases, with peak-to-peak swings of 21.828 and 29.766 linear units.
The corrected shaders pass the sequence and flat-depth/sky rejection controls.

In the same 1531x1033 sphere view at **1x render scale**, the fixed rim ROI
(x=700..899, y=548..568) had mean per-channel temporal standard deviation
**0.894 → 0.075 display codes** (about 92% lower). Maximum channel swing anywhere
in that ROI fell **109 → 8**; the originally reported pixel (869,557), red channel,
fell from a range of **40..149 to 112..116**. The rim retains its accumulated
brightness; it is not removed or hidden by a higher render scale.

These are 12 before and 24 after timestamped samples, not consecutive-frame
video. Camera, viewport and native scale match; capture dither/deband were off
and pending material texture binds were zero. Small residual sampling variation
remains; this is a targeted static-scene fix, not universal animation or
performance qualification. Captures and measurement metadata are local under
`Artifacts/AAQuality/rim-history-fix/`. The source scene was not saved or replaced.

Verification: macOS arm64 Debug Editor build and deep code-signature validation;
18 TAA, 14 SSAA and 161 pipeline-declaration tests pass with no skips (193 total).
