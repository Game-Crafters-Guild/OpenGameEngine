#include "Assets/ShaderProgramAsset.h"
#include "Core/Engine.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "TestTempDir.h"

#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace GameEngine;
namespace fs = std::filesystem;
namespace
{
std::vector<uint8_t> HostLoader([[maybe_unused]] const char* name) { return {1, 2, 3, 4}; }
fs::path HostResolver(const fs::path& name) { return fs::path("host-shaders") / name; }
std::string HostRebuild(const std::string& name) { return "host rebuild " + name; }

struct HookSnapshot
{
    Rendering::Utils::ShaderFileLoaderFunc Loader = Rendering::Utils::GetShaderFileLoader();
    Rendering::Utils::ShaderPathResolverFunc Resolver = Rendering::Utils::GetShaderPathResolver();
    Rendering::ShaderPackageRebuildActionFunc Rebuild = Rendering::GetShaderPackageRebuildAction();
    fs::path Cache = ShaderProgramAsset::GetShaderCacheRoot();

    void ExpectCurrent() const
    {
        EXPECT_EQ(Rendering::Utils::GetShaderFileLoader(), Loader);
        EXPECT_EQ(Rendering::Utils::GetShaderPathResolver(), Resolver);
        EXPECT_EQ(Rendering::GetShaderPackageRebuildAction(), Rebuild);
        EXPECT_EQ(ShaderProgramAsset::GetShaderCacheRoot(), Cache);
    }
    void Restore() const
    {
        Rendering::Utils::SetShaderFileLoader(Loader);
        Rendering::Utils::SetShaderPathResolver(Resolver);
        Rendering::SetShaderPackageRebuildAction(Rebuild);
        ShaderProgramAsset::SetShaderCacheRoot(Cache);
    }
};

class EngineShaderHookLifetimeTest : public testing::Test
{
protected:
    void SetUp() override
    {
        fs::create_directories(Temporary.Path() / "Assets");
        Config.WorkspaceDirectory = Temporary.Path().string();
        Config.AssetDirectory = (Temporary.Path() / "Assets").string();
        ScriptsConfig scripts;
        scripts.disableClr = true;
        scripts.enableHotReload = false;
        scripts.enableAsyncHotReload = false;
        scripts.enableAutoProjectGeneration = false;
        Engine.SetScriptsConfig(scripts);
        Rendering::Utils::SetShaderFileLoader(&HostLoader);
        Rendering::Utils::SetShaderPathResolver(&HostResolver);
        Rendering::SetShaderPackageRebuildAction(&HostRebuild);
        ShaderProgramAsset::SetShaderCacheRoot(Temporary.Path() / "HostCache");
    }
    void TearDown() override
    {
        Engine.Shutdown();
        Previous.Restore(); // This fixture owns the host callbacks it installed.
        fs::current_path(PreviousDirectory);
    }
    HookSnapshot Previous;
    fs::path PreviousDirectory = fs::current_path();
    TestUtils::ScopedTempDir Temporary{TestUtils::MakeUniqueTempDirectory("shader-hook-lifetime")};
    EngineCore Engine;
    ApplicationConfig Config;
};
}

TEST_F(EngineShaderHookLifetimeTest, ShutdownRestoresAllFourHostValues)
{
    const HookSnapshot host;
    ASSERT_TRUE(Engine.Initialize(Config));
    EXPECT_NE(Rendering::Utils::GetShaderFileLoader(), host.Loader);
    EXPECT_NE(Rendering::Utils::GetShaderPathResolver(), host.Resolver);
    EXPECT_NE(Rendering::GetShaderPackageRebuildAction(), host.Rebuild);
    EXPECT_NE(ShaderProgramAsset::GetShaderCacheRoot(), host.Cache);
    Engine.Shutdown();
    host.ExpectCurrent();
}

TEST_F(EngineShaderHookLifetimeTest, RepeatedInitializeAndShutdownPreserveTheCapturedHost)
{
    const HookSnapshot host;
    ASSERT_TRUE(Engine.Initialize(Config));
    ASSERT_TRUE(Engine.Initialize(Config));
    Engine.Shutdown();
    host.ExpectCurrent();
    ShaderProgramAsset::SetShaderCacheRoot(Temporary.Path() / "NextHostCache");
    const HookSnapshot afterShutdown;
    Engine.Shutdown();
    afterShutdown.ExpectCurrent();
}

TEST_F(EngineShaderHookLifetimeTest, EachLifetimeCapturesItsOwnHostIncludingNullCallbacks)
{
    const HookSnapshot first;
    ASSERT_TRUE(Engine.Initialize(Config));
    Engine.Shutdown();
    first.ExpectCurrent();
    Rendering::Utils::SetShaderFileLoader(nullptr);
    Rendering::Utils::SetShaderPathResolver(nullptr);
    Rendering::SetShaderPackageRebuildAction(nullptr);
    ShaderProgramAsset::SetShaderCacheRoot({});
    const HookSnapshot second;
    ASSERT_TRUE(Engine.Initialize(Config));
    Engine.Shutdown();
    second.ExpectCurrent();
}
