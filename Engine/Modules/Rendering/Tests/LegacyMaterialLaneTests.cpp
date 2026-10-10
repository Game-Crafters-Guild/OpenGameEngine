// The legacy lane map has a C++ half (kLegacyMaterialLanes, read by the material
// registry, the composer and the DDGI bake) and a GLSL half
// (Shaders/Includes/material_param_lanes.glsl, the alias macros an engine surface
// compiles against). Nothing in the build makes the two agree — a surface reading
// Mat.uParams3.y and a registry writing alphaCutoff somewhere else both compile.
// These tests read the GLSL and assert it against the table.

#include "Rendering/Materials/LegacyMaterialLanes.h"
#include "Rendering/Materials/MaterialParamsLayout.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>

#ifndef RENDERING_SOURCE_DIR
#error "RENDERING_SOURCE_DIR must be defined by CMake (rendering_test_shader_paths)"
#endif

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{
namespace fs = std::filesystem;

// name -> lane index, from the `#define <name> uParams[<n>]` lines.
std::map<std::string, uint32_t> ReadGlslLaneAliases()
{
    const fs::path path =
        fs::path(RENDERING_SOURCE_DIR) / "Shaders" / "Includes" / "material_param_lanes.glsl";
    std::ifstream in(path);
    EXPECT_TRUE(in.good()) << "cannot read " << path;

    const std::regex pattern(R"(^#define\s+(\w+)\s+uParams\[(\d+)\]\s*$)");
    std::map<std::string, uint32_t> aliases;
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::smatch m;
        if (std::regex_match(line, m, pattern))
            aliases[m[1].str()] = static_cast<uint32_t>(std::stoul(m[2].str()));
    }
    return aliases;
}

// accessor -> lane index, from the fixed-lane accessors the DDGI hardware trace
// reads the block through: `vec4 GE_DDGIMat<Name>(GE_DDGIMaterialData m) { return m.uParams[<n>]; }`.
std::map<std::string, uint32_t> ReadDdgiHardwareAccessorLanes()
{
    const fs::path path =
        fs::path(RENDERING_SOURCE_DIR) / "Shaders" / "Includes" / "ddgi_hit_shade.glsl";
    std::ifstream in(path);
    EXPECT_TRUE(in.good()) << "cannot read " << path;

    const std::regex pattern(
        R"(^vec4\s+(GE_DDGIMat\w+)\(GE_DDGIMaterialData\s+m\)\s*\{\s*return\s+m\.uParams\[(\d+)\];\s*\}.*$)");
    std::map<std::string, uint32_t> accessors;
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::smatch m;
        if (std::regex_match(line, m, pattern))
            accessors[m[1].str()] = static_cast<uint32_t>(std::stoul(m[2].str()));
    }
    return accessors;
}

} // namespace

TEST(LegacyMaterialLaneTests, GlslAliasesCoverTheWholeBlockInOrder)
{
    const auto aliases = ReadGlslLaneAliases();

    // uBaseColor is lane 0 and uParamsN is lane N+1 — the mapping every comment in
    // the C++ half cites. uUser0..3 are the four generic lanes at the top of the block.
    ASSERT_TRUE(aliases.count("uBaseColor")) << "material_param_lanes.glsl lost its uBaseColor alias";
    EXPECT_EQ(aliases.at("uBaseColor"), 0u);
    for (uint32_t n = 0; n <= 24; ++n)
    {
        const std::string name = "uParams" + std::to_string(n);
        ASSERT_TRUE(aliases.count(name)) << name << " has no alias";
        EXPECT_EQ(aliases.at(name), n + 1) << name;
    }
    for (uint32_t n = 0; n < 4; ++n)
    {
        const std::string name = "uUser" + std::to_string(n);
        ASSERT_TRUE(aliases.count(name)) << name << " has no alias — the graph editor's live "
                                                    "preview compiles Mat." + name + " into the "
                                                    "surface it previews";
        EXPECT_EQ(aliases.at(name), 26u + n) << name;
    }
    EXPECT_EQ(aliases.size(), 30u) << "an alias exists that this test does not know about";
}

TEST(LegacyMaterialLaneTests, EveryAliasedLaneIsInsideTheBlock)
{
    for (const auto& [name, lane] : ReadGlslLaneAliases())
        EXPECT_LT(lane, kMaterialParamLaneCount)
            << name << " aliases a lane past the end of MaterialGpuParams";
}

TEST(LegacyMaterialLaneTests, TableEntriesAgreeWithTheGlslTheSurfacesCompileAgainst)
{
    const auto aliases = ReadGlslLaneAliases();

    // Every table entry addresses a lane that the GLSL half also names, at the
    // same index, and stays inside that lane's four floats. That is what makes
    // "registry writes alphaCutoff at MaterialParamLaneOffset(4)+4" and "the
    // surface reads Mat.uParams3.y" the same bytes.
    for (const LegacyMaterialLane& lane : kLegacyMaterialLanes)
    {
        const std::string alias =
            lane.Lane == 0 ? "uBaseColor"
                           : (lane.Lane >= 26 ? "uUser" + std::to_string(lane.Lane - 26)
                                              : "uParams" + std::to_string(lane.Lane - 1));
        ASSERT_TRUE(aliases.count(alias)) << lane.Name << " sits on unaliased lane " << lane.Lane;
        EXPECT_EQ(aliases.at(alias), lane.Lane) << lane.Name;
        EXPECT_LE(lane.Component + lane.Components, 4u)
            << lane.Name << " spills past the end of its lane";
        EXPECT_GT(lane.Components, 0u) << lane.Name;
        EXPECT_LT(LegacyLaneByteOffset(lane) + LegacyLaneByteSize(lane),
                  kMaterialParamBlockBytes + 1u)
            << lane.Name << " writes past the param block";
    }
}

