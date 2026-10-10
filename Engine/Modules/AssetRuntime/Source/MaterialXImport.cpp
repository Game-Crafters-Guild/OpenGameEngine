#include "Assets/MaterialXImport.h"
#include "Assets/AssetRegistry.h"

#include "AssetCore/GUID.h"
#include "Logger/Logger.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace GameEngine
{

// OpenPBR expresses anisotropy rotation in TURNS [0..1]; our uParams11.y consumes radians.
// Shared by both the import (turns -> radians) and write-back (radians -> turns) anon namespaces.
constexpr float kTwoPi = 6.28318530718f;

namespace
{

// A lobe is treated as "on" when its OpenPBR weight exceeds this; our StandardPBR lobes are
// keyword-gated booleans while OpenPBR weights are always-on continuous, so the weight drives
// the enableX flag (and is also kept as the lobe strength where we have one).
constexpr float kLobeEnableEpsilon = 1.0e-4f;

// Depth-first search for the first <open_pbr_surface ...> element (the OpenPBR Surface shader
// node; in MaterialX the element name IS the node category). Handles it being nested in a
// <nodegraph> or referenced from a <surfacematerial>; the flat single-material case is the norm.
pugi::xml_node FindOpenPbrSurface(const pugi::xml_node& node)
{
    for (pugi::xml_node child : node.children())
    {
        if (std::string(child.name()) == "open_pbr_surface")
            return child;
        if (pugi::xml_node found = FindOpenPbrSurface(child))
            return found;
    }
    return {};
}

// An input is graph-driven (fed by a node/nodegraph, e.g. a texture) rather than a literal value
// when it carries any of these connection attributes. Single source for the connection-form list so
// the constant reader, the texture-trace reader, and the writer's preserve guard cannot drift.
bool IsGraphDriven(const pugi::xml_node& in)
{
    return in.attribute("nodename") || in.attribute("nodegraph") ||
           in.attribute("output") || in.attribute("interfacename");
}

// The constant value of a named input, or nullopt when the input is absent OR is driven by a
// node graph (nodename=/output=) rather than a literal — graph-driven inputs (textures) are a
// later phase, so P0 leaves them at the engine default and logs them.
std::optional<std::string> ConstantInput(const pugi::xml_node& shader, const char* name,
                                         std::vector<std::string>& graphDriven)
{
    for (pugi::xml_node in : shader.children("input"))
    {
        if (std::string(in.attribute("name").value()) != name)
            continue;
        if (IsGraphDriven(in))
        {
            graphDriven.emplace_back(name);
            return std::nullopt;
        }
        if (pugi::xml_attribute v = in.attribute("value"))
            return std::string(v.value());
        return std::nullopt;
    }
    return std::nullopt;
}

float ParseFloat(const std::string& s, float def)
{
    try { return std::stof(s); } catch (...) { return def; }
}

// MaterialX color3/vector values are comma-separated ("0.8, 0.2, 0.1").
std::vector<float> ParseColor(const std::string& s, size_t comps)
{
    std::vector<float> out;
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ','))
    {
        try { out.push_back(std::stof(tok)); } catch (...) { out.push_back(0.0f); }
    }
    out.resize(comps, out.empty() ? 0.0f : out.back());
    return out;
}

// Read a scalar input with a spec default.
float FloatInput(const pugi::xml_node& shader, const char* name, float def,
                 std::vector<std::string>& graphDriven)
{
    auto v = ConstantInput(shader, name, graphDriven);
    return v ? ParseFloat(*v, def) : def;
}

// Read a color3 input with a uniform-grey spec default.
std::vector<float> ColorInput(const pugi::xml_node& shader, const char* name, float def,
                              std::vector<std::string>& graphDriven)
{
    auto v = ConstantInput(shader, name, graphDriven);
    return v ? ParseColor(*v, 3) : std::vector<float>{def, def, def};
}

// --- Texture graph tracing -------------------------------------------------
// A surface input can be driven by an <image> node, possibly through a node graph and a few
// pass-through nodes (normalmap/convert). We trace the link back to the backing image's `file`.

constexpr int kMaxGraphDepth = 16;

bool IsImageNode(const pugi::xml_node& n)
{
    const std::string cat = n.name();
    return cat == "image" || cat == "tiledimage";
}

