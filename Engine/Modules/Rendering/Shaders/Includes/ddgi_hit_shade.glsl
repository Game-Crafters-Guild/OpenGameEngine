// DDGI ray-hit shading: instance/material lookup, next-event-estimation
// (NEE) direct lighting via shadow rays against the shared TLAS, and the
// world-space sky-miss term, plus the recursive bounce fetch (the previous
// frame's probe field, gathered the same way the forward consumer gathers it).
// Shared by every DDGI trace kernel (HW ray-query,
// ddgi_trace_hw.comp; software BVH traversal, ddgi_trace_sw.comp) so hit
// shading stays as close to identical across trace backends as GLSL allows —
// only ray generation/traversal differs per kernel.
//
// Self-contained bindings (set 0, b0..b4 — DDGI's trace kernel owns its own
// small descriptor set, entirely separate from the forward pass's set 0/1;
// see ddgi_trace_hw.comp for the rest of the set: volume params, the
// previous-frame atlas, and the ray output buffer). Reuses the SAME backing
// buffers the forward pass and clustered lighting already populate
// (MaterialParamsSSBO, LightBuffer, the IBL irradiance cube, GPUScene's
// instance buffer) — DDGI owns no separate material/light/instance table.
//
// Requires the including .comp to have already declared these extensions
// (GLSL #extension pragmas apply file-wide regardless of include order, but
// convention keeps them at the top of the top-level shader file):
//   #extension GL_EXT_ray_query : require
//   #extension GL_EXT_buffer_reference : require
//   #extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
//
// SOFTWARE-LANE SPLIT: an includer that #defines GE_DDGI_SOFTWARE_TRACE before
// including gets only the extension-free half — light array + attenuation
// curve + sky miss. The two guarded regions below need capabilities the
// software lane exists precisely to avoid: buffer_reference/int64 (the
// per-mesh vertex/index device addresses, which the software lane replaces
// with its own pooled vertex SSBO) and the accelerationStructureEXT type
// (NEE's shadow-ray parameter, which the software lane replaces with a
// BLAS/TLAS array walk). Every unguarded declaration below is shared verbatim
// by both kernels; the guarded halves have per-kernel counterparts.

#ifndef GE_DDGI_HIT_SHADE_GLSL
#define GE_DDGI_HIT_SHADE_GLSL

#include "ddgi_common.glsl"
// GE_EMISSION_PAPERWHITE_NITS — the emission authoring anchor. Included from
// the raster path's own header rather than restated here so the two can never
// drift; a GI lane that anchored emission anywhere else would light the scene
// in different units from the image it is supposed to match.
#include "surface_io.glsl"

const float GE_DDGI_PI = 3.14159265358979323846;

#ifndef GE_DDGI_SOFTWARE_TRACE

// Per-mesh geometry descriptor row — GLSL mirror of
// SceneAccelerationStructureService::MeshGeometryDescGPU (std430, 32 B,
// C++-side static_assert-locked). VertexAddress/IndexAddress are raw
// uint64_t device addresses, wrapped into buffer_reference pointers
// (GE_DDGIPositionsF32 / GE_DDGIIndices16 / GE_DDGIIndices32 below) at the
// point of use. Declared here (not ddgi_common.glsl) because uint64_t needs
// GL_EXT_shader_explicit_arithmetic_types_int64, which only this file's
// includers enable — see ddgi_common.glsl's note.
struct GE_MeshGeometryDesc
{
    uint64_t VertexAddress;
    uint64_t IndexAddress;
    uint VertexStrideBytes;
    uint FirstVertex;
    uint FirstIndex;
    uint IndexKind;         // 0 = uint16, 1 = uint32 (matches Rendering::IndexType)
    uint UV0OffsetBytes;    // byte offset of UV0 inside the interleaved core vertex;
                            // GE_DDGI_NO_UV0 when the mesh carries no UV0 stream
    uint Pad;
};

// UV0OffsetBytes sentinel: the mesh has no UV0 in its core stream, so a hit on
// it shades with the flat base-colour factor whatever its material's map says.
const uint GE_DDGI_NO_UV0 = 0xFFFFFFFFu;

