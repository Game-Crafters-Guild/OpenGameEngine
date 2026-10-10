// Contract: shader animation time comes from the engine frame clock, never from
// a wall clock.
//
// RenderServicesFrameGraph states the rule where the clocks are stamped —
// animation is part of the captured frame, so it must advance with the frame
// clock: movie capture pins a fixed recording delta, and backpressure can stall
// rendering past one frame, both of which make wall time jump between adjacent
// recorded frames. Time::GetCumulativeSeconds() also caps each frame's advance
// (kMaxAnimationDeltaSeconds), so a multi-second hitch bleeds across the
// recovery frames instead of snapping animation phase forward in one step.
//
// A wall clock defeats all of that, and carries a second failure the cap cannot
// reach: Rendering::Utils::GetCurrentTimeSeconds() is seconds since boot, so it
// arrives at the shader already large. Quantised to fp32 its resolution decays
// with uptime — ~0.0625 s at ten days, i.e. an animation term that only changes
// every fourth frame at 60 Hz.
//
// The rule was enforced by hand twice (grass wind, then both fog noise feeds)
// and re-broken in between, so it is pinned structurally here: a wall clock
// reintroduced into a renderer source reds this test rather than surfacing as a
// stutter someone has to chase to a capture.
//
// Reads the repo source via GE_RENDERER_REPO_ROOT (dev-only anchor, same
// precedent as LodCrossfadeShaderContractTests): the source file is the artifact
// under test, so a staged copy would be the wrong thing to read.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{

// The wall clock this contract bans from renderer sources. The millisecond and
// microsecond siblings are deliberately NOT banned: they measure durations for
// profiling, which is what a wall clock is for.
constexpr const char* kBannedWallClock = "GetCurrentTimeSeconds(";

// The sanctioned feed. Its presence doubles as the scanner's positive control:
// a scan that reads nothing would report zero banned hits and pass silently.
constexpr const char* kFrameClockFeed = "GetShaderAnimationTimeSeconds";

// Sources that upload per-frame shader parameters. The editor is in scope
// because it feeds shader uniforms too — SceneViewOverlaysRG hands the
// animation clock to the overlay pass — so a wall clock reintroduced there is
// the same defect, merely outside the engine tree.
//
// Only the named helper is banned, not std::chrono itself. There are ~213
// inline chrono::now() call sites under these roots and effectively all of them
// time a DURATION for profiling, which is what a wall clock is for; a ban broad
// enough to catch an inline animation clock would drown in them.
const char* const kScanRoots[] = {
    "Engine/Source/Engine/Rendering",
    "Engine/Modules",
    "Apps/Editor/Source",
};

std::string ReadTextFile(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open())
        return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Strip // and /* */ comments so prose naming a symbol never counts as a use —
// this file's own header comment would otherwise fail the contract it states.
std::string StripComments(const std::string& source)
{
    std::string out;
    out.reserve(source.size());
    for (std::size_t pos = 0; pos < source.size();)
    {
        if (source.compare(pos, 2, "//") == 0)
        {
            const std::size_t lineEnd = source.find('\n', pos);
            if (lineEnd == std::string::npos)
                break;
            pos = lineEnd; // keep the newline so line structure survives
        }
        else if (source.compare(pos, 2, "/*") == 0)
        {
            const std::size_t end = source.find("*/", pos + 2);
            if (end == std::string::npos)
                break;
            pos = end + 2;
        }
        else
        {
            out.push_back(source[pos]);
            ++pos;
        }
    }
    return out;
}

bool IsCppSource(const std::filesystem::path& p)
{
    const std::string ext = p.extension().string();
    return ext == ".cpp" || ext == ".h" || ext == ".hpp";
}

// The banned helper's own declaration and definition are not violations of it.
// Matched by exact path: excluding every file merely NAMED Utils would blind the
// scan to a real violation in any other module's Utils.cpp.
bool IsTheWallClockItself(const std::string& repoRelativePath)
{
    return repoRelativePath == "Engine/Modules/Rendering/Include/Rendering/Common/Utils.h" ||
           repoRelativePath == "Engine/Modules/Rendering/Source/Common/Utils.cpp";
}

struct ScannedFile
{
    std::string RelativePath;
    std::string Source; // comments stripped
};

std::vector<ScannedFile> ScanRendererSources()
{
    std::vector<ScannedFile> out;
#ifdef GE_RENDERER_REPO_ROOT
    const std::filesystem::path repoRoot(GE_RENDERER_REPO_ROOT);
    for (const char* relativeRoot : kScanRoots)
    {
        const std::filesystem::path root = repoRoot / relativeRoot;
        if (!std::filesystem::exists(root))
            continue;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
        {
            if (!entry.is_regular_file() || !IsCppSource(entry.path()))
                continue;
            std::string relativePath =
                std::filesystem::relative(entry.path(), repoRoot).generic_string();
            if (IsTheWallClockItself(relativePath))
                continue;
            out.push_back({std::move(relativePath), StripComments(ReadTextFile(entry.path()))});
        }
    }
#endif
    return out;
}

} // namespace

TEST(ShaderAnimationClockContract, NoRendererSourceReadsTheWallClock)
{
    const std::vector<ScannedFile> files = ScanRendererSources();

    // Positive control: a scan that found nothing would pass the ban vacuously.
    ASSERT_GT(files.size(), 100u)
        << "scanner read " << files.size()
        << " files — GE_RENDERER_REPO_ROOT is wrong or the tree moved; the ban below "
           "would have passed vacuously";

    std::size_t frameClockFeeds = 0;
    std::vector<std::string> violations;
    for (const ScannedFile& file : files)
    {
        if (file.Source.find(kFrameClockFeed) != std::string::npos)
            ++frameClockFeeds;
        if (file.Source.find(kBannedWallClock) != std::string::npos)
            violations.push_back(file.RelativePath);
    }

    // Second half of the positive control: the sanctioned feed IS visible to the
    // scanner, so a zero-violation result means "clean", not "read nothing".
    ASSERT_GE(frameClockFeeds, 2u)
        << "scanner saw " << frameClockFeeds << " uses of " << kFrameClockFeed
        << " — it is not reading real content, so the ban result is meaningless";

    std::string message;
    for (const std::string& path : violations)
        message += "\n  " + path;
    EXPECT_TRUE(violations.empty())
        << "these renderer sources read the wall clock:" << message
        << "\n\nShader animation time must come from the capped engine frame clock:"
           "\n  RenderServices::GetShaderAnimationTimeSeconds()  (phase-continuous effects)"
           "\n  RenderServices::GetScrollAnimationTimeSeconds()  (tile-periodic UV panners)"
           "\nA wall clock breaks capture determinism, escapes the per-frame hitch cap, and"
           "\nloses fp32 resolution as machine uptime grows. To time a DURATION for profiling,"
           "\nuse the millisecond/microsecond helpers instead — those are not banned here.";
}
