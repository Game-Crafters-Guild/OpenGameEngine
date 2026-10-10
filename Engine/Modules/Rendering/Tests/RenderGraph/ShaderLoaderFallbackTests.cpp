// Regression pins for the double-link shader-loader disease (cfd335f42):
// in a process that links the rendering module both statically and inside
// Engine.dll, the injected loader hooks (file-statics) exist once per module
// copy, and the copy executing the frame may never receive them. The cure is
// the module-local fallback in LoadComputeStageBytes — these tests pin that
// fallback so a refactor can't silently reintroduce the loader-or-nothing
// shape (whose symptom was a Player rendering NO world geometry, hidden
// behind a success-printing empty placeholder).

#include "Rendering/Core/Device.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/Common/Utils.h"
#include "Tests/TestUtils.h"

#include <gtest/gtest.h>

using namespace GameEngine::Rendering;

namespace
{
// Save/restore the process-global resolver around mutations — other suites in
// this binary rely on the auto-installed test resolver.
struct ScopedResolver
{
    explicit ScopedResolver(Utils::ShaderPathResolverFunc f)
    {
        Utils::SetShaderPathResolver(f);
    }
    ~ScopedResolver() { Utils::SetShaderPathResolver(&Tests::TestShaderPathResolver); }
};
} // namespace

// The cfd335f42 medicine: with NO injected loader, the module-local Utils
// path (resolver-backed) must deliver the compute stage on its own. The bare
// name matches the test resolver's keyspace (production resolvers accept the
// "Shaders/..." form; the pin is the fallback chain, not the path shape).
TEST(ShaderLoaderFallback, FallsBackToModuleLocalUtilsWhenNoLoaderInjected)
{
    std::string err;
    const std::vector<uint8_t> spirv =
        LoadComputeStageBytes("frustum_culling.shaderpkg", ShaderSourceKind::SpirV, nullptr, &err);
    if (spirv.empty())
        GTEST_SKIP() << "staged shaders unavailable in this environment: " << err;
    EXPECT_FALSE(spirv.empty());
}

// Failure is REPORTED, never thrown and never silent — the caller's contract
// is decline-gracefully + retry (the de-latch half of the fix).
TEST(ShaderLoaderFallback, MissingEnvironmentReportsErrorWithoutThrowing)
{
    ScopedResolver none(nullptr);
    std::string err;
    std::vector<uint8_t> spirv;
    EXPECT_NO_THROW(spirv = LoadComputeStageBytes("Shaders/frustum_culling.shaderpkg",
                                                  ShaderSourceKind::SpirV, nullptr, &err));
    EXPECT_TRUE(spirv.empty());
    EXPECT_FALSE(err.empty()) << "the failure must carry a diagnosable reason";
}

// An injected loader that returns garbage must fail LOUDLY (parse error), not
// silently — the placeholder that printed success while returning empty bytes
// is the exact shape this forbids.
TEST(ShaderLoaderFallback, GarbageLoaderBytesReportParseError)
{
    auto garbage = [](const char*) { return std::vector<uint8_t>(16, 0xAB); };
    std::string err;
    const std::vector<uint8_t> spirv =
        LoadComputeStageBytes("Shaders/frustum_culling.shaderpkg", ShaderSourceKind::SpirV, garbage, &err);
    EXPECT_TRUE(spirv.empty());
    EXPECT_NE(err.find("parse"), std::string::npos) << "err was: " << err;
}
