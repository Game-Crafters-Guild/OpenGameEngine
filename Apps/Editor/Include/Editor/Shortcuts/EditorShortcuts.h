#pragma once

#include "Editor/Settings/SettingsStore.h"
#include "Events/Event.h"
#include "Input/InputSystem.h"

#include <string>
#include <vector>

namespace GameEngine::Editor
{

// Mouse-wheel binding sentinel used by scroll handlers (not a real KeyCode).
inline constexpr int kShortcutKeyMouseScroll = 10001;

struct ShortcutCatalogEntry
{
    const char* groupLabel = nullptr;
    const char* displayName = nullptr;
    Input::ContextId context = 0;
    Input::ActionId action = 0;
    int defaultKey = 0;
    int defaultMods = 0;
    // When true the entry is shown for reference only — the shortcut is
    // hardcoded in an ad-hoc key handler and not wired through InputSystem
    // yet, so rebinding is disabled until the call site is migrated.
    bool readOnly = false;
    // Optional second default chord (platform alternate, e.g. Ctrl+S next to
    // Cmd+S). Both defaults participate in load fallback, reset, and conflict
    // detection so an alternate can never double-fire invisibly.
    int defaultKey2 = 0;
    int defaultMods2 = 0;
    // Preserve ordinary scrolling for actions layered over a scrollable view.
    bool scrollRequiresModifier = false;
};

struct ShortcutBinding
{
    int key = 0;
    int mods = 0;
    bool operator==(const ShortcutBinding& o) const { return key == o.key && mods == o.mods; }
};

const std::vector<ShortcutCatalogEntry>& GetShortcutCatalog();

// The entry's default chord set (one or two bindings).
std::vector<ShortcutBinding> DefaultShortcutBindings(const ShortcutCatalogEntry& entry);

std::vector<ShortcutBinding> LoadShortcutBindings(const SettingsStore& store,
                                                  const ShortcutCatalogEntry& entry);
void SaveShortcutBindings(SettingsStore& store,
                          const ShortcutCatalogEntry& entry,
                          const std::vector<ShortcutBinding>& bindings);
// Raised by SaveShortcutBindings with the entry and the bindings a load from the store now
// returns (the defaults when the save removed an override). Raised before the caller writes the
// store to disk, so a subscriber takes the bindings from here. Main thread only.
Event<const ShortcutCatalogEntry&, const std::vector<ShortcutBinding>&>& ShortcutBindingsSaved();
void ApplyShortcutBindings(Input::InputSystem& input,
                           const ShortcutCatalogEntry& entry,
                           const std::vector<ShortcutBinding>& bindings);
// The other catalog entry already bound to key+mods, or null. A mouse-wheel binding
// only competes with entries of its own group: the wheel acts on the surface under the
// pointer, and no two groups' wheel bindings act on the same surface (View Navigation's
// Resize Items on the item views and their size sliders, Asset Preview's on its viewport),
// so two groups' wheel bindings never see one event. A new wheel binding on a surface
// another group already uses belongs in that group.
const ShortcutCatalogEntry* FindShortcutConflict(const ShortcutCatalogEntry& self,
                                                 int key, int mods);

std::string FormatShortcutBinding(int keyCode, int mods);
bool IsModifierKey(int keyCode);
int CountShortcutModifiers(int mods);

// Applies user shortcut overrides from Preferences.json on top of the
// editor's default action bindings. Called after default RegisterAction
// calls during editor startup.
void ApplyShortcutOverridesFromPreferences(Input::InputSystem& input);

// True when the current scroll modifiers match the user's Asset Preview
// Dolly or Cycle Animation binding (Shortcuts settings).
bool AssetPreviewScrollMatchesDolly(int mods);
bool AssetPreviewScrollMatchesCycleAnimation(int mods);

// True when key+mods matches a Shortcuts-catalog binding (default or user
// override). Used by ad-hoc handlers that are not yet InputSystem actions.
bool MatchesCatalogShortcut(const char* groupLabel, const char* displayName,
                            int key, int mods);

// The Resize Items catalog binding: the item resize gesture's matcher, which
// ApplyShortcutOverridesFromPreferences installs in the UI module. A bare wheel
// never matches: the loader refuses a stored Resize Items binding without a modifier.
bool MatchesItemResizeGesture(int mods);
// The Resize Items catalog entry, and its bindings as the matcher sees them.
const ShortcutCatalogEntry& ItemResizeGestureCatalogEntry();
const std::vector<ShortcutBinding>& ItemResizeGestureBindings();

// Key-only variant for release edges: a hold begun on the press edge must end
// on the key's release even when modifiers changed mid-hold, and even while
// shortcut capture suppresses press-edge matching.
bool MatchesCatalogShortcutKeyOnly(const char* groupLabel, const char* displayName, int key);

// While a Shortcuts row is listening for a new binding, editor InputSystem
// actions (Save Scene, etc.) must not fire — the same key event feeds both.
void SetShortcutCaptureActive(bool active);
void NoteShortcutCaptureConsumedKey();
bool ShouldSuppressEditorShortcutActions();
void ClearShortcutCaptureConsumedKey();

// Sample capture suppression, then clear the one-frame consumed-key latch.
// Call once per Update() before polling InputSystem editor actions.
bool AllowShortcutActionsThisFrame();

} // namespace GameEngine::Editor
