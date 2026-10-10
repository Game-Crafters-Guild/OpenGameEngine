// The ledger on its own, with no asset manager and no file watcher above it: what it
// treats as this process's own write arriving a second time, and what it refuses to.
// The rule it enforces is that a change is an echo only while the file still holds the
// bytes the write reported, so an edit that moved the file on is never swallowed.

#include <gtest/gtest.h>

#include "Assets/ExpectedWriteLedger.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace
{
using namespace GameEngine;

// Long enough that nothing in these tests expires by accident; the expiry test uses
// its own short-windowed ledger instead of sleeping past this one.
constexpr std::chrono::milliseconds kTestEchoWindow{5000};

std::filesystem::path MakeUniqueTempDir()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = std::filesystem::temp_directory_path() / ("ge_expected_write_" + std::to_string(stamp));
    std::filesystem::create_directories(dir);
    return dir;
}

void WriteFile(const std::filesystem::path& path, std::string_view payload)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
}

struct LedgerFixture
{
    LedgerFixture() : Directory(MakeUniqueTempDir()) {}

    ~LedgerFixture()
    {
        std::error_code ec;
        std::filesystem::remove_all(Directory, ec);
    }

    std::filesystem::path Directory;
    ExpectedWriteLedger Ledger{kTestEchoWindow};
};

} // namespace

// The default answer. Anything the ledger was not told about is somebody else's edit,
// and treating it as an echo would lose the change entirely.
TEST(ExpectedWriteLedger, AnUnannouncedChangeIsNotAnEcho)
{
    LedgerFixture fixture;
    const std::filesystem::path file = fixture.Directory / "outside.bin";
    WriteFile(file, "external");

    EXPECT_FALSE(fixture.Ledger.IsEchoOfOurWrite(file));
}

// Why the announcement has to precede the write: a watcher can report a file appearing
// before the writer gets to report it, and that report is the one that would drive the
// pipeline a second time.
TEST(ExpectedWriteLedger, AChangeToAnAnnouncedPathIsAnEchoBeforeItIsReported)
{
    LedgerFixture fixture;
    const std::filesystem::path file = fixture.Directory / "in_flight.bin";

    const auto id = fixture.Ledger.Expect(file, /*subtree=*/false);
    ASSERT_NE(id, ExpectedWriteLedger::kNoRegistration);
    WriteFile(file, "just landed");

    EXPECT_TRUE(fixture.Ledger.IsEchoOfOurWrite(file));
}

// The ordinary desktop shape: the writer reports, the announcement is retired the
// moment the save returns, and the watcher's report of the same bytes arrives after
// both. Retiring must not end the echo window or the double drive comes straight back.
TEST(ExpectedWriteLedger, AReportedWriteIsStillAnEchoAfterItsAnnouncementRetires)
{
    LedgerFixture fixture;
    const std::filesystem::path file = fixture.Directory / "saved.bin";

    const auto id = fixture.Ledger.Expect(file, /*subtree=*/false);
    WriteFile(file, "saved bytes");
    fixture.Ledger.Reported(file);
    fixture.Ledger.Retire(id);

    EXPECT_TRUE(fixture.Ledger.IsEchoOfOurWrite(file));
}

// The half that keeps external edits alive. A file this process wrote is not exempt
// from being edited by something else a moment later, and that edit must drive.
TEST(ExpectedWriteLedger, AnEditAfterTheReportIsNotAnEcho)
{
    LedgerFixture fixture;
    const std::filesystem::path file = fixture.Directory / "edited.bin";

    const auto id = fixture.Ledger.Expect(file, /*subtree=*/false);
    WriteFile(file, "ours");
    fixture.Ledger.Reported(file);
    fixture.Ledger.Retire(id);

    WriteFile(file, "somebody else's, and longer");

    EXPECT_FALSE(fixture.Ledger.IsEchoOfOurWrite(file));
    EXPECT_FALSE(fixture.Ledger.IsEchoOfOurWrite(file))
        << "the record the edit retired came back";
}

