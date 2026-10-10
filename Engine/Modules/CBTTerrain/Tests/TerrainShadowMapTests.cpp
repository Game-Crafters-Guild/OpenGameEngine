// The terrain's sun-space clearance map (terrain_shadow.glsl, terrain_shadow_bake.comp,
// TerrainShadowGrid): the shared shader block runs here on the host, extracted from the shader the
// GPU compiles, so these tests execute the bake's per-sample steps and the receivers' lookup rather
// than a copy of them.
//
// - The bake emulates terrain_shadow_bake.comp's structure exactly (256 runs of 16 samples, the
//   run maxima, the Hillis-Steele suffix scan over the runs, the downwind walk) around the block's
//   functions, and stores floats where the GPU stores RG16F: half rounding moves a clearance by about
//   0.1 % of its value, which matters only where the clearance is near zero, at a shadow's edge,
//   which the comparisons exclude.
// - The reference is a brute-force march along the ray toward the sun over the bilinear height
//   field (the per-pixel march of the prototype, PR #2770, without its acceleration pyramid).

#include <gtest/gtest.h>

#include "CBTTerrain/TerrainShadowGrid.h"
#include "GlslShim.h"
#include "Mathematics/VectorOps.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <vector>

namespace
{
using GameEngine::CBTTerrain::ComputeTerrainShadowGrid;
using GameEngine::CBTTerrain::kTerrainShadowLineCapacity;
using GameEngine::CBTTerrain::kTerrainShadowMaxSide;
using GameEngine::CBTTerrain::TerrainShadowDirtyLines;
using GameEngine::CBTTerrain::TerrainShadowExtent;
using GameEngine::CBTTerrain::TerrainShadowGrid;
using GameEngine::CBTTerrain::TerrainShadowLines;
using GameEngine::CBTTerrain::TerrainShadowMapSide;
using GameEngine::CBTTerrain::TerrainShadowTexel;

// A lattice height field: (Cells + 1)^2 samples over Size x Size metres, heights in metres.
struct HeightField
{
    uint32_t Cells = 0;
    float Size = 0.0f;
    std::vector<float> Heights;

    float At(uint32_t i, uint32_t j) const { return Heights[static_cast<size_t>(j) * (Cells + 1) + i]; }

    // The bilinear surface at (x, z) clamped to the terrain's edge: CBT_SampleHeight's lattice tap.
    float Sample(float x, float z) const
    {
        const float lx = std::clamp(x / Size, 0.0f, 1.0f) * static_cast<float>(Cells);
        const float lz = std::clamp(z / Size, 0.0f, 1.0f) * static_cast<float>(Cells);
        const uint32_t i = std::min(static_cast<uint32_t>(lx), Cells - 1u);
        const uint32_t j = std::min(static_cast<uint32_t>(lz), Cells - 1u);
        const float fx = lx - static_cast<float>(i);
        const float fz = lz - static_cast<float>(j);
        const float a = At(i, j) + (At(i + 1, j) - At(i, j)) * fx;
        const float b = At(i, j + 1) + (At(i + 1, j + 1) - At(i, j + 1)) * fx;
        return a + (b - a) * fz;
    }
};

HeightField MakeField(uint32_t cells, float size, const std::function<float(float, float)>& height)
{
    HeightField f;
    f.Cells = cells;
    f.Size = size;
    f.Heights.resize(static_cast<size_t>(cells + 1) * (cells + 1));
    const float cell = size / static_cast<float>(cells);
    for (uint32_t j = 0; j <= cells; ++j)
        for (uint32_t i = 0; i <= cells; ++i)
            f.Heights[static_cast<size_t>(j) * (cells + 1) + i] = height(i * cell, j * cell);
    return f;
}

// The map as the GPU holds it: Side x Side texels, line v in row v; the clearance and the occluder
// distance (the RG channels).
struct ClearanceMap
{
    uint32_t Side = 0;
    std::vector<float> Texels;
    std::vector<float> Distances;

