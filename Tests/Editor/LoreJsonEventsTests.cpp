#include <gtest/gtest.h>

#include "LoreJsonEvents.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace
{
using GameEngine::LoreFileStatusEntry;
using GameEngine::LoreRevisionHeader;
using GameEngine::ParseLoreHistoryEvents;
using GameEngine::ParseLoreJsonEvents;
using GameEngine::ParseLoreRevisionHeader;
using GameEngine::ParseLoreStatusFileEvent;
using GameEngine::VCSFileStatus;
using GameEngine::VCSLogEntry;
using json = nlohmann::json;

// Wire shapes mirror the upstream serde attributes (EpicGames/lore,
// lore-revision/src/event.rs: `tag = "tagName", content = "data"`,
// camelCase fields, `u8_as_bool` flags, hex hashes). The pre-rename status
// scraper matched these by guesswork; these fixtures pin the contract.

json StatusFile(const char* action, bool staged, const char* path = "Assets/a.scene")
{
    return {{"tagName", "repositoryStatusFile"},
            {"data",
             {{"path", path},
              {"size", 12},
              {"action", action},
              {"type", "file"},
              {"flagStaged", staged},
              {"flagMerged", false},
              {"flagConflict", false},
              {"flagConflictUnresolved", false},
              {"flagConflictAutomerged", false},
              {"flagConflictMine", false},
              {"flagConflictTheirs", false},
              {"flagDirty", true},
              {"fromPath", ""}}}};
}

TEST(LoreJsonEventsTests, SplitsOneEventPerLineAndSkipsNoise)
{
    const std::string output =
        "{\"tagName\":\"repositoryStatusRevision\",\"data\":{\"branchName\":\"main\"}}\r\n"
        "\n"
        "Some human line the pager would print\n"
        "{not json\n"
        "{\"tagName\":\"repositoryStatusFile\",\"data\":{\"path\":\"x\",\"action\":\"add\"}}";

    const std::vector<json> events = ParseLoreJsonEvents(output);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0]["tagName"], "repositoryStatusRevision");
    EXPECT_EQ(events[1]["data"]["path"], "x");
}

TEST(LoreJsonEventsTests, MapsActionsAndStagingToEditorStatus)
{
    LoreFileStatusEntry entry;

    ASSERT_TRUE(ParseLoreStatusFileEvent(StatusFile("keep", false)["data"], entry));
    EXPECT_EQ(entry.Status, VCSFileStatus::Modified) << "keep + dirty is the CLI's M";
    EXPECT_EQ(entry.Path, "Assets/a.scene");

    ASSERT_TRUE(ParseLoreStatusFileEvent(StatusFile("add", false)["data"], entry));
    EXPECT_EQ(entry.Status, VCSFileStatus::Unversioned) << "an unstaged add is untracked";

    ASSERT_TRUE(ParseLoreStatusFileEvent(StatusFile("add", true)["data"], entry));
    EXPECT_EQ(entry.Status, VCSFileStatus::Added);

    ASSERT_TRUE(ParseLoreStatusFileEvent(StatusFile("delete", true)["data"], entry));
    EXPECT_EQ(entry.Status, VCSFileStatus::Deleted);

    ASSERT_TRUE(ParseLoreStatusFileEvent(StatusFile("move", true)["data"], entry));
    EXPECT_EQ(entry.Status, VCSFileStatus::Modified);

    ASSERT_TRUE(ParseLoreStatusFileEvent(StatusFile("copy", false)["data"], entry));
    EXPECT_EQ(entry.Status, VCSFileStatus::Modified);
}

