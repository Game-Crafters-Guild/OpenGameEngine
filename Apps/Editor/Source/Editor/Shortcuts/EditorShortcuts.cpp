#include "Editor/Shortcuts/EditorShortcuts.h"

#include "EditorInputActions.h"
#include "Editor/Settings/SettingsStore.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Platform/Capabilities.h"
#include "UI/Interaction/ItemResizeGesture.h"

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace GameEngine::Editor
{
namespace
{
std::string KeyCodeDisplayName(int keyCode)
{
    using namespace Input;
    if (keyCode >= kKeyCode_A && keyCode <= kKeyCode_Z)
        return std::string(1, static_cast<char>('A' + (keyCode - kKeyCode_A)));
    if (keyCode >= kKeyCode_0 && keyCode <= kKeyCode_9)
        return std::string(1, static_cast<char>('0' + (keyCode - kKeyCode_0)));
    switch (keyCode)
    {
        case kKeyCode_Space:      return "Space";
        case kKeyCode_Enter:      return "Enter";
        case kKeyCode_Escape:     return "Esc";
        case kKeyCode_Tab:        return "Tab";
        case kKeyCode_Backspace:  return "Backspace";
        case kKeyCode_Delete:     return "Delete";
        case kKeyCode_Insert:     return "Insert";
        case kKeyCode_Home:       return "Home";
        case kKeyCode_End:        return "End";
        case kKeyCode_PageUp:     return "PageUp";
        case kKeyCode_PageDown:   return "PageDown";
        case kKeyCode_Left:       return "Left";
        case kKeyCode_Right:      return "Right";
        case kKeyCode_Up:         return "Up";
        case kKeyCode_Down:       return "Down";
        case kKeyCode_F1:         return "F1";
        case kKeyCode_F2:         return "F2";
        case kKeyCode_F3:         return "F3";
        case kKeyCode_F4:         return "F4";
        case kKeyCode_F5:         return "F5";
        case kKeyCode_F6:         return "F6";
        case kKeyCode_F7:         return "F7";
        case kKeyCode_F8:         return "F8";
        case kKeyCode_F9:         return "F9";
        case kKeyCode_F10:        return "F10";
        case kKeyCode_F11:        return "F11";
        case kKeyCode_F12:        return "F12";
        case kKeyCode_LeftShift:
        case kKeyCode_RightShift:   return "Shift";
        case kKeyCode_LeftControl:
        case kKeyCode_RightControl: return "Ctrl";
        case kKeyCode_LeftAlt:
        case kKeyCode_RightAlt:     return "Alt";
        case kKeyCode_LeftSuper:
        case kKeyCode_RightSuper:
#ifdef __APPLE__
            return "Cmd";
#else
            return "Super";
#endif
        // Display-only sentinel for mouse-wheel bindings in the Shortcuts catalog.
        case kShortcutKeyMouseScroll: return "Scroll";
        default: break;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "Key%d", keyCode);
    return buf;
}

std::string ShortcutStoreKey(const ShortcutCatalogEntry& entry)
{
    // InputSystem-backed entries key by context+action. Hardcoded / panel-
    // consumed entries (context and action both 0) must key by group+name or
    // they would all share one preferences slot.
    if (entry.context != 0 || entry.action != 0)
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "shortcuts.%016llx.%016llx",
                 static_cast<unsigned long long>(entry.context),
                 static_cast<unsigned long long>(entry.action));
        return buf;
    }
    char buf[160];
    snprintf(buf, sizeof(buf), "shortcuts.manual.%s.%s",
             entry.groupLabel ? entry.groupLabel : "",
             entry.displayName ? entry.displayName : "");
    return buf;
}

std::atomic<int> g_ShortcutCaptureArmed{0};
std::atomic<bool> g_ShortcutCaptureConsumedKey{false};

// Per-entry binding cache so per-event lookups (key handlers, scroll) do not
// re-read Preferences.json from disk. Main-thread only, like the UI event
// dispatch that consumes it. Invalidated by SaveShortcutBindings; edits made
// by another editor process are picked up on this process's next save/restart.
uint64_t g_BindingCacheGeneration = 1;
uint64_t g_BindingCacheLoadedGeneration = 0;
std::vector<std::vector<ShortcutBinding>> g_CachedBindings;

