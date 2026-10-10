#pragma once

// Include search roots for a material's composed shader, and the portable
// spellings the composer substitutes into it.
//
// Single authority for both sides of one contract: the composer spells its
// substituted `#include` literals relative to these roots, and the compile
// request hands the same list to the compiler's includer. A spelling the
// composer emits is therefore a spelling the compile resolves — and, because it
// names no absolute location, the composed source is byte-identical wherever the
// content tree lands, which is what lets a cooked shader cache relocate.

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine { namespace Rendering {

struct MaterialBuildContext;

// Roots in the order the includer probes them: extra IncludeDirs, then
// ProjectRoots, PackageShaderDirs, the engine adapter tree, and finally
// `materialDir` (empty = no backing asset, contributes no root).
std::vector<std::filesystem::path> BuildMaterialIncludeRoots(
    const MaterialBuildContext& context, const std::filesystem::path& materialDir);

// Spell `resolved` relative to the DEEPEST root that contains it and still wins
// the includer's probe for that spelling, so the literal names a file rather
// than a location — and, because the deepest ancestor is a property of the file
// rather than of the root list, the same file gets the same spelling from hosts
// that configure different numbers of roots (an editor mounts the asset root
// above the shader tree; an offline cook does not). That equality is what lets a
// cooked variant address the same cache key the runtime asks for.
// Falls back to the absolute spelling — correct, but pinned to this machine —
// when no root can name it unambiguously, which happens only when a root earlier
// in probe order shadows the resolved file under the same relative spelling.
// `outShadowedBy`, when the fallback is taken, receives the shadowing file so
// the caller can name both sides.
std::string MakeRelocatableIncludeSpelling(const std::filesystem::path& resolved,
                                           const std::vector<std::filesystem::path>& roots,
                                           std::filesystem::path* outShadowedBy = nullptr);

}} // namespace GameEngine::Rendering
