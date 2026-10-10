// Contract for editor session provenance: what a running editor may claim about itself.
//
// Why this file exists: three separate incidents came from an editor that could not say
// who it was — a deliberately-slow measurement arm that looked broken, a get_editor_state
// query answered by a different lane's editor, and a commit landed into another lane's
// project. The fix is only worth anything if every field is a FACT, so the cases below are
// mostly about what the descriptor must REFUSE to claim: no branch for a detached HEAD, no
// worktree for a shipped build, no label it was not given, and nothing at all from a
// malformed repository. An inferred value would mislabel exactly the ambiguous sessions
// this exists to disambiguate.

#include "Startup/EditorSessionDescriptor.h"

#include "TestTempDir.h"

#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <system_error>

namespace GameEngine::Editor::Startup
{
namespace
{
namespace fs = std::filesystem;

void WriteFile(const fs::path& file, const std::string& contents)
{
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open()) << "could not write " << file.string();
    out << contents;
}

// Per-test scratch tree, following MeshBvhDiskFormatTests: created and removed per test so
// synthetic repositories never bleed between cases.
class EditorSessionDescriptorTest : public ::testing::Test
{
protected:
    fs::path Root;

    void SetUp() override
    {
        Root = GameEngine::TestUtils::MakeUniqueTempDirectory("GameEngine_EditorSessionDescriptorTests");
        std::error_code ec;
        fs::create_directories(Root, ec);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(Root, ec);
    }