// GPUInstance mirror — same field list GPUScene.h locks (gpu_instance_fields.glsl).
// Mirrors Rendering::kInstanceFlagGIEmitter. This instance's emissive is
// published as a type-3 sphere-proxy light record, so per-hit emissive must be
// suppressed for it or the same energy lands in the probe field twice.
const uint GE_DDGI_INSTANCE_FLAG_GI_EMITTER = 64u;

struct GE_DDGIInstance
{
#include "gpu_instance_fields.glsl"
};
layout(std430, set = 0, binding = 0) readonly buffer DDGIInstanceBuffer
{
    GE_DDGIInstance ge_ddgiInstances[];
};

// Material param block — same field list + stride as the forward pass's
// MaterialParamsSSBO (Includes/material_params.glsl), binding the identical
// buffer. Hit shading reads lane 0 (base colour), lane 1.x (metallic) and lane
// 19 (emission) through the accessors below; the rest exists to keep the
// struct's byte stride correct for indexing.
struct GE_DDGIMaterialData
{
#define GE_FIELD(type, name) type name;
#include "material_params.glsl"
#undef GE_FIELD
    uint TextureIndices[8];
    vec4 TextureST[8];
    vec4 TextureST2[8];
    uint SamplerIndices;
};
layout(std430, set = 0, binding = 1) readonly buffer DDGIMaterialBuffer
{
    GE_DDGIMaterialData ge_ddgiMaterials[];
};
#include "material_row_index.glsl"

// The block is a bare vec4 lane array; the lane names the engine surfaces use
// live in Includes/material_param_lanes.glsl (uBaseColor = lane 0, uParamsN =
// lane N+1). Spelled here as accessors rather than by including those alias
// macros, because a tracing shader also declares its own DDGIParams UBO with
// uParams0/uParams1 fields the macros would rewrite.
//
// These are fixed lanes, so they are only correct for a surface that addresses
// the block by lane name. A surface with `// @property` declarations packs them
// in declaration order and these read whatever landed there; the CPU bake
// (DDGISceneService) follows the declaration instead, so the two tracing lanes
// disagree on such a material. Closing that needs a per-material lane map in
// GE_DDGIMaterialData or a reserved-name rule — design doc §10.
vec4 GE_DDGIMatBaseColor(GE_DDGIMaterialData m) { return m.uParams[0]; }
vec4 GE_DDGIMatMetalRough(GE_DDGIMaterialData m) { return m.uParams[1]; }  // x=metallic, y=roughness
vec4 GE_DDGIMatEmission(GE_DDGIMaterialData m) { return m.uParams[19]; }   // rgb=tint, w=nits

#endif // !GE_DDGI_SOFTWARE_TRACE

// Light array — same field list as clustered_lighting.glsl's GE_LightPacked
// (light_packed_fields.glsl), binding the identical per-frame LightBuffer
// LightUploadNode already populates. Declared standalone (not via
// clustered_lighting.glsl) because that file also declares the cluster/
// shadow bindings this compute ray-hit context has no use for.
struct GE_DDGILightPacked
{
#include "light_packed_fields.glsl"
};
layout(std430, set = 0, binding = 2) readonly buffer DDGILightBuffer
{
    uvec4 ge_ddgiLightHeader;  // x = light count
    GE_DDGILightPacked ge_ddgiLights[];
};

// Same falloff curve the forward pass's clustered lighting uses
// (clustered_lighting.glsl's GE_LocalLightAttenuation) — kept here rather
// than shared via include so this file has no dependency on the cluster
// bindings; duplicated logic, single source of truth is the comment there.
float GE_DDGILocalLightAttenuation(float dist, float range, float decay, float falloffMode)
{
    uint mode = uint(clamp(floor(falloffMode + 0.5), 0.0, 3.0));
    if (mode == 1u)  // Linear
        return max(1.0 - dist / max(range, 0.001), 0.0);
    float distNorm = dist / max(range, 0.001);
    float rangeAtten = max(1.0 - distNorm * distNorm, 0.0);
    rangeAtten *= rangeAtten;
    if (mode == 2u)  // Smooth Range
        return rangeAtten;
    float safeDist = max(dist, 1.0);
    if (mode == 3u)  // Custom
    {
        if (decay > 0.01)
            rangeAtten /= pow(safeDist, decay * 0.5);
        return rangeAtten;
    }
    return rangeAtten / (safeDist * safeDist);  // Physical / inverse square
}