const std::vector<std::vector<ShortcutBinding>>& CachedCatalogBindings()
{
    if (g_BindingCacheLoadedGeneration != g_BindingCacheGeneration)
    {
        const auto& catalog = GetShortcutCatalog();
        auto store = OpenEditorPreferences();
        store.Load(nullptr);
        g_CachedBindings.clear();
        g_CachedBindings.reserve(catalog.size());
        for (const auto& entry : catalog)
            g_CachedBindings.push_back(LoadShortcutBindings(store, entry));
        g_BindingCacheLoadedGeneration = g_BindingCacheGeneration;
    }
    return g_CachedBindings;
}

void InvalidateBindingCache()
{
    ++g_BindingCacheGeneration;
}

constexpr const char* kItemResizeGroupLabel = "View Navigation";
constexpr const char* kItemResizeDisplayName = "Resize Items";

size_t ItemResizeGestureCatalogIndex()
{
    static const size_t index = []
    {
        const auto& catalog = GetShortcutCatalog();
        for (size_t i = 0; i < catalog.size(); ++i)
        {
            if (std::strcmp(catalog[i].groupLabel, kItemResizeGroupLabel) == 0 &&
                std::strcmp(catalog[i].displayName, kItemResizeDisplayName) == 0)
                return i;
        }
        assert(false && "the shortcut catalog has no Resize Items entry");
        return size_t{0};
    }();
    return index;
}
} // namespace

bool IsModifierKey(int keyCode)
{
    using namespace Input;
    return keyCode == kKeyCode_LeftShift || keyCode == kKeyCode_RightShift ||
           keyCode == kKeyCode_LeftControl || keyCode == kKeyCode_RightControl ||
           keyCode == kKeyCode_LeftAlt || keyCode == kKeyCode_RightAlt ||
           keyCode == kKeyCode_LeftSuper || keyCode == kKeyCode_RightSuper;
}

int CountShortcutModifiers(int mods)
{
    int count = 0;
    if (mods & Input::kModControl) ++count;
    if (mods & Input::kModAlt)     ++count;
    if (mods & Input::kModShift)   ++count;
    if (mods & Input::kModSuper)   ++count;
    return count;
}

std::string FormatShortcutBinding(int keyCode, int mods)
{
    if (keyCode == 0)
        return "Unbound";
    std::string out;
    auto add = [&](const char* s)
    {
        if (!out.empty()) out += "+";
        out += s;
    };
    if (mods & Input::kModControl) add("Ctrl");
    if (mods & Input::kModAlt)     add("Alt");
    if (mods & Input::kModShift)   add("Shift");
    if (mods & Input::kModSuper)
    {
#ifdef __APPLE__
        add("Cmd");
#else
        add("Super");
#endif
    }
    if (!out.empty()) out += "+";
    out += KeyCodeDisplayName(keyCode);
    return out;
}