// The `file` filename input value of an <image>/<tiledimage> node.
std::optional<std::string> ImageFile(const pugi::xml_node& imageNode)
{
    for (pugi::xml_node in : imageNode.children("input"))
    {
        if (std::string(in.attribute("name").value()) == "file")
            if (pugi::xml_attribute v = in.attribute("value"))
                return std::string(v.value());
    }
    return std::nullopt;
}

// Find a node element (any category) named @p name directly under @p scope.
pugi::xml_node FindNodeByName(const pugi::xml_node& scope, const std::string& name)
{
    for (pugi::xml_node child : scope.children())
        if (std::string(child.attribute("name").value()) == name)
            return child;
    return {};
}

// Walk from a node (by name, within @p scope) to its backing <image>, following the first
// upstream nodename through pass-through nodes. Bounded depth + visited-set so a malformed
// graph can neither recurse without limit nor cycle.
std::optional<std::string> WalkToImage(const pugi::xml_node& scope, const std::string& nodeName,
                                       int depth, std::set<std::string>& visited)
{
    if (depth <= 0 || nodeName.empty() || !visited.insert(nodeName).second)
        return std::nullopt;
    pugi::xml_node n = FindNodeByName(scope, nodeName);
    if (!n)
        return std::nullopt;
    if (IsImageNode(n))
        return ImageFile(n);
    for (pugi::xml_node in : n.children("input"))
        if (pugi::xml_attribute nn = in.attribute("nodename"))
            if (auto file = WalkToImage(scope, nn.value(), depth - 1, visited))
                return file;
    return std::nullopt;
}

// The surface <input> element named @p name, but only when it is graph-driven (a texture link);
// an empty node otherwise (absent, or a constant value handled by ConstantInput).
pugi::xml_node GraphInput(const pugi::xml_node& surf, const char* name)
{
    for (pugi::xml_node in : surf.children("input"))
    {
        if (std::string(in.attribute("name").value()) != name)
            continue;
        if (IsGraphDriven(in))
            return in;
        return {};
    }
    return {};
}

// Resolve a graph-driven surface input to the file path of its backing <image>, handling the
// two common link forms: a direct nodename to a sibling image/pass-through chain, and a
// nodegraph+output reference resolved through the graph's <output>.
std::optional<std::string> ResolveImageFile(const pugi::xml_node& root, const pugi::xml_node& surf,
                                            const pugi::xml_node& input)
{
    std::set<std::string> visited;
    if (pugi::xml_attribute ng = input.attribute("nodegraph"))
    {
        pugi::xml_node graph = FindNodeByName(root, ng.value());
        if (!graph || std::string(graph.name()) != "nodegraph")
            return std::nullopt;
        const std::string wantOut = input.attribute("output") ? input.attribute("output").value() : std::string();
        pugi::xml_node out;
        for (pugi::xml_node o : graph.children("output"))
        {
            if (wantOut.empty() || std::string(o.attribute("name").value()) == wantOut)
            {
                out = o;
                break;
            }
        }
        if (out)
            if (pugi::xml_attribute nn = out.attribute("nodename"))
                return WalkToImage(graph, nn.value(), kMaxGraphDepth, visited);
        return std::nullopt;
    }
    if (pugi::xml_attribute nn = input.attribute("nodename"))
        return WalkToImage(surf.parent(), nn.value(), kMaxGraphDepth, visited);
    return std::nullopt;
}

struct MtlxTextureBinding
{
    std::string slot; // our texture slot name (e.g. "albedoMap")
    std::string file; // authored image path, relative to the .mtlx
};
struct MtlxTextures
{
    std::vector<MtlxTextureBinding> bindings;
    std::vector<std::string> consumedInputs; // surface inputs resolved (so they leave the graph-driven log)
    bool needsExtendedShader = false;        // separate rough/metal + the coat normal live on the extended surface (slots 5-7)
    bool hasCoatNormal = false;              // geometry_coat_normal traced -> compile in the CoatNormal keyword
};