layout(set = 0, binding = 3) uniform samplerCube ge_ddgiSkyIrradiance;

// Material map atlas: every participating material's base-colour, emissive and
// metallic-roughness textures resampled to one square layer each
// (DDGIMaterialMapAtlas, engine side; filled by ddgi_map_atlas_blit.comp). One
// array holds every kind, deduplicated by source texture. Contents are LINEAR —
// the blit samples the source through its own sRGB-aware view, so no decode
// belongs here.
//
// Shared by BOTH trace kernels deliberately: textured albedo is the whole
// source of colour bleed and a textured emitter is the whole source of its own
// light, so a lane that shaded either differently from the other would make the
// two lanes' images incomparable. Only where the layer indices come from
// differs (software: the packed uber-material record's slots 12 and 16;
// hardware: the map table below, keyed the same way its material lookup is).
layout(set = 0, binding = 14) uniform sampler2DArray ge_ddgiMapAtlas;

// Both samplers below use textureLod, never texture(): probe tracing is compute
// with no implicit derivatives, and the atlas is single-mip by construction.
// `layer` is carried as a float because the software lane reads it out of a
// float record; negative means "no map", the same encoding the reference packer
// uses.

// Modulates a material's flat base-colour factor by its atlas layer, if it has
// one.
vec3 GE_DDGIApplyAlbedoMap(vec3 baseColorFactor, float layer, vec2 uv)
{
    if (layer < 0.0)
        return baseColorFactor;
    return baseColorFactor * textureLod(ge_ddgiMapAtlas, vec3(uv, layer), 0.0).rgb;
}

// Metalness the way the raster surface composes it (Surfaces/standard_pbr.glsl):
// the flat factor times the metallic-roughness map's blue channel. glTF leaves
// metallicFactor at 1 and puts the real value in the map, so the factor alone
// would read stone and cloth as metal and the Lambert term below would go
// black — texture colour would never leave the surface.
float GE_DDGIApplyMetallicMap(float metallicFactor, float layer, vec2 uv)
{
    if (layer < 0.0)
        return metallicFactor;
    return metallicFactor * textureLod(ge_ddgiMapAtlas, vec3(uv, layer), 0.0).b;
}

// Lambert energy that actually re-emits into the probe field. Metals have no
// diffuse lobe, so the reference probe kernel uses
// kd = albedo * (1 - metalness) (then ShapeBounce's peak cap).
vec3 GE_DDGIDiffuseAlbedo(vec3 albedo, float metalness)
{
    return max(albedo * (1.0 - clamp(metalness, 0.0, 1.0)), vec3(0.0));
}

// Scene-linear emitted radiance at a hit, composed the way the RASTER surface
// composes it (Surfaces/standard_pbr.glsl): the emissive map multiplies the
// material's emission colour, which carries the authored emission luminance
// folded into scene-linear by the reference-white anchor.
//
// `emissiveFactor` is ALREADY scene-linear (colour x nits / 203): each lane
// divides the authored nits by the reference-white anchor
// (GE_EMISSION_PAPERWHITE_NITS) at the point it materialises it — the software
// lane in its CPU uber-material bake (DDGISceneService), the hardware lane where
// it reads lane 19 through GE_DDGIMatEmission. Without a map (layer -1) the texel
// is white, and with a map assigned but not resident (layer -2,
// DDGIMaterialMapAtlas::kAwaitedLayer) it is black, as on the raster surface.
vec3 GE_DDGIApplyEmissiveMap(vec3 emissiveFactor, float layer, vec2 uv)
{
    if (layer < -1.5)
        return vec3(0.0);
    if (layer < 0.0)
        return emissiveFactor;
    return emissiveFactor * textureLod(ge_ddgiMapAtlas, vec3(uv, layer), 0.0).rgb;
}

