#include "Graph/ShaderGraphTemplate.h"

#include "Graph/GraphAsset.h"
#include "Graph/GraphModel.h"

#include "Rendering/Materials/MaterialDocument.h"

namespace GameEngine
{
namespace Graph
{
namespace {

// Master node identity and placement in a brand-new graph. The panel frames all
// nodes on open, so the position only decides where the node sits relative to
// the space an author fills in to its left.
constexpr const char* kSurfaceOutputNodeId = "surface_output";
constexpr float kSurfaceOutputPositionX = 320.f;
constexpr float kSurfaceOutputPositionY = 160.f;

Model MakeTemplateModel()
{
    Model model;
    model.KindId.assign(kKindIdMaterial);

    Node surfaceOutput;
    surfaceOutput.Id = kSurfaceOutputNodeId;
    surfaceOutput.TypeId = "SurfaceOutput";
    surfaceOutput.PositionX = kSurfaceOutputPositionX;
    surfaceOutput.PositionY = kSurfaceOutputPositionY;
    model.Nodes.push_back(std::move(surfaceOutput));

    return model;
}

} // namespace

std::string MakeShaderGraphTemplateSource(const std::string& stem)
{
    return SerializeMaterialGraphGlsl(MakeTemplateModel(), stem);
}

MaterialDocument MakeShaderGraphTemplateMaterial(const std::string& stem)
{
    MaterialDocument doc = MaterialDocument::CreateDefaultPBR(stem);
    doc.surfaceShader = stem + ".glsl"; // sibling file: material-dir-first resolution
    return doc;
}

} // namespace Graph
} // namespace GameEngine