// Map the open_pbr_surface's texture-driven inputs onto our texture slots, tracing each to its
// backing <image> file. A single packed file shared by roughness+metalness maps onto the combined
// metallicRoughnessMap (glTF G=roughness, B=metallic, base shader); separate files require the
// extended surface's discrete roughnessMap/metallicMap slots. The authored (relative) path is
// returned; callers resolve it against the .mtlx directory.
MtlxTextures TraceSurfaceTextures(const pugi::xml_node& root, const pugi::xml_node& surf)
{
    auto file = [&](const char* inputName) -> std::string {
        pugi::xml_node in = GraphInput(surf, inputName);
        if (!in)
            return {};
        auto f = ResolveImageFile(root, surf, in);
        return (f && !f->empty()) ? *f : std::string();
    };

    MtlxTextures out;

    if (std::string f = file("base_color"); !f.empty())
    {
        out.bindings.push_back({"albedoMap", f});
        out.consumedInputs.push_back("base_color");
    }

    const char* normalInput = "geometry_normal";
    std::string nf = file(normalInput);
    if (nf.empty())
    {
        normalInput = "normal";
        nf = file(normalInput);
    }
    if (!nf.empty())
    {
        out.bindings.push_back({"normalMap", nf});
        out.consumedInputs.push_back(normalInput);
    }

    // OpenPBR geometry_coat_normal -> our clear-coat normal slot (slot 5, extended surface).
    // Maps 1:1 with geometry_normal above but drives the coat lobe instead of the base.
    if (std::string cnf = file("geometry_coat_normal"); !cnf.empty())
    {
        out.bindings.push_back({"coatNormalMap", cnf});
        out.consumedInputs.push_back("geometry_coat_normal");
        out.needsExtendedShader = true; // coatNormalMap is binding 6 -> requires the extended surface
        out.hasCoatNormal = true;
    }

    if (std::string f = file("emission_color"); !f.empty())
    {
        out.bindings.push_back({"emissiveMap", f});
        out.consumedInputs.push_back("emission_color");
    }

    std::string rf = file("specular_roughness");
    std::string mf = file("base_metalness");
    if (!rf.empty() && rf == mf)
    {
        out.bindings.push_back({"metallicRoughnessMap", rf});
        out.consumedInputs.push_back("specular_roughness");
        out.consumedInputs.push_back("base_metalness");
    }
    else if (!rf.empty() || !mf.empty())
    {
        out.needsExtendedShader = true;
        if (!rf.empty())
        {
            out.bindings.push_back({"roughnessMap", rf});
            out.consumedInputs.push_back("specular_roughness");
        }
        if (!mf.empty())
        {
            out.bindings.push_back({"metallicMap", mf});
            out.consumedInputs.push_back("base_metalness");
        }
    }
    return out;
}

} // namespace