const std::vector<ShortcutCatalogEntry>& GetShortcutCatalog()
{
    using namespace EditorInput;
    using namespace Input;
    // A build-time __APPLE__ check gives the wrong answer on wasm, where one
    // binary reaches every host OS: Platform::PrimaryShortcutModifierIsCommand
    // asks the browser at runtime there, and falls back to the same __APPLE__
    // check natively, where the binary IS the OS it is compiled for.
    const bool isApple = Platform::PrimaryShortcutModifierIsCommand();
    const int kDefaultPrimaryShortcutModifier = isApple ? kModSuper : kModControl;
    const int kAlternatePrimaryShortcutModifier = isApple ? kModControl : kModSuper;
    // Finder renames on Return, so the Assets panel does too. F2 stays the
    // cross-platform binding; elsewhere Return keeps its open behaviour, which
    // is what Explorer and the Linux file managers do.
    const int kAssetsRenameAlternateKey = isApple ? kKeyCode_Enter : 0;
    static const std::vector<ShortcutCatalogEntry> kCatalog = {
        // --- Global (editable; consumed via MatchesCatalogShortcut / InputSystem) ---
        {"Global", "Save Scene",         kEditorGlobalContext, kEditorSaveScene,      kKeyCode_S,      kDefaultPrimaryShortcutModifier, false,
                                         kKeyCode_S,      kAlternatePrimaryShortcutModifier},
        {"Global", "Save Scene As",      kEditorGlobalContext, kEditorSaveSceneAs,    kKeyCode_S,      kDefaultPrimaryShortcutModifier | kModShift, false,
                                         kKeyCode_S,      kAlternatePrimaryShortcutModifier | kModShift},
        {"Global", "Universal Search",   kEditorGlobalContext, kEditorUniversalSearch, kKeyCode_K,     kUniversalSearchDefaultMods, false},
        {"Global", "Toggle Play Mode",   kEditorGlobalContext, kEditorTogglePlayMode, kKeyCode_P,      kModControl, false,
                                         kKeyCode_P,      kModSuper},
        {"Global", "Discard Play Review", kEditorGlobalContext, kEditorDiscardPlayReview, kKeyCode_Escape, 0,        false},
        {"Global", "Toggle Gizmos",      kEditorGlobalContext, kEditorToggleGizmos,   kKeyCode_G,      0,           false},
        {"Global", "Frame Selection",    kEditorGlobalContext, kEditorFrameSelection, kKeyCode_F,      0,           false},
        {"Global", "Frame All",          kEditorGlobalContext, kEditorFrameAll,       kKeyCode_F,      kModAlt,     false},
        {"Global", "Toggle Fullscreen",  kEditorGlobalContext, kEditorToggleWindowFullscreen, kKeyCode_F, kModControl, false,
                                         kKeyCode_F,      kModSuper},
        {"Global", "Undo",               0, 0, kKeyCode_Z, kDefaultPrimaryShortcutModifier, false,
                                         kKeyCode_Z,      kAlternatePrimaryShortcutModifier},
        {"Global", "Redo",               0, 0, kKeyCode_Z, kDefaultPrimaryShortcutModifier | kModShift, false,
                                         kKeyCode_Z,      kAlternatePrimaryShortcutModifier | kModShift},
        {"Global", "Redo (alt)",         0, 0, kKeyCode_Y, kDefaultPrimaryShortcutModifier, false,
                                         kKeyCode_Y,      kAlternatePrimaryShortcutModifier},
        {"Global", "Close Tab",          0, 0, kKeyCode_W, kDefaultPrimaryShortcutModifier, false,
                                         kKeyCode_W,      kAlternatePrimaryShortcutModifier},
        {"Global", "CSS Inspector",      0, 0, kKeyCode_I, kModControl, false},

        // --- Scene View ---------------------------------------------------
        {"Scene View", "Move Forward",   kSceneViewContext, kSceneViewMoveForward,  kKeyCode_W, 0, false},
        {"Scene View", "Move Backward",  kSceneViewContext, kSceneViewMoveBackward, kKeyCode_S, 0, false},
        {"Scene View", "Move Left",      kSceneViewContext, kSceneViewMoveLeft,     kKeyCode_A, 0, false},
        {"Scene View", "Move Right",     kSceneViewContext, kSceneViewMoveRight,    kKeyCode_D, 0, false},
        {"Scene View", "Move Faster",    kSceneViewContext, kSceneViewMoveFaster,   kKeyCode_LeftShift, 0, false},
        {"Scene View", "Delete Entity",  0, 0, kKeyCode_Delete, 0, false},
        {"Scene View", "Duplicate",      0, 0, kKeyCode_D, kDefaultPrimaryShortcutModifier, false,
                                         kKeyCode_D,      kAlternatePrimaryShortcutModifier},
        {"Scene View", "Pan Hold (2D)",  0, 0, kKeyCode_Space,  0, false},
        {kItemResizeGroupLabel, kItemResizeDisplayName, 0, 0, kShortcutKeyMouseScroll, kDefaultPrimaryShortcutModifier, false,
                                         kShortcutKeyMouseScroll, kAlternatePrimaryShortcutModifier, true},

        // --- Transform Tool (viewport QWER + extras) ----------------------
        {"Transform Tool", "Select Mode",        0, 0, kKeyCode_Q, 0, false},
        {"Transform Tool", "Translate Mode",     0, 0, kKeyCode_W, 0, false},
        {"Transform Tool", "Rotate Mode",        0, 0, kKeyCode_E, 0, false},
        {"Transform Tool", "Scale Mode",         0, 0, kKeyCode_R, 0, false},
        {"Transform Tool", "Toggle World/Local", 0, 0, kKeyCode_X, 0, false},

        // --- Hierarchy Panel (hardcoded) ----------------------------------
        {"Hierarchy", "Duplicate", 0, 0, kKeyCode_D,      kDefaultPrimaryShortcutModifier, false,
                                         kKeyCode_D,      kAlternatePrimaryShortcutModifier},
        {"Hierarchy", "Delete",    0, 0, kKeyCode_Delete, 0, false},

        // --- Assets Panel (hardcoded) -------------------------------------
        {"Assets", "Toggle Preview", 0, 0, kKeyCode_F, kModShift, false},
        {"Assets", "Rename",         0, 0, kKeyCode_F2, 0, false,
                                     kAssetsRenameAlternateKey, 0},

        // --- Asset Preview (editable wheel bindings; consumed by AssetViewPanel)
        {"Asset Preview", "Dolly In/Out",      0, 0, kShortcutKeyMouseScroll, 0,           false},
        {"Asset Preview", "Cycle Animation",   0, 0, kShortcutKeyMouseScroll, kModControl, false},

        // --- Script Editor (hardcoded; consumed by ScriptEditorPanel) -----
        {"Script Editor", "Save", 0, 0, kKeyCode_S, kDefaultPrimaryShortcutModifier, false,
                                   kKeyCode_S, kAlternatePrimaryShortcutModifier},

        // --- Animation Window (hardcoded) ---------------------------------
        {"Animation", "Save Clip",        0, 0, kKeyCode_S,      kDefaultPrimaryShortcutModifier | kModShift, false,
                                         kKeyCode_S,      kAlternatePrimaryShortcutModifier | kModShift},
        {"Animation", "Play/Pause",       0, 0, kKeyCode_Space,  0, false},
        {"Animation", "Frame Selected",   0, 0, kKeyCode_F,      0, false},
        {"Animation", "Frame All",        0, 0, kKeyCode_A,      0, false},
        {"Animation", "Previous Frame",   0, 0, kKeyCode_Left,   0, false},
        {"Animation", "Next Frame",       0, 0, kKeyCode_Right,  0, false},
        {"Animation", "Add Marker",       0, 0, kKeyCode_M,      0, false},
        {"Animation", "Insert Key",       0, 0, kKeyCode_I,      0, false},
        {"Animation", "Duplicate",        0, 0, kKeyCode_D,      0, false},
        {"Animation", "Delete Selected",  0, 0, kKeyCode_Delete, 0, false},

        // --- Node Graph (hardcoded) ---------------------------------------
        {"Node Graph", "Delete Nodes", 0, 0, kKeyCode_Delete, 0, false},
        {"Node Graph", "Copy Nodes",   0, 0, kKeyCode_C,      kDefaultPrimaryShortcutModifier, false,
                                         kKeyCode_C,      kAlternatePrimaryShortcutModifier},
        {"Node Graph", "Paste Nodes",  0, 0, kKeyCode_V,      kDefaultPrimaryShortcutModifier, false,
                                         kKeyCode_V,      kAlternatePrimaryShortcutModifier},
    };
    return kCatalog;
}

