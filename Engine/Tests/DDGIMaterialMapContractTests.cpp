// The DDGI material map atlas feeds BOTH trace lanes, and the two must shade a
// hit identically or their images stop being comparable (DDGIMaterialMapAtlas.h).
// This pins the parts of that contract that live in GLSL and have no CPU
// surface to link against — the same artifact-under-test approach as
// DDGIReflectionParallaxTests.
//
// The metallic-roughness layer is the one worth a gate: without it a glTF
// material with the default metallicFactor of 1 and a metallic map reads as a
// metal in the probe trace while the raster surface renders it dielectric, so
// every hit bounces nothing and the probe field holds only sky. That failure
// is silent — GI stays "on", the scene just loses its colour bounce.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "Engine/Rendering/DDGIMaterialMapAtlas.h"
#include "SceneBvh/UberMaterial.h"

namespace
{
std::string ReadShaderSource(const char* relativePath)
{
#ifdef GE_RENDERER_REPO_ROOT
    std::ifstream file(std::filesystem::path(GE_RENDERER_REPO_ROOT) / relativePath);
    if (!file)
        return {};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
#else
    (void)relativePath;
    return {};
#endif
}
}  // namespace

TEST(DDGIMaterialMapContractTests, DefaultRecordBindsNoMapOnAnyLayer)
{
    const GameEngine::Engine::Renderer::DDGIMaterialMapAtlas::MaterialRecord record{};
    EXPECT_EQ(record.AlbedoLayer, GameEngine::Engine::Renderer::DDGIMaterialMapAtlas::kNoLayer);
    EXPECT_EQ(record.EmissiveLayer, GameEngine::Engine::Renderer::DDGIMaterialMapAtlas::kNoLayer);
    EXPECT_EQ(record.MetallicLayer, GameEngine::Engine::Renderer::DDGIMaterialMapAtlas::kNoLayer)
        << "a fresh record must keep the flat metallic factor, or every unbound material reads "
           "layer 0's blue channel as its metalness";
}

TEST(DDGIMaterialMapContractTests, SoftwareRecordReservesTheMetalnessMapSlot)
{
    // ddgi_trace_sw.comp reads the metallic-roughness layer from packed slot
    // 15 of the uber-material record; the packer memcpys the struct, so the
    // field's byte offset IS the slot.
    EXPECT_EQ(offsetof(GameEngine::SceneBvh::UberMaterial, MetalnessMapLayer), 15u * sizeof(float));
    EXPECT_EQ(offsetof(GameEngine::SceneBvh::UberMaterial, AlbedoMapLayer), 12u * sizeof(float));
    EXPECT_EQ(offsetof(GameEngine::SceneBvh::UberMaterial, EmissiveMapLayer), 16u * sizeof(float));
}

TEST(DDGIMaterialMapContractTests, BothTraceLanesApplyTheMetallicMap)
{
#ifndef GE_RENDERER_REPO_ROOT
    GTEST_SKIP() << "GE_RENDERER_REPO_ROOT not defined (dev-only shader-source anchor)";
#else
    const std::string hitShade =
        ReadShaderSource("Engine/Modules/Rendering/Shaders/Includes/ddgi_hit_shade.glsl");
    const std::string hw = ReadShaderSource("Engine/Modules/Rendering/Shaders/ddgi_trace_hw.comp");
    const std::string sw = ReadShaderSource("Engine/Modules/Rendering/Shaders/ddgi_trace_sw.comp");
    ASSERT_FALSE(hitShade.empty());
    ASSERT_FALSE(hw.empty());
    ASSERT_FALSE(sw.empty());

    // The helper samples the glTF metallic channel (blue), the same channel
    // Surfaces/standard_pbr.glsl multiplies the factor by.
    EXPECT_NE(hitShade.find("float GE_DDGIApplyMetallicMap(float metallicFactor, float layer, vec2 uv)"),
              std::string::npos);
    EXPECT_NE(hitShade.find("textureLod(ge_ddgiMapAtlas, vec3(uv, layer), 0.0).b;"), std::string::npos)
        << "metalness must come from the map's blue channel (glTF packing)";

    // Hardware lane: the map table's z lane, keyed by the same materialIndex.
    EXPECT_NE(hw.find("GE_DDGIApplyMetallicMap(GE_DDGIMatMetalRough(mat).x, maps.Layers.z, mapUV)"),
              std::string::npos)
        << "the hardware lane no longer multiplies the metallic factor by its map";
    // Software lane: packed slot 15 (UberMaterial::MetalnessMapLayer).
    EXPECT_NE(sw.find("ge_ddgiSwPacked[materialRecord + 15u], mapUV)"), std::string::npos)
        << "the software lane no longer reads the metalness map layer from slot 15";

    // Neither lane may feed the raw factor to the Lambert term any more.
    EXPECT_EQ(hw.find("GE_DDGIDiffuseAlbedo(albedo, GE_DDGIMatMetalRough(mat).x);"), std::string::npos);
    EXPECT_EQ(sw.find("GE_DDGIDiffuseAlbedo(albedo, ge_ddgiSwPacked[materialRecord + 4u]);"),
              std::string::npos);
#endif
}
