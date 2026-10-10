#include "AssetCore/PathNormalization.h"

#include <algorithm>
#include <cctype>
#include <system_error>
#include <vector>
#include <utf8proc.h>

namespace GameEngine::AssetPaths
{

namespace
{

// NFC + Unicode case fold as a caller-allocated two-phase conversion:
// utf8proc_decompose to size and fill an engine-owned codepoint buffer, then
// utf8proc_reencode in place. utf8proc.dll must never allocate a buffer this
// module frees — allocator handoff across the DLL boundary is the
// RtlValidateHeap cross-CRT trap documented in cmake/OutputLayout.cmake.
// On invalid UTF-8, returns the input unchanged so we degrade gracefully
// rather than silently dropping characters.
std::string Utf8NfcCasefold(std::string_view input)
{
    if (input.empty())
        return std::string();

    const auto* src = reinterpret_cast<const utf8proc_uint8_t*>(input.data());
    const auto srcLen = static_cast<utf8proc_ssize_t>(input.size());
    constexpr auto kOptions =
        static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_CASEFOLD);

    const utf8proc_ssize_t needed = utf8proc_decompose(src, srcLen, nullptr, 0, kOptions);
    if (needed < 0)
        return std::string(input);

    // One extra element so the buffer exceeds the codepoint data by at least
    // one byte — utf8proc_reencode requires the slack for its NUL terminator.
    std::vector<utf8proc_int32_t> buffer(static_cast<size_t>(needed) + 1);
    const utf8proc_ssize_t decomposed = utf8proc_decompose(src, srcLen, buffer.data(), needed, kOptions);
    if (decomposed < 0 || decomposed != needed)
        return std::string(input);

    const utf8proc_ssize_t outLen = utf8proc_reencode(buffer.data(), decomposed, kOptions);
    if (outLen < 0)
        return std::string(input);

    return std::string(reinterpret_cast<const char*>(buffer.data()), static_cast<size_t>(outLen));
}

// Convert backslashes to forward slashes in-place. Path separators are
// part of the canonical form so cross-platform paths hash identically.
void NormalizeSeparators(std::string& s)
{
    std::replace(s.begin(), s.end(), '\\', '/');
}

// Case-fold + separator-normalize, without any lexical path processing.
//
// F.12 ASCII fast path: for ASCII-only inputs (the overwhelming common case
// for filesystem paths), NFC is identity and Unicode case fold is equivalent
// to ASCII `tolower`. The full utf8proc pipeline's table lookups + alloc cost
// ~5-10μs per call; this single-pass casefold+slash-normalize is ~100ns. At
// 50K records during PopulateHotCachesFromSource on Windows, this is the
// difference between ~75ms of populate cost and ~5ms.
//
// Detection is one branch per byte in the same pass that does the
// transformation, so non-ASCII paths pay a cheap walk over the prefix before
// falling through to the full utf8proc path.
std::string FoldAndNormalizeSeparators(std::string_view input)
{
    std::string folded;
    folded.reserve(input.size());
    for (char c : input)
    {
        if (static_cast<unsigned char>(c) >= 0x80)
        {
            folded = Utf8NfcCasefold(input);
            NormalizeSeparators(folded);
            return folded;
        }
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c + ('a' - 'A'));
        else if (c == '\\')
            c = '/';
        folded.push_back(c);
    }
    return folded;
}

} // namespace

std::string CaseFoldUtf8(std::string_view utf8)
{
    return Utf8NfcCasefold(utf8);
}

std::string NormalizeForRegistryKey(std::string_view utf8Path)
{
    if (utf8Path.empty())
        return std::string();

    const std::string folded = FoldAndNormalizeSeparators(utf8Path);

    // Apply lexically_normal via std::filesystem to collapse "." and ".." segments
    // and redundant separators. UTF-8 must be preserved end-to-end:
    //
    //  - Construct from char8_t* (not narrow char*) so the path's internal
    //    encoding goes through the standard UTF-8 → UTF-16 conversion on
    //    Windows rather than being interpreted via the active code page.
    //  - Read back via generic_u8string() (not generic_string()) so the wide
    //    internal form is converted back to UTF-8, not narrow ACP. ACP would
    //    throw on characters outside the host's code page (e.g. Cyrillic on a
    //    Western-1252 host) — that would defeat the cross-platform guarantee.
    const auto* u8data = reinterpret_cast<const char8_t*>(folded.data());
    std::filesystem::path p(u8data, u8data + folded.size());
    const std::u8string canonical = p.lexically_normal().generic_u8string();
    return std::string(reinterpret_cast<const char*>(canonical.data()), canonical.size());
}

