// cbt_domain.glsl — the CBT domain adapter (plan §8 C7). The LEB subdivision is
// domain-agnostic: it walks the HeapID path bits and produces, per triangle corner,
// an INTEGER barycentric weight triple (bx, by, bz) w.r.t. the corner's ROOT triangle,
// summing to scale = 2^numSubdiv. That walk (in cbt_kernels.comp Kernel_VertexEval)
// is shared by every domain. What differs per domain is only the mapping from
// (rootIndex, barycentric weights) -> world position — and that mapping lives HERE.
//
// Two domains, selected at RUNTIME by pc.domainMode (NOT #ifdef — one compiled kernel
// serves planar and spherical terrains; a terrain picks its domain via the Terrain
// component's Domain field):
//   PLANAR    — the pre-C7 behavior. Barycentric weights map to a unit-square UV via
//               the root's even/odd corners; UV -> world XZ + height displacement.
//   SPHERICAL — cube-sphere. 6 cube faces, each split into 4 pie-slice roots
//               (corner, faceCentroid, corner). Barycentric weights map to a cube-face
//               position; normalize -> sphere direction; displace along it from a base
//               radius. Cross-face conformity is native: a pie-slice's SPLIT edge is a
//               cube edge, and its twin is the pie-slice on the adjacent face sharing
//               that edge (see CBTSphereRoots.h) — so the compatibility chain that
//               conforms twins conforms across faces from depth 1.
//
// Crack-free decode (the C7 precision argument, plan §8 C7; int64 widening in slice 1): the
// barycentric weights are exact integers, and the cube/UV numerator N = bary·corners is
// formed as an EXACT int64 BEFORE any float cast (cube corners are the integers {-1,0,+1};
// the face centroid is an integer face-centre point; planar corners {0,1}). Two triangles
// meeting at a shared vertex therefore compute a BIT-IDENTICAL integer N -> the single
// vec3(N)/vec2(N) cast rounds identically on both sides -> bit-identical normalize() ->
// bit-identical world vertex. Cracks are impossible regardless of the fp32 magnitude of the
// radius (the projection is a deterministic function of the exact integer). This now holds
// past numSubdiv 23 (below which bary < 2^24 kept the float sum exact WITHOUT int64) up to
// the fp32 world-storage degeneracy ULP (~42, radius-independent); the Classify cap
// CBT_MAX_NUM_SUBDIV (40) stops ~2x short of it, taking the min planet facet edge from ~7 m
// to ~9 mm at R=20 km (decode-precision design, 2026-07).
//
// int64: the shared barycentric walk and the numerator N are int64 (i64vec3). HeapID (u64)
// is still decoded in Kernel_VertexEval before this include is reached. The final normalize +
// world-corner storage stay fp32 (Metal-portable — no shaderFloat64; sub-mm / Earth-scale is
// a documented storage-upgrade follow-up, not a conformity risk).
#ifndef CBT_DOMAIN_GLSL
#define CBT_DOMAIN_GLSL

// Domain selector (mirror kDomainPlanar / kDomainSpherical in CBTLayout.h).
const uint CBT_DOMAIN_PLANAR = 0u;
const uint CBT_DOMAIN_SPHERICAL = 1u;

// Cube-sphere base: 6 faces x 4 pie-slice roots = 24. baseDepth = ceil(log2(24)) = 5
// (2^5 = 32 >= 24), roots at heapID 32 + rootIndex. Mirror kCubeFaceCount /
// kSlicesPerFace / kSphereRootCount / kSphereBaseDepth in CBTSphereRoots.h.
const uint CBT_CUBE_FACE_COUNT = 6u;
const uint CBT_SLICES_PER_FACE = 4u;
const uint CBT_SPHERE_ROOT_COUNT = CBT_CUBE_FACE_COUNT * CBT_SLICES_PER_FACE; // 24

