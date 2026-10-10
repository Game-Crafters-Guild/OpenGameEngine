#pragma once

namespace GameEngine
{

struct MaterialDocument;

namespace Engine::Renderer
{
class Material;
}

// How the material inspector hands a document's textures to the renderer's runtime material. Both
// route by texture name through the material's own slot set, the way registration does, so a
// surface's own name (the standard surface's heightMap) reaches the ordinal it binds to and a name
// the surface does not have drives nothing.
namespace Editor::MaterialRuntimeTextures
{

// The document to rebind the runtime material's textures from: `doc` plus every slot the material's
// surface has and `doc` leaves out, written back empty so a removed binding clears. A name outside
// the surface's set is never added: it would be reported as an unknown key the user never wrote.
MaterialDocument WithEverySurfaceSlot(const MaterialDocument& doc, const Engine::Renderer::Material& runtime);

// Re-applies the document's texture transforms to the runtime material: every slot back to identity
// first, so a transform the document no longer has (an undo that removed it) leaves no stale tiling,
// then each authored transform by name.
void ApplyTextureTransforms(const MaterialDocument& doc, Engine::Renderer::Material& runtime);

} // namespace Editor::MaterialRuntimeTextures

} // namespace GameEngine
