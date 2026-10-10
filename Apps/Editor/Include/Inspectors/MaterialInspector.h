#pragma once

#include "InspectorRegistry.h"

namespace GameEngine
{

class MaterialAsset;
class UIElement;

namespace Editor { class UndoRedoService; }

void RegisterMaterialInspector();

/// Build the full material property inspector UI into the given root element.
/// Exposed so other inspectors (e.g. MeshRenderer) can embed material properties inline.
/// `inlineEmbed` = true when the material is rendered inside another inspector
/// (e.g. a MeshRenderer foldout); controls whether the File path section is shown.
/// `openAsset` is the editor's open-asset policy (InspectorContext::OpenAsset); the
/// surface-shader button routes through it so it opens exactly what a double-click would.
using OpenAssetFn = std::function<void(const std::filesystem::path&)>;

void BuildMaterialInspectorUI(UIElement* root, MaterialAsset* mat, OpenColorPickerWindowFn openPicker = {},
                              std::function<void(const std::filesystem::path&)> pingAsset = {},
                              Editor::UndoRedoService* undo = nullptr,
                              bool inlineEmbed = false,
                              OpenAssetFn openAsset = {},
                              std::function<void(const std::filesystem::path&)> pingAssetPreserveInspector = {});

} // namespace GameEngine