// 8 cube corners at (+/-1, +/-1, +/-1), indexed by bit pattern (bit0=x, bit1=y, bit2=z).
vec3 CBT_CubeCorner(int idx)
{
    return vec3((idx & 1) != 0 ? 1.0 : -1.0,
                (idx & 2) != 0 ? 1.0 : -1.0,
                (idx & 4) != 0 ? 1.0 : -1.0);
}

// 6 faces, 4 corner indices each, wound CCW as seen from OUTSIDE (outward normal by
// the right-hand rule). MUST match kCubeFaces in CBTSphereRoots.h exactly — the C++
// side builds the neighbor topology from this table and the GPU decodes geometry from
// it, so a divergence would put geometry and topology on different meshes.
const int CBT_CUBE_FACES[24] = int[24](
    1, 3, 7, 5,  // +X
    0, 4, 6, 2,  // -X
    2, 6, 7, 3,  // +Y
    0, 1, 5, 4,  // -Y
    4, 5, 7, 6,  // +Z
    0, 2, 3, 1   // -Z
);

// Pie-slice root corners: root r = face*4 + edge, corners (v0 = Q[edge],
// v1 = face centroid = apex, v2 = Q[(edge+1)%4]). The SPLIT edge is (v0, v2) — a cube
// edge — and v1 is the apex opposite it, matching the LEB convention (longest edge
// bisected, apex opposite). Centroid of an axis-aligned cube face is an integer point
// (the face centre, e.g. (1,0,0)), keeping the whole decode integer-exact.
void CBT_SphereRootCorners(uint rootIndex, out vec3 c0, out vec3 c1, out vec3 c2)
{
    uint f = rootIndex / CBT_SLICES_PER_FACE;
    uint e = rootIndex - f * CBT_SLICES_PER_FACE;
    vec3 q0 = CBT_CubeCorner(CBT_CUBE_FACES[f * CBT_SLICES_PER_FACE + 0u]);
    vec3 q1 = CBT_CubeCorner(CBT_CUBE_FACES[f * CBT_SLICES_PER_FACE + 1u]);
    vec3 q2 = CBT_CubeCorner(CBT_CUBE_FACES[f * CBT_SLICES_PER_FACE + 2u]);
    vec3 q3 = CBT_CubeCorner(CBT_CUBE_FACES[f * CBT_SLICES_PER_FACE + 3u]);
    vec3 faceCenter = (q0 + q1 + q2 + q3) * 0.25; // 'centroid' is a reserved GLSL keyword
    c0 = CBT_CubeCorner(CBT_CUBE_FACES[f * CBT_SLICES_PER_FACE + e]);
    c1 = faceCenter;
    c2 = CBT_CubeCorner(CBT_CUBE_FACES[f * CBT_SLICES_PER_FACE + ((e + 1u) & 3u)]);
}

