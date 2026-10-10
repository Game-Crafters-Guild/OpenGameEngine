#include "Engine/Rendering/MaterialCompiler.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "AssetCore/AssetTypes.h"
#include "Assets/MaterialAsset.h"
#include "Assets/ShaderProgramAsset.h"
#include "Core/Engine.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialShaderPackageBuilder.h"
#include "Engine/Rendering/PackageShaderDirs.h"
#include "Engine/Rendering/RenderServices.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Logger/Logger.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/ShaderGraph/SgPropertyBinding.h"
#include "Engine/Rendering/ShaderGraphMaterial.h"
#include "AssetCore/SharedFileRead.h"
#include "Graph/MaterialGraphCompiler.h"
#include "Rendering/ShaderGraph/SgGraphFileIO.h"
#include "Rendering/ShaderGraph/SgTagParser.h"

#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <sstream>

namespace GameEngine::Engine::Renderer
{
using namespace ::GameEngine::Rendering;
namespace
{
static std::filesystem::file_time_type TryGetMTime(const GameEngine::MaterialAsset& mat, bool& outOk)
{
    outOk = false;
    try
    {
        outOk = true;
        return mat.GetLastModified();
    }
    catch (...)
    {
        return {};
    }
}

static void ReconcileOneReference(const AssetRegistry& registry,
                                   std::string& guidStr, std::string& pathStr)
{
    AssetMetadata meta;
    if (!guidStr.empty() && pathStr.empty())
    {
        if (registry.TryGetAssetMetadata(GUID(guidStr), meta))
            pathStr = meta.Path.string();
    }
    if (guidStr.empty() && !pathStr.empty())
    {
        if (registry.TryGetAssetMetadata(std::filesystem::path(pathStr), meta))
            guidStr = meta.Guid.ToString();
    }
}

static std::filesystem::file_time_type TryPathMTime(const std::filesystem::path& path, bool& outOk)
{
    outOk = false;
    std::error_code ec;
    auto t = std::filesystem::last_write_time(path, ec);
    if (!ec)
    {
        outOk = true;
        return t;
    }
    return {};
}

static bool PathsReferToSameFile(const std::filesystem::path& a, const std::filesystem::path& b)
{
    std::error_code ec;
    if (std::filesystem::equivalent(a, b, ec))
        return true;
    return a.lexically_normal() == b.lexically_normal();
}

static bool MaterialReferencesGraph(const MaterialDocument& doc,
                                    const std::filesystem::path& materialPath,
                                    const std::filesystem::path& graphPathAbs,
                                    const GUID& graphGuid)
{
    if (graphGuid.IsNull() && graphPathAbs.empty())
        return false;

    if (!doc.surfaceGraphGuid.empty() && !graphGuid.IsNull())
    {
        if (GUID(doc.surfaceGraphGuid) == graphGuid)
            return true;
    }

    auto resolveRelative = [&](std::filesystem::path p) -> std::filesystem::path
    {
        if (p.empty())
            return {};
        if (!p.is_absolute() && !materialPath.empty())
            p = (materialPath.parent_path() / p).lexically_normal();
        else
            p = p.lexically_normal();
        return p;
    };

    if (!doc.surfaceGraph.empty())
    {
        if (PathsReferToSameFile(resolveRelative(doc.surfaceGraph), graphPathAbs))
            return true;
    }

    if (!doc.surfaceShader.empty())
    {
        if (PathsReferToSameFile(resolveRelative(doc.surfaceShader), graphPathAbs))
            return true;
    }

    if (!doc.surfaceShaderGuid.empty() && !graphGuid.IsNull())
    {
        if (GUID(doc.surfaceShaderGuid) == graphGuid)
            return true;
    }

    return false;
}

static void ScanMaterialsReferencingGraph(AssetManager& assets,
                                          const std::filesystem::path& graphPathAbs,
                                          const GUID& graphGuid,
                                          const std::function<void(const GUID&)>& onMatch)
{
    const Vector<GUID> materials = assets.GetRegistry().GetAssetsByType(AssetType::Material);
    for (const GUID& materialGuid : materials)
    {
        AssetMetadata matMeta;
        if (!assets.GetRegistry().TryGetAssetMetadata(materialGuid, matMeta))
            continue;

        std::string matText;
        if (!ReadFileTextShared(matMeta.Path, matText))
            continue;

        nlohmann::json j;
        try
        {
            j = nlohmann::json::parse(matText);
        }
        catch (...)
        {
            continue;
        }

        MaterialDocument doc{};
        doc.surfaceGraphGuid = j.value("surfaceGraphGuid", std::string());
        doc.surfaceGraph = j.value("surfaceGraph", std::string());
        doc.surfaceShaderGuid = j.value("surfaceShaderGuid", std::string());
        doc.surfaceShader = j.value("surfaceShader", std::string());

        if (!doc.surfaceGraphGuid.empty() && doc.surfaceGraph.empty())
        {
            AssetMetadata graphMeta;
            if (assets.GetRegistry().TryGetAssetMetadata(GUID(doc.surfaceGraphGuid), graphMeta))
                doc.surfaceGraph = graphMeta.Path.string();
        }

        if (!doc.surfaceShaderGuid.empty() && doc.surfaceShader.empty())
        {
            AssetMetadata shaderMeta;
            if (assets.GetRegistry().TryGetAssetMetadata(GUID(doc.surfaceShaderGuid), shaderMeta))
                doc.surfaceShader = shaderMeta.Path.string();
        }

        if (!MaterialReferencesGraph(doc, matMeta.Path, graphPathAbs, graphGuid))
            continue;

        onMatch(materialGuid);
    }
}
} // namespace

MaterialCompiler::MaterialCompiler(AssetManager& assets)
    : m_Assets(assets)
    , m_State(std::make_shared<CacheState>())
{
}

MaterialCompiler::~MaterialCompiler() = default;

bool MaterialCompiler::RefreshStaleness_NoLock(Result& entry)
{
    if (entry.materialPath.empty())
        return false;

    bool stale = false;

    std::error_code ec;
    auto ftime = std::filesystem::last_write_time(entry.materialPath, ec);
    if (!ec)
    {
        if (!entry.hasSourceMtime)
        {
            entry.hasSourceMtime = true;
            entry.sourceMtime = ftime;
        }
        else if (entry.sourceMtime != ftime)
        {
            stale = true;
        }
    }

    if (!entry.surfaceGraphPath.empty())
    {
        bool graphOk = false;
        const auto graphTime = TryPathMTime(entry.surfaceGraphPath, graphOk);
        if (graphOk)
        {
            if (!entry.hasGraphSourceMtime)
            {
                entry.hasGraphSourceMtime = true;
                entry.graphSourceMtime = graphTime;
            }
            else if (entry.graphSourceMtime != graphTime)
            {
                stale = true;
            }
        }
    }

    if (stale)
        entry.stale = true;
    return stale;
}

MaterialAsset* MaterialCompiler::TryLoadMaterialAsset(const GUID& guid)
{
    if (guid.IsNull())
        return nullptr;

    SharedPtr<Asset> a = m_Assets.GetAsset(guid);
    if (!a)
    {
        a = m_Assets.LoadAssetAsync(guid).get();
    }

    if (!a)
        return nullptr;

    auto* mat = dynamic_cast<MaterialAsset*>(a.get());
    return mat;
}

void MaterialCompiler::ReconcileShaderReferences(MaterialDocument& doc) const
{
    const auto& registry = m_Assets.GetRegistry();
    ReconcileOneReference(registry, doc.surfaceShaderGuid, doc.surfaceShader);
    ReconcileOneReference(registry, doc.vertexModifierGuid, doc.vertexModifier);
    ReconcileOneReference(registry, doc.surfaceGraphGuid, doc.surfaceGraph);
}

std::shared_ptr<const MaterialCompiler::Result> MaterialCompiler::Get(const GUID& materialGuid)
{
    if (materialGuid.IsNull())
        return nullptr;

    std::lock_guard<std::mutex> lock(m_State->Mutex);
    auto it = m_State->Cache.find(materialGuid);
    if (it == m_State->Cache.end())
        return nullptr;

    (void)RefreshStaleness_NoLock(*it->second);
    return it->second;
}

MaterialCompiler::Result MaterialCompiler::BuildResultFromSnapshot(
    const MaterialDocument& doc,
    const std::filesystem::path& materialPath,
    const std::string& debugName,
    AssetManager& assets)
{
    Result e;
    e.hasResult = true;
    e.materialPath = materialPath;

    Rendering::MaterialBuildContext ctx;
    ctx.AdapterShaderDir = ResolveAdapterShaderDir(assets);
    // The bundle cache root wins when one is set: a compiler-less runtime (web)
    // ships its cooked variants beside the binary, not under the workspace.
    const std::filesystem::path& shaderCacheRoot = ShaderProgramAsset::GetShaderCacheRoot();
    ctx.CacheRoot = shaderCacheRoot.empty()
        ? EngineCore::GetInstance().GetWorkspaceRoot() / ".Cache" / "Shaders"
        : shaderCacheRoot;
    ctx.IncludeDirs = {assets.GetAssetRoot()};
    ctx.ProjectRoots = CollectProjectRoots(assets);
    ctx.AssetSourceRoots = CollectAssetSourceRoots(assets);
    ctx.PackageShaderDirs = CollectPackageShaderDirs(assets);
    AppendStagedModuleShaderDirs(ctx.AdapterShaderDir, ctx.PackageShaderDirs);

    MaterialShaderPackageBuilder builder(doc, materialPath, debugName, ctx);
    e.surfaceGraphPath = builder.GetGraphSourcePath();
    if (!builder.IsPrepared())
    {
        e.errors = builder.GetErrors();
        e.success = false;
        return e;
    }

    if (!e.surfaceGraphPath.empty())
    {
        bool graphOk = false;
        e.graphSourceMtime = TryPathMTime(e.surfaceGraphPath, graphOk);
        e.hasGraphSourceMtime = graphOk;
    }

    // SPIR-V: this compiler produces reflection meta and cache paths for the
    // editor; nothing reads the package's stage bytes, so no device's ingestion
    // form is in play.
    Rendering::MaterialBuildResult built = builder.Build(Rendering::ShaderSourceKind::SpirV);

    e.generatedShaderPkgPath = built.generatedShaderPkgPath;

    if (!built.success || !built.package)
    {
        e.success = false;
        e.errors.insert(e.errors.end(), built.errors.begin(), built.errors.end());
        return e;
    }

    e.success = true;
    e.shaderMeta = std::make_shared<Rendering::ShaderMeta>(built.package->meta);
    e.shaderPackage = std::shared_ptr<Rendering::ShaderPackage>(
        built.package.release(), Rendering::ShaderPackageDeleter{});
    return e;
}

std::shared_ptr<const MaterialCompiler::Result> MaterialCompiler::CompileNow(const GUID& materialGuid)
{
    if (materialGuid.IsNull())
        return nullptr;

    auto* mat = TryLoadMaterialAsset(materialGuid);
    if (!mat)
        return nullptr;

    // Snapshot input on the calling thread (cheap copy of the document).
    const auto materialPath = mat->GetPath();
    const auto debugName = mat->GetName();
    const auto parseErrors = mat->GetErrors();
    const bool isLoaded = mat->GetState() == AssetState::Loaded;
    bool mtimeOk = false;
    const auto mtime = TryGetMTime(*mat, mtimeOk);
    MaterialDocument doc = mat->GetDocument();
    ReconcileShaderReferences(doc);

    std::shared_ptr<Result> entry;
    if (isLoaded)
    {
        entry = std::make_shared<Result>(BuildResultFromSnapshot(doc, materialPath, debugName, m_Assets));
    }
    else
    {
        entry = std::make_shared<Result>();
        entry->hasResult = true;
        entry->materialPath = materialPath;
        entry->success = false;
        entry->errors.push_back("MaterialCompiler: material asset is not loaded (parse failed).");
    }

    // Preserve parse errors alongside compile errors.
    entry->errors.insert(entry->errors.begin(), parseErrors.begin(), parseErrors.end());
    if (mtimeOk)
    {
        entry->hasSourceMtime = true;
        entry->sourceMtime = mtime;
    }
    entry->stale = false;

    std::lock_guard<std::mutex> lock(m_State->Mutex);
    m_State->Cache[materialGuid] = entry;
    return entry;
}

void MaterialCompiler::RequestCompile(const GUID& materialGuid)
{
    if (materialGuid.IsNull())
        return;

    {
        std::lock_guard<std::mutex> lock(m_State->Mutex);
        // Skip if already compiled (and not stale) or already in-flight.
        auto it = m_State->Cache.find(materialGuid);
        if (it != m_State->Cache.end() && it->second->hasResult && !it->second->stale
            && !RefreshStaleness_NoLock(*it->second))
        {
            return;
        }
        if (!m_State->InFlight.insert(materialGuid).second)
            return;
    }

    auto* mat = TryLoadMaterialAsset(materialGuid);
    if (!mat)
    {
        std::lock_guard<std::mutex> lock(m_State->Mutex);
        m_State->InFlight.erase(materialGuid);
        return;
    }

    // Snapshot all inputs the build needs while we're still on the UI thread.
    // The worker must not touch the AssetManager state for this material once
    // dispatched — the user could modify the document via the inspector.
    auto materialPath = mat->GetPath();
    auto debugName = mat->GetName();
    auto parseErrors = mat->GetErrors();
    const bool isLoaded = mat->GetState() == AssetState::Loaded;
    bool mtimeOk = false;
    const auto mtime = TryGetMTime(*mat, mtimeOk);
    MaterialDocument doc = mat->GetDocument();
    ReconcileShaderReferences(doc);

    auto& engine = GameEngine::EngineCore::GetInstance();
    if (!engine.IsInitialized())
    {
        // No JobSystem available — fall back to synchronous build.
        auto entry = std::make_shared<Result>(
            isLoaded ? BuildResultFromSnapshot(doc, materialPath, debugName, m_Assets) : Result{});
        if (!isLoaded)
        {
            entry->hasResult = true;
            entry->success = false;
            entry->materialPath = materialPath;
            entry->errors.push_back("MaterialCompiler: material asset is not loaded (parse failed).");
        }
        entry->errors.insert(entry->errors.begin(), parseErrors.begin(), parseErrors.end());
        if (mtimeOk) { entry->hasSourceMtime = true; entry->sourceMtime = mtime; }
        std::lock_guard<std::mutex> lock(m_State->Mutex);
        m_State->Cache[materialGuid] = std::move(entry);
        m_State->InFlight.erase(materialGuid);
        return;
    }

    // Capture a copy of the shared cache state (not `this`): the compiler can be
    // destroyed while this job is queued or running, and m_State keeps the cache and
    // its mutex alive for the job. The AssetManager is captured by raw pointer — a
    // long-lived engine system, the same exposure the previous code had via the
    // m_Assets reference. (At process teardown the AssetManager is destroyed just
    // before the JobSystem is joined, so a job still in flight in that narrow window
    // would dereference a freed AssetManager — a pre-existing shutdown risk, not
    // addressed here.)
    engine.GetJobSystem().Submit(
        [state = m_State,
         assets = &m_Assets,
         materialGuid,
         doc = std::move(doc),
         materialPath = std::move(materialPath),
         debugName = std::move(debugName),
         parseErrors = std::move(parseErrors),
         isLoaded,
         mtimeOk,
         mtime]() mutable
        {
            auto entry = std::make_shared<Result>();
            if (isLoaded)
            {
                *entry = BuildResultFromSnapshot(doc, materialPath, debugName, *assets);
            }
            else
            {
                entry->hasResult = true;
                entry->materialPath = materialPath;
                entry->success = false;
                entry->errors.push_back("MaterialCompiler: material asset is not loaded (parse failed).");
            }
            entry->errors.insert(entry->errors.begin(), parseErrors.begin(), parseErrors.end());
            if (mtimeOk) { entry->hasSourceMtime = true; entry->sourceMtime = mtime; }
            entry->stale = false;

            std::lock_guard<std::mutex> lock(state->Mutex);
            state->Cache[materialGuid] = std::move(entry);
            state->InFlight.erase(materialGuid);
        });
}

void MaterialCompiler::Clear(const GUID& materialGuid)
{
    if (materialGuid.IsNull())
        return;
    std::lock_guard<std::mutex> lock(m_State->Mutex);
    m_State->Cache.erase(materialGuid);
}

void MaterialCompiler::Reset()
{
    std::lock_guard<std::mutex> lock(m_State->Mutex);
    m_State->Cache.clear();
    // Don't clear `m_State->InFlight` — those workers still hold references and
    // will write back to a fresh cache entry; their results are harmless
    // even if stale (they target a guid that no longer matches anything
    // useful), and clearing here would race with the worker's later
    // `state->InFlight.erase`.
}

void MaterialCompiler::NotifySurfaceGraphChanged(const std::filesystem::path& graphPath)
{
    if (graphPath.empty())
        return;

    std::error_code ec;
    const std::filesystem::path graphAbs = std::filesystem::absolute(graphPath, ec);
    if (ec)
        return;

    GUID graphGuid = GUID::Null();
    AssetMetadata graphMeta;
    if (m_Assets.GetRegistry().TryGetAssetMetadata(graphAbs, graphMeta))
        graphGuid = graphMeta.Guid;

    ScanMaterialsReferencingGraph(
        m_Assets, graphAbs, graphGuid, [this](const GUID& materialGuid)
        {
            {
                std::lock_guard<std::mutex> lock(m_State->Mutex);
                auto it = m_State->Cache.find(materialGuid);
                if (it != m_State->Cache.end() && it->second)
                    it->second->stale = true;
            }
            RequestCompile(materialGuid);
        });
}

std::vector<GUID> MaterialCompiler::SyncMaterialDocumentsFromShaderGraph(
    const std::filesystem::path& graphPath,
    const std::vector<ShaderGraph::SgGraphProperty>* propertiesOverride)
{
    std::vector<GUID> updated;
    if (graphPath.empty())
        return updated;

    std::error_code ec;
    const std::filesystem::path graphAbs = std::filesystem::absolute(graphPath, ec);
    if (ec)
        return updated;

    GUID graphGuid = GUID::Null();
    AssetMetadata graphMeta;
    if (m_Assets.GetRegistry().TryGetAssetMetadata(graphAbs, graphMeta))
        graphGuid = graphMeta.Guid;

    ScanMaterialsReferencingGraph(m_Assets, graphAbs, graphGuid,
                                  [&](const GUID& materialGuid)
                                  {
                                      MaterialAsset* matAsset = TryLoadMaterialAsset(materialGuid);
                                      if (!matAsset)
                                          return;

                                      MaterialDocument doc = matAsset->GetDocument();
                                      if (propertiesOverride)
                                          ShaderGraph::ApplyShaderGraphPropertiesToMaterialDocument(
                                              *propertiesOverride, doc);
                                      else
                                          MergeShaderGraphTagsIntoDocument(graphAbs, doc);

                                      try
                                      {
                                          nlohmann::json j = SerializeMaterialDocument(doc);
                                          std::ofstream out(matAsset->GetPath());
                                          if (!out.is_open())
                                              return;
                                          out << j.dump(2);
                                      }
                                      catch (...)
                                      {
                                          return;
                                      }

                                      if (matAsset->Reload() != ReloadOutcome::Reloaded)
                                          return;

                                      updated.push_back(materialGuid);
                                  });
    return updated;
}

void MaterialCompiler::PushPublicShaderGraphPropertiesToRuntime(
    const std::filesystem::path& graphPath,
    const std::vector<ShaderGraph::SgGraphProperty>& properties,
    RenderServices& renderServices)
{
    if (graphPath.empty() || properties.empty())
        return;

    std::error_code ec;
    const std::filesystem::path graphAbs = std::filesystem::absolute(graphPath, ec);
    if (ec)
        return;

    GUID graphGuid = GUID::Null();
    AssetMetadata graphMeta;
    if (m_Assets.GetRegistry().TryGetAssetMetadata(graphAbs, graphMeta))
        graphGuid = graphMeta.Guid;

    MaterialDocument scratch;
    ShaderGraph::ApplyShaderGraphPropertiesToMaterialDocument(properties, scratch);

    ScanMaterialsReferencingGraph(m_Assets, graphAbs, graphGuid,
                                  [&](const GUID& materialGuid)
                                  {
                                      Material* rtMat = renderServices.Materials().Registry().Find(materialGuid);
                                      if (!rtMat)
                                          return;

                                      for (const ShaderGraph::SgGraphProperty& prop : properties)
                                      {
                                          if (!prop.IsPublic)
                                              continue;
                                          const auto binding =
                                              ShaderGraph::ResolvePublicPropertyBinding(prop.Name, prop.Type);
                                          if (!binding)
                                              continue;

                                          for (const std::string& key : binding->MaterialPropertyKeys)
                                          {
                                              auto it = scratch.properties.find(key);
                                              if (it == scratch.properties.end())
                                                  continue;
                                              const StringId sid = HashStringId(key);
                                              if (const float* fval = std::get_if<float>(&it->second))
                                                  rtMat->SetFloat(sid, *fval);
                                              else if (const std::vector<float>* vval =
                                                           std::get_if<std::vector<float>>(&it->second))
                                              {
                                                  if (!vval->empty())
                                                      rtMat->SetVector(sid, vval->data(),
                                                                       static_cast<uint32_t>(vval->size()));
                                              }
                                          }
                                      }
                                  });
    // The SetFloat/SetVector writes above bump the global content epoch;
    // PackMaterialSSBO repacks the shared MaterialParams SSBO next frame.
}

void MaterialCompiler::PushMaterialDocumentPropertiesToRuntime(const GUID& materialGuid,
                                                               const MaterialDocument& doc,
                                                               RenderServices& renderServices)
{
    if (materialGuid.IsNull())
        return;

    Material* rtMat = renderServices.Materials().Registry().Find(materialGuid);
    if (!rtMat)
        return;

    for (const auto& [key, value] : doc.properties)
    {
        const StringId sid = HashStringId(key);
        if (const float* fval = std::get_if<float>(&value))
            rtMat->SetFloat(sid, *fval);
        else if (const std::vector<float>* vval = std::get_if<std::vector<float>>(&value))
        {
            if (!vval->empty())
                rtMat->SetVector(sid, vval->data(), static_cast<uint32_t>(vval->size()));
        }
    }
    // The SetFloat/SetVector writes above bump the global content epoch;
    // PackMaterialSSBO repacks the shared MaterialParams SSBO next frame.
}

} // namespace GameEngine::Engine::Renderer
