// Phase B1 smoke test for the vendored fbtBlend .blend file parser.
//
// Goals:
//   1. The library compiles + links (target builds at all means GE_HAVE_FBTBLEND
//      is wired correctly).
//   2. fbtBlend can open every .blend file under
//      Tests/BlendSamples/ without parse errors.
//   3. The parser exposes Object / Mesh / Armature / Action lists for each file.
//   4. The largest sample (~50 MB) parses in under 500 ms on this machine
//      (the budget the plan locked in for B1).
//   5. Bytes that are not a readable .blend (junk, a truncated zstd frame, a
//      gzip legacy blend without blocks) fail through the model loader's
//      in-memory parse entry point without touching freed memory, and a gzip
//      blend's header survives inflation.
//   6. A minimal complete blend parses through the same entry point raw, as
//      gzip and as zstd.
//
// The sample tests SKIP (rather than fail) when blend-samples/ is not
// extracted — the bundles are large + user-local + gitignored. Hydrate on any
// machine with the opt-in fetch target (public Blender bundles, hash-pinned):
//   cmake --build --preset <preset> --target FetchBlendTestSamples
// The generated-input tests build their bytes in place and always run.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "StagedTestPaths.h"

// FBT classes are defined inline in fbtBlend.h. Implementation TU lives in
// Source/ThirdParty/fbtBlend/fbtBlend.cpp; this test consumes the
// public class surface only (no FBTBLEND_IMPLEMENTATION here).
//
// fbtBlend.h emits MSVC narrowing warnings (size_t -> int) that would be
// promoted to errors under our default /WX. Suppress at the include
// boundary so the test TU stays warnings-clean.
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable: 4244)
#  pragma warning(disable: 4267)
#  pragma warning(disable: 4305)
#  pragma warning(disable: 4309)
#  pragma warning(disable: 4838)
#  pragma warning(disable: 4996)
#endif
#include "fbtBlend.h"
#if defined(_MSC_VER)
#  pragma warning(pop)
#endif

#if defined(GE_FBTBLEND_HAVE_ZLIB)
#  include <zlib.h>
#endif
#if defined(GE_FBTBLEND_HAVE_ZSTD)
#  include <zstd.h>
#endif

// The parser's built-in SDNA, defined in the fbtBlend.cpp implementation TU.
extern unsigned char bfBlenderFBT[];
extern int bfBlenderLen;

namespace fs = std::filesystem;

namespace
{
    // Walk an intrusive fbtList and count its members (the class only stores
    // first/last; iteration is via the embedded next pointer that lives on
    // every Blender DNA datablock head).
    std::size_t CountFbtList(const fbtList& list)
    {
        std::size_t count = 0;
        for (fbtList::Link* link = list.first; link != nullptr; link = link->next)
        {
            ++count;
        }
        return count;
    }

    // Staged mirror of the (gitignored, hand-extracted) .blend samples —
    // StageTestAssets copies them under the build root when present on this
    // machine. Returns empty when absent; callers skip.
    fs::path ResolveSamplesRoot()
    {
        const fs::path staged =
            GameEngine::TestPaths::StagedRoot() / "Tests" / "BlendSamples";
        std::error_code ec;
        return fs::is_directory(staged, ec) ? staged : fs::path{};
    }

