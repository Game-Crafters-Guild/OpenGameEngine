// Behavioural tests for the hex-tiling lattice in Includes/terrain_material_albedo.glsl — the
// shared albedo resolve both terrain surfaces run. Pure math — no Vulkan device.
//
// These tests execute the SHIPPED shader code. The lattice block of that include is extracted
// verbatim at build time (Tests/ExtractShaderBlock.cmake) and compiled as C++ through GlslShim.h,
// so editing the shader edits what runs here. A hand-written mirror could not do that: it drifts,
// and a regex over the shader text proves only that a function exists, not that it is used.
//
// The oracle is the other half of the arrangement. It restates the grid from its definition —
// density, skew, which vertices a cell has, what a barycentric coordinate is — so it can disagree
// with the shader. Three properties are checked, in order of how load-bearing they are:
//
//   1. slot membership is a function of the lattice VERTEX (SlotMembershipIsAFunctionOfTheVertex).
//      This is the fix. A vertex shared by two simplices must sit in the same slot in both, or the
//      tap carrying it jumps O(1) in UV as a quad crosses the boundary, its implicit derivative
//      blows up and the sampler lands at the coarsest mip — a flat band along the lattice edge.
//   2. slot k's weight is slot k's vertex's barycentric (SlotWeightIsTheBarycentricOfItsOwnVertex).
//      A permutation of the weights against the vertices preserves property 1 and every sum rule,
//      and still corrupts the blend.
//   3. the colouring itself: slot k holds the vertex whose (v.x - v.y) mod 3 is k.
//
// The paper's traversal emission order is kept as the counterfactual arm. It is written against
// the oracle, not taken from the shader, so the separation between the two orders stays meaningful
// however the shader is edited.

#include <gtest/gtest.h>

#include "GlslShim.h"

// The shader's own lattice math, compiled as C++. Nested inside the shim namespace so that
// unqualified lookup of floor/fract/step/abs/mod/sin/cos inside the block resolves to the shim
// and never to the <cmath> globals.
namespace GameEngine::GlslShim::Shader
{
#include "CBTHexLatticeExtracted.h"
} // namespace GameEngine::GlslShim::Shader

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <random>
#include <utility>

namespace
{

namespace Glsl = GameEngine::GlslShim;
namespace Shader = GameEngine::GlslShim::Shader;
using Glsl::ivec2;
using Glsl::vec2;

// --- Oracle: the grid stated from its definition -------------------------------------------------
// `st` carries 2*sqrt(3) hex cells per texture repeat and is sheared so the lattice becomes the
// integer grid; each integer cell splits into two simplices along t.x + t.y = 1. These constants
// are what the shader is held to, so they are written out here rather than read from it — a change
// on the shader side has to fail these tests instead of propagating into them.
constexpr float kDensity = 3.4641016151f;
constexpr float kSkew[2][2] = {{1.0f, -0.57735027f}, {0.0f, 1.15470054f}};    // rows
constexpr float kInvSkew[2][2] = {{1.0f, 0.5f}, {0.0f, 0.8660254038f}};       // rows
constexpr float kHexExp = 7.0f;

vec2 ToSkewed(vec2 st)
{
    const float x = st.x * kDensity;
    const float y = st.y * kDensity;
    return {kSkew[0][0] * x + kSkew[0][1] * y, kSkew[1][0] * x + kSkew[1][1] * y};
}

vec2 FromSkewed(vec2 sk)
{
    return {(kInvSkew[0][0] * sk.x + kInvSkew[0][1] * sk.y) / kDensity,
            (kInvSkew[1][0] * sk.x + kInvSkew[1][1] * sk.y) / kDensity};
}

// Which simplex a point is in: the integer cell plus the half it fell on.
struct Cell
{
    int BaseX = 0;
    int BaseY = 0;
    int S = 0;

