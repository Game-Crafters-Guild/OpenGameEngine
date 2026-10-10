#include <gtest/gtest.h>

#include "Assets/AssetPathKey.h"
#include "AssetCore/PathNormalization.h"

#include <filesystem>
#include <string>

using GameEngine::AssetPathKey;
using GameEngine::IsDirectChildOfAssetDir;
using GameEngine::IsUnderAssetDir;
using GameEngine::RebaseRenamedAssetDir;

namespace
{

// The shape the asset browser actually sees: AssetRegistry::RegisterSource
// stores the project root through NormalizeForRegistryKey, so the browser's
// current directory arrives case-folded, while a file-watch event names the
// same directory in its on-disk case. The two must still key alike.
constexpr const char* kFoldedRoot = "c:/dev/scratch/player/assets";
constexpr const char* kOnDiskRoot = "C:/Dev/Scratch/Player/Assets";

// The host's own filesystem root: "C:\" on Windows, "/" on POSIX. Where a case
// pins the key against an ABSOLUTE path, the input has to be absolute in the
// host's spelling — a drive-letter path is absolute only on Windows, and
// AssetPathKey resolves a relative input against the current directory, so the
// two sides would then differ by a cwd prefix rather than by the property under
// test. The pairs above need no such root: both their sides take the same
// prefix, which is what keeps them comparable on either platform.
std::filesystem::path HostRoot()
{
    return std::filesystem::current_path().root_path();
}

} // namespace

TEST(AssetPathKey, FoldedRootAndOnDiskRootShareAKey)
{
    EXPECT_EQ(AssetPathKey(std::filesystem::path(kFoldedRoot)),
              AssetPathKey(std::filesystem::path(kOnDiskRoot)));
}

// The regression this key exists for: the registry hands the browser a folded
// root, the watcher delivers an on-disk-case event path, and the grid only
// refreshes when the two are recognized as parent and child.
TEST(AssetPathKey, OnDiskCaseEventIsDirectChildOfFoldedRoot)
{
    EXPECT_TRUE(IsDirectChildOfAssetDir(std::filesystem::path(kFoldedRoot),
                                        std::filesystem::path(kOnDiskRoot) / "NewMaterial.mat"));
}

TEST(AssetPathKey, OnDiskCaseEventIsDirectChildOfFoldedSubfolder)
{
    const std::filesystem::path foldedSubfolder = std::filesystem::path(kFoldedRoot) / "materials";
    const std::filesystem::path event = std::filesystem::path(kOnDiskRoot) / "Materials" / "Rock.mat";
    EXPECT_TRUE(IsDirectChildOfAssetDir(foldedSubfolder, event));
}

TEST(AssetPathKey, GrandchildIsNotADirectChild)
{
    EXPECT_FALSE(IsDirectChildOfAssetDir(std::filesystem::path(kFoldedRoot),
                                         std::filesystem::path(kOnDiskRoot) / "Materials" / "Rock.mat"));
}

TEST(AssetPathKey, DirectoryIsNotItsOwnDirectChild)
{
    EXPECT_FALSE(IsDirectChildOfAssetDir(std::filesystem::path(kFoldedRoot),
                                         std::filesystem::path(kOnDiskRoot)));
}

TEST(AssetPathKey, SiblingDirectoryIsNotADirectChild)
{
    EXPECT_FALSE(IsDirectChildOfAssetDir(std::filesystem::path(kFoldedRoot),
                                         std::filesystem::path("C:/Dev/Scratch/Player/Library/Rock.mat")));
}

TEST(AssetPathKey, EmptyPathsAreNeverRelated)
{
    EXPECT_TRUE(AssetPathKey(std::filesystem::path()).empty());
    EXPECT_FALSE(IsDirectChildOfAssetDir(std::filesystem::path(), std::filesystem::path(kOnDiskRoot)));
    EXPECT_FALSE(IsDirectChildOfAssetDir(std::filesystem::path(kFoldedRoot), std::filesystem::path()));
}

// FileWatcher builds event paths as m_WatchDirectory / filename, so a
// backslash-separated directory meets forward-slash comparison paths.
TEST(AssetPathKey, SeparatorStyleDoesNotChangeTheKey)
{
    EXPECT_EQ(AssetPathKey(std::filesystem::path("C:\\Dev\\Scratch\\Player\\Assets")),
              AssetPathKey(std::filesystem::path("C:/Dev/Scratch/Player/Assets")));
}

