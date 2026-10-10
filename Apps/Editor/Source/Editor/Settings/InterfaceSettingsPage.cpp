#include "Editor/Settings/InterfaceSettingsPage.h"

#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SettingsStore.h"
#include "EditorContextMenu/ContextMenuBackendPolicy.h"

#include <string>
#include <vector>
#include <utility>

namespace GameEngine::Editor
{
namespace
{

// The panel persists the row's value under this key; it is read back at
// registration below, because the row's Set only fires once the page is opened
// and menus must not wait for that.
constexpr const char* kContextMenuBackendPrefKey = "interface.contextMenuBackend";
constexpr const char* kContextMenuKeepOpenPrefKey = "interface.contextMenuKeepOpenOnToggle";

constexpr const char* kBackendBuiltIn = "builtin";
constexpr const char* kBackendNative = "native";

const char* BackendValue(ContextMenuBackend backend)
{
    return backend == ContextMenuBackend::Native ? kBackendNative : kBackendBuiltIn;
}

ContextMenuBackend BackendFromValue(const std::string& value)
{
    return value == kBackendNative ? ContextMenuBackend::Native : ContextMenuBackend::BuiltIn;
}

void ApplyStoredValues()
{
    SettingsStore prefs = OpenEditorPreferences();
    std::string error;
    prefs.Load(&error);

    std::string backend;
    if (prefs.TryGetString(kContextMenuBackendPrefKey, backend))
        SetContextMenuBackend(BackendFromValue(backend));

    bool keepOpen = true;
    if (prefs.TryGetBool(kContextMenuKeepOpenPrefKey, keepOpen))
        SetContextMenuKeepOpenOnToggle(keepOpen);
}

} // namespace

void RegisterInterfaceSettingsCategory()
{
    SettingsCategoryDescriptor interface_;
    interface_.CategoryId = "interface";
    interface_.Title = "Context Menus";
    interface_.Group = SettingsCategoryGroup::UIAppearance;
    interface_.SearchKeywords = "interface context menu native builtin popup right click appearance";
    interface_.Description = "Which implementation the editor's right-click menus use.";

    SettingsFieldDescriptor backend;
    backend.Label = "Context Menus";
    backend.Tooltip =
        "Which implementation right-click menus use. Built-in draws the editor's own menu — "
        "identical on every platform and stylable like the rest of the UI. Native uses the "
        "operating system's menu where one exists (Windows, macOS); platforms without one fall "
        "back to Built-in. Applies to menus opened after the change.";
    backend.SearchKeywords = "context menu native builtin os popup right click";
    backend.PrefKey = kContextMenuBackendPrefKey;

    SettingsFieldDescriptor::DropdownField dropdown;
    dropdown.OptionsProvider = []
    {
        std::vector<SettingsFieldDescriptor::DropdownField::Option> options;
        options.push_back({kBackendBuiltIn, "Built-in"});
        if (NativeContextMenuAvailable())
            options.push_back({kBackendNative, "Native OS"});
        return options;
    };
    dropdown.DefaultValue = BackendValue(DefaultContextMenuBackend());
    dropdown.Get = [] { return std::string(BackendValue(GetContextMenuBackend())); };
    dropdown.Set = [](const std::string& value) { SetContextMenuBackend(BackendFromValue(value)); };
    backend.Control = std::move(dropdown);
    interface_.Fields.push_back(std::move(backend));

    SettingsFieldDescriptor keepOpen;
    keepOpen.Label = "Keep Menu Open When Toggling";
    keepOpen.Tooltip =
        "A click on a checkable choice keeps the menu open so several options "
        "can be set in one visit; commands still close it, as do clicking "
        "outside and Escape. Off restores close-on-every-click.";
    keepOpen.SearchKeywords = "context menu toggle sticky keep open selection check";
    keepOpen.PrefKey = kContextMenuKeepOpenPrefKey;
    SettingsFieldDescriptor::ToggleField keepOpenToggle;
    keepOpenToggle.DefaultValue = true;
    keepOpenToggle.Get = [] { return GetContextMenuKeepOpenOnToggle(); };
    keepOpenToggle.Set = [](bool value) { SetContextMenuKeepOpenOnToggle(value); };
    keepOpen.Control = std::move(keepOpenToggle);
    interface_.Fields.push_back(std::move(keepOpen));

    EditorSettingsRegistry::Get().RegisterCategory(std::move(interface_));
    ApplyStoredValues();
}

} // namespace GameEngine::Editor
