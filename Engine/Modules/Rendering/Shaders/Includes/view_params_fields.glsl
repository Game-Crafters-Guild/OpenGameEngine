// Shared per-view ViewParams std140 field list — the single source for the
// canonical block (view_params.glsl, set 0 binding 7, 480 B), its truncated
// prefixes declared inline by the packaged Forward+ / post-FX shaders, and the
// C++ mirror (Rendering/Core/ViewParamsLayout.h). Field order and std140
// offsets are a GPU ABI; every declaration derives from this list via #include,
// so a change here ripples to all of them and drift cannot compile.
//
// Dual-language X-macro (no include guard — included multiple times by design):
// each consumer #defines GE_VP_MAT4 / GE_VP_VEC4 before including, #undefs after.
//   GLSL:  #define GE_VP_MAT4(n) mat4 n;      #define GE_VP_VEC4(n) vec4 n;
//   C++:   #define GE_VP_MAT4(n) float n[16]; #define GE_VP_VEC4(n) float n[4];
//
// Prefix rule: a shorter consumer declares an exact leading prefix by #defining
// a level guard BEFORE the include. The full 480 B block is the default.
//   GE_VIEWPARAMS_CORE_ONLY -> 160 B (invProj, view, nearFar, cameraPosWS)
//   GE_VIEWPARAMS_OMIT_PROJ -> 240 B (adds screenSize, viewProj; omits proj)
//   (neither)               -> 480 B (adds proj, mipBiasParams, prevViewProj, invView,
//                                 taaJitter, exposureParams) — canonical block + C++ mirror
//
// GROW BY APPENDING ONLY: existing order/offsets are shared with every consumer.
GE_VP_MAT4(ge_invProj)     //   0  view->clip inverse (unproject to view space)
GE_VP_MAT4(ge_view)        //  64  world->view
GE_VP_VEC4(ge_nearFar)     // 128  x=near y=far z=1/log(far/near) w=timeSeconds
GE_VP_VEC4(ge_cameraPosWS) // 144  xyz=world-space camera position
#ifndef GE_VIEWPARAMS_CORE_ONLY
GE_VP_VEC4(ge_screenSize)  // 160  xy=resolution px, zw=1/resolution
GE_VP_MAT4(ge_viewProj)    // 176  world->clip for this view
#ifndef GE_VIEWPARAMS_OMIT_PROJ
GE_VP_MAT4(ge_proj)        // 240  view->clip (== inverse(ge_invProj))
GE_VP_VEC4(ge_mipBiasParams) // 304 x=material mip bias: log2(renderW/outputW), [-2,0] — TAAU
                             //     texture-sharpness compensation, exactly 0.0 when the view
                             //     is not upscaling. yzw reserved.
GE_VP_MAT4(ge_prevViewProj)  // 320 previous rendered frame's UNJITTERED world->clip, for
                             //     temporal reprojection. Equals ge_viewProj on the first
                             //     frame of a view (zero motion).
GE_VP_MAT4(ge_invView)       // 384 view->world (inverse of ge_view): reconstruct a world
                             //     position without a per-pixel matrix inverse.
GE_VP_VEC4(ge_taaJitter)     // 448 xy=current, zw=previous frame sub-pixel NDC jitter; all
                             //     zero when no jittered AA drives this view. Consumers
                             //     reprojecting through the UNJITTERED ge_prevViewProj add
                             //     zw to land on the previous frame's actual raster grid.
GE_VP_VEC4(ge_exposureParams) // 464 the exposure the tonemap applies to this view: x = the
                             //     Fixed/Manual/Physical linear scale, y = 1 while auto exposure
                             //     meters the view (the metered scale is then in ExposureHistory).
                             //     zw reserved. GE_ViewExposureScale (view_exposure.glsl) resolves it.
#endif
#endif
