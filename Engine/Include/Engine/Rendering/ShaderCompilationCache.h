#pragma once

// ShaderCompilationCache: two-level shader compilation cache.
//
// Level 1 (global dedup): Materials with the same shader sources and variant
// key share compiled SPIR-V. Keyed on (ShaderVariantKey + surface/vertex paths).
//
// Level 2 (per-material fast-path): Maps (Material*, passKeywords) to the
// resolved SharedShaderVariant. Inner storage is a small vector (2-4 entries)
// since each material only sees a handful of pass keyword combinations.
//
// Owned by RenderServices.

#include "Engine/Rendering/MaterialCompileSpec.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/ShaderMeta.h"
#include "Rendering/Materials/ShaderVariantKey.h"

#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::DescriptorSetLayoutDesc;
using ::GameEngine::Rendering::MaterialBuildContext;
using ::GameEngine::Rendering::MaterialKeyword;
using ::GameEngine::Rendering::ShaderMeta;
using ::GameEngine::Rendering::ShaderVariantKey;
using ::GameEngine::Rendering::VertexAttributeFlags;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine
{
namespace Engine::Renderer
{
class Material;
class ShaderCompileErrorLog;

struct SharedShaderVariant
{
    std::vector<uint8_t> vertexBytes;
    std::vector<uint8_t> fragmentBytes;
    std::shared_ptr<Rendering::ShaderMeta> meta;
    std::vector<Rendering::DescriptorSetLayoutDesc> setLayouts;
};

// Identity of the SHADER SOURCE a compile reads: the authored surface and
// vertex-modifier references, plus the .material file they resolve relative to.
// The asset path is part of the identity because resolution is directory-local
// (ShaderComposer::ResolveShaderReference probes the material's own directory
// first), so the same authored string under two directories names two different
// files.
//
// One type, two uses: it is half of ShaderCacheKey, and it is the key of the
// recorded include closures. That sharing is the point — the invalidation sweep
// and the affected-material scan must agree on what "the same shader source"
// means, and one type makes disagreement impossible rather than merely unlikely.
struct ShaderSourceKey
{
    std::string SurfaceShaderPath;
    std::string VertexModifierPath;
    // Empty for materials with no backing asset (graph previews, headless
    // registration, primitives). Those have no material dir, so they all share
    // one key — an OVER-match, not an exact one: with the material-dir probe
    // skipped, an authored reference resolves through context.ProjectRoots first
    // (ShaderComposer::ResolveShaderReference), so the file behind the shared key
    // moves when the project does. As a closure row that costs a redundant
    // recompile; as a cache entry it is why Clear() runs on a project switch.
    std::filesystem::path MaterialAssetPath;

    bool operator==(const ShaderSourceKey& other) const
    {
        return SurfaceShaderPath == other.SurfaceShaderPath
            && VertexModifierPath == other.VertexModifierPath
            && MaterialAssetPath == other.MaterialAssetPath;
    }
};

// This key is the IN-MEMORY identity only, and is literally a variant key plus a
// source identity. The on-disk shaderpkg key (v6, ShaderCompileService) hashes
// the true compile inputs — composed source, inlined adapters, and the CONTENT
// of the whole #include closure — so donor-seeding `.Cache/Shaders` between
// builds is safe and the historical hand-wipe discipline ("adapter cache-key
// gap") is retired. What content edits must still invalidate is THIS cache:
// entries here are keyed by paths, not bytes, so InvalidateEntriesForShaderFile
// matches the edited file against each entry's surface/vertex references AND
// against the recorded include closure of its source identity (see
// m_SurfaceClosures).
struct ShaderCacheKey
{
    Rendering::ShaderVariantKey VariantKey;
    ShaderSourceKey Source;

    bool operator==(const ShaderCacheKey& other) const
    {
        return VariantKey == other.VariantKey && Source == other.Source;
    }
};

// What one shader-file edit reaches: the source identities that depend on the
// edited file, and how many compiled entries the sweep dropped.
//
// The identities are derived from the recorded closure ROWS, never from the
// entries that died. That distinction is the contract. Rows outlive entries on
// purpose, so a material whose entry an earlier save of the same file already
// dropped — and whose recompile is still waiting its turn in the per-frame
// budget — is still named here. Deriving the list from the dropped entries
// would silently drop that material from the queue and leave it drawing its
// pre-edit pipeline until something else disturbed it.
struct ShaderFileDependents
{
    std::vector<ShaderSourceKey> Identities;
    size_t DroppedEntries = 0;
};

} // namespace Engine::Renderer
} // namespace GameEngine

