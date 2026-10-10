// The shader compile key is built from a source's #include closure: every
// included file is hashed, and scanned for its own directives, before the
// compile cache can be probed. That work is shared across shaders -- one engine
// header reaches dozens of them -- and is held in memory so it happens once
// rather than once per shader.
//
// Caching content is only safe while an edit cannot survive it, so the
// staleness cases here matter more than the saving does: the compile cache
// would rather recompile needlessly than serve SPIR-V built from bytes a file
// no longer has. The cache is VALIDATED rather than announced -- every lookup
// re-reads the file and compares the bytes -- so these tests never tell it
// anything. That is the point: no watcher runs in a cook, a test, or a player.
#include <gtest/gtest.h>

#include "Source/Materials/ShaderIncludeClosure.h"
#include "Source/Materials/ShaderSourceCache.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace GameEngine::Rendering;

namespace
{
namespace fs = std::filesystem;

void WriteFile(const fs::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

class ShaderSourceCacheTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        m_Root = fs::temp_directory_path() /
                 ("shader_source_cache_" + std::to_string(::testing::UnitTest::GetInstance()
                                                              ->random_seed()) +
                  "_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::remove_all(m_Root);
        fs::create_directories(m_Root);
    }

    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(m_Root, ec);
    }

    IncludeClosure CollectFor(const std::string& sourceText, const fs::path& source)
    {
        IncludeClosure closure;
        CollectIncludeClosure(sourceText, source, {m_Root}, closure);
        return closure;
    }

    // The cache's entry for `path`, under the key the closure walk uses. The
    // same entry handed back twice is the hash and scan served from memory;
    // a new one is the work redone.
    static ShaderSourceCache::EntryPtr EntryFor(const fs::path& path)
    {
        return ShaderSourceCache::Get().Read(path, fs::absolute(path).lexically_normal().string());
    }

    fs::path m_Root;
};

TEST_F(ShaderSourceCacheTest, SharedHeaderIsHashedOncePerEditNotOncePerShader)
{
    WriteFile(m_Root / "common.glsl", "// shared by both\n");
    const std::string shaderA = "#include \"common.glsl\"\nvoid a() {}\n";
    const std::string shaderB = "#include \"common.glsl\"\nvoid b() {}\n";

    const IncludeClosure a = CollectFor(shaderA, m_Root / "a.glsl");
    const ShaderSourceCache::EntryPtr afterFirst = EntryFor(m_Root / "common.glsl");
    const IncludeClosure b = CollectFor(shaderB, m_Root / "b.glsl");
    const ShaderSourceCache::EntryPtr afterSecond = EntryFor(m_Root / "common.glsl");

    ASSERT_EQ(a.paths.size(), 1u);
    ASSERT_EQ(b.paths.size(), 1u);
    EXPECT_EQ(a.hashes[0], b.hashes[0]) << "same file, same content, same hash";

    // The saving this exists for: the second shader re-reads the header --
    // that is what proves it unchanged -- but pays for neither the FNV pass
    // over it nor the scan of its directives. A shader tree where fifty
    // programs share a header is where that stops being a rounding error.
    EXPECT_EQ(afterFirst, afterSecond) << "the shared header was hashed again";

    // And an edit is what makes it pay again.
    WriteFile(m_Root / "common.glsl", "// shared by both, edited\n");
    EXPECT_NE(EntryFor(m_Root / "common.glsl"), afterSecond);
}

TEST_F(ShaderSourceCacheTest, TransitiveIncludesAreCachedToo)
{
    WriteFile(m_Root / "leaf.glsl", "// leaf\n");
    WriteFile(m_Root / "middle.glsl", "#include \"leaf.glsl\"\n");
    const std::string shader = "#include \"middle.glsl\"\n";

    const IncludeClosure first = CollectFor(shader, m_Root / "a.glsl");
    const ShaderSourceCache::EntryPtr leaf = EntryFor(m_Root / "leaf.glsl");
    const ShaderSourceCache::EntryPtr middle = EntryFor(m_Root / "middle.glsl");
    const IncludeClosure second = CollectFor(shader, m_Root / "b.glsl");

    EXPECT_EQ(first.paths.size(), 2u);
    EXPECT_EQ(second.paths.size(), 2u);
    EXPECT_EQ(EntryFor(m_Root / "leaf.glsl"), leaf) << "the transitive include was hashed again";
    EXPECT_EQ(EntryFor(m_Root / "middle.glsl"), middle);
}