TEST(LoreJsonEventsTests, UnresolvedConflictWinsOverAction)
{
    json event = StatusFile("keep", true);
    event["data"]["flagConflict"] = true;
    event["data"]["flagConflictUnresolved"] = true;

    LoreFileStatusEntry entry;
    ASSERT_TRUE(ParseLoreStatusFileEvent(event["data"], entry));
    EXPECT_EQ(entry.Status, VCSFileStatus::Conflict);

    event["data"]["flagConflictUnresolved"] = false;
    ASSERT_TRUE(ParseLoreStatusFileEvent(event["data"], entry));
    EXPECT_EQ(entry.Status, VCSFileStatus::Modified) << "a resolved conflict is plain content";
}

TEST(LoreJsonEventsTests, NormalizesPathsAndRejectsEmptyOnes)
{
    LoreFileStatusEntry entry;
    ASSERT_TRUE(ParseLoreStatusFileEvent(StatusFile("keep", false, "\\Assets\\b.png")["data"], entry));
    EXPECT_EQ(entry.Path, "Assets/b.png");

    EXPECT_FALSE(ParseLoreStatusFileEvent(StatusFile("keep", false, "")["data"], entry));
    EXPECT_FALSE(ParseLoreStatusFileEvent(json::array(), entry));
}

TEST(LoreJsonEventsTests, ReadsTheRevisionHeader)
{
    const json data = {{"repository", "00ff"},
                       {"branch", "0a0b"},
                       {"branchName", "main"},
                       {"revision", "abcdef"},
                       {"revisionNumber", 42},
                       {"isLocalAhead", 0},
                       {"remoteAvailable", 1}};

    LoreRevisionHeader header;
    ASSERT_TRUE(ParseLoreRevisionHeader(data, header));
    EXPECT_EQ(header.BranchName, "main");
    EXPECT_EQ(header.Revision, "abcdef");
    EXPECT_EQ(header.RevisionNumber, 42u);

    EXPECT_FALSE(ParseLoreRevisionHeader(json{{"revisionNumber", 1}}, header))
        << "no branch name, no header";
}

TEST(LoreJsonEventsTests, HistoryEntriesCollectTheirMetadata)
{
    const std::vector<json> events = {
        {{"tagName", "revisionHistoryEntry"},
         {"data", {{"revision", "aa11"}, {"revisionNumber", 7}, {"parent", json::array({"0000", "0000"})}}}},
        {{"tagName", "metadata"},
         {"data", {{"key", "message"}, {"value", {{"tagName", "string"}, {"data", "Move the lamp"}}}}}},
        {{"tagName", "metadata"},
         {"data", {{"key", "created-by"}, {"value", {{"tagName", "string"}, {"data", "user-1"}}}}}},
        // Unix epoch milliseconds: 2026-08-31 07:22:50 UTC.
        {{"tagName", "metadata"},
         {"data", {{"key", "timestamp"}, {"value", {{"tagName", "numeric"}, {"data", 1788160970000ull}}}}}},
        {{"tagName", "revisionHistoryEntry"}, {"data", {{"revision", "bb22"}, {"revisionNumber", 6}}}},
        {{"tagName", "metadata"},
         {"data", {{"key", "branch"}, {"value", {{"tagName", "context"}, {"data", "0a0b"}}}}}},
    };

    const std::vector<VCSLogEntry> entries = ParseLoreHistoryEvents(events);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].revision, "7");
    EXPECT_EQ(entries[0].message, "Move the lamp");
    EXPECT_EQ(entries[0].author, "user-1");
    EXPECT_EQ(entries[0].date, "2026-08-31 07:22:50");
    EXPECT_EQ(entries[1].revision, "6");
    EXPECT_TRUE(entries[1].message.empty()) << "metadata never leaks across entries";
}

TEST(LoreJsonEventsTests, MetadataBeforeAnyEntryIsIgnored)
{
    const std::vector<json> events = {
        {{"tagName", "metadata"},
         {"data", {{"key", "message"}, {"value", {{"tagName", "string"}, {"data", "orphan"}}}}}},
    };
    EXPECT_TRUE(ParseLoreHistoryEvents(events).empty());
}

} // namespace
