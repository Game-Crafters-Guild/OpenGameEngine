#include "Engine/Rendering/ShaderCompilationCache.h"

#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialShaderPackageBuilder.h"
#include "Engine/Rendering/ShaderCompileErrorLog.h"
#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/MaterialKeywordDerivation.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <algorithm>
#include <cassert>
#include <cctype>

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace ::GameEngine::Rendering;

std::shared_ptr<SharedShaderVariant> ShaderCompilationCache::GetOrCompile(
    const ShaderCacheKey& key,
    const MaterialCompileSpec& spec,
    const Rendering::MaterialBuildContext& buildContext,
    const std::string& debugName,
    Rendering::ShaderSourceKind kind)
{
    // The build context may not be ready yet during early startup: the editor
    // 'editor' asset source that resolves AdapterShaderDir can be registered
    // after the first per-frame prewarm pass runs. Treat an empty AdapterShaderDir
    // as a transient not-ready state — return null WITHOUT caching, so the variant
    // recompiles once the context is populated. Caching null here would permanently
    // poison the cache and break the material for the whole session (the symptom:
    // a mesh casts shadows but is invisible in the color pass, because the shared
    // depth shader still works while its color variant stays dead). Checked before
    // single-flight registration so a not-ready request never blocks other keys.
    const bool contextReady = !buildContext.AdapterShaderDir.empty();

    // Single-flight election: consult the completed cache, then the in-flight map.
    // Exactly one thread per key becomes the compiler (holds `myPromise`); the rest
    // wait on the shared future. This kills both the N× redundant compile AND the
    // MaterialBuildService shared-intermediate-file race under parallel record.
    std::shared_ptr<std::promise<std::shared_ptr<SharedShaderVariant>>> myPromise;
    CompileFuture waitFuture;
    uint64_t epochAtStart = 0;
    std::function<void()> compileEnteredHook;
    {
        std::lock_guard lock(m_Mutex);
        auto it = m_Cache.find(key);
        if (it != m_Cache.end())
            return it->second;
        if (!contextReady)
            return nullptr; // transient — do NOT cache or register in-flight
        // Snapshot the invalidation epoch alongside the election: publish()
        // compares it to decide whether this compile's result is still current.
        epochAtStart = m_InvalidationEpoch;
        if (auto inflight = m_InFlight.find(key); inflight != m_InFlight.end())
        {
            waitFuture = inflight->second;
        }
        else
        {
            myPromise = std::make_shared<std::promise<std::shared_ptr<SharedShaderVariant>>>();
            m_InFlight.emplace(key, myPromise->get_future().share());
            compileEnteredHook = m_CompileEnteredHook;
        }
    }
    if (!myPromise)
        return waitFuture.get(); // another thread is compiling this key — wait for it

    if (compileEnteredHook)
        compileEnteredHook();

    // Sole compiler for this key. Compile OUTSIDE the lock (shaderc + I/O is the
    // expensive part). Every exit path funnels through `publish` so the in-flight
    // entry is always cleared and every waiter is always released.
    std::shared_ptr<SharedShaderVariant> result;
    // Lowercased filenames of the build's dependency record, reduced BEFORE the
    // lock: path parsing and case folding must not run inside m_Mutex, which
    // covers only the cache check and insert.
    std::vector<std::string> closureFilenames;
    auto publish = [&]() {
        {
            std::lock_guard lock(m_Mutex);
            // Resurrection guard: an invalidation landed while this compile ran,
            // so the SPIR-V just produced may predate the edit that swept the
            // cache. Skip the insert — the next request misses and recompiles
            // against the edited source. Waiters are still released with this
            // result (the same pre-edit variant they were already blocked on),
            // and their own next lookup misses too, so nobody is wedged.
            if (m_InvalidationEpoch == epochAtStart)
                m_Cache[key] = result;
            m_InFlight.erase(key);
            // Record what this build READ so InvalidateEntriesForShaderFile can
            // match include/adapter edits. Deliberately NOT epoch-gated the way
            // the SPIR-V insert is: the affected-material scan runs off these
            // rows and must keep naming this identity while its entries are
            // dropped and its recompile is carried. Never a replace either —
            // every keyword variant of this identity records into the one row,
            // so replacing would drop whatever the others read (see
            // m_SurfaceClosures).
            if (!closureFilenames.empty())
                RecordSurfaceClosureLocked(key, std::move(closureFilenames));
        }
        myPromise->set_value(result);
        return result;
    };

    MaterialDocument doc{};
    doc.surfaceShader = spec.surfaceShaderPath;
    doc.lightingModel = spec.lightingModel;
    doc.vertexModifier = spec.vertexModifierPath;
    doc.customVertexShader = spec.customVertexShader;
    // Keyword strings reconstitute the GE_USER_ defines on the cache-driven path;
    // without them a cache-miss recompile would hash-as-keyworded but compile
    // define-less.
    doc.keywords = spec.userKeywords;
    if (spec.alphaTest)
        doc.alphaMode = MaterialAlphaMode::Mask;
    // The relief march is derived from a bound height slot on a surface that declares one, so the
    // derived decision reaches the build as that binding. The build reads only that the slot is
    // bound, never the reference, so the value names what it stands for.
    assert(spec.parallax ==
               Rendering::HasKeyword(key.VariantKey.materialKeywords, Rendering::MaterialKeyword::Parallax) &&
           "the compile spec's parallax decision and the key's Parallax bit disagree: derive both where the "
           "registration key is derived");
    if (spec.parallax)
        doc.textures[Rendering::kParallaxHeightMapSlot] = "<height binding carried by the compile spec>";

    // When the key carries no material asset path, the build still needs a
    // non-empty stand-in path (generated-package naming, debug names). Anchor
    // it under the CACHE root — a directory that contains no shaders — so its
    // parent can never masquerade as a materialDir and shadow the
    // project/package/engine resolution chain. (Anchoring it under
    // AdapterShaderDir made engine surfaces resolve "through the material dir"
    // while PROJECT-side surfaces silently missed — the copied-project-root
    // portability bug.) ShaderComposer::ResolveShaderReference treats the
    // stand-in's nonexistent parent as a miss and walks the real roots.
    namespace fs = std::filesystem;
    const fs::path syntheticPath =
        buildContext.CacheRoot / "Synthetic" /
        ("cache_" + std::to_string(key.VariantKey.Hash()) + ".material");
    const fs::path buildMaterialPath =
        key.Source.MaterialAssetPath.empty() ? syntheticPath : key.Source.MaterialAssetPath;

    auto reportFailure = [&](const std::vector<std::string>& errors)
    {
        // A variant already in flight when its material file was renamed or
        // deleted fails against a path that no longer exists. That failure
        // describes an identity nothing can recompile — reporting it would
        // plant an error row no later success can ever clear.
        if (!key.Source.MaterialAssetPath.empty())
        {
            std::error_code ec;
            if (!fs::exists(key.Source.MaterialAssetPath, ec))
            {
                Logger::Log::Info(
                    "ShaderCompilationCache '{}': material file vanished mid-build "
                    "(renamed or deleted); dropping its compile failure", debugName);
                return;
            }
        }
        for (const auto& err : errors)
            Logger::Log::Warning("ShaderCompilationCache '{}': {}", debugName, err);
        if (m_ErrorLog)
        {
            ShaderCompileErrorLog::Entry entry;
            entry.MaterialName = debugName;
            entry.SurfaceShaderPath = spec.surfaceShaderPath;
            entry.MaterialAssetPath = key.Source.MaterialAssetPath;
            entry.Errors = errors;
            m_ErrorLog->ReportFailure(std::move(entry));
        }
    };

    Rendering::MaterialBuildContext ctx = buildContext;
    MaterialShaderPackageBuilder builder(std::move(doc), buildMaterialPath, debugName, ctx);
    Rendering::MaterialBuildResult buildResult = builder.Build(
        kind, key.VariantKey.materialKeywords, key.VariantKey.vertexFlags);

    // Populated even when the build failed (attempted dependencies) — publish
    // records it either way. Empty when composition itself failed.
    closureFilenames = ToLowercaseFilenames(buildResult.includeClosurePaths);

    if (!buildResult.success || !buildResult.package)
    {
        reportFailure(buildResult.errors);
        return publish(); // result stays null
    }

    if (m_ErrorLog)
        m_ErrorLog->ReportSuccess(debugName, spec.surfaceShaderPath);

    const auto& pkg = *buildResult.package;
    auto variant = std::make_shared<SharedShaderVariant>();

    auto itVs = pkg.stageBytes.find("vs");
    auto itFs = pkg.stageBytes.find("fs");
    if (itVs != pkg.stageBytes.end())
        variant->vertexBytes = itVs->second;
    if (itFs != pkg.stageBytes.end())
        variant->fragmentBytes = itFs->second;

    variant->meta = std::make_shared<Rendering::ShaderMeta>(pkg.meta);
    variant->setLayouts = Rendering::MaterialBuilder::BuildSetLayouts(pkg.meta);

    result = variant;
    return publish();
}

