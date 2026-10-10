// Staged engine assets do not live at one fixed offset from the executable: a
// macOS app bundle (the Editor, the Player) puts them at Contents/Resources/Assets,
// a sibling of the Contents/MacOS directory the executable sits in, while every
// other layout stages them at <exe>/Assets. PathUtils::GetInstallAssetsRoot() owns
// that rule; a lookup that joins the executable directory to "Assets" itself is
// right on Windows and Linux and silently wrong in a macOS bundle, and a lookup that
// spells the bundle's "Resources"/"Assets" sibling by hand is a second copy of
// the rule that drifts the moment the rule changes (it did: #1346 removed the
// existence probe from the root, and twenty-five hand-spelled copies kept it).
//
// Silently is the operative word: the loaders that hit this fall back to a C++
// default and log a warning, so the process keeps running and the asset simply
// never loads. graph-ported-node.uxml did exactly that on macOS from the day it
// was added.
//
// COVERAGE, stated so nobody reads more into a green than it carries. The scan
// is textual over the production source roots (Apps/Editor/Source,
// Apps/Player/Source, Engine/Source, Engine/Modules/*/Source; test directories
// excluded, and the Include/ trees are not scanned — a header that resolved a
// path inline would escape it): from each mention of the executable directory it
// reads one join expression and flags an "Assets" segment there, written as a
// literal or behind a constant whose own definition carries the prefix; and it
// flags any join that spells "Resources" / "Assets". It CANNOT see a lookup
// assembled across statements, one built by string concatenation (a
// std::string("Assets/") + "Shaders/..." join passes), or an "Assets" prefix
// that arrives through a function return; it flags a bare "Assets/Shaders"
// literal, the engine shader tree named relative to the working directory, but
// not other "Assets/" literals, which name project content legitimately across
// the tree. Engine packages (Packages/) resolve their own paths and are not scanned.
// Comment detection is per line: a spelling inside a /* */ body whose line does
// not start with '*' is reported, and a real join on a line that also carries
// a "//" is not. The behavioural cases over each loader are what cover the rest.
//
// Two files may spell the rule: Engine/Source/Core/PathUtils.cpp, which owns it,
// and Engine/Modules/Rendering/Source/ShaderGraph/SgGraphFileIO.cpp, whose module
// cannot link PathUtils (GameEngineRendering is linked standalone by its own test
// executables) and whose copy ShaderGraphNodesRootTests pins to the owner.
//
// Source-level invariant, so it reads the source tree (GE_REPO_SOURCE_DIR), the
// same way DebugServerReadPurityTests and SceneViewSnapshotCacheGuardTests do.

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace fs = std::filesystem;

std::string ReadFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

size_t LineNumberAt(const std::string& text, size_t at)
{
    return 1 + static_cast<size_t>(
        std::count(text.begin(), text.begin() + static_cast<long>(at), '\n'));
}

bool StartsCommentOrDirective(const std::string& text, size_t at)
{
    const size_t lineStart = text.rfind('\n', at);
    const std::string_view head(text.data() + (lineStart == std::string::npos ? 0 : lineStart + 1),
                                at - (lineStart == std::string::npos ? 0 : lineStart + 1));
    if (head.find("//") != std::string_view::npos)
        return true;
    const size_t firstGlyph = head.find_first_not_of(" \t");
    return firstGlyph != std::string_view::npos && (head[firstGlyph] == '#' || head[firstGlyph] == '*');
}

// Does any definition of this identifier bind it to a path literal that starts
// at an "Assets" segment? That is the staged-asset prefix the install assets root
// already supplies, so joining it to the executable directory is the defect
// wearing a constant's name — the shape graph-ported-node.uxml's lookup had.
bool IdentifierNamesAnAssetsRelativePath(const std::string& text, std::string_view identifier)
{
    size_t at = text.find(identifier);
    while (at != std::string::npos)
    {
        const size_t lineStart = text.rfind('\n', at);
        const size_t lineEnd = text.find('\n', at);
        const size_t from = lineStart == std::string::npos ? 0 : lineStart + 1;
        const std::string_view line(text.data() + from,
                                    (lineEnd == std::string::npos ? text.size() : lineEnd) - from);
        if (line.find('=') != std::string_view::npos &&
            (line.find("\"Assets/") != std::string_view::npos ||
             line.find("\"Assets\\") != std::string_view::npos))
            return true;
        at = text.find(identifier, at + identifier.size());
    }
    return false;
}