// The recursive one-bounce-behind term: this tick's hit re-emits last tick's
// blended irradiance at the hit point. Shared by both trace lanes so they
// cannot drift; the ONLY place either kernel is allowed to shape that term.
//
// Both of its bounds are HUE-PRESERVING, and that is the point. The two jobs
// they do are different and neither substitutes for the other:
//
// 1. GAIN BOUND (convergence). Per channel the field obeys
//    A' = (1-h)*(D + g*Abar) + h*A, where Abar is the trilinear/Chebyshev
//    gather — a convex combination of atlas texels, so |Abar|inf <= |A|inf.
//    A finite fixed point exists iff max_c g_c < 1; see
//    GE_DDGI_MAX_BOUNCE_ALBEDO. The gain vector is therefore rescaled
//    UNIFORMLY by MAX/peak-channel rather than clamped per channel: the
//    largest channel lands exactly on MAX in both branches — the identical
//    bound a per-channel min() gives — but the ratios between channels
//    survive. A per-channel min() instead pulls the vector toward grey as
//    soon as any one channel saturates, so a saturated red wall's bounce
//    lost its hue exactly when BounceIntensity was raised to strengthen it.
//    Below the threshold the rescale is an exact identity (MAX/max(peak,MAX)
//    == 1), so this is not a new attenuation, only a different direction of
//    clipping for the cases that already clipped.
//
// 2. LUMINANCE CEILING (firefly rejection), carried from the reference
//    library: scale the whole colour by clamp/luminance once luminance
//    exceeds the clamp, so a single blown-out bounce sample cannot smear a
//    bright dot through the temporal history — without clipping saturated
//    hues the way a per-channel cap would. Being a multiplier in (0,1]
//    applied AFTER the gain, it can only shrink the term: it cannot restore
//    divergence, and it cannot substitute for the gain bound either (a
//    ceiling alone leaves a gain of 1 ramping the field UP to the ceiling
//    instead of converging to a physical value).
//
// radianceClamp is a ceiling in the field's own units, so 0 means "no bounce
// admitted at all", not "unbounded" — DDGIVolume::RadianceClamp says so too.
// The 1e-6 floor keeps clamp=0 against a black bounce from evaluating 0/0 and
// poisoning the temporal history with NaN.
vec3 GE_DDGIShapeBounce(vec3 albedo, float bounceIntensity, vec3 incomingIrradiance,
                        float radianceClamp)
{
    vec3 gain = max(albedo * bounceIntensity, vec3(0.0));
    float peak = max(gain.r, max(gain.g, gain.b));
    gain *= GE_DDGI_MAX_BOUNCE_ALBEDO / max(peak, GE_DDGI_MAX_BOUNCE_ALBEDO);

    vec3 bounce = gain * incomingIrradiance;
    float luma = dot(bounce, vec3(0.2126, 0.7152, 0.0722));
    return bounce * (radianceClamp / max(max(radianceClamp, luma), 1.0e-6));
}

#ifndef GE_DDGI_SOFTWARE_TRACE

// Per-material map lookup for the HARDWARE lane, indexed by
// GPUInstance.materialIndex — the same key ge_ddgiMaterials is indexed by, so
// one hit does both lookups with one index. The software lane needs no
// counterpart: its uber-material record already carries these values in the
// reference's own slots (12 albedo layer, 16 emissive layer, 18/19 uv scale,
// 20/21 uv offset).
//
// ONE uv transform serves both layers, because the software lane's record has
// exactly one and the two lanes must sample a map at the same place.
struct GE_DDGIMaterialMapRecord
{
    vec4 Layers;       // x = albedo, y = emissive, z = metallic-roughness layer (<0 = none)
    vec4 UvTransform;  // xy = uv scale, zw = uv offset
};
layout(std430, set = 0, binding = 15) readonly buffer DDGIMapTable
{
    GE_DDGIMaterialMapRecord ge_ddgiMapTable[];
};

// Per-mesh vertex position fetch via device address (GL_EXT_buffer_reference).
// Only position is read — DDGI M2 computes a flat geometric (per-triangle)
// normal from the three hit-triangle vertices rather than fetching the full
// interleaved vertex attribute stream, which would need per-mesh
// VertexAttributeFlags awareness this compute context does not have. A flat
// bounce normal is a standard, visually reasonable simplification for
// diffuse-only indirect light (Lumen and similar systems make the same
// trade at this tier).
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer GE_DDGIPositionsF32
{
    float values[];
};
// 16-bit indices read as packed 32-bit words (two indices per word) rather
// than a native uint16_t buffer_reference — avoids requiring
// GL_EXT_shader_explicit_arithmetic_types_int16 (a second, narrower
// int-width extension beyond the int64 this file already needs), which is
// less consistently supported than plain ray query on otherwise-capable
// hardware. Portable across any GL_EXT_buffer_reference-capable GPU.
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer GE_DDGIIndices16Packed
{
    uint values[];
};
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer GE_DDGIIndices32
{
    uint values[];
};