    // The walk climbs to the filesystem root, so a scratch tree only proves "no repository"
    // if the temp directory itself is not inside one. Say so out loud instead of failing
    // for an unrelated reason on a machine whose TEMP happens to sit in a checkout.
    void RequireNoRepositoryAboveScratch()
    {
        for (fs::path dir = Root; !dir.empty(); dir = dir.parent_path())
        {
            std::error_code ec;
            if (fs::exists(dir / ".git", ec) && !ec)
                GTEST_SKIP() << "temp directory is inside a git repository (" << dir.string()
                             << "); this case needs a scratch tree with no repository above it";
            if (dir.parent_path() == dir)
                break;
        }
    }
};

// --- Fallback chain: label over derived, derived over absent ---
//
// These four are the load-bearing cases. They are written against a hand-built descriptor
// rather than a synthetic tree so the chain is tested in isolation from the git walk.

TEST(EditorSessionFallbackChainTests, ExplicitLabelWinsOverDerivedFacts)
{
    EditorSessionDescriptor descriptor;
    descriptor.SessionLabel = "forced-LOD0 measurement arm";
    descriptor.WorktreeName = "wt-lodcost";
    descriptor.BranchAtLaunch = "feat/lod-hysteresis";

    const auto suffix = FormatSessionChromeSuffix(descriptor);
    ASSERT_TRUE(suffix.has_value());
    EXPECT_EQ(*suffix, "forced-LOD0 measurement arm");
}

TEST(EditorSessionFallbackChainTests, DerivedFactsWinOverAbsent)
{
    EditorSessionDescriptor descriptor;
    descriptor.WorktreeName = "wt-lodcost";
    descriptor.BranchAtLaunch = "feat/lod-hysteresis";

    const auto suffix = FormatSessionChromeSuffix(descriptor);
    ASSERT_TRUE(suffix.has_value());
    EXPECT_EQ(*suffix, "wt-lodcost@feat/lod-hysteresis");
}

// A detached worktree still has the half of the identity that cannot lie.
TEST(EditorSessionFallbackChainTests, WorktreeWithoutBranchStillIdentifies)
{
    EditorSessionDescriptor descriptor;
    descriptor.WorktreeName = "wt-lodcost";

    const auto suffix = FormatSessionChromeSuffix(descriptor);
    ASSERT_TRUE(suffix.has_value());
    EXPECT_EQ(*suffix, "wt-lodcost");
}

TEST(EditorSessionFallbackChainTests, NothingKnownReportsAbsentRatherThanGuessing)
{
    EXPECT_FALSE(FormatSessionChromeSuffix(EditorSessionDescriptor{}).has_value());
}

// A branch alone is not an identity: without a worktree there is nothing to attribute it
// to, so the chain must fall through to absent rather than name a branch on its own.
TEST(EditorSessionFallbackChainTests, BranchWithoutWorktreeIsNotAnIdentity)
{
    EditorSessionDescriptor descriptor;
    descriptor.BranchAtLaunch = "feat/lod-hysteresis";

    EXPECT_FALSE(FormatSessionChromeSuffix(descriptor).has_value());
}

// --- Derivation from a primary clone (`.git` is a directory) ---

TEST_F(EditorSessionDescriptorTest, PrimaryCloneYieldsWorktreeAndBranch)
{
    const fs::path repo = Root / "GameEngine";
    WriteFile(repo / ".git" / "HEAD", "ref: refs/heads/main\n");
    const fs::path exeDir = repo / "build" / "vs2026-x64-local" / "bin" / "DebugFast";
    std::error_code ec;
    fs::create_directories(exeDir, ec);

    const auto d = DeriveEditorSessionDescriptor(exeDir, std::nullopt);
    ASSERT_TRUE(d.WorktreePath.has_value());
    EXPECT_EQ(*d.WorktreePath, repo);
    ASSERT_TRUE(d.WorktreeName.has_value());
    EXPECT_EQ(*d.WorktreeName, "GameEngine");
    ASSERT_TRUE(d.BranchAtLaunch.has_value());
    EXPECT_EQ(*d.BranchAtLaunch, "main");
    EXPECT_FALSE(d.SessionLabel.has_value());
}

// --- Derivation from a linked worktree (`.git` is a FILE, which is how this repo works) ---

TEST_F(EditorSessionDescriptorTest, LinkedWorktreeResolvesGitdirFileAndKeepsSlashesInBranch)
{
    const fs::path primaryGitDir = Root / "GameEngine" / ".git";
    const fs::path worktreeGitDir = primaryGitDir / "worktrees" / "wt-session";
    WriteFile(worktreeGitDir / "HEAD", "ref: refs/heads/feat/editor-session-provenance\n");

    const fs::path worktree = Root / "wt-session";
    WriteFile(worktree / ".git", "gitdir: " + worktreeGitDir.string() + "\n");
    const fs::path exeDir = worktree / "build" / "vs2026-x64-local" / "bin" / "DebugFast";
    std::error_code ec;
    fs::create_directories(exeDir, ec);

    const auto d = DeriveEditorSessionDescriptor(exeDir, std::nullopt);
    ASSERT_TRUE(d.WorktreeName.has_value());
    EXPECT_EQ(*d.WorktreeName, "wt-session");
    ASSERT_TRUE(d.BranchAtLaunch.has_value());
    // Branch names contain '/', so only the "refs/heads/" prefix may be stripped.
    EXPECT_EQ(*d.BranchAtLaunch, "feat/editor-session-provenance");
}

TEST_F(EditorSessionDescriptorTest, LinkedWorktreeAcceptsRelativeGitdir)
{
    const fs::path worktreeGitDir = Root / "GameEngine" / ".git" / "worktrees" / "wt-rel";
    WriteFile(worktreeGitDir / "HEAD", "ref: refs/heads/topic\n");

    const fs::path worktree = Root / "wt-rel";
    WriteFile(worktree / ".git", "gitdir: ../GameEngine/.git/worktrees/wt-rel\n");
    std::error_code ec;
    fs::create_directories(worktree / "bin", ec);

    const auto d = DeriveEditorSessionDescriptor(worktree / "bin", std::nullopt);
    ASSERT_TRUE(d.BranchAtLaunch.has_value());
    EXPECT_EQ(*d.BranchAtLaunch, "topic");
}

// --- What the descriptor must refuse to claim ---

// A detached HEAD has no branch. Many worktrees in this repo are detached, and naming the
// commit as though it were a branch would be a lie in the one field that is already the
// most fragile.
TEST_F(EditorSessionDescriptorTest, DetachedHeadReportsNoBranchButKeepsWorktree)
{
    const fs::path repo = Root / "wt-detached";
    WriteFile(repo / ".git" / "HEAD", "2e8517d4f0f7d0e4b8b3c9a1d5e6f70819a2b3c4\n");
    std::error_code ec;
    fs::create_directories(repo / "bin", ec);

    const auto d = DeriveEditorSessionDescriptor(repo / "bin", std::nullopt);
    ASSERT_TRUE(d.WorktreeName.has_value());
    EXPECT_EQ(*d.WorktreeName, "wt-detached");
    EXPECT_FALSE(d.BranchAtLaunch.has_value());
}

// The shipped-Player case: no repository anywhere above the executable. Absent fields,
// no error, no exception.
TEST_F(EditorSessionDescriptorTest, NoRepositoryAboveExecutableIsSilentlyAbsent)
{
    RequireNoRepositoryAboveScratch();

    const fs::path exeDir = Root / "ShippedGame" / "bin";
    std::error_code ec;
    fs::create_directories(exeDir, ec);

    const auto d = DeriveEditorSessionDescriptor(exeDir, std::nullopt);
    EXPECT_FALSE(d.WorktreePath.has_value());
    EXPECT_FALSE(d.WorktreeName.has_value());
    EXPECT_FALSE(d.BranchAtLaunch.has_value());
}

// A `.git` that exists but cannot be parsed is unknown provenance, not an invitation to
// fall back to the directory name anyway.
TEST_F(EditorSessionDescriptorTest, MalformedGitFileClaimsNothing)
{
    const fs::path worktree = Root / "wt-broken";
    WriteFile(worktree / ".git", "this is not a gitdir pointer\n");
    std::error_code ec;
    fs::create_directories(worktree / "bin", ec);

    const auto d = DeriveEditorSessionDescriptor(worktree / "bin", std::nullopt);
    EXPECT_FALSE(d.WorktreePath.has_value());
    EXPECT_FALSE(d.WorktreeName.has_value());
    EXPECT_FALSE(d.BranchAtLaunch.has_value());
}

// A worktree nested inside another checkout belongs to the NEAREST root: that is the tree
// the binary was actually built in.
TEST_F(EditorSessionDescriptorTest, NearestGitRootWins)
{
    const fs::path outer = Root / "GameEngine";
    WriteFile(outer / ".git" / "HEAD", "ref: refs/heads/main\n");
    const fs::path inner = outer / ".claude" / "worktrees" / "agent-a6214cee";
    WriteFile(inner / ".git" / "HEAD", "ref: refs/heads/agent-topic\n");
    std::error_code ec;
    fs::create_directories(inner / "build" / "bin", ec);

    const auto d = DeriveEditorSessionDescriptor(inner / "build" / "bin", std::nullopt);
    ASSERT_TRUE(d.WorktreeName.has_value());
    EXPECT_EQ(*d.WorktreeName, "agent-a6214cee");
    ASSERT_TRUE(d.BranchAtLaunch.has_value());
    EXPECT_EQ(*d.BranchAtLaunch, "agent-topic");
}

// --- The label is independent of git ---

TEST_F(EditorSessionDescriptorTest, LabelSurvivesWithNoRepository)
{
    RequireNoRepositoryAboveScratch();

    const fs::path exeDir = Root / "ShippedGame" / "bin";
    std::error_code ec;
    fs::create_directories(exeDir, ec);

    const auto d = DeriveEditorSessionDescriptor(exeDir, std::string("soak run 3"));
    ASSERT_TRUE(d.SessionLabel.has_value());
    EXPECT_EQ(*d.SessionLabel, "soak run 3");
    EXPECT_FALSE(d.WorktreeName.has_value());

    const auto suffix = FormatSessionChromeSuffix(d);
    ASSERT_TRUE(suffix.has_value());
    EXPECT_EQ(*suffix, "soak run 3");
}

// An empty --session-label is no label, not a label that renders as nothing.
TEST_F(EditorSessionDescriptorTest, EmptyLabelIsTreatedAsAbsent)
{
    std::error_code ec;
    fs::create_directories(Root / "bin", ec);

    const auto d = DeriveEditorSessionDescriptor(Root / "bin", std::string(""));
    EXPECT_FALSE(d.SessionLabel.has_value());
}
} // namespace
} // namespace GameEngine::Editor::Startup
