#include <gtest/gtest.h>

#include "Panels/ShaderErrorRows.h"

#include "AssetCore/PathNormalization.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

using namespace GameEngine;

namespace
{

// Classification runs paths through std::filesystem and rejects anything that
// is not absolute *on the host*. "C:/proj" is absolute on Windows and a
// relative path everywhere else, so a fixed literal would silently make every
// classification test vacuous off Windows. Build the roots per platform
// instead; the paths stay fictional, they just have to be well-formed here.
#ifdef _WIN32
constexpr const char* kProjectRootLiteral = "C:/proj";
constexpr const char* kEngineRootLiteral = "C:/editor";
#else
constexpr const char* kProjectRootLiteral = "/proj";
constexpr const char* kEngineRootLiteral = "/editor";
#endif

const std::filesystem::path kProjectRoot = kProjectRootLiteral;

// Project root only — with no editor-assets root supplied, engine paths
// display unshortened, which is what the display expectations below pin.
const ShaderErrorPathRoots kRootsNoEditor{kProjectRoot, {}};

std::string ProjectPath(std::string_view relative)
{
    return std::string(kProjectRootLiteral) + "/" + std::string(relative);
}

std::string EnginePath(std::string_view relative)
{
    return std::string(kEngineRootLiteral) + "/" + std::string(relative);
}

Engine::Renderer::ShaderCompileErrorLog::Entry MakeEntry(std::string material, std::string surface,
                                       std::filesystem::path materialAsset,
                                       std::vector<std::string> errors)
{
    Engine::Renderer::ShaderCompileErrorLog::Entry entry;
    entry.MaterialName = std::move(material);
    entry.SurfaceShaderPath = std::move(surface);
    entry.MaterialAssetPath = std::move(materialAsset);
    entry.Errors = std::move(errors);
    return entry;
}

// The engine-content failure the 2026-07-27 walk hit: three shipped editor
// materials failing on the same editor-mount surface, before the author touches
// anything.
Engine::Renderer::ShaderCompileErrorLog::Entry EngineFailure(std::string material)
{
    return MakeEntry(std::move(material), "Surfaces/unlit_solid.glsl", {},
                     {EnginePath("Assets/Materials/Surfaces/unlit_solid.glsl") +
                      ":6: error: 'uniform' : no qualifiers allowed for function return"});
}

} // namespace

TEST(ShaderErrorRowsTests, DiagnosticLineYieldsFileLineAndMessage)
{
    std::string file;
    size_t line = 0;
    std::string message;
    ASSERT_TRUE(ParseShaderDiagnosticLine("C:/proj/assets/NewSurface.glsl:44: error: ';' expected",
                                          file, line, message));
    EXPECT_EQ(file, "C:/proj/assets/NewSurface.glsl");
    EXPECT_EQ(line, 44u);
    EXPECT_EQ(message, "';' expected");
}

TEST(ShaderErrorRowsTests, ContextLineIsNotADiagnostic)
{
    std::string file;
    size_t line = 0;
    std::string message;
    EXPECT_FALSE(ParseShaderDiagnosticLine("Source: my_surface.glsl", file, line, message));
    EXPECT_FALSE(ParseShaderDiagnosticLine("Defines: HAS_UV0 HAS_NORMAL", file, line, message));
    // A path with no line number is a dump location, not a diagnostic.
    EXPECT_FALSE(ParseShaderDiagnosticLine("dumped to .Cache/Shaders/Failed/x.glsl:", file, line,
                                           message));
}

TEST(ShaderErrorRowsTests, PathClassificationFollowsTheProjectRoot)
{
    EXPECT_TRUE(IsProjectOwnedPath(ProjectPath("assets/NewSurface.glsl"), kProjectRoot));
    EXPECT_FALSE(IsProjectOwnedPath(EnginePath("Assets/Materials/Surfaces/unlit_solid.glsl"),
                                    kProjectRoot));
    // An authored reference names no mount on its own.
    EXPECT_FALSE(IsProjectOwnedPath("Surfaces/standard_pbr.glsl", kProjectRoot));
    EXPECT_FALSE(IsProjectOwnedPath({}, kProjectRoot));
    // With no project open nothing can be project-owned.
    EXPECT_FALSE(IsProjectOwnedPath(ProjectPath("assets/NewSurface.glsl"), {}));
}