// Size alone would not catch a rewrite in place, so the write time is part of the
// identity. The stamp is moved explicitly: on a filesystem whose resolution is coarser
// than these two writes are quick, the two states are genuinely indistinguishable and
// the test would be asserting something the ledger cannot know.
TEST(ExpectedWriteLedger, ASameSizeRewriteIsNotAnEcho)
{
    LedgerFixture fixture;
    const std::filesystem::path file = fixture.Directory / "same_size.bin";

    const auto id = fixture.Ledger.Expect(file, /*subtree=*/false);
    WriteFile(file, "aaaa");
    fixture.Ledger.Reported(file);
    fixture.Ledger.Retire(id);

    const std::filesystem::file_time_type before = std::filesystem::last_write_time(file);
    WriteFile(file, "bbbb");
    std::filesystem::last_write_time(file, before + std::chrono::seconds(1));
    ASSERT_NE(std::filesystem::last_write_time(file), before);
    ASSERT_EQ(std::filesystem::file_size(file), 4u);

    EXPECT_FALSE(fixture.Ledger.IsEchoOfOurWrite(file));
}

// A write nobody ever echoes must not keep the ledger holding the path forever.
TEST(ExpectedWriteLedger, TheEchoWindowExpires)
{
    LedgerFixture fixture;
    ExpectedWriteLedger shortWindow{std::chrono::milliseconds(1)};
    const std::filesystem::path file = fixture.Directory / "expiring.bin";

    const auto id = shortWindow.Expect(file, /*subtree=*/false);
    WriteFile(file, "reported once");
    shortWindow.Reported(file);
    shortWindow.Retire(id);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_FALSE(shortWindow.IsEchoOfOurWrite(file));
}

// A write that failed produced no file and reported nothing, so the path it did not
// touch has to go back to being somebody else's the moment the writer gives up on it.
TEST(ExpectedWriteLedger, RetiringAWriteThatNeverLandedStopsExpectingIt)
{
    LedgerFixture fixture;
    const std::filesystem::path file = fixture.Directory / "failed.bin";

    const auto id = fixture.Ledger.Expect(file, /*subtree=*/false);
    fixture.Ledger.Retire(id);

    WriteFile(file, "written by something else");
    EXPECT_FALSE(fixture.Ledger.IsEchoOfOurWrite(file));
}

// The writer names a path the way its caller spelled it; the watcher names the same
// file the way the filesystem holds it. Both have to reach the same entry. Asserted
// against a live announcement, which is decided on the key alone — a differently cased
// spelling does not open on a case-sensitive filesystem, so making the content check
// resolve it would be testing the filesystem rather than the key.
TEST(ExpectedWriteLedger, SpellingsOfTheSamePathMatch)
{
    LedgerFixture fixture;
    const std::filesystem::path announced = fixture.Directory / "Scenes" / "MixedCase.bin";

    const auto id = fixture.Ledger.Expect(announced, /*subtree=*/false);

    EXPECT_TRUE(fixture.Ledger.IsEchoOfOurWrite(fixture.Directory / "scenes" / "mixedcase.bin"))
        << "case";
    EXPECT_TRUE(fixture.Ledger.IsEchoOfOurWrite(fixture.Directory / "Scenes" / "." / "MixedCase.bin"))
        << "dot segment";
    EXPECT_TRUE(fixture.Ledger.IsEchoOfOurWrite(
        std::filesystem::path(fixture.Directory.string() + "/Scenes/Nested/../MixedCase.bin")))
        << "parent segment";
    EXPECT_FALSE(fixture.Ledger.IsEchoOfOurWrite(fixture.Directory / "Scenes" / "Other.bin"));

    fixture.Ledger.Retire(id);
}

