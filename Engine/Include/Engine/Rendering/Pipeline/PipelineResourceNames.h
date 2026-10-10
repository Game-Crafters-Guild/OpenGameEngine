#pragma once

// Canonical pipeline-blackboard resource names (the {ViewId, Name} keyspace of
// PipelineFrameResources). C++ publishers/consumers and the blueprint validator
// must reference these constants instead of repeating string literals so a
// rename is a compile error rather than a silently dropped binding (BP-4).
//
// Blueprint-only names (the post-FX chain: HDRFiltered, BloomA, LDRFinal, ...)
// deliberately have no constants — they exist only in .rendergraph JSON and are
// validated at Compile by ValidateBlueprint. Tests also keep raw literals: they
// pin the on-disk blueprint contract and act as rename canaries.
//
// NOT this keyspace (do not add here): RG pool/import names ("prefix" +
// viewId), RG pass/debug labels, and shader-reflection binding names (e.g. the
// "ViewParams" UBO binding — lexically identical to Res::ViewParams but a
// different domain).
namespace GameEngine::Engine::Renderer::Pipeline::Names
{

// Per-view builtins seeded by the pipeline spine and republishable by nodes.
namespace View
{
inline constexpr const char* Color = "View.Color";
inline constexpr const char* Depth = "View.Depth";
inline constexpr const char* Resolve = "View.Resolve";
// Single-sample R32F copy of the view depth: everything the colour pass depth-tests against.
inline constexpr const char* DepthResolved = "View.DepthResolved";
// The occluders' depth GTAO and the world pass's contact shadows read: View.DepthResolved without the
// non-occluding prepass heads (grass). The same texture when the view has none (DepthResolveNode).
inline constexpr const char* OccluderDepthResolved = "View.OccluderDepthResolved";
inline constexpr const char* GTAO = "View.GTAO";
inline constexpr const char* HZB = "View.HZB";
// Per-view movers pass (RGBA16F): .rg = unjittered current-minus-previous
// viewport UV, .b = previous raw depth, .a = 1 where a mover drew. Pixels no
// mover covers keep the clear sentinel (.rg far outside any real delta, .a 0),
// which tells a consumer to reproject analytically through the camera.
// Recorded by ViewMotionVectors for whichever of SSR and TAA declares first.
inline constexpr const char* MotionVectors = "View.MotionVectors";
// Nearest-surface (MAX-reduced) Hi-Z pyramid for SSR ray traversal — the
// opposite bound from the MIN/farthest occlusion HZB above. Built by
// HZBBuildNode only when SSSR is active for the view.
inline constexpr const char* SSRHiZ = "View.SSRHiZ";
// Forward-pass MRT slice: .rg = octahedral VIEW-space normal, .b = perceptual
// roughness, .a = metallic. Written only under MaterialKeyword::SSSRNormalRoughness.
// A consumer with its own per-view target for the slice (the DDGI glossy
// resolve's shading-normal history) hands it to ReflectionsProvider instead
// of resolving this name; the *Written publish below then names that texture.
inline constexpr const char* NormalRoughness = "View.NormalRoughness";
// Forward base-lobe evaluation. Weight.rgb * Radiance.rgb is exactly the
// replaceable indirect specular; the two alpha channels encode its direction.
inline constexpr const char* SSRSpecularWeight = "View.SSRSpecularWeight";
inline constexpr const char* SSRSpecularRadiance = "View.SSRSpecularRadiance";
// Positive "written this frame" signal for the SSR slices above.
// ReflectionsProvider publishes these only on frames it attaches the slices as
// world-pass MRTs (keyword on, slices cleared and written); the SSR node
// consumes the G-buffer exclusively through them. The raw names above resolving
// valid proves nothing — a blueprint-declared resource materializes on demand
// even on frames nothing writes it (MSAA withholds the attachments).
inline constexpr const char* NormalRoughnessWritten = "View.NormalRoughness.Written";
inline constexpr const char* SSRSpecularWeightWritten = "View.SSRSpecularWeight.Written";
inline constexpr const char* SSRSpecularRadianceWritten = "View.SSRSpecularRadiance.Written";
// DDGI scaled glossy-reflection resolve outputs (one per baked lobe),
// published by WorldRenderNode on frames it declares the resolve pass
// (DDGIProbeFeature::DeclareGlossyResolveForView) so the phase-B recover
// pass (ScatterBNode) binds the same textures the phase-A world pass did.
inline constexpr const char* DDGIResolveRough = "View.DDGIResolveRough";
inline constexpr const char* DDGIResolveGlossy = "View.DDGIResolveGlossy";
inline constexpr const char* DDGIResolveIrradiance = "View.DDGIResolveIrradiance";
inline constexpr const char* EffectiveColor = "View.EffectiveColor";
inline constexpr const char* DepthResolvedPostOcean = "View.DepthResolvedPostOcean";
// Split-active only (internal-resolution rendering): the caller's
// DISPLAY-resolution targets, preserved when the pre-pass redirects View.Depth
// to the scaled internal depth. The crossing node — TemporalAA under TAA,
// RenderScaleUpscale otherwise — writes both at the upscale point (OutputDepth
// via the depth upsample for the editor's overlay depth test; OutputColor
// becomes the republished View.Resolve). Absent when scale == 1.
inline constexpr const char* OutputColor = "View.OutputColor";
inline constexpr const char* OutputDepth = "View.OutputDepth";
} // namespace View

// Well-known declared-resource / node-published names.
namespace Res
{
inline constexpr const char* SceneColor = "SceneColor";
inline constexpr const char* ViewParams = "ViewParams";
inline constexpr const char* LightBuffer = "LightBuffer";
inline constexpr const char* HeightFogParams = "HeightFogParams";
inline constexpr const char* CloudParams = "CloudParams";
inline constexpr const char* ColorGradeParams = "ColorGradeParams";
inline constexpr const char* ShadowData = "ShadowData";
inline constexpr const char* ExposureHistory = "ExposureHistory";
inline constexpr const char* ClusterBuffer = "ClusterBuffer";
inline constexpr const char* ClusterParams = "ClusterParams";
inline constexpr const char* LightIndexBuffer = "LightIndexBuffer";
inline constexpr const char* DepthMinMaxBuffer = "DepthMinMaxBuffer";
} // namespace Res

// Pipeline output-slot names (RenderPipelineBlueprint::outputs keys).
namespace Output
{
inline constexpr const char* FinalColor = "FinalColor";
} // namespace Output

} // namespace GameEngine::Engine::Renderer::Pipeline::Names
