#pragma once

#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Core/Engine.h"
#include "UI/MaterialTexturePreviewHost.h"
#include "UI/UIElement.h"

#include <functional>
#include <memory>
#include <utility>

namespace GameEngine::InspectorUI
{

/// A texture slot's thumbnail, the square the material inspector's slots use: it previews what is
/// bound, takes a texture drop as a second way to assign one, and outlines as an empty slot when
/// nothing is. Sits under the field that names it, so the slot reads as a slot rather than as the
/// word "(None)".
inline void AddTextureSlotPreview(UIElement* parent, const GUID& guid,
                                  std::function<void(const GUID&)> onDropped)
{
    if (!parent)
        return;

    AssetRegistry& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();

    auto rowOwned = std::make_unique<UIElement>();
    rowOwned->AddClass("inspector-texture-preview-row");
    UIElement* row = rowOwned.get();
    parent->AddChild(std::move(rowOwned));

    auto preview = std::make_unique<MaterialTexturePreviewHost>();
    preview->AddClass("texture-preview");
    preview->AddClass("texture-preview-side");
    preview->SetAssetRegistry(&registry);
    preview->SetOnTextureDropped(std::move(onDropped));
    preview->SetTexturePreviewFromGuid(registry, guid);
    row->AddChild(std::move(preview));
}

} // namespace GameEngine::InspectorUI
