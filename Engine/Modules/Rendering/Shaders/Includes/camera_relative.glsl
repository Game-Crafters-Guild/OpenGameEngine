// Camera-relative rendering (Earth-scale precision) — shared vertex helpers used
// by the standard mesh path (adapter_vertex.glsl) and the depth-only / shadow
// path (shadow_depth_shared*.vert). Assumes a CameraUBO instance named `Cam`
// (camera_ubo_fields.glsl) is already in scope.
//
// The render origin is expressed as an integer sector (exact, large scale) plus
// an fp32 local remainder. When it is inactive (uRenderOriginSector.xyz == 0)
// every helper reduces to the full-world path, byte-identical to the pre-feature
// build. When active, a position is reconstructed relative to the render origin
// from the EXACT integer sector delta and projected with the rebased view-proj,
// so the clip position stays fp32-precise at planetary distance.

#ifndef GE_CAMERA_RELATIVE_GLSL
#define GE_CAMERA_RELATIVE_GLSL

// Meters per sector. GLSL mirror of Components::kWorldSectorSize (== 1024) — a
// tagged entity's world position (sector*GE_SECTOR_SIZE + local) is INTRINSIC,
// so this must be a fixed engine constant, NOT read from the per-view CameraUBO:
// a pass whose CameraUBO was not rebased (shadow cascades, probes, or any view
// that bypasses ComputeRebasedView) carries uRenderOriginSector.w == 0, which
// would otherwise collapse the tag to its sector-local position. Locked to the
// C++ side by RenderOriginTests.ComposeMatchesRenderReconstruction.
const float GE_SECTOR_SIZE = 1024.0;

// Project a full-world position. The planet branch (uCameraPos.w < 0) builds a
// reverse-Z depth from view-space distance for the auto-extended far plane.
vec4 GE_ProjectDepthPosition(vec4 worldPos)
{
    if (Cam.uCameraPos.w < 0.0)
    {
        float farPlane = max(-Cam.uCameraPos.w, 0.001);
        vec3 localPos = (Cam.uV * worldPos).xyz;
        float dist = length(localPos);
        vec3 dir = dist > 1e-6 ? localPos / dist : vec3(0.0, 0.0, 1.0);
        vec2 ndc = dir.xy / max(1.0 + abs(dir.z), 1e-6);
        float reverseDepth = clamp(1.0 - dist / farPlane, 0.0, 1.0);
        return vec4(ndc, reverseDepth, 1.0);
    }
    return Cam.uVP * worldPos;
}

bool GE_RenderOriginActive()
{
    return (Cam.uRenderOriginSector.x | Cam.uRenderOriginSector.y | Cam.uRenderOriginSector.z) != 0;
}

// World-space position of the render origin (originSector * sector size). Zero
// when the origin is inactive. Passes/geometry that own their world position
// (terrain, procedural, ocean) subtract this to feed shadow_sampling a
// render-origin-relative receiver position matching the rebased ge_shadowVP.
vec3 GE_RenderOriginWorld()
{
    return vec3(Cam.uRenderOriginSector.xyz) * GE_SECTOR_SIZE;
}

// GE_ProjectDepthPosition consuming a render-origin-relative position through the
// rebased view / view-proj (Cam.uVRel / Cam.uVPRel).
vec4 GE_ProjectDepthPositionRel(vec3 relWorldPos)
{
    if (Cam.uCameraPos.w < 0.0)
    {
        float farPlane = max(-Cam.uCameraPos.w, 0.001);
        vec3 localPos = (Cam.uVRel * vec4(relWorldPos, 1.0)).xyz;
        float dist = length(localPos);
        vec3 dir = dist > 1e-6 ? localPos / dist : vec3(0.0, 0.0, 1.0);
        vec2 ndc = dir.xy / max(1.0 + abs(dir.z), 1e-6);
        float reverseDepth = clamp(1.0 - dist / farPlane, 0.0, 1.0);
        return vec4(ndc, reverseDepth, 1.0);
    }
    return Cam.uVPRel * vec4(relWorldPos, 1.0);
}

// Project a SECTOR-LOCAL world position (model matrix applied) for an instance
// with integer sector `instSector`. Writes the absolute world position to
// outFullWorld for the fragment stage (lighting/fog), and the PRECISE render-
// origin-relative position to outRelWorld (small magnitude at any distance) for
// the fragment shadow receiver — it is the one value the receiver can project
// through the rebased ge_shadowVP without the big·big cancellation that speckles
// self-shadows. Returns the clip position. At sector 0 with the origin inactive
// outRelWorld == outFullWorld and the whole path equals the pre-feature build.
vec4 GE_ClipFromSectorLocal(vec3 sectorLocalWorldPos, ivec3 instSector,
                            out vec3 outFullWorld, out vec3 outRelWorld)
{
    ivec3 originSector = Cam.uRenderOriginSector.xyz;
    float sectorSize = GE_SECTOR_SIZE; // intrinsic engine constant, NOT the per-view .w (see above)
    if (GE_RenderOriginActive())
    {
        vec3 relWorldPos = vec3(instSector - originSector) * sectorSize + sectorLocalWorldPos;
        outFullWorld = relWorldPos + vec3(originSector) * sectorSize;
        outRelWorld = relWorldPos;
        return GE_ProjectDepthPositionRel(relWorldPos);
    }
    outFullWorld = sectorLocalWorldPos + vec3(instSector) * sectorSize;
    outRelWorld = outFullWorld; // origin inactive => relative frame IS the world frame
    return GE_ProjectDepthPosition(vec4(outFullWorld, 1.0));
}

// Project a FULL-world position for geometry that OWNS its world coordinates and
// has no per-instance sector — vertex-output modifiers such as CBT terrain, ocean,
// and other procedural surfaces whose vertices are already at planetary magnitude
// (unlike a mesh, whose small local verts ride a far instance transform). Subtracts
// the render origin so the projection runs on a small camera-relative coordinate
// through the rebased view-proj, killing the big·big cancellation that swims a
// planetary surface under camera motion. outRel receives the render-origin-relative
// position for the fragment shadow receiver. At origin (0,0,0) the subtraction is a
// zero (IEEE-exact) no-op and the full-world path is taken — byte-identical to the
// pre-feature build (the dark-ship gate). Mirror of GE_ClipFromSectorLocal for the
// no-sector case (Earth-scale slice 1b).
vec4 GE_ClipFromWorld(vec3 worldPos, out vec3 outRel)
{
    if (GE_RenderOriginActive())
    {
        vec3 relWorldPos = worldPos - GE_RenderOriginWorld();
        outRel = relWorldPos;
        return GE_ProjectDepthPositionRel(relWorldPos);
    }
    outRel = worldPos;
    return GE_ProjectDepthPosition(vec4(worldPos, 1.0));
}

#endif // GE_CAMERA_RELATIVE_GLSL