template <>
struct std::hash<GameEngine::Engine::Renderer::ShaderSourceKey>
{
    size_t operator()(const GameEngine::Engine::Renderer::ShaderSourceKey& k) const noexcept
    {
        size_t h = std::hash<std::string>{}(k.SurfaceShaderPath);
        h ^= std::hash<std::string>{}(k.VertexModifierPath) + 0x9e3779b9 + (h << 6) + (h >> 2);
        // Normalized for hashing while operator== compares the raw path: equal
        // keys still hash equal, and two spellings of one path merely collide.
        h ^= std::hash<std::string>{}(k.MaterialAssetPath.lexically_normal().string()) + 0x9e3779b9
             + (h << 6) + (h >> 2);
        return h;
    }
};

template <>
struct std::hash<GameEngine::Engine::Renderer::ShaderCacheKey>
{
    size_t operator()(const GameEngine::Engine::Renderer::ShaderCacheKey& k) const noexcept
    {
        size_t h = static_cast<size_t>(k.VariantKey.Hash());
        h ^= std::hash<GameEngine::Engine::Renderer::ShaderSourceKey>{}(k.Source) + 0x9e3779b9
             + (h << 6) + (h >> 2);
        return h;
    }
};

namespace GameEngine
{
namespace Engine::Renderer
{

class ShaderCompilationCache
{
  public:
    // --- Global dedup cache ---

    // Look up or compile a shader variant. Returns nullptr on compilation failure.
    // On cache hit, returns the shared variant immediately (no compilation).
    // On miss, compiles via BuildMaterialToShaderPackage and caches the result.
    // `kind` is the shader form the consuming device ingests
    // (IDevice::PreferredShaderSource); it decides what the variant's stage
    // bytes carry, so a cached variant is only valid for that device.
    std::shared_ptr<SharedShaderVariant> GetOrCompile(
        const ShaderCacheKey& key,
        const MaterialCompileSpec& spec,
        const Rendering::MaterialBuildContext& buildContext,
        const std::string& debugName,
        Rendering::ShaderSourceKind kind);

    // --- Per-material variant resolution ---

    // Resolve a variant for a specific material + pass keyword combination.
    // On per-material cache hit, returns immediately. On miss, builds a
    // ShaderCacheKey from the material's compile spec and variant key, checks
    // the global dedup cache (or compiles), then stores the result.
    // Takes const Material& -- the mutable variant map lives here, not on Material.
    std::shared_ptr<SharedShaderVariant> GetOrCompileVariant(
        const Material& material,
        Rendering::MaterialKeyword passKeywords,
        const Rendering::MaterialBuildContext& buildContext,
        Rendering::ShaderSourceKind kind,
        Rendering::VertexAttributeFlags meshVertexFlags = Rendering::VertexAttributeFlags::None);

    // Clear all per-material variant entries (called on pipeline rebuild).
    void ClearAllMaterialVariants();

    // Clear per-material entries for a specific material. Must be called
    // before the Material pointer is freed (during MaterialRegistry::Unregister).
    void ClearVariantsForMaterial(const Material* material);

    // Drop a global dedup entry so the next GetOrCompile re-reads surface sources
    // from disk. Needed when shader file content changes but paths stay the same.
    void InvalidateGlobalEntry(const ShaderCacheKey& key);

    // Drop every global entry that DEPENDS on the given shader file, and name
    // every source identity that does — one pass, one lock acquisition.
    //
    // Matching is by filename, case-insensitive: authored references are
    // relative strings while edit notifications carry absolute paths, so the
    // filename is the only stable common key. An entry matches on its own
    // surface / vertex-modifier reference OR on the recorded include closure of
    // its source identity, so an edit to an adapter, an engine include, or a
    // transitively included helper invalidates every rider and not just direct
    // references.
    //
    // The returned identities are what drives MaterialSystem's affected-material
    // scan, and this is the ONLY closure matcher in the engine. A second one on
    // the caller's side would be free to answer differently as rows and entries
    // drift apart; there is nothing to keep two in agreement but discipline.
    //
    // Callers must build their probe identities from the SAME strings the
    // ShaderCacheKey carried when the pair was built. Both sides derive from one
    // reconciled MaterialDocument today (RegisterMaterialFromDocument reconciles,
    // then hands `resolved` to both the registry — which stores the compile spec
    // — and CompileMaterialPipeline, which builds the key), so the spec and the
    // key agree. Probing with an unreconciled spelling would silently match
    // nothing and the include-edit trigger would go dead, with nothing failing
    // loudly.
    ShaderFileDependents InvalidateEntriesForShaderFile(const std::string& fileName);

