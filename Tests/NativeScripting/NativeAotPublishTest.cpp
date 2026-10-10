// The NativeAOT script publish of a ship-optimized build: BuildPipeline copies
// the library `dotnet publish` writes, by name, out of the publish directory.
// The name comes from ILCompiler's naming rule, so it is checked against a real
// publish on each host rather than against a platform convention.

#include "Engine/Build/BuildPipeline.h"
#include "Engine/Build/CancellableShellProcess.h"
#include "Engine/Build/NativeAotPublish.h"

#include "../TestTempDir.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace
{

// A cold publish restores the ILCompiler packages before compiling.
constexpr std::chrono::minutes kPublishTimeout{8};

// The properties of BuildPipeline's generated AOT project that decide the
// output name: PublishAot, and the target framework, which picks the ILCompiler
// version. The engine writes the framework as a literal in each project it
// generates, so keep this one the same as CompileScriptsNativeAOT's. The
// publish command adds the runtime identifier and NativeLib=Shared, as the
// pipeline's does.
constexpr const char* kProjectXml = R"(<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net10.0</TargetFramework>
    <PublishAot>true</PublishAot>
  </PropertyGroup>
</Project>
)";

constexpr const char* kExportSource = R"(public static class NativeAotPublishProbe
{
    [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "native_aot_publish_probe")]
    public static int Probe() => 1;
}
)";

void WriteTextFile(const fs::path& path, const char* text)
{
    std::ofstream file(path, std::ios::binary);
    file << text;
}

std::string ListDirectory(const fs::path& directory)
{
    std::string listing;
    std::error_code ec;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory, ec))
        listing += "  " + entry.path().filename().string() + "\n";
    return listing.empty() ? "  (empty or missing)\n" : listing;
}

} // namespace

TEST(NativeAotPublish, PublishWritesTheLibraryTheBuildPipelineCopies)
{
    std::string dotnetError;
    const fs::path dotnet = GameEngine::BuildPipeline::ResolveDotnetForPackaging({}, dotnetError);
    ASSERT_FALSE(dotnet.empty()) << dotnetError;

    using GameEngine::TestUtils::MakeUniqueTempDirectory;
    const GameEngine::TestUtils::ScopedTempDir scratch(MakeUniqueTempDirectory("NativeAotPublish"));
    const fs::path& root = scratch.Path();
    const fs::path project = root / (std::string(GameEngine::kNativeAotProjectName) + ".csproj");
    const fs::path publishDir = root / "publish";
    WriteTextFile(project, kProjectXml);
    WriteTextFile(root / "NativeAotPublishProbe.cs", kExportSource);

    const GameEngine::ShellProcessResult result = GameEngine::RunProcessCaptured(
        dotnet.string(),
        {"publish", project.string(), "-r", std::string(GameEngine::kNativeAotRuntimeIdentifier), "-c", "Release",
         "/p:NativeLib=Shared", "-o", publishDir.string()},
        kPublishTimeout);
    ASSERT_EQ(result.exitCode, 0) << result.output;

    EXPECT_TRUE(fs::is_regular_file(publishDir / GameEngine::kNativeAotPublishedLibraryName))
        << "BuildPipeline copies '" << GameEngine::kNativeAotPublishedLibraryName
        << "', but the publish directory holds:\n"
        << ListDirectory(publishDir);
}