TEST(AssetPathKey, RedundantAndDotSegmentsCollapse)
{
    EXPECT_EQ(AssetPathKey(std::filesystem::path("C:/Dev/Scratch/Player/Assets/./Materials")),
              AssetPathKey(std::filesystem::path("C:/Dev/Scratch/Player/Library/../Assets/Materials")));
}

// POSIX-shaped pair: no drive letter, forward slashes only. Both sides take
// the same absolute() prefix on Windows, so the comparison stays meaningful
// on either platform.
TEST(AssetPathKey, PosixStylePathPairFoldsToOneKey)
{
    EXPECT_EQ(AssetPathKey(std::filesystem::path("/proj/assets/materials")),
              AssetPathKey(std::filesystem::path("/proj/Assets/Materials")));
    EXPECT_TRUE(IsDirectChildOfAssetDir(std::filesystem::path("/proj/assets"),
                                        std::filesystem::path("/proj/Assets/Rock.mat")));
}

TEST(AssetPathKey, DistinctDirectoriesKeepDistinctKeys)
{
    EXPECT_NE(AssetPathKey(std::filesystem::path("/proj/Assets/Materials")),
              AssetPathKey(std::filesystem::path("/proj/Assets/Meshes")));
}

// Unicode full case folding, not ASCII tolower: the registry folds the German
// sharp s to "ss", so this key must too. This pins agreement with the registry
// key and nothing further — a non-ASCII project root does not currently reach
// the browser intact, because RegisterSource folds the root and then creates
// the folded directory.
TEST(AssetPathKey, NonAsciiFoldsTheSameWayTheRegistryFoldsIt)
{
    const std::filesystem::path sharpS(reinterpret_cast<const char8_t*>(u8"C:/Dev/Stra\u00dfe/Assets"));
    const std::filesystem::path spelledOut("C:/Dev/STRASSE/Assets");
    EXPECT_EQ(AssetPathKey(sharpS), AssetPathKey(spelledOut));
}

// The contract that makes the browser and the registry meet: this key is the
// registry's own key. If they ever diverge, watcher events stop matching the
// browser's current directory again.
TEST(AssetPathKey, MatchesTheRegistryKeyForAnAbsolutePath)
{
    const std::filesystem::path path = HostRoot() / "Dev" / "Scratch" / "Player" / "Assets";
    EXPECT_EQ(AssetPathKey(path), GameEngine::AssetPaths::NormalizeForRegistryKey(path));
}

// Renaming the folder the grid is showing: the browsed directory is the
// folded one the registry handed over, the event names the on-disk case.
TEST(RebaseRenamedAssetDir, FollowsARenameOfTheBrowsedDirectoryItself)
{
    const std::filesystem::path browsed = std::filesystem::path(kFoldedRoot) / "materials";
    const std::filesystem::path oldPath = std::filesystem::path(kOnDiskRoot) / "Materials";
    const std::filesystem::path newPath = std::filesystem::path(kOnDiskRoot) / "Surfaces";

    const auto rebased = RebaseRenamedAssetDir(browsed, oldPath, newPath);
    ASSERT_TRUE(rebased.has_value());
    EXPECT_EQ(AssetPathKey(*rebased), AssetPathKey(newPath));
}

TEST(RebaseRenamedAssetDir, FollowsARenameOfAnAncestorAndKeepsTheTail)
{
    const std::filesystem::path browsed =
        std::filesystem::path(kFoldedRoot) / "materials" / "rock";
    const std::filesystem::path oldPath = std::filesystem::path(kOnDiskRoot) / "Materials";
    const std::filesystem::path newPath = std::filesystem::path(kOnDiskRoot) / "Surfaces";

    const auto rebased = RebaseRenamedAssetDir(browsed, oldPath, newPath);
    ASSERT_TRUE(rebased.has_value());
    EXPECT_EQ(AssetPathKey(*rebased), AssetPathKey(newPath / "rock"));
}

TEST(RebaseRenamedAssetDir, IgnoresARenameOfAnUnrelatedSibling)
{
    const std::filesystem::path browsed = std::filesystem::path(kFoldedRoot) / "materials";
    const std::filesystem::path oldPath = std::filesystem::path(kOnDiskRoot) / "Meshes";
    const std::filesystem::path newPath = std::filesystem::path(kOnDiskRoot) / "Models";

    EXPECT_FALSE(RebaseRenamedAssetDir(browsed, oldPath, newPath).has_value());
}

