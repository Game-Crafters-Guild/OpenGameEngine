#include "Graph/GraphAsset.h"

#include "AssetCore/SharedFileRead.h"

#include "Graph/GraphGlslAuthoring.h"
#include "Graph/SgGraphModelBridge.h"

#include "Rendering/ShaderGraph/SgGraphCompiler.h"
#include "Rendering/ShaderGraph/SgGraphFileIO.h"
#include "Rendering/ShaderGraph/SgNodeReflector.h"
#include "Rendering/ShaderGraph/SgTagParser.h"

#include <fstream>
#include <iterator>
#include <string>

namespace GameEngine
{

GraphAsset::GraphAsset(const GUID& guid, const std::filesystem::path& path)
    : Asset(guid, AssetType::Graph, path)
{
}

bool GraphAsset::Load()
{
    const std::filesystem::path path = GetPath();
    if (path.extension() == ".glsl")
    {
        const auto parsed = ShaderGraph::ParseShaderGraphFile(path);
        if (ShaderGraph::IsShaderGraphSource(parsed.TagBlock))
        {
            if (!Graph::LoadModelFromShaderGraphComments(parsed.TagBlock, m_Model))
            {
                SetState(AssetState::Failed);
                return false;
            }
            SetState(AssetState::Loaded);
            return true;
        }
    }

    String text;
    if (!ReadFileTextShared(path, text))
    {
        SetState(AssetState::Failed);
        return false;
    }
    const bool ok = Graph::FromJson(text, m_Model);
    SetState(ok ? AssetState::Loaded : AssetState::Failed);
    return ok;
}

bool GraphAsset::LoadFromData(const Vector<uint8>& data)
{
    std::string text(reinterpret_cast<const char*>(data.data()), data.size());
    if (ShaderGraph::IsShaderGraphSource(text))
    {
        const auto parsed = ShaderGraph::ParseShaderGraphSource(text);
        if (!Graph::LoadModelFromShaderGraphComments(parsed.TagBlock, m_Model))
        {
            SetState(AssetState::Failed);
            return false;
        }
        SetState(AssetState::Loaded);
        return true;
    }
    const bool ok = Graph::FromJson(text, m_Model);
    SetState(ok ? AssetState::Loaded : AssetState::Failed);
    return ok;
}

void GraphAsset::Unload()
{
    m_Model.Nodes.clear();
    m_Model.Links.clear();
    SetState(AssetState::Unloaded);
}

std::string SerializeMaterialGraphGlsl(const Graph::Model& model, const std::string& graphName)
{
    const auto doc = GraphModelToSgDocument(model, graphName);
    const auto& library = ShaderGraph::GetSharedNodeLibraryIndex();
    const ShaderGraph::SgCompileResult compiled = ShaderGraph::SgGraphCompiler::Compile(doc, library);

    std::string serialized = Graph::AppendAuthoringJsonFence(
        ShaderGraph::SerializeGraphDocumentTags(doc), Graph::ToJson(model));
    if (!compiled.Success || compiled.Body.empty())
        return serialized;

    if (serialized.empty() || serialized.back() != '\n')
        serialized += '\n';
    serialized += compiled.Body;
    return serialized;
}

bool SaveGraphToPath(const Graph::Model& model, const std::filesystem::path& path)
{
    if (model.KindId == Graph::kKindIdMaterial && path.extension() == ".glsl")
    {
        const std::string graphName = path.stem().string();
        // `<stem>_built.glsl` is the derived surface the material pipeline consumes:
        // tags plus body only, no authoring fence to re-parse.
        if (graphName.ends_with("_built"))
        {
            const auto doc = GraphModelToSgDocument(model, graphName);
            const auto& library = ShaderGraph::GetSharedNodeLibraryIndex();
            const ShaderGraph::SgCompileResult compiled =
                ShaderGraph::SgGraphCompiler::Compile(doc, library);
            return ShaderGraph::SaveGraphFile(path, doc,
                                              compiled.Success ? compiled.Body : std::string{});
        }

        const std::string serialized = SerializeMaterialGraphGlsl(model, graphName);
        {
            std::ifstream in(path);
            if (in)
            {
                const std::string existing((std::istreambuf_iterator<char>(in)),
                                           std::istreambuf_iterator<char>());
                if (existing == serialized)
                    return true;
            }
        }
        std::ofstream out(path);
        if (!out.is_open())
            return false;
        out << serialized;
        return true;
    }

    std::string json = Graph::ToJson(model);
    std::ofstream out(path);
    if (!out.is_open())
        return false;
    out << json;
    return true;
}

} // namespace GameEngine