// The reported defect: the author's own row arrived SEVENTH, under six rows from
// shipped engine content. Whatever else is broken, their file sorts first.
TEST(ShaderErrorRowsTests, ProjectRowsSortAboveEngineRows)
{
    std::vector<Engine::Renderer::ShaderCompileErrorLog::Entry> entries;
    entries.push_back(EngineFailure("World Debug"));
    entries.push_back(EngineFailure("Unlit White (M0)"));
    entries.push_back(EngineFailure("Shadow Only"));
    entries.push_back(MakeEntry("NewSurface", "NewSurface.glsl",
                                ProjectPath("assets/NewSurface.material"),
                                {ProjectPath("assets/NewSurface.glsl") +
                                 ":44: error: ';' expected"}));

    const auto rows = BuildShaderErrorRows(entries, kRootsNoEditor);
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows.front().MaterialName, "NewSurface")
        << "the author's own failure must be the first row, not the last";
    EXPECT_TRUE(rows.front().FromProject);
    for (size_t i = 1; i < rows.size(); ++i)
        EXPECT_FALSE(rows[i].FromProject) << "row " << i << " is engine content";
}

// Mutation guard: dropping the stable_partition (or making the comparison
// unstable) breaks this — engine rows must keep their logged order among
// themselves, and so must project rows.
TEST(ShaderErrorRowsTests, OrderWithinEachGroupIsTheLoggedOrder)
{
    std::vector<Engine::Renderer::ShaderCompileErrorLog::Entry> entries;
    entries.push_back(EngineFailure("EngineA"));
    entries.push_back(MakeEntry("ProjectA", "a.glsl", ProjectPath("assets/a.material"),
                                {ProjectPath("assets/a.glsl") + ":1: error: first"}));
    entries.push_back(EngineFailure("EngineB"));
    entries.push_back(MakeEntry("ProjectB", "b.glsl", ProjectPath("assets/b.material"),
                                {ProjectPath("assets/b.glsl") + ":2: error: second"}));

    const auto rows = BuildShaderErrorRows(entries, kRootsNoEditor);
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows[0].MaterialName, "ProjectA");
    EXPECT_EQ(rows[1].MaterialName, "ProjectB");
    EXPECT_EQ(rows[2].MaterialName, "EngineA");
    EXPECT_EQ(rows[3].MaterialName, "EngineB");
}

// A material inside the project whose diagnostic names an ENGINE include is
// still the author's problem — they are the one who has to act on it.
TEST(ShaderErrorRowsTests, ProjectMaterialFailingInAnEngineIncludeStaysAProjectRow)
{
    std::vector<Engine::Renderer::ShaderCompileErrorLog::Entry> entries;
    entries.push_back(EngineFailure("World Debug"));
    entries.push_back(MakeEntry("MyMaterial", "my_surface.glsl", ProjectPath("assets/my.material"),
                                {EnginePath("Assets/Shaders/Includes/pbr.glsl") +
                                 ":120: error: boom"}));

    const auto rows = BuildShaderErrorRows(entries, kRootsNoEditor);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows.front().MaterialName, "MyMaterial");
    EXPECT_TRUE(rows.front().FromProject);
    // The path itself is engine-side and no editor-assets root was supplied,
    // so it displays unshortened.
    EXPECT_EQ(rows.front().DisplayPath, EnginePath("Assets/Shaders/Includes/pbr.glsl"));
}

TEST(ShaderErrorRowsTests, ProjectRowDisplaysItsProjectRelativePath)
{
    std::vector<Engine::Renderer::ShaderCompileErrorLog::Entry> entries;
    entries.push_back(MakeEntry("NewSurface", "NewSurface.glsl",
                                ProjectPath("assets/NewSurface.material"),
                                {ProjectPath("assets/NewSurface.glsl") +
                                 ":44: error: ';' expected"}));

    const auto rows = BuildShaderErrorRows(entries, kRootsNoEditor);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows.front().DisplayPath, "assets/NewSurface.glsl");
    EXPECT_EQ(rows.front().Line, 44u);
}