    // Bilinear at a grid position in samples (texel centres at integers), clamped to the map:
    // textureLod with a linear clamp sampler at (gridCoord + 0.5) / side.
    static float Bilinear(const std::vector<float>& channel, uint32_t side, float u, float v)
    {
        const float cu = std::clamp(u, 0.0f, static_cast<float>(side - 1u));
        const float cv = std::clamp(v, 0.0f, static_cast<float>(side - 1u));
        const uint32_t i = std::min(static_cast<uint32_t>(cu), side - 2u);
        const uint32_t j = std::min(static_cast<uint32_t>(cv), side - 2u);
        const float fu = cu - static_cast<float>(i);
        const float fv = cv - static_cast<float>(j);
        auto at = [&channel, side](uint32_t x, uint32_t y) { return channel[static_cast<size_t>(y) * side + x]; };
        const float a = at(i, j) + (at(i + 1, j) - at(i, j)) * fu;
        const float b = at(i, j + 1) + (at(i + 1, j + 1) - at(i, j + 1)) * fu;
        return a + (b - a) * fv;
    }
};

// The two reads the shader block takes from its includer, bound to the test's current field and map.
const HeightField* gField = nullptr;
const ClearanceMap* gMap = nullptr;

namespace Shader
{
using GameEngine::GlslShim::acos;
using GameEngine::GlslShim::clamp;
using GameEngine::GlslShim::max;
using GameEngine::GlslShim::min;
using GameEngine::GlslShim::sqrt;
using GameEngine::GlslShim::vec2;
using GameEngine::GlslShim::vec4;

vec2 GE_TerrainShadowReadClearance(vec2 gridCoord)
{
    return {ClearanceMap::Bilinear(gMap->Texels, gMap->Side, gridCoord.x, gridCoord.y),
            ClearanceMap::Bilinear(gMap->Distances, gMap->Side, gridCoord.x, gridCoord.y)};
}
float GE_TerrainShadowReadGround(float x, float z)
{
    return gField->Sample(x, z);
}
#include "TerrainShadowExtracted.h"
} // namespace Shader

// The shader's grid for `grid` over the field, in the terrain's local frame (corner at the origin).
Shader::GE_TerrainShadowGrid ShaderGrid(const TerrainShadowGrid& grid, const HeightField& field)
{
    Shader::GE_TerrainShadowGrid g{};
    g.CenterX = 0.5f * field.Size;
    g.CenterZ = 0.5f * field.Size;
    g.TerrainX = 0.0f;
    g.TerrainZ = 0.0f;
    g.TerrainSizeX = field.Size;
    g.TerrainSizeZ = field.Size;
    g.SunX = grid.SunX;
    g.SunZ = grid.SunZ;
    g.TanElevation = grid.TanElevation;
    g.Texel = grid.Texel;
    g.UMin = grid.UMin;
    g.VMin = grid.VMin;
    g.SamplesU = static_cast<float>(grid.SamplesU);
    g.SamplesV = static_cast<float>(grid.SamplesV);
    return g;
}

constexpr uint32_t kRunLength = 16u;
constexpr uint32_t kRuns = 256u;
static_assert(kRunLength * kRuns == kTerrainShadowLineCapacity, "the kernel's line capacity");

// terrain_shadow_bake.comp for `lines` of the grid, writing into `map` (allocated by the caller);
// `tanLower` is the grid's TanLowerEdge.
void Bake(const Shader::GE_TerrainShadowGrid& g, float tanLower, TerrainShadowLines lines, ClearanceMap& map)
{
    const Shader::vec4 noCaster = Shader::GE_TerrainShadowCasterCarry(Shader::kGE_TerrainShadowNoCaster);
    for (uint32_t line = lines.First; line < lines.First + lines.Count; ++line)
    {
        const float lineF = static_cast<float>(line);
        std::array<Shader::vec4, kRuns> runTop{};
        for (uint32_t run = 0; run < kRuns; ++run)
        {
            Shader::vec4 top = noCaster;
            for (uint32_t i = 0; i < kRunLength; ++i)
            {
                const float caster =
                    Shader::GE_TerrainShadowCasterAndGround(g, static_cast<float>(run * kRunLength + i), lineF).x;
                top = Shader::GE_TerrainShadowHigherCarry(
                    top, Shader::GE_TerrainShadowCarryDownwind(g, Shader::GE_TerrainShadowCasterCarry(caster),
                                                               static_cast<float>(i), tanLower));
            }
            runTop[run] = top;
        }
        for (uint32_t offset = 1u; offset < kRuns; offset <<= 1u)
        {
            std::array<Shader::vec4, kRuns> merged = runTop;
            for (uint32_t run = 0; run + offset < kRuns; ++run)
                merged[run] = Shader::GE_TerrainShadowHigherCarry(
                    runTop[run], Shader::GE_TerrainShadowCarryDownwind(
                                     g, runTop[run + offset], static_cast<float>(offset * kRunLength), tanLower));
            runTop = merged;
        }
        for (uint32_t run = 0; run < kRuns; ++run)
        {
            Shader::vec4 carry = run + 1u < kRuns ? runTop[run + 1u] : noCaster;
            for (int i = static_cast<int>(kRunLength) - 1; i >= 0; --i)
            {
                const uint32_t k = run * kRunLength + static_cast<uint32_t>(i);
                const Shader::vec2 cg = Shader::GE_TerrainShadowCasterAndGround(g, static_cast<float>(k), lineF);
                const Shader::vec4 seen = Shader::GE_TerrainShadowCarryDownwind(g, carry, 1.0f, tanLower);
                if (static_cast<float>(k) <= g.SamplesU - 1.0f)
                {
                    const Shader::vec2 value = Shader::GE_TerrainShadowSampleValue(g, seen, cg.y, tanLower);
                    map.Texels[static_cast<size_t>(line) * map.Side + k] = value.x;
                    map.Distances[static_cast<size_t>(line) * map.Side + k] = value.y;
                }
                carry = Shader::GE_TerrainShadowHigherCarry(Shader::GE_TerrainShadowCasterCarry(cg.x), seen);
            }
        }
    }
}

struct Scene
{
    HeightField Field;
    TerrainShadowExtent Extent;
    TerrainShadowGrid Grid;
    Shader::GE_TerrainShadowGrid ShaderG;
    ClearanceMap Map;
};

TerrainShadowExtent ExtentOf(const HeightField& field)
{
    return {field.Size, field.Size, field.Cells, field.Cells};
}

// Toward the sun at `azimuthDeg` (0 = +X, 90 = +Z) and `elevationDeg` above the horizon.
std::array<float, 3> TowardSun(float azimuthDeg, float elevationDeg)
{
    const float a = azimuthDeg * GameEngine::Mathematics::Pi / 180.0f;
    const float e = elevationDeg * GameEngine::Mathematics::Pi / 180.0f;
    return {std::cos(e) * std::cos(a), std::sin(e), std::cos(e) * std::sin(a)};
}

// The scene's map for a sun at `azimuthDeg`, `elevationDeg` whose disc has the angular radius
// atan(tanHalfAngle) (0: a point sun).
Scene BakeScene(HeightField field, float azimuthDeg, float elevationDeg, float tanHalfAngle = 0.0f)
{
    Scene s;
    s.Field = std::move(field);
    s.Extent = ExtentOf(s.Field);
    const std::array<float, 3> sun = TowardSun(azimuthDeg, elevationDeg);
    s.Grid = *ComputeTerrainShadowGrid(s.Extent, sun.data(), tanHalfAngle);
    s.ShaderG = ShaderGrid(s.Grid, s.Field);
    s.Map.Side = TerrainShadowMapSide(s.Extent);
    s.Map.Texels.assign(static_cast<size_t>(s.Map.Side) * s.Map.Side, 0.0f);
    s.Map.Distances.assign(s.Map.Texels.size(), 0.0f);
    gField = &s.Field;
    Bake(s.ShaderG, s.Grid.TanLowerEdge, {0u, s.Grid.SamplesV}, s.Map);
    return s;
}

// The map's verdict for a receiver: the share of the sun that reaches it, for a sun of angular
// radius atan(tanHalfAngle) (0: a point sun, 1 lit or 0 shadow) and a penumbra radius bounded by
// `maxPenumbraRadius` metres.
float MapLit(const Scene& s, float x, float y, float z, bool onGround, float tanHalfAngle = 0.0f,
             float maxPenumbraRadius = Shader::kGE_TerrainShadowUncappedPenumbra)
{
    gField = &s.Field;
    gMap = &s.Map;
    return Shader::GE_TerrainShadowLit(s.ShaderG, x, y, z, onGround ? 1.0f : 0.0f, tanHalfAngle, maxPenumbraRadius);
}

// The reference: march from the receiver toward the sun over the bilinear field in steps of a
// sixteenth of a cell until the ray leaves the terrain's rectangle; occluded where the field rises
// above the ray. A receiver on the ground starts on the field; others keep their height, a base
// sunk into the terrain lifted onto it.
float MarchLit(const HeightField& f, float azimuthDeg, float elevationDeg, float x, float y, float z, bool onGround)
{
    const std::array<float, 3> sun = TowardSun(azimuthDeg, elevationDeg);
    const float horizontal = std::sqrt(sun[0] * sun[0] + sun[2] * sun[2]);
    const float dx = sun[0] / horizontal;
    const float dz = sun[2] / horizontal;
    const float tanE = sun[1] / horizontal;
    const bool inside = x >= 0.0f && x <= f.Size && z >= 0.0f && z <= f.Size;
    float start = y;
    if (onGround)
        start = f.Sample(x, z);
    else if (inside)
        start = std::max(y, f.Sample(x, z));
    const float highest = *std::max_element(f.Heights.begin(), f.Heights.end());
    const float step = f.Size / static_cast<float>(f.Cells) / 16.0f;
    for (float t = step; start + t * tanE <= highest; t += step)
    {
        const float px = x + dx * t;
        const float pz = z + dz * t;
        const bool movingAway = (px < 0.0f && dx <= 0.0f) || (px > f.Size && dx >= 0.0f) ||
                                (pz < 0.0f && dz <= 0.0f) || (pz > f.Size && dz >= 0.0f);
        if (movingAway)
            return 1.0f;
        // Outside the rectangle nothing casts, but the ray may still enter it.
        const bool inRectangle = px >= 0.0f && px <= f.Size && pz >= 0.0f && pz <= f.Size;
        if (inRectangle && f.Sample(px, pz) > start + t * tanE)
            return 0.0f;
    }
    return 1.0f;
}

// The receivers lie on a square grid over the terrain and a margin around it (the sea), in two
// layers: on the ground (the terrain's own surface; at the base height off the terrain) and 3 m
// above the ground (a mesh; also at the base height off the terrain).
struct Comparison
{
    size_t Mismatches = 0;
    size_t Compared = 0;
    size_t Shadowed = 0;
};

// Map against march over every receiver, excluding those within `edgeCells` grid steps of a
// receiver of the same layer the march judges differently (the edge, where one texel of the map
// decides), and those whose neighbourhood leaves the receiver grid (where an edge just outside it
// cannot be seen). Returns the mismatches away from the edge, the receivers compared and how many of
// them the march shadows.
Comparison CompareWithMarch(const Scene& s, float azimuthDeg, float elevationDeg, float spacing, int edgeCells)
{
    const HeightField& f = s.Field;
    const float margin = 0.5f * f.Size;
    const float origin = -margin;
    const size_t n = static_cast<size_t>((f.Size + 2.0f * margin) / spacing) + 1u;
    auto coord = [origin, spacing](size_t i) { return origin + static_cast<float>(i) * spacing; };
    Comparison c;
    for (int layer = 0; layer < 2; ++layer)
    {
        std::vector<float> march(n * n);
        std::vector<float> height(n * n);
        std::vector<uint8_t> onGround(n * n);
        for (size_t j = 0; j < n; ++j)
        {
            for (size_t i = 0; i < n; ++i)
            {
                const float x = coord(i);
                const float z = coord(j);
                const bool inside = x >= 0.0f && x <= f.Size && z >= 0.0f && z <= f.Size;
                const size_t at = j * n + i;
                onGround[at] = (inside && layer == 0) ? 1u : 0u;
                height[at] = inside ? f.Sample(x, z) + (layer == 0 ? 0.0f : 3.0f) : 0.0f;
                march[at] = MarchLit(f, azimuthDeg, elevationDeg, x, height[at], z, onGround[at] != 0u);
            }
        }
        const size_t inner = static_cast<size_t>(edgeCells);
        for (size_t j = inner; j + inner < n; ++j)
        {
            for (size_t i = inner; i + inner < n; ++i)
            {
                const size_t at = j * n + i;
                bool nearEdge = false;
                for (int dj = -edgeCells; dj <= edgeCells && !nearEdge; ++dj)
                {
                    for (int di = -edgeCells; di <= edgeCells && !nearEdge; ++di)
                    {
                        const long long oi = static_cast<long long>(i) + di;
                        const long long oj = static_cast<long long>(j) + dj;
                        nearEdge = march[static_cast<size_t>(oj) * n + static_cast<size_t>(oi)] != march[at];
                    }
                }
                if (nearEdge)
                    continue;
                ++c.Compared;
                c.Shadowed += march[at] < 0.5f ? 1u : 0u;
                if (MapLit(s, coord(i), height[at], coord(j), onGround[at] != 0u) != march[at])
                    ++c.Mismatches;
            }
        }
    }
    return c;
}

// 64 m terrains at 1 m cells: a terrace (a 10 m step across x = 32), a ridge along z, a cone.
HeightField Terrace()
{
    return MakeField(64u, 64.0f, [](float x, float) { return x < 32.0f ? 10.0f : 0.0f; });
}
HeightField Ridge()
{
    return MakeField(64u, 64.0f, [](float x, float) { return 12.0f * std::exp(-0.5f * (x - 32.0f) * (x - 32.0f) / 16.0f); });
}
HeightField Cone()
{
    return MakeField(64u, 64.0f, [](float x, float z) {
        const float r = std::sqrt((x - 32.0f) * (x - 32.0f) + (z - 32.0f) * (z - 32.0f));
        return std::max(0.0f, 20.0f * (1.0f - r / 15.0f));
    });
}
} // namespace

