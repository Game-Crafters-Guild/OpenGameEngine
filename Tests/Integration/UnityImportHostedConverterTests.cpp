// The unity-import package's converter assembly, run the way the import modal
// runs it: RunConverterHosted binds EditorHost.Run in the vendored
// UnityConverter.dll on the engine's CoreCLR and lists the scenes of a
// synthetic extracted package (the modal's first converter call).

#include "Editor/Assets/UnityImportHostedConverter.h"

#include "Core/Engine.h"
#include "Platform/Process.h"
#include "Scripting/ScriptManager.h"
#include "Scripting/ScriptingABI.h"
#include "StagedTestPaths.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;
using namespace GameEngine;

namespace
{

constexpr const char* kSceneGuid = "0123456789abcdef0123456789abcdef";

void EnsureClrInitialized()
{
    auto& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized())
    {
        ApplicationConfig cfg;
        cfg.AssetDirectory = "Assets";
        ScriptsConfig scriptsConfig{};
        scriptsConfig.enableHotReload = false;
        engine.SetScriptsConfig(scriptsConfig);
        ASSERT_TRUE(engine.Initialize(cfg));
    }
    // Any ABI call bootstraps the runtime lazily; the bogus name only fails.
    int32_t out = 0;
    (void)GE_Invoke(0ULL, "Nope.DoesNotExist", 18, &out);
    ASSERT_TRUE(engine.GetScriptManager().GetCLRHost().IsInitialized());
}

// An extracted .unitypackage: one <guid>/ directory per asset holding the
// asset's project path in "pathname" and its bytes in "asset".
fs::path WriteExtractedPackageWithOneScene()
{
    // Per process: concurrent runs from two build trees must not share it.
    const fs::path root = fs::temp_directory_path() /
                          ("ge-unity-import-hosted-converter-" +
                           std::to_string(Platform::GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / kSceneGuid);
    std::ofstream(root / kSceneGuid / "pathname", std::ios::binary) << "Assets/Scenes/Demo.unity";
    std::ofstream(root / kSceneGuid / "asset", std::ios::binary) << "%YAML 1.1\n";
    return root;
}

} // namespace

TEST(UnityImportHostedConverter, PackageAssemblyListsScenesThroughEditorHostRun)
{
    const fs::path converterDll = TestPaths::ExecutableDirectory() / "TestData" / "Packages" /
                                  "unity-import" / "Tools" / "UnityConverter.dll";
    EnsureClrInitialized();
    const fs::path package = WriteExtractedPackageWithOneScene();

    std::string stdoutAll;
    const std::atomic<bool> cancelRequested{false};
    const ConverterRunResult result = RunConverterHosted(
        converterDll, {"--list-scenes", PathArgUtf8(package)},
        [&stdoutAll](const std::string& line) { stdoutAll += line; }, cancelRequested);

    EXPECT_TRUE(result.Spawned) << result.StderrTail;
    EXPECT_EQ(result.ExitCode, 0) << result.StderrTail;
    EXPECT_NE(stdoutAll.find("\"Demo\""), std::string::npos) << stdoutAll;
    EXPECT_NE(stdoutAll.find(kSceneGuid), std::string::npos) << stdoutAll;

    std::error_code ec;
    fs::remove_all(package, ec);
}
