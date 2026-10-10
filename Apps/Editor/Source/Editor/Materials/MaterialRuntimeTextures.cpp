#include "Editor/Materials/MaterialRuntimeTextures.h"

#include "Engine/Rendering/Material.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Types/StringId.h"

#include <string>
#include <string_view>

namespace GameEngine::Editor::MaterialRuntimeTextures
{

MaterialDocument WithEverySurfaceSlot(const MaterialDocument& doc, const Engine::Renderer::Material& runtime)
{
    MaterialDocument bound = doc;
    for (const std::string_view slot : runtime.GetTextureSlotNames())
        bound.textures.try_emplace(std::string(slot), "");
    return bound;
}

void ApplyTextureTransforms(const MaterialDocument& doc, Engine::Renderer::Material& runtime)
{
    for (uint32_t ordinal = 0; ordinal < static_cast<uint32_t>(Engine::Renderer::TextureSlot::kCount); ++ordinal)
        runtime.SetTextureTransform(static_cast<Engine::Renderer::TextureSlot>(ordinal), 1.0f, 1.0f, 0.0f, 0.0f);
    for (const auto& [name, transform] : doc.textureTransforms)
        runtime.SetTextureTransform(HashStringId(name), transform);
}

} // namespace GameEngine::Editor::MaterialRuntimeTextures