bool ParseMaterialX(const std::string& xml, const std::filesystem::path& mtlxPath,
                    AssetRegistry* registry, MaterialDocument& outDoc, std::vector<std::string>& errors)
{
    pugi::xml_document xmlDoc;
    pugi::xml_parse_result res = xmlDoc.load_buffer(xml.data(), xml.size());
    if (!res)
    {
        errors.push_back(std::string("MaterialX: XML parse error: ") + res.description());
        return false;
    }

    pugi::xml_node root = xmlDoc.child("materialx");
    if (!root)
    {
        errors.push_back("MaterialX: missing <materialx> root element.");
        return false;
    }

    pugi::xml_node surf = FindOpenPbrSurface(root);
    if (!surf)
    {
        errors.push_back("MaterialX: no <open_pbr_surface> node found (only OpenPBR Surface is supported).");
        return false;
    }

    std::vector<std::string> graphDriven; // inputs we skipped because a graph drives them (textures)

    // The produced material is an ordinary StandardPBR material with the shader locked.
    outDoc = MaterialDocument{};
    outDoc.schemaVersion = 3;
    outDoc.lightingModel = "StandardPBR";
    outDoc.surfaceShader = "Surfaces/standard_pbr.glsl";
    outDoc.shaderLocked = true;
    if (pugi::xml_attribute nm = surf.attribute("name"))
        outDoc.materialName = nm.value();

    auto& p = outDoc.properties;

    // --- Base ---
    float baseWeight = FloatInput(surf, "base_weight", 1.0f, graphDriven);
    std::vector<float> baseColor = ColorInput(surf, "base_color", 0.8f, graphDriven);
    float opacity = FloatInput(surf, "geometry_opacity", 1.0f, graphDriven);
    p["baseColor"] = std::vector<float>{baseWeight * baseColor[0], baseWeight * baseColor[1],
                                        baseWeight * baseColor[2], opacity};
    p["metallic"] = FloatInput(surf, "base_metalness", 0.0f, graphDriven);
    p["diffuseRoughness"] = FloatInput(surf, "base_diffuse_roughness", 0.0f, graphDriven);

    // --- Specular (always-on base) ---
    p["specularWeight"] = FloatInput(surf, "specular_weight", 1.0f, graphDriven);
    p["specularColor"] = ColorInput(surf, "specular_color", 1.0f, graphDriven);
    p["roughness"] = FloatInput(surf, "specular_roughness", 0.3f, graphDriven);
    p["specularIor"] = FloatInput(surf, "specular_ior", 1.5f, graphDriven);

    // --- Anisotropy ---
    float aniso = FloatInput(surf, "specular_roughness_anisotropy", 0.0f, graphDriven);
    p["enableAnisotropy"] = aniso > kLobeEnableEpsilon;
    p["anisotropy"] = aniso; // OpenPBR [0..1] magnitude maps onto our signed [-1..1] positive side
    // OpenPBR expresses the rotation in TURNS [0..1]; our uParams11.y consumes radians.
    p["anisotropyRotation"] =
        FloatInput(surf, "specular_roughness_anisotropy_rotation", 0.0f, graphDriven) * kTwoPi;

    // --- Transmission (glass) ---
    float transW = FloatInput(surf, "transmission_weight", 0.0f, graphDriven);
    bool transOn = transW > kLobeEnableEpsilon;
    p["enableTransmission"] = transOn;
    p["transmissionWeight"] = transW;
    p["transmissionColor"] = ColorInput(surf, "transmission_color", 1.0f, graphDriven);
    // OpenPBR thin_walled=false (its default) describes a volumetric interior, which maps onto our
    // "thick" two-surface refraction path; thin_walled=true is the single-interface thin path.
    auto thinWalledVal = ConstantInput(surf, "thin_walled", graphDriven);
    bool thinWalled = thinWalledVal && (*thinWalledVal == "true" || *thinWalledVal == "1");
    p["enableTransmissionThick"] = transOn && !thinWalled;
    // OpenPBR transmission_color IS the Beer–Lambert volume colour reached after travelling
    // transmission_depth (world units) through the medium — a direct mapping onto our thick-glass
    // attenuation pair (no unit conversion). transmission_depth 0 (spec default) = absorption off.
    p["attenuationColor"] = ColorInput(surf, "transmission_color", 1.0f, graphDriven);
    p["attenuationDistance"] = FloatInput(surf, "transmission_depth", 0.0f, graphDriven);

    // --- Subsurface (single scalar thickness vs OpenPBR's RGB radius — degrade) ---
    float subW = FloatInput(surf, "subsurface_weight", 0.0f, graphDriven);
    p["enableSubsurface"] = subW > kLobeEnableEpsilon;
    p["subsurfaceColor"] = ColorInput(surf, "subsurface_color", 0.8f, graphDriven);

    // --- Coat (clearcoat) ---
    float coatW = FloatInput(surf, "coat_weight", 0.0f, graphDriven);
    bool coatOn = coatW > kLobeEnableEpsilon;
    p["enableClearCoat"] = coatOn;
    p["clearCoat"] = coatW;
    p["clearCoatRoughness"] = FloatInput(surf, "coat_roughness", 0.0f, graphDriven);
    p["clearCoatIor"] = FloatInput(surf, "coat_ior", 1.5f, graphDriven);
    // OpenPBR coat_darkening defaults to 1.0 (full physical darkening) — honour that when the coat
    // is present rather than our material's inert 0.0 default, or coated imports look wrong.
    p["coatDarkening"] = coatOn ? FloatInput(surf, "coat_darkening", 1.0f, graphDriven) : 0.0f;
    p["coatColor"] = ColorInput(surf, "coat_color", 1.0f, graphDriven); // OpenPBR coat medium tint (default white)
    p["enableCoatNormal"] = false; // set true below iff geometry_coat_normal traced to an image (slot 5)

    // --- Fuzz — OpenPBR's outermost retroreflective layer, which sits OVER the coat. It maps to
    //     our distinct over-coat Fuzz lobe (NOT the under-coat Sheen lobe, which stays for glTF
    //     KHR_materials_sheen / .material "sheen" authoring). Colour magnitude is the strength, so
    //     fold the weight into the colour. ---
    float fuzzW = FloatInput(surf, "fuzz_weight", 0.0f, graphDriven);
    std::vector<float> fuzzColor = ColorInput(surf, "fuzz_color", 1.0f, graphDriven);
    p["enableFuzz"] = fuzzW > kLobeEnableEpsilon;
    p["fuzzColor"] = std::vector<float>{fuzzW * fuzzColor[0], fuzzW * fuzzColor[1], fuzzW * fuzzColor[2]};
    p["fuzzRoughness"] = FloatInput(surf, "fuzz_roughness", 0.3f, graphDriven);

    // --- Thin-film iridescence. OpenPBR thin_film_thickness is in MICROMETRES; our field is in
    //     nanometres, so convert (x1000). IORs are unitless and map directly. ---
    float tfW = FloatInput(surf, "thin_film_weight", 0.0f, graphDriven);
    p["enableIridescence"] = tfW > kLobeEnableEpsilon;
    p["thinFilmWeight"] = tfW;
    p["thinFilmThickness"] = FloatInput(surf, "thin_film_thickness", 0.5f, graphDriven) * 1000.0f; // um -> nm
    p["thinFilmIor"] = FloatInput(surf, "thin_film_ior", 1.5f, graphDriven);

    // --- Emission: emission_color x emission_luminance (nits, direct to our emissionLuminance) ---
    p["emissive"] = ColorInput(surf, "emission_color", 1.0f, graphDriven);
    p["emissionLuminance"] = FloatInput(surf, "emission_luminance", 0.0f, graphDriven);

    // --- Textures: trace graph-driven inputs to their backing <image> files, resolve to GUIDs ---
    // The constant pass above already logged these inputs as graph-driven; resolved ones are
    // removed from that list so the trailing "not imported" log only names what we truly skipped.
    MtlxTextures tex = TraceSurfaceTextures(root, surf);
    if (tex.needsExtendedShader)
        outDoc.surfaceShader = "Surfaces/standard_pbr_extended.glsl";
    if (tex.hasCoatNormal)
        p["enableCoatNormal"] = true; // compile in the CoatNormal keyword (shades the coat lobe about coatNormalMap)
    for (const MtlxTextureBinding& b : tex.bindings)
    {
        const std::string absPath = (mtlxPath.parent_path() / b.file).lexically_normal().string();
        std::string ref = absPath;
        if (registry)
        {
            GUID guid = registry->GetOrCreateAssetGUID(absPath);
            if (!guid.IsNull())
                ref = guid.ToString();
            else
                Logger::Log::Info("MaterialX '{}': texture not in asset DB, storing path: {}",
                                  outDoc.materialName, absPath);
        }
        outDoc.textures[b.slot] = ref;

        // Neutralise the matching constant factor so the texture (not the default) drives the channel.
        if (b.slot == "albedoMap")
            p["baseColor"] = std::vector<float>{baseWeight, baseWeight, baseWeight, opacity};
        else if (b.slot == "emissiveMap")
            p["emissive"] = std::vector<float>{1.0f, 1.0f, 1.0f}; // the texture is emission_color; luminance still scales it
        else if (b.slot == "metallicRoughnessMap")
        {
            p["roughness"] = 1.0f;
            p["metallic"] = 1.0f;
        }
        else if (b.slot == "roughnessMap")
            p["roughness"] = 1.0f;
        else if (b.slot == "metallicMap")
            p["metallic"] = 1.0f;
    }
    for (const std::string& in : tex.consumedInputs)
        graphDriven.erase(std::remove(graphDriven.begin(), graphDriven.end(), in), graphDriven.end());

    // Representability gap: the extended surface samples metallicMap with a NON-METAL (black) default,
    // so a roughness-only texture cannot also carry a uniform non-zero metalness — it would render
    // dielectric. (The reverse — a metalness texture + constant roughness — is fine: roughnessMap
    // defaults white, so the constant survives via its factor.) Warn rather than fail silently.
    {
        bool boundRough = false, boundMetal = false;
        for (const MtlxTextureBinding& b : tex.bindings)
        {
            boundRough = boundRough || b.slot == "roughnessMap";
            boundMetal = boundMetal || b.slot == "metallicMap";
        }
        float metalConst = 0.0f;
        if (const auto* mv = std::get_if<float>(&p["metallic"]))
            metalConst = *mv;
        if (boundRough && !boundMetal && metalConst > kLobeEnableEpsilon)
            Logger::Log::Warning(
                "MaterialX '{}': roughness texture with uniform metalness {} and no metalness map — the "
                "extended surface reads metalness from a non-metal default, so it renders ~0. Author a "
                "metalness map (or a packed metallic-roughness texture) to keep the metalness.",
                outDoc.materialName, metalConst);
    }

    if (!graphDriven.empty())
    {
        std::string list;
        for (size_t i = 0; i < graphDriven.size(); ++i)
            list += (i ? ", " : "") + graphDriven[i];
        Logger::Log::Info("MaterialX '{}': {} graph-driven/texture input(s) not imported in P0 (left at default): {}",
                          outDoc.materialName, graphDriven.size(), list);
    }
    return true;
}

