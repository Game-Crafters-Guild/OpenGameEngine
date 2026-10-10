#include "ShaderIncludeClosure.h"

#include "ShaderSourceCache.h"

#include <fstream>
#include <utility>

namespace GameEngine { namespace Rendering {

uint64_t HashShaderBytes(const void* data, size_t size, uint64_t seed)
{
    const uint8_t* b = static_cast<const uint8_t*>(data);
    constexpr uint64_t prime = 1099511628211ull;
    uint64_t h = seed;
    for (size_t i = 0; i < size; ++i)
    {
        h ^= static_cast<uint64_t>(b[i]);
        h *= prime;
    }
    return h;
}

uint64_t HashShaderString(uint64_t seed, const std::string& s)
{
    uint64_t h = HashShaderBytes(s.data(), s.size(), seed);
    const uint8_t delim = 0xFF;
    return HashShaderBytes(&delim, 1, h);
}

// One pass, no intermediate copy of the source: this runs on every compile-key
// construction, cache HITS included, so a second full-source pass would be real
// startup cost (a strip-comments-then-scan version measured 5x slower).
//
// A directive counts only when `#` is the first non-blank character on its line —
// what the preprocessor honours, and it also skips the commented-out usage
// examples the includes carry (Includes/light_packed_fields.glsl documents itself
// as `//     #include "light_packed_fields.glsl"`). A directive buried inside a
// `/* */` block WOULD still be scanned; nothing in the shader tree does that (233
// sources checked), and the failure mode is an extra file in the key, never a
// missing one.
//
// The macro form (`#include GE_VERTEX_MODIFIER_PATH`, left in adapter_vertex.glsl
// when no vertex modifier is bound) has no quoted operand and is skipped — those
// files reach the key only when the composer substitutes a literal, and are
// otherwise left to the cache-info revalidation.
std::vector<std::string> ScanShaderIncludeDirectives(const std::string& src)
{
    std::vector<std::string> operands;
    size_t lineStart = 0;
    while (lineStart < src.size())
    {
        size_t lineEnd = src.find('\n', lineStart);
        if (lineEnd == std::string::npos)
            lineEnd = src.size();

        size_t p = lineStart;
        auto skipBlanks = [&]() {
            while (p < lineEnd && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r'))
                ++p;
        };
        skipBlanks();
        if (p < lineEnd && src[p] == '#')
        {
            ++p;
            skipBlanks();
            static constexpr char kInclude[] = "include";
            const size_t kLen = sizeof(kInclude) - 1;
            if (lineEnd - p >= kLen && src.compare(p, kLen, kInclude) == 0)
            {
                p += kLen;
                skipBlanks();
                if (p < lineEnd && (src[p] == '"' || src[p] == '<'))
                {
                    const char close = src[p] == '"' ? '"' : '>';
                    const size_t open = ++p;
                    while (p < lineEnd && src[p] != close)
                        ++p;
                    if (p < lineEnd && p > open)
                        operands.emplace_back(src.substr(open, p - open));
                }
            }
        }
        lineStart = lineEnd + 1;
    }
    return operands;
}

bool ReadShaderFileText(const std::filesystem::path& p, std::string& out, std::string* outError)
{
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open())
    {
        if (outError)
            *outError = "Failed to open file: " + p.string();
        return false;
    }
    // Bulk read rather than std::istreambuf_iterator: the iterator form walks the
    // stream one character at a time, which dominated compile-key construction
    // once every file in a source's #include closure started being read there.
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    out.clear();
    if (size > 0)
    {
        out.resize(static_cast<size_t>(size));
        in.seekg(0, std::ios::beg);
        in.read(out.data(), size);
        out.resize(static_cast<size_t>(in.gcount()));
    }
    return true;
}

namespace {

// Where one #include directive resolves to.
struct IncludeResolution
{
    // The spelling the search produced, unnormalized: FileIncluder hands this
    // back as source_name, so nested relative directives inside the file must
    // resolve against it and not against the normalized form.
    std::filesystem::path Candidate;
    // Absolute and normalized — the identity a file is cached, deduped and
    // reported by.
    std::string Key;
};

// `dir / operand` if it exists. The normalized Key is the file's identity; the
// unnormalized Candidate is the spelling FileIncluder reports, which nested
// relative directives resolve against.
bool TryResolveIn(const std::filesystem::path& dir, const std::filesystem::path& operand,
                  IncludeResolution& out)
{
    std::error_code ec;
    std::filesystem::path candidate = dir / operand;
    if (!std::filesystem::exists(candidate, ec))
        return false;

    std::error_code absEc;
    std::filesystem::path absolutePath = std::filesystem::absolute(candidate, absEc);
    if (absEc)
        absolutePath = candidate;
    out.Key = absolutePath.lexically_normal().string();
    out.Candidate = std::move(candidate);
    return true;
}

// The first `dir / operand` that exists, searching the requesting file's own
// directory and then `roots` in order — FileIncluder's order. Searched on every
// walk, never remembered: which file a directive reaches depends on what is on
// disk now.
bool ResolveIncludeOperand(const std::filesystem::path& requestingDir,
                           const std::vector<std::filesystem::path>& roots,
                           const std::filesystem::path& operand, IncludeResolution& out)
{
    if (TryResolveIn(requestingDir, operand, out))
        return true;
    for (const std::filesystem::path& root : roots)
        if (TryResolveIn(root, operand, out))
            return true;
    return false;
}

// The closure walk over a file's already-scanned include operands. Splitting
// this from the text scan is what lets a file whose bytes are unchanged
// contribute its operands without its text being re-scanned.
void CollectFromOperands(const std::vector<std::string>& operands,
                         const std::filesystem::path& requestingDir,
                         const std::vector<std::filesystem::path>& roots, IncludeClosure& out)
{
    for (const std::string& operand : operands)
    {
        IncludeResolution resolved;
        if (!ResolveIncludeOperand(requestingDir, roots, operand, resolved))
            continue;

        const ShaderSourceCache::EntryPtr entry =
            ShaderSourceCache::Get().Read(resolved.Candidate, resolved.Key);
        if (!entry)
        {
            // Exists but unreadable. Fold a distinct marker so the key still
            // moves, and leave it OUT of `covered` so the cache-info
            // revalidation keeps checking it — then the compile itself reports
            // the unreadable include loudly.
            out.paths.push_back(resolved.Key + "<unreadable>");
            out.hashes.push_back(0);
            continue;
        }
        // First visit wins (mirrors FileIncluder's record dedupe) and
        // terminates include cycles.
        if (!out.covered.insert(resolved.Key).second)
            continue;
        out.paths.push_back(resolved.Key);
        out.hashes.push_back(entry->Hash);
        CollectFromOperands(entry->IncludeOperands, resolved.Candidate.parent_path(), roots, out);
    }
}

} // namespace

void CollectIncludeClosure(const std::string& sourceText,
                           const std::filesystem::path& requestingSource,
                           const std::vector<std::filesystem::path>& searchDirs,
                           IncludeClosure& out)
{
    // The top-level text is supplied by the caller -- it can be in-memory source
    // that exists on no disk -- so it is scanned here rather than read.
    CollectFromOperands(ScanShaderIncludeDirectives(sourceText), requestingSource.parent_path(),
                        searchDirs, out);
}

}} // namespace GameEngine::Rendering
