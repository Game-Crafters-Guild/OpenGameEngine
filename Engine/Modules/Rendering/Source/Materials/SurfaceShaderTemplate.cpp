#include "Rendering/Materials/SurfaceShaderTemplate.h"
#include "Rendering/Materials/MaterialDocument.h"

namespace GameEngine
{
namespace Rendering
{

std::string MakeSurfaceShaderTemplateSource()
{
    return
        "// Surface shader (created by Create → Surface Shader).\n"
        "//\n"
        "// Contract: SurfaceOutput EvaluateSurface(SurfaceInput). The engine's forward\n"
        "// adapter wraps this file with lighting (Forward+, shadows, IBL) and the\n"
        "// OpenPBR BRDF — you only describe the surface. This file compiles and\n"
        "// renders as-is; edit and save to hot-reload every material that uses it.\n"
        "//\n"
        "// Guide:  Engine/Modules/Rendering/docs/SurfaceShaderAuthoring.html\n"
        "// Structs: Engine/Modules/Rendering/Shaders/Includes/surface_io.glsl\n"
        "//\n"
        "// Rules: no #version, no layout()/bindings here — the adapter owns those.\n"
        "// Read parameters via Props.<name>; sample textures via declared names only.\n"
        "\n"
        "// --- Declared textures ---\n"
        "// albedoMap is a WELL-KNOWN name (keeps bindless slot 0, sampled by name).\n"
        "// accentMask is a USER name — packed into the lowest free slot and sampled\n"
        "// via GE_USER_TEXTURE(accentMask). Unassigned slots fall back to 1x1\n"
        "// defaults, so this renders before any texture is hooked up.\n"
        "// @texture albedoMap  srgb\n"
        "// @texture accentMask linear\n"
        "\n"
        "// --- Declared properties ---\n"
        "// One line per parameter: type, name, optional \"Display Name\", then\n"
        "// default= range= group= tooltip=. The name is the .material key, the\n"
        "// Props.<name> spelling below and the inspector row; a range makes a\n"
        "// slider, a group a collapsible section. The engine packs the storage.\n"
        "// @property color baseColor   \"Base Color\"    default=1,1,1,1\n"
        "// @property float metallic    \"Metallic\"      default=0 range=0,1\n"
        "// @property float roughness   \"Roughness\"     default=0.5 range=0,1\n"
        "// @property float alphaCutoff \"Alpha Cutoff\"  default=0.5 range=0,1 visibleIf=\"alphaMode=Mask\" tooltip=\"Mask alpha mode discards fragments whose opacity falls below this\"\n"
        "// @property color accentColor \"Accent Color\"  default=0.25,0.85,1 group=Accent\n"
        "// @property float accentNits  \"Accent (nits)\" default=406 range=0,2000 group=Accent tooltip=\"emission strength; 203 nits is reference white\"\n"
        "// @property float pulseSpeed  \"Pulse Speed\"   default=2 range=0,10 group=Accent tooltip=\"radians per second, used by the PULSE keyword\"\n"
        "\n"
        "SurfaceOutput EvaluateSurface(SurfaceInput sIn)\n"
        "{\n"
        "    SurfaceOutput o = DefaultSurfaceOutput();\n"
        "\n"
        "    // Slot-0 tiling/offset authored on the material.\n"
        "    vec2 uv = GE_TransformUV(sIn.uv0, sIn.textureST[0], sIn.textureST2[0]);\n"
        "\n"
        "    vec4 albedo = texture(albedoMap, uv);\n"
        "    o.baseColor = albedo.rgb * Props.baseColor.rgb * sIn.vertexColor.rgb;\n"
        "    o.metallic  = Props.metallic;\n"
        "    o.roughness = Props.roughness;\n"
        "    o.normalWS  = normalize(sIn.normalWS);\n"
        "    o.opacity   = albedo.a * Props.baseColor.a;\n"
        "\n"
        "#ifdef GE_USER_PULSE\n"
        "    // User keyword: \"keywords\": [\"PULSE\"] in the .material becomes the\n"
        "    // GE_USER_PULSE define. Light.uTimeParams.x is the always-bound\n"
        "    // elapsed-seconds clock (uTimeParams.y is a bounded scroll clock for\n"
        "    // UV panners).\n"
        "    float pulse = 0.5 + 0.5 * sin(Light.uTimeParams.x * Props.pulseSpeed);\n"
        "#else\n"
        "    float pulse = 1.0;\n"
        "#endif\n"
        "\n"
        "    // Emission is physical: scene-linear 1.0 == 203 nits (reference white),\n"
        "    // so the accent strength is authored in nits and mapped through the\n"
        "    // anchor rather than guessed display-referred.\n"
        "    float mask = texture(GE_USER_TEXTURE(accentMask), uv).r;\n"
        "    o.emissive = Props.accentColor * (mask * pulse * Props.accentNits / GE_EMISSION_PAPERWHITE_NITS);\n"
        "\n"
        "    return o;\n"
        "}\n";
}

MaterialDocument MakeSurfaceShaderTemplateMaterial(const std::string& stem)
{
    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = stem;
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = stem + ".glsl"; // sibling file: material-dir-first resolution
    doc.keywords = {"PULSE"};
    // No property values: every parameter renders at the default its @property
    // line declares, and the .material stores overrides only.
    // Seed the texture keys the template samples: the inspector surfaces a
    // user-declared slot as an assignable row only when the key exists in the
    // document (empty ref = unassigned, 1x1 default at runtime).
    doc.textures["albedoMap"] = "";
    doc.textures["accentMask"] = "";
    return doc;
}

} // namespace Rendering
} // namespace GameEngine
