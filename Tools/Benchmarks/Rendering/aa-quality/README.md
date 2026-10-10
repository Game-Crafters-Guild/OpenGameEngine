# AA quality lab

## Automated correctness tests

The independent `TaaResolveHistoryTests` and `SsaaResolveTests` CTest targets live
in `Engine/Tests`. They run shipped shaders headlessly with synthetic inputs;
neither imports this lab, needs Python, nor changes an editor scene or settings.
Missing shader packages fail; an unavailable Vulkan device is reported as skipped
by GoogleTest. Check the test output for skips when qualifying a GPU run.

```sh
cmake --build --preset macos-arm64-editor-fast --target TaaResolveHistoryTests SsaaResolveTests
ctest --test-dir build/macos-arm64-editor-fast -R '^(TaaResolveHistoryTests|SsaaResolveTests)$' --output-on-failure
```

SSAA compares every output channel with independent CPU point-tap tent
convolution at 1.25x, 1.5x, 2x and odd extents, including HDR constants, thin
detail, checker patterns, alpha and clamped borders. FP32 output isolates kernel
correctness from display processing and FP16 quantization. TAA includes both
single-resolve current/history/depth cases and a 96-frame sloped-silhouette
sequence that feeds GPU history into the next resolve across eight jitter
phases, then checks foreground removal. These targeted tests are not a general
perceptual-quality or animated-scene qualification suite.

## Interactive benchmark

This isolated project exercises diagonal geometry, subpixel fences, a deliberately
unfiltered checker shader, glossy geometry, a movable occluder, and a UV-border
diagnostic. Red in the UV diagnostic means a shaded fragment interpolated outside
the triangle's valid UV domain. The checker separates shading aliasing from MSAA
coverage; MSAA is not expected to supersample a triangle's interior.

## Reproduce

From a built worktree:

```sh
python3 Tools/Benchmarks/Rendering/aa-quality/make_scene.py Artifacts/AAQuality/Project
```

On macOS, Windows, or Linux, after a Release (or DebugFast) Editor build:

```sh
python3 Tools/Benchmarks/Rendering/aa-quality/launch.py --label baseline
```

Pass `--exe /path/to/Editor` (or `GE_EDITOR`) when the binary is not in a
default staged location. The script refuses an occupied debug port.
`GE_EDITOR_USER_DATA_ROOT` points at `Artifacts/AAQuality/EditorData` (prefs,
EditorAssets, and cache). Shared EditorAssets are otherwise refreshed by each
launching worktree, invalidating another build's shader measurements. The lab
fixes exposure at EV100 16.9 and disables post sharpening, bloom, color
filtering, and CRT in its private profile. Chart radiance is scaled to remain
visible at this exposure. Then:

```sh
python3 Tools/Benchmarks/Rendering/aa-quality/capture.py --output Artifacts/AAQuality/before
python3 Tools/Benchmarks/Rendering/aa-quality/analyze.py Artifacts/AAQuality/before
python3 Tools/Benchmarks/Rendering/aa-quality/verify_tent.py --output Artifacts/AAQuality/before/tent.json
```

Analysis and filter verification require `numpy` and `Pillow`. Keep the editor window dimensions identical
between runs. Wait for shader compilation and material loading to finish before
capturing. The harness checks worktree provenance and scene/material readiness;
each mode records requested settings, applied settings, render resources, camera,
frame indices, screenshots, and profiler samples.

The default matrix includes Off, native TAA, TAAU at 67%, MSAA 2/4/8, and SSAA at
125/150/200% linear resolution (156/225/400% pixel cost before other overhead).
Unsupported MSAA counts must be reported at their actual applied sample count.

## Interpretation

- Static standard deviation measures residual flicker in display code values,
  including an edge-only statistic. It does not measure spatial fidelity: blur
  can lower this number, so inspect the images and detail retention too.
- Small camera translations exercise subpixel movement. These are sampled
  captures with recorded frame indices, **not consecutive or frame-locked video**.
  Do not infer exact ghost lifetimes or fixed-speed motion quality from them.
- The profiler reports Metal encoder spans on Apple hardware. Shared spans
  cannot be added into pass costs; retain the reported timing semantics. The
  distinct-span statistic is an upper bound, not isolated AA cost.
- The generated scene deliberately disables shadows on chart geometry and uses
  mostly unlit materials to avoid confusing shadow/lighting noise with AA.
- PNG captures disable capture dithering and debanding. They are for display
  comparison, not linear-HDR radiometric error measurements.
- `verify_tent.py` reads tiled, undithered FP16 source/output textures from the
  actual spatial pass and compares them with CPU point-sample tent convolution.
  The chart and camera must stay stationary throughout the tiled captures.
  This is a live diagnostic report, not the automated correctness gate above.
  `verify_area.py` remains for historical box-filter comparisons, not as an oracle
  for the shipped tent kernel. Neither script measures perceptual quality.

Pass `--occlusion` to capture a covered fence and eight sampled reveals per mode.
After capturing the updated build and running tent verification, compare with:

```sh
python3 Tools/Benchmarks/Rendering/aa-quality/compare.py Artifacts/AAQuality/before Artifacts/AAQuality/after
```

This checks control-image parity, MSAA border diagnostics and TAA static stability,
then creates a slowed, sampled TAA comparison GIF. It deliberately expects the
historical defective baseline (MSAA red diagnostics and substantially more TAA
flicker); comparing two good builds need not pass. It is not a general CI gate
and does not depend on the optional live `tent.json` report.

Production qualification additionally needs frame-locked skinned animation,
alpha-tested foliage, transparency/reactive shading, exposure changes, camera
cuts, multiple frame rates, and representative hardware.

Measured results and remaining qualification gaps are in [RESULTS.md](RESULTS.md).