// The map and the march agree on every receiver away from a shadow's edge: on the ground, above it
// and around the terrain, for three shapes at five azimuths and three elevations.
TEST(TerrainShadowMap, MatchesTheMarchAwayFromTheEdge)
{
    const std::array<float, 5> azimuths = {0.0f, 30.0f, 45.0f, 90.0f, 180.0f};
    const std::array<float, 3> elevations = {5.0f, 15.0f, 45.0f};
    const std::array<std::pair<const char*, HeightField (*)()>, 3> shapes = {
        std::pair{"terrace", &Terrace}, std::pair{"ridge", &Ridge}, std::pair{"cone", &Cone}};
    for (const auto& [name, make] : shapes)
    {
        for (float azimuth : azimuths)
        {
            for (float elevation : elevations)
            {
                const Scene s = BakeScene(make(), azimuth, elevation);
                // Receivers every 0.75 m; the edge band is two of them, one and a half texels.
                const Comparison c = CompareWithMarch(s, azimuth, elevation, 0.75f, 2);
                EXPECT_EQ(c.Mismatches, 0u) << name << " azimuth " << azimuth << " elevation " << elevation << ": "
                                            << c.Mismatches << " of " << c.Compared << " receivers differ";
                EXPECT_GT(c.Shadowed, 0u) << name << " azimuth " << azimuth << " elevation " << elevation
                                          << ": no receiver in shadow, so the comparison shows nothing";
            }
        }
    }
}

