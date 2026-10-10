# Minimal AgX implementation (Benjamin Wrensch)

- Source: https://iolite-engine.com/blog_posts/minimal_agx_implementation
  (also published by the author as Shadertoy
  https://www.shadertoy.com/view/cd3XWr)
- License: MIT, Copyright (c) 2024 Missing Deadlines (Benjamin Wrensch)
  (see `LICENSE.txt`; the post's changelog records the license header being
  added 2024-01-11). Fetched 2026-08-04.
- AgX itself is Troy Sobotka's display transform
  (https://github.com/sobotka/AgX). That repository carries **no license file**
  (checked at revision `a66ad4b1e74952531d95634d60fe25de0025462e`, 2026-08-04) —
  what GameEngine adapts is Wrensch's MIT-licensed minimal implementation, whose
  own notice states its values are sourced from Sobotka's AgX config.
- Adapted components: `TonemapAgX` in
  `Engine/Modules/Rendering/Shaders/tonemap.frag` — the pipeline structure
  (inset matrix, log2 encoding over [-12.47393, 4.026069] EV, sigmoid, outset,
  `^2.2` linearization), the EV range (which originates as the `lg2` allocation
  in Sobotka's `config.ocio`), and the 6th-order sigmoid approximation
  digit-for-digit (`15.5, -40.14, 31.96, -6.868, 0.4298, 0.1191, -0.00232`).
- Not from this upstream: the inset/outset matrices are **not** Wrensch's
  printed values (max |Δ| ≈ 5.8e-5 against them and against the identical
  matrix in Sobotka's `config.ocio`), and they match no other published variant
  checked (Filament, three.js, and Godot were each fetched and compared
  2026-08-04). They form their own consistent pair — the outset is the inset's
  inverse to the printed precision (~5e-11) — so they are a re-derivation in
  the same AgX lineage rather than a transcription of any of the above; their
  concrete generator is not recorded (the values entered the tree in a bulk
  work-in-progress commit predating this note).
