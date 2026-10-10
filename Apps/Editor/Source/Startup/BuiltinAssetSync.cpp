#include "Startup/BuiltinAssetSync.h"

#include "Types/StringUtils.h" // ToLowerAscii (header-only; keeps this TU Engine-link-free)

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <system_error>
#include <unordered_set>

namespace GameEngine::Editor::Startup
{
namespace fs = std::filesystem;

namespace
{
constexpr size_t kCompareChunkSize = 64 * 1024;

bool FilesAreIdentical(const fs::path& a, const fs::path& b,
                       std::vector<char>& bufferA, std::vector<char>& bufferB)
{
    std::error_code ec;
    const std::uintmax_t sizeA = fs::file_size(a, ec);
    if (ec)
        return false;
    const std::uintmax_t sizeB = fs::file_size(b, ec);
    if (ec || sizeA != sizeB)
        return false;

    std::ifstream streamA(a, std::ios::binary);
    std::ifstream streamB(b, std::ios::binary);
    if (!streamA || !streamB)
        return false;

    std::uintmax_t remaining = sizeA;
    while (remaining > 0)
    {
        const std::streamsize toRead =
            static_cast<std::streamsize>(std::min<std::uintmax_t>(kCompareChunkSize, remaining));
        streamA.read(bufferA.data(), toRead);
        streamB.read(bufferB.data(), toRead);
        if (streamA.gcount() != toRead || streamB.gcount() != toRead)
            return false;
        if (std::memcmp(bufferA.data(), bufferB.data(), static_cast<size_t>(toRead)) != 0)
            return false;
        remaining -= static_cast<std::uintmax_t>(toRead);
    }
    return true;
}

// Manifest entries must be clean relative paths; anything that could resolve
// outside dstRoot (absolute paths, drive letters, parent references) is ignored.
bool IsSafeManifestEntry(const fs::path& entry)
{
    if (entry.empty())
        return false;
    if (entry.is_absolute() || entry.has_root_name())
        return false;
    for (const auto& part : entry)
    {
        if (part == "..")
            return false;
    }
    return true;
}
} // namespace

bool CopyMissingFilesRecursive(const fs::path& srcRoot, const fs::path& dstRoot, std::string* outError)
{
    std::error_code ec;
    if (srcRoot.empty() || dstRoot.empty())
    {
        if (outError) *outError = "Source/destination root is empty";
        return false;
    }

    if (!fs::exists(srcRoot, ec) || !fs::is_directory(srcRoot, ec))
    {
        if (outError) *outError = "Install assets root does not exist or is not a directory: " + srcRoot.string();
        return false;
    }

    fs::create_directories(dstRoot, ec);
    if (ec)
    {
        if (outError) *outError = "Failed to create user assets directory: " + dstRoot.string() + " (" + ec.message() + ")";
        return false;
    }

    for (fs::recursive_directory_iterator it(srcRoot, ec), end; it != end && !ec; it.increment(ec))
    {
        const fs::directory_entry& entry = *it;
        if (!entry.is_regular_file(ec))
            continue;

        // Iterator entries have srcRoot as a lexical prefix, so no filesystem
        // canonicalization (fs::relative) is needed.
        fs::path rel = entry.path().lexically_relative(srcRoot);
        if (rel.empty())
            continue;

        fs::path dstFile = (dstRoot / rel).lexically_normal();
        if (fs::exists(dstFile, ec))
        {
            ec.clear();
            continue; // do not overwrite user edits
        }
        ec.clear();

        fs::create_directories(dstFile.parent_path(), ec);
        ec.clear();

        fs::copy_file(entry.path(), dstFile, fs::copy_options::skip_existing, ec);
        ec.clear();
    }

    if (ec)
    {
        if (outError) *outError = "Seeding editor assets failed: " + ec.message();
        return false;
    }

    return true;
}

bool RefreshOwnedTree(const fs::path& srcRoot,
                      const fs::path& dstRoot,
                      const OwnedTreeSpec& spec,
                      std::vector<std::string>& outShippedFiles,
                      BuiltinSyncStats& stats,
                      std::string* outError)
{
    std::error_code ec;
    const fs::path srcDir = (srcRoot / spec.RelativeRoot).lexically_normal();
    const fs::path dstDir = (dstRoot / spec.RelativeRoot).lexically_normal();
    if (!fs::exists(srcDir, ec) || !fs::is_directory(srcDir, ec))
    {
        return true; // nothing shipped for this tree
    }

    std::vector<std::string> extensionsLower;
    extensionsLower.reserve(spec.Extensions.size());
    for (const std::string& extension : spec.Extensions)
    {
        extensionsLower.push_back(ToLowerAscii(extension));
    }

    std::vector<char> compareBufferA(kCompareChunkSize);
    std::vector<char> compareBufferB(kCompareChunkSize);

    for (fs::recursive_directory_iterator it(srcDir, ec), end; it != end && !ec; it.increment(ec))
    {
        if (!spec.Recursive)
        {
            it.disable_recursion_pending(); // top-level entries only
        }

        const fs::directory_entry& entry = *it;
        std::error_code fec;
        if (!entry.is_regular_file(fec))
            continue;

        if (!extensionsLower.empty())
        {
            const std::string ext = ToLowerAscii(entry.path().extension().string());
            if (std::find(extensionsLower.begin(), extensionsLower.end(), ext) == extensionsLower.end())
                continue;
        }

        const fs::path rel = entry.path().lexically_relative(srcDir);
        if (rel.empty())
            continue;

        const fs::path dstFile = (dstDir / rel).lexically_normal();
        outShippedFiles.push_back((spec.RelativeRoot / rel).lexically_normal().generic_string());

        // FilesAreIdentical fails cleanly (size query error) when dstFile is missing.
        if (FilesAreIdentical(entry.path(), dstFile, compareBufferA, compareBufferB))
        {
            ++stats.SkippedIdentical;
            continue;
        }

        fs::create_directories(dstFile.parent_path(), fec);
        fs::copy_file(entry.path(), dstFile, fs::copy_options::overwrite_existing, fec);
        if (fec)
        {
            if (outError) *outError = std::string("Failed to refresh built-in ") + spec.AssetKind +
                                      ": " + dstFile.string() + " (" + fec.message() + ")";
            return false;
        }
        ++stats.Copied;
    }

    if (ec)
    {
        if (outError) *outError = std::string("Refreshing built-in ") + spec.AssetKind +
                                  " tree failed: " + ec.message();
        return false;
    }

    return true;
}

fs::path BuiltinManifestPathFor(const fs::path& mirrorRoot)
{
    // A root written with a trailing separator ("C:/x/mirror/") has an empty
    // filename; the mirror is its parent.
    const fs::path mirror = mirrorRoot.has_filename() ? mirrorRoot : mirrorRoot.parent_path();
    return mirror.parent_path() / (mirror.filename().string() + ".builtin-manifest");
}

bool PruneStaleOwnedFiles(const fs::path& dstRoot,
                          const fs::path& manifestPath,
                          const std::vector<std::string>& currentShippedFiles,
                          BuiltinSyncStats& stats,
                          std::string* outError)
{
    // Case-insensitive membership: shipped trees can be renamed only by case, and
    // deleting the just-refreshed file on a case-insensitive filesystem would be
    // worse than leaving a stale entry.
    std::unordered_set<std::string> currentLower;
    currentLower.reserve(currentShippedFiles.size());
    for (const std::string& shipped : currentShippedFiles)
    {
        currentLower.insert(ToLowerAscii(shipped));
    }

    std::vector<std::string> previousEntries;
    {
        std::ifstream in(manifestPath);
        std::string line;
        while (in && std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty() || line[0] == '#')
                continue;
            previousEntries.push_back(std::move(line));
        }
    }

    for (const std::string& entry : previousEntries)
    {
        const fs::path entryPath = fs::path(entry).lexically_normal();
        if (!IsSafeManifestEntry(entryPath))
            continue;
        if (currentLower.count(ToLowerAscii(entryPath.generic_string())) != 0)
            continue;

        std::error_code removeEc;
        if (fs::remove(dstRoot / entryPath, removeEc) && !removeEc)
        {
            ++stats.RemovedStale;
        }
    }

    std::error_code ec;
    fs::create_directories(manifestPath.parent_path(), ec);
    std::ofstream out(manifestPath, std::ios::binary | std::ios::trunc);
    out << "# Engine-owned editor assets shipped by this install (auto-generated; do not edit).\n";
    out << "# Entries removed from a future install are deleted from the user copy on launch.\n";
    for (const std::string& shipped : currentShippedFiles)
    {
        out << shipped << '\n';
    }
    out.flush();
    if (!out)
    {
        if (outError) *outError = "Failed to write builtin asset manifest: " + manifestPath.string();
        return false;
    }

    return true;
}
} // namespace GameEngine::Editor::Startup