std::vector<std::string> CollectMaterialXTextureFiles(const std::string& xml)
{
    std::vector<std::string> files;
    pugi::xml_document xmlDoc;
    if (!xmlDoc.load_buffer(xml.data(), xml.size()))
        return files;
    pugi::xml_node root = xmlDoc.child("materialx");
    if (!root)
        return files;
    pugi::xml_node surf = FindOpenPbrSurface(root);
    if (!surf)
        return files;
    for (const MtlxTextureBinding& b : TraceSurfaceTextures(root, surf).bindings)
        files.push_back(b.file);
    return files;
}

namespace
{

float DocFloat(const MaterialDocument& doc, const char* key, float def)
{
    auto it = doc.properties.find(key);
    if (it == doc.properties.end())
        return def;
    if (auto* f = std::get_if<float>(&it->second))
        return *f;
    if (auto* i = std::get_if<int>(&it->second))
        return static_cast<float>(*i);
    return def;
}

bool DocBool(const MaterialDocument& doc, const char* key)
{
    auto it = doc.properties.find(key);
    if (it != doc.properties.end())
        if (auto* b = std::get_if<bool>(&it->second))
            return *b;
    return false;
}

std::vector<float> DocColor(const MaterialDocument& doc, const char* key)
{
    auto it = doc.properties.find(key);
    if (it != doc.properties.end())
        if (auto* v = std::get_if<std::vector<float>>(&it->second))
            return *v;
    return {1.0f, 1.0f, 1.0f};
}

std::string FmtF(float v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(v));
    return buf;
}

