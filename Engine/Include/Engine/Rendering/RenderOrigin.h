#pragma once

#include "Components/Rendering/WorldSectorCoord.h"
#include "Types/Types.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace GameEngine::Engine::Renderer
{

// Camera-relative rendering ("Earth-scale" precision). The render origin is an
// integer sector index (exact, large scale) plus an fp32 local remainder (fine
// scale) — a "manual double" for the one coordinate that must stay precise at
// planetary distance. The vertex stage reconstructs each instance's
// camera-relative position from a per-view origin uniform, so a moving camera
// re-uploads only the CameraUBO, never the persistent, change-tracked instance
// buffer (the #405/#490 quiescence law). Authoring space stays plain fp32
// Transforms; the sector is derived automatically from the camera each frame.
//
// GLSL mirror: Shaders/Includes/gpu_instance_fields.glsl (ge_UnpackInstanceSector)
// and Shaders/Includes/camera_ubo_fields.glsl (uRenderOriginSector). The pack /
// unpack round-trip is locked by RenderOriginTests.

// Meters per sector. THE single sector size, shared with the CPU composition
// path (Components::kWorldSectorSize — TLAS/picking) so a tagged entity renders
// where it picks/raycasts. Power of two so (integerSectorDelta * kSectorSize) is
// exact in fp32 up to the 2^24 mantissa ceiling, and the local remainder stays
// inside fp32's clean band (1 km => ~0.12 mm ULP).
inline constexpr float32 kSectorSize = Components::kWorldSectorSize;

// Camera distance from the world origin (max abs component) below which the
// render origin stays sector (0,0,0). Inside this radius the shader takes the
// full-world path unchanged, so every existing scene renders bit-for-bit as
// today (the dark-ship gate). The radius sits well inside fp32's clean band, so
// the activation crossing is sub-ULP and invisible; it only needs to be beyond
// any ordinary (non-planetary) scene's camera reach.
inline constexpr float32 kRenderOriginActivationRadius = 32768.0f; // 32 sectors

// Each sector axis is a 21-bit two's-complement integer: range
// [-2^20, 2^20-1] sectors => +/-1,048,576 * kSectorSize ~= +/-1.07e9 m
// (>1,000,000 km — planetary with vast margin). Three axes pack into the two
// reserved GPUInstance words (_pad0/_pad1) with no struct growth.
inline constexpr int32  kSectorAxisBits = 21;
inline constexpr uint32 kSectorAxisMask = (1u << kSectorAxisBits) - 1u; // 0x1FFFFF
inline constexpr int32  kSectorAxisMin  = -(1 << (kSectorAxisBits - 1));
inline constexpr int32  kSectorAxisMax  = (1 << (kSectorAxisBits - 1)) - 1;

// Pack three 21-bit sector axes into two 32-bit words.
//   p0: [0..20] X, [21..31] low 11 bits of Y
//   p1: [0..9]  high 10 bits of Y, [10..30] Z, [31] unused
// Out-of-range axes (|sector| > 2^20, i.e. beyond ~1.07 million km) are clamped
// rather than silently wrapped — a wrap would teleport the instance to the far
// side of the world. The assert catches it in dev; the clamp degrades to the
// edge of the representable range in release.
inline int32 ClampSectorAxis(int32 s)
{
    assert(s >= kSectorAxisMin && s <= kSectorAxisMax && "render-origin sector axis out of 21-bit range");
    return std::clamp(s, kSectorAxisMin, kSectorAxisMax);
}

inline void PackSector(int32 sx, int32 sy, int32 sz, uint32& p0, uint32& p1)
{
    const uint32 ux = static_cast<uint32>(ClampSectorAxis(sx)) & kSectorAxisMask;
    const uint32 uy = static_cast<uint32>(ClampSectorAxis(sy)) & kSectorAxisMask;
    const uint32 uz = static_cast<uint32>(ClampSectorAxis(sz)) & kSectorAxisMask;
    p0 = ux | ((uy & 0x7FFu) << 21);
    p1 = (uy >> 11) | (uz << 10);
}

// C++ mirror of the GLSL ge_UnpackInstanceSector sign-extending decode. Kept in
// lockstep with gpu_instance_fields.glsl; RenderOriginTests asserts the round-trip.
inline int32 SignExtendSectorAxis(uint32 v)
{
    const uint32 low = v & kSectorAxisMask;
    const uint32 signBit = 1u << (kSectorAxisBits - 1);
    return (low & signBit) ? static_cast<int32>(low | ~kSectorAxisMask)
                           : static_cast<int32>(low);
}

inline void UnpackSector(uint32 p0, uint32 p1, int32& sx, int32& sy, int32& sz)
{
    const uint32 ux = p0 & kSectorAxisMask;
    const uint32 uy = ((p0 >> 21) & 0x7FFu) | ((p1 & 0x3FFu) << 11);
    const uint32 uz = (p1 >> 10) & kSectorAxisMask;
    sx = SignExtendSectorAxis(ux);
    sy = SignExtendSectorAxis(uy);
    sz = SignExtendSectorAxis(uz);
}

// The render origin sector for a camera at (camX,camY,camZ) world meters.
// Returns (0,0,0) inside the activation radius (byte-identical full-world path);
// otherwise the camera's nearest sector, so the reconstructed local remainder
// stays within +/-kSectorSize/2. Stateless — a hard threshold whose crossing is
// invisible, so no hysteresis is needed.
// GE_ES_FORCE_WORLD_SPACE=1 pins the render origin to (0,0,0) at any distance —
// the full-world, pre-camera-relative path. The feature kill switch: a dark-ship
// A/B lane (prove the origin-inactive path is byte-identical) and the acne
// before/after repro (forced world-space reintroduces the self-shadow speckle at
// planetary distance that the rebased path removes). Read once at first use so the
// per-view/per-frame call cost stays nil; default (unset) => normal behavior.
inline bool IsRenderOriginForcedInactive()
{
    static const bool s_Forced = []()
    {
        const char* env = std::getenv("GE_ES_FORCE_WORLD_SPACE");
        return env != nullptr && std::strcmp(env, "0") != 0;
    }();
    return s_Forced;
}

inline Components::WorldSectorCoord ComputeRenderOriginSector(float32 camX, float32 camY, float32 camZ)
{
    if (IsRenderOriginForcedInactive())
        return Components::WorldSectorCoord{0, 0, 0};
    // A NaN/Inf camera (upstream bug) must NOT drive an active garbage origin —
    // lround of NaN is UB and the int cast produces nonsense. Degrade to sector 0
    // (the full-world path), i.e. exactly the pre-feature behavior.
    if (!std::isfinite(camX) || !std::isfinite(camY) || !std::isfinite(camZ))
    {
        assert(false && "render-origin camera position is not finite");
        return Components::WorldSectorCoord{0, 0, 0};
    }
    const float32 maxComp = std::max({std::fabs(camX), std::fabs(camY), std::fabs(camZ)});
    if (maxComp <= kRenderOriginActivationRadius)
        return Components::WorldSectorCoord{0, 0, 0};
    // Clamp as long BEFORE the int32 cast so a huge (but finite) camera can't
    // overflow the cast into a garbage sector.
    const auto axis = [](float32 c) -> int32 {
        const long s = std::lround(c / kSectorSize);
        return static_cast<int32>(std::clamp(s, static_cast<long>(kSectorAxisMin),
                                             static_cast<long>(kSectorAxisMax)));
    };
    return Components::WorldSectorCoord{axis(camX), axis(camY), axis(camZ)};
}

// World-space position of a sector's origin (sector * kSectorSize). Exact in
// fp32 while |sector| * kSectorSize < 2^24 (|sector| < 16384, i.e. within
// ~16,777 km); beyond that the low bits round, but the shader differences the
// integer sectors before scaling, so instance reconstruction stays exact
// regardless of absolute magnitude.
inline void SectorToWorld(const Components::WorldSectorCoord& s, float32& x, float32& y, float32& z)
{
    x = static_cast<float32>(s.x) * kSectorSize;
    y = static_cast<float32>(s.y) * kSectorSize;
    z = static_cast<float32>(s.z) * kSectorSize;
}

// The #660 double-rebase core, shared by the camera (ComputeRebasedView) and
// the cascade-shadow VP build (ComputeRebasedOrthoLightVP): rebuild a view
// matrix's translation from its own 3x3 and the eye position, with the origin
// exact in double from the integer sector and (eye − origin) a difference of
// fp32-representable values — exact in double. outT = −R3x3·(eye − origin),
// kept in double; the caller rounds to fp32 once, on store. The legacy fp32
// column rebase (translation + M3x3·origin, a big+big sum whose small result
// inherits ULP(|origin|) rounding) re-quantized the relative translation every
// frame the matrices changed — mm wobble at |origin| 5e4, meters at Earth
// radius.
inline void ComputeRebasedViewTranslation(const float32 view[16], float32 eyeX, float32 eyeY,
                                          float32 eyeZ, const int32 sector[3], float64 outT[3])
{
    const float64 sectorSize = static_cast<float64>(kSectorSize);
    const float64 lx = static_cast<float64>(eyeX) - static_cast<float64>(sector[0]) * sectorSize;
    const float64 ly = static_cast<float64>(eyeY) - static_cast<float64>(sector[1]) * sectorSize;
    const float64 lz = static_cast<float64>(eyeZ) - static_cast<float64>(sector[2]) * sectorSize;
    outT[0] = -(static_cast<float64>(view[0]) * lx + static_cast<float64>(view[4]) * ly +
                static_cast<float64>(view[8]) * lz);
    outT[1] = -(static_cast<float64>(view[1]) * lx + static_cast<float64>(view[5]) * ly +
                static_cast<float64>(view[9]) * lz);
    outT[2] = -(static_cast<float64>(view[2]) * lx + static_cast<float64>(view[6]) * ly +
                static_cast<float64>(view[10]) * lz);
}

// Cascade-shadow twin of ComputeRebasedView (the #660 shadow follow-up): build
// the render-origin-relative light view-projection in double from the fit's
// rotation-only view (rotView — translation ignored), its (orthographic)
// projection, and the shadow-camera eye. The full-world composition is never
// touched when the origin is active: at planetary |eye| its translation column
// (magnitude |eye|/extent in clip units) carries ULP(big) storage rounding
// coarser than a shadow texel, so any rebase THROUGH it — fp32 or double —
// inherits texel-scale wobble. Here every term stays small: (eye − origin)
// exact in double, viewRel = [rotView3x3 | −R·(eye − origin)], outVPRel =
// proj · viewRel accumulated in double, one fp32 rounding per element on
// store. With the origin inactive (sector 0,0,0) outVPRel is a byte-for-byte
// copy of worldVP — the dark-ship identity.
inline void ComputeRebasedOrthoLightVP(const float32 rotView[16], const float32 proj[16],
                                       const float32 worldVP[16], float32 eyeX, float32 eyeY,
                                       float32 eyeZ, const int32 sector[3], float32 outVPRel[16])
{
    for (int32 i = 0; i < 16; ++i)
        outVPRel[i] = worldVP[i];
    if (sector[0] == 0 && sector[1] == 0 && sector[2] == 0)
        return; // dark-ship identity: bit-for-bit copy

    float64 t[3];
    ComputeRebasedViewTranslation(rotView, eyeX, eyeY, eyeZ, sector, t);

    float64 viewRel[16];
    for (int32 i = 0; i < 12; ++i)
        viewRel[i] = static_cast<float64>(rotView[i]);
    viewRel[12] = t[0];
    viewRel[13] = t[1];
    viewRel[14] = t[2];
    viewRel[15] = static_cast<float64>(rotView[15]);

    for (int32 c = 0; c < 4; ++c)
    {
        for (int32 r = 0; r < 4; ++r)
        {
            float64 acc = 0.0;
            for (int32 k = 0; k < 4; ++k)
                acc += static_cast<float64>(proj[k * 4 + r]) * viewRel[c * 4 + k];
            outVPRel[c * 4 + r] = static_cast<float32>(acc);
        }
    }
}

// Re-anchor a RETAINED render-origin-relative matrix across a sector step:
// out = m ∘ translate((to − from) · kSectorSize), the delta exact in double
// from the integer sectors and the column accumulated in double. This is the
// cascade motion round-robin's deferral-window absorption — the retained
// layer's rel fit keyed to the origin it rasterized under (`from`) re-anchored
// to the CURRENT origin (`to`) without ever reconstituting a world-magnitude
// translation. Writes only the translation column (12..15); rotation/scale
// columns must already be copied into `out`. from == to is the caller's plain
// copy (bit-for-bit) — don't call for it.
inline void RebaseTranslationColumnBySectorDelta(const float32 m[16], const int32 from[3],
                                                 const int32 to[3], float32 out[16])
{
    const float64 size = static_cast<float64>(kSectorSize);
    const float64 dx = static_cast<float64>(to[0] - from[0]) * size;
    const float64 dy = static_cast<float64>(to[1] - from[1]) * size;
    const float64 dz = static_cast<float64>(to[2] - from[2]) * size;
    for (int32 r = 0; r < 4; ++r)
    {
        const float64 acc = static_cast<float64>(m[0 * 4 + r]) * dx +
                            static_cast<float64>(m[1 * 4 + r]) * dy +
                            static_cast<float64>(m[2 * 4 + r]) * dz +
                            static_cast<float64>(m[12 + r]);
        out[12 + r] = static_cast<float32>(acc);
    }
}

// Fill a CameraData's camera-relative fields (viewRel / viewProjRel / sector)
// from its full-world view + proj + viewProj + camera position. The rebased
// matrices consume render-origin-relative positions (view * translate(origin));
// the vertex stage projects reconstructed relative positions through them. When
// the camera sits inside the activation radius the sector is (0,0,0), the origin
// is zero, and viewRel/viewProjRel are copied bit-for-bit from view/viewProj —
// so the shader's full-world path renders exactly as before the feature.
//
// Origin-ACTIVE precision: the old fp32 rebase (translation + M3x3*origin, a
// big+big sum whose small result inherits ULP(|eye|) rounding) re-quantized the
// relative translation every frame the rotation changed — a ~4-8 mm wobble of
// the whole rebased world at |eye| 5e4 and ~0.5-1 m at Earth radius, visible as
// shimmer during interactive camera rotation. Every view the engine builds maps
// the camera position to the view-space origin, so the stored translation is
// -M3x3*eye and the rebased one is exactly -M3x3*(eye - origin). (eye - origin)
// is a difference of fp32-representable values — exact in double — so the
// column is rebuilt in double from the matrix's own 3x3 and rounds to fp32
// once, on store. viewProjRel is then proj * viewRel accumulated in double.
inline void ComputeRebasedView(const float32 view[16], const float32 proj[16],
                               const float32 viewProj[16],
                               float32 camX, float32 camY, float32 camZ,
                               float32 outViewRel[16], float32 outViewProjRel[16],
                               int32 outSector[4])
{
    const Components::WorldSectorCoord origin = ComputeRenderOriginSector(camX, camY, camZ);
    outSector[0] = origin.x;
    outSector[1] = origin.y;
    outSector[2] = origin.z;
    outSector[3] = static_cast<int32>(kSectorSize);

    for (int32 i = 0; i < 16; ++i)
    {
        outViewRel[i] = view[i];
        outViewProjRel[i] = viewProj[i];
    }
    if (origin.x == 0 && origin.y == 0 && origin.z == 0)
        return; // dark-ship identity: bit-for-bit copies

    // Origin in double straight from the integer sector: exact for any 21-bit
    // sector (fp32 SectorToWorld rounds past |sector*size| = 2^24, ~16,777 km).
    float64 t[3];
    ComputeRebasedViewTranslation(view, camX, camY, camZ, outSector, t);
    outViewRel[12] = static_cast<float32>(t[0]);
    outViewRel[13] = static_cast<float32>(t[1]);
    outViewRel[14] = static_cast<float32>(t[2]);
    outViewRel[15] = view[15];

    for (int32 c = 0; c < 4; ++c)
    {
        for (int32 r = 0; r < 4; ++r)
        {
            float64 acc = 0.0;
            for (int32 k = 0; k < 4; ++k)
                acc += static_cast<float64>(proj[k * 4 + r]) *
                       static_cast<float64>(outViewRel[c * 4 + k]);
            outViewProjRel[c * 4 + r] = static_cast<float32>(acc);
        }
    }
}

} // namespace GameEngine::Engine::Renderer