std::vector<ShortcutBinding> DefaultShortcutBindings(const ShortcutCatalogEntry& entry)
{
    std::vector<ShortcutBinding> out;
    if (entry.defaultKey != 0)
        out.push_back({entry.defaultKey, entry.defaultMods});
    if (entry.defaultKey2 != 0)
        out.push_back({entry.defaultKey2, entry.defaultMods2});
    return out;
}

std::vector<ShortcutBinding> LoadShortcutBindings(const SettingsStore& store,
                                              const ShortcutCatalogEntry& entry)
{
    std::vector<ShortcutBinding> out;
    const std::string key = ShortcutStoreKey(entry);
    const auto& j = store.Json();
    auto it = j.find(key);
    if (it == j.end())
        return DefaultShortcutBindings(entry);
    auto readOne = [&entry](const nlohmann::json& obj, ShortcutBinding& c) -> bool
    {
        if (!obj.is_object())
            return false;
        auto k = obj.find("key");
        auto m = obj.find("mods");
        if (k == obj.end() || !k->is_number_integer())
            return false;
        c.key = k->get<int>();
        c.mods = (m != obj.end() && m->is_number_integer()) ? m->get<int>() : 0;
        if (entry.scrollRequiresModifier &&
            c.key == kShortcutKeyMouseScroll && (c.mods & Input::kModShortcutMask) == 0)
            return false;
        return true;
    };
    if (it->is_array())
    {
        for (const auto& el : *it)
        {
            ShortcutBinding c;
            if (readOne(el, c))
                out.push_back(c);
        }
    }
    else if (it->is_object())
    {
        ShortcutBinding c;
        if (readOne(*it, c))
            out.push_back(c);
    }
    if (out.empty())
        return DefaultShortcutBindings(entry);
    return out;
}

