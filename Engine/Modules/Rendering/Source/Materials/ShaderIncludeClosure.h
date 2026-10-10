#pragma once

// Textual #include closure of a shader source, and the content hashing the
// compile cache key is built from. Independent of any shader compiler: the key
// a runtime uses to ADDRESS a cached program must be computable on a runtime
// that cannot compile one.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace GameEngine { namespace Rendering {

inline constexpr uint64_t kShaderHashOffsetBasis = 1469598103934665603ull;

// FNV-1a over raw bytes. Seeded so callers can fold many inputs into one key.
uint64_t HashShaderBytes(const void* data, size_t size, uint64_t seed = kShaderHashOffsetBasis);

// Fold a string plus a delimiter, so "ab"+"c" and "a"+"bc" hash differently.
uint64_t HashShaderString(uint64_t seed, const std::string& s);

bool ReadShaderFileText(const std::filesystem::path& p, std::string& out, std::string* outError);

// Operands of each `#include "x"` / `#include <x>` directive, in source order.
std::vector<std::string> ScanShaderIncludeDirectives(const std::string& src);

// Content of a source's textual #include closure, folded into the cache key.
//
// Keying only the TOP-LEVEL source bytes is not enough: a material's composed
// fragment reaches its surface shader through a substituted literal
// `#include "<surface>"` (ShaderComposer), and every engine adapter reaches
// standard_pbr/clustered_lighting/shadow_sampling/... the same way. Folding the
// closure CONTENT is what makes an mtime-preserving, same-length edit of any of
// them produce a different key instead of leaning on a staleness check.
struct IncludeClosure
{
    // Resolved absolute paths in scan order, paired with their content hashes.
    // Order is deterministic (depth-first over the source text), so the folded
    // key is stable for stable content. The paths are DIAGNOSTIC and dependency-
    // tracking data only — they are machine-specific and never enter the key.
    std::vector<std::string> paths;
    std::vector<uint64_t> hashes;
    // Same spelling as ShaderCacheInfo::Include::path, so a cache-hit
    // revalidation can tell which recorded includes the key already covers.
    std::unordered_set<std::string> covered;
};

// Walk the #include closure of `sourceText`, resolving each directive with the
// SAME search order the compiler's includer uses: the requesting source's own
// directory first, then `searchDirs` in order, taking the first path that
// exists. KEEP IN SYNC WITH FileIncluder — a scan that resolves a directive to a
// different file than the compiler does would key content the compile never read.
//
// Conservative by construction: preprocessor conditionals are NOT evaluated, so
// the closure is a SUPERSET of what the compiler actually pulls in for a given
// variant. Keying a file the variant does not compile costs at most one extra
// recompile when that file changes; it can never serve stale SPIR-V.
void CollectIncludeClosure(const std::string& sourceText,
                           const std::filesystem::path& requestingSource,
                           const std::vector<std::filesystem::path>& searchDirs,
                           IncludeClosure& out);

}} // namespace GameEngine::Rendering
