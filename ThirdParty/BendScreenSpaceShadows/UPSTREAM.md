# Bend Studio screen-space shadows

Copyright 2023 Sony Interactive Entertainment. Apache License 2.0.

Public source: https://www.bendstudio.com/blog/inside-bend-screen-space-shadows/
Archive: https://www.bendstudio.com/assets/cms/downloads/code_final_candidate.zip
Retrieved 2026-09-09. Archive SHA-256: `75707a8e287d485c0f71d04fb0ede245bb9a7e9569f1492b1c4d1f6ab943de83`.

The two bend_sss headers are retained with their content unmodified; the
repository normalises their line endings to LF. The archive has no NOTICE file.
Engine/Modules/Rendering/Shaders/ScreenSpaceShadows/screen_space_shadow.comp is a modified GLSL port
of bend_sss_gpu.h: reverse-Z only, portable workgroup barriers, explicit border
reads and bounds/finite guards, 60 samples, built-in contrast/fade filtering,
strength control, numerically stable ray deltas, the upstream default bilinear
sampling mode (point receiver, conservative envelope blocker),
continuous segment intersections gated by depth
continuity, and atomic visibility reduction
for overlapping radial rays. The interleaved sample lanes combine with minimum
visibility for opaque blockers rather than the upstream four-lane average: the
depth-aware resolve already removes the isolated single-lane hits the average
exists to soften, and averaging costs about a tenth of the term's strength while
banding the remaining gradient with the fade samples' steps.
IgnoreEdgePixels stays off, its upstream default; a scene of large flat
surfaces at grazing angles can want it on. Compute initializes a transient visibility buffer,
traces into it with portable buffer atomics, and resolves it into a sampled
R32_UINT mask. The resolve uses a depth-gated structure tensor to filter along coherent
shadow edges with a directional 7x7 footprint, retaining a 5x5 binomial
filter elsewhere. It rejects samples across receiver silhouettes.
This initializes the light-center singularity too. The mask
stores the upper 24 bits of float receiver depth and an 8-bit visibility value.
The engine's additional compute passes project the active fixed grid-PCF
footprint onto the receiver and accumulate depth-validated visibility through
camera reprojection. They use the existing cascade constants and per-view
packed history; these are engine extensions rather than part of the upstream
tracer. The tracer uses no subgroup size assumptions, geometry buffers, normals,
or motion vectors. The upstream wave-intrinsic early-out has no portable
equivalent under that rule and is not ported: a sky pixel skips the sample
loop per lane instead, and a wavefront covering only sky still pays its
four depth reads.
ScreenSpaceShadowDispatch.cpp validates inputs before calling the original CPU
planner. The engine enables this path for perspective cameras only. Nearly
parallel or extreme light projections retain the existing directional shadow
source. GPU validation covers native Metal and Vulkan; other backends require
their corresponding cooked shader packages.