TEST_F(ShaderSourceCacheTest, AnEditedIncludeChangesTheKeyWithNothingAnnounced)
{
    const fs::path header = m_Root / "common.glsl";
    WriteFile(header, "// before\n");
    const std::string shader = "#include \"common.glsl\"\n";

    const IncludeClosure before = CollectFor(shader, m_Root / "a.glsl");
    ASSERT_EQ(before.hashes.size(), 1u);

    // Nobody tells the cache anything. This is the case the whole design rests
    // on: miss it and the next compile keys on content the file no longer has,
    // then serves SPIR-V built from it.
    WriteFile(header, "// after, a different length entirely\n");

    const IncludeClosure after = CollectFor(shader, m_Root / "a.glsl");
    ASSERT_EQ(after.hashes.size(), 1u);
    EXPECT_NE(before.hashes[0], after.hashes[0])
        << "a cached include kept serving its old content";
}

TEST_F(ShaderSourceCacheTest, AnMtimeAndLengthPreservingEditStillChangesTheKey)
{
    // The case a timestamp check cannot see, and the reason the key is built
    // from content at all: same byte count, same mtime, different bytes. A
    // cache that revalidated on anything cheaper than the bytes would serve
    // the old hash here.
    const fs::path header = m_Root / "common.glsl";
    WriteFile(header, "// aaaa\n");
    const std::string shader = "#include \"common.glsl\"\n";

    const IncludeClosure before = CollectFor(shader, m_Root / "a.glsl");
    ASSERT_EQ(before.hashes.size(), 1u);
    const auto stamp = fs::last_write_time(header);
    const auto size = fs::file_size(header);

    WriteFile(header, "// bbbb\n");
    fs::last_write_time(header, stamp);
    ASSERT_EQ(fs::file_size(header), size) << "the rewrite must be the same length";
    ASSERT_EQ(fs::last_write_time(header), stamp) << "the rewrite must preserve the mtime";

    const IncludeClosure after = CollectFor(shader, m_Root / "a.glsl");
    ASSERT_EQ(after.hashes.size(), 1u);
    EXPECT_NE(before.hashes[0], after.hashes[0])
        << "an mtime-preserving rewrite slipped past the cache";
}

TEST_F(ShaderSourceCacheTest, AMissingIncludeAddsNothingToTheKey)
{
    // A directive naming nothing on disk resolves to no file at all, so the
    // closure records nothing for it -- the compile reports it, not the key.
    const std::string shader = "#include \"missing.glsl\"\n";
    const IncludeClosure closure = CollectFor(shader, m_Root / "a.glsl");
    EXPECT_TRUE(closure.paths.empty());
    EXPECT_TRUE(closure.hashes.empty());
}

TEST_F(ShaderSourceCacheTest, AnEntryWhoseFileCannotBeReadIsDropped)
{
    // A deleted header must not stay pinned in memory, and when a file of that
    // name comes back it is hashed afresh rather than matched to the old entry.
    const fs::path header = m_Root / "gone.glsl";
    WriteFile(header, "// here for now\n");
    const ShaderSourceCache::EntryPtr before = EntryFor(header);
    ASSERT_NE(before, nullptr);

    fs::remove(header);
    EXPECT_EQ(EntryFor(header), nullptr);

    WriteFile(header, "// here for now\n");
    const ShaderSourceCache::EntryPtr after = EntryFor(header);
    ASSERT_NE(after, nullptr);
    EXPECT_NE(after, before) << "the entry for the deleted file was kept";
    EXPECT_EQ(after->Hash, before->Hash);
}

TEST_F(ShaderSourceCacheTest, AnIncludeThatAppearsLaterEntersTheKeyWithoutInvalidation)
{
    // An author fixing a missing include by creating the file gets no
    // invalidation event. The next walk must find the file on its own, or the
    // program is keyed without it and later edits to it cannot move the key.
    const std::string shader = "#include \"appears_later.glsl\"\n";

    const IncludeClosure missing = CollectFor(shader, m_Root / "a.glsl");
    ASSERT_TRUE(missing.paths.empty());

    WriteFile(m_Root / "appears_later.glsl", "// created after the failed walk\n");

    const IncludeClosure found = CollectFor(shader, m_Root / "a.glsl");
    ASSERT_EQ(found.paths.size(), 1u) << "a failed resolution was remembered";
    EXPECT_NE(found.hashes[0], 0u);
}

TEST_F(ShaderSourceCacheTest, TheSameOperandFromTwoDirectoriesResolvesToEachOwnFile)
{
    // The requesting file's own directory is searched FIRST, so the identical
    // spelling must reach a different file depending on who wrote it. Serving
    // one shader the other's header would build the key from bytes that shader
    // never compiles.
    const fs::path nested = m_Root / "nested";
    fs::create_directories(nested);
    WriteFile(m_Root / "common.glsl", "// the root one\n");
    WriteFile(nested / "common.glsl", "// the nested one, shadowing it\n");

    const std::string shader = "#include \"common.glsl\"\n";
    const IncludeClosure fromRoot = CollectFor(shader, m_Root / "a.glsl");
    const IncludeClosure fromNested = CollectFor(shader, nested / "b.glsl");

    ASSERT_EQ(fromRoot.paths.size(), 1u);
    ASSERT_EQ(fromNested.paths.size(), 1u);
    EXPECT_NE(fromRoot.paths[0], fromNested.paths[0]);
    EXPECT_NE(fromRoot.hashes[0], fromNested.hashes[0])
        << "one directory's header was served to the other";
}

