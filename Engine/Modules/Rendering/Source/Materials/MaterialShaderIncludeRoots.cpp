#include "Rendering/Materials/MaterialShaderIncludeRoots.h"

#include "Rendering/Materials/MaterialBuildContext.h"

#include <algorithm>

namespace GameEngine { namespace Rendering {

namespace {

std::filesystem::path NormalizeRoot(const std::filesystem::path& p)
{
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(p, ec);
    if (ec)
        abs = p;
    return abs.lexically_normal();
}

} // namespace

std::vector<std::filesystem::path> BuildMaterialIncludeRoots(
    const MaterialBuildContext& context, const std::filesystem::path& materialDir)
{
    std::vector<std::filesystem::path> roots = context.IncludeDirs;
    // ResolveShaderReference consults ProjectRoots immediately after materialDir.
    // The compile includer must search the same roots: the composer substitutes
    // the authored relative #include (intern-stable across machines) and shaderc
    // looks that spelling up here. Without these roots it substitutes empty
    // content and EvaluateSurface disappears.
    for (const auto& projectRoot : context.ProjectRoots)
        roots.push_back(projectRoot);
    // Package roots come before the engine tree so a package shader's own
    // root-relative #includes resolve inside the package first — mirroring the
    // composer's reference-resolution order.
    for (const auto& packageDir : context.PackageShaderDirs)
        roots.push_back(packageDir);
    roots.push_back(context.AdapterShaderDir);
    // The inlined adapter text carries `#include "../Includes/x.glsl"`
    // directives written relative to Adapters/, so that directory must be a root
    // in its own right.
    std::error_code ec;
    const auto adaptersDir = context.AdapterShaderDir / "Adapters";
    if (std::filesystem::exists(adaptersDir, ec))
        roots.push_back(adaptersDir);
    const auto includesDir = context.AdapterShaderDir / "Includes";
    if (std::filesystem::exists(includesDir, ec))
        roots.push_back(includesDir);
    // Last, so a material-local file can be NAMED without shadowing any engine
    // or package include that resolves under the same relative spelling.
    if (!materialDir.empty())
        roots.push_back(materialDir);
    return roots;
}

std::string MakeRelocatableIncludeSpelling(const std::filesystem::path& resolved,
                                           const std::vector<std::filesystem::path>& roots,
                                           std::filesystem::path* outShadowedBy)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path target = NormalizeRoot(resolved);

    std::vector<fs::path> normalizedRoots;
    normalizedRoots.reserve(roots.size());
    for (const auto& r : roots)
        normalizedRoots.push_back(NormalizeRoot(r));

    // Spell against the DEEPEST root that contains the file, not the first one
    // in probe order. Probe order is a property of how many roots a host
    // happens to configure — an editor adds the asset root above the shader
    // tree, an offline cook does not — and spelling against it would make the
    // composed source differ between two hosts that resolve the identical file,
    // which breaks the cook/runtime cache-key match the whole cooked-variant
    // path depends on. The deepest ancestor is a property of the file.
    std::vector<fs::path> spellingRoots = normalizedRoots;
    std::stable_sort(spellingRoots.begin(), spellingRoots.end(),
                     [](const fs::path& a, const fs::path& b) {
                         return a.native().size() > b.native().size();
                     });

    for (const fs::path& root : spellingRoots)
    {
        const fs::path rel = target.lexically_relative(root);
        if (rel.empty() || *rel.begin() == "..")
            continue;

        // The includer takes the FIRST root under which the spelling exists, so
        // the spelling is only usable when that probe lands on this same file.
        for (const fs::path& probe : normalizedRoots)
        {
            const fs::path cand = (probe / rel).lexically_normal();
            if (!fs::exists(cand, ec))
                continue;
            if (cand == target)
                return rel.generic_string();
            if (outShadowedBy)
                *outShadowedBy = cand;
            break;
        }
    }

    return target.generic_string();
}

}} // namespace GameEngine::Rendering
