#pragma once

// CBTDeepDecode.h — Earth-scale decode-precision prototype (arc slice S1, gated by
// GE_CBT_DEEP_DECODE, default OFF).
//
// The problem this solves: gVertex stores fp32 WORLD corners. At Earth radius
// (6.371e6 m, fp32 octave [2^22, 2^23)) a world component quantizes on a 0.5 m grid at
// STORE time — before the #524 camera-relative origin subtraction can help — so facets
// below a few metres are unrepresentable no matter how exact the int64 barycentric walk
// is. kMaxDecodeSubdiv (40) guards ~2x short of that degeneracy and floors the Earth
// facet at ~9.5 m; the 0.5 m walking bar is unreachable under fp32-world storage.
//
// The cure prototyped here: decode each corner to (integer SECTOR on the 1024 m world
// grid, fp32 LOCAL offset) — the SAME representation instanced meshes already use for
// Earth-scale (camera_relative.glsl GE_ClipFromSectorLocal; Components::WorldSectorCoord).
// Local magnitude is <= 512·√3 m, so its fp32 ULP is ~6e-5 m; the sector delta to the
// render origin is EXACT integer arithmetic. Consumption becomes
//   rel = float(sector - originSector) * 1024 + local
// which is the mesh path's math verbatim — the CBT draw stops being the one Earth-scale
// consumer that cannot compose with the render origin at full precision.
//
// The decode arithmetic is df64 ("double-float": an unevaluated sum of two fp32 values,
// Dekker/Knuth error-free transforms, ~2^-47 relative) — NOT fp64. Rationale (design §5):
// shaderFloat64 is absent from our device init, 1/16–1/64 rate on consumer GPUs, and
// VK_FALSE on MoltenVK/Metal; df64 is plain fp32 ALU + fma(), portable everywhere the
// CBT already runs. At R = 6.371e6 the df64 decode error is ~5e-8 m — five orders below
// the 6e-5 m local-storage ULP, so the representation, not the arithmetic, is the limit.
//
// Determinism (the crack-freedom argument, extending the #474 proof structure): every
// function here is a pure function of the corner's exact int64 cube numerator N (and
// power-of-two Scale). Two bisectors sharing a vertex hold bit-identical (N, Scale) —
// or (2N, 2·Scale) across one LEB level, and every df64 op is exactly invariant under
// power-of-two input scaling — so both decode bit-identical (Sector, Local) and the
// reconstruction cannot crack. Validated by CBTDeepDecodeTests.SharedCornerDeterminism.
//
// GPU lockstep (arc slice S2a — LIVE): Shaders/cbt_deep_decode.glsl is the GLSL twin of
// this header, edited in LOCKSTEP (the CBTLayout.h <-> cbt_layout.glsl discipline; locked
// by CBTLayoutTests.GlslDeepDecodeMirrorsCpp and bit-for-bit by the GPU readback probe
// CBTDeepDecodeGpu.GpuDecodeMatchesCpuLibraryBitExact). The error-free transforms REQUIRE
// uncontracted fp32 (an auto-fused a*b+c breaks TwoSum/TwoProd): the GLSL twin marks every
// op `precise` (SPIR-V NoContraction), and this header disables contraction below for the
// same reason. CPU<->GPU bit-parity is a HELD gate, not just desirable: every fp32 op in
// the decode is IEEE-correctly-rounded on both sides (add/sub/mul/fma; the one exception
// Vulkan permits — division/inversesqrt — is avoided: the 1/sqrt seed is the shared
// deterministic bit-pattern seed in RSqrtSeed below, and the only division, invScale, is an
// exact power of two), and the sector split uses the floor(x + 0.5) tie convention (GLSL
// round()'s half-tie is implementation-defined; std::roundf's half-away differs at negative
// ties — both replaced by the shared floor form).

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "CBTTerrain/CBTLayout.h"      // kMaxDecodeSubdiv
#include "CBTTerrain/CBTSphereRoots.h" // CBTSphereRoot (exact integer root corners)

// The error-free transforms below are correct ONLY without FP contraction (see header
// note). MSVC: float_control(push) saves the includer's fp_contract state so this
// header cannot leak a contraction change into the including TU; GCC/Clang honor the
// STDC pragma (leaking OFF there is conservative — stricter, never wrong). The
// Df64ArithmeticHoldsExtendedPrecision probe fails loudly if contraction ever breaks
// the transforms regardless of pragma behavior.
#if defined(_MSC_VER)
#pragma float_control(push)
#pragma fp_contract(off)
#else
#pragma STDC FP_CONTRACT OFF
#endif

