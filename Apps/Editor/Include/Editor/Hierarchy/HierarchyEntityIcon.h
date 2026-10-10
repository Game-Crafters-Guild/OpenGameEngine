#pragma once

#include "AssetCore/GUID.h"
#include "ECS/ECS.h"

#include <filesystem>
#include <string>

namespace GameEngine {

class AssetManager;
class UIElement;
class UIManager;
struct EditorContext;

namespace Editor {

void ClearHierarchyEntityIconClasses(UIElement* element);
void ApplyHierarchyEntityIconClasses(ECS::World* world, UIElement* element, ECS::EntityHandle handle);
/// Same, for the element that actually draws the glyph. Adds the shared
/// `entity-icon` marker on top of the kind class, so one CSS selector
/// (`.entity-icon.hierarchy-entity-*`) styles the Hierarchy tree's preview
/// icon, the Inspector header icon and the Bookmarks row icon alike. Rows use
/// the plain overload above: a row carrying the marker would paint the glyph
/// as its own background.
void ApplyHierarchyEntityIconClassesToIcon(ECS::World* world, UIElement* icon,
                                           ECS::EntityHandle handle);

/// True when \p element carries a hierarchy-entity CSS class that renders as a pure CSS
/// glyph (lights, cameras, primitives, empties, terrain, ocean, physics, etc.) rather than
/// a model/sprite thumbnail. Callers use this to skip the model-GUID subtree scan for rows
/// that will never show a thumbnail. This is the single source of truth — both the Hierarchy
/// row bind and the Inspector header icon consult it.
bool EntityIconIsCssOnly(const UIElement& element);

GUID FindModelGuidForEntityOrChildren(ECS::World* world, ECS::EntityHandle handle);

/// If \p handle is a sprite (plane primitive + file-backed MaterialAsset),
/// returns the absolute filesystem path of its albedoMap texture. Returns an
/// empty path when the entity isn't a sprite or the texture can't be resolved.
std::filesystem::path TryResolveSpriteAlbedoTexturePath(ECS::World* world,
                                                        ECS::EntityHandle handle,
                                                        AssetManager* assets);

/// Returns the source-relative asset path (metadata path) corresponding to the
/// asset displayed by the inspector header icon: the sprite's albedo texture if
/// the entity is a sprite, otherwise the model asset referenced (directly or by
/// a child) by its MeshRenderer. Returns an empty path for CSS-only icons
/// (lights, primitives, cameras, etc.) that have no underlying file asset.
std::filesystem::path TryResolveInspectorIconAssetPath(ECS::World* world,
                                                       ECS::EntityHandle handle,
                                                       AssetManager* assets);

void ApplyTitleModelThumbnail(UIElement& titleEl, const std::string& relOrEngine, float iconSizePx);

/// Matches Hierarchy panel visuals: CSS entity classes and/or model / Polyhaven thumbnails on \p iconEl.
void ApplyInspectorEntityIcon(ECS::World* world,
                              const EditorContext* ctx,
                              UIElement& iconEl,
                              ECS::EntityHandle handle,
                              float iconSizePx,
                              UIManager* uiManager,
                              bool uiReplayActive,
                              UIElement* postActionHost);

} // namespace Editor
} // namespace GameEngine
