// Stub SettingsPanel / shortcut-catalog symbols for EditorTests.
//
// The real SettingsPanel at Apps/Editor/Source/Panels/SettingsPanel.cpp is
// thousands of lines and transitively depends on dropdown/tree/color-picker
// controls we don't want to pull into the test target. Compiled EditorTests
// TUs reach:
//   - SettingsPanel::NotifyActivePipelineChanged (RenderPipelineInspector)
//   - Editor::MatchesCatalogShortcut (SceneViewTransformTool)
//   - Editor::ShouldSuppressEditorShortcutActions (SceneEditorController)
//   - Editor::ItemResizeGestureBindings, ItemResizeGestureCatalogEntry,
//     ShortcutBindingsSaved and FormatShortcutBinding (ItemSizeSlider's tooltip)
// Catalog lookup needs SettingsStore; returning false is enough to link
// and to keep TransformTool tests off the catalog path.

#include "Editor/Shortcuts/EditorShortcuts.h"
#include "Panels/SettingsPanel.h"

namespace GameEngine
{
    void SettingsPanel::NotifyActivePipelineChanged(const std::string&) {}

    namespace Editor
    {
        bool MatchesCatalogShortcut(const char*, const char*, int, int)
        {
            return false;
        }

        bool ShouldSuppressEditorShortcutActions()
        {
            return false;
        }

        // No catalog: Resize Items has no bindings, so the size slider shows its unbound tooltip.
        const ShortcutCatalogEntry& ItemResizeGestureCatalogEntry()
        {
            static const ShortcutCatalogEntry entry{};
            return entry;
        }

        const std::vector<ShortcutBinding>& ItemResizeGestureBindings()
        {
            static const std::vector<ShortcutBinding> none;
            return none;
        }

        Event<const ShortcutCatalogEntry&, const std::vector<ShortcutBinding>&>& ShortcutBindingsSaved()
        {
            static Event<const ShortcutCatalogEntry&, const std::vector<ShortcutBinding>&> saved;
            return saved;
        }

        std::string FormatShortcutBinding(int, int)
        {
            return {};
        }
    } // namespace Editor
}
