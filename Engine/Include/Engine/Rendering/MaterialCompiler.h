#pragma once

#include "AssetCore/GUID.h"
#include "Rendering/Materials/ShaderMeta.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "Rendering/Materials/MaterialBuildService.h"

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
using ::GameEngine::Rendering::ShaderMeta;
using ::GameEngine::Rendering::ShaderPackage;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine
{
class AssetManager;
class MaterialAsset;
struct MaterialDocument;

namespace ShaderGraph
{
struct SgGraphProperty;
}

namespace Engine::Renderer
{
class RenderServices;
}

namespace Rendering
{
struct ShaderPackage;
} // namespace Rendering

namespace Engine::Renderer
{
// Material compiler + cache.
//
// - Inputs: `.material` (parsed to MaterialDocument by MaterialAsset)
// - Outputs: compiled `.shaderpkg` + reflection meta
//
// `CompileNow` is synchronous (called by hot-reload paths that need the
// result this frame). `RequestCompile` dispatches the build to JobSystem
// and is safe for UI hot paths — material inspector open used to block on
// the synchronous compile, causing a noticeable hiccup on selection.
//
// Renderer-owned because compilation policy, caching, and hot-reload are
// render concerns.
class MaterialCompiler
{
  public:
    struct Result
    {
        bool hasResult = false;
        bool success = false;
        bool stale = false; // source changed since this result was produced

        std::filesystem::path materialPath;
        std::filesystem::file_time_type sourceMtime{};
        bool hasSourceMtime = false;

        // When the material uses a surface graph, track graph mtime for staleness.
        std::filesystem::path surfaceGraphPath;
        std::filesystem::file_time_type graphSourceMtime{};
        bool hasGraphSourceMtime = false;

        std::string generatedShaderPkgPath;
        std::vector<std::string> errors;

        // Present on success.
        std::shared_ptr<const Rendering::ShaderMeta> shaderMeta;
        std::shared_ptr<const Rendering::ShaderPackage> shaderPackage;
    };

    explicit MaterialCompiler(AssetManager& assets);
    ~MaterialCompiler();

    // Returns the cached result (`shared_ptr` so worker-thread updates don't
    // invalidate the caller's pointer mid-frame). Marks the entry stale if
    // the material file changed since the cached result. Never triggers
    // compilation. `nullptr` if no compile has been requested yet.
    std::shared_ptr<const Result> Get(const GUID& materialGuid);

    // Synchronous compile. Returns the updated cached result. Use sparingly
    // — prefer `RequestCompile` from UI threads.
    std::shared_ptr<const Result> CompileNow(const GUID& materialGuid);

    // Submit a compile to JobSystem and return immediately. No-op if a
    // compile for `materialGuid` is already in-flight, or if a non-stale
    // result is already cached. Use `Get` to poll for the result.
    void RequestCompile(const GUID& materialGuid);

    // Clear cached compile result (and errors) for this guid.
    void Clear(const GUID& materialGuid);

    // Drop all cached compile results. Use on project switch — entries
    // captured shader/include paths and adapter-shader-dir state from the
    // previous project, and re-running an inspector compile against those
    // would either no-op (cached "success" we believe is current) or write
    // the wrong cache file.
    void Reset();

    // Ensure both GUID and path are populated for surface shader and vertex
    // modifier references. If one is present but the other is missing, resolve
    // via AssetRegistry. Called automatically by CompileNow(); also available
    // for callers that build materials without going through CompileNow().
    void ReconcileShaderReferences(MaterialDocument& doc) const;

    // Mark compile results stale and request rebuild for materials referencing
    // the given surface graph file (called when a .graph is saved).
    void NotifySurfaceGraphChanged(const std::filesystem::path& graphPath);

    // Writes public @sg-property values into referencing .material files and reloads assets.
    std::vector<GUID> SyncMaterialDocumentsFromShaderGraph(
        const std::filesystem::path& graphPath,
        const std::vector<ShaderGraph::SgGraphProperty>* propertiesOverride = nullptr);

    void PushPublicShaderGraphPropertiesToRuntime(
        const std::filesystem::path& graphPath,
        const std::vector<ShaderGraph::SgGraphProperty>& properties,
        RenderServices& renderServices);

    void PushMaterialDocumentPropertiesToRuntime(const GUID& materialGuid,
                                                 const MaterialDocument& doc,
                                                 RenderServices& renderServices);

  private:
    // Run the build off the snapshot. Safe to call from a worker thread.
    static Result BuildResultFromSnapshot(const MaterialDocument& doc,
                                          const std::filesystem::path& materialPath,
                                          const std::string& debugName,
                                          AssetManager& assets);

    bool RefreshStaleness_NoLock(Result& entry);
    MaterialAsset* TryLoadMaterialAsset(const GUID& guid);

  private:
    // Cache state lives on the heap behind a shared_ptr so async build jobs can
    // capture a copy and write their result through it without dereferencing
    // `this` — the compiler can be destroyed while a job is still in flight.
    struct CacheState
    {
        std::mutex Mutex;
        std::unordered_map<GUID, std::shared_ptr<Result>> Cache;
        std::unordered_set<GUID> InFlight;
    };

    AssetManager& m_Assets;

    std::shared_ptr<CacheState> m_State;
};

} // namespace Engine::Renderer
} // namespace GameEngine