    bool operator==(const Cell& o) const { return BaseX == o.BaseX && BaseY == o.BaseY && S == o.S; }
    bool operator!=(const Cell& o) const { return !(*this == o); }
    bool operator<(const Cell& o) const
    {
        return BaseX != o.BaseX ? BaseX < o.BaseX : (BaseY != o.BaseY ? BaseY < o.BaseY : S < o.S);
    }
};

Cell CellOf(vec2 st)
{
    const vec2 sk = ToSkewed(st);
    const float fx = std::floor(sk.x);
    const float fy = std::floor(sk.y);
    return {static_cast<int>(fx), static_cast<int>(fy), ((sk.x - fx) + (sk.y - fy) >= 1.0f) ? 1 : 0};
}

// A point strictly inside a given simplex, for enumerating cells rather than sampling them.
vec2 PointInCell(const Cell& c)
{
    const float tx = (c.S == 0) ? 0.23f : 0.61f;
    const float ty = (c.S == 0) ? 0.31f : 0.67f; // s = 1 half: tx + ty > 1
    return FromSkewed({static_cast<float>(c.BaseX) + tx, static_cast<float>(c.BaseY) + ty});
}

// The cell's three lattice vertices, in the paper's emission order.
void CellVertices(const Cell& c, ivec2 out[3])
{
    out[0] = {c.BaseX + c.S, c.BaseY + c.S};
    out[1] = {c.BaseX + c.S, c.BaseY + 1 - c.S};
    out[2] = {c.BaseX + 1 - c.S, c.BaseY + c.S};
}

// (v.x - v.y) mod 3, mathematically — in 64-bit so the difference is exact at every representable
// id pair, and with the residue folded non-negative.
int TrueColour(ivec2 v)
{
    const std::int64_t d = static_cast<std::int64_t>(v.x) - static_cast<std::int64_t>(v.y);
    return static_cast<int>(((d % 3) + 3) % 3);
}

// The barycentric weights of the cell's three vertices, in the same emission order.
void CellBarycentrics(vec2 st, const Cell& c, float out[3])
{
    const vec2 sk = ToSkewed(st);
    const float tx = sk.x - std::floor(sk.x);
    const float ty = sk.y - std::floor(sk.y);
    const float tz = 1.0f - tx - ty;
    if (c.S == 0)
    {
        out[0] = tz;
        out[1] = ty;
        out[2] = tx;
        return;
    }
    out[0] = -tz;
    out[1] = 1.0f - ty;
    out[2] = 1.0f - tx;
}

// --- The two slot orders under comparison ---------------------------------------------------------
struct Simplex
{
    ivec2 V[3] = {};
    float W[3] = {0.0f, 0.0f, 0.0f};
};

using SimplexFn = Simplex (*)(vec2);

// What the shader does today.
Simplex ShaderSimplex(vec2 st)
{
    const Shader::CBTHexSimplex sx = Shader::CBT_HexTriangleGrid(st);
    Simplex out;
    for (int k = 0; k < 3; ++k)
        out.V[k] = sx.v[k];
    out.W[0] = sx.w.x;
    out.W[1] = sx.w.y;
    out.W[2] = sx.w.z;
    return out;
}

// The paper's emission order — the defect signature, built from the oracle so it stays fixed.
Simplex TraversalSimplex(vec2 st)
{
    const Cell c = CellOf(st);
    Simplex out;
    CellVertices(c, out.V);
    CellBarycentrics(st, c, out.W);
    return out;
}

vec2 TapUV(vec2 st, ivec2 v, float rotStrength) { return Shader::CBT_HexTapUV(st, v, rotStrength); }

// The normalized blend weight with the luminance term held at 1. The real dw = mix(1, luminance,
// 0.6) lies in [0.4, 1], so this is within 2.5x of the weight the shader applies.
void BlendWeights(const float bw[3], double out[3])
{
    double p[3];
    double total = 0.0;
    for (int i = 0; i < 3; ++i)
    {
        p[i] = std::pow(static_cast<double>(std::max(bw[i], 0.0f)), static_cast<double>(kHexExp));
        total += p[i];
    }
    for (int i = 0; i < 3; ++i)
        out[i] = (total > 0.0) ? p[i] / total : 0.0;
}

// --- Boundary crossing search ---------------------------------------------------------------------
enum BoundaryFamily
{
    kFamilyDiagonal = 0, // the s-flip edge t.x + t.y = 1, cell unchanged
    kFamilyXLine = 1,    // skewed.x crosses an integer
    kFamilyYLine = 2,
    kFamilyCount = 3
};

int ClassifyBoundary(const Cell& a, const Cell& b)
{
    if (a.BaseX != b.BaseX)
        return kFamilyXLine;
    if (a.BaseY != b.BaseY)
        return kFamilyYLine;
    return kFamilyDiagonal;
}

struct DiscontinuityReport
{
    double MaxJumpTimesBarycentric = 0.0;
    double MaxJumpTimesBlendWeight = 0.0;
    int Crossings = 0;
    int FamilyCounts[kFamilyCount] = {0, 0, 0};
    // Worst case detail, for the failure message.
    double WorstJump = 0.0;
    double WorstWeight = 0.0;
    int WorstSlot = -1;
    ivec2 WorstFrom = {};
    ivec2 WorstTo = {};
};

// Folds one pair of points straddling a simplex boundary into the report. For each slot, both
// sides' vertex is evaluated at the SAME point, so the measured jump is the pure identity-induced
// offset — zero when the slot keeps its vertex, and free of the sampling-distance drift a
// continuous slot would otherwise show.
void AccumulatePair(DiscontinuityReport& rep, vec2 a, vec2 c, SimplexFn simplexOf, float rotStrength)
{
    const Cell ka = CellOf(a);
    const Cell kc = CellOf(c);
    if (ka == kc)
        return;
    ++rep.Crossings;
    ++rep.FamilyCounts[ClassifyBoundary(ka, kc)];

    const Simplex sa = simplexOf(a);
    const Simplex sc = simplexOf(c);
    double bwa[3], bwc[3];
    BlendWeights(sa.W, bwa);
    BlendWeights(sc.W, bwc);
    for (int k = 0; k < 3; ++k)
    {
        const vec2 ua = TapUV(a, sa.V[k], rotStrength);
        const vec2 uc = TapUV(a, sc.V[k], rotStrength);
        const double jump = std::hypot(static_cast<double>(uc.x - ua.x), static_cast<double>(uc.y - ua.y));
        const double bary = std::max(std::fabs(static_cast<double>(sa.W[k])), std::fabs(static_cast<double>(sc.W[k])));
        const double blend = std::max(bwa[k], bwc[k]);
        if (jump * bary > rep.MaxJumpTimesBarycentric)
        {
            rep.MaxJumpTimesBarycentric = jump * bary;
            rep.WorstJump = jump;
            rep.WorstWeight = bary;
            rep.WorstSlot = k;
            rep.WorstFrom = sa.V[k];
            rep.WorstTo = sc.V[k];
        }
        rep.MaxJumpTimesBlendWeight = std::max(rep.MaxJumpTimesBlendWeight, jump * blend);
    }
}

// Walks random rays until the simplex changes, bisects onto the boundary, then samples `eps`
// either side of it.
DiscontinuityReport MeasureDiscontinuity(SimplexFn simplexOf, float rotStrength, int samples, float eps, uint32_t seed)
{
    DiscontinuityReport rep;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> pos(-40.0f, 40.0f);
    std::uniform_real_distribution<float> ang(0.0f, 6.28318531f);
    constexpr float kWalkStep = 0.02f;
    constexpr int kMaxWalk = 400;
    constexpr int kBisectIters = 40;

    for (int i = 0; i < samples; ++i)
    {
        const vec2 origin = {pos(rng), pos(rng)};
        const float theta = ang(rng);
        const vec2 dir = {std::cos(theta), std::sin(theta)};
        const Cell k0 = CellOf(origin);

        float lo = 0.0f;
        float hi = kWalkStep;
        bool found = false;
        for (int step = 0; step < kMaxWalk; ++step)
        {
            if (CellOf({origin.x + dir.x * hi, origin.y + dir.y * hi}) != k0)
            {
                found = true;
                break;
            }
            lo = hi;
            hi += kWalkStep;
        }
        if (!found)
            continue;
        for (int b = 0; b < kBisectIters; ++b)
        {
            const float mid = 0.5f * (lo + hi);
            if (CellOf({origin.x + dir.x * mid, origin.y + dir.y * mid}) != k0)
                hi = mid;
            else
                lo = mid;
        }

        const vec2 a = {origin.x + dir.x * (lo - eps), origin.y + dir.y * (lo - eps)};
        const vec2 c = {origin.x + dir.x * (hi + eps), origin.y + dir.y * (hi + eps)};
        AccumulatePair(rep, a, c, simplexOf, rotStrength);
    }
    return rep;
}

// Quads can also straddle a lattice VERTEX, where six simplices meet and two of them share only
// that one vertex. Samples pairs on a small circle around lattice vertices and feeds the ones that
// landed in different simplices through the same metric.
DiscontinuityReport MeasureAroundLatticeVertices(SimplexFn simplexOf, float rotStrength, int vertices, float radius,
                                                 uint32_t seed)
{
    DiscontinuityReport rep;
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> id(-120, 120);
    std::uniform_real_distribution<float> ang(0.0f, 6.28318531f);
    constexpr int kProbesPerVertex = 12;

    for (int i = 0; i < vertices; ++i)
    {
        const ivec2 v = {id(rng), id(rng)};
        const vec2 centre = Shader::CBT_HexCenter(v); // st where skewed == v exactly
        vec2 probe[kProbesPerVertex];
        for (int p = 0; p < kProbesPerVertex; ++p)
        {
            const float theta = ang(rng);
            probe[p] = {centre.x + radius * std::cos(theta), centre.y + radius * std::sin(theta)};
        }
        for (int p = 0; p < kProbesPerVertex; ++p)
            for (int q = p + 1; q < kProbesPerVertex; ++q)
                AccumulatePair(rep, probe[p], probe[q], simplexOf, rotStrength);
    }
    return rep;
}

// The window the enumerating tests sweep: every simplex of every cell in it, both parities,
// straddling the origin so negative ids are covered.
constexpr int kWindow = 50;
constexpr int kWindowSimplices = (2 * kWindow + 1) * (2 * kWindow + 1) * 2;

} // namespace