namespace GameEngine::CBTTerrain::DeepDecode
{

// Deep-prototype subdivision cap. 50 lands the Earth facet floor at
// (pi·6.371e6/2)·2^-25 = 0.298 m — under the 0.5 m walking bar — while staying 2 below
// the int64 walk-exactness ceiling (~52: Scale = 2^52 < 2^63; design §4). The heap key
// itself (u64, findMSB-bounded) holds depth ~58 and is NOT the binding limit.
inline constexpr uint32_t kDeepDecodeSubdiv = 50u;

// World sector grid (metres). Mirror of camera_relative.glsl GE_SECTOR_SIZE /
// Components::kWorldSectorCoord's 1024 — the render-origin machinery's intrinsic
// constant. Deliberately a power of two: dividing/multiplying by it is exact in fp32.
inline constexpr float kSectorSizeMeters = 1024.0f;

// The env-gated cap the arc's probes exercise: kMaxDecodeSubdiv (the shipped 40) unless
// GE_CBT_DEEP_DECODE is set truthy. Read per call, uncached, so tests can toggle the
// flag in-process. The LIVE pipeline does not read this: CBTUpdateSystem reads the same
// env var once (static-cached) and feeds ONE bool into both CBTClassifyDesc::DeepDecode
// and ResolveTerrainMaxDepth (TerrainProvisioning SubdivCapFor — the S2b flag->cap
// coupling), so storage mode and depth cap cannot desync mid-session.
inline uint32_t SubdivCap()
{
    if (const char* v = std::getenv("GE_CBT_DEEP_DECODE"))
        if (v[0] != '\0' && v[0] != '0')
            return kDeepDecodeSubdiv;
    return kMaxDecodeSubdiv;
}

// ---------------------------------------------------------------------------
// Exact integer LEB walk (CPU twin of Kernel_VertexEval's int64 walk)
// ---------------------------------------------------------------------------
struct Bary
{
    int64_t B0[3]; // corner 0 weights w.r.t. (rootV0, rootV1, rootV2), sum = Scale
    int64_t B1[3];
    int64_t B2[3];
    int64_t Scale; // 2^numSubdiv
};

// Walks the HeapID path bits exactly as cbt_kernels.comp Kernel_VertexEval does
// (bit 0: child = (v2, mid, v1); bit 1: child = (v1, mid, v0); mid = (b0+b2)/2, always
// an exact even halving). Exact to numSubdiv 52 (Scale < 2^63).
inline Bary WalkBary(uint64_t heapID, uint32_t baseDepth)
{
    uint32_t depth = 0u;
    for (uint64_t h = heapID; h > 1u; h >>= 1)
        ++depth;
    const uint32_t numSubdiv = depth - baseDepth;
    const int64_t scale = int64_t(1) << numSubdiv;

    int64_t b0[3] = {scale, 0, 0};
    int64_t b1[3] = {0, scale, 0};
    int64_t b2[3] = {0, 0, scale};
    for (uint32_t s = numSubdiv; s > 0u; --s)
    {
        const uint32_t bit = static_cast<uint32_t>(heapID >> (s - 1u)) & 1u;
        int64_t mid[3];
        for (int c = 0; c < 3; ++c)
            mid[c] = (b0[c] + b2[c]) / 2;
        if (bit == 0u)
        {
            for (int c = 0; c < 3; ++c)
            {
                const int64_t nb0 = b2[c], nb2 = b1[c];
                b0[c] = nb0;
                b1[c] = mid[c];
                b2[c] = nb2;
            }
        }
        else
        {
            for (int c = 0; c < 3; ++c)
            {
                const int64_t nb0 = b1[c], nb2 = b0[c];
                b0[c] = nb0;
                b1[c] = mid[c];
                b2[c] = nb2;
            }
        }
    }
    Bary out{};
    for (int c = 0; c < 3; ++c)
    {
        out.B0[c] = b0[c];
        out.B1[c] = b1[c];
        out.B2[c] = b2[c];
    }
    out.Scale = scale;
    return out;
}

// One corner's exact cube numerator N = bary·rootCorners (|N| <= Scale; root corners are
// the integers {-1,0,+1}). N/Scale is the exact cube-face position. This is the ONLY
// geometric input the decode consumes — the determinism anchor.
struct CubeNum
{
    int64_t N[3];
    int64_t Scale;
};

inline CubeNum CornerNumerator(const Bary& bary, int corner, const CBTSphereRoot& root)
{
    const int64_t* b = corner == 0 ? bary.B0 : corner == 1 ? bary.B1 : bary.B2;
    CubeNum out{};
    for (int c = 0; c < 3; ++c)
        out.N[c] = b[0] * root.V0[c] + b[1] * root.V1[c] + b[2] * root.V2[c];
    out.Scale = bary.Scale;
    return out;
}

// ---------------------------------------------------------------------------
// df64 — double-float arithmetic on fp32 (Dekker/Knuth error-free transforms)
// ---------------------------------------------------------------------------
struct DF
{
    float Hi;
    float Lo; // unevaluated sum Hi + Lo, |Lo| <= 0.5 ulp(Hi)
};

// Knuth TwoSum: s + err == a + b exactly. Branch-free (GLSL-portable).
inline DF TwoSum(float a, float b)
{
    const float s = a + b;
    const float bb = s - a;
    const float err = (a - (s - bb)) + (b - bb);
    return {s, err};
}

// Dekker FastTwoSum: requires |a| >= |b|.
inline DF FastTwoSum(float a, float b)
{
    const float s = a + b;
    return {s, b - (s - a)};
}

// TwoProd via FMA: p + err == a * b exactly (GLSL: fma(a, b, -p)).
inline DF TwoProd(float a, float b)
{
    const float p = a * b;
    const float err = std::fmaf(a, b, -p);
    return {p, err};
}

inline DF Add(DF a, DF b)
{
    DF s = TwoSum(a.Hi, b.Hi);
    s.Lo += a.Lo + b.Lo;
    return FastTwoSum(s.Hi, s.Lo);
}

inline DF Mul(DF a, DF b)
{
    DF p = TwoProd(a.Hi, b.Hi);
    p.Lo += a.Hi * b.Lo + a.Lo * b.Hi;
    return FastTwoSum(p.Hi, p.Lo);
}

inline DF MulF(DF a, float b)
{
    DF p = TwoProd(a.Hi, b);
    p.Lo += a.Lo * b;
    return FastTwoSum(p.Hi, p.Lo);
}

// int64 -> df64. Not exact past 48 bits (fp32 holds 24; the residual may round), but
// deterministic and exactly power-of-two-scale invariant — which is what crack-freedom
// needs (header note). Relative error <= ~2^-47.
inline DF FromI64(int64_t v)
{
    const float hi = static_cast<float>(v);
    const float lo = static_cast<float>(v - static_cast<int64_t>(hi));
    return {hi, lo};
}

// Deterministic shared 1/sqrt seed (S2a): hardware rsqrt estimates differ CPU<->GPU (and
// Vulkan only bounds inversesqrt to 2 ULP), so the seed is the classic exponent-halving
// bit pattern + three fp32 Newton polishes — pure IEEE add/mul, bit-identical on every
// IEEE fp32 implementation. Seed error ~3.4% squares down each step (~1.7e-3 -> ~4.6e-6
// -> fp32 rounding floor ~1 ulp), so the df64 Newton in RSqrt lands the same ~2^-44 the
// old hardware-seeded path measured — but now bit-portable (gate: the GPU readback probe).
inline float RSqrtSeed(float x)
{
    float r = std::bit_cast<float>(0x5F3759DFu - (std::bit_cast<uint32_t>(x) >> 1));
    r = r * (1.5f - 0.5f * x * r * r);
    r = r * (1.5f - 0.5f * x * r * r);
    r = r * (1.5f - 0.5f * x * r * r);
    return r;
}

// df64 1/sqrt: shared deterministic fp32 seed + one df64 Newton step (~2^-44 relative).
// GLSL twin: CBT_DfRSqrt (cbt_deep_decode.glsl), identical ops, all `precise`.
inline DF RSqrt(DF s)
{
    const DF rDf{RSqrtSeed(s.Hi), 0.0f};
    // df64 Newton: r' = r * (1.5 - 0.5 * s * r * r)
    const DF r2 = Mul(rDf, rDf);
    const DF sr2 = Mul(s, r2);
    const DF half = Add(DF{1.5f, 0.0f}, MulF(sr2, -0.5f));
    return Mul(rDf, half);
}

// ---------------------------------------------------------------------------
// The deep decode: exact numerator -> (sector, local)
// ---------------------------------------------------------------------------
// One decoded corner: integer world sector (1024 m grid) + fp32 sector-local offset.
// |Local| <= 512 (+0.5 ulp), so Local's ULP is ~6.1e-5 m at any planet radius. The
// reconstruction contract is ReconstructRel below — the GE_ClipFromSectorLocal math.
struct DeepVertex
{
    int32_t Sector[3];
    float Local[3];
};

// dir = N/|N| in df64, world = dir * (radius + relief) in df64, then split each
// component into (round-to-nearest sector, local remainder). Pure function of
// (N, Scale, radius, relief) — see the determinism note in the header.
inline DeepVertex DecodeCorner(const CubeNum& num, float radiusMeters, float reliefMeters)
{
    const DF nx = FromI64(num.N[0]);
    const DF ny = FromI64(num.N[1]);
    const DF nz = FromI64(num.N[2]);
    // |N|^2 in df64. |N| <= Scale <= 2^50 -> N^2 <= 2^100 exceeds fp32 EXPONENT range!
    // Pre-scale by invScale (exact power of two) first: c = N * 2^-numSubdiv in [-1,1]^3.
    const float invScale = 1.0f / static_cast<float>(num.Scale); // exact: Scale = 2^k
    const DF cx = MulF(nx, invScale);
    const DF cy = MulF(ny, invScale);
    const DF cz = MulF(nz, invScale);
    const DF len2 = Add(Add(Mul(cx, cx), Mul(cy, cy)), Mul(cz, cz));
    const DF rlen = RSqrt(len2);
    const DF scaleR = Mul(rlen, TwoSum(radiusMeters, reliefMeters)); // (R + relief) exact sum
    const DF w[3] = {Mul(cx, scaleR), Mul(cy, scaleR), Mul(cz, scaleR)};

    DeepVertex out{};
    constexpr float kInvSector = 1.0f / kSectorSizeMeters; // exact power of two
    for (int c = 0; c < 3; ++c)
    {
        // floor(x + 0.5) is the shared CPU/GPU tie convention (GLSL round()'s half-tie is
        // implementation-defined; roundf's half-away differs at negative ties). x + 0.5 is
        // exact while |x| < 2^22 (sector magnitude ~6.2e3 at Earth — vast margin).
        const float k = std::floor(w[c].Hi * kInvSector + 0.5f);
        const DF local = Add(w[c], DF{-k * kSectorSizeMeters, 0.0f}); // k*1024 exact (< 2^23)
        out.Sector[c] = static_cast<int32_t>(k);
        out.Local[c] = local.Hi + local.Lo; // |local| <= 512 -> single fp32 holds it to ~6e-5 m
    }
    return out;
}

// The consumption math (mirror of camera_relative.glsl GE_ClipFromSectorLocal): exact
// integer sector delta -> fp32, times the sector size, plus the local offset. Near the
// render origin the delta is small, so the result is fp32-precise at any planet radius.
inline void ReconstructRel(const DeepVertex& v, const int32_t originSector[3], float out[3])
{
    for (int c = 0; c < 3; ++c)
        out[c] = static_cast<float>(v.Sector[c] - originSector[c]) * kSectorSizeMeters + v.Local[c];
}

// Double-precision reference decode (probes only — the "true" geometry the fp32/df64
// paths are measured against; never a GPU candidate).
inline void DecodeExact(const CubeNum& num, double radiusMeters, double reliefMeters, double out[3])
{
    const double s = static_cast<double>(num.Scale);
    const double x = static_cast<double>(num.N[0]) / s;
    const double y = static_cast<double>(num.N[1]) / s;
    const double z = static_cast<double>(num.N[2]) / s;
    const double len = std::sqrt(x * x + y * y + z * z);
    const double r = (radiusMeters + reliefMeters) / len;
    out[0] = x * r;
    out[1] = y * r;
    out[2] = z * r;
}

} // namespace GameEngine::CBTTerrain::DeepDecode

#if defined(_MSC_VER)
#pragma float_control(pop)
#endif