    std::vector<fs::path> CollectBlendFiles(const fs::path& root)
    {
        std::vector<fs::path> out;
        if (root.empty())
        {
            return out;
        }
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root, ec);
             !ec && it != fs::recursive_directory_iterator();
             it.increment(ec))
        {
            if (!it->is_regular_file(ec)) continue;
            const fs::path& path = it->path();
            if (path.extension() == ".blend")
            {
                out.push_back(path);
            }
        }
        return out;
    }

    struct ParseSummary
    {
        fs::path path;
        std::uintmax_t bytes = 0;
        double parseMs = 0.0;
        std::size_t objectCount = 0;
        std::size_t meshCount = 0;
        std::size_t armatureCount = 0;
        std::size_t actionCount = 0;
        int parseStatus = 0;
        bool ok = false;
    };

    ParseSummary ParseOne(const fs::path& path)
    {
        ParseSummary summary;
        summary.path = path;
        std::error_code ec;
        summary.bytes = fs::file_size(path, ec);

        const std::string utf8 = path.string();

        fbtBlend blend;
        const auto t0 = std::chrono::steady_clock::now();
        // Auto-detect compression via the BLENDER magic prefix (fbtBlend's
        // PM_UNCOMPRESSED-or-PM_COMPRESSED autoselect mode).
        summary.parseStatus = blend.parse(utf8.c_str());
        const auto t1 = std::chrono::steady_clock::now();
        summary.parseMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        summary.ok = (summary.parseStatus == fbtFile::FS_OK);

        if (summary.ok)
        {
            summary.objectCount   = CountFbtList(blend.m_object);
            summary.meshCount     = CountFbtList(blend.m_mesh);
            summary.armatureCount = CountFbtList(blend.m_armature);
            summary.actionCount   = CountFbtList(blend.m_action);
        }
        return summary;
    }

    // The mode selection ModelAsset::LoadFromBlendData makes: bytes without the
    // "BLENDER" magic go to the in-memory parse as compressed (zstd, then gzip).
    int ParseLikeModelLoader(const std::vector<unsigned char>& bytes)
    {
        const bool isUncompressed =
            bytes.size() >= 7 && std::memcmp(bytes.data(), "BLENDER", 7) == 0;
        fbtBlend blend;
        return blend.parse(bytes.data(), bytes.size(),
                           isUncompressed ? fbtFile::PM_UNCOMPRESSED : fbtFile::PM_COMPRESSED,
                           /*suppressHeaderWarning=*/false);
    }

    constexpr char kLegacyBlendHeader[] = "BLENDER-v279";
    constexpr std::size_t kHeaderOnlyBlendBytes = 1040;

    // A Blender 2.79 header followed by zeros: no blocks, and a zero block
    // header ends the block scan with FS_INV_READ.
    std::vector<unsigned char> HeaderOnlyBlend()
    {
        std::vector<unsigned char> bytes(kHeaderOnlyBlendBytes, 0);
        std::memcpy(bytes.data(), kLegacyBlendHeader, sizeof(kLegacyBlendHeader) - 1);
        return bytes;
    }

    constexpr char kBlend405Header[] = "BLENDER-v405";
    constexpr std::size_t kBlockHeaderBytes = 24;

    // A block header as a 64-bit little-endian file stores it ('-' and 'v'
    // in the header): code, length, old pointer, SDNA index, count.
    void AppendBlockHeader(std::vector<unsigned char>& bytes, const char* code,
                           std::uint32_t length, std::uint32_t count)
    {
        unsigned char header[kBlockHeaderBytes] = {};
        std::memcpy(header, code, 4);
        std::memcpy(header + 4, &length, sizeof(length));
        std::memcpy(header + 20, &count, sizeof(count));
        bytes.insert(bytes.end(), header, header + kBlockHeaderBytes);
    }

    // The smallest .blend the parser reads to the end: a header, a DNA1 block
    // holding the parser's own SDNA, and ENDB.
    std::vector<unsigned char> MinimalBlend()
    {
        std::vector<unsigned char> bytes(kBlend405Header, kBlend405Header + sizeof(kBlend405Header) - 1);
        AppendBlockHeader(bytes, "DNA1", static_cast<std::uint32_t>(bfBlenderLen), 1);
        bytes.insert(bytes.end(), bfBlenderFBT, bfBlenderFBT + bfBlenderLen);
        AppendBlockHeader(bytes, "ENDB", 0, 0);
        return bytes;
    }

#if defined(GE_FBTBLEND_HAVE_ZLIB)
    constexpr int kZlibDefaultMemLevel = 8;

    // Deflate in a gzip wrapper (windowBits 16 + MAX_WBITS), the container
    // Blender < 3.0 wrote for a compressed .blend.
    std::vector<unsigned char> GzipCompress(const std::vector<unsigned char>& bytes, int level)
    {
        z_stream strm{};
        if (deflateInit2(&strm, level, Z_DEFLATED, 16 + MAX_WBITS,
                         kZlibDefaultMemLevel, Z_DEFAULT_STRATEGY) != Z_OK)
        {
            return {};
        }
        std::vector<unsigned char> out(deflateBound(&strm, static_cast<uLong>(bytes.size())));
        strm.next_in   = const_cast<Bytef*>(bytes.data());
        strm.avail_in  = static_cast<uInt>(bytes.size());
        strm.next_out  = out.data();
        strm.avail_out = static_cast<uInt>(out.size());
        const int status = deflate(&strm, Z_FINISH);
        out.resize(strm.total_out);
        deflateEnd(&strm);
        return status == Z_STREAM_END ? out : std::vector<unsigned char>{};
    }
#endif