// THE FIX. Slot membership must be a function of the lattice vertex alone: wherever a vertex
// appears, it appears in the same slot. That is what survives a simplex-edge crossing — the shared
// vertices keep their taps, so those taps' UVs stay continuous and their implicit derivatives stay
// the true gradient. Traversal order fails this on nearly every cell; dropping the negative-d fold
// from the colouring fails it on the v.x == v.y diagonal, where the residues stop being distinct.
TEST(CBTHexTiling, SlotMembershipIsAFunctionOfTheVertex)
{
    std::map<std::pair<int, int>, int> slotOfVertex;
    int conflicts = 0;
    int firstConflictSlot = -1;
    ivec2 firstConflictVertex = {};
    int cells = 0;

    for (int by = -kWindow; by <= kWindow; ++by)
    {
        for (int bx = -kWindow; bx <= kWindow; ++bx)
        {
            for (int si = 0; si <= 1; ++si)
            {
                const Cell c = {bx, by, si};
                const vec2 st = PointInCell(c);
                ASSERT_EQ(CellOf(st), c) << "the probe point left its cell at " << bx << "," << by << " s=" << si;
                ++cells;

                const Simplex sx = ShaderSimplex(st);
                ivec2 expected[3];
                CellVertices(c, expected);
                for (int k = 0; k < 3; ++k)
                {
                    const bool present = sx.V[k] == expected[0] || sx.V[k] == expected[1] || sx.V[k] == expected[2];
                    ASSERT_TRUE(present) << "slot " << k << " holds a vertex outside the simplex at " << bx << ","
                                         << by << " s=" << si;
                }
                ASSERT_TRUE(sx.V[0] != sx.V[1] && sx.V[1] != sx.V[2] && sx.V[0] != sx.V[2])
                    << "a vertex is repeated across slots at " << bx << "," << by << " s=" << si;

                for (int k = 0; k < 3; ++k)
                {
                    const auto key = std::make_pair(sx.V[k].x, sx.V[k].y);
                    const auto it = slotOfVertex.find(key);
                    if (it == slotOfVertex.end())
                    {
                        slotOfVertex.emplace(key, k);
                    }
                    else if (it->second != k)
                    {
                        if (conflicts == 0)
                        {
                            firstConflictSlot = it->second;
                            firstConflictVertex = sx.V[k];
                        }
                        ++conflicts;
                    }
                }
            }
        }
    }

    ASSERT_EQ(cells, kWindowSimplices) << "the sweep did not cover the window";
    EXPECT_EQ(conflicts, 0) << conflicts << " of " << (3 * kWindowSimplices)
                            << " slot assignments put a vertex in a slot it does not occupy elsewhere; first at ("
                            << firstConflictVertex.x << "," << firstConflictVertex.y << "), seen in slot "
                            << firstConflictSlot;
}