// A descendant rename must not drag the browsed directory anywhere: the
// folder being shown did not move.
TEST(RebaseRenamedAssetDir, IgnoresARenameBeneathTheBrowsedDirectory)
{
    const std::filesystem::path browsed = std::filesystem::path(kFoldedRoot) / "materials";
    const std::filesystem::path oldPath =
        std::filesystem::path(kOnDiskRoot) / "Materials" / "Rock";
    const std::filesystem::path newPath =
        std::filesystem::path(kOnDiskRoot) / "Materials" / "Stone";

    EXPECT_FALSE(RebaseRenamedAssetDir(browsed, oldPath, newPath).has_value());
}

TEST(RebaseRenamedAssetDir, EmptyInputsRebaseNothing)
{
    const std::filesystem::path browsed(kFoldedRoot);
    const std::filesystem::path other(kOnDiskRoot);
    EXPECT_FALSE(RebaseRenamedAssetDir({}, other, other).has_value());
    EXPECT_FALSE(RebaseRenamedAssetDir(browsed, {}, other).has_value());
    EXPECT_FALSE(RebaseRenamedAssetDir(browsed, other, {}).has_value());
}

// A directory spelled with a trailing separator is the same directory. The
// browser joins its roots, so this is latent rather than live — but the two
// spellings meeting is the whole contract of this key.
TEST(AssetPathKey, TrailingSeparatorDoesNotChangeTheKey)
{
    EXPECT_EQ(AssetPathKey(std::filesystem::path(kOnDiskRoot) / ""),
              AssetPathKey(std::filesystem::path(kOnDiskRoot)));
    EXPECT_EQ(AssetPathKey(std::filesystem::path("C:/Dev/Scratch/Player/Assets/")),
              AssetPathKey(std::filesystem::path(kFoldedRoot)));
}

TEST(AssetPathKey, TrailingSeparatorDirStillMatchesItsChildren)
{
    EXPECT_TRUE(IsDirectChildOfAssetDir(std::filesystem::path("C:/Dev/Scratch/Player/Assets/"),
                                        std::filesystem::path(kOnDiskRoot) / "Rock.mat"));
}

// A filesystem root is spelled with its separator, and there the separator IS
// the path rather than a spelling of one — the trailing-separator trim must
// leave it alone, or the root keys as "c:" / "" and matches nothing.
//
// Which half of that trim's guard runs is platform-dependent, so this case
// pins the invariant rather than one branch: on Windows "c:/" survives via the
// drive-root test (key[size - 2] != ':'), on POSIX "/" survives via the
// size > 1 test. Only Windows exercises the drive-root branch.
TEST(AssetPathKey, FilesystemRootKeepsItsSeparator)
{
    const std::string key = AssetPathKey(HostRoot());
    ASSERT_FALSE(key.empty());
    EXPECT_EQ(key.back(), '/');
    EXPECT_EQ(key, GameEngine::AssetPaths::NormalizeForRegistryKey(HostRoot()));
}

TEST(IsUnderAssetDir, MatchesADescendantAtAnyDepthAcrossCase)
{
    EXPECT_TRUE(IsUnderAssetDir(std::filesystem::path(kFoldedRoot),
                                std::filesystem::path(kOnDiskRoot) / "Materials" / "Rock.mat"));
    EXPECT_TRUE(IsUnderAssetDir(std::filesystem::path(kFoldedRoot),
                                std::filesystem::path(kOnDiskRoot) / "Materials"));
}

TEST(IsUnderAssetDir, ADirectoryIsNotUnderItself)
{
    EXPECT_FALSE(IsUnderAssetDir(std::filesystem::path(kFoldedRoot),
                                 std::filesystem::path(kOnDiskRoot)));
}

// Whole-segment matching: a raw string prefix test would call ".../Rocks" a
// descendant of ".../Rock".
TEST(IsUnderAssetDir, ASiblingSharingANamePrefixIsNotADescendant)
{
    EXPECT_FALSE(IsUnderAssetDir(std::filesystem::path(kOnDiskRoot) / "Rock",
                                 std::filesystem::path(kOnDiskRoot) / "Rocks" / "a.mat"));
}

TEST(IsUnderAssetDir, EmptyInputsAreNeverRelated)
{
    EXPECT_FALSE(IsUnderAssetDir({}, std::filesystem::path(kOnDiskRoot)));
    EXPECT_FALSE(IsUnderAssetDir(std::filesystem::path(kFoldedRoot), {}));
}