// Planar: integer barycentric weights -> unit-square UV (== the LEB corner coord).
// Compute-only (slice 1): the barycentric decode is int64, so guard the two decode functions
// on CBT_DECODE_INT64 (defined by cbt_layout.glsl's int64 block). Only the compute kernels include
// cbt_layout.glsl, so only they define it and get i64vec. The surface FRAGMENT includes this file
// but NOT cbt_layout.glsl (and has no int64 extension), so CBT_DECODE_INT64 is undefined there and
// these functions are skipped — the graphics stages read the precomputed gVertex corners rather
// than re-decoding. Guarding on the extension's own GL_EXT_..._int64 macro does NOT work: glslang
// predefines it even when the extension is not enabled. This avoids the plan §9 null-variant trap.
//
// The two unit-square roots are the twin triangles across the (1,0)-(0,1) diagonal. Even root:
// v0=(1,0), v1=(0,0), v2=(0,1); odd: v0=(0,1), v1=(1,1), v2=(1,0). The corner table is folded
// into the two scalar sums below rather than multiplied in as a vector: every operation on the
// wide numerators is a SCALAR 64-bit add, and each numerator is converted to float on its own.
// Trap: the CBT kernels do no 64-bit integer VECTOR arithmetic or conversion anywhere; every add,
// multiply, divide and int/float conversion of a wide value is on a scalar. A chain of 64-bit
// vector ops (OpIMul/OpIAdd v2long, OpConvertSToF v2float v2long) can evaluate as (u+v, u+v) on a
// fraction of invocations on the NVIDIA driver (observed after a terrain re-provision with blocking
// readbacks between frames) — a degenerate sliver per affected bisector, which Classify then culls
// and freezes on. The scalar form is exact (root corners are 0/1, the numerators are exact integers
// below 2^40). The CBTKernelWideVectorOps test (CBTTerrainTests) fails on any such op in the
// compiled blobs.
#if defined(CBT_DECODE_INT64) || defined(CBT_DECODE_INT32)
vec2 CBT_PlanarBaryToUV(uint rootIndex, CBT_BVEC3 bary, float invScale)
{
    CBT_BINT numU;
    CBT_BINT numV;
    if ((rootIndex & 1u) == 0u)
    {
        numU = bary.x;
        numV = bary.z;
    }
    else
    {
        numU = bary.y + bary.z;
        numV = bary.x + bary.y;
    }
    return vec2(float(numU) * invScale, float(numV) * invScale);
}
#endif // CBT_DECODE_INT64 || CBT_DECODE_INT32

// Deterministic procedural relief for the planet demo (heightmap-driven relief on a
// sphere is a follow-up: the E-pipeline bakes planar heightmaps per terrain today).
// A function of the UNIT direction only — two triangles sharing a vertex normalize to
// the same direction and so get the same relief: crack-free (see the header note).
//
// LOCKSTEP with CBTPlanetShading.h (PlanetBaseNoise / PlanetRelief / PlanetReliefGradient):
// the CPU mirror is what the arc-law oracles validate, so these must stay bit-for-bit
// equivalent. The fBM stack (persistence/lacunarity) is normalized by its amplitude sum so
// the TOTAL relief keeps the +/- 1.5*amplitude envelope for ANY octave count — the on-shell
// bound in CBTSphereDomainTests stays valid regardless of octave count.
const float CBT_RELIEF_PERSISTENCE = 0.5;
// Non-harmonic (irrational) lacunarity so octave frequencies are NOT integer multiples of the
// base. On a sphere the axis-separable sin-product octaves at 2x/4x harmonics reinforce on a
// regular grid -> a visible lattice. 2.13 gives frequencies f, 2.13f, 4.54f, ... that never
// re-align, breaking the lattice (plan §planet-shading lattice diagnosis).
const float CBT_RELIEF_LACUNARITY = 2.13;
const float CBT_RELIEF_ENVELOPE = 1.5; // max |base noise| = 1.0 + 0.5 (its two terms)

// Single-octave base noise at direction d and angular frequency f. Envelope [-1.5, 1.5].
float CBT_PlanetBaseNoise(vec3 d, float f)
{
    float s1 = sin(d.x * f);
    float s2 = sin(d.y * f * 1.3 + 0.7);
    float s3 = sin(d.z * f * 0.7 + 1.9);
    float s4 = sin(d.x * f * 2.1 + 1.3);
    float s5 = sin(d.y * f * 1.7 + 2.4);
    return s1 * s2 * s3 + 0.5 * s4 * s5;
}