// The colouring is total: it returns the true residue for every representable id pair, including
// the pairs whose difference does not fit in an int. That is what abs() cannot deliver — the
// absolute value of the most negative int is itself negative, and a negative operand puts % in
// undefined territory, which collapsed the simplex at baseId = (INT_MIN, 0) to two colours instead
// of three.
TEST(CBTHexTiling, ColourIsTotalAcrossTheIdRange)
{
    for (int d = -3000; d <= 3000; ++d)
    {
        const int expected = ((d % 3) + 3) % 3;
        ASSERT_EQ(Shader::CBT_HexColor({d, 0}), expected) << "d=" << d;
        ASSERT_EQ(Shader::CBT_HexColor({0, -d}), expected) << "d=" << d;
        ASSERT_EQ(Shader::CBT_HexColor({d + 7, 7}), expected) << "d=" << d;
    }

    constexpr int kMin = std::numeric_limits<int>::min();
    constexpr int kMax = std::numeric_limits<int>::max();
    const int ends[] = {kMin, kMin + 1, -1, 0, 1, kMax - 1, kMax};
    for (const int x : ends)
        for (const int y : ends)
            EXPECT_EQ(Shader::CBT_HexColor({x, y}), TrueColour({x, y})) << x << "," << y;

    // The simplex the missing fold collapsed. Its vertex ids are all representable, so it has to
    // come out with three colours. A simplex based at INT_MAX does not, because forming its
    // vertices overflows the id itself — that is the grid's limit rather than the colouring's, and
    // it sits far beyond the magnitudes where ivec2(floor(...)) means anything.
    for (int si = 0; si <= 1; ++si)
    {
        const ivec2 p[3] = {{kMin + si, si}, {kMin + si, 1 - si}, {kMin + 1 - si, si}};
        const int k[3] = {Shader::CBT_HexColor(p[0]), Shader::CBT_HexColor(p[1]), Shader::CBT_HexColor(p[2])};
        EXPECT_NE(k[0], k[1]) << "s=" << si;
        EXPECT_NE(k[1], k[2]) << "s=" << si;
        EXPECT_NE(k[0], k[2]) << "s=" << si;
    }
}