// The bake's scan (the runs, their suffix combination, the walk) stores, at every sample of every
// line, the brute-force maximum over the casters upwind of it: top(k) = max over j > k of
// (h(j) - (j - k) * texel * tan(elevation)), less the ground at k; and the distance to the j that
// attains it, (j - k) * texel, wherever one caster attains it clearly.
TEST(TerrainShadowMap, ScanEqualsTheBruteForceMaximum)
{
    const Scene s = BakeScene(Cone(), 30.0f, 15.0f);
    gField = &s.Field;
    const float step = s.ShaderG.Texel * s.ShaderG.TanElevation;
    size_t checked = 0;
    size_t distancesChecked = 0;
    for (uint32_t line = 0; line < s.Grid.SamplesV; ++line)
    {
        std::vector<float> caster(s.Grid.SamplesU);
        std::vector<float> ground(s.Grid.SamplesU);
        for (uint32_t k = 0; k < s.Grid.SamplesU; ++k)
        {
            const Shader::vec2 cg = Shader::GE_TerrainShadowCasterAndGround(s.ShaderG, static_cast<float>(k),
                                                                            static_cast<float>(line));
            caster[k] = cg.x;
            ground[k] = cg.y;
        }
        for (uint32_t k = 0; k < s.Grid.SamplesU; ++k)
        {
            float top = Shader::kGE_TerrainShadowNoCaster;
            float runnerUp = Shader::kGE_TerrainShadowNoCaster;
            uint32_t argmax = 0;
            for (uint32_t j = k + 1; j < s.Grid.SamplesU; ++j)
            {
                const float candidate = caster[j] - static_cast<float>(j - k) * step;
                if (candidate > top)
                {
                    runnerUp = top;
                    top = candidate;
                    argmax = j;
                }
                else
                {
                    runnerUp = std::max(runnerUp, candidate);
                }
            }
            const float expected = std::clamp(top - ground[k], -Shader::kGE_TerrainShadowClearanceLimit,
                                              Shader::kGE_TerrainShadowClearanceLimit);
            const size_t at = static_cast<size_t>(line) * s.Map.Side + k;
            ASSERT_NEAR(s.Map.Texels[at], expected, 1e-3f) << "line " << line << " sample " << k;
            ++checked;
            if (top > Shader::kGE_TerrainShadowNoCaster && top - runnerUp > 1e-2f)
            {
                ASSERT_NEAR(s.Map.Distances[at], static_cast<float>(argmax - k) * s.ShaderG.Texel, 1e-3f)
                    << "line " << line << " sample " << k;
                ++distancesChecked;
            }
        }
    }
    EXPECT_GT(checked, 1000u);
    EXPECT_GT(distancesChecked, 1000u);
}