void SaveShortcutBindings(SettingsStore& store,
                        const ShortcutCatalogEntry& entry,
                        const std::vector<ShortcutBinding>& bindings)
{
    const std::string key = ShortcutStoreKey(entry);
    const bool isDefault = bindings == DefaultShortcutBindings(entry);
    InvalidateBindingCache();
    if (isDefault || bindings.empty())
    {
        store.Remove(key);
    }
    else
    {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& c : bindings)
        {
            nlohmann::json v;
            v["key"] = c.key;
            v["mods"] = c.mods;
            arr.push_back(std::move(v));
        }
        store.SetJson(key, arr);
    }
    ShortcutBindingsSaved().Invoke(entry, LoadShortcutBindings(store, entry));
}

Event<const ShortcutCatalogEntry&, const std::vector<ShortcutBinding>&>& ShortcutBindingsSaved()
{
    static Event<const ShortcutCatalogEntry&, const std::vector<ShortcutBinding>&> saved;
    return saved;
}

void ApplyShortcutBindings(Input::InputSystem& input,
                         const ShortcutCatalogEntry& entry,
                         const std::vector<ShortcutBinding>& bindings)
{
    if (entry.context == 0 && entry.action == 0)
        return; // Panel-consumed / not yet wired through InputSystem.

    input.ClearBindings(entry.context, entry.action);
    for (const auto& c : bindings)
    {
        if (c.key == 0 || c.key == kShortcutKeyMouseScroll)
            continue;
        Input::ActionBinding b{};
        b.device = Input::DeviceType::Keyboard;
        b.code = c.key;
        b.scale = 1.0f;
        b.requiredMods = c.mods;
        b.forbiddenMods = Input::kModShortcutMask & ~c.mods;
        input.BindKey(entry.context, entry.action, b);
    }
}

const ShortcutCatalogEntry* FindShortcutConflict(const ShortcutCatalogEntry& self,
                                                 int key, int mods)
{
    if (key == 0)
        return nullptr;
    const auto& catalog = GetShortcutCatalog();
    const auto& cached = CachedCatalogBindings();
    for (size_t i = 0; i < catalog.size(); ++i)
    {
        const auto& e = catalog[i];
        if (&e == &self)
            continue;
        if (key == kShortcutKeyMouseScroll && std::strcmp(e.groupLabel, self.groupLabel) != 0)
            continue;
        if (e.readOnly)
        {
            if ((e.defaultKey == key && e.defaultMods == mods) ||
                (e.defaultKey2 == key && e.defaultKey2 != 0 && e.defaultMods2 == mods))
                return &e;
            continue;
        }
        for (const auto& c : cached[i])
        {
            if (c.key == key && c.mods == mods)
                return &e;
        }
    }
    return nullptr;
}

void ApplyShortcutOverridesFromPreferences(Input::InputSystem& input)
{
    UI::SetItemResizeGestureMatcher(&MatchesItemResizeGesture);
    auto store = OpenEditorPreferences();
    std::string err;
    store.Load(&err);
    for (const auto& entry : GetShortcutCatalog())
    {
        if (entry.readOnly)
            continue;
        const auto bindings = LoadShortcutBindings(store, entry);
        if (bindings == DefaultShortcutBindings(entry))
            continue; // defaults already registered during RegisterAction
        ApplyShortcutBindings(input, entry, bindings);
    }
}

