// The terrain blend width: how many materials may shade one fragment.
//
// ONE definition, included by BOTH terrain surfaces — the CBT ground (CBT/cbt_surface.glsl) and the
// grass standing on it (TerrainGrass/terrain_grass_surface.glsl). A blade tints its base with the
// ground colour beneath it, so the two cannot blend different numbers of materials without the
// blade disagreeing with its own ground. Kept here rather than in either surface because a knob
// spelled out in two places is a knob that gets half-turned.
//
// Include this BEFORE Includes/terrain_blend_resolve.glsl, which is parameterized by it and
// deliberately defaults nothing.

#ifndef GE_TERRAIN_BLEND_WIDTH_DECLARED
#define GE_TERRAIN_BLEND_WIDTH_DECLARED

// How many materials may shade one fragment. The splat carries four channels; this caps how many
// of them survive to be sampled, keeping the heaviest and renormalizing their weights to sum 1.
//
// SHIPPED AT 3, which on a four-channel splat means the cap is reached in the regions where all
// four materials are painted together — measured 2026-08-12 on ComposedIsland as ~7.5% of frame pixels
// (|delta| max over RGB > 6/255)
// at the recorded texel pose: an area, not isolated corners. Anywhere three or fewer are active nothing is dropped,
// so the softening below subtracts zero and the result is BITWISE the plain renormalization — the
// reference image, not an approximation of it (CBTMaterialBlend asserts that exactly, not to a
// tolerance). Only those four-material regions are shaded differently from a full blend, and there
// the difference is a continuous fade rather than a boundary.
//
// THE TRADE. Each surviving material costs a full set of taps — albedo, normal and ORM, each up to
// three triplanar projections, each up to three more under hex tiling — so this is very nearly a
// direct multiplier on the fragment's texture traffic. 2 is one edit away and is the perf-lean
// option: measured 2026-08-12 (Release, GPU profiler, same-session pairs), 3 costs +0.204 ms over 2
// with the terrain at texel range and +0.129 ms at mid range.
//
// What no width costs is a seam, and that took a second attempt to get right. Selection FADES the
// losers out rather than clipping them (CBT_SelectTopWeights), so the blend stays continuous where
// the ranking order swaps. Clipping them looked defensible on the assumption that materials only
// meet at isolated points — false on a painted terrain, where materials brushed across each other
// share wide bands and the swap contour draws a long, visible line right through the middle of one.
//
// K = 4 keeps every channel, leaving the blend to the weight floor alone. The renormalization
// still runs there, so the survivors sum to exactly 1 rather than to 1 minus whatever the floor
// dropped — a fraction of a percent, in the direction of not darkening.
//
// PROMOTION PATH. A #define because this surface has no spec-constant or keyword mechanism today.
// It becomes runtime-selectable by lifting it to a MaterialKeyword — the surface already composes
// under ForwardPlus | Shadows | IBL — and compiling the variants.
#define CBT_MAX_BLEND_MATERIALS 3

#endif // GE_TERRAIN_BLEND_WIDTH_DECLARED
