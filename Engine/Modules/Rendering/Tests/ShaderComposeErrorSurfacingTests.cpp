// Compile-error surfacing for composed material shaders.
//
// The composed top-level source exists only in memory under a phantom logical
// path; the composer re-anchors it with a cpp-style #line directive so
// diagnostics report REAL files:
//   - an error inside the user's surface .glsl names that file and its true
//     line (the includer names includes by resolved absolute path);
//   - a top-level error (e.g. missing EvaluateSurface, which fails at the
//     adapter's call site) names the real adapter file and its true line, not
//     "<stem>_<hash>_composed.frag:<line+preamble offset>";
//   - on failure the composed source is dumped under <cacheRoot>/Failed and
//     the dump path is appended to the error list;
//   - a missing/mis-signatured EvaluateSurface appends a hint naming the
//     user's surface file.

#include <gtest/gtest.h>

#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderVariantKey.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "TestUtils.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{
namespace fs = std::filesystem;

void WriteTextFile(const fs::path& p, const std::string& contents)
{
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << contents;
}

// Self-cleaning unique temp tree; fresh cache root per test.
struct TempTree
{
    fs::path Root;

    explicit TempTree(const char* prefix)
    {
        static std::atomic<uint32_t> counter{0};
        Root = fs::temp_directory_path() /
               (std::string(prefix) + "_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                "_" + std::to_string(counter.fetch_add(1)));
        fs::create_directories(Root);
    }
    ~TempTree()
    {
        std::error_code ec;
        fs::remove_all(Root, ec);
    }
};

std::string JoinErrors(const std::vector<std::string>& errors)
{
    std::string all;
    for (const auto& e : errors)
    {
        all += e;
        all += '\n';
    }
    return all;
}

MaterialBuildResult BuildSurface(const TempTree& tree, const std::string& surfaceFile,
                                 const std::string& surfaceSource)
{
    const fs::path materialDir = tree.Root / "Materials";
    WriteTextFile(materialDir / surfaceFile, surfaceSource);

    MaterialDocument doc{};
    doc.materialName = "ErrSurfacing";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = surfaceFile;

    MaterialBuildContext ctx{};
    ctx.AdapterShaderDir = GameEngine::Rendering::Tests::GetAdapterShaderDir();
    ctx.CacheRoot = tree.Root / "Cache";

    return BuildMaterialToShaderPackage(doc, materialDir / "err_surfacing.material",
                                        "err_surfacing", ctx, ShaderSourceKind::SpirV, MaterialKeyword::Instanced);
}

#define SKIP_WITHOUT_SHADERC(result)                                                        \
    do                                                                                      \
    {                                                                                       \
        for (const auto& e : (result).errors)                                               \
            if (e.find("shaderc is not available") != std::string::npos)                    \
                GTEST_SKIP() << "shaderc not built into this target";                       \
    } while (0)

// 1-based line of the adapter's EvaluateSurface call site in the REAL
// adapter_forward.glsl — the ground truth the re-anchored diagnostics must hit.
int AdapterEvaluateSurfaceCallLine()
{
    const fs::path adapter = GameEngine::Rendering::Tests::GetAdapterShaderDir() /
                             "Adapters" / "adapter_forward.glsl";
    std::ifstream in(adapter, std::ios::binary);
    if (!in.is_open())
        return -1;
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line))
    {
        ++lineNo;
        if (line.find("= EvaluateSurface(") != std::string::npos)
            return lineNo;
    }
    return -1;
}

} // namespace

TEST(ShaderComposeErrorSurfacing, BrokenSurfaceNamesUserFileAndTrueLine)
{
    TempTree tree("ge_err_userfile");
    // Deliberate syntax error on LINE 6 of the user's file.
    const auto result = BuildSurface(tree, "broken_surface.glsl",
                                     "// line 1\n"
                                     "// line 2\n"
                                     "SurfaceOutput EvaluateSurface(SurfaceInput sIn)\n"
                                     "{\n"
                                     "    SurfaceOutput o = DefaultSurfaceOutput();\n"
                                     "    float bad = ;\n"
                                     "    return o;\n"
                                     "}\n");
    SKIP_WITHOUT_SHADERC(result);
    ASSERT_FALSE(result.success);

    const std::string all = JoinErrors(result.errors);
    EXPECT_NE(all.find("broken_surface.glsl:6"), std::string::npos)
        << "error must attribute to the USER's file at its true line, got:\n" << all;
}

