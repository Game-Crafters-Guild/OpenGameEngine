#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Materials/ShaderMeta.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <algorithm>
#include <string>

using namespace GameEngine::Rendering;

namespace
{
const DescriptorSetMeta* FindSet(const ShaderMeta& meta, uint32_t set)
{
    const auto it = std::find_if(meta.Sets.begin(), meta.Sets.end(),
                                 [set](const DescriptorSetMeta& candidate) { return candidate.Set == set; });
    return it == meta.Sets.end() ? nullptr : &*it;
}

const DescriptorBindingMeta* FindBinding(const DescriptorSetMeta& set, uint32_t binding)
{
    const auto it = std::find_if(set.Bindings.begin(), set.Bindings.end(),
                                 [binding](const DescriptorBindingMeta& candidate)
                                 { return candidate.Binding == binding; });
    return it == set.Bindings.end() ? nullptr : &*it;
}
} // namespace

// The ocean loads every program by name from a cooked package first; a program
// with no package can only compile from source, which no shipped runtime does.
TEST(OceanShaderPackages, EveryRequestedOceanProgramIsCooked)
{
    const char* programs[] = {"ocean_scenegrab_resolve", "ocean_shadow_sim",       "ocean_spline_raster_depth",
                              "ocean_spline_raster_flow", "ocean_spline_raster_clip", "ocean_spline_raster_albedo",
                              "ocean_spray_sim",          "ocean_spray_cull",       "ocean_spray_draw"};
    for (const char* program : programs)
    {
        ShaderPackage package{};
        std::string error;
        EXPECT_TRUE(LoadShaderPkg(std::string("Shaders/") + program + ".shaderpkg", ShaderSourceKind::SpirV,
                                  package, &error))
            << program << ": " << error;
        EXPECT_FALSE(package.stageBytes.empty()) << program;
    }
}

// The MSAA scene grab resolves colour by the depth sample the depth resolve kept:
// colour, depth and the options block are all reflected, so the three-binding
// layout the ocean builds matches the cooked program.
TEST(OceanShaderPackages, SceneGrabResolveReflectsDepthAwareBindings)
{
    ShaderPackage package{};
    std::string error;
    ASSERT_TRUE(LoadShaderPkg("Shaders/ocean_scenegrab_resolve.shaderpkg", ShaderSourceKind::SpirV, package,
                              &error))
        << error;
    const DescriptorSetMeta* set = FindSet(package.meta, 0u);
    ASSERT_NE(set, nullptr);
    const DescriptorBindingMeta* color = FindBinding(*set, 0u);
    const DescriptorBindingMeta* depth = FindBinding(*set, 1u);
    const DescriptorBindingMeta* options = FindBinding(*set, 2u);
    ASSERT_NE(color, nullptr);
    ASSERT_NE(depth, nullptr) << "the package was cooked from the colour-only resolve";
    ASSERT_NE(options, nullptr);
    EXPECT_EQ(color->Name, "uSrc");
    EXPECT_EQ(depth->Name, "uDepth");
}