layout(std430, set = 0, binding = 4) readonly buffer DDGIMeshGeometryBuffer
{
    GE_MeshGeometryDesc ge_ddgiMeshGeometry[];
};

uint GE_DDGIFetchIndex(GE_MeshGeometryDesc mesh, uint indexInTriangle)
{
    uint idx = mesh.FirstIndex + indexInTriangle;
    if (mesh.IndexKind == 0u)
    {
        GE_DDGIIndices16Packed ib = GE_DDGIIndices16Packed(mesh.IndexAddress);
        uint packed = ib.values[idx >> 1u];
        return (idx & 1u) == 0u ? (packed & 0xFFFFu) : (packed >> 16u);
    }
    GE_DDGIIndices32 ib = GE_DDGIIndices32(mesh.IndexAddress);
    return ib.values[idx];
}

// Fetches vertex `poolVertexIndex` (pool-space, i.e. already includes
// FirstVertex) position from the interleaved core vertex stream. Position
// leads every interleaved vertex (VertexAttributeFlags binding-0 contract,
// same as BlasTriangleGeometry's doc) — reading the first 3 floats of the
// strided record is always valid regardless of what attributes follow.
vec3 GE_DDGIFetchPosition(GE_MeshGeometryDesc mesh, uint poolVertexIndex)
{
    GE_DDGIPositionsF32 vb = GE_DDGIPositionsF32(mesh.VertexAddress);
    uint strideFloats = max(mesh.VertexStrideBytes / 4u, 3u);
    uint base = poolVertexIndex * strideFloats;
    return vec3(vb.values[base + 0u], vb.values[base + 1u], vb.values[base + 2u]);
}

// Fetches vertex `poolVertexIndex`'s UV0 from the interleaved core vertex
// stream. Unlike position (which always leads the record), UV0's offset varies
// with the mesh's VertexAttributeFlags, so it is published per mesh.
vec2 GE_DDGIFetchUV0(GE_MeshGeometryDesc mesh, uint poolVertexIndex)
{
    GE_DDGIPositionsF32 vb = GE_DDGIPositionsF32(mesh.VertexAddress);
    uint strideFloats = max(mesh.VertexStrideBytes / 4u, 3u);
    uint base = poolVertexIndex * strideFloats + mesh.UV0OffsetBytes / 4u;
    return vec2(vb.values[base + 0u], vb.values[base + 1u]);
}

// Barycentric-interpolated UV0 at a ray-query hit. `bary` is the pair the
// ray query reports (weights of vertices 1 and 2; vertex 0 takes the
// remainder). Returns (0,0) for a mesh with no UV0 stream — the caller's
// albedo layer is what decides whether that value is ever sampled.
vec2 GE_DDGITriangleUV(GE_MeshGeometryDesc mesh, uint primitiveIndex, vec2 bary)
{
    if (mesh.UV0OffsetBytes == GE_DDGI_NO_UV0)
        return vec2(0.0);
    uint i0 = GE_DDGIFetchIndex(mesh, primitiveIndex * 3u + 0u);
    uint i1 = GE_DDGIFetchIndex(mesh, primitiveIndex * 3u + 1u);
    uint i2 = GE_DDGIFetchIndex(mesh, primitiveIndex * 3u + 2u);
    vec2 uv0 = GE_DDGIFetchUV0(mesh, mesh.FirstVertex + i0);
    vec2 uv1 = GE_DDGIFetchUV0(mesh, mesh.FirstVertex + i1);
    vec2 uv2 = GE_DDGIFetchUV0(mesh, mesh.FirstVertex + i2);
    return uv0 * (1.0 - bary.x - bary.y) + uv1 * bary.x + uv2 * bary.y;
}