TEST(ShaderComposeErrorSurfacing, MissingEvaluateSurfaceReportsRealAdapterLineAndHint)
{
    const int callLine = AdapterEvaluateSurfaceCallLine();
    ASSERT_GT(callLine, 0) << "could not locate the EvaluateSurface call in adapter_forward.glsl";

    TempTree tree("ge_err_adapterline");
    const auto result = BuildSurface(tree, "empty_surface.glsl", "// defines nothing\n");
    SKIP_WITHOUT_SHADERC(result);
    ASSERT_FALSE(result.success);

    const std::string all = JoinErrors(result.errors);

    // The #line re-anchor: the adapter-side failure names the REAL adapter file
    // at its REAL line, not the phantom composed path at a preamble-shifted line.
    std::ostringstream expected;
    expected << "adapter_forward.glsl:" << callLine;
    EXPECT_NE(all.find(expected.str()), std::string::npos)
        << "expected '" << expected.str() << "' in:\n" << all;

    // No error line may attribute to the phantom composed file. (The "Source:"
    // header names the phantom path without a :<line> suffix; an attribution
    // would appear as "_composed.frag:<line>".)
    EXPECT_EQ(all.find("_composed.frag:"), std::string::npos)
        << "phantom composed-path attribution survived:\n" << all;

    // The hint names the user's surface file and the contract.
    EXPECT_NE(all.find("empty_surface.glsl"), std::string::npos) << all;
    EXPECT_NE(all.find("SurfaceOutput EvaluateSurface(SurfaceInput)"), std::string::npos) << all;
}

TEST(ShaderComposeErrorSurfacing, FailedCompileDumpsComposedSource)
{
    TempTree tree("ge_err_dump");
    const auto result = BuildSurface(tree, "broken_surface.glsl",
                                     "SurfaceOutput EvaluateSurface(SurfaceInput sIn)\n"
                                     "{\n"
                                     "    float bad = ;\n"
                                     "}\n");
    SKIP_WITHOUT_SHADERC(result);
    ASSERT_FALSE(result.success);

    // The failing fragment stage's composed source is dumped and the error
    // list names the dump path.
    const std::string marker = "source dumped to: ";
    fs::path dumpPath;
    for (const auto& e : result.errors)
    {
        const size_t at = e.find(marker);
        if (at != std::string::npos && e.find("Composed fs") != std::string::npos)
            dumpPath = e.substr(at + marker.size());
    }
    ASSERT_FALSE(dumpPath.empty()) << "no fs dump entry in:\n" << JoinErrors(result.errors);
    ASSERT_TRUE(fs::exists(dumpPath)) << dumpPath;

    // Only the FAILING stage dumps — the healthy vertex stage stays quiet.
    EXPECT_EQ(JoinErrors(result.errors).find("Composed vs source dumped"), std::string::npos);

    // The dump is the exact compiled source: preamble + #line anchor + the
    // resolved surface include literal.
    std::ifstream in(dumpPath, std::ios::binary);
    std::string dumped((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_NE(dumped.find("ShaderComposer injected defines"), std::string::npos);
    EXPECT_NE(dumped.find("#line 2 "), std::string::npos);
    EXPECT_NE(dumped.find("broken_surface.glsl"), std::string::npos);
}

TEST(ShaderComposeErrorSurfacing, ValidSurfaceStillCompilesWithLineAnchor)
{
    TempTree tree("ge_err_valid");
    const auto result = BuildSurface(tree, "valid_surface.glsl",
                                     "SurfaceOutput EvaluateSurface(SurfaceInput sIn)\n"
                                     "{\n"
                                     "    SurfaceOutput o = DefaultSurfaceOutput();\n"
                                     "    o.baseColor = Mat.uBaseColor.rgb;\n"
                                     "    o.normalWS = normalize(sIn.normalWS);\n"
                                     "    return o;\n"
                                     "}\n");
    SKIP_WITHOUT_SHADERC(result);
    for (const auto& e : result.errors)
        ADD_FAILURE() << "Build error: " << e;
    ASSERT_TRUE(result.success);
    ASSERT_NE(result.package, nullptr);
    EXPECT_FALSE(result.package->stageBytes.at("vs").empty());
    EXPECT_FALSE(result.package->stageBytes.at("fs").empty());
}