// An edit re-bakes only the lines whose samples read the edited heights, and that leaves the map
// exactly as a full bake of the edited terrain would.
TEST(TerrainShadowMap, DirtyLineRebakeEqualsAFullRebake)
{
    Scene s = BakeScene(Ridge(), 30.0f, 15.0f);
    HeightField edited = s.Field;
    const float minU = 0.40f, minV = 0.60f, maxU = 0.50f, maxV = 0.70f;
    for (uint32_t j = 0; j <= edited.Cells; ++j)
    {
        for (uint32_t i = 0; i <= edited.Cells; ++i)
        {
            const float u = static_cast<float>(i) / static_cast<float>(edited.Cells);
            const float v = static_cast<float>(j) / static_cast<float>(edited.Cells);
            if (u >= minU && u <= maxU && v >= minV && v <= maxV)
                edited.Heights[static_cast<size_t>(j) * (edited.Cells + 1) + i] += 8.0f;
        }
    }
    const TerrainShadowLines lines = TerrainShadowDirtyLines(s.Grid, s.Extent, minU, minV, maxU, maxV);
    ASSERT_GT(lines.Count, 0u);
    EXPECT_LT(lines.Count, s.Grid.SamplesV / 2u) << "an edit re-bakes a band of lines, not the map";

    s.Field = edited;
    gField = &s.Field;
    Bake(s.ShaderG, s.Grid.TanLowerEdge, lines, s.Map);

    const Scene full = BakeScene(edited, 30.0f, 15.0f);
    size_t differing = 0;
    for (size_t i = 0; i < s.Map.Texels.size(); ++i)
        differing += (s.Map.Texels[i] != full.Map.Texels[i] || s.Map.Distances[i] != full.Map.Distances[i]) ? 1u : 0u;
    EXPECT_EQ(differing, 0u) << "texels the partial re-bake left stale";
}