// The colouring itself: slot k holds the vertex whose (v.x - v.y) mod 3 is k. Stricter than the
// property above — a globally consistent relabelling would satisfy that one — and it pins the rule
// the shader documents, including the fold that makes the residue non-negative.
TEST(CBTHexTiling, SlotHoldsTheVertexOfItsOwnColour)
{
    int mismatches = 0;
    int firstBadSlot = -1;
    ivec2 firstBadVertex = {};

    for (int by = -kWindow; by <= kWindow; ++by)
    {
        for (int bx = -kWindow; bx <= kWindow; ++bx)
        {
            for (int si = 0; si <= 1; ++si)
            {
                const Simplex sx = ShaderSimplex(PointInCell({bx, by, si}));
                for (int k = 0; k < 3; ++k)
                {
                    if (TrueColour(sx.V[k]) == k)
                        continue;
                    if (mismatches == 0)
                    {
                        firstBadSlot = k;
                        firstBadVertex = sx.V[k];
                    }
                    ++mismatches;
                }
            }
        }
    }

    EXPECT_EQ(mismatches, 0) << mismatches << " of " << (3 * kWindowSimplices)
                             << " slots hold a vertex of the wrong colour; first: slot " << firstBadSlot
                             << " holds (" << firstBadVertex.x << "," << firstBadVertex.y << ") of colour "
                             << TrueColour(firstBadVertex);
}