// Closed-form gradient of CBT_PlanetBaseNoise w.r.t. d (product rule on the two sin terms).
vec3 CBT_PlanetBaseNoiseGradient(vec3 d, float f)
{
    float s1 = sin(d.x * f), c1 = cos(d.x * f);
    float s2 = sin(d.y * f * 1.3 + 0.7), c2 = cos(d.y * f * 1.3 + 0.7);
    float s3 = sin(d.z * f * 0.7 + 1.9), c3 = cos(d.z * f * 0.7 + 1.9);
    float s4 = sin(d.x * f * 2.1 + 1.3), c4 = cos(d.x * f * 2.1 + 1.3);
    float s5 = sin(d.y * f * 1.7 + 2.4), c5 = cos(d.y * f * 1.7 + 2.4);
    float gx = f * c1 * s2 * s3 + 0.5 * (2.1 * f) * c4 * s5;
    float gy = (1.3 * f) * s1 * c2 * s3 + 0.5 * (1.7 * f) * s4 * c5;
    float gz = (0.7 * f) * s1 * s2 * c3;
    return vec3(gx, gy, gz);
}

// Multi-octave (fBM) relief in metres at UNIT direction dir. octaves 0 -> 1. Normalized by
// the amplitude sum so the envelope is +/- CBT_RELIEF_ENVELOPE * amplitude for any octaves.
float CBT_PlanetRelief(vec3 dir, float amplitude, float frequency, uint octaves)
{
    if (amplitude <= 0.0)
        return 0.0;
    uint oct = max(octaves, 1u);
    float total = 0.0, a = 1.0, f = frequency, ampSum = 0.0;
    for (uint i = 0u; i < oct; ++i)
    {
        total += a * CBT_PlanetBaseNoise(dir, f);
        ampSum += a;
        a *= CBT_RELIEF_PERSISTENCE;
        f *= CBT_RELIEF_LACUNARITY;
    }
    return amplitude * total / ampSum;
}

// The 3D gradient d(relief)/d(dir) of the multi-octave relief. Project out the radial
// component (g - dot(g,dir)*dir) for the tangential gradient the shading normal wants.
vec3 CBT_PlanetReliefGradient(vec3 dir, float amplitude, float frequency, uint octaves)
{
    if (amplitude <= 0.0)
        return vec3(0.0);
    uint oct = max(octaves, 1u);
    vec3 g = vec3(0.0);
    float a = 1.0, f = frequency, ampSum = 0.0;
    for (uint i = 0u; i < oct; ++i)
    {
        g += a * CBT_PlanetBaseNoiseGradient(dir, f);
        ampSum += a;
        a *= CBT_RELIEF_PERSISTENCE;
        f *= CBT_RELIEF_LACUNARITY;
    }
    return amplitude * g / ampSum;
}

// Spherical: integer barycentric weights -> the exact cube-face position (before the
// normalize + radius scale). All three corners of a pie slice lie on ONE cube face, so
// this stays on that face's plane. Split out from the world map so VertexEval can derive
// the face-local UV (for the editable sculpt layer) from the same exact cube point.
// Compute-only (slice 1) — guarded on the int64 extension macro for the same reason as
// CBT_PlanarBaryToUV above (no int64 extension in the graphics stages; they read gVertex).
#if defined(CBT_DECODE_INT64) || defined(CBT_DECODE_INT32)
// Exact int64 numerator N = bary·cubeCorners, formed BEFORE any float (decode-precision
// slice 1). The cube corners are integers {-1,0,+1} and the face centroid is an integer
// face-centre point, so i64vec3(c*) is exact; N is then the crack-free integer both sides
// of a shared edge decode identically. Split out (S2a) so the deep (sector, local) df64
// decode can consume the SAME exact numerator the fp32 cast below consumes.
// Scalar 64-bit multiply-adds per component (see CBT_PlanarBaryToUV for why the numerator is
// never formed as a wide vector); the cube corners are converted one integer at a time.
CBT_BVEC3 CBT_SphereBaryToCubeNum(uint rootIndex, CBT_BVEC3 bary)
{
    vec3 c0, c1, c2;
    CBT_SphereRootCorners(rootIndex, c0, c1, c2);
    CBT_BVEC3 num;
    num.x = bary.x * CBT_BINT(c0.x) + bary.y * CBT_BINT(c1.x) + bary.z * CBT_BINT(c2.x);
    num.y = bary.x * CBT_BINT(c0.y) + bary.y * CBT_BINT(c1.y) + bary.z * CBT_BINT(c2.y);
    num.z = bary.x * CBT_BINT(c0.z) + bary.y * CBT_BINT(c1.z) + bary.z * CBT_BINT(c2.z);
    return num;
}