std::string NormalizeForRegistryKey(const std::filesystem::path& path)
{
    if (path.empty())
        return std::string();

    // generic_u8string returns UTF-8 with forward slashes. On Windows this
    // crosses the wide-string boundary correctly; on POSIX it's a no-op.
    const std::u8string u8 = path.generic_u8string();
    std::string_view view(reinterpret_cast<const char*>(u8.data()), u8.size());
    return NormalizeForRegistryKey(view);
}

std::filesystem::path NormalizeMountRoot(const std::filesystem::path& path)
{
    if (path.empty())
        return path;

    std::error_code ec;
    std::filesystem::path p = path.is_absolute() ? path : std::filesystem::absolute(path, ec);
    if (ec)
        p = path;
    p = p.lexically_normal();

    // A trailing separator makes filename() empty and parent_path() name the
    // same directory; strip it so a root spelled "Assets/" keys and joins like
    // "Assets". The loop stops where stepping up no longer changes the path,
    // which is the filesystem root itself.
    std::filesystem::path prev;
    while (!p.empty() && p.filename().empty())
    {
        prev = p;
        p = p.parent_path();
        if (p == prev)
            break;
    }

    return p;
}

std::string CanonicalizeStorePath(std::string s)
{
    for (auto& c : s)
    {
        if (c == '\\')
            c = '/';
    }
    while (s.size() >= 2 && s[0] == '.' && s[1] == '/')
        s.erase(0, 2);
    while (!s.empty() && s.back() == '/')
        s.pop_back();
    return s;
}

std::string FoldStorePathKey(std::string_view canonicalStorePath)
{
    if (canonicalStorePath.empty())
        return std::string();
    return FoldAndNormalizeSeparators(canonicalStorePath);
}

bool TryMakeCanonicalRelativePath(const std::filesystem::path& assetRoot,
                                  const std::filesystem::path& assetPath,
                                  std::string& outCanonical)
{
    outCanonical.clear();
    if (assetRoot.empty() || assetPath.empty())
        return false;

    // Skip the fs::absolute syscall when the input is already absolute — source
    // roots always are, and so are the registry-normalized paths on the hot path.
    std::error_code ec;
    const std::filesystem::path absRoot =
        (assetRoot.is_absolute() ? assetRoot : std::filesystem::absolute(assetRoot, ec)).lexically_normal();
    const std::filesystem::path absPath =
        (assetPath.is_absolute() ? assetPath : std::filesystem::absolute(assetPath, ec)).lexically_normal();

    auto elementKey = [](const std::filesystem::path& element) {
        const std::u8string u8 = element.generic_u8string();
        return FoldStorePathKey(std::string_view(reinterpret_cast<const char*>(u8.data()), u8.size()));
    };

    // Containment is decided element by element in the identity domain, so the
    // two sides may arrive in different spellings — a mount root carries the
    // caller's spelling while a registry-normalized asset path is folded on
    // case-insensitive platforms, and fs::path compares case-sensitively.
    // Comparing whole elements is also what keeps ".../Rock" out of ".../Rocks".
    auto rootIt = absRoot.begin();
    auto pathIt = absPath.begin();
    for (; rootIt != absRoot.end(); ++rootIt)
    {
        // A trailing separator yields a final empty element ("a/b/" → a, b, "").
        if (rootIt->empty())
            continue;
        if (pathIt == absPath.end() || elementKey(*rootIt) != elementKey(*pathIt))
            return false;
        ++pathIt;
    }

    // The tail keeps assetPath's OWN spelling. This string is what store rows
    // record and what gets joined back onto a root to reopen the file, and on a
    // case-sensitive filesystem only the on-disk spelling reopens it. Fold it at
    // the point of comparison instead (FoldStorePathKey), never here.
    for (; pathIt != absPath.end(); ++pathIt)
    {
        if (pathIt->empty())
            continue;
        const std::u8string u8 = pathIt->generic_u8string();
        if (!outCanonical.empty())
            outCanonical += '/';
        outCanonical.append(reinterpret_cast<const char*>(u8.data()), u8.size());
    }

    outCanonical = CanonicalizeStorePath(std::move(outCanonical));
    return !outCanonical.empty();
}

std::string_view StorePathParentDir(std::string_view canonicalPath)
{
    const size_t slash = canonicalPath.rfind('/');
    if (slash == std::string_view::npos)
        return {};
    return canonicalPath.substr(0, slash);
}

bool IsAbsoluteStorePath(std::string_view canonicalPath)
{
    if (canonicalPath.empty())
        return false;
    if (canonicalPath.front() == '/')
        return true;
    return canonicalPath.size() >= 3 &&
           std::isalpha(static_cast<unsigned char>(canonicalPath[0])) &&
           canonicalPath[1] == ':' &&
           canonicalPath[2] == '/';
}

} // namespace GameEngine::AssetPaths
