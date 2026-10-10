#include "Graph/GraphNodeRegistry.h"

#include "Graph/GraphNodeIconStems.h"
#include "Graph/GraphTypeRegistry.h"
#include "Graph/GraphValue.h"
#include "Rendering/ShaderGraph/SgGraphFileIO.h"
#include "Rendering/ShaderGraph/SgPseudoNodes.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

namespace GameEngine {
namespace {

constexpr NodeIconStemEntry kMaterialNodeIconStems[] = {
    {"Abs", "abs"},
    {"Add", "add"},
    {"Billboard", "billboard"},
    {"Ceil", "ceil"},
    {"Clamp", "clamp"},
    {"ColorConstant", "color"},
    {"ColorMix", "blend"},
    {"ColorParameter", "parameter"},
    {"CombineVec4", "compose"},
    {"Comment", "organization"},
    {"Compare", "compare"},
    {"Cos", "cos"},
    {"Cross", "cross"},
    {"CurveTexture", "curve"},
    {"DDX", "derivative"},
    {"DDY", "derivative"},
    {"DistanceFade", "fade"},
    {"Divide", "divide"},
    {"DotProduct", "dot"},
    {"Exp", "exp"},
    {"FWidth", "derivative"},
    {"FloatConstant", "float"},
    {"FloatParameter", "parameter"},
    {"Floor", "floor"},
    {"Fract", "fract"},
    {"Fresnel", "fresnel"},
    {"Grayscale", "grayscale"},
    {"Group", "organization"},
    {"HSV2RGB", "hsv"},
    {"If", "if"},
    {"Length", "length"},
    {"Lerp", "lerp"},
    {"Log", "log"},
    {"Max", "max"},
    {"Min", "min"},
    {"Mod", "mod"},
    {"Multiply", "multiply"},
    {"MultiplyAdd", "madd"},
    {"Negate", "negate"},
    {"NormalBlend", "normal-blend"},
    {"NormalStrength", "normal-strength"},
    {"NormalVector", "normal-input"},
    {"Normalize", "normalize"},
    {"OneMinus", "oneminus"},
    {"Power", "power"},
    {"ProximityFade", "fade"},
    {"RGB2HSV", "hsv"},
    {"RandomRange", "random"},
    {"Reflect", "reflect"},
    {"Refract", "refract"},
    {"Remap", "remap"},
    {"Reroute", "reroute"},
    {"RotateByAxis", "rotate"},
    {"SampleCubemap", "sample-cube"},
    {"SampleTexture", "sample2d"},
    {"SampleTexture2D", "sample2d"},
    {"SampleTexture2DLOD", "sample-lod"},
    {"SampleTextureArray", "sample-array"},
    {"Saturate", "saturate"},
    {"ScreenUV", "uv"},
    {"Sign", "sign"},
    {"Sin", "sin"},
    {"SmoothStep", "smoothstep"},
    {"Sqrt", "sqrt"},
    {"Step", "step"},
    {"Subtract", "subtract"},
    {"SurfaceOutput", "output"},
    {"Tan", "tan"},
    {"Tangent", "tangent"},
    {"Time", "time"},
    {"TransformDirection", "transform-dir"},
    {"TransformNormal", "transform-nrm"},
    {"TransformPosition", "transform-pos"},
    {"TransformUV", "transform-uv"},
    {"TriplanarSample", "triplanar"},
    {"TriplanarWeights", "triplanar"},
    {"UV", "uv"},
    {"UV1", "uv"},
    {"Vec2Constant", "vec2"},
    {"Vec2Parameter", "parameter"},
    {"Vec3Constant", "vec3"},
    {"Vec3Parameter", "parameter"},
    {"Vec4Constant", "vec4"},
    {"Vec4Parameter", "parameter"},
    {"VectorAdd", "add"},
    {"VectorClamp", "clamp"},
    {"VectorCompose", "compose"},
    {"VectorDecompose", "decompose"},
    {"VectorDistance", "distance"},
    {"VectorDivide", "divide"},
    {"VectorLerp", "lerp"},
    {"VectorMultiply", "multiply"},
    {"VectorSubtract", "subtract"},
    {"VertexColor", "vertex-color"},
    {"ViewDirection", "view"},
    {"WindSway", "wind"},
    {"WorldPosition", "position"},
};


constexpr NodeIconStemEntry kMaterialCategoryIconStems[] = {
    {"Color", "category-color"},
    {"Input", "category-input"},
    {"Math", "category-math"},
    {"Motion", "category-motion"},
    {"Normal", "category-normal"},
    {"Organization", "category-organization"},
    {"Output", "category-output"},
    {"Parameters", "category-parameters"},
    {"Sampling", "category-sampling"},
    {"Transform", "category-transform"},
    {"Utility", "category-utility"},
};


void AttachMaterialNodeIconStem(NodeTypeMeta& meta)
{
    meta.IconStem = std::string(FindIconStem(kMaterialNodeIconStems, meta.TypeId));
}


/* Shared editing ranges for material node values. Only genuinely bounded
   quantities get one; arbitrary multipliers and positions stay unbounded. */
constexpr float kNormalizedMin = 0.0f;
constexpr float kNormalizedMax = 1.0f;
constexpr float kExponentMin = 0.01f;
constexpr float kExponentMax = 64.0f;
constexpr float kUvTilingLimit = 64.0f;
constexpr float kUvOffsetLimit = 1024.0f;
constexpr float kMipLodMax = 16.0f;
constexpr float kTriplanarSharpnessMax = 16.0f;
constexpr float kNormalStrengthMax = 8.0f;
constexpr float kTextureArrayLayerMax = 2048.0f;

void SetPortRange(NodeTypeMeta& meta, std::string_view portId, float min, float max)
{
    for (NodePortTemplate& port : meta.Ports)
    {
        if (port.Direction == Graph::PortDirection::In && port.Id == portId)
        {
            port.Min = min;
            port.Max = max;
            return;
        }
    }
}

void AttachKnownMaterialPortRanges(NodeTypeMeta& meta)
{
    const std::string& typeId = meta.TypeId;
    if (typeId == "Fresnel")
        SetPortRange(meta, "power", kExponentMin, kExponentMax);
    else if (typeId == "Lerp" || typeId == "VectorLerp" || typeId == "ColorMix")
        SetPortRange(meta, "weight", kNormalizedMin, kNormalizedMax);
    else if (typeId == "CurveTexture")
    {
        SetPortRange(meta, "value", kNormalizedMin, kNormalizedMax);
        SetPortRange(meta, "row", kNormalizedMin, kNormalizedMax);
    }
    else if (typeId == "TriplanarWeights")
        SetPortRange(meta, "sharpness", 0.0f, kTriplanarSharpnessMax);
    else if (typeId == "TriplanarTexture" || typeId == "TriplanarSample")
        SetPortRange(meta, "tiling", -kUvTilingLimit, kUvTilingLimit);
    else if (typeId == "SampleTexture2DLOD")
        SetPortRange(meta, "lod", 0.0f, kMipLodMax);
    else if (typeId == "SampleTextureArray")
        SetPortRange(meta, "layer", 0.0f, kTextureArrayLayerMax);
    else if (typeId == "NormalStrength")
        SetPortRange(meta, "strength", 0.0f, kNormalStrengthMax);
    else if (typeId == "TransformUV")
    {
        SetPortRange(meta, "scale", -kUvTilingLimit, kUvTilingLimit);
        SetPortRange(meta, "offset", -kUvOffsetLimit, kUvOffsetLimit);
    }
    else if (typeId == "SurfaceOutput")
    {
        SetPortRange(meta, "Metallic", kNormalizedMin, kNormalizedMax);
        SetPortRange(meta, "Roughness", kNormalizedMin, kNormalizedMax);
    }
}

bool MaterialPaletteHasReflectedShaderGraphCategories(
    const std::vector<std::string>& categories)
{
    for (const std::string& category : categories)
    {
        if (category.find('/') != std::string::npos)
            return true;
    }
    return false;
}

void AttachKnownMaterialParameters(NodeTypeMeta& meta)
{
    if (!meta.Parameters.empty())
        return;

    if (meta.TypeId == "Time")
        meta.Parameters = {{"scale", 1.0f}, {"offset", 0.0f}};
    else if (meta.TypeId == "FloatConstant")
        meta.Parameters = {{"value", 0.0f}};
    else if (meta.TypeId == "Vec2Constant")
        meta.Parameters = {{"x", 0.0f}, {"y", 0.0f}};
    else if (meta.TypeId == "Vec3Constant")
        meta.Parameters = {{"x", 0.0f}, {"y", 0.0f}, {"z", 0.0f}};
    else if (meta.TypeId == "Vec4Constant")
        meta.Parameters = {{"x", 0.0f}, {"y", 0.0f}, {"z", 0.0f}, {"w", 1.0f}};
    else if (meta.TypeId == "ColorConstant")
        meta.Parameters = {{"r", 1.0f}, {"g", 1.0f}, {"b", 1.0f}};
    else if (meta.TypeId == "SampleTexture" || meta.TypeId == "TriplanarTexture")
        meta.Parameters = {{"texture", "albedoMap"}};
    else if (meta.TypeId == "SampleTextureArray")
        meta.Parameters = {{"texture", "textureArrayMap"}};
    else if (meta.TypeId == "SampleCubemap")
        meta.Parameters = {{"texture", "cubemap"}};
    else if (meta.TypeId == "CurveTexture")
        meta.Parameters = {{"texture", "albedoMap"},
                           {"row", 0.5f, {}, kNormalizedMin, kNormalizedMax}};
    else if (meta.TypeId == "Fresnel")
        meta.Parameters = {{"power", 5.0f, {}, kExponentMin, kExponentMax}};
    else if (meta.TypeId == "Compare")
        /* Option values are the GLSL selector constants SG_Compare branches on
           (Utility/Nodes.glsl): the compiler splices a parameter value straight
           into the call, so the wire format has to be a name the shader knows. */
        meta.Parameters = {{"function", "SG_COMPARE_GREATER",
                            {{"SG_COMPARE_GREATER", "Greater"},
                             {"SG_COMPARE_LESS", "Less"},
                             {"SG_COMPARE_EQUAL", "Equal"},
                             {"SG_COMPARE_NOT_EQUAL", "Not Equal"},
                             {"SG_COMPARE_GREATER_EQUAL", "Greater or Equal"},
                             {"SG_COMPARE_LESS_EQUAL", "Less or Equal"}}}};
    else if (meta.TypeId == "FloatParameter")
        meta.Parameters = {{"slot", 0},
                           {"component", "x", {{"x", "X"}, {"y", "Y"}, {"z", "Z"}, {"w", "W"}}},
                           {"variableName", "param"}};
    else if (meta.TypeId == "Vec2Parameter")
        meta.Parameters = {{"slot", 0},
                           {"swizzle", "xy",
                            {{"xy", "XY"}, {"yz", "YZ"}, {"zw", "ZW"},
                             {"xz", "XZ"}, {"xw", "XW"}, {"yw", "YW"}}},
                           {"variableName", "param"}};
    else if (meta.TypeId == "Vec3Parameter")
        meta.Parameters = {{"slot", 0},
                           {"swizzle", "xyz",
                            {{"xyz", "XYZ"}, {"yzw", "YZW"},
                             {"xyw", "XYW"}, {"xzw", "XZW"}}},
                           {"variableName", "param"}};
    else if (meta.TypeId == "Vec4Parameter")
        meta.Parameters = {{"slot", 0}, {"variableName", "param"}};
    else if (meta.TypeId == "ColorParameter")
        meta.Parameters = {{"slot", 0},
                           {"swizzle", "xyz",
                            {{"xyz", "XYZ"}, {"yzw", "YZW"},
                             {"xyw", "XYW"}, {"xzw", "XZW"}}},
                           {"variableName", "param"}};
}


} // namespace

void GraphNodeRegistry::RegisterMaterialConstantsAndOrganization()
{
    // The registry holds types; what a material type carries beyond its own
    // declaration is this catalogue's business, so it hands the registry the
    // two answers it will need and never has to be asked about a kind again.
    SetMetaDecorator(Graph::kKindIdMaterial,
                     [](NodeTypeMeta& meta)
                     {
                         AttachMaterialNodeIconStem(meta);
                         AttachKnownMaterialParameters(meta);
                         AttachKnownMaterialPortRanges(meta);
                     });
    SetTypeAliasResolver(Graph::kKindIdMaterial,
                         [](const std::string& typeId) -> std::string
                         {
                             const std::string resolved =
                                 ShaderGraph::ResolveNodeTypeAlias(typeId);
                             return resolved == typeId ? std::string() : resolved;
                         });

    Register(Graph::kKindIdMaterial,
             {"FloatConstant", "Float", "Input",
              {{"value", Graph::PortDirection::Out, "float", "Value"}}});

    Register(Graph::kKindIdMaterial,
             {"Vec2Constant", "Vector2", "Input",
              {{"value", Graph::PortDirection::Out, "float2", "Value"}}});

    Register(Graph::kKindIdMaterial,
             {"ColorConstant", "Color", "Input",
              {{"value", Graph::PortDirection::Out, "float3", "Color"}}});

    Register(Graph::kKindIdMaterial,
             {"Vec3Constant", "Vector3", "Input",
              {{"value", Graph::PortDirection::Out, "float3", "Value"}}});

    Register(Graph::kKindIdMaterial,
             {"Vec4Constant", "Vector4", "Input",
              {{"value", Graph::PortDirection::Out, "float4", "Value"}}});

    Register(Graph::kKindIdMaterial,
             {"FloatParameter", "Float Parameter", "Parameters",
              {{"value", Graph::PortDirection::Out, "float", "Value"}}});

    Register(Graph::kKindIdMaterial,
             {"Vec2Parameter", "Vector2 Parameter", "Parameters",
              {{"value", Graph::PortDirection::Out, "float2", "Value"}}});

    Register(Graph::kKindIdMaterial,
             {"Vec3Parameter", "Vector3 Parameter", "Parameters",
              {{"value", Graph::PortDirection::Out, "float3", "Value"}}});

    Register(Graph::kKindIdMaterial,
             {"Vec4Parameter", "Vector4 Parameter", "Parameters",
              {{"value", Graph::PortDirection::Out, "float4", "Value"}}});

    Register(Graph::kKindIdMaterial,
             {"ColorParameter", "Color Parameter", "Parameters",
              {{"value", Graph::PortDirection::Out, "float3", "Color"}}});

    Register(Graph::kKindIdMaterial,
             {"UV", "UV", "Input",
              {{"out", Graph::PortDirection::Out, "float2", "UV"}}});

    Register(Graph::kKindIdMaterial,
             {"UV1", "UV1", "Input",
              {{"out", Graph::PortDirection::Out, "float2", "UV1"}}});

    Register(Graph::kKindIdMaterial,
             {"NormalVector", "Normal", "Input",
              {{"normal", Graph::PortDirection::Out, "float3", "Normal"}}});

    Register(Graph::kKindIdMaterial,
             {"ViewDirection", "View", "Input",
              {{"view", Graph::PortDirection::Out, "float3", "View"}}});

    Register(Graph::kKindIdMaterial,
             {"WorldPosition", "World Position", "Input",
              {{"out", Graph::PortDirection::Out, "float3", "Position"}}});

    Register(Graph::kKindIdMaterial,
             {"Time", "Time", "Input",
              {{"out", Graph::PortDirection::Out, "float", "Time"}}});

    Register(Graph::kKindIdMaterial,
             {"ScreenUV", "Screen UV", "Input",
              {{"out", Graph::PortDirection::Out, "float2", "Screen UV"}}});

    Register(Graph::kKindIdMaterial,
             {"VertexColor", "Vertex Color", "Input",
              {{"out", Graph::PortDirection::Out, "float4", "Color"}}});

    Register(Graph::kKindIdMaterial,
             {"Tangent", "Tangent", "Input",
              {{"out", Graph::PortDirection::Out, "float3", "Tangent"}}});

    Register(Graph::kKindIdMaterial,
             {"Group", "Group", "Organization", {}});

    Register(Graph::kKindIdMaterial,
             {"Comment", "Comment", "Organization", {}});
}

void GraphNodeRegistry::RegisterMaterialTypesFromShaderGraph()
{
    using namespace ShaderGraph;

    const SgNodeLibraryIndex& index = GetSharedNodeLibraryIndex();

    for (const auto& entry : index.NodesByType)
    {
        const SgNodeDefinition& def = entry.second;
        if (def.Overloads.empty())
            continue;

        const SgNodeOverload& overload = def.Overloads.front();
        std::vector<NodePortTemplate> ports;
        ports.reserve(overload.Inputs.size() + overload.Outputs.size());
        for (const SgPortMeta& port : overload.Inputs)
            ports.push_back({port.Name, Graph::PortDirection::In, port.GraphType, port.DisplayName});
        for (const SgPortMeta& port : overload.Outputs)
            ports.push_back({port.Name, Graph::PortDirection::Out, port.GraphType, port.DisplayName});

        Register(Graph::kKindIdMaterial, {def.TypeId, def.DisplayName, def.Category, std::move(ports)});
    }

    if (const NodeTypeMeta* sample2d = Find(Graph::kKindIdMaterial, "SampleTexture2D"))
    {
        NodeTypeMeta alias = *sample2d;
        alias.TypeId = "SampleTexture";
        /* Lookup alias for graphs saved with the old TypeId. No category:
           the palette and menus list by category, so the alias never shows
           beside SampleTexture2D as a duplicate entry. */
        alias.Category.clear();
        Register(Graph::kKindIdMaterial, std::move(alias));
    }
}

void GraphNodeRegistry::RegisterMaterialTypes()
{
    RegisterMaterialConstantsAndOrganization();
    RegisterMaterialTypesFromShaderGraph();
    for (const NodeIconStemEntry& entry : kMaterialCategoryIconStems)
        RegisterCategoryIcon(Graph::kKindIdMaterial, entry.TypeId, entry.Stem);
}

void GraphNodeRegistry::EnsureMaterialShaderGraphTypesRegistered()
{
    constexpr size_t kMinMaterialPaletteTypes = 50;
    const std::vector<std::string> categories = GetCategories(Graph::kKindIdMaterial);
    const size_t typeCount = GetAllTypes(Graph::kKindIdMaterial).size();
    if (typeCount >= kMinMaterialPaletteTypes &&
        MaterialPaletteHasReflectedShaderGraphCategories(categories))
    {
        return;
    }

    using namespace ShaderGraph;
    InvalidateSharedNodeLibraryIndex();

    const std::filesystem::path nodesRoot = GetEngineGraphNodesRoot();
    std::error_code ec;
    if (nodesRoot.empty())
    {
        // The resolver found nothing at any exe-anchored layout, so it has no
        // path to name. Say where the tree was expected and what puts it there.
        Logger::Log::Warning(
            "GraphNodeRegistry: no shader graph nodes root beside this executable "
            "(expected '<exe dir>/Assets/Shaders/Graph/Nodes') — material palette will be "
            "incomplete. The Editor stages it via StageEditorAssets; a test executable via "
            "ge_stage_shader_graph_nodes().");
    }
    else if (!std::filesystem::exists(nodesRoot, ec))
    {
        Logger::Log::Warning(
            "GraphNodeRegistry: shader graph nodes root not found at '{}' — material palette will be incomplete.",
            nodesRoot.string());
    }

    const size_t beforeCount = GetAllTypes(Graph::kKindIdMaterial).size();
    RegisterMaterialTypesFromShaderGraph();
    const size_t afterCount = GetAllTypes(Graph::kKindIdMaterial).size();

    if (afterCount < kMinMaterialPaletteTypes)
    {
        Logger::Log::Warning(
            "GraphNodeRegistry: expected ~80 material nodes but only registered {} (was {}). Nodes root: '{}'",
            afterCount,
            beforeCount,
            nodesRoot.string());
    }
}


} // namespace GameEngine
