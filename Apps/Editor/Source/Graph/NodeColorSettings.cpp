#include "Graph/NodeColorSettings.h"

#include "Editor/Settings/SettingsStore.h"
#include "Graph/GraphNodeRegistry.h"

#include <nlohmann/json.hpp>

#include <mutex>
#include <string>
#include <unordered_map>

namespace GameEngine
{
namespace NodeColorSettings
{
namespace
{
constexpr const char* kPrefKeyNodeColors = "ui.nodeGraph.nodeTypeColors";
constexpr const char* kPrefKeyNodeBodyColor = "ui.nodeGraph.nodeBodyColor";
constexpr const char* kPrefKeyCanvasColor = "ui.nodeGraph.canvasColor";
constexpr uint32_t kDefaultNodeColor = 0xFF3C3C3Cu;
// Midpoint between the previous (0xFF2A2A2C) and brighter (0xFF3A3A3C) body fills.
constexpr uint32_t kDefaultNodeBodyColor = 0xFF323234u;
// Canvas behind the nodes, painted by GraphCanvas itself (the element's CSS
// background never shows through that rect).
constexpr uint32_t kDefaultCanvasColor = 0xFF222222u;

std::mutex g_Mutex;
bool g_Loaded = false;
std::unordered_map<std::string, uint32_t> g_Overrides;

bool g_BodyLoaded = false;
uint32_t g_BodyColor = kDefaultNodeBodyColor;

bool g_CanvasLoaded = false;
uint32_t g_CanvasColor = kDefaultCanvasColor;

void LoadBodyLocked()
{
    if (g_BodyLoaded)
        return;

    g_BodyLoaded = true;
    g_BodyColor = kDefaultNodeBodyColor;

    auto prefs = Editor::OpenEditorPreferences();
    prefs.Load(nullptr);

    int64_t stored = 0;
    if (prefs.TryGetInt64(kPrefKeyNodeBodyColor, stored))
        g_BodyColor = 0xFF000000u | (static_cast<uint32_t>(stored) & 0x00FFFFFFu);
}

void LoadCanvasLocked()
{
    if (g_CanvasLoaded)
        return;

    g_CanvasLoaded = true;
    g_CanvasColor = kDefaultCanvasColor;

    auto prefs = Editor::OpenEditorPreferences();
    prefs.Load(nullptr);

    int64_t stored = 0;
    if (prefs.TryGetInt64(kPrefKeyCanvasColor, stored))
        g_CanvasColor = 0xFF000000u | (static_cast<uint32_t>(stored) & 0x00FFFFFFu);
}

std::string Key(std::string_view kindId, const std::string& typeId)
{
    return std::string(kindId) + ":" + typeId;
}

// Returns the color for an exact category name, or kDefaultNodeColor if unmapped.
uint32_t ExactCategoryColor(const std::string& category)
{
    if (category == "Flow")
        return 0xFF34526Eu;
    if (category == "Logic")
        return 0xFF4E426Eu;
    if (category == "Variables" || category == "Parameters")
        return 0xFF3F5F52u;
    if (category == "Input")
        return 0xFF51445Fu;
    if (category == "Sampling")
        return 0xFF2F635Fu;
    if (category == "Math")
        return 0xFF384F63u;
    if (category == "Math/Basic")
        return 0xFF2D4A61u;
    if (category == "Math/Derivative")
        return 0xFF3A5570u;
    if (category == "Math/Vector")
        return 0xFF30506Au;
    if (category == "Vector")
        return 0xFF3D5961u;
    if (category == "Motion")
        return 0xFF2E5F63u;
    if (category == "Transform")
        return 0xFF6A482Eu;
    if (category == "Utility")
        return 0xFF4D5643u;
    if (category == "Color")
        return 0xFF5C3F48u;
    if (category == "Organization")
        return 0xFF454545u;
    if (category == "Output")
        return 0xFF633C3Cu;
    if (category == "Pose")
        return 0xFF34526Eu;
    if (category == "IK")
        return 0xFF8A5A32u;
    return kDefaultNodeColor;
}

// Node categories may be hierarchical (e.g. "Math/Vector", "Input/Constants").
// Match the full name first, then fall back to the top-level segment so
// sub-categories inherit their parent's color instead of the default gray.
uint32_t CategoryColor(const std::string& category)
{
    const uint32_t exact = ExactCategoryColor(category);
    if (exact != kDefaultNodeColor)
        return exact;

    const std::size_t slash = category.find('/');
    if (slash != std::string::npos)
        return ExactCategoryColor(category.substr(0, slash));

    return kDefaultNodeColor;
}

void LoadLocked()
{
    if (g_Loaded)
        return;

    g_Loaded = true;
    g_Overrides.clear();

    auto prefs = Editor::OpenEditorPreferences();
    prefs.Load(nullptr);

    const auto& root = prefs.Json();
    if (!root.is_object())
        return;
    auto it = root.find(kPrefKeyNodeColors);
    if (it == root.end() || !it->is_object())
        return;

    for (auto kv = it->begin(); kv != it->end(); ++kv)
    {
        try
        {
            if (kv.value().is_number_integer() || kv.value().is_number_unsigned())
                g_Overrides[kv.key()] = static_cast<uint32_t>(kv.value().get<uint64_t>());
        }
        catch (...)
        {
        }
    }
}

void SaveLocked()
{
    auto prefs = Editor::OpenEditorPreferences();
    prefs.Load(nullptr);

    nlohmann::json colors = nlohmann::json::object();
    for (const auto& [key, color] : g_Overrides)
        colors[key] = static_cast<uint64_t>(color);

    if (colors.empty())
        prefs.Remove(kPrefKeyNodeColors);
    else
        prefs.SetJson(kPrefKeyNodeColors, colors);
    prefs.Save(nullptr);
}
} // namespace

uint32_t DefaultNodeColorArgb(std::string_view kindId, const std::string& typeId)
{
    if (const NodeTypeMeta* meta = GraphNodeRegistry::Get().Find(kindId, typeId))
        return CategoryColor(meta->Category);
    return kDefaultNodeColor;
}

uint32_t GetNodeColorArgb(std::string_view kindId, const std::string& typeId)
{
    std::lock_guard lock(g_Mutex);
    LoadLocked();
    auto it = g_Overrides.find(Key(kindId, typeId));
    if (it != g_Overrides.end())
        return 0xFF000000u | (it->second & 0x00FFFFFFu);
    return DefaultNodeColorArgb(kindId, typeId);
}

void SetNodeColorArgb(std::string_view kindId, const std::string& typeId, uint32_t argb)
{
    std::lock_guard lock(g_Mutex);
    LoadLocked();
    g_Overrides[Key(kindId, typeId)] = 0xFF000000u | (argb & 0x00FFFFFFu);
    SaveLocked();
}

void ResetNodeColor(std::string_view kindId, const std::string& typeId)
{
    std::lock_guard lock(g_Mutex);
    LoadLocked();
    g_Overrides.erase(Key(kindId, typeId));
    SaveLocked();
}

uint32_t DefaultNodeBodyColorArgb()
{
    return kDefaultNodeBodyColor;
}

uint32_t GetNodeBodyColorArgb()
{
    std::lock_guard lock(g_Mutex);
    LoadBodyLocked();
    return g_BodyColor;
}

namespace
{
uint32_t DarkenedBodyArgb(uint32_t percent)
{
    const uint32_t body = GetNodeBodyColorArgb();
    auto darken = [percent](uint32_t c) { return static_cast<uint32_t>(c * percent / 100); };
    return 0xFF000000u | (darken((body >> 16) & 0xFF) << 16) |
           (darken((body >> 8) & 0xFF) << 8) | darken(body & 0xFF);
}
} // namespace

uint32_t GetNodeValueSurfaceColorArgb() { return DarkenedBodyArgb(70); }

uint32_t GetNodePlateColorArgb() { return DarkenedBodyArgb(85); }

uint32_t DefaultCanvasColorArgb()
{
    return kDefaultCanvasColor;
}

uint32_t GetCanvasColorArgb()
{
    std::lock_guard lock(g_Mutex);
    LoadCanvasLocked();
    return g_CanvasColor;
}

void SetCanvasColorArgb(uint32_t argb)
{
    std::lock_guard lock(g_Mutex);
    g_CanvasColor = 0xFF000000u | (argb & 0x00FFFFFFu);
    g_CanvasLoaded = true;

    auto prefs = Editor::OpenEditorPreferences();
    prefs.Load(nullptr);
    prefs.SetInt64(kPrefKeyCanvasColor, static_cast<int64_t>(g_CanvasColor));
    prefs.Save(nullptr);
}

void ResetCanvasColor()
{
    std::lock_guard lock(g_Mutex);
    g_CanvasColor = kDefaultCanvasColor;
    g_CanvasLoaded = true;

    auto prefs = Editor::OpenEditorPreferences();
    prefs.Load(nullptr);
    prefs.Remove(kPrefKeyCanvasColor);
    prefs.Save(nullptr);
}

void SetNodeBodyColorArgb(uint32_t argb)
{
    std::lock_guard lock(g_Mutex);
    g_BodyColor = 0xFF000000u | (argb & 0x00FFFFFFu);
    g_BodyLoaded = true;

    auto prefs = Editor::OpenEditorPreferences();
    prefs.Load(nullptr);
    prefs.SetInt64(kPrefKeyNodeBodyColor, static_cast<int64_t>(g_BodyColor));
    prefs.Save(nullptr);
}

void ResetNodeBodyColor()
{
    std::lock_guard lock(g_Mutex);
    g_BodyColor = kDefaultNodeBodyColor;
    g_BodyLoaded = true;

    auto prefs = Editor::OpenEditorPreferences();
    prefs.Load(nullptr);
    prefs.Remove(kPrefKeyNodeBodyColor);
    prefs.Save(nullptr);
}

void Reload()
{
    std::lock_guard lock(g_Mutex);
    g_Loaded = false;
    g_Overrides.clear();
    g_BodyLoaded = false;
    g_CanvasLoaded = false;
    LoadLocked();
}
} // namespace NodeColorSettings
} // namespace GameEngine
