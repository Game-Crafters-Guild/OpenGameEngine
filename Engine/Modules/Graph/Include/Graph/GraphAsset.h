#pragma once

#include "AssetCore/Asset.h"
#include "Graph/GraphModel.h"
#include <memory>
#include <string>

namespace GameEngine {

/**
 * Asset type for .graph files (node-based game logic or material graphs).
 * Loads/serializes Graph::Model from/to JSON.
 */
class GraphAsset : public Asset {
public:
    GraphAsset(const GUID& guid, const std::filesystem::path& path);

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;

    const Graph::Model& GetModel() const { return m_Model; }
    Graph::Model& GetModelMutable() { return m_Model; }

private:
    Graph::Model m_Model;
};

/** Shader-graph .glsl text for a material-kind model: `@sg-*` tags, the authoring
 *  JSON fence, and the compiled `EvaluateSurface` body. The body is empty when the
 *  model does not compile; the tags and fence still round-trip. */
std::string SerializeMaterialGraphGlsl(const Graph::Model& model, const std::string& graphName);

/** Save graph model to a file path (JSON). Returns true on success. */
bool SaveGraphToPath(const Graph::Model& model, const std::filesystem::path& path);

} // namespace GameEngine
