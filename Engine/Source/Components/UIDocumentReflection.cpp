// Manual reflection + cross-DLL template instantiation for UIDocument.
//
// Like LensFlareSource, this component is hand-registered: its AssetRef<> fields
// are templated types the build-time ComponentScanner can't parse, so it skips
// the struct. That also means the UIRenderMode enum-name table (for the inspector
// dropdown) must be written by hand here rather than auto-emitted by the scanner.

#include "Components/ComponentRegistration.h" // GE_REGISTER_COMPONENT, GE_REFLECT_ENUM_*
#include "Components/UI/UIDocument.h"

#include "ECS/ECSTemplates.h" // GE_INSTANTIATE_ENGINE_COMPONENT

// Field table + factory registration (Add Component menu + scene serialization).
GE_REGISTER_COMPONENT(GameEngine::Components::UIDocument, Layout, Style, SortOrder, RenderMode);

// Enum value-name table so RenderMode shows as a named dropdown in the inspector.
GE_REFLECT_ENUM_TABLE_BEGIN(UIRenderMode)
    GE_REFLECT_ENUM_TABLE_VALUE("Overlay", 0)
    GE_REFLECT_ENUM_TABLE_VALUE("Fullscreen", 1)
GE_REFLECT_ENUM_TABLE_END()
GE_REFLECT_ENUM_FIELD(GameEngine::Components::UIDocument, RenderMode, UIRenderMode);

// Explicit instantiation of the type-erased handler + World accessors so the
// component works across the Engine DLL boundary.
namespace GameEngine::ECS
{
GE_INSTANTIATE_ENGINE_COMPONENT(Components::UIDocument);
} // namespace GameEngine::ECS