// One display spelling for the panel and the inspector's error block:
// project-relative first, then the editor mount alias, then as-is.
TEST(ShaderErrorRowsTests, DisplayPathSpeaksMountsNotMachines)
{
    const ShaderErrorPathRoots roots{kProjectRoot,
                                     {std::filesystem::path(kEngineRootLiteral) / "Assets"}};
    EXPECT_EQ(ShaderErrorDisplayPath(ProjectPath("assets/NewSurface.glsl"), roots),
              "assets/NewSurface.glsl");
    EXPECT_EQ(ShaderErrorDisplayPath(EnginePath("Assets/Examples/Materials/Water/water_stylized.glsl"), roots),
              "editor:Examples/Materials/Water/water_stylized.glsl");
    // Under neither root: as-is, never empty.
    EXPECT_EQ(ShaderErrorDisplayPath(EnginePath("Other/x.glsl"), roots), EnginePath("Other/x.glsl"));
}

// An entry whose errors carry no parseable diagnostic still produces one row —
// a failure that vanishes from the panel is worse than an unattributed one.
TEST(ShaderErrorRowsTests, UnparseableFailureStillProducesARow)
{
    std::vector<Engine::Renderer::ShaderCompileErrorLog::Entry> entries;
    entries.push_back(MakeEntry("Broken", "missing_surface.glsl",
                                ProjectPath("assets/broken.material"),
                                {"Material build failed: surface 'missing_surface.glsl' not found"}));

    const auto rows = BuildShaderErrorRows(entries, kRootsNoEditor);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows.front().MaterialName, "Broken");
    EXPECT_EQ(rows.front().File, "missing_surface.glsl");
    EXPECT_EQ(rows.front().Message,
              "Material build failed: surface 'missing_surface.glsl' not found");
    EXPECT_TRUE(rows.front().FromProject) << "the material is project content";
}

// Renaming (or deleting) a material leaves its logged failures keyed to a path
// nothing can recompile — no later success clears them, so the panel would show
// an error for a file that no longer exists. RemoveMaterialAsset is the rename
// command's purge; entries for other materials are untouched.
//
// The compile path stores the REGISTRY-normalized metadata path (case-folded
// on Windows/macOS) while the purge caller passes the on-disk casing, so the
// two sides deliberately differ here: an exact-compare purge could never
// match a real entry and left phantom rows forever.
TEST(ShaderErrorRowsTests, RemoveMaterialAssetMatchesRegistryNormalizedEntriesFromAnOnDiskCasedPath)
{
    Engine::Renderer::ShaderCompileErrorLog log;
    log.ReportFailure(MakeEntry(
        "NewSurface", "NewSurface.glsl",
        AssetPaths::NormalizeForRegistryKey(ProjectPath("Assets/NewSurface.material")),
        {"Material build failed: ShaderComposer could not produce source."}));
    log.ReportFailure(MakeEntry("Other", "other.glsl", ProjectPath("Assets/other.material"),
                                {ProjectPath("Assets/other.glsl") + ":3: error: ';' expected"}));
    const uint64_t versionBefore = log.Version();

    log.RemoveMaterialAsset(ProjectPath("Assets/NewSurface.material"));

    const auto entries = log.Snapshot();
    ASSERT_EQ(entries.size(), 1u)
        << "an on-disk-cased purge path must clear the registry-normalized entry";
    EXPECT_EQ(entries.front().MaterialName, "Other");
    EXPECT_GT(log.Version(), versionBefore);

    // A path with no entries is a no-op — the panel must not rebuild for it.
    const uint64_t versionAfter = log.Version();
    log.RemoveMaterialAsset(ProjectPath("Assets/NewSurface.material"));
    EXPECT_EQ(log.Version(), versionAfter);
}
