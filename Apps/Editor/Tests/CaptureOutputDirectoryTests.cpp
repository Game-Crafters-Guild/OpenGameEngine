#include "Editor/CaptureOutputDirectory.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace
{

unsigned long SelfProcessId()
{
#if defined(_WIN32)
    return static_cast<unsigned long>(::GetCurrentProcessId());
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

// The file a previous process holding this same (recycled) process id would
// have left behind.
std::filesystem::path StaleFrameMarker()
{
    return std::filesystem::temp_directory_path() / "gameengine_mcp" /
           ("pid-" + std::to_string(SelfProcessId())) / "screenshot_viewport_1.png";
}

// Seeds that file before any test body runs — gtest drives environments ahead
// of the whole suite, so the assertion holds under any test order or filter,
// including a run of the clearing test alone.
class StaleFrameSeed : public ::testing::Environment
{
public:
    void SetUp() override
    {
        const std::filesystem::path marker = StaleFrameMarker();
        std::error_code ec;
        std::filesystem::create_directories(marker.parent_path(), ec);
        std::ofstream ofs(marker, std::ios::binary);
        ofs << "frames from a process that is gone";
    }
};

const ::testing::Environment* kStaleFrameSeed =
    ::testing::AddGlobalTestEnvironment(new StaleFrameSeed());

} // namespace

// The defect this pins: capture file names are a per-process sequence and a
// resource name, so two editors writing into one shared directory produce the
// same names and overwrite each other's frames. Scoping the directory to the
// process id is what makes that structurally impossible — a name collision now
// requires two live processes to share a pid, which the OS does not allow.
TEST(CaptureOutputDirectory, IsScopedToThisProcess)
{
    const std::filesystem::path dir = GameEngine::Editor::CaptureOutputDirectory();

    EXPECT_EQ(dir.filename().string(), "pid-" + std::to_string(SelfProcessId()));
    EXPECT_EQ(dir.parent_path().filename().string(), "gameengine_mcp");

    std::error_code ec;
    EXPECT_TRUE(std::filesystem::is_directory(dir, ec)) << dir.string();
}

// Process ids are recycled and the capture sequence restarts at 1, so a dead
// editor's frames can already occupy the exact names this process is about to
// hand out. First use empties the directory; without that, a caller re-reading
// a returned path could be served a frame no live editor ever rendered.
TEST(CaptureOutputDirectory, FirstUseClearsFramesLeftByARecycledProcessId)
{
    // Pins the seed to the production layout: if the directory moved, this
    // fails rather than passing because the marker was seeded somewhere the
    // code no longer looks.
    ASSERT_EQ(StaleFrameMarker().parent_path(), GameEngine::Editor::CaptureOutputDirectory());

    std::error_code ec;
    EXPECT_FALSE(std::filesystem::exists(StaleFrameMarker(), ec));
}

TEST(CaptureOutputDirectory, RepeatedCallsNameOneDirectory)
{
    EXPECT_EQ(GameEngine::Editor::CaptureOutputDirectory(),
              GameEngine::Editor::CaptureOutputDirectory());
}

// The directory is emptied once per process, not once per capture: a later call
// must not delete the frames this session already wrote, or every capture but
// the newest would vanish from under a caller holding its path.
TEST(CaptureOutputDirectory, LaterCallsKeepAlreadyWrittenFiles)
{
    const std::filesystem::path file =
        GameEngine::Editor::CaptureOutputDirectory() / "capture-output-directory-test.bin";
    {
        std::ofstream ofs(file, std::ios::binary);
        ofs << "pixels";
    }
    ASSERT_TRUE(std::filesystem::exists(file));

    GameEngine::Editor::CaptureOutputDirectory();

    std::error_code ec;
    EXPECT_TRUE(std::filesystem::exists(file, ec));
    std::filesystem::remove(file, ec);
}

// A temp sweep between captures must not turn the next write into a silent
// failure: the encoder would succeed, the ofstream would not, and the response
// would name a file that does not exist. Removes only a subdirectory so a
// shuffled run cannot eat the first-use marker another test seeds in the
// process directory itself.
TEST(CaptureOutputDirectory, IsRecreatedAfterRemoval)
{
    const std::filesystem::path dir = GameEngine::Editor::CaptureOutputDirectory();

    std::error_code ec;
    const std::filesystem::path sub = dir / "sweep-victim";
    std::filesystem::create_directories(sub, ec);
    ASSERT_TRUE(std::filesystem::exists(sub, ec));
    std::filesystem::remove_all(sub, ec);
    ASSERT_FALSE(std::filesystem::exists(sub, ec));

    EXPECT_TRUE(std::filesystem::is_directory(GameEngine::Editor::CaptureOutputDirectory(), ec));
}