vec3 CBT_SphereBaryToCube(uint rootIndex, CBT_BVEC3 bary, float invScale)
{
    // Each exact numerator converts to float on its own; * invScale gives the cube-face
    // position (in [-1,1]^3), consumed by the normalize + the face-local UV. At numSubdiv <= 23
    // |N| < 2^24 converts exactly, so this is bit-identical to the pre-slice float sum
    // (dark-ship); past 23 the integer stays exact where float(bary) would round.
    CBT_BVEC3 num = CBT_SphereBaryToCubeNum(rootIndex, bary);
    return vec3(float(num.x) * invScale, float(num.y) * invScale, float(num.z) * invScale);
}
#endif // CBT_DECODE_INT64 || CBT_DECODE_INT32

// A pie-slice root's cube face (rootIndex = face*4 + slice). face = axis*2 + (pos?0:1),
// matching WorldDirToFaceUV / kCubeFaces (CBTSphereFaceMap.h).
uint CBT_FaceOfRoot(uint rootIndex) { return rootIndex / CBT_SLICES_PER_FACE; }

// Cube point on face `face` -> face-local UV in [0,1] (mirror of CubePosToFaceUV in
// CBTSphereFaceMap.h: Q0 origin, (Q1-Q0) the U axis, (Q3-Q0) the V axis, exact affine on
// the axis-aligned face square). Feeds the sculpt-atlas sample + the (face,rect) Classify.
vec2 CBT_FaceUVFromCube(uint face, vec3 cubePos)
{
    vec3 q0 = CBT_CubeCorner(CBT_CUBE_FACES[face * 4u + 0u]);
    vec3 q1 = CBT_CubeCorner(CBT_CUBE_FACES[face * 4u + 1u]);
    vec3 q3 = CBT_CubeCorner(CBT_CUBE_FACES[face * 4u + 3u]);
    vec3 du = q1 - q0;
    vec3 dv = q3 - q0;
    vec3 rel = cubePos - q0;
    float uu = dot(du, du);
    float vv = dot(dv, dv);
    return vec2(uu > 0.0 ? dot(rel, du) / uu : 0.0, vv > 0.0 ? dot(rel, dv) / vv : 0.0);
}

// World direction -> the cube face it projects onto (dominant axis) + face-local UV. Mirror
// of WorldDirToFaceUV (CBTSphereFaceMap.h): the fragment self-derives (face, uv) from
// normalize(positionWS) so it can sample the sculpt atlas by DIRECTION (seam-free — two
// pixels straddling a cube edge sample the same directions). face = axis*2 + (positive?0:1).
void CBT_WorldDirToFaceUV(vec3 d, out uint face, out vec2 uv)
{
    vec3 ad = abs(d);
    uint axis;
    float sgn;
    if (ad.x >= ad.y && ad.x >= ad.z) { axis = 0u; sgn = d.x >= 0.0 ? 1.0 : -1.0; }
    else if (ad.y >= ad.z)            { axis = 1u; sgn = d.y >= 0.0 ? 1.0 : -1.0; }
    else                              { axis = 2u; sgn = d.z >= 0.0 ? 1.0 : -1.0; }
    face = axis * 2u + (sgn > 0.0 ? 0u : 1u);
    float denom = (axis == 0u) ? ad.x : (axis == 1u) ? ad.y : ad.z;
    float inv = denom > 0.0 ? 1.0 / denom : 0.0;
    uv = CBT_FaceUVFromCube(face, d * inv);
}

#endif // CBT_DOMAIN_GLSL