// The edit's lines are the ones crossing its rectangle across the sun: a rectangle at the far side
// of the terrain from another reaches other lines.
TEST(TerrainShadowMap, DirtyLinesFollowTheRectangle)
{
    const TerrainShadowExtent extent{64.0f, 64.0f, 64u, 64u};
    const std::array<float, 3> sun = TowardSun(0.0f, 20.0f); // u along +X, lines across z
    const TerrainShadowGrid grid = *ComputeTerrainShadowGrid(extent, sun.data(), 0.0f);
    const TerrainShadowLines low = TerrainShadowDirtyLines(grid, extent, 0.1f, 0.10f, 0.9f, 0.15f);
    const TerrainShadowLines high = TerrainShadowDirtyLines(grid, extent, 0.1f, 0.85f, 0.9f, 0.90f);
    ASSERT_GT(low.Count, 0u);
    ASSERT_GT(high.Count, 0u);
    EXPECT_LT(low.First + low.Count, high.First) << "z 6-10 m and z 54-58 m share no line";
    EXPECT_EQ(TerrainShadowDirtyLines(grid, extent, 0.5f, 0.5f, 0.5f, 0.6f).Count, 0u) << "an empty rectangle";
}

// The map's size: one lattice cell per texel while the terrain's diagonal fits the cap, a coarser
// texel beyond it, and every sun's grid within the map.
TEST(TerrainShadowMap, GridFitsTheMapAtEveryAzimuth)
{
    const TerrainShadowExtent lakeside{576.0f, 576.0f, 576u, 576u};
    EXPECT_EQ(TerrainShadowMapSide(lakeside), 816u);
    EXPECT_FLOAT_EQ(TerrainShadowTexel(lakeside), 1.0f);
    const TerrainShadowExtent r4{4096.0f, 4096.0f, 4096u, 4096u};
    EXPECT_EQ(TerrainShadowMapSide(r4), kTerrainShadowMaxSide);
    EXPECT_NEAR(TerrainShadowTexel(r4), 1.4146f, 1e-3f);
    for (const TerrainShadowExtent& extent : {lakeside, r4, TerrainShadowExtent{1000.0f, 250.0f, 500u, 125u}})
    {
        for (float azimuth = 0.0f; azimuth < 360.0f; azimuth += 7.5f)
        {
            const std::array<float, 3> sun = TowardSun(azimuth, 25.0f);
            const auto grid = ComputeTerrainShadowGrid(extent, sun.data(), 0.0f);
            ASSERT_TRUE(grid.has_value());
            EXPECT_LE(grid->SamplesU, TerrainShadowMapSide(extent)) << "azimuth " << azimuth;
            EXPECT_LE(grid->SamplesV, TerrainShadowMapSide(extent)) << "azimuth " << azimuth;
            EXPECT_LE(grid->SamplesU, kTerrainShadowLineCapacity);
        }
    }
}

