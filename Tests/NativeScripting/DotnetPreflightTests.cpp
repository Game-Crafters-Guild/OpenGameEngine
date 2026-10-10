// Packaged-C# dotnet preflight (BuildPipeline::ResolveDotnetForPackaging):
// the pipeline must never spawn a bare PATH-dependent "dotnet". Resolution
// goes staged-SDK-manifest first, then DOTNET_ROOT, then PATH, then standard
// install locations, and a machine where nothing resolves gets a loud,
// actionable error before any compile step runs. Env manipulation is
// process-scoped (restored per test) — never machine-wide.

#include "Engine/Build/BuildPipeline.h"
#include "NativeScripting/SdkManifest.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using GameEngine::BuildPipeline;

namespace
{

// Scoped environment override, restored on destruction so later tests in
// this process see the real environment again. An empty value reads as
// unset to the resolver (it ignores empty env vars).
class ScopedEnvOverride
{
  public:
    ScopedEnvOverride(const char* name, const std::string& value) : m_Name(name)
    {
        const char* current = std::getenv(name);
        m_Saved = current ? current : "";
#if defined(_WIN32)
        _putenv_s(name, value.c_str());
#else
        setenv(name, value.c_str(), 1);
#endif
    }

    ~ScopedEnvOverride()
    {
#if defined(_WIN32)
        _putenv_s(m_Name, m_Saved.c_str());
#else
        setenv(m_Name, m_Saved.c_str(), 1);
#endif
    }

  private:
    const char* m_Name;
    std::string m_Saved;
};

fs::path MakeTempDir(const char* tag)
{
    const fs::path dir = fs::temp_directory_path() / tag;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

void WriteFile(const fs::path& p, const std::string& text)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

} // namespace

TEST(DotnetPreflight, SdkManifestRecordedPathWinsEvenWithEmptyPath)
{
    const fs::path root = MakeTempDir("ge_dotnet_preflight_manifest");
    // A fake dotnet executable the manifest records; PATH cleared so only the
    // manifest can produce it.
    const fs::path fakeDotnet = root / "tools" / "dotnet.exe";
    WriteFile(fakeDotnet, "MZ");
    const fs::path sdkRoot = root / "SDK";
    WriteFile(sdkRoot / "nativescripting" / "manifest.txt",
              "config=Debug\n"
              "includedir=include\n"
              "importlib=lib/Debug/Engine.lib\n"
              "dotnet=" + fakeDotnet.generic_string() + "\n");
    // LoadSdkManifest requires the include dir's parent fields only (strings);
    // the resolver does not stat includedir/importlib.

    ScopedEnvOverride emptyPath("PATH", "");
    std::string error;
    const fs::path resolved = BuildPipeline::ResolveDotnetForPackaging(sdkRoot, error);
    EXPECT_EQ(resolved, fakeDotnet) << error;
    EXPECT_TRUE(error.empty());

    std::error_code ec;
    fs::remove_all(root, ec);
}

TEST(DotnetPreflight, WithDotnetOffPathResolutionIsAbsoluteOrFailsWithActionableError)
{
    // The original bug: packaged C# steps spawned a bare "dotnet", so a PATH
    // without it failed deep inside the build with an unactionable shell
    // error. With PATH cleared and no SDK manifest, the preflight must either
    // still resolve an ABSOLUTE existing dotnet (common install locations —
    // machine-dependent) or refuse up front with the loud, actionable text.
    const fs::path sdkRoot = MakeTempDir("ge_dotnet_preflight_nopath") / "SDK"; // no manifest staged

    ScopedEnvOverride emptyPath("PATH", "");
    std::string error;
    const fs::path resolved = BuildPipeline::ResolveDotnetForPackaging(sdkRoot, error);
    if (resolved.empty())
    {
        EXPECT_EQ(error, BuildPipeline::DescribeMissingDotnet(sdkRoot));
    }
    else
    {
        EXPECT_TRUE(resolved.is_absolute()) << resolved.string();
        std::error_code ec;
        EXPECT_TRUE(fs::exists(resolved, ec)) << resolved.string();
    }

    std::error_code ec;
    fs::remove_all(sdkRoot.parent_path(), ec);
}

TEST(DotnetPreflight, MissingDotnetErrorNamesTheFixAndTheSearchOrder)
{
    // The exact text the build reports when nothing resolves: it must name
    // the remedy (install the SDK / fix PATH) and everywhere discovery looked
    // (the staged manifest, PATH, common locations).
    const fs::path sdkRoot = fs::path("C:/fake-editor/SDK");
    const std::string error = BuildPipeline::DescribeMissingDotnet(sdkRoot);
    EXPECT_NE(error.find(".NET 10 SDK"), std::string::npos) << error;
    EXPECT_NE(error.find("add 'dotnet' to PATH"), std::string::npos) << error;
    EXPECT_NE(error.find("C:/fake-editor/SDK/nativescripting/manifest.txt"), std::string::npos) << error;
    EXPECT_NE(error.find("DOTNET_ROOT"), std::string::npos) << error;
    EXPECT_NE(error.find("common install locations"), std::string::npos) << error;
}

TEST(DotnetPreflight, DotnetRootOutranksPathScan)
{
    // Agent/CI shells routinely have no dotnet on PATH; DOTNET_ROOT is the
    // .NET host's own install-root override and must win over the PATH scan
    // (a broken or wrong-arch PATH entry must not shadow the explicit root).
    const fs::path root = MakeTempDir("ge_dotnet_root_precedence");
    const fs::path rootInstall = root / "dotnet-root";
    const fs::path pathInstall = root / "path-dir";
#if defined(_WIN32)
    const char* exeName = "dotnet.exe";
#else
    const char* exeName = "dotnet";
#endif
    WriteFile(rootInstall / exeName, "MZ");
    WriteFile(pathInstall / exeName, "MZ");

    ScopedEnvOverride dotnetRoot("DOTNET_ROOT", rootInstall.string());
    ScopedEnvOverride path("PATH", pathInstall.string());
    EXPECT_EQ(GameEngine::NativeScripting::ResolveDotnetExecutable({}), rootInstall / exeName);

    // With DOTNET_ROOT gone, the PATH scan is next.
    {
        ScopedEnvOverride noRoot("DOTNET_ROOT", "");
        EXPECT_EQ(GameEngine::NativeScripting::ResolveDotnetExecutable({}), pathInstall / exeName);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
}