TEST(LegacyMaterialLaneTests, TheAdapterReadsResolveToTheOffsetsTheRegistryWrites)
{
    // The four names the forward adapter declares and reads through Props on an
    // undeclared surface. The composer splices LegacyMaterialLaneGlsl for each;
    // the registry writes the authored value at LegacyLaneByteOffset. These are
    // the two ends of the same wire, spelled out so a table edit that moves one
    // without the other fails here rather than in a frame.
    struct Expected { const char* Name; const char* Glsl; uint32_t Offset; uint32_t Size; };
    const Expected expected[] = {
        {"alphaCutoff", "uParams[4].y", MaterialParamLaneOffset(4) + 4, 4},
        {"specularIor", "uParams[15].x", MaterialParamLaneOffset(15) + 0, 4},
        {"transmissionColor", "uParams[16].xyz", MaterialParamLaneOffset(16) + 0, 12},
        {"transmissionWeight", "uParams[16].w", MaterialParamLaneOffset(16) + 12, 4},
    };
    for (const Expected& e : expected)
    {
        const LegacyMaterialLane* lane = FindLegacyMaterialLane(e.Name);
        ASSERT_NE(lane, nullptr) << e.Name << " left the legacy table";
        EXPECT_EQ(LegacyMaterialLaneGlsl(*lane), e.Glsl) << e.Name;
        EXPECT_EQ(LegacyLaneByteOffset(*lane), e.Offset) << e.Name;
        EXPECT_EQ(LegacyLaneByteSize(*lane), e.Size) << e.Name;
    }
}

TEST(LegacyMaterialLaneTests, NoTwoNamesShareAPlacementWithDifferentWidths)
{
    // Aliasing is deliberate (userVec0 covers user0..user3; families that never
    // ship together overlap), but two spellings of the SAME first byte must cover
    // the same floats or one of them silently truncates the other's write.
    std::map<uint32_t, const LegacyMaterialLane*> byOffset;
    for (const LegacyMaterialLane& lane : kLegacyMaterialLanes)
    {
        const auto [it, inserted] = byOffset.emplace(LegacyLaneByteOffset(lane), &lane);
        if (!inserted && it->second->Components != lane.Components)
        {
            // Distinct families own distinct names at one offset by design; only a
            // width disagreement is a bug, and only within one family's names.
            EXPECT_NE(std::string(lane.Name).substr(0, 6),
                      std::string(it->second->Name).substr(0, 6))
                << lane.Name << " and " << it->second->Name
                << " start at the same byte with different widths";
        }
    }
}

TEST(LegacyMaterialLaneTests, TheDdgiHardwareAccessorsReadTheLanesTheTablePlaces)
{
    // The hardware trace cannot follow a declared table (GE_DDGIMaterialData has
    // no per-material lane map — design doc §10), so it reads three names at
    // their legacy placements through fixed-lane accessors, while the CPU bake
    // reads the same names by name. Two ends of one wire: an accessor that drifts
    // from the table shades every hardware-traced bounce from the wrong lane, and
    // no compile notices. The component pins say which floats the accessors'
    // consumers take (.x metallic, .y roughness, .rgb tint, .w nits).
    const auto accessors = ReadDdgiHardwareAccessorLanes();
    struct Expected { const char* Accessor; const char* Name; uint32_t Component; uint32_t Components; };
    const Expected expected[] = {
        {"GE_DDGIMatBaseColor", "baseColor", 0, 4},
        {"GE_DDGIMatMetalRough", "metallic", 0, 1},
        {"GE_DDGIMatMetalRough", "roughness", 1, 1},
        {"GE_DDGIMatEmission", "emissive", 0, 3},
        {"GE_DDGIMatEmission", "emissionLuminance", 3, 1},
    };
    for (const Expected& e : expected)
    {
        ASSERT_TRUE(accessors.count(e.Accessor))
            << e.Accessor << " is not a fixed-lane accessor in ddgi_hit_shade.glsl";
        const LegacyMaterialLane* lane = FindLegacyMaterialLane(e.Name);
        ASSERT_NE(lane, nullptr) << e.Name << " left the legacy table";
        EXPECT_EQ(accessors.at(e.Accessor), lane->Lane)
            << e.Accessor << " reads a different lane than the table places " << e.Name;
        EXPECT_EQ(lane->Component, e.Component) << e.Name << " moved within its lane";
        EXPECT_EQ(lane->Components, e.Components) << e.Name;
    }
    EXPECT_EQ(accessors.size(), 3u) << "an accessor exists that this test does not know about";
}
