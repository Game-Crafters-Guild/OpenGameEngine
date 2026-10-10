#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace GameEngine
{

struct MaterialDocument;

// The material inspector's row model: which built-in property and texture rows
// a material document produces, in what order, and how each row is presented.
// Pure functions of the document — no UI context, no engine services — so the
// panel is left owning only widget construction and event wiring.
namespace Editor::MaterialRows
{

// Lighting models that suppress whole families of rows. ShadowOnly draws depth
// only, so every surface-response row is meaningless on it; Unlit keeps
// baseColor/emissive but drops the BRDF rows.
bool IsLightingModelUnlit(const std::string& lightingModel);
bool IsLightingModelShadowOnly(const std::string& lightingModel);

// Built-in property rows in the order the inspector lays them out.
const std::vector<std::string>& BuiltInPropertyOrder();

// Built-in property keys, for separating built-in rows from a document's custom
// (shader-declared) properties.
const std::unordered_set<std::string>& BuiltInPropertyNames();

// Texture slot rows in the order the inspector lays them out.
const std::vector<std::string>& TextureSlotOrder();

// Whether a row is shown at all for this document. A hidden row is one whose
// value cannot affect what the material renders — the gate is "does anything
// read it", not "is it tidy".
bool IsPropertyVisible(const std::string& key, const MaterialDocument& doc);
// `surfaceTextureNames` are the `// @texture` names the document's surface
// declares (empty for a surface that declares none and samples the fixed
// ladder). The Height row is offered only where the surface declares
// `heightMap`: no ladder surface reads one.
bool IsTextureVisible(const std::string& slot, const MaterialDocument& doc,
                      const std::vector<std::string>& surfaceTextureNames);

// The Relief Depth row: shown exactly when the Height row is and a height map
// is bound, since nothing reads the depth without one.
bool IsReliefDepthVisible(const MaterialDocument& doc, const std::vector<std::string>& surfaceTextureNames);

// The Relief Depth row's range and tooltip. The range is a fraction of one
// height repeat; 0 turns the relief off. The top is the deepest relief whose
// self-shadow the eight-sample shadow march follows without tearing: the
// inspector clamps the slider and a typed value to it.
inline constexpr float kReliefDepthSliderMax = 0.04f;
inline constexpr const char* kReliefDepthTooltip =
    "How deep the relief is, as a fraction of one texture repeat: 0.02 on a 2 m repeat is 4 cm. "
    "0 turns parallax off. The engine decides how many samples to take from how large the relief "
    "is on screen.";

// The Emissive Exposure Weight row's tooltip: the two ends of the [0, 1] slider and what it leaves
// alone (the light the emission casts and its reflections), and how a large one moves the exposure.
inline constexpr const char* kEmissiveExposureWeightTooltip =
    "How far the emission follows the view's exposure. 1 (the default) is physical: the emission is "
    "in nits and brightens and darkens with exposure like every light. 0 shows it at its authored "
    "brightness whatever the exposure, the same at noon and at night; values between follow exposure "
    "part of the way. The light the emission casts on the scene and its reflections stay physical at "
    "every value. Auto exposure meters the emission as shown, so a surface below 1 that fills much of "
    "the view darkens or brightens everything else.";

// Control selection for a visible property row.
bool IsColorProperty(const std::string& name);
bool IsSlider01Property(const std::string& name);

// camelCase key -> display label ("baseColor" -> "Base Color", "ao" -> "AO";
// texture slots drop a trailing "Map": "metallicRoughnessMap" -> "Metallic
// Roughness").
std::string PropertyLabel(const std::string& key);
std::string TextureSlotLabel(const std::string& slot);

} // namespace Editor::MaterialRows

} // namespace GameEngine