// Each slot's weight must be its OWN vertex's barycentric. A barycentric decomposition
// reconstructs the point it came from — sum_k w[k] * v[k] is the skewed position, the weights sum
// to 1 and none is negative — and no permutation of the weights against the vertices does. This is
// the check the slot-membership and colouring tests cannot make: pairing the right vertex with the
// wrong weight leaves both of them satisfied.
TEST(CBTHexTiling, SlotWeightIsTheBarycentricOfItsOwnVertex)
{
    // Reconstruction runs in skewed coordinates, where the lattice ids ARE the vertex positions.
    // |st| <= 40 puts them inside +/-160, so float rounding contributes at most ~1e-4; a weight
    // paired with the wrong vertex moves the reconstruction by O(1), four orders above that.
    constexpr double kResidualBound = 1e-3;

    std::mt19937 rng(20260803u);
    std::uniform_real_distribution<float> pos(-40.0f, 40.0f);
    double worstResidual = 0.0;
    double worstSumError = 0.0;
    double mostNegative = 0.0;
    vec2 worstAt = {};

    for (int i = 0; i < 20000; ++i)
    {
        const vec2 st = {pos(rng), pos(rng)};
        const Simplex sx = ShaderSimplex(st);
        const vec2 skewed = ToSkewed(st);

        double sum = 0.0;
        double rx = 0.0;
        double ry = 0.0;
        for (int k = 0; k < 3; ++k)
        {
            sum += sx.W[k];
            rx += static_cast<double>(sx.W[k]) * sx.V[k].x;
            ry += static_cast<double>(sx.W[k]) * sx.V[k].y;
            mostNegative = std::min(mostNegative, static_cast<double>(sx.W[k]));
        }
        const double residual = std::hypot(rx - skewed.x, ry - skewed.y);
        if (residual > worstResidual)
        {
            worstResidual = residual;
            worstAt = st;
        }
        worstSumError = std::max(worstSumError, std::fabs(sum - 1.0));
    }

    GTEST_LOG_(INFO) << "barycentric reconstruction: max residual " << worstResidual << ", max |sum-1| "
                     << worstSumError << ", most negative weight " << mostNegative;

    EXPECT_LT(worstResidual, kResidualBound)
        << "sum_k w[k]*v[k] misses the point it decomposes by " << worstResidual << " at st=" << worstAt.x << ","
        << worstAt.y << " — a slot is carrying another slot's weight";
    EXPECT_LT(worstSumError, 1e-4) << "the slot weights are not a partition of unity";
    EXPECT_GT(mostNegative, -1e-4) << "a slot weight is negative, so the point is outside its own simplex";
}

// The consequence, measured: traversal order lets a slot's UV jump O(1) while carrying O(1)
// weight; colour slotting drives the weighted jump to ~eps^7. The bounds are pre-registered with a
// margin of more than ten orders of magnitude between the two orders.
TEST(CBTHexTiling, ColourSlottingKeepsTapUVContinuousWhereItCarriesWeight)
{
    // Sampling offset either side of the located boundary. Kept at 1e-4 so bw^7 stays well inside
    // normal float range and the boundary bisection resolves it at these coordinate magnitudes.
    constexpr float kEps = 1e-4f;
    // Traversal order must breach this — it is the defect signature, and a counterfactual that
    // stopped reproducing it would no longer be measuring anything.
    constexpr double kTraversalFloor = 0.1;
    // The shader must stay under this. Measured ~2e-22 (the weight is O(eps^7)); the bound is ten
    // orders looser so it is a law, not a curve fit.
    constexpr double kShaderBound = 1e-12;

    for (const float rotStrength : {1.0f, 0.35f})
    {
        const DiscontinuityReport traversal = MeasureDiscontinuity(&TraversalSimplex, rotStrength, 3000, kEps, 1234u);
        const DiscontinuityReport shader = MeasureDiscontinuity(&ShaderSimplex, rotStrength, 3000, kEps, 1234u);

        GTEST_LOG_(INFO) << "rot=" << rotStrength << " crossings=" << traversal.Crossings << " (diagonal "
                         << traversal.FamilyCounts[kFamilyDiagonal] << ", x-line " << traversal.FamilyCounts[kFamilyXLine]
                         << ", y-line " << traversal.FamilyCounts[kFamilyYLine] << ")"
                         << "  max |dUV|*blendW: traversal " << traversal.MaxJumpTimesBlendWeight << " -> shader "
                         << shader.MaxJumpTimesBlendWeight << "  |  max |dUV|*barycentric: traversal "
                         << traversal.MaxJumpTimesBarycentric << " -> shader " << shader.MaxJumpTimesBarycentric;

        ASSERT_GT(traversal.Crossings, 2000) << "the ray walk is not finding boundaries";
        ASSERT_EQ(traversal.Crossings, shader.Crossings) << "the two orders must see the same crossings";
        for (int f = 0; f < kFamilyCount; ++f)
            EXPECT_GT(traversal.FamilyCounts[f], 0) << "boundary family " << f << " was never crossed";

        EXPECT_GT(traversal.MaxJumpTimesBlendWeight, kTraversalFloor)
            << "rot=" << rotStrength << ": traversal order no longer reproduces the discontinuity";
        EXPECT_LT(shader.MaxJumpTimesBlendWeight, kShaderBound)
            << "rot=" << rotStrength << ": the shader's tap UV jumps by " << shader.WorstJump << " in slot "
            << shader.WorstSlot << " while carrying barycentric " << shader.WorstWeight << " (vertex "
            << shader.WorstFrom.x << "," << shader.WorstFrom.y << " -> " << shader.WorstTo.x << ","
            << shader.WorstTo.y << ")";
        // The barycentric-weighted form is the same statement without the w^7 sharpening: O(eps)
        // instead of exactly zero only because the samples sit eps off the boundary.
        EXPECT_GT(traversal.MaxJumpTimesBarycentric, kTraversalFloor) << "rot=" << rotStrength;
        EXPECT_LT(shader.MaxJumpTimesBarycentric, 100.0 * static_cast<double>(kEps)) << "rot=" << rotStrength;
    }
}

