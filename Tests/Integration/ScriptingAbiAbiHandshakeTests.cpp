#include "ManagedFixturePaths.h"
#include <gtest/gtest.h>
#include <filesystem>
#include "Scripting/CoreCLRHost.h"
#include "Scripting/PathResolver.h"
#include "Scripting/ScriptingABI.h"

using namespace std;
namespace fs = std::filesystem;

static fs::path FindFirstExisting(std::initializer_list<fs::path> candidates)
{
    for (const auto& candidate : candidates) {
        if (fs::exists(candidate)) return candidate;
    }
    return {};
}

TEST(ScriptingAbi_Abi, AbiVersion_CurrentMatchesHeader)
{
    uint32_t v = GE_ScriptingGetAbiVersion();
    EXPECT_EQ(v, GE_ABI_VERSION_CURRENT);
}

TEST(ScriptingAbi_Abi, GetInterface_FailsOnMajorMismatch)
{
    const uint32_t bad = GE_ABI_VERSION_ENCODE(GE_ABI_VERSION_MAJOR(GE_ABI_VERSION_CURRENT) + 1u, 0u);
    const void* table = nullptr;
    uint32_t size = 0;
    GE_Result r = GE_GetInterface(bad, &table, &size);
    EXPECT_EQ(r, GE_Result_Fail);
    EXPECT_EQ(table, nullptr);
    EXPECT_EQ(size, 0u);
}

TEST(ScriptingAbi_Abi, CoreBridgeInitialize_ReportsManagedAbiMismatch)
{
    // CoreBridge.Initialize runs once per CoreCLRHost (InitializeCoreBridge is idempotent) and the
    // engine's host has run it by the time this test executes whenever a scripts assembly exists,
    // so the handshake is driven on a host of its own, sharing the process-wide runtime.
    using GameEngine::Tests::ScriptAssembliesDir;
    using GameEngine::Tests::StagedBinDir;
    fs::path coreBridge = FindFirstExisting({
        GameEngine::TestPaths::ExecutableDirectory() / "GameEngine.CoreBridge.dll",
        StagedBinDir() / "GameEngine.CoreBridge.dll",
        ScriptAssembliesDir() / "GameEngine.CoreBridge.dll"
    });
    if (coreBridge.empty()) {
        GTEST_SKIP() << "CoreBridge.dll not found; skipping managed ABI mismatch test";
    }

    fs::path runtimeConfig = GameEngine::ScriptingPaths::ResolveScriptsRuntimeConfig();
    if (runtimeConfig.empty()) {
        runtimeConfig = FindFirstExisting({
            GameEngine::TestPaths::ExecutableDirectory() / "GameEngine.CoreBridge.runtimeconfig.json",
            StagedBinDir() / "GameEngine.CoreBridge.runtimeconfig.json",
            ScriptAssembliesDir() / "GameEngine.CoreBridge.runtimeconfig.json"
        });
    }
    if (runtimeConfig.empty()) {
        GTEST_SKIP() << "No runtimeconfig.json staged; skipping managed ABI mismatch test";
    }

    GameEngine::CoreCLRHost host;
    ASSERT_TRUE(host.Initialize(runtimeConfig));
    host.SetHostSwitches(GE_HostSwitch_ForceAbiMismatch);

    // The managed handshake reports -3; the host records it and refuses to treat the bridge as initialized.
    EXPECT_FALSE(host.InitializeCoreBridge(coreBridge));
    EXPECT_EQ(host.GetLastCoreBridgeInitResult(), -3);

    host.SetHostSwitches(0);
    host.Shutdown();
}