std::shared_ptr<SharedShaderVariant> ShaderCompilationCache::GetOrCompileVariant(
    const Material& material,
    Rendering::MaterialKeyword passKeywords,
    const Rendering::MaterialBuildContext& buildContext,
    Rendering::ShaderSourceKind kind,
    Rendering::VertexAttributeFlags meshVertexFlags)
{
    VariantCacheKey lookupKey{passKeywords, meshVertexFlags};

    uint64_t epochAtStart = 0;
    {
        std::lock_guard lock(m_Mutex);
        epochAtStart = m_InvalidationEpoch;
        auto outerIt = m_MaterialVariants.find(&material);
        if (outerIt != m_MaterialVariants.end())
        {
            for (auto& [key, variant] : outerIt->second)
            {
                if (key == lookupKey)
                    return variant;
            }
        }
    }

    Rendering::ShaderVariantKey mergedKey = material.GetVariantKey();
    mergedKey.materialKeywords |= passKeywords;
    if (meshVertexFlags != Rendering::VertexAttributeFlags::None)
        mergedKey.vertexFlags = meshVertexFlags;
    // Fully-procedural materials ignore mesh vertex flags entirely — without
    // this the cache fragments into per-mesh-layout keys that all compile to
    // the same clamped SPIR-V.
    if (material.GetCompileSpec().customVertexShader)
        Rendering::ApplyCustomVertexShaderClamp(mergedKey);

    ShaderCacheKey cacheKey{};
    cacheKey.VariantKey = mergedKey;
    cacheKey.Source.SurfaceShaderPath = material.GetCompileSpec().surfaceShaderPath;
    cacheKey.Source.VertexModifierPath = material.GetCompileSpec().vertexModifierPath;
    cacheKey.Source.MaterialAssetPath = material.GetMaterialAssetPath();

    auto variant =
        GetOrCompile(cacheKey, material.GetCompileSpec(), buildContext, material.GetName(), kind);

    // Don't memoize a transient not-ready result. A null with an empty
    // AdapterShaderDir is the early-startup case (GetOrCompile returned null
    // without caching); memoizing it here would defeat the retry and leave the
    // material permanently dead once the context becomes ready. A null with a
    // populated context is a genuine compile failure — keep memoizing it so we
    // don't retry the same broken build every frame.
    if (variant || !buildContext.AdapterShaderDir.empty())
    {
        std::lock_guard lock(m_Mutex);
        // Mirror of the publish-side resurrection guard: the memo is a fast path
        // ONTO the global entry, so memoizing a result whose compile started
        // before an invalidation would keep serving pre-edit SPIR-V to this
        // material even though m_Cache correctly dropped it.
        if (m_InvalidationEpoch == epochAtStart)
            m_MaterialVariants[&material].push_back({lookupKey, variant});
    }
    return variant;
}