// Flat (per-triangle) object-space geometric normal for primitive
// `primitiveIndex` of `mesh`, transformed to world space by `normalMatrix`.
vec3 GE_DDGITriangleNormalWS(GE_MeshGeometryDesc mesh, uint primitiveIndex, mat3 normalMatrix)
{
    uint i0 = GE_DDGIFetchIndex(mesh, primitiveIndex * 3u + 0u);
    uint i1 = GE_DDGIFetchIndex(mesh, primitiveIndex * 3u + 1u);
    uint i2 = GE_DDGIFetchIndex(mesh, primitiveIndex * 3u + 2u);
    vec3 p0 = GE_DDGIFetchPosition(mesh, mesh.FirstVertex + i0);
    vec3 p1 = GE_DDGIFetchPosition(mesh, mesh.FirstVertex + i1);
    vec3 p2 = GE_DDGIFetchPosition(mesh, mesh.FirstVertex + i2);
    vec3 nOS = cross(p1 - p0, p2 - p0);
    float len = length(nOS);
    if (len < 1e-12)
        return vec3(0.0, 1.0, 0.0);  // degenerate triangle: arbitrary stable fallback, never NaN
    return normalize(normalMatrix * (nOS / len));
}

// NEE direct lighting at a hit point: loops the packed light array, shadow-
// tests each emitting light against `tlas`, and accumulates a Lambertian
// (albedo/PI) response. Bounded by `lightCount` (ge_ddgiLightHeader.x) — no
// clustering, since a world-space probe hit has no screen-space cluster to
// consult; every light in the scene is a candidate. Cost (rays x lights x
// probes) is the field the M4 "reactivity + budget" milestone is expected
// to tune.
vec3 GE_DDGIEvaluateDirectNEE(accelerationStructureEXT tlas, vec3 hitPosWS, vec3 hitNormalWS,
                              vec3 albedo, uint lightCount, float traceBiasWS)
{
    vec3 origin = hitPosWS + hitNormalWS * traceBiasWS;
    vec3 result = vec3(0.0);
    for (uint i = 0u; i < lightCount; ++i)
    {
        GE_DDGILightPacked lp = ge_ddgiLights[i];
        if (lp.meta.w == 0u)  // castsLight
            continue;
        uint type = lp.meta.x;  // 0=dir,1=point,2=spot,3=emitter proxy,4=area
        vec3 toLight;
        float dist = GE_DDGI_T_MAX;
        if (type == 0u)
        {
            toLight = normalize(-lp.dirIntensity.xyz);
        }
        else
        {
            vec3 delta = lp.posRange.xyz - hitPosWS;
            dist = length(delta);
            if (dist < 1e-5 || dist > lp.posRange.w)
                continue;
            toLight = delta / dist;
        }
        float ndotl = dot(hitNormalWS, toLight);
        if (ndotl <= 0.0)
            continue;
        if (type == 2u)  // spot cone — hard cutoff at the outer cone (spotParams.x); the
                        // inner/outer smoothstep blend the raster path applies is left for
                        // a later pass, a hard cutoff is still physically defensible for NEE
        {
            float cosAngle = dot(normalize(lp.dirIntensity.xyz), -toLight);
            if (cosAngle < lp.spotParams.x)
                continue;
        }

        // A type-3 emitter proxy's CENTER sits inside its own geometry, so a
        // shadow ray aimed at it is occluded by the emitter itself and every
        // sample is rejected — the surface goes black instead of lighting the
        // room. Stop the ray at the proxy's surface (areaParams.z is the source
        // radius) rather than its centre. Point and spot lights have no such
        // body and keep the centre-distance cutoff.
        float shadowT = (type == 0u) ? GE_DDGI_T_MAX : (dist - traceBiasWS * 2.0);
        if (type == 3u)
            shadowT = max(dist - lp.areaParams.z - traceBiasWS * 2.0, GE_DDGI_RAY_EPS);
        rayQueryEXT shadowRq;
        rayQueryInitializeEXT(shadowRq, tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT,
                              0xFFu, origin, GE_DDGI_RAY_EPS, toLight, shadowT);
        while (rayQueryProceedEXT(shadowRq)) { }
        if (rayQueryGetIntersectionTypeEXT(shadowRq, true) != gl_RayQueryCommittedIntersectionNoneEXT)
            continue;  // occluded

        float atten = (type == 0u)
            ? 1.0
            : GE_DDGILocalLightAttenuation(dist, lp.posRange.w, lp.areaParams.z, lp.areaUp.w);
        vec3 radiance = lp.colorAreaWidth.rgb * lp.dirIntensity.w * atten;
        result += (albedo / GE_DDGI_PI) * radiance * ndotl;
    }
    return result;
}