#if defined(GE_FBTBLEND_HAVE_ZSTD)
    std::vector<unsigned char> ZstdCompress(const std::vector<unsigned char>& bytes)
    {
        std::vector<unsigned char> out(ZSTD_compressBound(bytes.size()));
        const std::size_t written =
            ZSTD_compress(out.data(), out.size(), bytes.data(), bytes.size(), ZSTD_CLEVEL_DEFAULT);
        if (ZSTD_isError(written))
        {
            return {};
        }
        out.resize(written);
        return out;
    }
#endif
}

class BlendSmokeParseTests : public ::testing::Test
{
protected:
    static fs::path& Root()
    {
        static fs::path s_root = ResolveSamplesRoot();
        return s_root;
    }

    static const std::vector<fs::path>& Files()
    {
        static std::vector<fs::path> s_files = CollectBlendFiles(Root());
        return s_files;
    }
};

TEST_F(BlendSmokeParseTests, SamplesDirectoryHydrated)
{
    if (Root().empty())
    {
        GTEST_SKIP() << "Tests/BlendSamples not staged; hydrate with "
                        "`cmake --build --preset <preset> --target FetchBlendTestSamples`.";
    }
    EXPECT_FALSE(Files().empty()) << "blend-samples/ exists but contains no .blend files. "
                                  << "Delete it and re-run the FetchBlendTestSamples target.";
}

TEST_F(BlendSmokeParseTests, EveryBlendFileParsesWithoutError)
{
    if (Files().empty())
    {
        GTEST_SKIP() << "no .blend files found under blend-samples/.";
    }

    std::vector<ParseSummary> summaries;
    summaries.reserve(Files().size());
    for (const fs::path& path : Files())
    {
        ParseSummary summary = ParseOne(path);
        summaries.push_back(summary);

        EXPECT_TRUE(summary.ok)
            << "fbtBlend::parse failed for " << summary.path.string()
            << " (status=" << summary.parseStatus << "). "
            << "Either the file is from a Blender version > 5.1 (raise upstream + "
            << "bump fbtBlend) or the file is compressed and the matching codec "
            << "(zlib for <3.0, zstd for >=3.0) is not linked.";

        // A .blend without ANY of these lists is almost certainly a parse
        // failure that fbtBlend silently glossed over (returns FS_OK on truly
        // empty files but our 3 bundles all carry meshes / armatures / actions).
        const std::size_t totalBlocks = summary.objectCount + summary.meshCount
                                      + summary.armatureCount + summary.actionCount;
        EXPECT_GT(totalBlocks, 0u)
            << "fbtBlend reported FS_OK but produced 0 datablocks for "
            << summary.path.string();
    }

    // Print summary table to stdout (visible via --gtest_print_time=1 / on failure).
    std::printf("\n[BlendSmokeParseTests] Parsed %zu .blend file(s):\n", summaries.size());
    std::printf("  %-70s %10s %8s %5s %5s %5s %5s\n",
                "path", "bytes", "ms", "obj", "mesh", "arm", "act");
    for (const auto& summary : summaries)
    {
        std::printf("  %-70s %10llu %8.2f %5zu %5zu %5zu %5zu\n",
                    summary.path.filename().string().c_str(),
                    static_cast<unsigned long long>(summary.bytes),
                    summary.parseMs,
                    summary.objectCount, summary.meshCount,
                    summary.armatureCount, summary.actionCount);
    }
    std::fflush(stdout);
}