namespace
{
bool AssetPreviewScrollMatchesNamedBinding(const char* displayName, int mods)
{
    const int normalizedMods = mods & Input::kModShortcutMask;
    const auto& catalog = GetShortcutCatalog();
    const auto& cached = CachedCatalogBindings();
    for (size_t i = 0; i < catalog.size(); ++i)
    {
        const auto& entry = catalog[i];
        if (std::strcmp(entry.groupLabel, "Asset Preview") != 0)
            continue;
        if (std::strcmp(entry.displayName, displayName) != 0)
            continue;
        for (const auto& binding : cached[i])
        {
            if (binding.key == kShortcutKeyMouseScroll &&
                (binding.mods & Input::kModShortcutMask) == normalizedMods)
                return true;
        }
        return false;
    }
    return false;
}
} // namespace

bool AssetPreviewScrollMatchesDolly(int mods)
{
    return AssetPreviewScrollMatchesNamedBinding("Dolly In/Out", mods);
}

bool AssetPreviewScrollMatchesCycleAnimation(int mods)
{
    return AssetPreviewScrollMatchesNamedBinding("Cycle Animation", mods);
}

bool MatchesCatalogShortcut(const char* groupLabel, const char* displayName,
                            int key, int mods)
{
    if (!groupLabel || !displayName || key == 0)
        return false;
    if (ShouldSuppressEditorShortcutActions())
        return false;

    const int normalizedMods = mods & Input::kModShortcutMask;
    const auto& catalog = GetShortcutCatalog();
    const auto& cached = CachedCatalogBindings();
    for (size_t i = 0; i < catalog.size(); ++i)
    {
        const auto& entry = catalog[i];
        if (std::strcmp(entry.groupLabel, groupLabel) != 0)
            continue;
        if (std::strcmp(entry.displayName, displayName) != 0)
            continue;
        for (const auto& binding : cached[i])
        {
            if (binding.key == key &&
                (binding.mods & Input::kModShortcutMask) == normalizedMods)
                return true;
        }
        return false;
    }
    return false;
}

bool MatchesItemResizeGesture(int mods)
{
    return MatchesCatalogShortcut(kItemResizeGroupLabel, kItemResizeDisplayName,
                                  kShortcutKeyMouseScroll, mods);
}

const ShortcutCatalogEntry& ItemResizeGestureCatalogEntry()
{
    return GetShortcutCatalog()[ItemResizeGestureCatalogIndex()];
}

const std::vector<ShortcutBinding>& ItemResizeGestureBindings()
{
    return CachedCatalogBindings()[ItemResizeGestureCatalogIndex()];
}

bool MatchesCatalogShortcutKeyOnly(const char* groupLabel, const char* displayName, int key)
{
    if (!groupLabel || !displayName || key == 0)
        return false;

    const auto& catalog = GetShortcutCatalog();
    const auto& cached = CachedCatalogBindings();
    for (size_t i = 0; i < catalog.size(); ++i)
    {
        const auto& entry = catalog[i];
        if (std::strcmp(entry.groupLabel, groupLabel) != 0)
            continue;
        if (std::strcmp(entry.displayName, displayName) != 0)
            continue;
        for (const auto& binding : cached[i])
        {
            if (binding.key == key)
                return true;
        }
        return false;
    }
    return false;
}

void SetShortcutCaptureActive(bool active)
{
    if (active)
        g_ShortcutCaptureArmed.fetch_add(1);
    else
    {
        int prev = g_ShortcutCaptureArmed.load();
        while (prev > 0 &&
               !g_ShortcutCaptureArmed.compare_exchange_weak(prev, prev - 1))
        {
        }
    }
}

void NoteShortcutCaptureConsumedKey()
{
    g_ShortcutCaptureConsumedKey.store(true);
}

bool ShouldSuppressEditorShortcutActions()
{
    return g_ShortcutCaptureArmed.load() > 0 || g_ShortcutCaptureConsumedKey.load();
}

void ClearShortcutCaptureConsumedKey()
{
    g_ShortcutCaptureConsumedKey.store(false);
}

bool AllowShortcutActionsThisFrame()
{
    const bool suppress = ShouldSuppressEditorShortcutActions();
    ClearShortcutCaptureConsumedKey();
    return !suppress;
}
} // namespace GameEngine::Editor