#endif // !GE_DDGI_SOFTWARE_TRACE

// ---------------------------------------------------------------------------
// Recursive bounce fetch — the "infinite bounce" term, shared verbatim by both
// trace kernels.
//
// Lives in THIS file rather than ddgi_common.glsl because it needs three
// bindings of its own, and this file is included by exactly the two trace
// kernels (verified: nothing else includes it), so declaring them here grows
// exactly the two descriptor layouts that need them. ddgi_common.glsl is
// included by every DDGI kernel — blend, upload, clear, classify, both glossy
// kernels and the forward consumer — and bindings there would grow six
// unrelated layouts.
//
// It is also hit shading: this is the second-and-later-bounce half of a hit
// point's outgoing radiance, the direct NEE term above being the first.
// ---------------------------------------------------------------------------

layout(set = 0, binding = 6) uniform sampler2D uPrevIrradianceAtlas;

// Depth moments (r = mean hit distance, g = mean of its square) from the SAME
// previous-frame upload as the irradiance atlas — the Chebyshev gather's input.
// b16: free in both kernels (hardware takes b0..b9 + b14/b15, software takes
// b2/b3 + b5..b14). A sampled texture, not a storage buffer, so the software
// kernel's SSBO count against the web/compat budget is unchanged.
layout(set = 0, binding = 16) uniform sampler2D uPrevDepthAtlas;

// Per-probe relocation offset (xyz) and classify active flag (w) —
// ddgi_classify.comp owns the only write. Read here for the gather's
// corner rejection, and by each kernel's main() for its own probe's offset.
layout(std430, set = 0, binding = 8) readonly buffer DDGIProbeStateRO
{
    vec4 ge_ddgiProbeState[];
};