void ShaderCompilationCache::Clear()
{
    std::lock_guard lock(m_Mutex);
    m_Cache.clear();
    m_MaterialVariants.clear();
    // Drop in-flight election so a post-clear request for a still-compiling key
    // starts a fresh compile rather than waiting on the pre-clear one. The old
    // compiler still releases its (now-orphaned) waiters with the pre-clear
    // result. Its publish erases m_InFlight by KEY without checking the entry is
    // still its own, so a post-clear compiler that re-registered that key gets
    // de-registered by it and the next request elects a third compile — bounded
    // to one extra cold compile per key in flight here. Gating that erase on the
    // epoch is NOT the repair: it leaks the entry and pins every later request to
    // the pre-clear future (see m_InFlight).
    m_InFlight.clear();
    // m_SurfaceClosures is deliberately NOT cleared: every material that outlives
    // this call keeps its live pipeline and is never recompiled, so dropping its
    // row strands it. See the declaration for the full argument.
    //
    // Bar the same resurrection the sweep bars: a compile that ELECTED before
    // this call must not reinstate its entry or its per-material memo
    // afterwards. A compile that only QUEUED before it and elects after
    // snapshots this new value, compares equal, and does publish — see
    // MaterialSystem::OnProjectSwitched for the residual that leaves.
    ++m_InvalidationEpoch;
}

void ShaderCompilationCache::ClearAllMaterialVariants()
{
    std::lock_guard lock(m_Mutex);
    m_MaterialVariants.clear();
}

void ShaderCompilationCache::ClearVariantsForMaterial(const Material* material)
{
    std::lock_guard lock(m_Mutex);
    m_MaterialVariants.erase(material);
}

void ShaderCompilationCache::InvalidateGlobalEntry(const ShaderCacheKey& key)
{
    std::lock_guard lock(m_Mutex);
    // Bump regardless of whether the entry is resident: same discipline as
    // InvalidateEntriesForShaderFile — a compile currently in flight for this
    // key is NOT in m_Cache yet, and a zero-drop invalidation is precisely the
    // case the resurrection guard exists for. Without the bump, that compile's
    // publish() re-inserts SPIR-V built from the pre-edit source.
    ++m_InvalidationEpoch;
    const auto it = m_Cache.find(key);
    if (it == m_Cache.end())
        return;
    // Drop every per-material memo that resolved to this entry: the memo is a
    // fast path onto the global entry, and a memo that outlives its entry keeps
    // serving stale SPIR-V to EVERY material sharing the shader — not just the
    // one the caller is about to recompile.
    // dying may be null (a memoized compile failure) — the sweep below then
    // drops every null memo, which is exactly right: invalidation means
    // "something changed, recompile", and that includes retrying failures.
    const std::shared_ptr<SharedShaderVariant> dying = it->second;
    m_Cache.erase(it);
    for (auto& [material, variants] : m_MaterialVariants)
    {
        std::erase_if(variants,
                      [&dying](const VariantEntry& e) { return e.second == dying; });
    }
}