std::string FmtC(const std::vector<float>& c)
{
    float r = c.size() > 0 ? c[0] : 0.0f;
    float g = c.size() > 1 ? c[1] : 0.0f;
    float b = c.size() > 2 ? c[2] : 0.0f;
    return FmtF(r) + ", " + FmtF(g) + ", " + FmtF(b);
}

// Update-or-add a constant input on the shader node; never overwrite a graph-driven input
// (texture link), so re-saving a material whose base_color is texture-fed keeps the texture.
void SetInput(pugi::xml_node shader, const char* name, const char* type, const std::string& value)
{
    for (pugi::xml_node in : shader.children("input"))
    {
        if (std::string(in.attribute("name").value()) != name)
            continue;
        if (IsGraphDriven(in))
            return; // graph-driven -> preserve
        if (pugi::xml_attribute a = in.attribute("value"))
            a.set_value(value.c_str());
        else
            in.append_attribute("value") = value.c_str();
        return;
    }
    pugi::xml_node in = shader.append_child("input");
    in.append_attribute("name") = name;
    in.append_attribute("type") = type;
    in.append_attribute("value") = value.c_str();
}

// The set of input names already present on the surface node in the source document. Used by the
// MaybeSet* writers to preserve an author's explicit value even when it equals the spec default.
std::set<std::string> CollectPresentInputs(const pugi::xml_node& surf)
{
    std::set<std::string> present;
    for (pugi::xml_node in : surf.children("input"))
        present.insert(in.attribute("name").value());
    return present;
}