// The same property where six simplices meet. Two simplices around a lattice vertex can share only
// that vertex, so all three slots are in play at once; the shared vertex carries nearly all the
// weight and must therefore keep its slot, which is exactly what the colouring guarantees.
TEST(CBTHexTiling, ColourSlottingHoldsAroundLatticeVertices)
{
    constexpr float kRadius = 1e-4f;
    constexpr double kTraversalFloor = 0.1;
    constexpr double kShaderBound = 1e-12;

    const DiscontinuityReport traversal = MeasureAroundLatticeVertices(&TraversalSimplex, 1.0f, 400, kRadius, 4321u);
    const DiscontinuityReport shader = MeasureAroundLatticeVertices(&ShaderSimplex, 1.0f, 400, kRadius, 4321u);

    GTEST_LOG_(INFO) << "lattice-vertex neighbourhoods: crossings=" << traversal.Crossings << " (diagonal "
                     << traversal.FamilyCounts[kFamilyDiagonal] << ", x-line " << traversal.FamilyCounts[kFamilyXLine]
                     << ", y-line " << traversal.FamilyCounts[kFamilyYLine] << ")  max |dUV|*blendW: traversal "
                     << traversal.MaxJumpTimesBlendWeight << " -> shader " << shader.MaxJumpTimesBlendWeight;

    ASSERT_GT(traversal.Crossings, 1000) << "the probe ring is not straddling simplices";
    ASSERT_EQ(traversal.Crossings, shader.Crossings);
    EXPECT_GT(traversal.MaxJumpTimesBlendWeight, kTraversalFloor)
        << "traversal order no longer reproduces the discontinuity at lattice vertices";
    EXPECT_LT(shader.MaxJumpTimesBlendWeight, kShaderBound)
        << "the shader's tap UV jumps by " << shader.WorstJump << " in slot " << shader.WorstSlot
        << " while carrying barycentric " << shader.WorstWeight;
}

// Slotting is a permutation of the same vertex set with the same per-vertex weights, so the blended
// result is invariant GIVEN exact arithmetic and well-defined derivatives. That precondition does
// NOT hold in the running shader — the taps sit inside the divergent per-layer loop and the
// triplanar gates, where implicit derivatives are undefined — so this asserts a property of the
// blend algebra, not a prediction that the rendered frame is unchanged. A measured capture pair
// contradicts the latter (23.737% of pixels, mechanism unresolved; see the design doc's §2.4).
// This test also does not discriminate the fix: with the slotting removed both arms become
// identical and it still passes.
TEST(CBTHexTiling, SlotOrderDoesNotChangeTheBlendedColour)
{
    // Stand-in for the layer texture: any deterministic function of the tap UV works, since the
    // claim is about the blend algebra and not about texture content.
    const auto SampleAt = [](vec2 uv) {
        return std::array<float, 3>{0.5f + 0.5f * std::sin(uv.x * 6.0f),
                                    0.5f + 0.5f * std::sin(uv.y * 5.0f + 1.0f),
                                    0.5f + 0.5f * std::sin((uv.x + uv.y) * 4.0f + 2.0f)};
    };
    std::mt19937 rng(99u);
    std::uniform_real_distribution<float> pos(-40.0f, 40.0f);
    constexpr float kLum[3] = {0.299f, 0.587f, 0.114f};
    const SimplexFn orders[2] = {&TraversalSimplex, &ShaderSimplex};
    for (int i = 0; i < 20000; ++i)
    {
        const vec2 st = {pos(rng), pos(rng)};
        const float rotStrength = (i % 2 == 0) ? 1.0f : 0.35f;
        std::array<float, 3> result[2];
        for (int o = 0; o < 2; ++o)
        {
            const Simplex sx = orders[o](st);
            std::array<float, 3> c[3];
            double w[3];
            double total = 0.0;
            for (int k = 0; k < 3; ++k)
            {
                c[k] = SampleAt(TapUV(st, sx.V[k], rotStrength));
                const double lum = kLum[0] * c[k][0] + kLum[1] * c[k][1] + kLum[2] * c[k][2];
                const double dw = 1.0 + static_cast<double>(Shader::CBT_HEX_FALLOFF) * (lum - 1.0); // mix(1, lum, 0.6)
                w[k] = dw * std::pow(static_cast<double>(std::max(sx.W[k], 0.0f)), static_cast<double>(kHexExp));
                total += w[k];
            }
            for (int ch = 0; ch < 3; ++ch)
            {
                double acc = 0.0;
                for (int k = 0; k < 3; ++k)
                    acc += (w[k] / total) * static_cast<double>(c[k][ch]);
                result[o][ch] = static_cast<float>(acc);
            }
        }
        for (int ch = 0; ch < 3; ++ch)
            ASSERT_NEAR(result[0][ch], result[1][ch], 1e-5f) << "st=" << st.x << "," << st.y << " ch=" << ch;
    }
}