    // Test seam for the resurrection race: invoked on the compiling thread after
    // the single-flight election (so the publish's epoch snapshot is already
    // taken) and before the shaderc work, with no lock held. A test uses it to
    // run a real invalidation strictly inside the publish window — the one
    // interleaving no external thread can hit deterministically. Null in
    // production; set before any compile, cleared after.
    void SetCompileEnteredHookForTesting(std::function<void()> hook)
    {
        std::lock_guard lock(m_Mutex);
        m_CompileEnteredHook = std::move(hook);
    }

    // --- Diagnostics ---

    // Every compile outcome is reported here (failure upserts, success clears)
    // so the editor can surface broken shaders without an inspector selection.
    // Owned by MaterialSystem; set once at Initialize. Null = no reporting.
    void SetErrorLog(ShaderCompileErrorLog* log) { m_ErrorLog = log; }

    size_t GetEntryCount() const
    {
        std::lock_guard lock(m_Mutex);
        return m_Cache.size();
    }
    size_t GetMaterialVariantCount() const
    {
        std::lock_guard lock(m_Mutex);
        return m_MaterialVariants.size();
    }
    bool HasVariantsForMaterial(const Material* material) const
    {
        std::lock_guard lock(m_Mutex);
        return m_MaterialVariants.find(material) != m_MaterialVariants.end();
    }

    // Drop the compiled entries, the per-material memos and the in-flight
    // election: shutdown, and project switch. Entries are keyed by path rather
    // than content, so an identity with an EMPTY material asset path (primitives,
    // graph previews, headless registrations) is identical in both projects while
    // the roots its references resolve against have just moved — a retained entry
    // serves the old project's SPIR-V for a reference that now names a different
    // file.
    //
    // Closure ROWS survive this call. See m_SurfaceClosures.
    void Clear();

  private:
    struct VariantCacheKey
    {
        Rendering::MaterialKeyword passKeywords;
        Rendering::VertexAttributeFlags vertexFlags;
        bool operator==(const VariantCacheKey& o) const
        {
            return passKeywords == o.passKeywords && vertexFlags == o.vertexFlags;
        }
    };
    using VariantEntry = std::pair<VariantCacheKey, std::shared_ptr<SharedShaderVariant>>;

    // Global dedup cache: ShaderCacheKey -> compiled variant.
    std::unordered_map<ShaderCacheKey, std::shared_ptr<SharedShaderVariant>> m_Cache;

    // Single-flight: per-key in-flight compile future so same-key racers WAIT for
    // the first compiler instead of re-compiling (A2.4-P0-R: the depth prepass, 4
    // cascades and color pass can cold-compile the same material concurrently on
    // workers — that both wasted N× the shaderc work AND collided on the material
    // build service's shared intermediate file, poisoning the cache on a torn read).
    //
    // The invalidation epoch does NOT cancel an election: InvalidateEntries-
    // ForShaderFile bumps the epoch and leaves m_InFlight alone, so a same-key
    // request arriving after an edit still waits on the pre-edit compile and
    // receives its pre-edit bytes. It is not cached (publish's epoch guard skips
    // the insert), so the waiter's NEXT lookup misses and recompiles. Gating the
    // publish-side erase on the epoch would leak the entry instead and pin every
    // later request to that stale future forever.
    using CompileFuture = std::shared_future<std::shared_ptr<SharedShaderVariant>>;
    std::unordered_map<ShaderCacheKey, CompileFuture> m_InFlight;

    // Per-material fast-path cache. Inner vector holds 2-4 entries — linear
    // scan beats hash lookup for this size, fits in 1-2 cache lines.
    std::unordered_map<const Material*, std::vector<VariantEntry>> m_MaterialVariants;