// No map below the horizon or near the zenith: the terrain then casts no shadow at all.
TEST(TerrainShadowMap, NoGridWithoutALowEnoughSun)
{
    const TerrainShadowExtent extent{64.0f, 64.0f, 64u, 64u};
    const std::array<float, 3> below = TowardSun(10.0f, -3.0f);
    const std::array<float, 3> zenith = TowardSun(10.0f, 89.8f);
    const std::array<float, 3> high = TowardSun(10.0f, 89.0f);
    EXPECT_FALSE(ComputeTerrainShadowGrid(extent, below.data(), 0.0f).has_value());
    EXPECT_FALSE(ComputeTerrainShadowGrid(extent, zenith.data(), 0.0f).has_value());
    EXPECT_TRUE(ComputeTerrainShadowGrid(extent, high.data(), 0.0f).has_value());
}

namespace
{
// A 128 m terrain at 1 m cells with a 10 m plateau over x < 32: a straight edge whose shadow falls
// across level ground toward +X under a sun on the -X side.
HeightField Plateau()
{
    return MakeField(128u, 128.0f, [](float x, float) { return x < 32.0f ? 10.0f : 0.0f; });
}

// The penumbra on the ground across the plateau's shadow edge, along z = 64: the last receiver in
// full shadow and the first fully lit one, walked in steps of 5 mm.
struct Penumbra
{
    float LastShadowed = 0.0f;
    float FirstLit = 0.0f;
};

Penumbra MeasurePenumbra(const Scene& s, float tanHalfAngle, float maxPenumbraRadius)
{
    Penumbra p{};
    for (float x = 50.0f; x < 90.0f; x += 0.005f)
    {
        const float lit = MapLit(s, x, s.Field.Sample(x, 64.0f), 64.0f, true, tanHalfAngle, maxPenumbraRadius);
        if (lit <= 0.0f)
            p.LastShadowed = x;
        if (lit >= 1.0f && p.FirstLit == 0.0f)
            p.FirstLit = x;
    }
    return p;
}
} // namespace