// A tree copy discovers its files by performing the copy, so the destination root is
// all that can be announced in advance.
TEST(ExpectedWriteLedger, ASubtreeAnnouncementCoversTheFilesUnderIt)
{
    LedgerFixture fixture;
    const std::filesystem::path destination = fixture.Directory / "Pasted";
    const std::filesystem::path nested = destination / "Nested" / "leaf.bin";

    const auto id = fixture.Ledger.Expect(destination, /*subtree=*/true);
    WriteFile(nested, "copied");

    EXPECT_TRUE(fixture.Ledger.IsEchoOfOurWrite(nested));
    EXPECT_TRUE(fixture.Ledger.IsEchoOfOurWrite(destination));

    fixture.Ledger.Retire(id);
    EXPECT_FALSE(fixture.Ledger.IsEchoOfOurWrite(nested));
}

// The subtree test is a path test, not a string test: a sibling directory whose name
// starts with the announced one is not inside it.
TEST(ExpectedWriteLedger, ASubtreeAnnouncementStopsAtItsOwnDirectory)
{
    LedgerFixture fixture;
    const std::filesystem::path destination = fixture.Directory / "Mats";
    const std::filesystem::path sibling = fixture.Directory / "Materials" / "unrelated.bin";

    const auto id = fixture.Ledger.Expect(destination, /*subtree=*/true);
    WriteFile(sibling, "not part of the paste");

    EXPECT_FALSE(fixture.Ledger.IsEchoOfOurWrite(sibling));
    fixture.Ledger.Retire(id);
}

// While a tree copy is still running its announcement covers everything under it, but a
// file it has already reported is held to its bytes like any other: an edit to it is a
// change of its own even though the paste has not finished.
TEST(ExpectedWriteLedger, AnEditToAReportedFileUnderALiveSubtreeIsNotAnEcho)
{
    LedgerFixture fixture;
    const std::filesystem::path destination = fixture.Directory / "Pasted";
    const std::filesystem::path copied = destination / "first.bin";

    const auto id = fixture.Ledger.Expect(destination, /*subtree=*/true);
    WriteFile(copied, "copied");
    fixture.Ledger.Reported(copied);
    ASSERT_TRUE(fixture.Ledger.IsEchoOfOurWrite(copied));

    WriteFile(copied, "edited while the paste was still running");
    EXPECT_FALSE(fixture.Ledger.IsEchoOfOurWrite(copied));

    fixture.Ledger.Retire(id);
}

// An announcement for a path that cannot be keyed is refused rather than recorded as a
// blanket match on everything.
TEST(ExpectedWriteLedger, AnEmptyPathIsNotARegistration)
{
    LedgerFixture fixture;
    EXPECT_EQ(fixture.Ledger.Expect({}, /*subtree=*/false), ExpectedWriteLedger::kNoRegistration);
    EXPECT_FALSE(fixture.Ledger.IsEchoOfOurWrite({}));
}

// A second write of a path this process already wrote. The first write's recorded identity
// describes bytes the second write has already replaced, so it must not be what decides
// whether the second write's own echo is an echo: while a write is announced, a change to
// its path is that write arriving. Reading it the other way round makes the stale record
// override the live announcement and the save drives twice.
TEST(ExpectedWriteLedger, ASecondWriteOfTheSamePathIsNotJudgedByTheFirstsIdentity)
{
    LedgerFixture fixture;
    const std::filesystem::path file = fixture.Directory / "written_twice.bin";

    const auto first = fixture.Ledger.Expect(file, /*subtree=*/false);
    WriteFile(file, "first");
    fixture.Ledger.Reported(file);
    fixture.Ledger.Retire(first);

    // The second write is announced and its bytes are on disk, but it has not reported yet
    // — the window a watcher's report of it lands in.
    const auto second = fixture.Ledger.Expect(file, /*subtree=*/false);
    WriteFile(file, "second, and a different length");

    EXPECT_TRUE(fixture.Ledger.IsEchoOfOurWrite(file))
        << "the first write's identity outvoted the announcement of the second";

    fixture.Ledger.Reported(file);
    fixture.Ledger.Retire(second);
    EXPECT_TRUE(fixture.Ledger.IsEchoOfOurWrite(file));
}
