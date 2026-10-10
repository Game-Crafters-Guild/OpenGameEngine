#include "Editor/Materials/EmissiveMapAssignment.h"

#include "Components/Rendering/LightPhotometry.h"
#include "Rendering/Materials/MaterialDocument.h"

#include <algorithm>
#include <variant>
#include <vector>

namespace GameEngine::Editor::MaterialRows
{

void TurnOnEmissionForAssignedMap(MaterialDocument& doc)
{
    const auto luminance = doc.properties.find("emissionLuminance");
    const float* nits = luminance != doc.properties.end() ? std::get_if<float>(&luminance->second) : nullptr;
    if (nits == nullptr || !(*nits > 0.0f))
        doc.properties["emissionLuminance"] = Components::kReferenceWhiteNits;

    const auto color = doc.properties.find("emissive");
    const auto* rgb = color != doc.properties.end() ? std::get_if<std::vector<float>>(&color->second) : nullptr;
    if (rgb == nullptr || std::none_of(rgb->begin(), rgb->end(), [](float c) { return c > 0.0f; }))
        doc.properties["emissive"] = std::vector<float>{1.0f, 1.0f, 1.0f};
}

} // namespace GameEngine::Editor::MaterialRows
