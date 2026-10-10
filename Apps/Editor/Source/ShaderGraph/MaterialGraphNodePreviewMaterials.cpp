#include "ShaderGraph/MaterialGraphNodePreviewMaterials.h"

#include "Assets/MaterialAsset.h"
#include "EditorContext.h"
#include "Engine/Rendering/ShaderGraphMaterial.h"
#include "Graph/SgGraphModelBridge.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/ShaderGraph/SgGraphFileIO.h"
#include "ShaderGraph/GraphPreviewMaterialRuntime.h"
#include "ShaderGraph/MaterialGraphPreviewModel.h"
#include "Types/Fnv1a.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <system_error>
#include <utility>
#include <vector>

namespace GameEngine {
namespace Editor {
namespace {

constexpr const char* kNodeSurfaceStem = "node_";

/** The parameter set is shared by every node variant — only the Output wiring
    differs — so one document serves the whole pass. */
MaterialDocument BuildNodePreviewDocument(const MaterialGraphPreviewModel& preview)
{
    MaterialDocument doc = MaterialDocument::CreateDefaultPBR("Graph Node Preview");
    doc.surfaceGraph.clear();
    doc.surfaceGraphGuid.clear();
    if (!preview.Model.LightingModel.empty())
        doc.lightingModel = preview.Model.LightingModel;
    ApplyMaterialGraphVariablesToPreviewDocument(preview.Model, doc);
    ApplyPreviewLaneValues(preview, doc);
    return doc;
}

std::filesystem::path NodeCacheDirectory(const GraphPreviewCacheSource& source)
{
    if (source.Root.empty())
        return {};
    return source.Root / "Generated" / "GraphPreview" / "Nodes";
}

const Graph::Port* FirstOutputPin(const Graph::Node& node)
{
    for (const Graph::Port& pin : node.Ports)
    {
        if (pin.Direction == Graph::PortDirection::Out)
            return &pin;
    }
    return nullptr;
}

} // namespace

MaterialGraphNodePreviewMaterials::SyncResult
MaterialGraphNodePreviewMaterials::Sync(const EditorContext* ctx, const Graph::Model& model)
{
    SyncResult result;
    if (!ctx)
        return result;

    if (model.KindId != Graph::kKindIdMaterial)
    {
        result.SetChanged = !m_Materials.empty();
        Clear();
        return result;
    }

    /* Digest the PREVIEW projection, not the authored model: a constant's value
       lives in a user lane there, so a scrub leaves the digest still and never
       reaches the compile path. */
    const MaterialGraphPreviewModel preview = MakeMaterialGraphPreviewModel(model);
    const std::uint64_t digest = MaterialGraphSurfaceDigest(preview.Model);
    if (m_HasModelDigest && digest == m_ModelDigest)
        return result;

    const GraphPreviewCacheSource cacheSource = ResolveGraphPreviewCacheSource(ctx);
    const std::filesystem::path cacheDir = NodeCacheDirectory(cacheSource);
    if (cacheDir.empty())
        return result;
    std::error_code ec;
    std::filesystem::create_directories(cacheDir, ec);
    if (ec)
        return result;

    const Graph::Node* output = nullptr;
    for (const Graph::Node& node : preview.Model.Nodes)
    {
        if (node.TypeId == "SurfaceOutput")
            output = &node;
    }
    if (!output)
    {
        result.SetChanged = !m_Materials.empty();
        Clear();
        return result;
    }

    /* One copy for the whole pass. Every variant is this graph with the Output
       fed by a different node, so the links that reached the Output come off
       once and each node's wire is pushed and popped in turn. */
    Graph::Model variant = preview.Model;
    const std::string outputId = output->Id;
    variant.Links.erase(std::remove_if(variant.Links.begin(), variant.Links.end(),
                                       [&](const Graph::Edge& link)
                                       { return link.TargetNodeId == outputId; }),
                        variant.Links.end());
    const std::string wireId = variant.GenerateLinkId();

    const MaterialDocument sharedDoc = BuildNodePreviewDocument(preview);

    std::unordered_map<std::string, Entry> next;
    next.reserve(preview.Model.Nodes.size());
    for (const Graph::Node& node : preview.Model.Nodes)
    {
        if (node.Id == outputId)
            continue;
        const Graph::Port* firstOut = FirstOutputPin(node);
        if (!firstOut)
            continue;

        Graph::Edge wire;
        wire.Id = wireId;
        wire.SourceNodeId = node.Id;
        wire.SourcePortId = firstOut->Id;
        wire.TargetNodeId = outputId;
        wire.TargetPortId = "BaseColor";
        variant.Links.push_back(std::move(wire));

        const std::filesystem::path glslPath = cacheDir / (kNodeSurfaceStem + node.Id + ".glsl");
        std::string source;
        std::vector<std::string> errors;
        const bool materialized = Engine::Renderer::MaterializeShaderGraphSurfaceFromModel(
            variant, "Node Preview " + node.Id, glslPath, source, errors);
        variant.Links.pop_back();
        if (!materialized)
            continue;

        const std::uint64_t sourceHash = Hashing::Fnv1a64(source);
        const auto previous = m_Entries.find(node.Id);
        if (previous != m_Entries.end() && previous->second.SourceHash == sourceHash
            && !previous->second.Material.IsNull())
        {
            // Identical GLSL: the file on disk and the compiled pipeline both
            // still answer for this node.
            next.emplace(node.Id, previous->second);
            continue;
        }

        if (!ShaderGraph::WriteGraphFileTextIfChanged(glslPath, source))
            continue;

        MaterialDocument matDoc = sharedDoc;
        // Absolute: the preview variant compiles through a synthetic cache
        // material whose directory is not this one, so a relative reference
        // resolves elsewhere, the #include yields no EvaluateSurface, and the
        // fragment falls back to the magenta missing-texture.
        matDoc.surfaceShader = glslPath.string();
        const std::filesystem::path matPath =
            cacheDir / (kNodeSurfaceStem + node.Id + ".material");
        if (!WriteGraphPreviewMaterialFile(matPath, matDoc))
            continue;

        const GUID guid = SyncGraphPreviewMaterial(ctx, matPath, matDoc,
                                                   GraphPreviewCompile::Shader,
                                                   cacheSource.Alias);
        if (guid.IsNull())
            continue;

        next.emplace(node.Id, Entry{guid, sourceHash});
        result.SourcesChanged = true;
    }

    result.SetChanged = next.size() != m_Entries.size()
                        || std::any_of(next.begin(), next.end(),
                                       [this](const auto& entry)
                                       {
                                           const auto it = m_Entries.find(entry.first);
                                           return it == m_Entries.end()
                                                  || it->second.Material != entry.second.Material;
                                       });

    m_Entries = std::move(next);
    m_ModelDigest = digest;
    m_HasModelDigest = true;
    m_Materials.clear();
    m_Materials.reserve(m_Entries.size());
    for (const auto& [nodeId, entry] : m_Entries)
        m_Materials.emplace(nodeId, entry.Material);
    // The regenerated documents already carry the current values, so the next
    // property push has nothing to say until they move again.
    m_PropsJson = SerializeMaterialDocument(sharedDoc).dump(2);
    return result;
}

bool MaterialGraphNodePreviewMaterials::NeedsSync(const Graph::Model& model) const
{
    if (model.KindId != Graph::kKindIdMaterial)
        return !m_Entries.empty();
    return !m_HasModelDigest || MaterialGraphSurfaceDigest(model) != m_ModelDigest;
}

bool MaterialGraphNodePreviewMaterials::RefreshProperties(const EditorContext* ctx,
                                                          const Graph::Model& model)
{
    if (!ctx || m_Entries.empty())
        return false;

    const MaterialGraphPreviewModel preview = MakeMaterialGraphPreviewModel(model);
    const MaterialDocument sharedDoc = BuildNodePreviewDocument(preview);
    std::string propsJson = SerializeMaterialDocument(sharedDoc).dump(2);
    if (propsJson == m_PropsJson)
        return false;
    m_PropsJson = std::move(propsJson);

    const GraphPreviewCacheSource cacheSource = ResolveGraphPreviewCacheSource(ctx);
    const std::filesystem::path cacheDir = NodeCacheDirectory(cacheSource);
    if (cacheDir.empty())
        return false;

    for (const auto& [nodeId, entry] : m_Entries)
    {
        MaterialDocument doc = sharedDoc;
        doc.surfaceShader = (cacheDir / (kNodeSurfaceStem + nodeId + ".glsl")).string();
        SyncGraphPreviewMaterial(ctx, cacheDir / (kNodeSurfaceStem + nodeId + ".material"), doc,
                                 GraphPreviewCompile::PropertiesOnly, cacheSource.Alias);
    }
    return true;
}

void MaterialGraphNodePreviewMaterials::Clear()
{
    m_Entries.clear();
    m_Materials.clear();
    m_ModelDigest = 0;
    m_HasModelDigest = false;
    m_PropsJson.clear();
}

} // namespace Editor
} // namespace GameEngine