ShaderFileDependents ShaderCompilationCache::InvalidateEntriesForShaderFile(
    const std::string& fileName)
{
    ShaderFileDependents dependents;
    if (fileName.empty())
        return dependents;
    std::string wanted = fileName;
    std::transform(wanted.begin(), wanted.end(), wanted.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    auto references = [&wanted](const std::string& authoredPath)
    {
        if (authoredPath.empty())
            return false;
        std::string fn = std::filesystem::path(authoredPath).filename().string();
        std::transform(fn.begin(), fn.end(), fn.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return fn == wanted;
    };

    std::lock_guard lock(m_Mutex);

    // Name the dependent source identities FIRST, and from the ROWS. The edited
    // file may appear in no entry's key at all — an adapter, an engine include,
    // a helper the surface pulls in transitively — and the row is the only thing
    // that knows about it. A row also matches on its own authored surface /
    // vertex-modifier filename, so this is a complete answer to "which sources
    // depend on this file", not merely "which of them have an include record for
    // it".
    //
    // Rows, not dropped entries: an identity whose entries an earlier save of
    // this same file already dropped still depends on the file, and its
    // recompile may still be sitting in the caller's carried budget.
    std::unordered_set<ShaderSourceKey> matched;
    for (const auto& [source, closure] : m_SurfaceClosures)
    {
        if (closure.count(wanted) != 0 || references(source.SurfaceShaderPath)
            || references(source.VertexModifierPath))
            matched.insert(source);
    }

    // Bump the epoch before the sweep and regardless of what the sweep finds:
    // the entry a compile currently in flight is about to publish is NOT in
    // m_Cache yet, so a zero-drop invalidation is precisely the case the
    // resurrection guard exists for. publish() and the memo insert both compare
    // against the snapshot they took, so any compile that started before this
    // point is now barred from re-populating either cache. It does not cancel the
    // in-flight ELECTION — see the m_InFlight declaration for why not.
    ++m_InvalidationEpoch;
    // Same discipline as InvalidateGlobalEntry: every per-material memo that
    // resolved to a dying entry must go with it, or it keeps serving stale
    // SPIR-V to every material sharing the shader. A dying null (memoized
    // compile failure) sweeps ALL null memos — over-broad but correct:
    // invalidation means "something changed, retry", and that includes retrying
    // failures.
    std::vector<std::shared_ptr<SharedShaderVariant>> dying;
    for (auto it = m_Cache.begin(); it != m_Cache.end();)
    {
        // A probe into the identity set the rows just produced — the same
        // identities the caller will requeue on. The direct reference checks
        // stay because an entry whose build failed before recording any
        // dependency has no row to be named by.
        if (references(it->first.Source.SurfaceShaderPath)
            || references(it->first.Source.VertexModifierPath)
            || matched.count(it->first.Source) != 0)
        {
            dying.push_back(it->second);
            it = m_Cache.erase(it);
        }
        else
        {
            ++it;
        }
    }

    dependents.DroppedEntries = dying.size();
    dependents.Identities.reserve(matched.size());
    while (!matched.empty())
        dependents.Identities.push_back(std::move(matched.extract(matched.begin()).value()));

    if (dying.empty())
        return dependents;
    for (auto& [material, variants] : m_MaterialVariants)
    {
        std::erase_if(variants,
                      [&dying](const VariantEntry& e)
                      { return std::find(dying.begin(), dying.end(), e.second) != dying.end(); });
    }
    return dependents;
}

std::vector<std::string> ShaderCompilationCache::ToLowercaseFilenames(
    const std::vector<std::string>& paths)
{
    std::vector<std::string> names;
    names.reserve(paths.size());
    for (const std::string& path : paths)
    {
        std::string fn = std::filesystem::path(path).filename().string();
        if (fn.empty())
            continue;
        std::transform(fn.begin(), fn.end(), fn.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        names.push_back(std::move(fn));
    }
    return names;
}

void ShaderCompilationCache::RecordSurfaceClosureLocked(const ShaderCacheKey& key,
                                                        std::vector<std::string>&& closureFilenames)
{
    auto& row = m_SurfaceClosures[key.Source];
    for (std::string& fn : closureFilenames)
        row.insert(std::move(fn));
}

} // namespace Engine::Renderer
} // namespace GameEngine