// The three cases below are what a file appearing or disappearing earlier in a
// search order does to the key. None of them is announced -- a cook, a test or
// a player has no watcher -- so each walk must reach the file the compiler
// would read now.
TEST_F(ShaderSourceCacheTest, AShadowingIncludeTakesOverWithNothingAnnounced)
{
    const fs::path nested = m_Root / "nested";
    fs::create_directories(nested);
    WriteFile(m_Root / "common.glsl", "// the root one\n");

    const std::string shader = "#include \"common.glsl\"\n";
    const IncludeClosure before = CollectFor(shader, nested / "b.glsl");
    ASSERT_EQ(before.paths.size(), 1u);

    const fs::path shadowing = nested / "common.glsl";
    WriteFile(shadowing, "// now shadowing the root one\n");

    const IncludeClosure after = CollectFor(shader, nested / "b.glsl");
    ASSERT_EQ(after.paths.size(), 1u);
    EXPECT_EQ(fs::path(after.paths[0]), fs::absolute(shadowing).lexically_normal())
        << "the key still folds the file the compiler no longer reads";
    EXPECT_NE(before.hashes[0], after.hashes[0]);
}

TEST_F(ShaderSourceCacheTest, DeletingAShadowingIncludeFallsBackWithNothingAnnounced)
{
    const fs::path nested = m_Root / "nested";
    fs::create_directories(nested);
    const fs::path rootCommon = m_Root / "common.glsl";
    const fs::path shadowing = nested / "common.glsl";
    WriteFile(rootCommon, "// the root one\n");
    WriteFile(shadowing, "// shadowing the root one\n");

    const std::string shader = "#include \"common.glsl\"\n";
    ASSERT_EQ(CollectFor(shader, nested / "b.glsl").paths.size(), 1u);

    fs::remove(shadowing);
    const IncludeClosure afterDelete = CollectFor(shader, nested / "b.glsl");
    ASSERT_EQ(afterDelete.paths.size(), 1u);
    EXPECT_EQ(fs::path(afterDelete.paths[0]), fs::absolute(rootCommon).lexically_normal())
        << "the key is stuck on a file that no longer exists";

    // And the file it fell back to still moves the key when edited.
    WriteFile(rootCommon, "// the root one, edited\n");
    const IncludeClosure afterEdit = CollectFor(shader, nested / "b.glsl");
    ASSERT_EQ(afterEdit.hashes.size(), 1u);
    EXPECT_NE(afterDelete.hashes[0], afterEdit.hashes[0]);
}

TEST_F(ShaderSourceCacheTest, AWarmWalkKeysWhatAFreshWalkOfTheSameDiskWould)
{
    // After any history of shadowing files coming and going, the closure is
    // the one a process that never saw that history computes from the disk
    // alone: the cook and the runtime must agree on the key.
    const fs::path nested = m_Root / "nested";
    fs::create_directories(nested);
    const fs::path rootCommon = m_Root / "common.glsl";
    const fs::path shadowing = nested / "common.glsl";
    const std::string shader = "#include \"common.glsl\"\n";
    WriteFile(rootCommon, "// the root one\n");
    CollectFor(shader, nested / "b.glsl");
    WriteFile(shadowing, "// shadowing\n");
    CollectFor(shader, nested / "b.glsl");
    fs::remove(shadowing);
    CollectFor(shader, nested / "b.glsl");
    const std::string finalText = "// the root one, final\n";
    WriteFile(rootCommon, finalText);

    const IncludeClosure warm = CollectFor(shader, nested / "b.glsl");
    ASSERT_EQ(warm.paths.size(), 1u);
    EXPECT_EQ(fs::path(warm.paths[0]), fs::absolute(rootCommon).lexically_normal());
    EXPECT_EQ(warm.hashes[0], HashShaderBytes(finalText.data(), finalText.size()));
}

TEST_F(ShaderSourceCacheTest, ClosureContentIsIdenticalWarmAndCold)
{
    WriteFile(m_Root / "leaf.glsl", "// leaf\n");
    WriteFile(m_Root / "middle.glsl", "#include \"leaf.glsl\"\n");
    const std::string shader = "#include \"middle.glsl\"\n#include \"leaf.glsl\"\n";

    const IncludeClosure cold = CollectFor(shader, m_Root / "a.glsl");
    const IncludeClosure warm = CollectFor(shader, m_Root / "a.glsl");

    // The cache must not change what the key is built from -- only how often
    // the bytes behind it are read.
    ASSERT_EQ(cold.paths, warm.paths);
    ASSERT_EQ(cold.hashes, warm.hashes);
}
} // namespace