// Previous-frame irradiance arriving at `posWS` from direction `normalWS` (the
// hit's geometric normal), gathered from the probe field exactly the way the
// forward consumer gathers it: 8-corner trilinear, each corner weighted by a
// Chebyshev visibility test against the depth moments toward the sample point
// and rejected outright when classified inactive, then normalized.
// GE_DDGISampleC0 (Includes/ddgi_probes.glsl) is the mirror; the two differ
// only in which bindings they read and in that this one applies no
// consume-time Intensity. `chebyshevStrength` must be the SAME value the
// consumer is given (both come from DDGIVolume::ChebyshevStrength) — the two
// gathers disagreeing on where light stops is exactly the drift this shared
// implementation exists to prevent. `classifyStrength` carries the same
// requirement for where a probe IS and whether it counts.
//
// The normalization is what keeps the recursive loop convergent: the weights
// are non-negative and divided by their own sum, so the result is a convex
// combination of atlas texels and can never exceed the largest of them. The
// per-cycle feedback gain therefore stays whatever the caller's clamped
// bounceAlbedo says it is (GE_DDGI_MAX_BOUNCE_ALBEDO), exactly as it did under
// the previous single-tap fetch. The all-corners-rejected case returns black
// rather than dividing by a near-zero denominator.
//
// The atlas holds E/PI (ddgi_blend.comp's estimator; ge_irradianceCube uses the
// same convention), which is what the caller's `bounceAlbedo * <this>` term
// expects — no PI factor belongs anywhere on this path.
//
// `depthOctRes` is the previous-frame depth atlas's interior tile resolution
// (DDGIVolumeParams.uGridMinWS.w) — see GE_DDGIDepthTexelUV.
vec3 GE_DDGISampleBounce(GE_DDGIGridInfo grid, vec3 posWS, vec3 normalWS,
                         float chebyshevStrength, float classifyStrength, int depthOctRes)
{
    // Push the lookup point off the hit surface before locating it in the
    // grid: a hit exactly on a wall otherwise indexes the probe embedded in
    // that wall, whose irradiance is meaningless (and, with classify running,
    // whose active flag is 0). Same bias, same units as the consumer's.
    vec3 biasedPos = posWS + normalWS * (grid.minCellWS * GE_DDGI_SURFACE_NORMAL_BIAS_CELL *
                                         grid.normalBiasScale);
    vec3 gridSpace = clamp(GE_DDGIWorldToGridSpace(grid, biasedPos), vec3(0.0),
                           vec3(grid.probeCount - ivec3(1)));
    ivec3 base = clamp(ivec3(floor(gridSpace)), ivec3(0), max(grid.probeCount - ivec3(2), ivec3(0)));
    vec3 frac = gridSpace - vec3(base);

    vec3 numerator = vec3(0.0);
    float denom = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        ivec3 offset = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        ivec3 coord = base + offset;
        float trilinear = 1.0;
        trilinear *= (offset.x == 1) ? frac.x : (1.0 - frac.x);
        trilinear *= (offset.y == 1) ? frac.y : (1.0 - frac.y);
        trilinear *= (offset.z == 1) ? frac.z : (1.0 - frac.z);
        if (trilinear <= 0.0)
            continue;

        int probeIdx = GE_DDGIProbeIndex(grid, coord);
        vec4 state = GE_DDGIProbeStateApply(ge_ddgiProbeState[probeIdx], classifyStrength);
        if (state.w <= 0.0)
            continue;  // classified inactive (buried) at full classify strength

        vec3 probePosWS = GE_DDGIProbePosition(grid, probeIdx) + state.xyz;
        vec3 toPoint = biasedPos - probePosWS;
        float distToPoint = length(toPoint);
        vec3 towardPoint = distToPoint > 1.0e-5 ? (toPoint / distToPoint) : normalWS;

        vec2 depthUV = GE_DDGIDepthTexelUV(grid, probeIdx, towardPoint, depthOctRes);
        vec2 moments = GE_DDGIBoundedMoments(textureLod(uPrevDepthAtlas, depthUV, 0.0));
        // Same weighting as the consumer's GE_DDGISampleC0 — the two gathers
        // must agree on how much a probe counts, or the bounce feeds the
        // field light the consumer would have rejected.
        float weight = GE_DDGIBackfaceWeight(-towardPoint, normalWS, chebyshevStrength) *
                       GE_DDGIVisibilityWeight(moments, distToPoint, chebyshevStrength,
                                               grid.minCellWS, depthOctRes);
        weight *= trilinear * state.w;

        vec2 irrUV = GE_DDGIProbeTexelUV(grid, probeIdx, normalWS);
        numerator += textureLod(uPrevIrradianceAtlas, irrUV, 0.0).rgb * weight;
        denom += weight;
    }
    if (denom <= 1.0e-4)
        return vec3(0.0);
    return numerator / denom;
}

// Miss term: the engine's pre-convolved diffuse irradiance cube — the same
// physical texture the forward pass's ambient term samples (Includes/ibl.glsl)
// — so a probe with an empty hemisphere converges toward the same sky
// contribution flat IBL alone would give. A direct cube sample (not
// GE_SampleEnvironmentIrradiance's ground-darkening/tint pipeline, which is
// bound to the forward pass's own descriptor set and Env UBO) — a documented
// M2 simplification; the two terms share the same source texture and will
// read the same to a first approximation.
// `scale` is DDGIVolume::SkyIntensity folded with the scene environment's
// global IblIntensity on the CPU (DDGIProbeFeature's SkyIntensityScale). The
// fold has to happen somewhere on this path: ibl.glsl applies the global
// intensity inside GE_SampleEnvironmentIrradiance, and DDGI's field REPLACES
// that call for the receiver, so a raw cube sample here would leave the
// scene's sky slider with no effect on anything DDGI lights.
vec3 GE_DDGISampleSkyMiss(vec3 rayDirWS, float scale)
{
    // Returns RADIANCE, which is what the ray buffer holds. The cube stores
    // E/PI (sky_diffuse_convolve.comp: cosine-weighted sampling makes the
    // unweighted sample mean an unbiased estimator of E/PI), and for a
    // uniform-radiance hemisphere E = PI*L, so E/PI is exactly L — the
    // stored value is already the radiance this needs. No scale here.
    return texture(ge_ddgiSkyIrradiance, normalize(rayDirWS)).rgb * max(scale, 0.0);
}

#endif // GE_DDGI_HIT_SHADE_GLSL