// The blend exponent the oracle mirrors is the shader's. It is not extracted into the tap loop
// above by accident: the discontinuity bounds are stated in terms of eps^7.
TEST(CBTHexTiling, BlendConstantsAreTheShaderConstants)
{
    EXPECT_FLOAT_EQ(Shader::CBT_HEX_EXP, kHexExp);
    EXPECT_FLOAT_EQ(Shader::CBT_HEX_FALLOFF, 0.6f);
}

// What the per-tile randomization has to be for the tiling to break up repetition at all: an
// offset inside one texture repeat, a different one per tile, and a rotation that is a rotation.
// The specific hash constants are the paper's and are not pinned here — a hash that collapses or
// leaves the unit square is a defect, a hash that returns different values is a decision.
TEST(CBTHexTiling, TileHashAndRotationAreWellFormed)
{
    std::map<std::pair<int, int>, int> distinct;
    int ids = 0;
    float worstDet = 0.0f;
    float worstNorm = 0.0f;

    for (int y = -60; y <= 60; ++y)
    {
        for (int x = -60; x <= 60; ++x)
        {
            const ivec2 v = {x, y};
            const vec2 h = Shader::CBT_HexHash(v);
            ASSERT_GE(h.x, 0.0f);
            ASSERT_LT(h.x, 1.0f);
            ASSERT_GE(h.y, 0.0f);
            ASSERT_LT(h.y, 1.0f);
            ++ids;
            // Quantized so "distinct" means visibly distinct, not distinct in the last bit.
            distinct[{static_cast<int>(h.x * 4096.0f), static_cast<int>(h.y * 4096.0f)}] = 1;

            const Glsl::mat2 r = Shader::CBT_HexRot(v, 1.0f);
            const float det = r.c0.x * r.c1.y - r.c1.x * r.c0.y;
            worstDet = std::max(worstDet, std::fabs(det - 1.0f));
            worstNorm = std::max(worstNorm, std::fabs(r.c0.x * r.c0.x + r.c0.y * r.c0.y - 1.0f));
        }
    }

    GTEST_LOG_(INFO) << "tile hash: " << distinct.size() << " distinct of " << ids
                     << " ids; rotation max |det-1| " << worstDet << ", max |col0|^2-1 " << worstNorm;
    // The bar is collapse, not perfection. A sin() hash carries structure — 14310 of 14641 ids are
    // distinct to 1/4096 over this window, where chance alone would give ~6 collisions rather than
    // 331 — so the useful assertion is that the tiles do not fall into a handful of buckets.
    EXPECT_GT(static_cast<int>(distinct.size()), (ids * 90) / 100) << "the tile hash is collapsing";
    EXPECT_LT(worstDet, 1e-5f) << "CBT_HexRot is not a rotation";
    EXPECT_LT(worstNorm, 1e-5f) << "CBT_HexRot is not orthonormal";
}