    // Recorded include closures, one row per ShaderSourceKey. Values are
    // LOWERCASED FILENAMES of every file any build of that identity read:
    // adapters, the surface itself, the vertex modifier, and all transitive
    // includes.
    //
    // The row key carries the material asset path, so two materials in different
    // directories authoring the same surface FILENAME get separate rows and an
    // edit to one's helper cannot drag the other into a recompile. The cost is a
    // row per material rather than per surface — order 100 materials × 20-30
    // filenames in a project of that size. Every keyword variant of one identity
    // still shares a row; they are builds of the same source, and the row is the
    // union of what any of them read.
    //
    // Rows are a UNION and are never narrowed by a build. An include removed
    // from a surface therefore keeps triggering until the session ends, costing
    // a redundant recompile the content-keyed disk cache absorbs; the opposite
    // error — a row that forgot a file — is a missed invalidation, i.e. the
    // stale serve this whole mechanism exists to prevent.
    //
    // Rows also persist across invalidation sweeps on purpose. The
    // affected-material scan runs off the identities
    // InvalidateEntriesForShaderFile returns, and those must keep naming a
    // material while its entries are dropped and its recompile is still
    // carried across budgeted frames.
    //
    // They persist across Clear() for the same reason, one scope wider. A project
    // switch does not tear down the material registry, so every material
    // registered before it survives holding a live pipeline, and re-registration
    // recompiles only when pipeline-affecting state changed — which for a
    // primitive or the default material it has not. Without its row such a
    // material is reachable only by the direct filename check, which by
    // construction cannot fire for an adapter, an engine include or a transitive
    // helper: its include-edit trigger is dead for the rest of the session. The
    // retention is safe in both directions — a row keyed on the old project's
    // material asset path matches nothing the new project can probe with, and a
    // row keyed on an EMPTY asset path is shared with the new project's own
    // empty-path identities, whose references CAN resolve to different files
    // (ProjectRoots precede AdapterShaderDir), so it over-matches at worst and
    // costs a redundant recompile.
    std::unordered_map<ShaderSourceKey, std::unordered_set<std::string>> m_SurfaceClosures;

    // Reduce a build's dependency record to lowercased filenames. Runs OUTSIDE
    // m_Mutex: path parsing and case folding are the expensive half, and the
    // mutex covers only the cache check and insert.
    static std::vector<std::string> ToLowercaseFilenames(const std::vector<std::string>& paths);

    // Union already-reduced filenames into the closure row for `key`'s source
    // identity. Caller must hold m_Mutex.
    void RecordSurfaceClosureLocked(const ShaderCacheKey& key,
                                    std::vector<std::string>&& closureFilenames);

    // Guards both caches. Recursive because GetOrCompileVariant calls
    // GetOrCompile internally; the latter must re-acquire on the same thread.
    // The slow shaderc work runs OUTSIDE the lock — the mutex only covers
    // the cache check + insert.
    mutable std::recursive_mutex m_Mutex;

    // Invalidation epoch, bumped by InvalidateEntriesForShaderFile. GetOrCompile
    // snapshots it under its first lock and publish() skips the m_Cache insert
    // when it has moved; GetOrCompileVariant mirrors the guard on the memo
    // insert. Without it, a compile already in flight when an edit notification
    // lands republishes its PRE-EDIT SPIR-V after the invalidation swept the
    // cache, leaving half-new/half-old variants on screen until the next save —
    // and rapid save bursts (the core authoring workload) are exactly when
    // in-flight compiles overlap edits.
    //
    // One global counter, not per-file: an edit therefore also suppresses
    // in-flight compiles of UNRELATED shaders. That costs at most one redundant
    // recompile of the handful in flight at that instant, and never a stale
    // pixel. Clear() advances it for the same reason the sweep does. Never
    // RESET — a wrapping-free monotonic counter is what makes a stale snapshot
    // detectable, and a counter that returned to a previous value would let a
    // pre-clear snapshot compare equal again.
    uint64_t m_InvalidationEpoch = 0;

    // See SetCompileEnteredHookForTesting.
    std::function<void()> m_CompileEnteredHook;

    // Not guarded by m_Mutex: the log is internally synchronized and set once
    // before any compile runs.
    ShaderCompileErrorLog* m_ErrorLog = nullptr;
};

} // namespace Engine::Renderer
} // namespace GameEngine
