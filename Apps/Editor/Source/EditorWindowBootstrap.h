#pragma once

// Internal window-bootstrap helpers shared between EditorApplication.cpp
// (which defines them) and EditorApplication_Windows.cpp (floating-window
// creation). Same pattern as EditorDockNodeJson.h: declaration here,
// definition in one translation unit.

#include "Core/WindowUiBootstrap.h"
#include "EditorApplication.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace GameEngine
{
class AssetManager;
class UIManager;

// Saved "renderer.graphicsApi" preference (auto|vulkan|metal).
Rendering::GraphicsAPI LoadPreferredGraphicsApi();

// One config point every editor window's UIManager passes through (JobSystem
// wiring + HDR UI paper-white/black-lift derivation + fast-path policy).
void ApplyUiRuntimeConfig(GameEngine::UIManager* ui);

// True if the current UI focus is in a TextField/TextArea (hotkey routing).
bool IsFocusInTextField(UIManager* ui);

// Apply a WindowDescriptor's role/capabilities/title to a window context.
void ApplyWindowDescriptor(EditorApplication::EditorWindowContext& ctx,
                           const EditorApplication::WindowDescriptor& desc,
                           uint64_t windowId);

// Re-apply the persisted accent color to a freshly created UIManager.
void ApplySavedAccentStyle(UIManager* ui);

// Editor font registration + defaults for a freshly created UIManager.
void ConfigureEditorUiFonts(UIManager* ui, AssetManager* assetManager,
                            const std::filesystem::path& assetsDirectory);

// Ensure the editor theme stylesheet is attached (sync path).
bool EnsureEditorThemeStyleAttached(UIManager* ui,
                                    AssetManager* assetManager,
                                    const std::filesystem::path& styleAssetPath,
                                    GUID styleGuid,
                                    const std::string& contextLabel,
                                    std::string_view sourceAlias);

// Synchronous editor-window UI bootstrap (layout + style load).
WindowUiBootstrapResult BootstrapEditorWindowUi(UIManager* ui,
                                                AssetManager* assets,
                                                const GUID& layoutGuid,
                                                const GUID& styleGuid,
                                                const std::string& contextLabel);

} // namespace GameEngine
