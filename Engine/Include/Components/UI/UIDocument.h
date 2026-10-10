#pragma once

#include <cstdint>
#include <type_traits>

#include "AssetCore/AssetTypes.h"
#include "Components/AssetRef.h"

namespace GameEngine {
namespace Components {

// How a UI document composites into its render target.
enum class UIRenderMode : uint8_t
{
    Overlay = 0,    // composite the UI OVER existing content (a HUD over the scene) — the default
    Fullscreen = 1, // the UI owns/clears the target (a full-screen menu)
};

// Attaches a UI document (a layout + style) to an entity — the way Unity's UI
// Toolkit UIDocument references a VisualTreeAsset + StyleSheets, but with the
// PanelSettings concerns folded into this one component + host defaults (no
// separate settings asset to create and assign).
//
// The referenced layout (.uxml) and style (.css) are mounted as a subtree of the
// shared game UIManager; multiple UIDocuments compose in one space ordered by
// SortOrder (back-to-front compositing + hit-test priority, realized as a CSS
// z-index). The render TARGET (Player backbuffer, editor Game View, Scene View)
// is chosen by the host — not the component — so the SAME document renders
// identically in each. Compose multiple stylesheets via CSS @import on Style.
struct UIDocument
{
    AssetRef<AssetType::UILayout> Layout; // the .uxml — the only field you must set
    AssetRef<AssetType::UIStyle>  Style;  // the .css (composes more sheets via @import)

    int32_t      SortOrder  = 0;                     // back-to-front order + hit-test priority
    UIRenderMode RenderMode = UIRenderMode::Overlay; // Overlay=HUD (blend) · Fullscreen=menu (clear)
};

static_assert(std::is_trivially_copyable_v<UIDocument>,
              "UIDocument must be trivially copyable for ECS chunk storage");
static_assert(std::is_standard_layout_v<UIDocument>,
              "UIDocument must be standard layout for ECS chunk storage");

} // namespace Components
} // namespace GameEngine