// The terrain's shadow edge is the sun's disc cut by the edge ray over the plateau, as the mesh
// shadows model it: on level ground the penumbra is 2 * r / sin(elevation) wide, with
// r = (D / cos(elevation)) * tan(angular radius) the disc's radius across the ray at the occluder's
// distance D; a bound on r caps it as the cascades' Max Penumbra caps a mesh's; a point sun leaves
// the edge hard.
TEST(TerrainShadowMap, PenumbraWidthMatchesTheSunsDiscAtTheOccludersDistance)
{
    constexpr float kElevationDeg = 15.0f;
    const float tanHalfAngle = std::tan(2.0f * GameEngine::Mathematics::Pi / 180.0f); // a 4 degree sun
    const Scene s = BakeScene(Plateau(), 180.0f, kElevationDeg, tanHalfAngle);
    const float elevation = kElevationDeg * GameEngine::Mathematics::Pi / 180.0f;
    // The crest is the plateau's last lattice sample (x = 31); the grid's samples place it within one
    // texel, which moves the occluder's distance, and so the uncapped width, by up to 0.3 m.
    const float crestX = 31.0f;
    const float distance = 10.0f / std::tan(elevation);
    const float edgeX = crestX + distance;

    const Penumbra soft = MeasurePenumbra(s, tanHalfAngle, Shader::kGE_TerrainShadowUncappedPenumbra);
    const float radius = distance / std::cos(elevation) * tanHalfAngle;
    EXPECT_NEAR(soft.FirstLit - soft.LastShadowed, 2.0f * radius / std::sin(elevation), 0.3f)
        << "shadowed to x " << soft.LastShadowed << ", lit from x " << soft.FirstLit;
    EXPECT_NEAR(0.5f * (soft.FirstLit + soft.LastShadowed), edgeX, s.ShaderG.Texel) << "the penumbra centres on the edge";

    constexpr float kMaxPenumbraRadius = 0.2f;
    const Penumbra capped = MeasurePenumbra(s, tanHalfAngle, kMaxPenumbraRadius);
    EXPECT_NEAR(capped.FirstLit - capped.LastShadowed, 2.0f * kMaxPenumbraRadius / std::sin(elevation), 0.02f)
        << "shadowed to x " << capped.LastShadowed << ", lit from x " << capped.FirstLit;

    const Penumbra hard = MeasurePenumbra(s, 0.0f, Shader::kGE_TerrainShadowUncappedPenumbra);
    EXPECT_LE(hard.FirstLit - hard.LastShadowed, 0.0051f) << "a point sun's edge is one step";
}