// Write a float input only when it carries information: it was present in the source, OR (when the
// lobe is active) its value differs from the OpenPBR spec default. Inputs equal to the spec default
// are omitted — a compliant reader fills the default — so a minimal .mtlx round-trips minimal.
void MaybeSetFloat(pugi::xml_node surf, const std::set<std::string>& present, const char* name,
                   float value, float specDefault, bool active = true)
{
    if (present.count(name) || (active && std::fabs(value - specDefault) > kLobeEnableEpsilon))
        SetInput(surf, name, "float", FmtF(value));
}

void MaybeSetColor(pugi::xml_node surf, const std::set<std::string>& present, const char* name,
                   const std::vector<float>& value, float d0, float d1, float d2, bool active = true)
{
    auto comp = [&](size_t i) { return i < value.size() ? value[i] : 0.0f; };
    const bool differs = std::fabs(comp(0) - d0) > kLobeEnableEpsilon ||
                         std::fabs(comp(1) - d1) > kLobeEnableEpsilon ||
                         std::fabs(comp(2) - d2) > kLobeEnableEpsilon;
    if (present.count(name) || (active && differs))
        SetInput(surf, name, "color3", FmtC(value));
}

} // namespace

bool WriteMaterialX(const std::string& mtlxPath, const MaterialDocument& doc)
{
    pugi::xml_document xmlDoc;
    if (!xmlDoc.load_file(mtlxPath.c_str()))
        return false;
    pugi::xml_node root = xmlDoc.child("materialx");
    if (!root)
        return false;
    pugi::xml_node surf = FindOpenPbrSurface(root);
    if (!surf)
        return false;

    // Only write inputs that carry information (present in source, or differing from the OpenPBR
    // spec default), so a minimal .mtlx stays minimal across a round-trip and VCS diffs stay
    // readable. Lobe bodies are additionally gated on the lobe being enabled.
    const std::set<std::string> present = CollectPresentInputs(surf);

    // Base. base_weight was folded into baseColor.rgb on import, so write it back as base_color
    // with base_weight 1 (the effective colour round-trips); the alpha rides geometry_opacity.
    std::vector<float> bc = DocColor(doc, "baseColor");
    MaybeSetFloat(surf, present, "base_weight", 1.0f, 1.0f);
    MaybeSetColor(surf, present, "base_color", bc, 0.8f, 0.8f, 0.8f);
    MaybeSetFloat(surf, present, "geometry_opacity", bc.size() > 3 ? bc[3] : 1.0f, 1.0f);
    MaybeSetFloat(surf, present, "base_metalness", DocFloat(doc, "metallic", 0.0f), 0.0f);
    MaybeSetFloat(surf, present, "base_diffuse_roughness", DocFloat(doc, "diffuseRoughness", 0.0f), 0.0f);

    MaybeSetFloat(surf, present, "specular_weight", DocFloat(doc, "specularWeight", 1.0f), 1.0f);
    MaybeSetColor(surf, present, "specular_color", DocColor(doc, "specularColor"), 1.0f, 1.0f, 1.0f);
    MaybeSetFloat(surf, present, "specular_roughness", DocFloat(doc, "roughness", 0.3f), 0.3f);
    MaybeSetFloat(surf, present, "specular_ior", DocFloat(doc, "specularIor", 1.5f), 1.5f);

    // Lobe weights are gated by our enableX flags so the on/off state round-trips: a disabled lobe
    // writes weight 0 (== spec default), which MaybeSet omits unless it was authored in the source.
    MaybeSetFloat(surf, present, "specular_roughness_anisotropy",
                  DocBool(doc, "enableAnisotropy") ? DocFloat(doc, "anisotropy", 0.0f) : 0.0f, 0.0f);
    // Rotation round-trips radians -> OpenPBR turns [0..1]. Spec default is 0 (no rotation),
    // so MaybeSetFloat omits it unless authored, and a disabled lobe writes 0.
    MaybeSetFloat(surf, present, "specular_roughness_anisotropy_rotation",
                  DocBool(doc, "enableAnisotropy") ? DocFloat(doc, "anisotropyRotation", 0.0f) / kTwoPi : 0.0f, 0.0f);

    const bool transOn = DocBool(doc, "enableTransmission");
    MaybeSetFloat(surf, present, "transmission_weight",
                  transOn ? DocFloat(doc, "transmissionWeight", 0.0f) : 0.0f, 0.0f);
    // OpenPBR transmission_color @ transmission_depth IS the Beer–Lambert volume — our attenuation
    // pair (the surface transmissionColor tint has no OpenPBR slot, so it stays engine-side; on
    // import both seed equal from transmission_color, so this still round-trips an imported .mtlx).
    // transmission_depth spec default is 0 (absorption off), so it is omitted unless authored.
    MaybeSetColor(surf, present, "transmission_color", DocColor(doc, "attenuationColor"), 1.0f, 1.0f, 1.0f, transOn);
    MaybeSetFloat(surf, present, "transmission_depth", DocFloat(doc, "attenuationDistance", 0.0f), 0.0f, transOn);
    // thin_walled is the inverse of our thick (two-surface) path; write it so a thick/thin toggle
    // round-trips. Spec default is false (volumetric), so only emit it when true or already authored.
    const bool thinWalled = transOn && !DocBool(doc, "enableTransmissionThick");
    if (present.count("thin_walled") || thinWalled)
        SetInput(surf, "thin_walled", "boolean", thinWalled ? "true" : "false");

    const bool subsurfaceOn = DocBool(doc, "enableSubsurface");
    MaybeSetFloat(surf, present, "subsurface_weight", subsurfaceOn ? 1.0f : 0.0f, 0.0f);
    MaybeSetColor(surf, present, "subsurface_color", DocColor(doc, "subsurfaceColor"), 0.8f, 0.8f, 0.8f, subsurfaceOn);

    const bool coatOn = DocBool(doc, "enableClearCoat");
    MaybeSetFloat(surf, present, "coat_weight", coatOn ? DocFloat(doc, "clearCoat", 0.0f) : 0.0f, 0.0f);
    MaybeSetFloat(surf, present, "coat_roughness", DocFloat(doc, "clearCoatRoughness", 0.0f), 0.0f, coatOn);
    MaybeSetFloat(surf, present, "coat_ior", DocFloat(doc, "clearCoatIor", 1.5f), 1.5f, coatOn);
    // coat_darkening's OpenPBR spec default is 1.0 (our material default is 0.0 inert); gate on the coat.
    MaybeSetFloat(surf, present, "coat_darkening", DocFloat(doc, "coatDarkening", 0.0f), 1.0f, coatOn);
    MaybeSetColor(surf, present, "coat_color", DocColor(doc, "coatColor"), 1.0f, 1.0f, 1.0f, coatOn);

    // OpenPBR fuzz_* round-trips to/from our distinct over-coat Fuzz lobe (import folds fuzz_weight
    // into fuzzColor, so write a presence weight of 1 + the folded colour back). The under-coat
    // Sheen lobe has no OpenPBR fuzz equivalent and is intentionally NOT written here.
    const bool fuzzOn = DocBool(doc, "enableFuzz");
    MaybeSetFloat(surf, present, "fuzz_weight", fuzzOn ? 1.0f : 0.0f, 0.0f);
    MaybeSetColor(surf, present, "fuzz_color", DocColor(doc, "fuzzColor"), 1.0f, 1.0f, 1.0f, fuzzOn);
    MaybeSetFloat(surf, present, "fuzz_roughness", DocFloat(doc, "fuzzRoughness", 0.3f), 0.3f, fuzzOn);

    const bool iridescenceOn = DocBool(doc, "enableIridescence");
    MaybeSetFloat(surf, present, "thin_film_weight",
                  iridescenceOn ? DocFloat(doc, "thinFilmWeight", 0.0f) : 0.0f, 0.0f);
    MaybeSetFloat(surf, present, "thin_film_thickness",
                  DocFloat(doc, "thinFilmThickness", 500.0f) / 1000.0f, 0.5f, iridescenceOn); // nm -> um
    MaybeSetFloat(surf, present, "thin_film_ior", DocFloat(doc, "thinFilmIor", 1.5f), 1.5f, iridescenceOn);

    const bool emissiveOn = DocFloat(doc, "emissionLuminance", 0.0f) > kLobeEnableEpsilon;
    MaybeSetColor(surf, present, "emission_color", DocColor(doc, "emissive"), 1.0f, 1.0f, 1.0f, emissiveOn);
    MaybeSetFloat(surf, present, "emission_luminance", DocFloat(doc, "emissionLuminance", 0.0f), 0.0f);

    return xmlDoc.save_file(mtlxPath.c_str(), "  ");
}

} // namespace GameEngine