std::vector<std::string> FindViolations(const fs::path& file, const std::string& text)
{
    // How the executable directory is spelled at a use site.
    static constexpr std::string_view kExeDirNames[] = {"GetExecutableDirectory", "exeDir",
                                                        "editorBinDir"};
    // The bundle's asset root spelled by hand instead of asked of the rule, and the engine
    // shader tree named as a bare literal — a path that resolves against the working directory,
    // which is the project root in the editor.
    static constexpr std::string_view kBundleAssetsSpellings[] = {"\"Resources\" / \"Assets\"",
                                                                  "\"Resources/Assets", "\"Assets/Shaders"};

    std::vector<std::string> out;
    const auto report = [&](size_t at) {
        out.push_back(file.filename().string() + ":" + std::to_string(LineNumberAt(text, at)));
    };

    for (std::string_view spelling : kBundleAssetsSpellings)
    {
        size_t at = text.find(spelling);
        while (at != std::string::npos)
        {
            if (!StartsCommentOrDirective(text, at))
                report(at);
            at = text.find(spelling, at + spelling.size());
        }
    }

    for (std::string_view exeDirName : kExeDirNames)
    {
        size_t at = text.find(exeDirName);
        while (at != std::string::npos)
        {
            const size_t next = at + exeDirName.size();
            if (StartsCommentOrDirective(text, at))
            {
                at = text.find(exeDirName, next);
                continue;
            }

            // One join expression: the executable directory up to the end of
            // the statement, or of this element in a braced list. Stopping at
            // the comma is what keeps a sibling repoRoot candidate in the same
            // initializer from reading as this one's operand.
            const size_t stop = std::min(text.find(';', next), text.find(',', next));
            const std::string_view expr(text.data() + next,
                                        (stop == std::string::npos ? text.size() : stop) - next);

            bool violation = expr.find("\"Assets\"") != std::string_view::npos ||
                             expr.find("\"Assets/") != std::string_view::npos;

            // The same join written through a named constant.
            if (!violation)
            {
                size_t slash = expr.find('/');
                while (!violation && slash != std::string_view::npos)
                {
                    const size_t identStart = expr.find_first_not_of(" \t\n", slash + 1);
                    if (identStart == std::string_view::npos)
                        break;
                    const size_t identEnd = expr.find_first_not_of(
                        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_", identStart);
                    const std::string_view identifier =
                        expr.substr(identStart, (identEnd == std::string_view::npos ? expr.size() : identEnd) - identStart);
                    if (!identifier.empty() && !std::isdigit(static_cast<unsigned char>(identifier[0])))
                        violation = IdentifierNamesAnAssetsRelativePath(text, identifier);
                    slash = expr.find('/', slash + 1);
                }
            }

            if (violation)
                report(at);
            at = text.find(exeDirName, next);
        }
    }

    // One report per offending line: a hand-spelled bundle probe joined to the
    // executable directory is caught by both scans above.
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool IsTestPath(const fs::path& relative)
{
    for (const fs::path& part : relative)
    {
        if (part == "Tests")
            return true;
    }
    return false;
}

bool IsExempt(const fs::path& relative)
{
    static const fs::path kOwners[] = {
        fs::path("Engine") / "Source" / "Core" / "PathUtils.cpp",
        fs::path("Engine") / "Modules" / "Rendering" / "Source" / "ShaderGraph" / "SgGraphFileIO.cpp",
    };
    return std::find(std::begin(kOwners), std::end(kOwners), relative) != std::end(kOwners);
}

std::vector<fs::path> ProductionSourceRoots(const fs::path& repo)
{
    std::vector<fs::path> roots = {repo / "Apps" / "Editor" / "Source", repo / "Apps" / "Player" / "Source",
                                   repo / "Engine" / "Source"};
    for (const auto& module : fs::directory_iterator(repo / "Engine" / "Modules"))
    {
        if (module.is_directory() && fs::is_directory(module.path() / "Source"))
            roots.push_back(module.path() / "Source");
    }
    return roots;
}

} // namespace

TEST(StagedAssetPathGuardTests, NoProductionSourceResolvesStagedAssetsBesideTheRule)
{
    const fs::path repo(GE_REPO_SOURCE_DIR);
    std::vector<std::string> violations;
    size_t scanned = 0;
    for (const fs::path& root : ProductionSourceRoots(repo))
    {
        ASSERT_TRUE(fs::is_directory(root)) << root.string();
        for (const auto& entry : fs::recursive_directory_iterator(root))
        {
            if (!entry.is_regular_file())
                continue;
            const fs::path& file = entry.path();
            if (file.extension() != ".cpp" && file.extension() != ".h" && file.extension() != ".mm")
                continue;
            const fs::path relative = fs::relative(file, repo);
            if (IsTestPath(relative) || IsExempt(relative))
                continue;

            ++scanned;
            for (std::string& hit : FindViolations(file, ReadFile(file)))
                violations.push_back(std::move(hit));
        }
    }

    // A scan that read nothing is a false green, not a pass.
    EXPECT_GT(scanned, 1000u);

    std::string report;
    for (const std::string& v : violations)
        report += "\n  " + v;
    EXPECT_TRUE(violations.empty())
        << "Resolve staged engine assets through PathUtils::GetInstallAssetsRoot() (Editor code:"
           " Editor::GetEditorGlobalPaths().installAssetsRoot) — <exe>/Assets does not exist inside"
           " a macOS app bundle and Resources/Assets exists only in one. Offenders:"
        << report;
}

// The guard is only as good as its ability to see the shapes it forbids.
TEST(StagedAssetPathGuardTests, TheScannerSeesEverySpellingAndLeavesLegalJoinsAlone)
{
    const std::string literalJoin =
        "const auto path = PathUtils::GetExecutableDirectory() / \"Assets/UI/Graph/x.uxml\";";
    EXPECT_EQ(FindViolations("Literal.cpp", literalJoin).size(), 1u);

    // The spelling the defect actually wore: the "Assets" prefix behind a name.
    const std::string constantJoin =
        "constexpr const char* kRel = \"Assets/UI/Graph/x.uxml\";\n"
        "const auto path = PathUtils::GetExecutableDirectory() / kRel;";
    EXPECT_EQ(FindViolations("Constant.cpp", constantJoin).size(), 1u);

    // The bundle sibling spelled by hand, with either separator style.
    const std::string bundleProbe =
        "const auto fonts = exeDir / \"..\" / \"Resources\" / \"Assets\" / \"Fonts\";";
    EXPECT_EQ(FindViolations("Bundle.cpp", bundleProbe).size(), 1u);
    const std::string bundleProbeSlashes = "shaderSrc = root.parent_path() / \"Resources/Assets/Shaders\";";
    EXPECT_EQ(FindViolations("BundleSlashes.cpp", bundleProbeSlashes).size(), 1u);
    const std::string bundleProbeInComment = "// the Editor stages Assets under \"Resources\" / \"Assets\"\n";
    EXPECT_TRUE(FindViolations("BundleComment.cpp", bundleProbeInComment).empty());

    // The engine shader tree as a working-directory literal: the project root in the editor.
    const std::string cwdShader = "spirv = Rendering::Utils::ReadFile(\"Assets/Shaders/animation_skinning.comp.spv\");";
    EXPECT_EQ(FindViolations("Cwd.cpp", cwdShader).size(), 1u);

    const std::string resolved =
        "constexpr const char* kRel = \"UI/Graph/x.uxml\";\n"
        "const auto path = PathUtils::GetInstallAssetsRoot() / kRel;";
    EXPECT_TRUE(FindViolations("Resolved.cpp", resolved).empty());

    // A project-relative Assets join is a different thing entirely and stays legal.
    const std::string projectRelative = "const auto dir = projectRoot / \"Assets\" / \"Materials\";";
    EXPECT_TRUE(FindViolations("Project.cpp", projectRelative).empty());

    // Non-asset payloads genuinely do sit beside the executable on every
    // platform, macOS bundles included; the guard must not chase them, and a
    // bundle's other resources (notices, the ICD) are not the asset root.
    const std::string exeSibling = "const auto sdk = PathUtils::GetExecutableDirectory() / \"SDK\";";
    EXPECT_TRUE(FindViolations("Sibling.cpp", exeSibling).empty());
    const std::string notices = "const auto dst = contentRoot.parent_path() / \"Resources\" / \"ThirdPartyNotices\";";
    EXPECT_TRUE(FindViolations("Notices.cpp", notices).empty());

    // A sibling candidate in the same initializer is its own join, not this one's.
    const std::string listWithRepoRoot =
        "const std::vector<fs::path> c = { exeDir / \"Shaders\", repoRoot / \"Assets\" / \"Shaders\" };";
    EXPECT_TRUE(FindViolations("List.cpp", listWithRepoRoot).empty());
}