// Plan B1 budget: parse a typical 50 MB .blend in <= 500 ms. Run on the largest
// file in the corpus and assert the budget. We measure best-of-3 so jitter
// from background processes (antivirus, indexer) doesn't flake the gate.
TEST_F(BlendSmokeParseTests, LargestFileParsesWithinBudget)
{
    if (Files().empty())
    {
        GTEST_SKIP() << "no .blend files found under blend-samples/.";
    }

    fs::path largest;
    std::uintmax_t largestBytes = 0;
    for (const fs::path& path : Files())
    {
        std::error_code ec;
        const std::uintmax_t bytes = fs::file_size(path, ec);
        if (ec) continue;
        if (bytes > largestBytes)
        {
            largestBytes = bytes;
            largest = path;
        }
    }

    ASSERT_FALSE(largest.empty());

    constexpr int kIterations = 3;
    double bestMs = std::numeric_limits<double>::infinity();
    for (int i = 0; i < kIterations; ++i)
    {
        ParseSummary summary = ParseOne(largest);
        ASSERT_TRUE(summary.ok)
            << "Largest sample failed to parse: " << largest.string()
            << " (status=" << summary.parseStatus << ").";
        bestMs = std::min(bestMs, summary.parseMs);
    }

    // Plan B1 budget: 500 ms / 50 MB on Release. Debug builds run with /MDd,
    // /RTC1, validation layers, and Address Sanitizer-style runtime checks
    // that cost roughly 3-4x. We multiply the budget by 3x in Debug so the
    // gate measures the right thing on each config; the user-facing
    // engineering budget remains the Release number.
#if defined(_DEBUG) || defined(GE_DEBUG_BUILD)
    constexpr double kBudgetMsPer50MB = 500.0 * 3.0;
    constexpr const char* kBudgetLabel = "Debug (3x Release)";
#else
    constexpr double kBudgetMsPer50MB = 500.0;
    constexpr const char* kBudgetLabel = "Release";
#endif
    const double budgetMs = kBudgetMsPer50MB
                          * (static_cast<double>(largestBytes) / (50.0 * 1024.0 * 1024.0));

    std::printf("\n[BlendSmokeParseTests] Largest=%s bytes=%llu best-of-%d=%.2f ms budget=%.2f ms (%s)\n",
                largest.filename().string().c_str(),
                static_cast<unsigned long long>(largestBytes),
                kIterations, bestMs, budgetMs, kBudgetLabel);
    std::fflush(stdout);

    EXPECT_LE(bestMs, budgetMs)
        << "fbtBlend parse exceeded the 500 ms budget for a 50 MB file (which "
        << "already carries 3x headroom for Debug-build runtime overheads).";
}

// Bytes in no .blend container reach the zstd decoder first and the gzip
// decoder second. Both must fail and leave the stream closed, so the parse
// reports FS_FAILED instead of reading or freeing the buffer a decoder dropped.
TEST_F(BlendSmokeParseTests, JunkBytesFailToOpen)
{
    const std::string junk = "not a blend\n";
    EXPECT_EQ(ParseLikeModelLoader({junk.begin(), junk.end()}), fbtFile::FS_FAILED);
}

TEST_F(BlendSmokeParseTests, TruncatedZstdBlendFailsToOpen)
{
#if defined(GE_FBTBLEND_HAVE_ZSTD)
    std::vector<unsigned char> frame = ZstdCompress(HeaderOnlyBlend());
    ASSERT_FALSE(frame.empty());
    frame.resize(frame.size() / 2);
    EXPECT_EQ(ParseLikeModelLoader(frame), fbtFile::FS_FAILED);
#else
    GTEST_SKIP() << "built without zstd; a zstd .blend cannot be generated.";
#endif
}

// The zstd decoder rejects the gzip stream, the gzip decoder inflates it, and
// the block scan rejects the missing blocks. FS_INV_HEADER_STR instead of
// FS_INV_READ means the inflated header did not survive decompression.
TEST_F(BlendSmokeParseTests, GzipLegacyBlendWithoutBlocksFailsToParse)
{
#if defined(GE_FBTBLEND_HAVE_ZLIB)
    const std::vector<unsigned char> gzip = GzipCompress(HeaderOnlyBlend(), Z_BEST_COMPRESSION);
    ASSERT_FALSE(gzip.empty());
    EXPECT_EQ(ParseLikeModelLoader(gzip), fbtFile::FS_INV_READ);
#else
    GTEST_SKIP() << "built without zlib; a gzip .blend cannot be generated.";
#endif
}

// A complete .blend parses raw, as gzip at the fastest, default and best
// levels, and as zstd. Each gzip form inflates past its first output buffer,
// so FS_OK needs every inflated byte to survive the growth.
TEST_F(BlendSmokeParseTests, MinimalBlendParsesRawGzipAndZstd)
{
    const std::vector<unsigned char> blend = MinimalBlend();
    ASSERT_EQ(ParseLikeModelLoader(blend), fbtFile::FS_OK);
#if defined(GE_FBTBLEND_HAVE_ZLIB)
    for (const int level : {Z_BEST_SPEED, Z_DEFAULT_COMPRESSION, Z_BEST_COMPRESSION})
    {
        const std::vector<unsigned char> gzip = GzipCompress(blend, level);
        ASSERT_FALSE(gzip.empty());
        EXPECT_EQ(ParseLikeModelLoader(gzip), fbtFile::FS_OK) << "gzip level " << level;
    }
#endif
#if defined(GE_FBTBLEND_HAVE_ZSTD)
    const std::vector<unsigned char> zstd = ZstdCompress(blend);
    ASSERT_FALSE(zstd.empty());
    EXPECT_EQ(ParseLikeModelLoader(zstd), fbtFile::FS_OK);
#endif
}
