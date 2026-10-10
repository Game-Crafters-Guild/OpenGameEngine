#include "Panels/InspectorPanel.h"
#include "VersionControl/EditorVersionControlService.h"
#include "VersionControl/Ui/InspectorVcsController.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Platform/SystemMetrics.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Inspectors/AnimationPreviewManager.h"
#include "Logger/Logger.h"
#include "Panels/ScriptEditorPanel.h" // For ScriptVariable, ScriptMethod
#include "Panels/OnDiskFileName.h"
#include "Panels/ScriptVariablesEmptyState.h"

#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <type_traits>

#if defined(__GNUG__)
#include <cxxabi.h>
#endif

#include "Editor/EditorTreeTitleIconVars.h"
#include "Editor/Hierarchy/HierarchyEntityIcon.h"
#include "Editor/Settings/SettingsStore.h"
#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "Editor/Hierarchy/HierarchyEnableState.h"
#include "EditorContext.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "Platform/Capabilities.h"
#include "Platform/Clipboard.h"
#include "Platform/ContextMenu.h"
#include "Platform/Window.h"
#include "PlayMode/PlayModeManager.h"
#include "Scene/WorldSnapshotCommand.h"
#include "Types/StringUtils.h"

#include "Panels/ConfirmActionModal.h"
#include "Panels/SettingsPanel.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/SearchDialog.h"
#include "UI/Controls/TextField.h"
#include "UI/Controls/Toggle.h"
#include "UI/EditorIcons.h"
#include "UI/PanelSearchBar.h"
#include "UI/SmartFolder/SmartFolder.h"
#include "UI/SmartFolder/SmartFolderInspector.h"
#include "UI/SmartFolder/SmartFolderManager.h"
#include "UI/StyleProperties.h"
#include "UndoRedo/UndoRedoService.h"

#include "Thumbnails/IThumbnailProvider.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include "InspectorRegistry.h"
#include "Inspectors/ComponentPresets.h"
#include "Inspectors/GeneratedEntityNotice.h"
#include "Inspectors/InspectorEntityActivity.h"
#include "Graph/GraphInspectorRows.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorSectionHosts.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/PreservedFieldNotice.h"
#include "UI/InspectorSection.h"
#include "UI/PickerQueryFilter.h"

#include "EditorChangeNotifications.h"
#include "UndoRedo/IEditorCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include "AssetCore/Asset.h"
#include "AssetCore/AssetEvents.h"
#include "AssetCore/AssetTypes.h"
#include "Assets/AssetCreation.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Audio/AudioSystem.h"
#include "Components/Hierarchy.h"
#include "Components/Measure/MeasureComponent.h"
#include "Components/Name.h"
#include "Components/Rendering/Camera.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/LightPhotometry.h"
#include "Components/Rendering/MeshGPUData.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/SkinnedMeshRenderer.h"
#include "Components/RuntimeOnlyEntity.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "Engine/Rendering/RenderServices.h"
#include "ECS/ComponentRegistry.h"
#include "ECS/Components.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/UnresolvedComponentStore.h"  // preserved (unloaded) component rows
#include "ECS/World.h"

// Physics authoring components (for minimal Add Component UX)
#include "PhysicsECS/Components/BoxColliderShape.h"
#include "PhysicsECS/Components/CapsuleColliderShape.h"
#include "PhysicsECS/Components/CharacterController.h"
#include "PhysicsECS/Components/HeightFieldColliderShape.h"
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/PhysicsColliderOwner.h"
#include "PhysicsECS/Components/PhysicsWorldSettingsComponent.h"
#include "PhysicsECS/Components/PlaneColliderShape.h"
#include "PhysicsECS/Components/SphereColliderShape.h"

#include "Components/Animation/AnimatedNodeRef.h"
#include "Components/Animation/AnimatedSprite2D.h"
#include "Components/Animation/Animator.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/HumanoidRetargeterComponent.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Animation/ValueCurve.h"
#include "Components/Audio/AudioEmitter.h"
#include "Components/Audio/AudioListener.h"
#include "Components/Rendering/LODGroup.h"
#include "Components/Rendering/LensFlareSource.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/Ocean.h"
#include "Components/Rendering/Particles.h"
// Post-process effect chrome comes from PostProcessEffectRegistry; the typed
// includes that remain serve the scanner-unreflectable Enabled fallbacks
// (VolumetricFog, HeightFog, AtmosphericCloudLayer), the LDR StackOrder sync
// (ColorFilter, CAS, CRT, CubeLut, Vignette), and HeightFog's add-time tuning.
#include "Components/Rendering/PostProcessEffects/AtmosphericCloudLayer.h"
#include "Components/Rendering/PostProcessEffects/VolumetricClouds.h"
#include "Components/Rendering/PostProcessEffects/ColorFilterEffect.h"
#include "Components/Rendering/PostProcessEffects/ContrastAdaptiveSharpenEffect.h"
#include "Components/Rendering/PostProcessEffects/CrtEffect.h"
#include "Components/Rendering/PostProcessEffects/CubeLutEffect.h"
#include "Components/Rendering/PostProcessEffects/VignetteEffect.h"
#include "Components/Rendering/ReflectionProbe.h"
#include "Components/Rendering/PostProcessEffects/HeightFogEffect.h"
#include "Components/Rendering/PostProcessEffects/VolumetricFogEffect.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "Components/Rendering/RenderLayer.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/Skybox.h"
#include "Components/Rendering/WindVolume.h"
#include "Components/SceneBlueprintInstance.h"
#include "Components/SceneEntityTag.h"
#include "Components/SceneSubsceneInstance.h"
#include "Components/Spline/SplineComponent.h"
#include "Components/Terrain/Terrain.h"
#include "Components/Terrain/TerrainGrass.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "Components/Terrain/TerrainModifierVolume.h"
#include "Components/Terrain/TerrainModifiers.h"
#include "Components/Terrain/TerrainPlanetRelief.h"
#include "Components/Video/VideoTextureComponent.h"
#include "Editor/Entities/ComponentEnabledToggle.h"
#include "Editor/Entities/ComponentRemoval.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "Editor/Entities/EntityDisplayName.h"
#include "Editor/Entities/EditorInspectorPseudoComponents.h"
#include "Engine/Rendering/PostProcessEffectRegistry.h"
#include "TerrainECS/TerrainModifierComponents.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/DefaultComponentInspector.h"
#include "ECS/ComponentFactory.h"
#include "ECS/ComponentFieldRegistry.h"

#include <unordered_set>

namespace GameEngine
{

using namespace ECS;
using namespace Components;
using InspectorUI::AddTextBlock;

// The Add Component preset vocabulary, its catalog and the table pairing a
// terrain preset with its effect live in Inspectors/ComponentPresets.h; this
// panel is their UI.
using Editor::ComponentPreset;
using Editor::ComponentPresetCatalog;
using Editor::ComponentPresetEntry;
using Editor::ComponentTypeIdsForPreset;
using Editor::PresetUndoDisplayName;

// Static counter for unique inspector instance IDs
int InspectorPanel::s_InspectorInstanceCounter = 0;

std::size_t MaterialSlotOrderKeyHash::operator()(const MaterialSlotOrderKey& k) const
{
    const std::size_t h1 = std::hash<uint32_t>{}(k.EntityPacked);
    const std::size_t h2 = std::hash<GUID>{}(k.ModelGuid);
    return h1 ^ (h2 + 0x9e3779b97f4a7c15ULL + (h1 << 6) + (h1 >> 2));
}

namespace
{

// Icon for an Add Component category submenu row.
const char* CategoryIcon(const std::string& category)
{
    if (category == "Animation") return EditorIcons::kFilm;
    if (category == "Audio") return EditorIcons::kMusicNote;
    if (category == "Physics") return EditorIcons::kPhysics;
    if (category == "Rendering") return EditorIcons::kMaterial;
    if (category == "Spline") return EditorIcons::kSplineCurve;
    if (category == "Terrain") return EditorIcons::kTerrain;
    return EditorIcons::kPlus;
}


// Small UIElement that invokes a callback on left-mouse-up inside its bounds.
// Used to make the inspector header icon clickable so it can reveal the
// underlying asset (texture / model) in the Assets panel.
class InspectorHeaderIconClickable final : public UIElement
{
  public:
    using ClickFn = std::function<void()>;
    explicit InspectorHeaderIconClickable(ClickFn onClick)
        : m_OnClick(std::move(onClick)) {}

    void OnEvent(UIEvent& e) override
    {
        if (e.Id == kEventMouseDown && e.Button == 0)
        {
            m_Armed = true;
            e.Capture(this);
            e.Stop();
            return;
        }
        if (e.Id == kEventMouseUp && e.Button == 0 && m_Armed)
        {
            m_Armed = false;
            const float lx = GetLayoutX();
            const float ly = GetLayoutY();
            const float lw = GetLayoutWidth();
            const float lh = GetLayoutHeight();
            const bool inside = (e.X >= lx && e.Y >= ly && e.X < lx + lw && e.Y < ly + lh);
            if (inside && m_OnClick)
                m_OnClick();
            e.Stop();
            return;
        }
    }

  private:
    ClickFn m_OnClick;
    bool m_Armed = false;
};

static bool GetInspectorShowInfoCardsPreference()
{
    bool showInfoCards = true;
    Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);
    prefs.TryGetBool("ui.inspectorShowInfoCards", showInfoCards);
    return showInfoCards;
}

static float ParseGraphColorParam(const std::unordered_map<std::string, std::string>& parameters,
                                  const char* key, float fallback)
{
    auto it = parameters.find(key);
    if (it == parameters.end())
        return fallback;
    try
    {
        size_t consumed = 0;
        const float value = std::stof(it->second, &consumed);
        return consumed > 0 ? value : fallback;
    }
    catch (...)
    {
        return fallback;
    }
}

static std::string FormatGraphColorParam(float value)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", value);
    return buf;
}

static bool TryParseGraphFloatParam(const std::string& text, float& out)
{
    return FloatField::TryParseFloat(text, out);
}

static bool TryParseGraphIntParam(const std::string& text, int& out)
{
    if (text.empty())
        return false;
    const char* begin = text.c_str();
    char* end = nullptr;
    long value = std::strtol(begin, &end, 10);
    if (end == begin)
        return false;
    while (*end == ' ' || *end == '\t')
        ++end;
    if (*end != '\0')
        return false;
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
        return false;
    out = static_cast<int>(value);
    return true;
}

static std::string FormatGraphFloatParam(float value)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", value);
    return buf;
}

static uint32_t GraphColorToArgb(float r, float g, float b, float a = 1.0f, float intensity = 1.0f)
{
    auto toByte = [](float v) -> uint32_t
    {
        const float clamped = std::max(0.0f, std::min(1.0f, v));
        return static_cast<uint32_t>(clamped * 255.0f + 0.5f);
    };
    const float denom = std::max(1.0f, intensity);
    const uint32_t aa = toByte(a);
    const uint32_t rr = toByte(r / denom);
    const uint32_t gg = toByte(g / denom);
    const uint32_t bb = toByte(b / denom);
    return (aa << 24) | (rr << 16) | (gg << 8) | bb;
}

static void ArgbIntensityToGraphColor(uint32_t argb, float intensity, float& r, float& g, float& b)
{
    const float scale = std::max(1.0f, intensity) / 255.0f;
    r = static_cast<float>((argb >> 16) & 0xFF) * scale;
    g = static_cast<float>((argb >> 8) & 0xFF) * scale;
    b = static_cast<float>(argb & 0xFF) * scale;
}

static void StyleGraphColorSwatch(UIElement* swatch, uint32_t argb)
{
    if (!swatch)
        return;
    swatch->Overrides()
        .Set(Style::Width, StyleLength::Px(20.0f))
        .Set(Style::Height, StyleLength::Px(20.0f))
        .Set(Style::MinWidth, StyleLength::Px(20.0f))
        .Set(Style::MinHeight, StyleLength::Px(20.0f))
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{3.0f, 3.0f, 3.0f, 3.0f})
        .Set(Style::BorderWidth, Box4{1.0f, 1.0f, 1.0f, 1.0f})
        .Set(Style::BorderColor, BorderColorsTRBL{0xFF555555u, 0xFF555555u, 0xFF555555u, 0xFF555555u})
        .Set(Style::BackgroundColor, argb)
        .Set(Style::Cursor, CursorStyle::Pointer);
}

static std::string FormatGraphVariableVec3(float r, float g, float b)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6g, %.6g, %.6g", r, g, b);
    return buf;
}

static std::string FormatGraphVariableComponents(const std::vector<float>& values)
{
    std::string out;
    for (size_t i = 0; i < values.size(); ++i)
    {
        if (i > 0)
            out += ", ";
        out += FormatGraphFloatParam(values[i]);
    }
    return out;
}

static void AddGraphVariableNameRow(UIElement* parent, const std::string& variableName)
{
    if (!parent || variableName.empty())
        return;

    auto row = std::make_unique<UIElement>();
    row->AddClass("inspector-row");

    auto label = std::make_unique<Label>();
    label->AddClass("inspector-label");
    label->SetText("Variable");
    row->AddChild(std::move(label));

    auto value = std::make_unique<Label>();
    value->SetText(variableName);
    value->AddClass("inspector-field");
    row->AddChild(std::move(value));

    parent->AddChild(std::move(row));
}

static void AddGraphColorSwatchRow(UIElement* parent,
                                   const std::string& labelText,
                                   float r,
                                   float g,
                                   float b,
                                   OpenColorPickerWindowFn openPicker,
                                   std::function<void(uint32_t argb, float intensity, bool commitUndo)> applyColor)
{
    if (!parent)
        return;

    const float intensity = std::max(1.0f, std::max(r, std::max(g, b)));
    const uint32_t argb = GraphColorToArgb(r, g, b, 1.0f, intensity);

    auto row = std::make_unique<UIElement>();
    row->AddClass("inspector-row");

    auto label = std::make_unique<Label>();
    label->AddClass("inspector-label");
    label->SetText(labelText);
    row->AddChild(std::move(label));

    auto fieldContainer = std::make_unique<UIElement>();
    fieldContainer->AddClass("inspector-field");
    fieldContainer->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::Gap, StyleLength::Px(6.0f));
    UIElement* fieldContainerRaw = fieldContainer.get();

    auto swatch = std::make_unique<UIElement>();
    UIElement* swatchRaw = swatch.get();
    StyleGraphColorSwatch(swatchRaw, argb);
    fieldContainerRaw->AddChild(std::move(swatch));

    auto applyWithSwatch = [applyColor = std::move(applyColor), swatchRaw](uint32_t newArgb, float newIntensity, bool commitUndo)
    {
        if (applyColor)
            applyColor(newArgb, newIntensity, commitUndo);
        float nr = 1.0f;
        float ng = 1.0f;
        float nb = 1.0f;
        ArgbIntensityToGraphColor(newArgb, newIntensity, nr, ng, nb);
        StyleGraphColorSwatch(swatchRaw, GraphColorToArgb(nr, ng, nb, 1.0f, std::max(1.0f, newIntensity)));
    };

    auto clickHandler = [argb, intensity, openPicker, applyWithSwatch](UIEvent& e)
    {
        if (e.Button != 0)
            return;
        e.Stop();
        if (!openPicker)
            return;
        ColorPickerCallbacks cbs;
        cbs.onApply = [applyWithSwatch](uint32_t newArgb, float newIntensity)
        {
            applyWithSwatch(newArgb, newIntensity, true);
        };
        cbs.onCancel = [applyWithSwatch, argb, intensity]()
        {
            applyWithSwatch(argb, intensity, true);
        };
        cbs.onValueChanging = [applyWithSwatch](uint32_t newArgb, float newIntensity)
        {
            applyWithSwatch(newArgb, newIntensity, false);
        };
        openPicker(argb, intensity, std::move(cbs));
    };

    swatchRaw->RegisterEventHandler(kEventMouseDown, clickHandler);

    row->AddChild(std::move(fieldContainer));
    parent->AddChild(std::move(row));
}

static void AddGraphColorParameterRow(UIElement* parent,
                                      const std::string& nodeId,
                                      const std::unordered_map<std::string, std::string>& parameters,
                                      OpenColorPickerWindowFn openPicker,
                                      const std::function<void(const std::string&, const std::string&, const std::string&, bool)>& onChanged)
{
    if (!parent)
        return;

    float r = ParseGraphColorParam(parameters, "r", 1.0f);
    float g = ParseGraphColorParam(parameters, "g", 1.0f);
    float b = ParseGraphColorParam(parameters, "b", 1.0f);

    AddGraphColorSwatchRow(parent, "Color", r, g, b, openPicker,
                           [nodeId, onChanged](uint32_t newArgb, float newIntensity, bool commitUndo)
                           {
                               float nr = 1.0f;
                               float ng = 1.0f;
                               float nb = 1.0f;
                               ArgbIntensityToGraphColor(newArgb, newIntensity, nr, ng, nb);
                               if (onChanged)
                               {
                                   onChanged(nodeId, "r", FormatGraphColorParam(nr), false);
                                   onChanged(nodeId, "g", FormatGraphColorParam(ng), false);
                                   onChanged(nodeId, "b", FormatGraphColorParam(nb), commitUndo);
                               }
                           });
}

static void AddGraphVariableColorRow(UIElement* parent,
                                     const std::string& variableName,
                                     float r,
                                     float g,
                                     float b,
                                     OpenColorPickerWindowFn openPicker,
                                     const std::function<void(const std::string&, const std::string&, bool)>& onVariableChanged)
{
    if (!parent || variableName.empty())
        return;

    AddGraphColorSwatchRow(parent, "Color", r, g, b, openPicker,
                           [variableName, onVariableChanged](uint32_t newArgb, float newIntensity, bool commitUndo)
                           {
                               float nr = 1.0f;
                               float ng = 1.0f;
                               float nb = 1.0f;
                               ArgbIntensityToGraphColor(newArgb, newIntensity, nr, ng, nb);
                               if (onVariableChanged)
                                   onVariableChanged(variableName, FormatGraphVariableVec3(nr, ng, nb), commitUndo);
                           });
}

constexpr uint32_t kCmdInspectorRemoveComponent = 0x5201;
// Preset ordinal is ComponentPreset cast to uint32_t (distinct from clipboard/settings ranges).
constexpr uint32_t kCmdInspectorAddPresetBase = 0x5300;
constexpr uint32_t kCmdInspectorClearMeshRendererMaterial = 0x5226;
constexpr uint32_t kCmdInspectorCopyComponent = 0x5240;
constexpr uint32_t kCmdInspectorPasteComponent = 0x5241;
constexpr uint32_t kCmdInspectorSaveComponentSettings = 0x5242;
constexpr uint32_t kCmdInspectorRestoreLastSettings = 0x5243;
constexpr uint32_t kCmdInspectorResetComponentValues = 0x5244;
constexpr uint32_t kCmdInspectorRestoreSavedSettingsBase = 0x5260;
constexpr uint32_t kCmdInspectorRestoreSavedSettingsMax = 16;
constexpr size_t kMaxSavedComponentSettingsPerType = 24;
constexpr size_t kMaxSavedComponentSettingsMenuItems = 10;
constexpr const char* kProjectInspectorSavedSettingsKey = "inspector.componentSavedSettings";

// Add Component / Add Post FX share one picker width — the picker is as wide as
// the button that opens it.
constexpr float kInspectorPickerWidthPx = 360.0f;

// Horizontal inset for asset inspectors (script, texture, material, fallback, etc.).
constexpr float kInspectorAssetContentHorizontalInsetPx = 16.0f;
// Vertical inset when framing script-variable empty state (matches type-line spacing).
constexpr float kInspectorAssetContentVerticalInsetPx = 8.0f;
static std::unique_ptr<UIElement> MakeInspectorPaddedContentColumn()
{
    auto column = std::make_unique<UIElement>();
    column->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::PaddingLeft, StyleLength::Px(kInspectorAssetContentHorizontalInsetPx))
        .Set(Style::PaddingRight, StyleLength::Px(kInspectorAssetContentHorizontalInsetPx))
        // Top gap comes from `.inspector-scrollview .scroll-content` (8px); avoid stacking extra top padding.
        .Set(Style::PaddingTop, StyleLength::Px(0.0f))
        .Set(Style::PaddingBottom, StyleLength::Px(kInspectorAssetContentVerticalInsetPx));
    return column;
}

template <typename T, typename = std::enable_if_t<std::is_base_of_v<UIElement, T>>>
static void AddInspectorPaddedContent(UIElement* contentRoot, std::unique_ptr<T> inner)
{
    auto wrap = std::make_unique<UIElement>();
    wrap->AddClass("inspector-asset-content");
    wrap->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::PaddingLeft, StyleLength::Px(kInspectorAssetContentHorizontalInsetPx))
        .Set(Style::PaddingRight, StyleLength::Px(kInspectorAssetContentHorizontalInsetPx));
    wrap->AddChild(std::unique_ptr<UIElement>(inner.release()));
    contentRoot->AddChild(std::move(wrap));
}

// Get default value string based on C# type name
std::string GetDefaultValueForType(const std::string& typeName)
{
    if (typeName == "int" || typeName == "Int32" || typeName == "long" || typeName == "Int64" ||
        typeName == "short" || typeName == "Int16" || typeName == "byte" || typeName == "Byte")
        return "0";
    if (typeName == "float" || typeName == "Single")
        return "0f";
    if (typeName == "double" || typeName == "Double")
        return "0.0";
    if (typeName == "bool" || typeName == "Boolean")
        return "false";
    if (typeName == "string" || typeName == "String")
        return "\"\"";
    if (typeName == "Vector2")
        return "new Vector2(0, 0)";
    if (typeName == "Vector3")
        return "new Vector3(0, 0, 0)";
    if (typeName == "Vector4" || typeName == "Quaternion")
        return "new " + typeName + "(0, 0, 0, 0)";
    if (typeName == "Color")
        return "new Color(1, 1, 1, 1)";
    // Default for unknown types
    return "0";
}

static void StripLeading(std::string& s, const char* prefix)
{
    if (!prefix)
        return;
    const size_t n = std::strlen(prefix);
    if (s.size() >= n && s.compare(0, n, prefix) == 0)
    {
        s.erase(0, n);
    }
}

static std::string PrettifyTypeName(const char* typeName)
{
    std::string s = typeName ? std::string(typeName) : std::string();

#if defined(__GNUG__)
    int status = 0;
    if (char* demangled = abi::__cxa_demangle(s.c_str(), nullptr, nullptr, &status))
    {
        if (status == 0)
            s = demangled;
        std::free(demangled);
    }
#endif

    // MSVC often prefixes with "struct " / "class ".
    StripLeading(s, "struct ");
    StripLeading(s, "class ");

    // Strip common namespaces for UI display.
    StripLeading(s, "GameEngine::Components::");
    StripLeading(s, "GameEngine::ECS::");
    StripLeading(s, "GameEngine::");
    StripLeading(s, "Components::");
    StripLeading(s, "ECS::");

    std::string pretty;
    pretty.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i)
    {
        const unsigned char ch = static_cast<unsigned char>(s[i]);
        if (i > 0 && std::isupper(ch))
        {
            const unsigned char prev = static_cast<unsigned char>(s[i - 1]);
            const bool prevIsLowerOrDigit = std::islower(prev) || std::isdigit(prev);
            const bool nextIsLower = (i + 1 < s.size()) &&
                                     std::islower(static_cast<unsigned char>(s[i + 1]));
            if (prevIsLowerOrDigit || nextIsLower)
                pretty.push_back(' ');
        }
        pretty.push_back(s[i]);
    }

    return pretty.empty() ? std::string("Component") : pretty;
}

// Human-readable display names for known components (avoids showing mangled typeid names like N10GameEngine10components12MeshRendererE).
static const std::unordered_map<ECS::ComponentTypeId, std::string>& GetComponentDisplayNames()
{
    static std::unordered_map<ECS::ComponentTypeId, std::string> s_Names;
    static bool s_Init = false;
    if (!s_Init)
    {
        s_Init = true;
        s_Names[ECS::GetComponentTypeId<Transform>()] = "Transform";
        s_Names[ECS::GetComponentTypeId<Components::WorldTransform>()] = "World Transform";
        s_Names[ECS::GetComponentTypeId<Name>()] = "Name";
        s_Names[ECS::GetComponentTypeId<Parent>()] = "Parent";
        s_Names[ECS::GetComponentTypeId<MeshRenderer>()] = "Mesh Renderer";
        s_Names[ECS::GetComponentTypeId<SkinnedMeshRenderer>()] = "Skinned Mesh Renderer";
        s_Names[ECS::GetComponentTypeId<Camera>()] = "Camera";
        s_Names[ECS::GetComponentTypeId<Light>()] = "Light";
        s_Names[ECS::GetComponentTypeId<PhysicsBody>()] = "Physics Body";
        s_Names[ECS::GetComponentTypeId<CharacterController>()] = "Character Controller";
        s_Names[ECS::GetComponentTypeId<PhysicsCollider>()] = "Physics Collider";
        s_Names[ECS::GetComponentTypeId<PhysicsColliderOwner>()] = "Physics Collider Owner";
        s_Names[ECS::GetComponentTypeId<PhysicsWorldSettingsComponent>()] = "Physics World Settings";
        s_Names[ECS::GetComponentTypeId<BoxColliderShape>()] = "Box Collider";
        s_Names[ECS::GetComponentTypeId<SphereColliderShape>()] = "Sphere Collider";
        s_Names[ECS::GetComponentTypeId<PlaneColliderShape>()] = "Plane Collider";
        s_Names[ECS::GetComponentTypeId<CapsuleColliderShape>()] = "Capsule Collider";

        s_Names[ECS::GetComponentTypeId<LODGroup>()] = "LOD Group";
        s_Names[ECS::GetComponentTypeId<SkyEnvironment>()] = "Sky Environment";
        s_Names[ECS::GetComponentTypeId<Skybox>()] = "Skybox";
        s_Names[ECS::GetComponentTypeId<Components::ReflectionProbe>()] = "Reflection Probe";
        s_Names[ECS::GetComponentTypeId<PostProcessVolume>()] = "Post Process Volume";
        s_Names[ECS::GetComponentTypeId<Components::WindVolume>()] = "Wind Volume";
        // Post-process effect names come from the PostProcessEffectRegistry
        // descriptors (ComponentTitle consults it before this map).
        s_Names[ECS::GetComponentTypeId<RenderLayer>()] = "Render Layer";
        s_Names[ECS::GetComponentTypeId<LocalBounds>()] = "World Bounds";

        s_Names[ECS::GetComponentTypeId<AudioEmitter>()] = "Audio Emitter";
        s_Names[ECS::GetComponentTypeId<AudioListener>()] = "Audio Listener";

        s_Names[ECS::GetComponentTypeId<Animator>()] = "Animator";
        s_Names[ECS::GetComponentTypeId<ValueCurve>()] = "Value Curve";
        s_Names[ECS::GetComponentTypeId<AnimatedNodeRef>()] = "Animated Node";

        s_Names[ECS::GetComponentTypeId<SceneEntityTag>()] = "Scene Entity Tag";
        s_Names[ECS::GetComponentTypeId<SceneBlueprintInstance>()] = "Scene Blueprint Instance";
        s_Names[ECS::GetComponentTypeId<SceneSubsceneInstance>()] = "Scene Subscene Instance";

        s_Names[ECS::GetComponentTypeId<Components::Terrain>()] = "Terrain";
        s_Names[ECS::GetComponentTypeId<Components::TerrainPlanetRelief>()] = "Planet Relief";
        s_Names[ECS::GetComponentTypeId<Components::TerrainGrass>()] = "Terrain Grass";
        s_Names[ECS::GetComponentTypeId<Components::TerrainModifierVolume>()] = "Terrain Modifier Volume";
        // Terrain effect names come from the canonical effect list in TerrainECS
        // (ComponentTitle consults it before this map), as post-process effect
        // names come from the PostProcessEffectRegistry.
        s_Names[ECS::GetComponentTypeId<Components::SplineComponent>()] = "Spline";
        s_Names[ECS::GetComponentTypeId<Components::VideoTextureComponent>()] = "Video Texture";

        s_Names[ECS::GetComponentTypeId<Editor::MeshRendererMaterialInspectorSection>()] = "Material";
    }
    return s_Names;
}

static std::string ComponentCategory(ECS::ComponentTypeId typeId)
{
    if (Editor::EditorComponentTraits traits;
        Editor::EditorComponentTraitsRegistry::Get().TryGet(typeId, traits) && !traits.InspectorCategory.empty())
        return traits.InspectorCategory;
    static const ECS::ComponentTypeId transformId = ECS::GetComponentTypeId<Transform>();

    static const ECS::ComponentTypeId meshRendererId = ECS::GetComponentTypeId<MeshRenderer>();
    static const ECS::ComponentTypeId skinnedMeshRendererId = ECS::GetComponentTypeId<SkinnedMeshRenderer>();
    static const ECS::ComponentTypeId lodGroupId = ECS::GetComponentTypeId<LODGroup>();
    static const ECS::ComponentTypeId renderLayerId = ECS::GetComponentTypeId<RenderLayer>();
    static const ECS::ComponentTypeId skyEnvId = ECS::GetComponentTypeId<SkyEnvironment>();
    static const ECS::ComponentTypeId skyboxId = ECS::GetComponentTypeId<Skybox>();
    static const ECS::ComponentTypeId reflectionProbeId = ECS::GetComponentTypeId<Components::ReflectionProbe>();
    static const ECS::ComponentTypeId postProcessId = ECS::GetComponentTypeId<PostProcessVolume>();
    static const ECS::ComponentTypeId windVolumeId = ECS::GetComponentTypeId<Components::WindVolume>();
    static const ECS::ComponentTypeId videoTextureId = ECS::GetComponentTypeId<Components::VideoTextureComponent>();


    static const ECS::ComponentTypeId physicsBodyId = ECS::GetComponentTypeId<PhysicsBody>();
    static const ECS::ComponentTypeId physicsColliderId = ECS::GetComponentTypeId<PhysicsCollider>();
    static const ECS::ComponentTypeId physicsColliderOwnerId = ECS::GetComponentTypeId<PhysicsColliderOwner>();
    static const ECS::ComponentTypeId physicsWorldSettingsId = ECS::GetComponentTypeId<PhysicsWorldSettingsComponent>();
    static const ECS::ComponentTypeId boxColliderId = ECS::GetComponentTypeId<BoxColliderShape>();
    static const ECS::ComponentTypeId sphereColliderId = ECS::GetComponentTypeId<SphereColliderShape>();
    static const ECS::ComponentTypeId planeColliderId = ECS::GetComponentTypeId<PlaneColliderShape>();
    static const ECS::ComponentTypeId capsuleColliderId = ECS::GetComponentTypeId<CapsuleColliderShape>();

    static const ECS::ComponentTypeId audioEmitterId = ECS::GetComponentTypeId<AudioEmitter>();
    static const ECS::ComponentTypeId audioListenerId = ECS::GetComponentTypeId<AudioListener>();

    static const ECS::ComponentTypeId animatorId = ECS::GetComponentTypeId<Animator>();
    static const ECS::ComponentTypeId valueCurveId = ECS::GetComponentTypeId<ValueCurve>();
    static const ECS::ComponentTypeId animatedNodeRefId = ECS::GetComponentTypeId<AnimatedNodeRef>();
    static const ECS::ComponentTypeId animatedSpriteId = ECS::GetComponentTypeId<AnimatedSprite2D>();
    static const ECS::ComponentTypeId humanoidRetargeterId = ECS::GetComponentTypeId<HumanoidRetargeterComponent>();

    static const ECS::ComponentTypeId terrainId = ECS::GetComponentTypeId<Components::Terrain>();
    static const ECS::ComponentTypeId terrainGrassId = ECS::GetComponentTypeId<Components::TerrainGrass>();
    static const ECS::ComponentTypeId terrainModifierVolumeId =
        ECS::GetComponentTypeId<Components::TerrainModifierVolume>();
    static const ECS::ComponentTypeId splineId = ECS::GetComponentTypeId<Components::SplineComponent>();

    static const ECS::ComponentTypeId measureId = ECS::GetComponentTypeId<MeasureComponent>();

    if (typeId == transformId)
        return "Transform";

    if (typeId == meshRendererId || typeId == skinnedMeshRendererId || typeId == lodGroupId ||
        typeId == renderLayerId || typeId == skyEnvId || typeId == skyboxId ||
        typeId == reflectionProbeId || typeId == postProcessId || typeId == windVolumeId || typeId == videoTextureId)
        return "Rendering";

    // Post-process effects don't reach the generic component picker (they add
    // through the Volume inspector), so they need no category here.

    if (typeId == physicsBodyId || typeId == physicsColliderId || typeId == physicsColliderOwnerId ||
        typeId == physicsWorldSettingsId || typeId == boxColliderId || typeId == sphereColliderId ||
        typeId == planeColliderId || typeId == capsuleColliderId)
        return "Physics";

    if (typeId == audioEmitterId || typeId == audioListenerId)
        return "Audio";

    if (typeId == animatorId || typeId == valueCurveId || typeId == animatedNodeRefId ||
        typeId == animatedSpriteId || typeId == humanoidRetargeterId)
        return "Animation";

    // Terrain EFFECTS are absent: they add through the volume inspector's Add
    // Effect picker rather than the Add Component one, so they reach no category.
    if (typeId == terrainId || typeId == terrainGrassId || typeId == terrainModifierVolumeId)
        return "Terrain";

    if (typeId == splineId)
        return "Spline";

    if (typeId == measureId)
        return "Tools";

    return "Component";
}

static bool ShouldHideComponent(ECS::ComponentTypeId typeId)
{
    if (Editor::EditorComponentTraits traits;
        Editor::EditorComponentTraitsRegistry::Get().TryGet(typeId, traits) && traits.HideInInspector)
        return true;
    static const ECS::ComponentTypeId meshGpuId = ECS::GetComponentTypeId<MeshGPUData>();
    static const ECS::ComponentTypeId nameId = ECS::GetComponentTypeId<Name>();
    static const ECS::ComponentTypeId parentId = ECS::GetComponentTypeId<Parent>();
    static const ECS::ComponentTypeId sceneEntityTagId = ECS::GetComponentTypeId<SceneEntityTag>();
    static const ECS::ComponentTypeId localBoundsId = ECS::GetComponentTypeId<LocalBounds>();
    static const ECS::ComponentTypeId heightFieldColliderId = ECS::GetComponentTypeId<HeightFieldColliderShape>();
    static const ECS::ComponentTypeId worldTransformId = ECS::GetComponentTypeId<Components::WorldTransform>();
    static const ECS::ComponentTypeId hierarchyOrderId = ECS::GetComponentTypeId<Components::HierarchyOrder>();
    static const ECS::ComponentTypeId animatorRefId = ECS::GetComponentTypeId<AnimatorRef>();
    static const ECS::ComponentTypeId skeletonRefId = ECS::GetComponentTypeId<SkeletonRef>();
    static const ECS::ComponentTypeId runtimeOnlyId = ECS::GetComponentTypeId<RuntimeOnlyEntity>();

    return ECS::ComponentRegistry::IsEnableStateTag(typeId) || // shown as the entity toggle and the section dots
           typeId == meshGpuId ||
           typeId == nameId ||                // Name is shown and edited in the header; no separate section
           typeId == parentId ||              // Hierarchy relationship; managed by the Hierarchy panel, no inspector UI
           typeId == sceneEntityTagId ||      // Serialization ID; assigned automatically by SceneIO, not user-editable
           typeId == localBoundsId ||         // Derived from mesh geometry; no user-editable state
           typeId == heightFieldColliderId || // Auto-wired from TerrainService; all fields system-managed
           typeId == worldTransformId ||      // System-computed from Transform + hierarchy; read-only, no section
           typeId == hierarchyOrderId ||      // Editor sibling order; maintained by hierarchy reparent/reorder
           typeId == animatorRefId ||         // Internal runtime playback state; exposed via Animator inspector
           typeId == skeletonRefId ||         // Internal skeleton handle; not user-editable
           typeId == runtimeOnlyId;           // Generator-output marker; surfaced as the notice above the sections
}

// The entity's sorted component signature restricted to types that render a section.
// This is what rebuild detection compares: hidden components (ECS::Disabled from the
// entity enable toggle, the lazy Name-add, system-managed tags) change the archetype
// without changing anything the inspector displays.
static std::vector<ECS::ComponentTypeId> CollectVisibleComponentSignature(ECS::World* world,
                                                                          ECS::EntityHandle entity)
{
    std::vector<ECS::ComponentTypeId> ids;
    if (!world || !entity.IsValid() || !world->IsValid(entity))
        return ids;
    if (ECS::Archetype* a = world->GetEntityArchetype(entity))
    {
        ids = a->GetSignature().GetComponents();
        ids.erase(std::remove_if(ids.begin(), ids.end(), ShouldHideComponent), ids.end());
        std::sort(ids.begin(), ids.end());
    }
    return ids;
}

static int SortKeyForComponent(ECS::ComponentTypeId typeId)
{
    // Stable, human-friendly ordering: core -> rendering -> everything else.
    static const ECS::ComponentTypeId transformId = ECS::GetComponentTypeId<Transform>();
    static const ECS::ComponentTypeId nameId = ECS::GetComponentTypeId<Name>();
    static const ECS::ComponentTypeId parentId = ECS::GetComponentTypeId<Parent>();

    static const ECS::ComponentTypeId terrainId = ECS::GetComponentTypeId<Components::Terrain>();
    static const ECS::ComponentTypeId terrainReliefId = ECS::GetComponentTypeId<Components::TerrainPlanetRelief>();
    static const ECS::ComponentTypeId terrainGrassId = ECS::GetComponentTypeId<Components::TerrainGrass>();
    static const ECS::ComponentTypeId physicsBodyId = ECS::GetComponentTypeId<PhysicsBody>();
    static const ECS::ComponentTypeId physicsColliderId = ECS::GetComponentTypeId<PhysicsCollider>();
    static const ECS::ComponentTypeId physicsColliderOwnerId = ECS::GetComponentTypeId<PhysicsColliderOwner>();
    static const ECS::ComponentTypeId boxColliderId = ECS::GetComponentTypeId<BoxColliderShape>();
    static const ECS::ComponentTypeId sphereColliderId = ECS::GetComponentTypeId<SphereColliderShape>();
    static const ECS::ComponentTypeId planeColliderId = ECS::GetComponentTypeId<PlaneColliderShape>();
    static const ECS::ComponentTypeId capsuleColliderId = ECS::GetComponentTypeId<CapsuleColliderShape>();

    static const ECS::ComponentTypeId meshRendererId = ECS::GetComponentTypeId<MeshRenderer>();
    static const ECS::ComponentTypeId meshMaterialInspectorSectionId =
        ECS::GetComponentTypeId<Editor::MeshRendererMaterialInspectorSection>();
    static const ECS::ComponentTypeId skinnedMeshRendererId = ECS::GetComponentTypeId<SkinnedMeshRenderer>();
    static const ECS::ComponentTypeId animatorId = ECS::GetComponentTypeId<Animator>();
    static const ECS::ComponentTypeId valueCurveId = ECS::GetComponentTypeId<ValueCurve>();
    static const ECS::ComponentTypeId animatedNodeRefId = ECS::GetComponentTypeId<AnimatedNodeRef>();
    static const ECS::ComponentTypeId cameraId = ECS::GetComponentTypeId<Camera>();
    static const ECS::ComponentTypeId lightId = ECS::GetComponentTypeId<Light>();
    static const ECS::ComponentTypeId reflectionProbeId = ECS::GetComponentTypeId<Components::ReflectionProbe>();

    if (Editor::EditorComponentTraits traits;
        Editor::EditorComponentTraitsRegistry::Get().TryGet(typeId, traits) && traits.LeadsInspector)
        return -1;
    if (typeId == transformId)
        return 0;
    if (typeId == nameId)
        return 10;
    if (typeId == parentId)
        return 20;

    // Mesh/skinned renderer sit directly under Transform for primitive and
    // model entities. Terrain (mutually exclusive with MeshRenderer on the
    // same entity) shares the same slot, with physics body/collider family
    // immediately below.
    if (typeId == meshRendererId)
        return 30;
    if (typeId == meshMaterialInspectorSectionId)
        return 31;
    if (typeId == skinnedMeshRendererId)
        return 32;
    if (typeId == animatorId)
        return 33;
    if (typeId == valueCurveId)
        return 34;
    if (typeId == animatedNodeRefId)
        return 35;
    if (typeId == terrainId)
        return 37;
    if (typeId == terrainReliefId)
        return 38;
    if (typeId == terrainGrassId)
        return 39;

    if (typeId == physicsBodyId)
        return 40;
    if (typeId == physicsColliderId)
        return 41;
    if (typeId == physicsColliderOwnerId)
        return 42;
    if (typeId == boxColliderId)
        return 43;
    if (typeId == sphereColliderId)
        return 44;
    if (typeId == planeColliderId)
        return 45;
    if (typeId == capsuleColliderId)
        return 46;

    // A modifier volume sits above the effects that stack inside it, so the
    // section order the user drags is already the stack order. Effects take
    // consecutive keys in canonical-list order, derived rather than restated, so
    // one added to ModifierEffectComponents sorts among its siblings with no
    // edit here.
    constexpr int kModifierVolumeSortKey = 50;
    constexpr int kFirstTerrainEffectSortKey = kModifierVolumeSortKey + 1;
    constexpr int kCameraSortKey = 120;
    static_assert(kFirstTerrainEffectSortKey
                          + static_cast<int>(TerrainECS::kTerrainEffectTypeCount)
                      <= kCameraSortKey,
                  "the terrain effect band grew into the camera's section slot: move the keys "
                  "below apart before adding another effect.");

    if (typeId == ECS::GetComponentTypeId<Components::TerrainModifierVolume>())
        return kModifierVolumeSortKey;
    int effectSortKey = kFirstTerrainEffectSortKey;
    for (const TerrainECS::TerrainEffectTypeInfo& info : TerrainECS::TerrainEffectTypes())
    {
        if (info.TypeId == typeId)
            return effectSortKey;
        ++effectSortKey;
    }

    if (typeId == cameraId)
        return kCameraSortKey;
    if (typeId == lightId)
        return 130;
    if (typeId == reflectionProbeId)
        return 140;

    return 1000;
}

// --- Component clipboard (copy/paste an entire component as text) ----------

static std::string ToHex(const std::vector<uint8_t>& bytes)
{
    static const char kHex[] = "0123456789abcdef";
    std::string s;
    s.reserve(bytes.size() * 2);
    for (uint8_t b : bytes)
    {
        s.push_back(kHex[(b >> 4) & 0xF]);
        s.push_back(kHex[b & 0xF]);
    }
    return s;
}

static bool FromHex(const std::string& s, std::vector<uint8_t>& out)
{
    if (s.size() % 2 != 0)
        return false;
    out.clear();
    out.reserve(s.size() / 2);
    auto hexVal = [](char c) -> int
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); i += 2)
    {
        int hi = hexVal(s[i]);
        int lo = hexVal(s[i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

static constexpr const char* kComponentClipPrefix = "GE_COMPONENT|";
static constexpr const char* kComponentsClipPrefix = "GE_COMPONENTS|";

static std::string EncodeComponentClipboard(const std::string& typeName,
                                            const std::vector<uint8_t>& bytes)
{
    return std::string(kComponentClipPrefix) + typeName + "|" + ToHex(bytes);
}

static bool DecodeComponentClipboard(const std::string& clip,
                                     std::string& outTypeName,
                                     std::vector<uint8_t>& outBytes)
{
    const size_t prefixLen = std::strlen(kComponentClipPrefix);
    if (clip.size() <= prefixLen || clip.compare(0, prefixLen, kComponentClipPrefix) != 0)
        return false;
    const size_t typeStart = prefixLen;
    const size_t typeEnd = clip.find('|', typeStart);
    if (typeEnd == std::string::npos)
        return false;
    outTypeName = clip.substr(typeStart, typeEnd - typeStart);
    if (outTypeName.empty())
        return false;
    return FromHex(clip.substr(typeEnd + 1), outBytes);
}

using ComponentClipboardEntry = std::pair<std::string, std::vector<uint8_t>>;

static std::string EncodeComponentsClipboard(const std::vector<ComponentClipboardEntry>& entries)
{
    std::string out(kComponentsClipPrefix);
    for (size_t i = 0; i < entries.size(); ++i)
    {
        out += entries[i].first;
        out.push_back('|');
        out += ToHex(entries[i].second);
        if (i + 1 < entries.size())
            out.push_back('\n');
    }
    return out;
}

static bool DecodeComponentsClipboard(const std::string& clip,
                                      std::vector<ComponentClipboardEntry>& outEntries)
{
    const size_t prefixLen = std::strlen(kComponentsClipPrefix);
    if (clip.size() <= prefixLen || clip.compare(0, prefixLen, kComponentsClipPrefix) != 0)
        return false;

    outEntries.clear();
    size_t cursor = prefixLen;
    while (cursor < clip.size())
    {
        const size_t lineEnd = clip.find('\n', cursor);
        const std::string_view line = (lineEnd == std::string::npos)
                                          ? std::string_view(clip).substr(cursor)
                                          : std::string_view(clip).substr(cursor, lineEnd - cursor);
        if (!line.empty())
        {
            const size_t sep = line.find('|');
            if (sep == std::string_view::npos || sep == 0 || sep + 1 >= line.size())
                return false;

            ComponentClipboardEntry entry{};
            entry.first = std::string(line.substr(0, sep));
            if (!FromHex(std::string(line.substr(sep + 1)), entry.second))
                return false;
            outEntries.push_back(std::move(entry));
        }

        if (lineEnd == std::string::npos)
            break;
        cursor = lineEnd + 1;
    }
    return !outEntries.empty();
}

static bool TryFindComponentTypeIdByName(const std::string& name, ECS::ComponentTypeId& out)
{
    const auto& displayNames = GetComponentDisplayNames();
    for (const auto& kv : displayNames)
    {
        if (kv.second == name)
        {
            out = kv.first;
            return true;
        }
    }
    // Plugin-registered display names (clipboard paste / preset resolution
    // must round-trip package components, e.g. "Tree Generator").
    for (const auto& [typeId, traits] : Editor::EditorComponentTraitsRegistry::Get().Snapshot())
    {
        if (traits.DisplayName == name)
        {
            out = typeId;
            return true;
        }
    }
    // Post-process effect display names live in the effect registry.
    ECS::ComponentTypeId effectId{};
    Rendering::PostProcessEffectRegistry::ForEach(
        [&](const Rendering::PostProcessEffectDescriptor& d)
        {
            if (effectId == 0 && d.DisplayName == name)
                effectId = d.Type;
        });
    if (effectId != 0)
    {
        out = effectId;
        return true;
    }
    return false;
}

static std::string ComponentTitle(ECS::ComponentTypeId typeId)
{
    // 0. Plugin-registered traits (editor package modules) — checked live, not
    //    folded into the static map: modules register at project open, after
    //    the map's one-time build.
    if (Editor::EditorComponentTraits traits;
        Editor::EditorComponentTraitsRegistry::Get().TryGet(typeId, traits) && !traits.DisplayName.empty())
        return traits.DisplayName;
    // 0b. Post-process effects carry their display name in the effect registry.
    if (const auto* effect = Rendering::PostProcessEffectRegistry::Find(typeId);
        effect && !effect->DisplayName.empty())
        return std::string(effect->DisplayName);
    // 0c. Terrain effects carry theirs in the canonical effect list, so the
    //     section header and the Add Effect picker cannot drift apart.
    if (const auto* terrainEffect = TerrainECS::FindTerrainEffectType(typeId))
        return std::string(terrainEffect->Title);
    // 1. Explicit override for names that don't derive cleanly (acronyms like
    //    "CRT"/"LUT", or renamed labels like LocalBounds -> "World Bounds").
    const auto& displayNames = GetComponentDisplayNames();
    auto it = displayNames.find(typeId);
    if (it != displayNames.end())
        return it->second;
    // 2. Reflected components carry a normalized, cross-compiler-stable name;
    //    derive the title from it so no per-component map entry is needed.
    if (const std::string_view canonical = ECS::ComponentFieldRegistry::GetCanonicalName(typeId); !canonical.empty())
        return PrettifyTypeName(std::string(canonical).c_str());
    // 3. Fallback for a component with no reflected field table: the handler's
    //    canonical type name, which is the same normalized spelling on every
    //    compiler.
    if (ECS::IComponentHandler* handler = ECS::ComponentRegistry::GetHandler(typeId))
        return PrettifyTypeName(handler->GetTypeName());
    return "Component " + std::to_string(static_cast<std::size_t>(typeId));
}

// Inspector header context menu — Transform copy/paste wording reflects local-space TRS.
static std::string BuildInspectorCopyMenuLabel(const std::vector<ECS::ComponentTypeId>& selection)
{
    static const ECS::ComponentTypeId transformId = ECS::GetComponentTypeId<Transform>();
    if (selection.empty())
        return "Copy Component";
    if (selection.size() == 1 && selection[0] == transformId)
        return "Copy Transform (local position, rotation & scale)";
    if (selection.size() == 1)
        return "Copy " + ComponentTitle(selection[0]);
    return "Copy " + std::to_string(selection.size()) + " Components";
}

static std::string BuildInspectorPasteMenuLabel(bool multiClipValid,
                                                size_t multiClipEntryCount,
                                                bool clipValid,
                                                const std::string& clipTypeName,
                                                bool entityHasClipType,
                                                ECS::ComponentTypeId clipTypeId)
{
    static const ECS::ComponentTypeId transformId = ECS::GetComponentTypeId<Transform>();

    if (multiClipValid && multiClipEntryCount > 0)
        return "Paste " + std::to_string(multiClipEntryCount) + " component values";
    if (!clipValid)
        return "Paste Component";

    const bool clipboardIsTransform = (clipTypeId == transformId);

    if (entityHasClipType && clipboardIsTransform)
        return "Paste Transform component values (local position, rotation & scale)";
    if (entityHasClipType)
        return "Paste " + clipTypeName + " component values";
    if (clipboardIsTransform)
        return "Paste Transform as New Component";
    return "Paste " + clipTypeName + " as New Component";
}

static const char* ComponentTooltip(ECS::ComponentTypeId typeId)
{
    static const ECS::ComponentTypeId transformId = ECS::GetComponentTypeId<Transform>();
    static const ECS::ComponentTypeId meshRendererId = ECS::GetComponentTypeId<MeshRenderer>();
    static const ECS::ComponentTypeId meshMaterialInspectorSectionId =
        ECS::GetComponentTypeId<Editor::MeshRendererMaterialInspectorSection>();
    static const ECS::ComponentTypeId skinnedMeshId = ECS::GetComponentTypeId<SkinnedMeshRenderer>();
    static const ECS::ComponentTypeId cameraId = ECS::GetComponentTypeId<Camera>();
    static const ECS::ComponentTypeId lightId = ECS::GetComponentTypeId<Light>();
    static const ECS::ComponentTypeId physicsBodyId = ECS::GetComponentTypeId<PhysicsBody>();
    static const ECS::ComponentTypeId physicsColliderId = ECS::GetComponentTypeId<PhysicsCollider>();
    static const ECS::ComponentTypeId boxColliderId = ECS::GetComponentTypeId<BoxColliderShape>();
    static const ECS::ComponentTypeId sphereColliderId = ECS::GetComponentTypeId<SphereColliderShape>();
    static const ECS::ComponentTypeId planeColliderId = ECS::GetComponentTypeId<PlaneColliderShape>();
    static const ECS::ComponentTypeId capsuleColliderId = ECS::GetComponentTypeId<CapsuleColliderShape>();
    static const ECS::ComponentTypeId audioEmitterId = ECS::GetComponentTypeId<AudioEmitter>();
    static const ECS::ComponentTypeId audioListenerId = ECS::GetComponentTypeId<AudioListener>();
    static const ECS::ComponentTypeId animatorId = ECS::GetComponentTypeId<Animator>();
    static const ECS::ComponentTypeId valueCurveId = ECS::GetComponentTypeId<ValueCurve>();
    static const ECS::ComponentTypeId skyEnvId = ECS::GetComponentTypeId<SkyEnvironment>();
    static const ECS::ComponentTypeId skyboxId = ECS::GetComponentTypeId<Skybox>();
    static const ECS::ComponentTypeId postProcessId = ECS::GetComponentTypeId<PostProcessVolume>();

    if (typeId == transformId)
        return "Position, rotation, and scale in local space";
    if (typeId == meshRendererId)
        return "Renders a static mesh with a material";
    if (typeId == meshMaterialInspectorSectionId)
        return "Material slots and shader properties for this mesh renderer";
    if (typeId == skinnedMeshId)
        return "Renders a mesh deformed by a skeleton for animation";
    if (typeId == cameraId)
        return "Defines a view frustum used to render the scene";
    if (typeId == lightId)
        return "Emits light into the scene (directional, point, spot, or ambient)";
    if (typeId == physicsBodyId)
        return "Simulated rigid body with mass, damping, and collision response";
    if (typeId == physicsColliderId)
        return "Collision shape attached to a physics body";
    if (typeId == boxColliderId)
        return "Axis-aligned box collision shape";
    if (typeId == sphereColliderId)
        return "Sphere collision shape";
    if (typeId == planeColliderId)
        return "Infinite plane collision shape";
    if (typeId == capsuleColliderId)
        return "Capsule (cylinder + hemispherical caps) collision shape";
    if (typeId == audioEmitterId)
        return "Plays audio clips from this entity's position";
    if (typeId == audioListenerId)
        return "Marks this entity as the audio listener (usually the camera)";
    if (typeId == animatorId)
        return "Controls playback of skeletal animations on a mesh";
    if (typeId == valueCurveId)
        return "Tween curve with easing, duration, and looping behavior";
    if (typeId == skyEnvId)
        return "Procedural sky with atmospheric scattering and stars";
    if (typeId == skyboxId)
        return "HDRI skybox rendered as the scene background";
    if (typeId == postProcessId)
        return "Post-processing volume for spatial blending and core overrides";
    // Post-process effect tooltips come from the effect registry (Description).
    return nullptr;
}

static std::string_view RequiredPackageForComponent(ECS::ComponentTypeId typeId)
{
    if (const auto* effect = Rendering::PostProcessEffectRegistry::Find(typeId))
        return effect->RequiredPackage;
    return {};
}

// Maps a component type to the CSS class that controls its inspector header icon.
// Returns an empty string for components without a dedicated icon; the icon slot
// is hidden in that case.
static std::string ComponentIconClass(ECS::World* world, ECS::EntityHandle entity, ECS::ComponentTypeId typeId)
{
    // Plugin-registered traits (editor package modules) — live lookup, before
    // the built-in chains.
    if (Editor::EditorComponentTraits traits;
        Editor::EditorComponentTraitsRegistry::Get().TryGet(typeId, traits) && !traits.InspectorIconClass.empty())
        return traits.InspectorIconClass;

    static const ECS::ComponentTypeId transformId = ECS::GetComponentTypeId<Transform>();
    static const ECS::ComponentTypeId meshRendererId = ECS::GetComponentTypeId<MeshRenderer>();
    static const ECS::ComponentTypeId skinnedMeshRendererId = ECS::GetComponentTypeId<SkinnedMeshRenderer>();
    static const ECS::ComponentTypeId materialSectionId = ECS::GetComponentTypeId<Editor::MeshRendererMaterialInspectorSection>();
    static const ECS::ComponentTypeId cameraId = ECS::GetComponentTypeId<Camera>();
    static const ECS::ComponentTypeId lightId = ECS::GetComponentTypeId<Light>();
    static const ECS::ComponentTypeId measureId = ECS::GetComponentTypeId<MeasureComponent>();
    static const ECS::ComponentTypeId skyEnvId = ECS::GetComponentTypeId<SkyEnvironment>();
    static const ECS::ComponentTypeId skyboxId = ECS::GetComponentTypeId<Skybox>();
    static const ECS::ComponentTypeId reflectionProbeId = ECS::GetComponentTypeId<Components::ReflectionProbe>();
    static const ECS::ComponentTypeId postProcessId = ECS::GetComponentTypeId<PostProcessVolume>();
    static const ECS::ComponentTypeId windVolumeId = ECS::GetComponentTypeId<Components::WindVolume>();
    static const ECS::ComponentTypeId lensFlareSourceId = ECS::GetComponentTypeId<Components::LensFlareSource>();
    static const ECS::ComponentTypeId terrainId = ECS::GetComponentTypeId<Components::Terrain>();
    static const ECS::ComponentTypeId terrainGrassId = ECS::GetComponentTypeId<Components::TerrainGrass>();
    static const ECS::ComponentTypeId terrainModifierVolumeId =
        ECS::GetComponentTypeId<Components::TerrainModifierVolume>();
    static const ECS::ComponentTypeId videoTextureId = ECS::GetComponentTypeId<Components::VideoTextureComponent>();
    static const ECS::ComponentTypeId oceanSurfaceId = ECS::GetComponentTypeId<Components::OceanSurface>();
    static const ECS::ComponentTypeId oceanSpectrumId = ECS::GetComponentTypeId<Components::OceanWaveSpectrum>();
    static const ECS::ComponentTypeId oceanBuoyancyId = ECS::GetComponentTypeId<Components::OceanBuoyancy>();
    static const ECS::ComponentTypeId splineId = ECS::GetComponentTypeId<Components::SplineComponent>();
    static const ECS::ComponentTypeId physicsBodyId = ECS::GetComponentTypeId<PhysicsBody>();
    static const ECS::ComponentTypeId physicsColliderId = ECS::GetComponentTypeId<PhysicsCollider>();
    static const ECS::ComponentTypeId physicsColliderOwnerId = ECS::GetComponentTypeId<PhysicsColliderOwner>();
    static const ECS::ComponentTypeId physicsWorldSettingsId = ECS::GetComponentTypeId<PhysicsWorldSettingsComponent>();
    static const ECS::ComponentTypeId boxColliderId = ECS::GetComponentTypeId<BoxColliderShape>();
    static const ECS::ComponentTypeId sphereColliderId = ECS::GetComponentTypeId<SphereColliderShape>();
    static const ECS::ComponentTypeId planeColliderId = ECS::GetComponentTypeId<PlaneColliderShape>();
    static const ECS::ComponentTypeId capsuleColliderId = ECS::GetComponentTypeId<CapsuleColliderShape>();
    static const ECS::ComponentTypeId audioEmitterId = ECS::GetComponentTypeId<AudioEmitter>();
    static const ECS::ComponentTypeId audioListenerId = ECS::GetComponentTypeId<AudioListener>();
    static const ECS::ComponentTypeId animatorId = ECS::GetComponentTypeId<Animator>();
    static const ECS::ComponentTypeId valueCurveId = ECS::GetComponentTypeId<ValueCurve>();
    static const ECS::ComponentTypeId animatedNodeRefId = ECS::GetComponentTypeId<AnimatedNodeRef>();
    static const ECS::ComponentTypeId lodGroupId = ECS::GetComponentTypeId<LODGroup>();
    static const ECS::ComponentTypeId renderLayerId = ECS::GetComponentTypeId<RenderLayer>();

    if (typeId == transformId)
        return "inspector-section-icon-transform";
    if (typeId == meshRendererId)
        return "inspector-section-icon-mesh-renderer";
    if (typeId == skinnedMeshRendererId)
        return "inspector-section-icon-skinned-mesh";
    if (typeId == materialSectionId)
        return "inspector-section-icon-material";
    if (typeId == cameraId)
        return "inspector-section-icon-camera";
    if (typeId == lightId)
    {
        if (world && world->IsValid(entity))
        {
            if (const auto* light = world->GetComponent<Components::Light>(entity))
            {
                using LT = Components::LightType;
                switch (light->Type)
                {
                case LT::Ambient:
                    return "inspector-section-icon-light-ambient";
                case LT::Directional:
                    return "inspector-section-icon-light-directional";
                case LT::Spot:
                    return "inspector-section-icon-light-spot";
                case LT::Point:
                    return "inspector-section-icon-light-point";
                case LT::Area:
                    return "inspector-section-icon-light-area";
                case LT::Volume:
                    return "inspector-section-icon-light-volume";
                default:
                    break;
                }
            }
        }
        return "inspector-section-icon-light-directional";
    }
    if (typeId == skyEnvId)
        return "inspector-section-icon-sky";
    if (typeId == measureId)
        return "inspector-section-icon-measure";
    if (typeId == skyboxId)
        return "inspector-section-icon-sky";
    if (typeId == reflectionProbeId)
        return "inspector-section-icon-reflection-probe";
    if (typeId == postProcessId)
        return "inspector-section-icon-post-process-volume";
    if (typeId == windVolumeId)
        return "inspector-section-icon-wind-volume";
    if (typeId == lensFlareSourceId)
        return "inspector-section-icon-lens-flare";
    if (typeId == terrainId || typeId == terrainModifierVolumeId)
        return "inspector-section-icon-terrain";
    if (typeId == terrainGrassId)
        return "inspector-section-icon-grass";
    if (typeId == videoTextureId)
        return "inspector-section-icon-video-texture";
    if (typeId == oceanSurfaceId || typeId == oceanSpectrumId || typeId == oceanBuoyancyId)
        return "inspector-section-icon-ocean";
    if (typeId == splineId)
        return "inspector-section-icon-spline";
    if (typeId == physicsBodyId ||
        typeId == physicsColliderId ||
        typeId == physicsColliderOwnerId)
        return "inspector-section-icon-physics";
    if (typeId == physicsWorldSettingsId)
        return "inspector-section-icon-physics-settings";
    if (typeId == boxColliderId)
        return "inspector-section-icon-shape-cube";
    if (typeId == sphereColliderId)
        return "inspector-section-icon-shape-sphere";
    if (typeId == planeColliderId)
        return "inspector-section-icon-shape-plane";
    if (typeId == capsuleColliderId)
        return "inspector-section-icon-shape-capsule";
    if (typeId == audioEmitterId)
        return "inspector-section-icon-audio-emitter";
    if (typeId == audioListenerId)
        return "inspector-section-icon-audio-listener";
    if (typeId == animatorId)
        return "inspector-section-icon-animator";
    if (typeId == valueCurveId)
        return "inspector-section-icon-animator";
    if (typeId == animatedNodeRefId)
        return "inspector-section-icon-animated-node";
    if (typeId == lodGroupId)
        return "inspector-section-icon-lod-group";
    if (typeId == renderLayerId)
        return "inspector-section-icon-render-layer";
    return "inspector-section-icon-default";
}

static bool IsRemovableComponent(ECS::ComponentTypeId typeId)
{
    // Preserve core authoring invariants: most entities assume Transform + Name.
    static const ECS::ComponentTypeId transformId = ECS::GetComponentTypeId<Transform>();
    static const ECS::ComponentTypeId nameId = ECS::GetComponentTypeId<Name>();
    static const ECS::ComponentTypeId meshMaterialInspectorSectionId =
        ECS::GetComponentTypeId<Editor::MeshRendererMaterialInspectorSection>();
    if (typeId == transformId || typeId == nameId || typeId == meshMaterialInspectorSectionId)
    {
        return false;
    }
    return true;
}

static bool ShouldCollapseByDefault(ECS::ComponentTypeId typeId)
{
    static const ECS::ComponentTypeId skeletonRefId = ECS::GetComponentTypeId<SkeletonRef>();
    static const ECS::ComponentTypeId animatorRefId = ECS::GetComponentTypeId<AnimatorRef>();
    static const ECS::ComponentTypeId animatedNodeRefId = ECS::GetComponentTypeId<AnimatedNodeRef>();
    return typeId == skeletonRefId || typeId == animatorRefId || typeId == animatedNodeRefId;
}

static void NotifyComponentChanged(Editor::EditorChangeNotifications* notifications,
                                   ECS::World* world,
                                   ECS::EntityHandle entity,
                                   ECS::ComponentTypeId typeId)
{
    if (!notifications)
        return;
    Editor::EditorChangeNotifications::ComponentChangedEvent e{};
    e.world = world;
    e.entity = entity;
    e.componentType = typeId;
    e.kind = Editor::EditorChangeNotifications::ChangeKind::Commit;
    notifications->NotifyComponentChanged(e);
}

class PasteComponentCommand final : public Editor::IEditorCommand
{
  public:
    PasteComponentCommand(std::string name,
                          ECS::World* world,
                          Editor::EditorChangeNotifications* notifications,
                          ECS::EntityHandle entity,
                          ECS::ComponentTypeId typeId,
                          std::vector<uint8_t> newBytes)
        : m_Name(std::move(name)), m_World(world), m_Notifications(notifications), m_Entity(entity), m_TypeId(typeId), m_NewBytes(std::move(newBytes))
    {
        if (m_World && m_Entity.IsValid() && m_World->IsValid(m_Entity))
        {
            m_HadComponent = m_World->CaptureComponentBytes(m_Entity, m_TypeId, m_PrevBytes);
        }
    }

    const char* GetName() const override { return m_Name.c_str(); }
    void Do() override { Redo(); }

    void Undo() override
    {
        if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
            return;

        if (m_HadComponent)
        {
            if (m_World->ApplyComponentBytesImmediate(m_Entity, m_TypeId, m_PrevBytes))
                NotifyComponentChanged(m_Notifications, m_World, m_Entity, m_TypeId);
        }
        else
        {
            if (m_World->RemoveComponentByTypeIdImmediate(m_Entity, m_TypeId))
                NotifyComponentChanged(m_Notifications, m_World, m_Entity, m_TypeId);
        }
    }

    void Redo() override
    {
        if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
            return;
        if (m_World->ApplyComponentBytesImmediate(m_Entity, m_TypeId, m_NewBytes))
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, m_TypeId);
    }

  private:
    std::string m_Name;
    ECS::World* m_World = nullptr;
    Editor::EditorChangeNotifications* m_Notifications = nullptr;
    ECS::EntityHandle m_Entity{};
    ECS::ComponentTypeId m_TypeId{};
    std::vector<uint8_t> m_NewBytes;
    std::vector<uint8_t> m_PrevBytes;
    bool m_HadComponent = false;
};

class PasteComponentsCommand final : public Editor::IEditorCommand
{
  public:
    struct Item
    {
        ECS::ComponentTypeId TypeId{};
        std::vector<uint8_t> NewBytes;
        std::vector<uint8_t> PrevBytes;
        bool HadComponent = false;
    };

    PasteComponentsCommand(std::string name,
                           ECS::World* world,
                           Editor::EditorChangeNotifications* notifications,
                           ECS::EntityHandle entity,
                           std::vector<Item> items)
        : m_Name(std::move(name)), m_World(world), m_Notifications(notifications), m_Entity(entity), m_Items(std::move(items))
    {
        if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
            return;
        for (auto& item : m_Items)
            item.HadComponent = m_World->CaptureComponentBytes(m_Entity, item.TypeId, item.PrevBytes);
    }

    const char* GetName() const override { return m_Name.c_str(); }
    void Do() override { Redo(); }

    void Undo() override
    {
        if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
            return;

        for (const auto& item : m_Items)
        {
            if (item.HadComponent)
            {
                if (m_World->ApplyComponentBytesImmediate(m_Entity, item.TypeId, item.PrevBytes))
                    NotifyComponentChanged(m_Notifications, m_World, m_Entity, item.TypeId);
            }
            else
            {
                if (m_World->RemoveComponentByTypeIdImmediate(m_Entity, item.TypeId))
                    NotifyComponentChanged(m_Notifications, m_World, m_Entity, item.TypeId);
            }
        }
    }

    void Redo() override
    {
        if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
            return;
        for (const auto& item : m_Items)
        {
            if (m_World->ApplyComponentBytesImmediate(m_Entity, item.TypeId, item.NewBytes))
                NotifyComponentChanged(m_Notifications, m_World, m_Entity, item.TypeId);
        }
    }

  private:
    std::string m_Name;
    ECS::World* m_World = nullptr;
    Editor::EditorChangeNotifications* m_Notifications = nullptr;
    ECS::EntityHandle m_Entity{};
    std::vector<Item> m_Items;
};

class AddPresetComponentsCommand final : public Editor::IEditorCommand
{
  public:
    AddPresetComponentsCommand(std::string name,
                               ECS::World* world,
                               Editor::EditorChangeNotifications* notifications,
                               ECS::EntityHandle entity,
                               ComponentPreset preset)
        : m_Name(std::move(name)), m_World(world), m_Notifications(notifications), m_Entity(entity), m_Preset(preset)
    {
    }

    const char* GetName() const override { return m_Name.c_str(); }
    void Do() override { Redo(); }

    void Undo() override
    {
        if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
            return;

        // Remove only what we added in Redo().
        if (m_AddedCamera)
        {
            m_World->RemoveComponentImmediate<Components::Camera>(m_Entity);
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, ECS::GetComponentTypeId<Components::Camera>());
        }
        if (m_AddedLight)
        {
            m_World->RemoveComponentImmediate<Components::Light>(m_Entity);
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, ECS::GetComponentTypeId<Components::Light>());
        }
        if (m_AddedSkyEnvironment)
        {
            m_World->RemoveComponentImmediate<Components::SkyEnvironment>(m_Entity);
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, ECS::GetComponentTypeId<Components::SkyEnvironment>());
        }
        if (m_AddedSkybox)
        {
            m_World->RemoveComponentImmediate<Components::Skybox>(m_Entity);
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, ECS::GetComponentTypeId<Components::Skybox>());
        }
        if (m_AddedPhysicsBody)
        {
            m_World->RemoveComponentImmediate<Components::PhysicsBody>(m_Entity);
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, ECS::GetComponentTypeId<Components::PhysicsBody>());
        }
        if (m_AddedCharacterController)
        {
            m_World->RemoveComponentImmediate<Components::CharacterController>(m_Entity);
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, ECS::GetComponentTypeId<Components::CharacterController>());
        }
        if (m_AddedPhysicsCollider)
        {
            m_World->RemoveComponentImmediate<Components::PhysicsCollider>(m_Entity);
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, ECS::GetComponentTypeId<Components::PhysicsCollider>());
        }
        if (m_AddedBoxShape)
        {
            m_World->RemoveComponentImmediate<Components::BoxColliderShape>(m_Entity);
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, ECS::GetComponentTypeId<Components::BoxColliderShape>());
        }
        if (m_AddedSphereShape)
        {
            m_World->RemoveComponentImmediate<Components::SphereColliderShape>(m_Entity);
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, ECS::GetComponentTypeId<Components::SphereColliderShape>());
        }
        if (m_AddedPlaneShape)
        {
            m_World->RemoveComponentImmediate<Components::PlaneColliderShape>(m_Entity);
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, ECS::GetComponentTypeId<Components::PlaneColliderShape>());
        }
        if (m_AddedAudioEmitter)
        {
            m_World->RemoveComponentImmediate<Components::AudioEmitter>(m_Entity);
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, ECS::GetComponentTypeId<Components::AudioEmitter>());
        }
        if (m_AddedAudioListener)
        {
            m_World->RemoveComponentImmediate<Components::AudioListener>(m_Entity);
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, ECS::GetComponentTypeId<Components::AudioListener>());
        }
        if (m_AddedPostProcessVolume)
        {
            m_World->RemoveComponentImmediate<Components::PostProcessVolume>(m_Entity);
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, ECS::GetComponentTypeId<Components::PostProcessVolume>());
        }
        if (m_AddedWindVolume)
        {
            m_World->RemoveComponentImmediate<Components::WindVolume>(m_Entity);
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, ECS::GetComponentTypeId<Components::WindVolume>());
        }
        if (m_AddedParticleEmitter)
        {
            m_World->RemoveComponentImmediate<Components::ParticleEmitter3D>(m_Entity);
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, ECS::GetComponentTypeId<Components::ParticleEmitter3D>());
        }
        if (m_AddedParticleCollisionEvents)
        {
            m_World->RemoveComponentImmediate<Components::ParticleCollisionEventsBuffer>(m_Entity);
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, ECS::GetComponentTypeId<Components::ParticleCollisionEventsBuffer>());
        }
        if (m_AddedTerrainEffectTypeId != 0
            && m_World->RemoveComponentByTypeIdImmediate(m_Entity, m_AddedTerrainEffectTypeId))
        {
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, m_AddedTerrainEffectTypeId);
        }
        if (m_AddedTerrainModifierVolume)
        {
            m_World->RemoveComponentImmediate<Components::TerrainModifierVolume>(m_Entity);
            NotifyComponentChanged(m_Notifications, m_World, m_Entity,
                                   ECS::GetComponentTypeId<Components::TerrainModifierVolume>());
        }
        if (m_AddedSplineComponent)
        {
            m_World->RemoveComponentImmediate<Components::SplineComponent>(m_Entity);
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, ECS::GetComponentTypeId<Components::SplineComponent>());
        }
        if (m_AddedValueCurve)
        {
            m_World->RemoveComponentImmediate<Components::ValueCurve>(m_Entity);
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, ECS::GetComponentTypeId<Components::ValueCurve>());
        }
        if (m_AddedVideoTexture)
        {
            m_World->RemoveComponentImmediate<Components::VideoTextureComponent>(m_Entity);
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, ECS::GetComponentTypeId<Components::VideoTextureComponent>());
        }
    }

    void Redo() override
    {
        if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
            return;

        // Reset "added" flags on redo so repeated redo/undo behaves correctly.
        m_AddedCamera = false;
        m_AddedLight = false;
        m_AddedSkyEnvironment = false;
        m_AddedSkybox = false;
        m_AddedPhysicsBody = false;
        m_AddedCharacterController = false;
        m_AddedPhysicsCollider = false;
        m_AddedBoxShape = false;
        m_AddedSphereShape = false;
        m_AddedPlaneShape = false;
        m_AddedAudioEmitter = false;
        m_AddedAudioListener = false;
        m_AddedPostProcessVolume = false;
        m_AddedWindVolume = false;
        m_AddedParticleEmitter = false;
        m_AddedParticleCollisionEvents = false;
        m_AddedTerrainModifierVolume = false;
        m_AddedTerrainEffectTypeId = 0;
        m_AddedSplineComponent = false;
        m_AddedValueCurve = false;
        m_AddedVideoTexture = false;

        auto addIfMissing = [&](auto tag, auto value)
        {
            using T = typename decltype(tag)::type;
            if (!m_World->HasComponent<T>(m_Entity))
            {
                m_World->AddComponentImmediate<T>(m_Entity, value);
                NotifyComponentChanged(m_Notifications, m_World, m_Entity, ECS::GetComponentTypeId<T>());
                return true;
            }
            return false;
        };

        // Terrain area presets are answered by the table that pairs a preset
        // with its effect, never by a case label: a sixth canonical effect
        // reaches this command from its table row alone.
        if (const Editor::TerrainAreaPreset* area = Editor::FindTerrainAreaPreset(m_Preset))
        {
            const Editor::TerrainAreaPresetAdditions added =
                Editor::ApplyTerrainAreaPreset(*m_World, m_Entity, *area);
            m_AddedTerrainModifierVolume = added.AddedVolume;
            m_AddedTerrainEffectTypeId = added.AddedEffectTypeId;
            m_AddedSplineComponent = added.AddedSpline;
            if (added.AddedSpline)
            {
                NotifyComponentChanged(m_Notifications, m_World, m_Entity,
                                       ECS::GetComponentTypeId<Components::SplineComponent>());
            }
            if (added.AddedVolume)
            {
                NotifyComponentChanged(m_Notifications, m_World, m_Entity,
                                       ECS::GetComponentTypeId<Components::TerrainModifierVolume>());
            }
            if (added.AddedEffectTypeId != 0)
                NotifyComponentChanged(m_Notifications, m_World, m_Entity, added.AddedEffectTypeId);
            return;
        }

        switch (m_Preset)
        {
        case ComponentPreset::Camera:
            m_AddedCamera = addIfMissing(ECS::TypeTag<Components::Camera>{}, Components::Camera{});
            break;
        case ComponentPreset::Light:
        {
            Components::Light l{};
            l.Type = Components::LightType::Directional;
            l.Intensity = Components::kClearNoonSunIlluminanceLux;
            l.IntensityUnit = Components::LightUnit::Lux;   // physical units by default
            m_AddedLight = addIfMissing(ECS::TypeTag<Components::Light>{}, l);
            break;
        }
        case ComponentPreset::SkyEnvironment:
            m_AddedSkyEnvironment = addIfMissing(ECS::TypeTag<Components::SkyEnvironment>{}, Components::SkyEnvironment{});
            break;
        case ComponentPreset::Skybox:
            m_AddedSkybox = addIfMissing(ECS::TypeTag<Components::Skybox>{}, Components::Skybox{});
            break;
        case ComponentPreset::PhysicsDynamicBox:
        {
            Components::PhysicsBody body{};
            body.motionType = Physics::MotionType::Dynamic;
            m_AddedPhysicsBody = addIfMissing(ECS::TypeTag<Components::PhysicsBody>{}, body);
            m_AddedPhysicsCollider = addIfMissing(ECS::TypeTag<Components::PhysicsCollider>{}, Components::PhysicsCollider{});
            m_AddedBoxShape = addIfMissing(ECS::TypeTag<Components::BoxColliderShape>{}, Components::BoxColliderShape{});
            break;
        }
        case ComponentPreset::PhysicsDynamicSphere:
        {
            Components::PhysicsBody body{};
            body.motionType = Physics::MotionType::Dynamic;
            m_AddedPhysicsBody = addIfMissing(ECS::TypeTag<Components::PhysicsBody>{}, body);
            m_AddedPhysicsCollider = addIfMissing(ECS::TypeTag<Components::PhysicsCollider>{}, Components::PhysicsCollider{});
            m_AddedSphereShape = addIfMissing(ECS::TypeTag<Components::SphereColliderShape>{}, Components::SphereColliderShape{});
            break;
        }
        case ComponentPreset::PhysicsStaticPlane:
        {
            Components::PhysicsBody body{};
            body.motionType = Physics::MotionType::Static;
            m_AddedPhysicsBody = addIfMissing(ECS::TypeTag<Components::PhysicsBody>{}, body);
            m_AddedPhysicsCollider = addIfMissing(ECS::TypeTag<Components::PhysicsCollider>{}, Components::PhysicsCollider{});
            Components::PlaneColliderShape plane{};
            plane.normalX = 0.0f;
            plane.normalY = 1.0f;
            plane.normalZ = 0.0f;
            plane.d = 0.0f;
            plane.halfExtent = 50.0f;
            m_AddedPlaneShape = addIfMissing(ECS::TypeTag<Components::PlaneColliderShape>{}, plane);
            break;
        }
        case ComponentPreset::PhysicsStaticBox:
        {
            Components::PhysicsBody body{};
            body.motionType = Physics::MotionType::Static;
            m_AddedPhysicsBody = addIfMissing(ECS::TypeTag<Components::PhysicsBody>{}, body);
            m_AddedPhysicsCollider = addIfMissing(ECS::TypeTag<Components::PhysicsCollider>{}, Components::PhysicsCollider{});
            m_AddedBoxShape = addIfMissing(ECS::TypeTag<Components::BoxColliderShape>{}, Components::BoxColliderShape{});
            break;
        }
        case ComponentPreset::PhysicsCharacterController:
            m_AddedCharacterController =
                addIfMissing(ECS::TypeTag<Components::CharacterController>{}, Components::CharacterController{});
            break;
        case ComponentPreset::AudioEmitter:
            m_AddedAudioEmitter = addIfMissing(ECS::TypeTag<Components::AudioEmitter>{}, Components::AudioEmitter{});
            break;
        case ComponentPreset::AudioListener:
            m_AddedAudioListener = addIfMissing(ECS::TypeTag<Components::AudioListener>{}, Components::AudioListener{});
            break;
        case ComponentPreset::PostProcessVolume:
            m_AddedPostProcessVolume = addIfMissing(ECS::TypeTag<Components::PostProcessVolume>{}, Components::PostProcessVolume{});
            break;
        case ComponentPreset::WindVolume:
            m_AddedWindVolume = addIfMissing(ECS::TypeTag<Components::WindVolume>{}, Components::WindVolume{});
            break;
        case ComponentPreset::ParticleEmitter2D:
            m_AddedParticleEmitter = addIfMissing(ECS::TypeTag<Components::ParticleEmitter3D>{}, Components::MakeParticleEmitter2DDefaults());
            break;
        case ComponentPreset::ParticleEmitter3D:
            m_AddedParticleEmitter = addIfMissing(ECS::TypeTag<Components::ParticleEmitter3D>{}, Components::ParticleEmitter3D{});
            break;
        case ComponentPreset::ParticleCollisionEvents:
            m_AddedParticleCollisionEvents = addIfMissing(ECS::TypeTag<Components::ParticleCollisionEventsBuffer>{}, Components::ParticleCollisionEventsBuffer{});
            break;
        case ComponentPreset::TerrainSplineFlatten:
        {
            // The spline supplies the region, so the volume takes its shape from
            // it. Adding the SplineComponent here is the point: the pre-volume
            // spline preset added a modifier that silently did nothing without one.
            m_AddedSplineComponent = addIfMissing(
                ECS::TypeTag<Components::SplineComponent>{}, Components::SplineComponent{});
            Components::TerrainModifierVolume volume{};
            volume.Shape = Components::TerrainVolumeShape::SplineArea;
            m_AddedTerrainModifierVolume =
                addIfMissing(ECS::TypeTag<Components::TerrainModifierVolume>{}, volume);
            AddTerrainEffect(ECS::GetComponentTypeId<Components::TerrainFlattenEffect>());
            break;
        }
        case ComponentPreset::SplineComponent:
            m_AddedSplineComponent = addIfMissing(ECS::TypeTag<Components::SplineComponent>{}, Components::SplineComponent{});
            break;
        case ComponentPreset::ValueCurve:
            m_AddedValueCurve = addIfMissing(ECS::TypeTag<Components::ValueCurve>{}, Components::ValueCurve{});
            break;
        case ComponentPreset::VideoTexture:
            m_AddedVideoTexture = addIfMissing(ECS::TypeTag<Components::VideoTextureComponent>{}, Components::VideoTextureComponent{});
            break;
        }
    }

  private:
    // Adds one terrain effect by type id, stacked above any already there.
    // Records it so Undo removes exactly what this preset created.
    void AddTerrainEffect(ECS::ComponentTypeId effectTypeId)
    {
        const int32 stackOrder = TerrainECS::NextEffectStackOrder(*m_World, m_Entity);
        if (!TerrainECS::AddTerrainEffectDefault(*m_World, m_Entity, effectTypeId, stackOrder))
            return;
        m_AddedTerrainEffectTypeId = effectTypeId;
        NotifyComponentChanged(m_Notifications, m_World, m_Entity, effectTypeId);
    }

    std::string m_Name;
    ECS::World* m_World = nullptr;                                // not owned
    Editor::EditorChangeNotifications* m_Notifications = nullptr; // not owned
    ECS::EntityHandle m_Entity{};
    ComponentPreset m_Preset{};

    bool m_AddedCamera = false;
    bool m_AddedLight = false;
    bool m_AddedSkyEnvironment = false;
    bool m_AddedSkybox = false;
    bool m_AddedPhysicsBody = false;
    bool m_AddedCharacterController = false;
    bool m_AddedPhysicsCollider = false;
    bool m_AddedBoxShape = false;
    bool m_AddedSphereShape = false;
    bool m_AddedPlaneShape = false;
    bool m_AddedAudioEmitter = false;
    bool m_AddedAudioListener = false;
    bool m_AddedTerrainModifierVolume = false;
    ECS::ComponentTypeId m_AddedTerrainEffectTypeId = 0;
    bool m_AddedSplineComponent = false;
    bool m_AddedValueCurve = false;
    bool m_AddedPostProcessVolume = false;
    bool m_AddedWindVolume = false;
    bool m_AddedParticleEmitter = false;
    bool m_AddedParticleCollisionEvents = false;
    bool m_AddedVideoTexture = false;
};

// Map context menu add-command IDs to ComponentPreset (kCmdInspectorAddPresetBase + ordinal).
// Returns false if cmd is not an add-component command.
static bool TryGetAddPreset(uint32_t cmd, ComponentPreset& outPreset)
{
    if (cmd < kCmdInspectorAddPresetBase)
        return false;
    const uint32_t ord = cmd - kCmdInspectorAddPresetBase;
    if (ord >= static_cast<uint32_t>(ComponentPreset::Count))
        return false;
    outPreset = static_cast<ComponentPreset>(ord);
    return true;
}

static const char* ComponentPickerFallbackIconClass(const std::string& category)
{
    if (category == "Animation")
        return "inspector-section-icon-animator";
    if (category == "Atmosphere")
        return "inspector-section-icon-post-process";
    if (category == "Audio")
        return "inspector-section-icon-audio-emitter";
    if (category == "Physics")
        return "inspector-section-icon-physics";
    if (category == "Post FX")
        return "inspector-section-icon-post-process";
    if (category == "Rendering")
        return "inspector-section-icon-mesh-renderer";
    if (category == "Spline")
        return "inspector-section-icon-spline";
    if (category == "Terrain")
        return "inspector-section-icon-terrain";
    return "inspector-section-icon-default";
}

static std::string ComponentPickerIconClass(ECS::ComponentTypeId typeId, const std::string& category)
{
    std::string iconClass = ComponentIconClass(nullptr, ECS::EntityHandle{}, typeId);
    if (!iconClass.empty() && iconClass != "inspector-section-icon-default")
        return iconClass;
    return ComponentPickerFallbackIconClass(category);
}

static std::string ComponentPickerIconClass(ComponentPreset preset, const std::string& category)
{
    const std::vector<ECS::ComponentTypeId> typeIds = ComponentTypeIdsForPreset(preset);
    for (auto it = typeIds.rbegin(); it != typeIds.rend(); ++it)
    {
        std::string iconClass = ComponentIconClass(nullptr, ECS::EntityHandle{}, *it);
        if (!iconClass.empty() && iconClass != "inspector-section-icon-default")
            return iconClass;
    }
    return ComponentPickerFallbackIconClass(category);
}

static const char* ComponentPickerIconPathForClass(const std::string& iconClass)
{
    if (iconClass == "inspector-section-icon-transform")
        return "Icons/vector.png";
    if (iconClass == "inspector-section-icon-mesh-renderer")
        return "Icons/Sphere.png";
    if (iconClass == "inspector-section-icon-skinned-mesh")
        return "Icons/person.png";
    if (iconClass == "inspector-section-icon-material")
        return "Icons/material.png";
    if (iconClass == "inspector-section-icon-camera")
        return "Icons/videocam.png";
    if (iconClass == "inspector-section-icon-light-ambient")
        return "Icons/AmbientLight.png";
    if (iconClass == "inspector-section-icon-light-directional")
        return "Icons/DirectionalLight.png";
    if (iconClass == "inspector-section-icon-light-spot")
        return "Icons/SpotLight.png";
    if (iconClass == "inspector-section-icon-light-point")
        return "Icons/PointLight.png";
    if (iconClass == "inspector-section-icon-light-area")
        return "Icons/arealight.png";
    if (iconClass == "inspector-section-icon-light-volume")
        return "Icons/PointLight.png";
    if (iconClass == "inspector-section-icon-sky")
        return "Icons/skyenvironment.png";
    if (iconClass == "inspector-section-icon-reflection-probe")
        return "Icons/probe.png";
    if (iconClass == "inspector-section-icon-post-process")
        return "Icons/postfx.png";
    if (iconClass == "inspector-section-icon-post-process-volume")
        return "Icons/color-filter.svg";
    if (iconClass == "inspector-section-icon-wind-volume")
        return "Icons/wind.svg";
    if (iconClass == "inspector-section-icon-terrain")
        return "Icons/terrain.png";
    if (iconClass == "inspector-section-icon-grass")
        return "Icons/grass.png";
    if (iconClass == "inspector-section-icon-tree-generator")
        return "Icons/leaf.svg";
    if (iconClass == "inspector-section-icon-ocean")
        return "Icons/ocean.svg";
    if (iconClass == "inspector-section-icon-particles")
        return "Icons/sparkles.png";
    if (iconClass == "inspector-section-icon-video-texture")
        return "Icons/film.png";
    if (iconClass == "inspector-section-icon-lens-flare")
        return "Icons/flame.png";
    if (iconClass == "inspector-section-icon-spline")
        return "Icons/GizmoSpline.png";
    if (iconClass == "inspector-section-icon-measure")
        return "Icons/Ruler.png";
    if (iconClass == "inspector-section-icon-physics" || iconClass == "inspector-section-icon-physics-settings")
        return "Icons/PhysicsSettings.png";
    if (iconClass == "inspector-section-icon-shape-cube")
        return "Icons/Cube.png";
    if (iconClass == "inspector-section-icon-shape-sphere")
        return "Icons/Sphere.png";
    if (iconClass == "inspector-section-icon-shape-capsule")
        return "Icons/Capsule.png";
    if (iconClass == "inspector-section-icon-shape-plane")
        return "Icons/Plane.png";
    if (iconClass == "inspector-section-icon-audio-emitter" || iconClass == "inspector-section-icon-audio-listener")
        return "Icons/music-note-icon@32px.png";
    if (iconClass == "inspector-section-icon-animator" || iconClass == "inspector-section-icon-animated-node")
        return "Icons/film.png";
    if (iconClass == "inspector-section-icon-lod-group")
        return "Icons/layout3split.png";
    if (iconClass == "inspector-section-icon-render-layer")
        return "Icons/list.png";
    return "Icons/node.png";
}

static SearchIcon ComponentPickerSearchIcon(const std::string& iconClass)
{
    SearchIcon icon = SearchIcon::FromClass(iconClass);
    icon.ImagePath = ComponentPickerIconPathForClass(iconClass);
    return icon;
}

// A reflection-discoverable single component, chosen by runtime type id. Stored
// in a SearchResultItem's UserData to distinguish it from a curated Preset.
struct AddSingleComponentChoice
{
    ECS::ComponentTypeId TypeId = 0;
};

// Adds one default-constructed component (by type id) via ECS::ComponentFactory
// — the typed path that honours member defaults. Undo removes it by type id,
// but only if this command is what added it (no-op if it was already present).
class AddSingleComponentCommand final : public Editor::IEditorCommand
{
  public:
    AddSingleComponentCommand(std::string name,
                              ECS::World* world,
                              Editor::EditorChangeNotifications* notifications,
                              ECS::EntityHandle entity,
                              ECS::ComponentTypeId typeId)
        : m_Name(std::move(name)), m_World(world), m_Notifications(notifications), m_Entity(entity), m_TypeId(typeId)
    {
    }

    const char* GetName() const override { return m_Name.c_str(); }

    void Do() override
    {
        if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
            return;
        std::vector<uint8_t> existing;
        m_Added = !m_World->CaptureComponentBytes(m_Entity, m_TypeId, existing);
        if (!m_Added)
            return;
        if (ECS::ComponentFactory::Create(*m_World, m_Entity, m_TypeId))
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, m_TypeId);
        else
            m_Added = false;
    }

    void Undo() override
    {
        if (!m_Added || !m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
            return;
        m_World->RemoveComponentByTypeIdImmediate(m_Entity, m_TypeId);
        NotifyComponentChanged(m_Notifications, m_World, m_Entity, m_TypeId);
    }

  private:
    std::string m_Name;
    ECS::World* m_World = nullptr;
    Editor::EditorChangeNotifications* m_Notifications = nullptr;
    ECS::EntityHandle m_Entity{};
    ECS::ComponentTypeId m_TypeId = 0;
    bool m_Added = false;
};

} // namespace

class ComponentPresetSearchProvider : public ISearchProvider
{
  public:
    std::vector<SearchDialogFilterOption> GetFilterOptions()
    {
        if (m_Entries.empty())
            BuildEntries();

        std::set<std::string> categories;
        for (const auto& entry : m_Entries)
            categories.insert(entry.Category);
        for (const auto& entry : m_Singles)
            categories.insert(entry.Category);

        std::vector<SearchDialogFilterOption> options;
        options.reserve(categories.size() + 1);
        options.push_back({"all", "All components", ""});
        for (const std::string& category : categories)
            options.push_back({category, category, "category:" + category + "\n"});
        return options;
    }

    void BeginSearch(const std::string& query, ResultSink sink) override
    {
        if (m_Entries.empty())
            BuildEntries();

        std::vector<SearchResultItem> results;
        const PickerQueryFilter filter = PickerQueryFilter::Parse(query);

        // Curated multi-component presets.
        for (size_t i = 0; i < m_Entries.size(); ++i)
        {
            const auto& entry = m_Entries[i];
            if (!filter.Matches(entry.Label, entry.Category))
                continue;

            SearchResultItem item;
            item.Id = static_cast<SearchItemId>(i + 1);
            item.Label = entry.Label;
            item.Detail = entry.Category;
            item.Icon = ComponentPickerSearchIcon(ComponentPickerIconClass(entry.Preset, entry.Category));
            item.UserData = entry.Preset;
            results.push_back(std::move(item));
        }

        // Reflection-discoverable single components (default-construct via factory).
        for (size_t i = 0; i < m_Singles.size(); ++i)
        {
            const auto& entry = m_Singles[i];
            if (!filter.Matches(entry.Label, entry.Category))
                continue;

            SearchResultItem item;
            item.Id = static_cast<SearchItemId>(m_Entries.size() + i + 1);
            item.Label = entry.Label;
            item.Detail = entry.Category;
            item.Icon = ComponentPickerSearchIcon(ComponentPickerIconClass(entry.TypeId, entry.Category));
            item.UserData = AddSingleComponentChoice{entry.TypeId};
            results.push_back(std::move(item));
        }

        std::sort(results.begin(), results.end(), [](const SearchResultItem& a, const SearchResultItem& b) {
            if (a.Detail != b.Detail)
                return a.Detail < b.Detail;
            return a.Label < b.Label;
        });

        sink(std::move(results), true);
    }

    void CancelSearch() override {}

    std::string GetPlaceholderText() const override { return "Search components..."; }

private:
    struct SingleComponentEntry
    {
        std::string Label;
        std::string Category;
        ECS::ComponentTypeId TypeId = 0;
    };

    void BuildEntries()
    {
        m_Entries = ComponentPresetCatalog();
        BuildSingles();
    }

    void BuildSingles()
    {
        m_Singles.clear();

        // Components a curated preset already adds shouldn't also appear as a
        // standalone single (avoids Camera/Light duplicating their presets).
        std::unordered_set<ECS::ComponentTypeId> presetCovered;
        for (const auto& preset : ComponentPresetCatalog())
            for (ECS::ComponentTypeId id : ComponentTypeIdsForPreset(preset.Preset))
                presetCovered.insert(id);

        // The modifier volume is the exception: every terrain preset adds one,
        // but none of them IS one. An empty volume authored on its own — region
        // first, effects from its Add Effect picker after — is a real flow, so
        // it keeps its standalone entry instead of being deduplicated away.
        presetCovered.erase(ECS::GetComponentTypeId<Components::TerrainModifierVolume>());

        // Any component with a default-construct factory is addable, unless the
        // inspector hides it, a preset covers it, or it is a volume-owned stack
        // effect (post-process effects add only through Add Post FX, terrain
        // effects only through the modifier volume's Add Effect). Title +
        // filtering are derived (reflection + ShouldHideComponent) — no
        // per-component chrome.
        for (ECS::ComponentTypeId typeId : ECS::ComponentFactory::RegisteredTypes())
        {
            if (ShouldHideComponent(typeId) || presetCovered.count(typeId) != 0)
                continue;
            if (Rendering::PostProcessEffectRegistry::Find(typeId) != nullptr)
                continue;
            if (TerrainECS::IsTerrainEffectComponent(typeId))
                continue;
            m_Singles.push_back({ComponentTitle(typeId), ComponentCategory(typeId), typeId});
        }
        std::sort(m_Singles.begin(), m_Singles.end(), [](const SingleComponentEntry& a, const SingleComponentEntry& b) {
            if (a.Category != b.Category)
                return a.Category < b.Category;
            return a.Label < b.Label;
        });
    }

    std::vector<ComponentPresetEntry> m_Entries;
    std::vector<SingleComponentEntry> m_Singles;
};

InspectorPanel::InspectorPanel()
    : DockPanel("Inspector")
{
    m_VcsController = std::make_unique<Editor::InspectorVcsController>(*this);
    // Generate unique instance ID for this inspector panel
    m_InstanceId = "Inspector_" + std::to_string(++s_InspectorInstanceCounter);
    SetId(m_InstanceId);

    AddClass("inspector-panel");

    auto top = std::make_unique<UIElement>();
    top->AddClass("inspector-top");
    m_TopRoot = top.get();
    AddChild(std::move(top));

    auto scroll = std::make_unique<ScrollView>();
    scroll->AddClass("inspector-scrollview");
    m_Scroll = scroll.get();
    m_ContentRoot = m_Scroll->GetViewport();
    AddChild(std::move(scroll));

    // Fixed (non-scrolling) bottom bar: Add Component UX lives here so dropdown popups
    // are not clipped by the ScrollView viewport scissor.
    auto bottom = std::make_unique<UIElement>();
    bottom->AddClass("inspector-bottom");
    m_BottomRoot = bottom.get();
    AddChild(std::move(bottom));

    // Search bar placement and visibility are controlled via Settings.
    auto built = BuildPanelSearchBar(
        "inspector-search-field",
        []()
        { return SettingsPanel::GetSearchBarsVisible(); },
        {}, // onTextChanged (on Enter)
        [this](const std::string& text)
        { ApplySearchFilter(text); }, // onTextChanging (real-time)
        {{"all", "All fields"}, {"label", "Labels"}, {"section", "Sections"}},
        [this](const std::string& scope)
        {
            m_SearchFieldScope = scope;
            ApplySearchFilter(m_SearchField ? m_SearchField->GetValue() : std::string{});
        }
    );
    m_SearchBar = built.RootPtr;
    m_SearchField = built.FieldPtr;
    AddChild(std::move(built.Root));
    SettingsPanel::RegisterSearchBar(m_SearchBar, built.IconPtr);

    // Initial message
    ShowSelectedAssets({});
}

void InspectorPanel::EnsureInspectorTabLockMounted()
{
    UIManager* ui = GetOwnerManager();
    if (!ui)
        return;
    UIElement* root = ui->GetRootElement();
    if (!root)
        return;

    // Tab ids encode the registered panel id (see DockspaceElement::BuildFromNode),
    // not the inspector element's unique m_InstanceId. EditorApplication wires
    // m_DockTabPanelId immediately after RegisterPanel; if it isn't set we have
    // no tab to attach the lock to.
    if (m_DockTabPanelId.empty())
    {
        m_LockButton = nullptr;
        return;
    }
    UIElement* tab = root->FindById("tab:" + m_DockTabPanelId);
    if (!tab)
    {
        m_LockButton = nullptr;
        return;
    }

    if (UIElement* existing = tab->FindById("inspector-tab-lock"))
    {
        m_LockButton = existing;
        m_LockButton->AddClass("icon-button");
        if (m_Locked)
            m_LockButton->AddClass("locked");
        else
            m_LockButton->RemoveClass("locked");
        return;
    }

    m_LockButton = nullptr;

    auto lockBtn = std::make_unique<UIElement>();
    lockBtn->SetId("inspector-tab-lock");
    lockBtn->AddClass("icon-button");
    lockBtn->AddClass("inspector-lock-button");
    m_LockButton = lockBtn.get();
    lockBtn->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e)
                                  {
        SetLocked(!m_Locked);
        e.Stop(); });

    tab->InsertChild(0, std::move(lockBtn));
    if (m_Locked)
        m_LockButton->AddClass("locked");
}

void InspectorPanel::SetLocked(bool locked)
{
    EnsureInspectorTabLockMounted();
    m_Locked = locked;
    if (m_LockButton)
    {
        if (m_Locked)
            m_LockButton->AddClass("locked");
        else
            m_LockButton->RemoveClass("locked");
    }
    SyncInspectorHistoryButtonState();
}

void InspectorPanel::SetSoloKeepTransform(bool enabled)
{
    m_SoloKeepTransformEnabled = enabled;

    // Only affect section visibility when solo mode is active.
    if (!m_SoloSectionsEnabled || !m_ContentRoot)
        return;

    if (m_TransformSection)
    {
        // When enabling "keep Transform always visible", expand Transform.
        // When disabling it, collapse Transform to match the toggle state.
        m_TransformSection->SetCollapsed(!enabled);
    }

    // Re-apply solo handlers so pinned/non-pinned logic is updated.
    WireSoloHandlersForSections();
}

void InspectorPanel::WireSoloHandlersForSections()
{
    if (!m_ContentRoot || !m_SoloSectionsEnabled)
        return;

    InspectorSection* firstExpandedNonPinned = nullptr;
    for (const auto& child : m_ContentRoot->GetChildren())
    {
        if (auto* section = dynamic_cast<InspectorSection*>(child.get()))
        {
            const bool isPinned = m_SoloKeepTransformEnabled && section == m_TransformSection;

            if (!section->IsCollapsed() && !isPinned)
            {
                if (!firstExpandedNonPinned)
                {
                    firstExpandedNonPinned = section;
                }
                else
                {
                    section->SetCollapsed(true);
                }
            }

            section->SetOnCollapsedChanged([this](InspectorSection& self, bool collapsed)
                                           {
                                               if (collapsed || !m_SoloSectionsEnabled || !m_ContentRoot)
                                                   return;

                                               for (const auto& otherChild : m_ContentRoot->GetChildren())
                                               {
                                                   auto* otherSection = dynamic_cast<InspectorSection*>(otherChild.get());
                                                   const bool otherPinned =
                                                       m_SoloKeepTransformEnabled && otherSection == m_TransformSection;
                                                   if (otherSection && otherSection != &self && !otherPinned && !otherSection->IsCollapsed())
                                                   {
                                                       otherSection->SetCollapsed(true);
                                                   }
                                               } });
        }
    }
}

void InspectorPanel::ApplyToggleAlign(InspectorPanel* panel, const std::string& value)
{
    if (!panel)
        return;
    panel->RemoveClass("inspector-toggle-align-center");
    panel->RemoveClass("inspector-toggle-align-right");
    panel->RemoveClass("inspector-toggle-offset");
    if (value == "middle")
        panel->AddClass("inspector-toggle-offset");
    else if (value == "right")
        panel->AddClass("inspector-toggle-align-right");
}

void InspectorPanel::ApplySoloSections(InspectorPanel* panel, bool enabled)
{
    if (!panel)
        return;

    panel->m_SoloSectionsEnabled = enabled;

    if (!panel->m_ContentRoot)
        return;

    if (enabled)
    {
        panel->WireSoloHandlersForSections();
    }
    else
    {
        // Disable solo behaviour: clear callbacks so sections operate independently again.
        for (const auto& child : panel->m_ContentRoot->GetChildren())
        {
            if (auto* section = dynamic_cast<InspectorSection*>(child.get()))
            {
                section->SetOnCollapsedChanged({});
            }
        }
    }
}

InspectorPanel::~InspectorPanel()
{
    if (m_SearchBar)
        SettingsPanel::UnregisterSearchBar(m_SearchBar);

    if (m_ChangeNotifications && m_ComponentSub)
    {
        m_ChangeNotifications->Unsubscribe(m_ComponentSub);
        m_ComponentSub = {};
    }
    if (m_ChangeNotifications && m_StructureSub)
    {
        m_ChangeNotifications->Unsubscribe(m_StructureSub);
        m_StructureSub = {};
    }
    ReleaseShownAssetReloadSubscription();
}

void InspectorPanel::SetContext(const EditorContext* ctx)
{
    if (m_VcsController)
        m_VcsController->SetContext(ctx);
    m_Context = ctx;
    m_Window = ctx ? ctx->MainWindow : nullptr;
    // Prefer context-provided undo service if the panel wasn't explicitly wired.
    if (ctx && ctx->UndoRedo && !m_Undo)
    {
        m_Undo = ctx->UndoRedo;
    }
    if (ctx)
        PostAction([this]()
                   { EnsureInspectorTabLockMounted(); });
}

void InspectorPanel::EnsureComponentSettingsStorageLoaded()
{
    const std::filesystem::path workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
    if (workspaceRoot.empty())
        return;

    if (m_ComponentSettingsLoaded && m_ComponentSettingsWorkspaceRoot == workspaceRoot)
        return;

    m_ComponentSettingsLoaded = true;
    m_ComponentSettingsWorkspaceRoot = workspaceRoot;
    m_SavedComponentSettingsByType.clear();
    m_ContextSavedSettingsMenuIndices.clear();

    Editor::SettingsStore store = Editor::OpenProjectSettings(workspaceRoot);
    std::string err;
    (void)store.Load(&err);
    const nlohmann::json& root = store.Json();
    const auto it = root.find(kProjectInspectorSavedSettingsKey);
    if (it == root.end() || !it->is_array())
        return;

    for (const auto& componentEntry : *it)
    {
        if (!componentEntry.is_object())
            continue;

        const auto compNameIt = componentEntry.find("component");
        const auto itemsIt = componentEntry.find("items");
        if (compNameIt == componentEntry.end() || !compNameIt->is_string() ||
            itemsIt == componentEntry.end() || !itemsIt->is_array())
        {
            continue;
        }

        ECS::ComponentTypeId typeId{};
        if (!TryFindComponentTypeIdByName(compNameIt->get<std::string>(), typeId))
            continue;

        std::vector<SavedComponentSettings> loaded;
        loaded.reserve(itemsIt->size());
        for (const auto& item : *itemsIt)
        {
            if (!item.is_object())
                continue;
            const auto nameIt = item.find("name");
            const auto bytesIt = item.find("bytesHex");
            if (nameIt == item.end() || !nameIt->is_string() ||
                bytesIt == item.end() || !bytesIt->is_string())
            {
                continue;
            }

            SavedComponentSettings saved{};
            saved.Name = nameIt->get<std::string>();
            if (saved.Name.empty())
                continue;
            if (!FromHex(bytesIt->get<std::string>(), saved.Bytes))
                continue;
            loaded.push_back(std::move(saved));
            if (loaded.size() >= kMaxSavedComponentSettingsPerType)
                break;
        }

        if (!loaded.empty())
            m_SavedComponentSettingsByType[typeId] = std::move(loaded);
    }
}

void InspectorPanel::SaveComponentSettingsToProject()
{
    const std::filesystem::path workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
    if (workspaceRoot.empty())
        return;

    Editor::SettingsStore store = Editor::OpenProjectSettings(workspaceRoot);
    std::string err;
    (void)store.Load(&err);

    std::vector<std::pair<std::string, nlohmann::json>> serialized;
    serialized.reserve(m_SavedComponentSettingsByType.size());
    for (const auto& [typeId, settingsList] : m_SavedComponentSettingsByType)
    {
        if (settingsList.empty())
            continue;
        const std::string componentName = ComponentTitle(typeId);
        if (componentName.empty())
            continue;

        nlohmann::json items = nlohmann::json::array();
        for (const SavedComponentSettings& saved : settingsList)
        {
            if (saved.Name.empty())
                continue;
            nlohmann::json item = nlohmann::json::object();
            item["name"] = saved.Name;
            item["bytesHex"] = ToHex(saved.Bytes);
            items.push_back(std::move(item));
        }
        if (items.empty())
            continue;

        nlohmann::json componentEntry = nlohmann::json::object();
        componentEntry["component"] = componentName;
        componentEntry["items"] = std::move(items);
        serialized.emplace_back(componentName, std::move(componentEntry));
    }

    std::sort(serialized.begin(), serialized.end(),
              [](const auto& a, const auto& b)
              { return a.first < b.first; });

    nlohmann::json out = nlohmann::json::array();
    for (auto& pair : serialized)
        out.push_back(std::move(pair.second));

    store.SetJson(kProjectInspectorSavedSettingsKey, out);
    (void)store.Save(&err);
}

void InspectorPanel::ExecuteRemoveComponent(ECS::ComponentTypeId typeId)
{
    if (!IsRemovableComponent(typeId))
        return;

    const std::string compName = ComponentTitle(typeId);
    const std::string undoName = std::string("Remove ") + compName + " Component";
    if (m_World)
        Editor::CommitComponentRemoval(*m_World, m_Undo, m_ChangeNotifications, m_Entity, typeId, undoName);
    ShowEntity(m_World, m_Entity, /*force=*/true);
}

void InspectorPanel::EnsurePostProcessVolumeRemoveModal()
{
    if (m_RemovePostProcessConfirmModal)
        return;

    auto modal = std::make_unique<ConfirmActionModal>();
    m_RemovePostProcessConfirmModal = modal.get();
    m_RemovePostProcessConfirmModal->SetOnConfirm([this]()
                                                  {
        if (m_PendingRemoveComponentTypeId)
            ExecuteRemoveComponent(m_PendingRemoveComponentTypeId);
        m_PendingRemoveComponentTypeId = {}; });
    m_RemovePostProcessConfirmModal->SetOnCancel([this]()
                                                 { m_PendingRemoveComponentTypeId = {}; });

    UIManager* ui = GetOwnerManager();
    UIElement* uiRoot = ui ? ui->GetRootElement() : nullptr;
    if (uiRoot)
        uiRoot->AddChild(std::move(modal));
    else
        AddChild(std::move(modal));
}

std::vector<ECS::ComponentTypeId> InspectorPanel::GetContextCopySelection() const
{
    std::vector<ECS::ComponentTypeId> out;
    if (m_SelectedComponentTypes.size() > 1 &&
        m_SelectedComponentTypes.find(m_ContextComponentTypeId) != m_SelectedComponentTypes.end())
    {
        for (ECS::ComponentTypeId typeId : m_ComponentSectionOrder)
        {
            if (m_SelectedComponentTypes.find(typeId) != m_SelectedComponentTypes.end())
                out.push_back(typeId);
        }
    }
    if (out.empty())
        out.push_back(m_ContextComponentTypeId);
    return out;
}

void InspectorPanel::RefreshComponentSectionSelectionVisuals()
{
    // Highlight only when multiple sections are selected (batch workflows).
    const bool highlightMulti = m_SelectedComponentTypes.size() > 1;
    for (const auto& [typeId, section] : m_ComponentSectionsByType)
    {
        if (!section)
            continue;
        if (highlightMulti && m_SelectedComponentTypes.find(typeId) != m_SelectedComponentTypes.end())
            section->AddClass("inspector-section-selected");
        else
            section->RemoveClass("inspector-section-selected");
    }
}

void InspectorPanel::SelectSingleComponentSection(ECS::ComponentTypeId typeId)
{
    m_SelectedComponentTypes.clear();
    m_SelectedComponentTypes.insert(typeId);
    m_ComponentSelectionAnchor = typeId;
    RefreshComponentSectionSelectionVisuals();
}

void InspectorPanel::ToggleComponentSectionSelection(ECS::ComponentTypeId typeId)
{
    auto it = m_SelectedComponentTypes.find(typeId);
    if (it == m_SelectedComponentTypes.end())
        m_SelectedComponentTypes.insert(typeId);
    else
        m_SelectedComponentTypes.erase(it);

    m_ComponentSelectionAnchor = typeId;
    RefreshComponentSectionSelectionVisuals();
}

void InspectorPanel::SelectComponentSectionRange(ECS::ComponentTypeId typeId)
{
    if (m_ComponentSelectionAnchor == ECS::ComponentTypeId{})
    {
        SelectSingleComponentSection(typeId);
        return;
    }

    int anchorIdx = -1;
    int currentIdx = -1;
    for (size_t i = 0; i < m_ComponentSectionOrder.size(); ++i)
    {
        if (m_ComponentSectionOrder[i] == m_ComponentSelectionAnchor)
            anchorIdx = static_cast<int>(i);
        if (m_ComponentSectionOrder[i] == typeId)
            currentIdx = static_cast<int>(i);
    }

    if (anchorIdx < 0 || currentIdx < 0)
    {
        SelectSingleComponentSection(typeId);
        return;
    }

    m_SelectedComponentTypes.clear();
    const int lo = std::min(anchorIdx, currentIdx);
    const int hi = std::max(anchorIdx, currentIdx);
    for (int i = lo; i <= hi; ++i)
        m_SelectedComponentTypes.insert(m_ComponentSectionOrder[static_cast<size_t>(i)]);
    RefreshComponentSectionSelectionVisuals();
}

void InspectorPanel::ClearContent()
{
    m_ShowingGraphNode = false;

    // Stop audio asset preview when inspector focus is lost (e.g. selection changes).
    if (m_AudioPreviewHandle.IsValid())
    {
        if (Audio::AudioSystem* audio = EngineCore::GetInstance().GetAudioSystem())
            audio->Stop(m_AudioPreviewHandle);
        m_AudioPreviewHandle = Audio::INVALID_AUDIO_EMITTER_HANDLE;
    }

    // Clear refresh callbacks eagerly so TickSimulationRefresh cannot
    // invoke callbacks to inspector sections that are about to be destroyed.
    m_SectionRefreshCallbacks.clear();
    // Same rule for the reload hooks: they update widgets in the tree below.
    m_AssetReloadedCallbacks.clear();

    if (!m_ContentRoot && !m_TopRoot && !m_BottomRoot)
    {
        return;
    }

    // If we're in the middle of event dispatch, defer clearing until it's safe.
    if (UIElement::IsInEventDispatch())
    {
        this->PostAction([this]()
                         { this->ClearContent(); });
        return;
    }

    InspectorDrag::FinalizeAndClearLabelDragState();

    // Search state holds non-owning pointers into the content being replaced.
    m_SearchHighlightedElements.clear();
    m_SearchFilteredElements.clear();

    auto clearRoot = [](UIElement* root)
    {
        if (!root)
            return;
        // Snapshot-then-remove via the UIElement helper — see
        // UIElement::RemoveAllChildren for the dispatch-deferral rationale.
        root->RemoveAllChildren();
    };
    clearRoot(m_TopRoot);
    clearRoot(m_ContentRoot);
    clearRoot(m_BottomRoot);
    m_EntityToggle = nullptr;
    m_EntityActivity.Bind(nullptr, nullptr, false);
    m_HistoryButtonsRow = nullptr;
    m_HistoryBackButton = nullptr;
    m_HistoryForwardButton = nullptr;
    m_HeaderIconHost = nullptr;
    m_HeaderTitleField = nullptr;
    m_SectionTypeIds.clear();
    m_SectionDragSource = nullptr;
    m_SectionDragIndicatorTarget = nullptr;
    if (m_SectionDragGhost)
    {
        if (UIElement* parent = m_SectionDragGhost->GetParent())
            parent->RemoveChild(m_SectionDragGhost);
        m_SectionDragGhost = nullptr;
    }
    if (m_SectionDragHoverBlocker)
    {
        if (UIElement* parent = m_SectionDragHoverBlocker->GetParent())
            parent->RemoveChild(m_SectionDragHoverBlocker);
        m_SectionDragHoverBlocker = nullptr;
    }
    m_SectionDragActive = false;
    m_SectionDropIndex = -1;
    m_SectionDragReorderRoot = nullptr;
    m_MaterialSlotSectionIndices.clear();
    m_ComponentSectionsByType.clear();
    m_ComponentSectionOrder.clear();
    m_SelectedComponentTypes.clear();
    m_ComponentSelectionAnchor = {};
}

void InspectorPanel::RefreshCurrentAssetInspector()
{
    if (m_Locked)
        return;
    if (m_LastSelectedAssetPaths.empty())
        return;

    if (UIElement::IsInEventDispatch())
    {
        this->PostAction([this]()
                         { this->RefreshCurrentAssetInspector(); });
        return;
    }

    std::vector<std::filesystem::path> paths = m_LastSelectedAssetPaths;
    const bool prevSuppressHistory = m_SuppressInspectorHistory;
    m_SuppressInspectorHistory = true;
    m_LastSelectedAssetPaths.clear();
    ShowSelectedAssets(paths);
    m_SuppressInspectorHistory = prevSuppressHistory;
}

void InspectorPanel::RequestEntityRefresh()
{
    if (m_EntityRefreshPosted)
        return;
    m_EntityRefreshPosted = true;
    const bool posted = PostAction([this]()
                                   {
        m_EntityRefreshPosted = false;
        if (m_World && m_Entity.IsValid() && m_World->IsValid(m_Entity))
            ShowEntity(m_World, m_Entity, true); });
    // A dropped action never runs to clear the latch, and every later request would be ignored.
    if (!posted)
        m_EntityRefreshPosted = false;
}

void InspectorPanel::ReShowAssetAfterItsReload()
{
    if (m_Locked)
        return;

    // The hooks are copied before they run: an inspector that answers false has
    // asked for the rebuild below, and that rebuild clears the vector they live
    // in while this loop still holds it.
    const std::vector<std::function<bool()>> hooks = m_AssetReloadedCallbacks;
    bool represented = !hooks.empty();
    for (const std::function<bool()>& hook : hooks)
    {
        if (!hook())
            represented = false;
    }
    if (represented)
        return;

    RefreshCurrentAssetInspector();
}

void InspectorPanel::EnsureShownAssetReloadSubscription()
{
    if (m_ShownAssetReloadCallback != 0)
        return;
    EngineCore& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized())
        return;

    // AssetReloaded is the only event handled, and every dispatch of it comes from the
    // main thread — AssetManager::Update drains the completed reloads and dispatches
    // the events it collected once its locks are released (AssetManager.cpp, "Lock
    // released. Safe for handlers to re-enter LoadAsset etc."). Events raised on a
    // worker (a registry scan's AssetModified) return at the first test without
    // reading a member, so re-presenting from here needs no synchronisation. Capturing
    // `this` is safe because the destructor removes the callback and every dispatch that
    // reaches a member runs on the main thread, the thread that destroys this panel;
    // DispatchEvent copies its callback list under the mutex and invokes the copies after
    // releasing it, so the mutex itself orders nothing here.
    m_ShownAssetReloadCallback = engine.GetAssetManager().GetEventDispatcher().AddCallback(
        [this](const AssetEvent& event)
        {
            if (event.EventType != AssetEventType::AssetReloaded)
                return;
            if (!m_SelectedAsset || event.AssetGuid != m_SelectedAsset->GetGUID())
                return;
            ReShowAssetAfterItsReload();
        });
}

void InspectorPanel::ReleaseShownAssetReloadSubscription()
{
    if (m_ShownAssetReloadCallback == 0)
        return;
    EngineCore& engine = EngineCore::GetInstance();
    if (engine.IsInitialized())
        engine.GetAssetManager().GetEventDispatcher().RemoveCallback(m_ShownAssetReloadCallback);
    m_ShownAssetReloadCallback = 0;
}

void InspectorPanel::RefreshCurrentTarget()
{
    if (UIElement::IsInEventDispatch())
    {
        this->PostAction([this]() { RefreshCurrentTarget(); });
        return;
    }

    if (!m_LastSelectedAssetPaths.empty())
    {
        const bool wasLocked = m_Locked;
        m_Locked = false;
        RefreshCurrentAssetInspector();
        m_Locked = wasLocked;
        return;
    }

    if (m_World && m_Entity.IsValid())
        ShowEntity(m_World, m_Entity, /*force=*/true);
}

void InspectorPanel::RequestRefreshForInspectorModule(std::string_view moduleId)
{
    if (moduleId.empty() || m_ModuleInspectorRefreshPending)
        return;

    const InspectorRegistry& inspectors = InspectorRegistry::Get();
    bool relevant = false;
    if (!m_LastSelectedAssetPaths.empty() && m_SelectedAsset)
    {
        relevant = inspectors.IsAssetInspectorOwnedBy(m_SelectedAsset->GetType(), moduleId);
    }
    else if (m_World && m_Entity.IsValid() && m_World->IsValid(m_Entity))
    {
        if (ECS::Archetype* archetype = m_World->GetEntityArchetype(m_Entity))
        {
            for (const ECS::ComponentTypeId typeId : archetype->GetSignature().GetComponents())
            {
                if (inspectors.IsComponentInspectorOwnedBy(typeId, moduleId))
                {
                    relevant = true;
                    break;
                }
            }
        }
    }

    if (!relevant)
        return;

    m_ModuleInspectorRefreshPending = true;
    PostAction([this]()
    {
        m_ModuleInspectorRefreshPending = false;
        RefreshCurrentTarget();
    });
}

bool InspectorSelectionHistoryEntry::Equals(const InspectorSelectionHistoryEntry& o) const
{
    if (EntryKind != o.EntryKind)
        return false;
    switch (EntryKind)
    {
    case Kind::Empty:
        return true;
    case Kind::Entity:
        return World == o.World && Entity == o.Entity;
    case Kind::Entities:
        return World == o.World && Entities == o.Entities;
    case Kind::Assets:
        return AssetPaths == o.AssetPaths;
    case Kind::Script:
        return ScriptPath == o.ScriptPath;
    case Kind::SmartFolder:
        return SmartFolderId == o.SmartFolderId && SmartFolderMgr == o.SmartFolderMgr;
    }
    return false;
}

void InspectorPanel::ClearInspectorHistory()
{
    m_InspectorSelectionHistory.clear();
    m_InspectorHistoryIndex = 0;
    m_HistoryHasCursor = false;
    m_SuppressInspectorHistory = false;
}

void InspectorPanel::RecordInspectorHistory(InspectorSelectionHistoryEntry entry)
{
    // Truncate any "future" entries when appending after navigating back.
    if (m_HistoryHasCursor && m_InspectorHistoryIndex + 1 < m_InspectorSelectionHistory.size())
    {
        m_InspectorSelectionHistory.erase(
            m_InspectorSelectionHistory.begin() + static_cast<std::ptrdiff_t>(m_InspectorHistoryIndex + 1),
            m_InspectorSelectionHistory.end());
    }

    if (m_HistoryHasCursor && !m_InspectorSelectionHistory.empty())
    {
        if (m_InspectorSelectionHistory[m_InspectorHistoryIndex].Equals(entry))
            return;
    }

    m_InspectorSelectionHistory.push_back(std::move(entry));
    m_InspectorHistoryIndex = m_InspectorSelectionHistory.size() - 1;
    m_HistoryHasCursor = true;

    constexpr size_t kMaxHistory = 64;
    while (m_InspectorSelectionHistory.size() > kMaxHistory)
    {
        m_InspectorSelectionHistory.erase(m_InspectorSelectionHistory.begin());
        if (m_InspectorHistoryIndex > 0)
            --m_InspectorHistoryIndex;
    }
}

void InspectorPanel::MountHistoryButtons(UIElement& headerContainer)
{
    auto historyRow = std::make_unique<UIElement>();
    historyRow->SetId("inspector-header-history");
    historyRow->AddClass("inspector-header-history");
    historyRow->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::Gap, StyleLength::Px(2.0f))
        .Set(Style::FlexShrink, 0.0f);
    m_HistoryButtonsRow = historyRow.get();

    auto backBtn = std::make_unique<Button>();
    backBtn->SetId("inspector-history-back");
    backBtn->AddClass("icon-button");
    backBtn->AddClass("inspector-header-history-icon");
    backBtn->AddClass("inspector-history-back-btn");
    backBtn->SetText("");
    backBtn->SetTooltip("Previous selection");
    backBtn->Overrides().Set(Style::FlexShrink, 0.0f);
    m_HistoryBackButton = backBtn.get();
    backBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
                        { NavigateSelectionHistoryBack(); });

    auto fwdBtn = std::make_unique<Button>();
    fwdBtn->SetId("inspector-history-forward");
    fwdBtn->AddClass("icon-button");
    fwdBtn->AddClass("inspector-header-history-icon");
    fwdBtn->AddClass("inspector-history-forward-btn");
    fwdBtn->SetText("");
    fwdBtn->SetTooltip("Next selection");
    fwdBtn->Overrides().Set(Style::FlexShrink, 0.0f);
    m_HistoryForwardButton = fwdBtn.get();
    fwdBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
                       { NavigateSelectionHistoryForward(); });

    historyRow->AddChild(std::move(backBtn));
    historyRow->AddChild(std::move(fwdBtn));
    headerContainer.AddChild(std::move(historyRow));
}

namespace
{
// Classify an asset path into the inspector-header icon kind class. Folder-first check
// must be performed by the caller (only files land here).
const char* ClassifyAssetHeaderKind(const std::filesystem::path& p)
{
    if (!p.has_extension())
        return "inspector-header-kind-asset";
    std::string ext = p.extension().string();
    for (auto& c : ext)
        c = (char)std::tolower((unsigned char)c);
    if (ext == ".scene")
        return "inspector-header-kind-scene";
    if (ext == ".cs" || GetAssetTypeFromExtension(ext) == AssetType::NativeSource)
        return "inspector-header-kind-script";
    if (ext == ".mp4" || ext == ".mov" || ext == ".avi" || ext == ".mkv" ||
        ext == ".m4v" || ext == ".webm" || ext == ".wmv")
        return "inspector-header-kind-video";
    return "inspector-header-kind-asset";
}

// Friendly, human-readable type label for the inspector. NativeSource splits by extension into
// "C++ Header" / "C++ Source"; everything else uses the canonical AssetTypeToString.
std::string FriendlyAssetTypeLabel(AssetType type, const std::filesystem::path& path)
{
    if (type == AssetType::NativeSource)
    {
        std::string ext = path.extension().string();
        for (auto& c : ext)
            c = (char)std::tolower((unsigned char)c);
        if (ext == ".h" || ext == ".hpp" || ext == ".hxx" || ext == ".hh")
            return "C++ Header";
        if (ext == ".c")
            return "C Source";
        return "C++ Source";
    }
    return AssetTypeToString(type);
}
} // namespace

void InspectorPanel::ApplyAssetIconToHeader(UIElement* iconHost, const std::filesystem::path& assetPath)
{
    if (!iconHost || assetPath.empty())
        return;

    auto applyThumb = [](UIElement* host, const std::string& relOrEngine)
    {
        if (!host)
            return;
        if (relOrEngine.empty())
        {
            UI::Layout::ClearBackgroundOverride(*host);
            return;
        }
        constexpr const char* kEnginePrefix = "engine:";
        constexpr size_t kEnginePrefixLen = 7;
        if (relOrEngine.rfind(kEnginePrefix, 0) == 0)
            UI::Layout::SetBackgroundResourceName(*host, relOrEngine.substr(kEnginePrefixLen));
        else
            UI::Layout::SetBackgroundPath(*host, relOrEngine);
    };

    IThumbnailProvider* thumbs = (m_Context ? m_Context->Thumbnails : nullptr);
    if (!thumbs)
        return;

    const std::string iconId = iconHost->GetId();
    std::string immediate = thumbs->GetOrRequest(
        assetPath, 32,
        [this, iconId, applyThumb, post = GetPostHandle()](const std::string& rel)
        {
            if (rel.empty() || iconId.empty())
                return;
            post.Post([this, iconId, rel, applyThumb]()
                             {
                if (UIElement* host = this->FindById(iconId))
                {
                    applyThumb(host, rel);
                    host->AddClass("has-thumbnail");
                } });
        });

    if (!immediate.empty())
    {
        applyThumb(iconHost, immediate);
        iconHost->AddClass("has-thumbnail");
    }
}

void InspectorPanel::BuildSimpleTopHeader(const std::string& titleText,
                                          const std::string& iconClass,
                                          std::function<void(const std::string&)> onTitleChanging,
                                          std::function<void(const std::string&)> onTitleChanged)
{
    if (!m_TopRoot)
        return;

    auto headerContainer = std::make_unique<UIElement>();
    headerContainer->AddClass("inspector-header-container");

    auto iconHost = std::make_unique<UIElement>();
    iconHost->AddClass("inspector-header-kind-icon");
    if (!iconClass.empty())
        iconHost->AddClass(iconClass);
    // Stable id so async thumbnail callbacks can locate this icon after rebuilds.
    iconHost->SetId("inspector-header-kind-icon-host");
    m_HeaderIconHost = iconHost.get();
    headerContainer->AddChild(std::move(iconHost));

    auto title = std::make_unique<TextField>();
    title->AddClass("inspector-header");
    title->SetValue(titleText);
    if (onTitleChanging)
        title->SetOnValueChanging(std::move(onTitleChanging));
    if (onTitleChanged)
        title->SetOnValueChanged(std::move(onTitleChanged));
    m_HeaderTitleField = title.get();
    headerContainer->AddChild(std::move(title));

    MountHistoryButtons(*headerContainer);

    // Reserve space where the entity enable/disable toggle would sit so the arrows
    // keep a consistent horizontal position across all inspector modes.
    auto togglePlaceholder = std::make_unique<UIElement>();
    togglePlaceholder->AddClass("inspector-header-toggle-placeholder");
    togglePlaceholder->Overrides()
        .Set(Style::Width, StyleLength::Px(36.0f))
        .Set(Style::MinWidth, StyleLength::Px(36.0f))
        .Set(Style::MaxWidth, StyleLength::Px(36.0f))
        .Set(Style::Height, StyleLength::Px(20.0f))
        .Set(Style::FlexShrink, 0.0f);
    headerContainer->AddChild(std::move(togglePlaceholder));

    m_TopRoot->AddChild(std::move(headerContainer));
    SyncInspectorHistoryButtonState();
}

void InspectorPanel::NavigateInspectorHistoryAt(size_t index)
{
    if (m_Locked)
        return;
    if (index >= m_InspectorSelectionHistory.size())
        return;

    const InspectorSelectionHistoryEntry target = m_InspectorSelectionHistory[index];
    m_SuppressInspectorHistory = true;
    m_InspectorHistoryIndex = index;
    m_HistoryHasCursor = true;

    switch (target.EntryKind)
    {
    case InspectorSelectionHistoryEntry::Kind::Empty:
        ShowSelectedAssets({});
        break;

    case InspectorSelectionHistoryEntry::Kind::Entity:
    {
        if (!target.World || !target.Entity.IsValid() || !target.World->IsValid(target.Entity))
        {
            ShowSelectedAssets({});
            break;
        }
        if (m_OnHistoryEntityNavigate)
            m_OnHistoryEntityNavigate(target.World, target.Entity);
        else
            ShowEntity(target.World, target.Entity, true);
        break;
    }

    case InspectorSelectionHistoryEntry::Kind::Entities:
    {
        std::vector<EntityHandle> validEntities;
        validEntities.reserve(target.Entities.size());
        for (const EntityHandle& h : target.Entities)
        {
            if (target.World && h.IsValid() && target.World->IsValid(h))
                validEntities.push_back(h);
        }
        if (validEntities.empty())
        {
            ShowSelectedAssets({});
            break;
        }
        if (m_OnHistoryEntitiesNavigate)
            m_OnHistoryEntitiesNavigate(target.World, validEntities);
        else
            ShowEntities(target.World, validEntities);
        break;
    }

    case InspectorSelectionHistoryEntry::Kind::Assets:
    {
        if (m_OnHistoryAssetNavigate)
            m_OnHistoryAssetNavigate(target.AssetPaths);
        // Always refresh the inspector view in case the external sync is async.
        ShowSelectedAssets(target.AssetPaths);
        break;
    }

    case InspectorSelectionHistoryEntry::Kind::Script:
    {
        if (m_OnHistoryScriptNavigate)
            m_OnHistoryScriptNavigate(target.ScriptPath);
        else
            ShowSelectedAssets({target.ScriptPath});
        break;
    }

    case InspectorSelectionHistoryEntry::Kind::SmartFolder:
    {
        if (target.SmartFolderMgr && !target.SmartFolderId.empty())
            ShowSmartFolder(target.SmartFolderId, target.SmartFolderMgr);
        else
            ShowSelectedAssets({});
        break;
    }
    }

    m_SuppressInspectorHistory = false;
    SyncInspectorHistoryButtonState();
}

void InspectorPanel::NavigateSelectionHistoryBack()
{
    if (m_Locked || m_InspectorSelectionHistory.empty() || m_InspectorHistoryIndex == 0)
        return;
    NavigateInspectorHistoryAt(m_InspectorHistoryIndex - 1);
}

void InspectorPanel::NavigateSelectionHistoryForward()
{
    if (m_Locked || m_InspectorSelectionHistory.empty())
        return;
    if (m_InspectorHistoryIndex + 1 >= m_InspectorSelectionHistory.size())
        return;
    NavigateInspectorHistoryAt(m_InspectorHistoryIndex + 1);
}

void InspectorPanel::SyncInspectorHistoryButtonState()
{
    const bool hasHistory = m_InspectorSelectionHistory.size() > 1;
    const bool canBack = hasHistory && !m_Locked && m_InspectorHistoryIndex > 0;
    const bool canFwd =
        hasHistory && !m_Locked && m_InspectorHistoryIndex + 1 < m_InspectorSelectionHistory.size();

    // Hide with Visibility (not Display::None) so the row stays in flow and the
    // header height doesn't shift when locking/unlocking.
    const bool showChrome = !m_Locked;
    if (m_HistoryButtonsRow)
    {
        m_HistoryButtonsRow->Overrides().Set(Style::Visibility, showChrome);
    }
    if (m_HistoryBackButton)
        m_HistoryBackButton->SetEnabled(showChrome && canBack);
    if (m_HistoryForwardButton)
        m_HistoryForwardButton->SetEnabled(showChrome && canFwd);
}

void InspectorPanel::WireMaterialSlotSectionDrag(InspectorSection* slotSection,
                                                 UIElement* slotsContainer,
                                                 uint32_t modelSlotIndex,
                                                 const GUID& modelGuid,
                                                 ECS::EntityHandle entityForOrder)
{
    if (!slotSection || !slotsContainer)
        return;
    m_MaterialSlotSectionIndices[slotSection] = modelSlotIndex;
    BindSectionHeaderDragReorder(
        slotSection,
        slotsContainer,
        /*restrictToMaterialSlotSections=*/true,
        [this, slotsContainer, modelGuid, entityForOrder](UIElement* /*reorderedRoot*/)
        {
            MaterialSlotOrderKey key{};
            key.EntityPacked = entityForOrder.id;
            key.ModelGuid = modelGuid;
            std::vector<uint32_t> order;
            order.reserve(slotsContainer->GetChildren().size());
            for (const auto& ch : slotsContainer->GetChildren())
            {
                auto* sec = dynamic_cast<InspectorSection*>(ch.get());
                if (!sec)
                    continue;
                auto it = m_MaterialSlotSectionIndices.find(sec);
                if (it != m_MaterialSlotSectionIndices.end())
                    order.push_back(it->second);
            }
            AssetMetadata modelMd{};
            auto& am = EngineCore::GetInstance().GetAssetManager();
            if (am.GetRegistry().TryGetAssetMetadata(modelGuid, modelMd) && !modelMd.Path.empty())
            {
                Editor::ApplyModelMaterialSlotDisplayOrderToFiles(modelMd.Path, modelGuid, order, am);
            }
            m_MeshRendererMaterialSlotDisplayOrder[key] = std::move(order);
            if (m_World && entityForOrder.IsValid() && m_World->IsValid(entityForOrder))
                ShowEntity(m_World, entityForOrder, true);
        });
}

void InspectorPanel::PersistComponentOrderFromSections()
{
    m_ComponentOrderEntity = m_Entity;
    m_ComponentOrder.clear();
    if (!m_ContentRoot)
        return;

    // Walk the section DOM depth-first: top-level sections in visual order,
    // with each Volume-nested effect section following its Volume. Material
    // slot sections are not in m_SectionTypeIds and fall through.
    static const ECS::ComponentTypeId pseudoMaterialId =
        ECS::GetComponentTypeId<Editor::MeshRendererMaterialInspectorSection>();
    const std::function<void(UIElement*)> append = [&](UIElement* root)
    {
        for (const auto& child : root->GetChildren())
        {
            auto* sec = dynamic_cast<InspectorSection*>(child.get());
            if (!sec)
                continue;
            auto it = m_SectionTypeIds.find(sec);
            if (it != m_SectionTypeIds.end() && it->second != pseudoMaterialId)
                m_ComponentOrder.push_back(it->second);
            append(sec->GetContentRoot());
        }
    };
    append(m_ContentRoot);

    if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
        return;

    // Keep reorderable LDR post-FX components in sync with the visual order.
    static const ECS::ComponentTypeId colorFilterId = ECS::GetComponentTypeId<Components::ColorFilterEffect>();
    static const ECS::ComponentTypeId casId = ECS::GetComponentTypeId<Components::ContrastAdaptiveSharpenEffect>();
    static const ECS::ComponentTypeId crtId = ECS::GetComponentTypeId<Components::CrtEffect>();
    static const ECS::ComponentTypeId cubeLutId = ECS::GetComponentTypeId<Components::CubeLutEffect>();
    static const ECS::ComponentTypeId vignetteId = ECS::GetComponentTypeId<Components::VignetteEffect>();

    int nextOrder = 0;
    for (ECS::ComponentTypeId orderedId : m_ComponentOrder)
    {
        if (orderedId == colorFilterId)
        {
            if (auto* c = m_World->GetComponentForWrite<Components::ColorFilterEffect>(m_Entity))
                c->StackOrder = nextOrder++;
        }
        else if (orderedId == casId)
        {
            if (auto* c = m_World->GetComponentForWrite<Components::ContrastAdaptiveSharpenEffect>(m_Entity))
                c->StackOrder = nextOrder++;
        }
        else if (orderedId == crtId)
        {
            if (auto* c = m_World->GetComponentForWrite<Components::CrtEffect>(m_Entity))
                c->StackOrder = nextOrder++;
        }
        else if (orderedId == cubeLutId)
        {
            if (auto* c = m_World->GetComponentForWrite<Components::CubeLutEffect>(m_Entity))
                c->StackOrder = nextOrder++;
        }
        else if (orderedId == vignetteId)
        {
            if (auto* c = m_World->GetComponentForWrite<Components::VignetteEffect>(m_Entity))
                c->StackOrder = nextOrder++;
        }
    }

    // Terrain modifier effects run their own stack under their own volume, so
    // they number from zero independently of the LDR post-FX chain above. The
    // stamp folds the canonical effect list, so a section for an effect added
    // there is renumbered without an edit here; sections that are not effects on
    // this entity decline and consume no stack slot.
    int32 nextTerrainOrder = 0;
    for (ECS::ComponentTypeId orderedId : m_ComponentOrder)
        if (TerrainECS::SetTerrainEffectStackOrder(*m_World, m_Entity, orderedId, nextTerrainOrder))
            ++nextTerrainOrder;
}

void InspectorPanel::BindSectionHeaderDragReorder(
    InspectorSection* sectionRaw,
    UIElement* reorderRoot,
    bool restrictToMaterialSlotSections,
    std::function<void(UIElement* reorderedRoot)> onPersistOrder)
{
    if (!sectionRaw || !reorderRoot)
        return;

    const std::function<void(UIElement*)> persistCopy = onPersistOrder;

    sectionRaw->SetOnHeaderDragStarted([this, sectionRaw, reorderRoot](float x, float y)
                                       {
        m_SectionDragSource = sectionRaw;
        m_SectionDragReorderRoot = reorderRoot;
        m_SectionDragActive = true;
        m_SectionDropIndex = -1;
        sectionRaw->AddClass("inspector-section-dragging");

        if (!m_SectionDragHoverBlocker)
        {
            auto blocker = std::make_unique<UIElement>();
            blocker->Overrides()
                .Set(Style::Position, PositionType::Absolute)
                .Set(Style::PositionLeft, StyleLength::Px(0.0f))
                .Set(Style::PositionTop, StyleLength::Px(0.0f))
                .Set(Style::PositionRight, StyleLength::Px(0.0f))
                .Set(Style::PositionBottom, StyleLength::Px(0.0f));
            m_SectionDragHoverBlocker = blocker.get();
            AddChild(std::move(blocker));
        }

        if (UIManager* ui = GetOwnerManager())
        {
            if (UIElement* root = ui->GetRootElement())
            {
                auto ghost = std::make_unique<Label>();
                ghost->AddClass("inspector-section-drag-ghost");
                ghost->SetText(sectionRaw->GetTitle());
                ghost->SetOverlayLayer(OverlayLayer::DragPreview);
                ghost->Overrides()
                    .Set(Style::Position, PositionType::Absolute)
                    .Set(Style::PointerEvents, false)
                    .Set(Style::PositionLeft, StyleLength::Px(x + 12.0f))
                    .Set(Style::PositionTop, StyleLength::Px(y - 12.0f));
                m_SectionDragGhost = ghost.get();
                root->AddChild(std::move(ghost));
            }
        } });

    sectionRaw->SetOnHeaderDragMoved([this, sectionRaw, reorderRoot, restrictToMaterialSlotSections](float x, float y)
                                     {
        if (m_SectionDragGhost)
        {
            m_SectionDragGhost->Overrides()
                .Set(Style::PositionLeft, StyleLength::Px(x + 12.0f))
                .Set(Style::PositionTop, StyleLength::Px(y - 12.0f));
            m_SectionDragGhost->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        }

        if (!m_SectionDragActive || !reorderRoot)
            return;

        std::vector<InspectorSection*> sections;
        for (const auto& child : reorderRoot->GetChildren())
        {
            auto* sec = dynamic_cast<InspectorSection*>(child.get());
            if (!sec || sec == m_SectionDragSource)
                continue;
            if (restrictToMaterialSlotSections && !sec->HasClass("inspector-mesh-material-slot"))
                continue;
            sections.push_back(sec);
        }

        int dropIndex = static_cast<int>(sections.size());
        for (int i = 0; i < static_cast<int>(sections.size()); ++i)
        {
            const float centerY = sections[i]->GetLayoutY() + sections[i]->GetLayoutHeight() * 0.5f;
            if (y < centerY)
            {
                dropIndex = i;
                break;
            }
        }
        m_SectionDropIndex = dropIndex;

        if (m_SectionDragIndicatorTarget)
        {
            m_SectionDragIndicatorTarget->RemoveClass("inspector-section-drag-indicator-above");
            m_SectionDragIndicatorTarget->RemoveClass("inspector-section-drag-indicator-below");
            m_SectionDragIndicatorTarget = nullptr;
        }
        if (dropIndex < static_cast<int>(sections.size()))
        {
            sections[dropIndex]->AddClass("inspector-section-drag-indicator-above");
            m_SectionDragIndicatorTarget = sections[dropIndex];
        }
        else if (!sections.empty())
        {
            sections.back()->AddClass("inspector-section-drag-indicator-below");
            m_SectionDragIndicatorTarget = sections.back();
        } });

    sectionRaw->SetOnHeaderDragEnded([this, sectionRaw, reorderRoot, persistCopy]()
                                     {
        sectionRaw->RemoveClass("inspector-section-dragging");
        if (m_SectionDragIndicatorTarget)
        {
            m_SectionDragIndicatorTarget->RemoveClass("inspector-section-drag-indicator-above");
            m_SectionDragIndicatorTarget->RemoveClass("inspector-section-drag-indicator-below");
            m_SectionDragIndicatorTarget = nullptr;
        }
        if (m_SectionDragGhost)
        {
            if (UIElement* parent = m_SectionDragGhost->GetParent())
                parent->RemoveChild(m_SectionDragGhost);
            m_SectionDragGhost = nullptr;
        }
        if (m_SectionDragHoverBlocker)
        {
            if (UIElement* parent = m_SectionDragHoverBlocker->GetParent())
                parent->RemoveChild(m_SectionDragHoverBlocker);
            m_SectionDragHoverBlocker = nullptr;
        }

        m_SectionDragReorderRoot = nullptr;

        if (!m_SectionDragActive)
            return;

        const int dropIndex = m_SectionDropIndex;
        m_SectionDragSource = nullptr;
        m_SectionDragActive = false;
        m_SectionDropIndex = -1;

        if (dropIndex < 0 || !reorderRoot)
            return;

        sectionRaw->PostAction([this, sectionRaw, dropIndex, reorderRoot, persistCopy]() {
            if (!reorderRoot)
                return;

            const auto& children = reorderRoot->GetChildren();

            int srcIdx = -1;
            for (int i = 0; i < static_cast<int>(children.size()); ++i)
            {
                if (children[i].get() == sectionRaw)
                {
                    srcIdx = i;
                    break;
                }
            }
            if (srcIdx < 0)
                return;

            if (dropIndex == srcIdx)
                return;

            std::vector<UIElement*> order;
            order.reserve(children.size());
            for (const auto& c : children)
                order.push_back(c.get());

            std::vector<std::unique_ptr<UIElement>> taken;
            taken.reserve(order.size());
            for (auto* elem : order)
            {
                auto t = reorderRoot->TakeChild(elem);
                if (t)
                    taken.push_back(std::move(t));
            }

            auto moved = std::move(taken[srcIdx]);
            taken.erase(taken.begin() + srcIdx);
            const int insertAt = std::min(dropIndex, static_cast<int>(taken.size()));
            taken.insert(taken.begin() + insertAt, std::move(moved));

            for (auto& child : taken)
                reorderRoot->AddChild(std::move(child));

            if (persistCopy)
                persistCopy(reorderRoot);
        }); });
}

void InspectorPanel::ShowSelectedAssets(const std::vector<std::filesystem::path>& paths)
{
    if (m_Locked)
        return;

    if (UIElement::IsInEventDispatch())
    {
        std::vector<std::filesystem::path> pathsCopy = paths;
        const bool suppress = m_SuppressInspectorHistory;
        this->PostAction([this, pathsCopy, suppress]()
                         {
            const bool prev = m_SuppressInspectorHistory;
            m_SuppressInspectorHistory = suppress;
            this->ShowSelectedAssets(pathsCopy);
            m_SuppressInspectorHistory = prev; });
        return;
    }

    if (!m_World && paths == m_LastSelectedAssetPaths && !m_SuppressInspectorHistory)
    {
        return;
    }
    m_LastSelectedAssetPaths = paths;

    // Clear entity selection state
    m_World = nullptr;
    m_Entity = EntityHandle{};
    m_SelectedAsset.reset();

    // Record every asset selection change (including empty) to the history trail.
    if (!m_SuppressInspectorHistory && !m_Locked)
    {
        InspectorSelectionHistoryEntry entry{};
        if (paths.empty())
            entry.EntryKind = InspectorSelectionHistoryEntry::Kind::Empty;
        else
        {
            entry.EntryKind = InspectorSelectionHistoryEntry::Kind::Assets;
            entry.AssetPaths = paths;
        }
        RecordInspectorHistory(std::move(entry));
    }

    ClearContent();
    if (!m_ContentRoot)
    {
        return;
    }

    if (paths.empty())
    {
        BuildSimpleTopHeader("Nothing selected", "inspector-header-kind-empty");
        auto label = std::make_unique<Label>();
        label->AddClass("inspector-text");
        label->SetText("Select an asset or entity to inspect");
        AddInspectorPaddedContent(m_ContentRoot, std::move(label));
        return;
    }

    // Multi-select: show a summary for now.
    if (paths.size() != 1)
    {
        BuildSimpleTopHeader(std::to_string(paths.size()) + " items selected", "inspector-header-kind-asset");
        auto label = std::make_unique<Label>();
        label->AddClass("inspector-text");
        std::ostringstream oss;
        oss << paths.size() << " items selected:\n";
        size_t n = std::min<size_t>(paths.size(), 10);
        for (size_t i = 0; i < n; ++i)
        {
            oss << " • " << paths[i].string() << "\n";
        }
        if (paths.size() > n)
        {
            oss << " …";
        }
        label->SetText(oss.str());
        AddInspectorPaddedContent(m_ContentRoot, std::move(label));
        return;
    }

    const std::filesystem::path& selectedPath = paths[0];
    std::error_code ec;
    if (std::filesystem::is_directory(selectedPath, ec))
    {
        std::string folderTitle = selectedPath.filename().string();
        if (folderTitle.empty())
            folderTitle = selectedPath.string();
        BuildSimpleTopHeader(folderTitle, "inspector-header-kind-folder");
        auto label = std::make_unique<Label>();
        label->AddClass("inspector-text");
        label->SetText(selectedPath.string());
        AddInspectorPaddedContent(m_ContentRoot, std::move(label));
        return;
    }

    // Video files have no registered AssetType — show a simple description instead.
    {
        static constexpr std::string_view kVideoExts[] = {
            ".mp4", ".mov", ".avi", ".mkv", ".webm", ".m4v", ".wmv"};
        const std::string ext = selectedPath.extension().string();
        bool isVideo = false;
        for (auto& e : kVideoExts)
            if (ext == e)
            {
                isVideo = true;
                break;
            }

        if (isVideo)
        {
            BuildSimpleTopHeader(selectedPath.filename().string(), ClassifyAssetHeaderKind(selectedPath));
            ApplyAssetIconToHeader(m_HeaderIconHost, selectedPath);

            auto info = std::make_unique<Label>();
            info->AddClass("inspector-text");
            std::error_code fec;
            const uintmax_t bytes = std::filesystem::file_size(selectedPath, fec);
            std::ostringstream oss;
            oss << "Type: Video (" << ext.substr(1) << ")\n";
            if (!fec && bytes != static_cast<uintmax_t>(-1))
            {
                if (bytes >= 1024 * 1024)
                    oss << "Size: " << (bytes / (1024 * 1024)) << " MB";
                else
                    oss << "Size: " << (bytes / 1024) << " KB";
            }
            info->SetText(oss.str());
            AddInspectorPaddedContent(m_ContentRoot, std::move(info));
            return;
        }
    }

    // Resolve path -> GUID and load the asset
    AssetManager& assetManager = EngineCore::GetInstance().GetAssetManager();

    // Resolve through AssetManager, which also keeps registry metadata aligned with the
    // path on disk (new files, renames, moves) so Load() never uses a stale path. The
    // RegisterAsset + raw GetAssetGUID pair it replaces performed the same sequence.
    GUID guid = assetManager.ResolveAssetGuid(selectedPath);

    if (guid.IsNull())
    {
        BuildSimpleTopHeader(selectedPath.filename().string(), ClassifyAssetHeaderKind(selectedPath));
        ApplyAssetIconToHeader(m_HeaderIconHost, selectedPath);
        auto label = std::make_unique<Label>();
        label->AddClass("inspector-text");
        label->SetText("Not a registered asset:\n" + selectedPath.string());
        AddInspectorPaddedContent(m_ContentRoot, std::move(label));
        return;
    }

    assetManager.ClearLoadSuppressed(guid);

    m_PendingAssetLoad.reset();
    GameEngine::SharedPtr<Asset> asset = assetManager.GetAsset(guid);
    const bool loadFailed = !asset && m_FailedAssetLoad == guid;
    m_FailedAssetLoad = GUID::Null();
    if (!asset && !loadFailed)
    {
        // Not loaded yet: start the load and say so; PollPendingAssetLoad shows the asset once
        // it lands. Waiting here would hold the main thread for the asset's cook or import.
        m_PendingAssetLoad = std::make_unique<AssetFuture>(assetManager.LoadAssetAsync(guid, AssetLoadPriority::High));
        m_PendingAssetPath = selectedPath;
        m_SelectedAsset = nullptr;
        BuildSimpleTopHeader(selectedPath.stem().string(), ClassifyAssetHeaderKind(selectedPath));
        ApplyAssetIconToHeader(m_HeaderIconHost, selectedPath);
        auto label = std::make_unique<Label>();
        label->AddClass("inspector-text");
        label->SetText("Loading " + selectedPath.filename().string() + "...");
        AddInspectorPaddedContent(m_ContentRoot, std::move(label));
        return;
    }
    m_SelectedAsset = asset;
    EnsureShownAssetReloadSubscription();

    if (!asset)
    {
        BuildSimpleTopHeader(selectedPath.filename().string(), ClassifyAssetHeaderKind(selectedPath));
        ApplyAssetIconToHeader(m_HeaderIconHost, selectedPath);
        auto label = std::make_unique<Label>();
        label->AddClass("inspector-text");
        label->SetText("Failed to load asset:\n" + selectedPath.string());
        AddInspectorPaddedContent(m_ContentRoot, std::move(label));
        return;
    }

    // The selected path, not Asset::GetName(): an asset's own path is the registry's key, which
    // NormalizePathForMap case-folds on Windows and macOS (AssetRegistry.cpp:69-78), so its stem
    // spells "islandterrain" for a file named IslandTerrain. This path came from the asset
    // browser's directory enumeration and carries the real on-disk spelling — the same string the
    // browser row shows. The two branches above already header from it.
    BuildSimpleTopHeader(selectedPath.stem().string(), ClassifyAssetHeaderKind(selectedPath));
    ApplyAssetIconToHeader(m_HeaderIconHost, selectedPath);

    auto assetContentRoot = std::make_unique<UIElement>();
    assetContentRoot->AddClass("inspector-asset-content");
    assetContentRoot->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::PaddingLeft, StyleLength::Px(kInspectorAssetContentHorizontalInsetPx))
        .Set(Style::PaddingRight, StyleLength::Px(kInspectorAssetContentHorizontalInsetPx));
    UIElement* const assetContent = assetContentRoot.get();
    m_ContentRoot->AddChild(std::move(assetContentRoot));

    // Dispatch to registered asset inspector (by AssetType)
    InspectorContext ctx{};
    ctx.Parent = assetContent;
    ctx.Object = asset.get();
    ctx.Undo = m_Undo;
    ctx.ChangeNotifications = m_ChangeNotifications;
    ctx.Thumbnails = m_Context ? m_Context->Thumbnails : nullptr;
    ctx.GetSelectedAssetPaths = m_GetSelectedAssetPaths;
    ctx.SetAudioPreviewHandle = [this](Audio::AudioEmitterHandle h)
    { m_AudioPreviewHandle = h; };
    ctx.OpenColorPickerWindow = m_OpenColorPickerWindow;
    ctx.PingAsset = m_PingAsset;
    ctx.PingAssetPreserveInspector = m_PingAssetPreserveInspector;
    ctx.SelectEntity = m_SelectEntity;
    ctx.OpenScript = m_OpenScript;
    ctx.OpenMaterialGraph = m_OpenMaterialGraph;
    ctx.OpenAsset = m_OpenAsset;
    ctx.RequestInspectorRefresh = [this]()
    { RefreshCurrentAssetInspector(); };
    ctx.AssetReloadedCallbacks = &m_AssetReloadedCallbacks;
    ctx.Window = m_Window;
    ctx.EditorCtx = m_Context;
    ctx.ShowInfoCards = GetInspectorShowInfoCardsPreference();

    if (InspectorFn* fn = InspectorRegistry::Get().TryGetAssetInspector(asset->GetType()))
    {
        (*fn)(ctx);
        return;
    }

    // Fallback: show basic info if no inspector registered.
    {
        AddTextBlock(assetContent, selectedPath.string(), "inspector-asset-path");
        AddTextBlock(assetContent,
                     std::string("Type: ") + FriendlyAssetTypeLabel(asset->GetType(), selectedPath),
                     "inspector-asset-type-line");
    }
}

void InspectorPanel::ShowEntities(World* world, const std::vector<EntityHandle>& entities)
{
    if (m_Locked)
        return;
    if (entities.empty())
    {
        ShowEntity(world, {}, /*force=*/false, /*keepMultiSelection=*/false);
        return;
    }
    if (entities.size() == 1)
    {
        ShowEntity(world, entities[0], /*force=*/false, /*keepMultiSelection=*/false);
        return;
    }

    // Record multi-entity selection to history; inner ShowEntity won't record while suppressed.
    if (!m_SuppressInspectorHistory)
    {
        InspectorSelectionHistoryEntry entry{};
        entry.EntryKind = InspectorSelectionHistoryEntry::Kind::Entities;
        entry.World = world;
        entry.Entities = entities;
        RecordInspectorHistory(std::move(entry));
    }

    // Store all entities; ShowEntity builds the UI for the primary (anchor).
    // Force suppression across the nested ShowEntity so it doesn't record a duplicate
    // single-entity entry, then restore the caller's value.
    const bool prevSuppress = m_SuppressInspectorHistory;
    m_SuppressInspectorHistory = true;
    m_Entities = entities;
    ShowEntity(world, entities[0], /*force=*/true);
    m_SuppressInspectorHistory = prevSuppress;
}

void InspectorPanel::RefreshTrackedComponentSignature()
{
    m_LastComponentSignature = CollectVisibleComponentSignature(m_World, m_Entity);
}

void InspectorPanel::BuildComponentSectionBody(InspectorSection* section, ECS::ComponentTypeId typeId)
{
    const ECS::EntityHandle entity = m_Entity;
    static const ECS::ComponentTypeId pseudoMaterialSectionId =
        ECS::GetComponentTypeId<Editor::MeshRendererMaterialInspectorSection>();

    InspectorContext ctx{};
    ctx.Section = section;
    ctx.Parent = section->GetContentRoot();
    ctx.World = m_World;
    ctx.Entity = entity;
    ctx.Entities = m_Entities;
    ctx.Undo = m_Undo;
    ctx.ChangeNotifications = m_ChangeNotifications;
    ctx.Thumbnails = m_Context ? m_Context->Thumbnails : nullptr;
    ctx.GetSelectedAssetPaths = m_GetSelectedAssetPaths;
    ctx.OpenColorPickerWindow = m_OpenColorPickerWindow;
    SectionRefreshCallbacks& refreshCallbacks = m_SectionRefreshCallbacks[typeId];
    ctx.SimulationRefreshCallbacks = &refreshCallbacks.Simulation;
    ctx.FrameRefreshCallbacks = &refreshCallbacks.Frame;
    ctx.PingAsset = m_PingAsset;
    ctx.PingAssetPreserveInspector = m_PingAssetPreserveInspector;
    ctx.SelectEntity = m_SelectEntity;
    ctx.RequestInspectorRefresh = [this]()
    { RequestEntityRefresh(); };
    ctx.GetWorld = [this]() -> ECS::World*
    { return m_World; };
    ctx.Window = m_Window;
    ctx.EditorCtx = m_Context;
    ctx.ShowInfoCards = GetInspectorShowInfoCardsPreference();

    if (typeId == pseudoMaterialSectionId)
    {
        ctx.GetMaterialSlotDisplayOrder = [this, entity](uint32_t matCount) -> std::vector<uint32_t>*
        {
            if (!m_World || !entity.IsValid() || !m_World->IsValid(entity))
                return nullptr;
            auto* mr = m_World->GetComponent<MeshRenderer>(entity);
            if (!mr)
                return nullptr;
            const GUID modelGuid = mr->modelAssetGuid.ToGuid();
            if (modelGuid.IsNull())
                return nullptr;
            MaterialSlotOrderKey key{};
            key.EntityPacked = entity.id;
            key.ModelGuid = modelGuid;
            std::vector<uint32_t>& vec = m_MeshRendererMaterialSlotDisplayOrder[key];
            if (vec.size() != matCount)
            {
                vec.resize(matCount);
                for (uint32_t i = 0; i < matCount; ++i)
                    vec[i] = i;
            }
            return &vec;
        };
        ctx.RegisterMaterialSlotSectionForDrag = [this, entity](InspectorSection* sec, UIElement* container, uint32_t slotIdx)
        {
            if (!m_World || !entity.IsValid() || !m_World->IsValid(entity))
                return;
            auto* mr = m_World->GetComponent<MeshRenderer>(entity);
            if (!mr)
                return;
            const GUID modelGuid = mr->modelAssetGuid.ToGuid();
            WireMaterialSlotSectionDrag(sec, container, slotIdx, modelGuid, entity);
        };
    }

    // A custom inspector owns its own layout, so the per-field preserved notice the reflection
    // path emits above each affected row has nowhere to go; the reflection path emits its own and
    // must not be given a second, component-level copy. Only these two arms need one, which is
    // exactly why it cannot be hoisted above the dispatch.
    if (InspectorFn* fn = InspectorRegistry::Get().TryGetComponentInspector(typeId))
    {
        Editor::AddPreservedComponentFieldsNotice(ctx.Parent, *ctx.World, entity, typeId,
                                                  ctx.RequestInspectorRefresh);
        (*fn)(ctx);
        section->EnsureHasPlaceholderIfEmpty("(No properties)");
    }
    else if (RenderDefaultComponentInspector(ctx, typeId))
    {
        // Reflection-driven fallback: walked the component's GE_REFLECT field
        // table and emitted a widget per field, each with its own notice.
    }
    else
    {
        Editor::AddPreservedComponentFieldsNotice(ctx.Parent, *ctx.World, entity, typeId,
                                                  ctx.RequestInspectorRefresh);
        section->EnsureHasPlaceholderIfEmpty("(No inspector registered)");
    }
}

bool InspectorPanel::RebuildComponentSection(ECS::ComponentTypeId typeId)
{
    if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
        return false;
    const auto it = m_ComponentSectionsByType.find(typeId);
    if (it == m_ComponentSectionsByType.end() || !it->second)
        return false;
    InspectorSection* section = it->second;
    UIElement* body = section->GetContentRoot();
    if (!body)
        return false;

    // The Post Process Volume section hosts nested effect sections (and the
    // Add Post FX button) in its content root; ClearContent would destroy those
    // live sections and leave m_ComponentSectionsByType dangling. Sections
    // that nest other sections rebuild via the full ShowEntity path.
    for (const auto& child : body->GetChildren())
    {
        if (child && dynamic_cast<InspectorSection*>(child.get()))
            return false;
    }

    // Search state holds raw pointers into the widgets being replaced; release the
    // styling while the pointers are still valid, then re-apply the active filter
    // over the rebuilt content below.
    ClearSearchHighlights();

    // Label-drag tracking can hold raw pointers to widgets in this section.
    InspectorDrag::FinalizeAndClearLabelDragState();

    // Drop this section's live-refresh callbacks before destroying the widgets
    // they capture; every other section keeps its callbacks (and widgets).
    m_SectionRefreshCallbacks.erase(typeId);

    section->ClearContent();
    BuildComponentSectionBody(section, typeId);

    // The section header stays alive across the rebuild; re-sync its enabled dot
    // in case the change being reflected also flipped the component's flag.
    bool currentEnabled = true;
    if (Editor::TryGetComponentEnabled(m_World, m_Entity, typeId, currentEnabled))
    {
        section->SetEnabled(currentEnabled);
        if (Editor::IsComponentEnabledMixed(m_World, m_Entities, typeId))
            section->SetEnabledMixed();
        section->SetEnabledToggleTooltip(Editor::ComponentEnabledToggleTooltip(m_World, m_Entity, m_Entities, typeId));
    }

    InspectorDrag::AutoSetupDragForInspector(body);
    if (!m_CurrentSearchText.empty())
        ApplySearchFilter(m_CurrentSearchText);
    return true;
}

void InspectorPanel::CaptureLoadedComponentValues(
    ECS::World* world,
    ECS::EntityHandle entity,
    const std::vector<ECS::ComponentTypeId>& componentTypeIds)
{
    if (!world || !entity.IsValid() || !world->IsValid(entity))
        return;

    const uint64_t worldId = world->GetWorldId();
    for (const ECS::ComponentTypeId typeId : componentTypeIds)
    {
        EntityComponentStateKey key{worldId, entity.id, typeId};
        if (m_LoadedComponentValues.find(key) != m_LoadedComponentValues.end())
            continue;

        std::vector<uint8_t> bytes;
        if (world->CaptureComponentBytes(entity, typeId, bytes))
            m_LoadedComponentValues.emplace(std::move(key), std::move(bytes));
    }
}

bool InspectorPanel::TryGetLoadedComponentValues(
    ECS::World* world,
    ECS::EntityHandle entity,
    ECS::ComponentTypeId componentTypeId,
    std::vector<uint8_t>& outBytes) const
{
    outBytes.clear();
    if (!world || !entity.IsValid())
        return false;
    const EntityComponentStateKey key{world->GetWorldId(), entity.id, componentTypeId};
    const auto it = m_LoadedComponentValues.find(key);
    if (it == m_LoadedComponentValues.end())
        return false;
    outBytes = it->second;
    return true;
}

void InspectorPanel::ShowEntity(World* world, EntityHandle entity, bool force, bool keepMultiSelection)
{
    if (m_Locked && !force)
        return;

    if (UIElement::IsInEventDispatch())
    {
        // Coalesce: store the latest request and queue a single dispatch.
        // A `force=true` request must not be downgraded by a later !force
        // request for the *same* entity. When the entity/world target
        // changes mid-coalesce, reset the force bit — a force flag attached
        // to E1 must not leak to E2 (would bypass m_Locked / same-entity
        // skip on a different target than the caller asked for).
        const bool targetChanged =
            (m_PendingShowWorld != world) || (m_PendingShowEntity != entity);
        m_PendingShowWorld = world;
        m_PendingShowEntity = entity;
        m_PendingShowForce = targetChanged ? force : (m_PendingShowForce || force);
        m_PendingShowSuppressHistory = m_SuppressInspectorHistory;
        // Latest request's intent wins: a later "collapse to single" (keep=false)
        // must not be lost when coalesced with an earlier refresh.
        m_PendingShowKeepMultiSelection = keepMultiSelection;
        if (!m_ShowEntityCoalescePending)
        {
            m_ShowEntityCoalescePending = true;
            this->PostAction([this]()
                             {
                m_ShowEntityCoalescePending = false;
                World* pendingWorld = m_PendingShowWorld;
                EntityHandle pendingEntity = m_PendingShowEntity;
                bool pendingForce = m_PendingShowForce;
                bool pendingSuppress = m_PendingShowSuppressHistory;
                bool pendingKeep = m_PendingShowKeepMultiSelection;
                m_PendingShowWorld = nullptr;
                m_PendingShowEntity = {};
                m_PendingShowForce = false;
                m_PendingShowSuppressHistory = false;
                m_PendingShowKeepMultiSelection = true;

                const bool prev = m_SuppressInspectorHistory;
                m_SuppressInspectorHistory = pendingSuppress;
                this->ShowEntity(pendingWorld, pendingEntity, pendingForce, pendingKeep);
                m_SuppressInspectorHistory = prev;
            });
        }
        return;
    }

    EnsureInspectorTabLockMounted();

    // Re-showing the same single entity is a no-op. But when a multi-selection is
    // currently displayed (m_Entities holds peers), always fall through — a new
    // selection whose anchor happens to be the same entity (e.g. deselecting a peer
    // to collapse to one) must still rebuild and recompute the selection state.
    if (!force && m_World == world && m_Entity == entity && m_Entities.size() <= 1)
    {
        return;
    }

    // ShowEntity assigns the new target before rebuilding its sections. Preserve
    // the old identity now so the outgoing sections are saved against the entity
    // they actually belong to, not the entity that is about to be shown.
    const bool hadPreviousEntity = m_World && m_Entity.IsValid();
    const uint64_t previousWorldId = hadPreviousEntity ? m_World->GetWorldId() : 0;
    const uint32_t previousEntityId = hadPreviousEntity ? m_Entity.id : ECS::kInvalidEntity;

    // Close any open add-component dialog before rebuilding.
    if (m_AddComponentDialog)
    {
        if (UIElement* parent = m_AddComponentDialog->GetParent())
            parent->RemoveChild(m_AddComponentDialog);
        m_AddComponentDialog = nullptr;
        m_AddComponentProvider.reset();
    }

    // Switching to entity mode: any cached asset-selection dedupe should no longer apply.
    m_LastSelectedAssetPaths.clear();

    m_World = world;
    m_Entity = entity;
    // Drop the multi-selection set on a brand-new single selection. A refresh or
    // the ShowEntities() anchor render keeps it (keepMultiSelection) as long as
    // this entity is still that set's anchor; a different anchor is always stale.
    if (!keepMultiSelection || m_Entities.empty() || m_Entities[0] != entity)
        m_Entities.clear();

    // Record to history only for single-entity shows; multi-edit is recorded by ShowEntities().
    if (!m_SuppressInspectorHistory && !m_Locked && m_Entities.size() <= 1)
    {
        InspectorSelectionHistoryEntry entry{};
        if (m_World && entity.IsValid() && m_World->IsValid(entity))
        {
            entry.EntryKind = InspectorSelectionHistoryEntry::Kind::Entity;
            entry.World = world;
            entry.Entity = entity;
        }
        else
        {
            entry.EntryKind = InspectorSelectionHistoryEntry::Kind::Empty;
        }
        RecordInspectorHistory(std::move(entry));
    }

    // Scene entities have no Name until the user renames them. Adding an empty Name here on
    // every single-entity selection used to trigger an archetype move + a full Hierarchy
    // rebuild on the first click (~530ms at 100k entities). The Name is now created lazily on
    // the first keystroke in the header field (applyHeaderNameToWorld, below), which refreshes
    // m_LastComponentSignature so the self-inflicted structural change does not trip the change
    // subscription into a field-resetting inspector rebuild.

    // Track the last component signature so we can rebuild only when the set changes
    // (e.g. component add/remove during undo/redo).
    RefreshTrackedComponentSignature();

    // Subscribe to component changes so the inspector rebuilds after undo/redo
    // or when the component signature changes for the active entity.
    if (m_ChangeNotifications)
    {
        if (m_ComponentSub)
        {
            m_ChangeNotifications->Unsubscribe(m_ComponentSub);
            m_ComponentSub = {};
        }
        if (m_StructureSub)
        {
            m_ChangeNotifications->Unsubscribe(m_StructureSub);
            m_StructureSub = {};
        }

        const ECS::EntityHandle tracked = entity;
        m_ComponentSub = m_ChangeNotifications->SubscribeComponentChanged(
            [this, tracked](const Editor::EditorChangeNotifications::ComponentChangedEvent& e)
            {
                if (!m_World || e.world != m_World)
                    return;
                if (tracked != m_Entity || e.entity != m_Entity)
                    return;
                if (e.kind == Editor::EditorChangeNotifications::ChangeKind::Preview)
                    return;

                if (!m_World->IsValid(m_Entity))
                    return;

                std::vector<ECS::ComponentTypeId> current =
                    CollectVisibleComponentSignature(m_World, m_Entity);
                const bool signatureChanged = (current != m_LastComponentSignature);
                m_LastComponentSignature = std::move(current);

                // Rebuild for undo/redo (field values changed externally) or when the
                // component set changes. Plain Commit from in-inspector edits is skipped:
                // the fields already show the new value, and rebuilding would destroy
                // double-click state on the label that triggered the commit.
                const bool isUndoRedo = (e.kind == Editor::EditorChangeNotifications::ChangeKind::UndoRedo);
                const bool inspectorRebuild =
                    (e.kind == Editor::EditorChangeNotifications::ChangeKind::InspectorRebuild);
                if (!isUndoRedo && !inspectorRebuild && !signatureChanged)
                {
                    // Normal Inspector commits already updated the controls;
                    // refresh only the in-place VCS decorations after dispatch.
                    this->PostAction([this]()
                    {
                        if (m_VcsController)
                            m_VcsController->ApplyDecorations();
                    });
                    return;
                }

                // InspectorRebuild is scoped: scene data is unchanged, only the named
                // component's rows changed shape (e.g. a toggle revealing conditional
                // fields). Rebuild just that section so the rest of the panel keeps its
                // widgets, scroll, and focus. Undo/redo and add/remove still rebuild all.
                if (inspectorRebuild && !isUndoRedo && !signatureChanged)
                {
                    const ECS::ComponentTypeId sectionType = e.componentType;
                    auto rebuildSection = [this, sectionType]()
                    {
                        if (!RebuildComponentSection(sectionType))
                            this->ShowEntity(m_World, m_Entity, /*force=*/true);
                    };
                    if (UIElement::IsInEventDispatch())
                        this->PostAction(rebuildSection);
                    else
                        rebuildSection();
                    return;
                }

                if (UIElement::IsInEventDispatch())
                {
                    this->PostAction([this]()
                                     { this->ShowEntity(m_World, m_Entity, /*force=*/true); });
                }
                else
                {
                    this->ShowEntity(m_World, m_Entity, /*force=*/true);
                }
            });

        m_StructureSub = m_ChangeNotifications->SubscribeWorldStructureChanged(
            [this, tracked](const Editor::EditorChangeNotifications::WorldStructureChangedEvent& e)
            {
                if (!m_World || e.world != m_World)
                    return;
                if (!m_EntityToggle || tracked != m_Entity)
                    return;
                if (!m_World->IsValid(m_Entity))
                    return;

                const bool enabled = m_World->GetComponent<ECS::Disabled>(m_Entity) == nullptr;
                m_UpdatingToggle = true;
                m_EntityToggle->SetChecked(enabled);
                m_UpdatingToggle = false;
            });
    }

    // Snapshot section collapse states before clearing so they survive rebuilds.
    if (hadPreviousEntity)
    {
        for (const auto& [section, typeId] : m_SectionTypeIds)
        {
            const EntityComponentStateKey key{previousWorldId, previousEntityId, typeId};
            m_SectionCollapseStates[key] = section->IsCollapsed();
        }
    }

    ClearContent();
    if (!m_ContentRoot && !m_TopRoot && !m_BottomRoot)
    {
        return;
    }

    if (!m_World || !entity.IsValid() || !m_World->IsValid(entity))
    {
        BuildSimpleTopHeader("Nothing selected", "inspector-header-kind-empty");
        auto label = std::make_unique<Label>();
        label->AddClass("inspector-text");
        label->SetText("Entity is not valid");
        m_ContentRoot->AddChild(std::move(label));
        return;
    }

    // Create header container with entity name and toggle
    auto headerContainer = std::make_unique<UIElement>();
    headerContainer->AddClass("inspector-header-container");

    {
        double iconPrefStored = 20.0;
        Editor::OpenEditorPreferences().TryGetDouble("ui.hierarchyTreeIconSize", iconPrefStored);
        const float treeIconPx = std::clamp(static_cast<float>(iconPrefStored),
                                            kMinEditorTreeIconSizePx, kMaxEditorHierarchyTreeIconSizePx);
        ApplyTreeTitleIconLayoutVars(headerContainer.get(), treeIconPx);

        // Resolve the asset backing the header icon (sprite texture / model).
        // If present, the icon becomes clickable and reveals the asset in the
        // Assets panel; otherwise it's a plain static glyph (CSS-only icons).
        AssetManager* assets = m_Context ? m_Context->Assets : nullptr;
        const std::filesystem::path headerAssetPath =
            Editor::TryResolveInspectorIconAssetPath(m_World, entity, assets);

        std::unique_ptr<UIElement> iconHost;
        if (!headerAssetPath.empty() && m_PingAsset)
        {
            auto pingAsset = m_PingAsset;
            auto path = headerAssetPath;
            iconHost = std::make_unique<InspectorHeaderIconClickable>(
                [pingAsset, path]()
                { pingAsset(path); });
            iconHost->AddClass("clickable");
            iconHost->Overrides().Set(Style::Cursor, CursorStyle::Pointer);
        }
        else
        {
            iconHost = std::make_unique<UIElement>();
        }
        iconHost->AddClass("inspector-hierarchy-entity-icon");
        // Lock icon slot to 32×32 so header row height is constant regardless
        // of whether a sprite icon or model thumbnail is shown.
        constexpr float kInspectorIconSlotPx = 32.0f;
        iconHost->Overrides()
            .Set(Style::Width, StyleLength::Px(kInspectorIconSlotPx))
            .Set(Style::Height, StyleLength::Px(kInspectorIconSlotPx));
        UIElement* iconHostRaw = iconHost.get();
        headerContainer->AddChild(std::move(iconHost));

        Editor::ApplyInspectorEntityIcon(m_World, m_Context, *iconHostRaw, entity, treeIconPx, GetOwnerManager(),
                                         m_Context != nullptr && m_Context->UIReplayActive, this);
    }

    // Entity name: editable field when single-select (always) or multi-select with a
    // Name on the primary entity. The Name component is created lazily on the first edit
    // (applyHeaderNameToWorld), so single-select entities are editable even before one
    // exists. Live typing updates come through SetOnValueChanging — not SetOnValueChanged —
    // because SetOnCommit replaces TextInput's hook and bypasses TextFieldBase::OnTextCommit
    // (see wiring below).
    const bool isMultiEdit = m_Entities.size() > 1;
    std::string headerText = Editor::EntityDisplayName(*m_World, entity);
    auto* nameComponent = m_World->GetComponent<Name>(entity);
    if (isMultiEdit)
    {
        headerText += " (+" + std::to_string(m_Entities.size() - 1) + " more)";
    }

    const bool headerNameEditable = !isMultiEdit || (nameComponent != nullptr);
    if (headerNameEditable)
    {
        auto headerField = std::make_unique<TextField>();
        headerField->AddClass("inspector-header");
        headerField->SetValue(headerText);
        headerField->SetFocusable(true);
        auto* headerFieldRaw = headerField.get();

        const std::string multiEditSuffix =
            isMultiEdit ? (" (+" + std::to_string(m_Entities.size() - 1) + " more)") : std::string{};

        auto sanitizeHeaderNameInput = [isMultiEdit, multiEditSuffix](std::string text) -> std::string
        {
            if (isMultiEdit && !multiEditSuffix.empty() && text.size() >= multiEditSuffix.size())
            {
                if (text.compare(text.size() - multiEditSuffix.size(), multiEditSuffix.size(), multiEditSuffix) ==
                    0)
                    text.resize(text.size() - multiEditSuffix.size());
            }
            return text;
        };

        auto applyHeaderNameToWorld = [this, entity, sanitizeHeaderNameInput](const std::string& rawName)
        {
            if (!m_World || !entity.IsValid() || !m_World->IsValid(entity))
                return;
            const std::string cleaned = sanitizeHeaderNameInput(rawName);
            constexpr size_t kNameCapacity = 63; // Name::value is char[64]
            auto* n = m_World->GetComponentForWrite<Name>(entity);
            if (!n)
            {
                Name nm{};
                std::memset(nm.value, 0, sizeof(nm.value));
                std::strncpy(nm.value, cleaned.c_str(), kNameCapacity);
                nm.value[kNameCapacity] = '\0';
                m_World->AddComponentImmediate(entity, nm);
                // Lazy Name-add (see ShowEntity): fold this archetype move into the tracked
                // signature before NotifyComponentCommit fires below, so our own structural
                // change does not trip the change subscription into rebuilding the inspector
                // and resetting the header field mid-type.
                RefreshTrackedComponentSignature();
            }
            else
            {
                std::strncpy(n->value, cleaned.c_str(), kNameCapacity);
                n->value[kNameCapacity] = '\0';
            }
            if (m_ChangeNotifications)
                m_ChangeNotifications->NotifyComponentCommit<Name>(m_World, entity);
        };

        // TextField routes keystrokes through SetOnValueChanging only; SetOnValueChanged runs from the
        // inner TextInput→OnTextCommit path, which we replace below via SetOnCommit. Sync the ECS on
        // every edit and again on blur/Enter so names persist and saves see the latest string.
        headerField->SetOnValueChanging(
            [applyHeaderNameToWorld](const std::string& newName)
            { applyHeaderNameToWorld(newName); });
        headerField->SetOnValueChanged(
            [applyHeaderNameToWorld](const std::string& newName)
            { applyHeaderNameToWorld(newName); });

        headerFieldRaw->SetOnCommit([this, entity, headerFieldRaw, applyHeaderNameToWorld]()
                                    {
            if (!m_ChangeNotifications || !m_World || !entity.IsValid() || !m_World->IsValid(entity))
                return;
            applyHeaderNameToWorld(headerFieldRaw->GetValue());
            Editor::EditorChangeNotifications::WorldStructureChangedEvent e{};
            e.world = m_World;
            e.kind = Editor::EditorChangeNotifications::ChangeKind::Commit;
            m_ChangeNotifications->NotifyWorldStructureChanged(e); });
        headerContainer->AddChild(std::move(headerField));
    }
    else
    {
        auto header = std::make_unique<Label>();
        header->AddClass("inspector-header");
        header->SetText(headerText);
        headerContainer->AddChild(std::move(header));
    }

    // Selection history arrows, rendered between the name and the toggle.
    MountHistoryButtons(*headerContainer);

    // Toggle control for enable/disable: the entity's own state.
    ECS::Entity entityObj(m_World, entity);
    auto toggle = std::make_unique<Toggle>();
    toggle->AddClass("inspector-entity-toggle");
    m_EntityToggle = toggle.get();
    toggle->SetChecked(entityObj.IsEnabled());

    // The toggle switches every selected entity's own state in one undo step, as the hierarchy
    // row's icon does; their descendants follow through the hierarchy pass.
    toggle->SetOnValueChanged([this, entity](const bool& enabled)
                              {
        if (m_UpdatingToggle)
            return;
        if (!m_World || !entity.IsValid() || !m_World->IsValid(entity)) {
            return;
        }

        std::vector<ECS::EntityHandle> entities = m_Entities;
        if (entities.empty())
            entities.push_back(entity);
        Editor::CommitEntityEnabledToggle(*m_World, m_Undo, m_ChangeNotifications, entities, enabled); });

    headerContainer->AddChild(std::move(toggle));

    // Under the header: why an entity that is switched on does not run.
    auto reasonLine = std::make_unique<Label>();
    reasonLine->AddClass("inspector-entity-activity-reason");
    m_EntityActivity.Bind(m_EntityToggle, reasonLine.get(), isMultiEdit);
    m_PresentedEntityActive = m_EntityActivity.Present(*m_World, entity).ActiveInHierarchy;

    if (UIElement* headerParent = m_TopRoot ? m_TopRoot : m_ContentRoot)
    {
        headerParent->AddChild(std::move(headerContainer));
        headerParent->AddChild(std::move(reasonLine));
    }

    // Generator output says so before its fields are read. It goes in the
    // scrolling content rather than the pinned strip: the notice wraps, and a
    // multi-line banner in `inspector-top` would shorten the section area for
    // every entity, generated or not.
    Editor::AddGeneratedEntityNotice(m_ContentRoot, *m_World, entity, m_SelectEntity);

    SyncInspectorHistoryButtonState();

    // Enumerate the entity's components from its archetype signature and build one
    // inspector section per component.
    ECS::Archetype* archetype = m_World->GetEntityArchetype(entity);
    if (!archetype)
    {
        auto label = std::make_unique<Label>();
        label->AddClass("inspector-text");
        label->SetText("(Entity has no archetype)");
        m_ContentRoot->AddChild(std::move(label));
        return;
    }

    std::vector<ECS::ComponentTypeId> componentIds = archetype->GetSignature().GetComponents();
    if (componentIds.empty())
    {
        auto label = std::make_unique<Label>();
        label->AddClass("inspector-text");
        label->SetText("(Entity has no components)");
        m_ContentRoot->AddChild(std::move(label));
        return;
    }

    // Capture once, before any inspector control can edit the component. Forced
    // refreshes deliberately keep the first snapshot so Reset means "as loaded"
    // rather than "the value immediately before this refresh".
    CaptureLoadedComponentValues(m_World, entity, componentIds);

    // In multi-edit mode, keep only components shared by ALL selected entities.
    if (isMultiEdit)
    {
        std::vector<ECS::ComponentTypeId> shared;
        for (ECS::ComponentTypeId cid : componentIds)
        {
            bool allHave = true;
            for (const auto& ent : m_Entities)
            {
                if (ent == entity)
                    continue;
                if (!ent.IsValid() || !m_World->IsValid(ent))
                {
                    allHave = false;
                    break;
                }
                ECS::Archetype* otherArch = m_World->GetEntityArchetype(ent);
                if (!otherArch || !otherArch->GetSignature().Contains(cid))
                {
                    allHave = false;
                    break;
                }
            }
            if (allHave)
                shared.push_back(cid);
        }
        componentIds = std::move(shared);
    }

    std::stable_sort(componentIds.begin(), componentIds.end(),
                     [](ECS::ComponentTypeId a, ECS::ComponentTypeId b)
                     { return SortKeyForComponent(a) < SortKeyForComponent(b); });

    // Apply persisted drag-reorder for this entity on top of the default sort.
    if (m_ComponentOrderEntity == entity && !m_ComponentOrder.empty())
    {
        std::stable_sort(componentIds.begin(), componentIds.end(),
                         [this](ECS::ComponentTypeId a, ECS::ComponentTypeId b)
                         {
                             const auto& order = m_ComponentOrder;
                             auto rank = [&](ECS::ComponentTypeId id) -> size_t
                             {
                                 for (size_t i = 0; i < order.size(); ++i)
                                     if (order[i] == id)
                                         return i;
                                 return order.size(); // unknown components go to end, preserving relative order
                             };
                             return rank(a) < rank(b);
                         });
    }

    static const ECS::ComponentTypeId meshRendererTypeId = ECS::GetComponentTypeId<MeshRenderer>();
    static const ECS::ComponentTypeId meshMaterialInspectorSectionTypeId =
        ECS::GetComponentTypeId<Editor::MeshRendererMaterialInspectorSection>();

    std::vector<ECS::ComponentTypeId> displayComponentIds;
    displayComponentIds.reserve(componentIds.size() + 1);
    for (ECS::ComponentTypeId cid : componentIds)
    {
        displayComponentIds.push_back(cid);
        if (cid == meshRendererTypeId)
            displayComponentIds.push_back(meshMaterialInspectorSectionTypeId);
    }

    // A hosted section attaches to its host's section, which must exist first.
    Editor::LeadWithSectionHosts(displayComponentIds);

    m_TransformSection = nullptr;

    int shown = 0;
    for (ECS::ComponentTypeId typeId : displayComponentIds)
    {
        const ECS::ComponentTypeId pseudoMaterialSectionId = meshMaterialInspectorSectionTypeId;

        if (ShouldHideComponent(typeId))
        {
            continue;
        }

        const Rendering::PostProcessEffectDescriptor* effectDesc =
            Rendering::PostProcessEffectRegistry::Find(typeId);
        // A section another component hosts (an effect in its volume's stack)
        // renders inside the host's section, so the section a user drags is the
        // stack entry they are reordering. Without the host on the entity it stays
        // top-level with a visible warning, never silently hidden.
        Editor::EditorComponentTraits traits;
        Editor::EditorComponentTraitsRegistry::Get().TryGet(typeId, traits);
        ECS::ComponentTypeId hostTypeId{};
        Editor::EditorComponentTraits hostTraits;
        const bool isHosted =
            Editor::EditorComponentTraitsRegistry::Get().TryGetSectionHost(typeId, hostTypeId, hostTraits);
        InspectorSection* hostSection = nullptr;
        if (isHosted)
        {
            auto hostSectionIt = m_ComponentSectionsByType.find(hostTypeId);
            if (hostSectionIt != m_ComponentSectionsByType.end())
                hostSection = hostSectionIt->second;
        }

        const std::string title = ComponentTitle(typeId);

        auto section = std::make_unique<InspectorSection>(title);
        InspectorSection* sectionRaw = section.get();
        m_SectionTypeIds[sectionRaw] = typeId;
        m_ComponentSectionsByType[typeId] = sectionRaw;
        // The material slots are the renderer's own settings: they read as off with its body.
        if (typeId == pseudoMaterialSectionId)
        {
            const auto rendererSection = m_ComponentSectionsByType.find(meshRendererTypeId);
            if (rendererSection != m_ComponentSectionsByType.end() && rendererSection->second)
                rendererSection->second->SetBodyStateFollower(sectionRaw);
        }
        m_ComponentSectionOrder.push_back(typeId);
        if (m_ComponentSelectionAnchor == ECS::ComponentTypeId{})
            m_ComponentSelectionAnchor = typeId;
        // Effects carry a per-effect icon image in their registration; every other
        // component styles its header icon through a CSS class. This runs before the
        // orphan/missing-package warnings below: those tint the icon, and assigning an
        // image path resets the tint override.
        if (effectDesc && !effectDesc->IconPath.empty())
            sectionRaw->SetHeaderIconImagePath(std::string(effectDesc->IconPath));
        else
            sectionRaw->SetHeaderIconClass(ComponentIconClass(m_World, entity, typeId));
        if (effectDesc && !effectDesc->Description.empty())
            sectionRaw->SetHeaderTooltip(std::string(effectDesc->Description));
        else if (!traits.InspectorTooltip.empty())
            sectionRaw->SetHeaderTooltip(traits.InspectorTooltip);
        else if (const char* tip = ComponentTooltip(typeId))
            sectionRaw->SetHeaderTooltip(tip);
        if (hostSection)
        {
            sectionRaw->AddClass("inspector-hosted-section");
            if (!hostTraits.HostedSectionBodyClass.empty())
                sectionRaw->GetContentRoot()->AddClass(hostTraits.HostedSectionBodyClass);
        }
        else if (isHosted)
        {
            sectionRaw->SetHeaderTooltip(hostTraits.MissingHostWarning);
            sectionRaw->SetHeaderIconTint(0xFFD7A13Bu);
            sectionRaw->SetHeaderBadge(hostTraits.MissingHostBadge, hostTraits.MissingHostWarning);
        }
        const std::string_view requiredPackage = RequiredPackageForComponent(typeId);
        if (!requiredPackage.empty() && m_Context && m_Context->RenderServices &&
            !m_Context->RenderServices->IsPackageAvailable(requiredPackage))
        {
            const std::string packageWarning =
                "Required package '" + std::string(requiredPackage) +
                "' is not installed or enabled. The scene data is preserved, but this effect is inactive.";
            sectionRaw->SetHeaderTooltip(packageWarning);
            sectionRaw->SetHeaderIconTint(0xFFD7A13Bu);
            sectionRaw->SetHeaderBadge("Package Not Installed", packageWarning);
        }
        sectionRaw->SetOnHeaderMouseDown([this, typeId](int mods) -> bool
                                         {
            // Cmd/Ctrl + mouse-down seeds batch selection when starting from empty (mouse-up does not arm).
            if (!Input::IsPrimaryShortcutModifier(mods))
                return false;
            if (!m_SelectedComponentTypes.empty())
                return false;
            ToggleComponentSectionSelection(typeId);
            return true; });
        sectionRaw->SetOnHeaderClick([this, typeId](int mods) -> bool
                                     {
            const bool shift = (mods & Input::kModShift) != 0;
            const bool primaryMod = Input::IsPrimaryShortcutModifier(mods);
            if (shift)
            {
                SelectComponentSectionRange(typeId);
                return true;
            }
            if (primaryMod)
            {
                ToggleComponentSectionSelection(typeId);
                return true;
            }
            SelectSingleComponentSection(typeId);
            return false; });

        static const ECS::ComponentTypeId transformId = ECS::GetComponentTypeId<Transform>();
        if (typeId == transformId)
        {
            m_TransformSection = sectionRaw;
        }

        // The enable dot shows and switches the component's on/off state. A component with none
        // (a NotToggleable type such as Transform) gets a transparent placeholder, so its title
        // still aligns with the titles beside a dot instead of shifting left.
        bool currentEnabled = true;
        if (!Editor::TryGetComponentEnabled(m_World, entity, typeId, currentEnabled))
        {
            sectionRaw->SetEnabledTogglePlaceholder(true);
        }
        else
        {
            sectionRaw->SetEnabled(currentEnabled);
            if (Editor::IsComponentEnabledMixed(m_World, m_Entities, typeId))
                sectionRaw->SetEnabledMixed();
            sectionRaw->SetEnabledToggleTooltip(
                Editor::ComponentEnabledToggleTooltip(m_World, entity, m_Entities, typeId));
            sectionRaw->SetOnEnabledChanged([this, typeId, title, sectionRaw](bool enabled)
                                            {
                if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
                    return;
                Editor::CommitComponentEnabledToggle(*m_World, m_Undo, m_ChangeNotifications, m_Entity,
                                                     m_Entities, typeId, title, enabled);
                // A click that ends a mixed selection does not rebuild the section, so the
                // tooltip's mixed sentence is dropped here.
                sectionRaw->SetEnabledToggleTooltip(
                    Editor::ComponentEnabledToggleTooltip(m_World, m_Entity, m_Entities, typeId));
            });
        }

        sectionRaw->SetInactive(!m_PresentedEntityActive);

        sectionRaw->SetOnHeaderContextMenu([this, typeId, title, pseudoMaterialSectionId](float x, float y)
                                           {
                                               if (!m_Window)
                                                   return;
                                               if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
                                                   return;
                                               EnsureComponentSettingsStorageLoaded();

                                               m_ContextComponentTypeId = typeId;
                                               if (m_SelectedComponentTypes.find(typeId) == m_SelectedComponentTypes.end())
                                                   SelectSingleComponentSection(typeId);

                                               if (!m_ContextMenu)
                                               {
                                                   m_ContextMenu = CreateContextMenu();
                                                   if (m_ContextMenu)
                                                   {
                                                       m_ContextMenu->SetCommandHandler([this](uint32_t cmd)
                                                                                        {
                                                                                            if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
                                                                                                return;

                                                                                            if (cmd == kCmdInspectorCopyComponent)
                                                                                            {
                                                                                                const std::vector<ECS::ComponentTypeId> selection = GetContextCopySelection();
                                                                                                if (selection.size() <= 1)
                                                                                                {
                                                                                                    std::vector<uint8_t> bytes;
                                                                                                    if (!m_World->CaptureComponentBytes(m_Entity, m_ContextComponentTypeId, bytes))
                                                                                                        return;
                                                                                                    const std::string name = ComponentTitle(m_ContextComponentTypeId);
                                                                                                    Platform::SetClipboardText(EncodeComponentClipboard(name, bytes).c_str());
                                                                                                }
                                                                                                else
                                                                                                {
                                                                                                    std::vector<ComponentClipboardEntry> entries;
                                                                                                    entries.reserve(selection.size());
                                                                                                    for (ECS::ComponentTypeId typeId : selection)
                                                                                                    {
                                                                                                        std::vector<uint8_t> bytes;
                                                                                                        if (!m_World->CaptureComponentBytes(m_Entity, typeId, bytes))
                                                                                                            continue;
                                                                                                        entries.emplace_back(ComponentTitle(typeId), std::move(bytes));
                                                                                                    }
                                                                                                    if (!entries.empty())
                                                                                                        Platform::SetClipboardText(EncodeComponentsClipboard(entries).c_str());
                                                                                                }
                                                                                                return;
                                                                                            }

                                                                                            if (cmd == kCmdInspectorPasteComponent)
                                                                                            {
                                                                                                std::string clip = Platform::GetClipboardText();
                                                                                                std::vector<ComponentClipboardEntry> entries;
                                                                                                if (DecodeComponentsClipboard(clip, entries))
                                                                                                {
                                                                                                    std::vector<PasteComponentsCommand::Item> items;
                                                                                                    items.reserve(entries.size());
                                                                                                    for (auto& entry : entries)
                                                                                                    {
                                                                                                        ECS::ComponentTypeId tid{};
                                                                                                        if (!TryFindComponentTypeIdByName(entry.first, tid))
                                                                                                            continue;
                                                                                                        PasteComponentsCommand::Item item{};
                                                                                                        item.TypeId = tid;
                                                                                                        item.NewBytes = std::move(entry.second);
                                                                                                        items.push_back(std::move(item));
                                                                                                    }
                                                                                                    if (items.empty())
                                                                                                        return;
                                                                                                    const std::string undoName = std::string("Paste ") +
                                                                                                        std::to_string(items.size()) + " Components";
                                                                                                    if (m_Undo)
                                                                                                    {
                                                                                                        m_Undo->Execute(std::make_unique<PasteComponentsCommand>(
                                                                                                            undoName,
                                                                                                            m_World,
                                                                                                            m_ChangeNotifications,
                                                                                                            m_Entity,
                                                                                                            std::move(items)));
                                                                                                    }
                                                                                                    else
                                                                                                    {
                                                                                                        PasteComponentsCommand cmdObj(
                                                                                                            undoName,
                                                                                                            m_World,
                                                                                                            m_ChangeNotifications,
                                                                                                            m_Entity,
                                                                                                            std::move(items));
                                                                                                        cmdObj.Redo();
                                                                                                    }
                                                                                                }
                                                                                                else
                                                                                                {
                                                                                                    std::string typeName;
                                                                                                    std::vector<uint8_t> bytes;
                                                                                                    if (!DecodeComponentClipboard(clip, typeName, bytes))
                                                                                                        return;
                                                                                                    ECS::ComponentTypeId tid{};
                                                                                                    if (!TryFindComponentTypeIdByName(typeName, tid))
                                                                                                        return;

                                                                                                    std::vector<uint8_t> dummy;
                                                                                                    const bool entityHasIt =
                                                                                                        m_World->CaptureComponentBytes(m_Entity, tid, dummy);
                                                                                                    const std::string undoName = entityHasIt
                                                                                                        ? std::string("Paste ") + typeName + " Values"
                                                                                                        : std::string("Paste ") + typeName + " as New Component";

                                                                                                    if (m_Undo)
                                                                                                    {
                                                                                                        m_Undo->Execute(std::make_unique<PasteComponentCommand>(
                                                                                                            undoName,
                                                                                                            m_World,
                                                                                                            m_ChangeNotifications,
                                                                                                            m_Entity,
                                                                                                            tid,
                                                                                                            std::move(bytes)));
                                                                                                    }
                                                                                                    else
                                                                                                    {
                                                                                                        PasteComponentCommand cmdObj(
                                                                                                            undoName,
                                                                                                            m_World,
                                                                                                            m_ChangeNotifications,
                                                                                                            m_Entity,
                                                                                                            tid,
                                                                                                            std::move(bytes));
                                                                                                        cmdObj.Redo();
                                                                                                    }
                                                                                                }
                                                                                                this->ShowEntity(m_World, m_Entity, /*force=*/true);
                                                                                                return;
                                                                                            }

                                                                                            if (cmd == kCmdInspectorSaveComponentSettings)
                                                                                            {
                                                                                                std::vector<uint8_t> bytes;
                                                                                                if (!m_World->CaptureComponentBytes(m_Entity, m_ContextComponentTypeId, bytes))
                                                                                                    return;

                                                                                                auto& settingsList = m_SavedComponentSettingsByType[m_ContextComponentTypeId];
                                                                                                SavedComponentSettings saved{};
                                                                                                saved.Name = "Setting " + std::to_string(settingsList.size() + 1);
                                                                                                saved.Bytes = std::move(bytes);
                                                                                                settingsList.push_back(std::move(saved));

                                                                                                if (settingsList.size() > kMaxSavedComponentSettingsPerType)
                                                                                                {
                                                                                                    const size_t trimCount =
                                                                                                        settingsList.size() - kMaxSavedComponentSettingsPerType;
                                                                                                    settingsList.erase(settingsList.begin(),
                                                                                                                      settingsList.begin() + trimCount);
                                                                                                }
                                                                                                SaveComponentSettingsToProject();
                                                                                                return;
                                                                                            }

                                                                                            if (cmd == kCmdInspectorRestoreLastSettings)
                                                                                            {
                                                                                                const auto it = m_SavedComponentSettingsByType.find(m_ContextComponentTypeId);
                                                                                                if (it == m_SavedComponentSettingsByType.end() || it->second.empty())
                                                                                                    return;

                                                                                                const SavedComponentSettings& saved = it->second.back();
                                                                                                std::vector<uint8_t> restoreBytes = saved.Bytes;
                                                                                                const std::string undoName =
                                                                                                    "Restore " + ComponentTitle(m_ContextComponentTypeId) + " " + saved.Name;
                                                                                                if (m_Undo)
                                                                                                {
                                                                                                    m_Undo->Execute(std::make_unique<PasteComponentCommand>(
                                                                                                        undoName,
                                                                                                        m_World,
                                                                                                        m_ChangeNotifications,
                                                                                                        m_Entity,
                                                                                                        m_ContextComponentTypeId,
                                                                                                        std::move(restoreBytes)));
                                                                                                }
                                                                                                else
                                                                                                {
                                                                                                    PasteComponentCommand cmdObj(
                                                                                                        undoName,
                                                                                                        m_World,
                                                                                                        m_ChangeNotifications,
                                                                                                        m_Entity,
                                                                                                        m_ContextComponentTypeId,
                                                                                                        std::move(restoreBytes));
                                                                                                    cmdObj.Redo();
                                                                                                }
                                                                                                this->ShowEntity(m_World, m_Entity, /*force=*/true);
                                                                                                return;
                                                                                            }

                                                                                            if (cmd == kCmdInspectorResetComponentValues)
                                                                                            {
                                                                                                std::vector<uint8_t> resetBytes;
                                                                                                if (!TryGetLoadedComponentValues(
                                                                                                        m_World,
                                                                                                        m_Entity,
                                                                                                        m_ContextComponentTypeId,
                                                                                                        resetBytes) &&
                                                                                                    !ECS::ComponentFactory::GetDefaultBytes(
                                                                                                        m_ContextComponentTypeId,
                                                                                                        resetBytes))
                                                                                                    return;

                                                                                                const std::string undoName =
                                                                                                    "Reset " + ComponentTitle(m_ContextComponentTypeId) + " values";
                                                                                                if (m_Undo)
                                                                                                {
                                                                                                    m_Undo->Execute(std::make_unique<PasteComponentCommand>(
                                                                                                        undoName,
                                                                                                        m_World,
                                                                                                        m_ChangeNotifications,
                                                                                                        m_Entity,
                                                                                                        m_ContextComponentTypeId,
                                                                                                        std::move(resetBytes)));
                                                                                                }
                                                                                                else
                                                                                                {
                                                                                                    PasteComponentCommand cmdObj(
                                                                                                        undoName,
                                                                                                        m_World,
                                                                                                        m_ChangeNotifications,
                                                                                                        m_Entity,
                                                                                                        m_ContextComponentTypeId,
                                                                                                        std::move(resetBytes));
                                                                                                    cmdObj.Redo();
                                                                                                }
                                                                                                this->ShowEntity(m_World, m_Entity, /*force=*/true);
                                                                                                return;
                                                                                            }

                                                                                            if (cmd >= kCmdInspectorRestoreSavedSettingsBase &&
                                                                                                cmd < (kCmdInspectorRestoreSavedSettingsBase + kCmdInspectorRestoreSavedSettingsMax))
                                                                                            {
                                                                                                const uint32_t menuOffset = cmd - kCmdInspectorRestoreSavedSettingsBase;
                                                                                                if (menuOffset >= m_ContextSavedSettingsMenuIndices.size())
                                                                                                    return;

                                                                                                const auto it = m_SavedComponentSettingsByType.find(m_ContextComponentTypeId);
                                                                                                if (it == m_SavedComponentSettingsByType.end())
                                                                                                    return;

                                                                                                const size_t slotIndex = m_ContextSavedSettingsMenuIndices[menuOffset];
                                                                                                if (slotIndex >= it->second.size())
                                                                                                    return;

                                                                                                const SavedComponentSettings& saved = it->second[slotIndex];
                                                                                                std::vector<uint8_t> restoreBytes = saved.Bytes;
                                                                                                const std::string undoName =
                                                                                                    "Restore " + ComponentTitle(m_ContextComponentTypeId) + " " + saved.Name;
                                                                                                if (m_Undo)
                                                                                                {
                                                                                                    m_Undo->Execute(std::make_unique<PasteComponentCommand>(
                                                                                                        undoName,
                                                                                                        m_World,
                                                                                                        m_ChangeNotifications,
                                                                                                        m_Entity,
                                                                                                        m_ContextComponentTypeId,
                                                                                                        std::move(restoreBytes)));
                                                                                                }
                                                                                                else
                                                                                                {
                                                                                                    PasteComponentCommand cmdObj(
                                                                                                        undoName,
                                                                                                        m_World,
                                                                                                        m_ChangeNotifications,
                                                                                                        m_Entity,
                                                                                                        m_ContextComponentTypeId,
                                                                                                        std::move(restoreBytes));
                                                                                                    cmdObj.Redo();
                                                                                                }
                                                                                                this->ShowEntity(m_World, m_Entity, /*force=*/true);
                                                                                                return;
                                                                                            }

                                                                                            if (cmd == kCmdInspectorClearMeshRendererMaterial)
                                                                                            {
                                                                                                static const ECS::ComponentTypeId kMaterialSectionId =
                                                                                                    ECS::GetComponentTypeId<Editor::MeshRendererMaterialInspectorSection>();
                                                                                                if (m_ContextComponentTypeId != kMaterialSectionId)
                                                                                                    return;

                                                                                                std::vector<ECS::EntityHandle> extras;
                                                                                                for (const auto& e : m_Entities)
                                                                                                {
                                                                                                    if (e != m_Entity && e.IsValid() && m_World->IsValid(e))
                                                                                                        extras.push_back(e);
                                                                                                }

                                                                                                std::optional<Editor::UndoRedoService::InteractiveEdit> clearMatEdit;
                                                                                                if (m_Undo)
                                                                                                {
                                                                                                    auto target = InspectorDrag::MakeMultiComponentSnapshotTarget<MeshRenderer>(
                                                                                                        m_World, m_Entity, extras, m_ChangeNotifications, "Remove Material");
                                                                                                    clearMatEdit.emplace(m_Undo->BeginInteractiveEdit("Remove Material", std::move(target)));
                                                                                                }

                                                                                                auto clearMaterial = [&](ECS::EntityHandle ent) {
                                                                                                    auto* mr = m_World->GetComponent<MeshRenderer>(ent);
                                                                                                    if (!mr)
                                                                                                        return;
                                                                                                    MeshRenderer updated = *mr;
                                                                                                    updated.materialAssetGuid.Clear();
                                                                                                    Editor::CommitComponentUpdate(m_World, ent, m_ChangeNotifications, updated);
                                                                                                };

                                                                                                clearMaterial(m_Entity);
                                                                                                for (auto& ex : extras)
                                                                                                    clearMaterial(ex);

                                                                                                if (clearMatEdit)
                                                                                                    clearMatEdit->Commit();

                                                                                                this->ShowEntity(m_World, m_Entity, /*force=*/true);
                                                                                                return;
                                                                                            }

                                                                                            if (cmd == kCmdInspectorRemoveComponent)
                                                                                            {
                                                                                                if (!IsRemovableComponent(m_ContextComponentTypeId))
                                                                                                    return;

                                                                                                const ECS::ComponentTypeId postProcessVolumeId =
                                                                                                    ECS::GetComponentTypeId<Components::PostProcessVolume>();
                                                                                                const bool removingPostProcessVolume =
                                                                                                    (m_ContextComponentTypeId == postProcessVolumeId);
                                                                                                bool hasPostProcessEffects = false;
                                                                                                if (m_World)
                                                                                                {
                                                                                                    Rendering::PostProcessEffectRegistry::ForEach(
                                                                                                        [&](const Rendering::PostProcessEffectDescriptor& d)
                                                                                                        {
                                                                                                            if (!hasPostProcessEffects && m_World->HasComponent(m_Entity, d.Type))
                                                                                                                hasPostProcessEffects = true;
                                                                                                        });
                                                                                                }
                                                                                                if (removingPostProcessVolume && hasPostProcessEffects)
                                                                                                {
                                                                                                    m_PendingRemoveComponentTypeId = m_ContextComponentTypeId;
                                                                                                    EnsurePostProcessVolumeRemoveModal();
                                                                                                    if (m_RemovePostProcessConfirmModal)
                                                                                                    {
                                                                                                        m_RemovePostProcessConfirmModal->Show(
                                                                                                            "Remove Post Process Volume",
                                                                                                            "This entity still has volume effect components.\n"
                                                                                                            "Removing the volume also removes those effect components.\n\n"
                                                                                                            "Remove Post Process Volume anyway?",
                                                                                                            "Remove");
                                                                                                    }
                                                                                                    return;
                                                                                                }

                                                                                                ExecuteRemoveComponent(m_ContextComponentTypeId);
                                                                                                return;
                                                                                            }

                                                                                            ComponentPreset preset{};
                                                                                            if (TryGetAddPreset(cmd, preset))
                                                                                            {
                                                                                                const std::string sel = PresetUndoDisplayName(preset);
                                                                                                if (m_Undo)
                                                                                                {
                                                                                                    m_Undo->Execute(std::make_unique<AddPresetComponentsCommand>(
                                                                                                        std::string("Add ") + sel,
                                                                                                        m_World,
                                                                                                        m_ChangeNotifications,
                                                                                                        m_Entity,
                                                                                                        preset));
                                                                                                }
                                                                                                else
                                                                                                {
                                                                                                    AddPresetComponentsCommand addCmd(
                                                                                                        std::string("Add ") + sel,
                                                                                                        m_World,
                                                                                                        m_ChangeNotifications,
                                                                                                        m_Entity,
                                                                                                        preset);
                                                                                                    addCmd.Redo();
                                                                                                }
                                                                                                if (m_ComponentOrderEntity != m_Entity)
                                                                                                {
                                                                                                    m_ComponentOrderEntity = m_Entity;
                                                                                                    m_ComponentOrder.clear();
                                                                                                }
                                                                                                if (m_ComponentOrder.empty() && m_ContentRoot)
                                                                                                {
                                                                                                    const ECS::ComponentTypeId pseudoMaterialId =
                                                                                                        ECS::GetComponentTypeId<Editor::MeshRendererMaterialInspectorSection>();
                                                                                                    for (const auto& child : m_ContentRoot->GetChildren())
                                                                                                    {
                                                                                                        if (auto* sec = dynamic_cast<InspectorSection*>(child.get()))
                                                                                                        {
                                                                                                            auto it = m_SectionTypeIds.find(sec);
                                                                                                            if (it != m_SectionTypeIds.end() && it->second != pseudoMaterialId)
                                                                                                                m_ComponentOrder.push_back(it->second);
                                                                                                        }
                                                                                                    }
                                                                                                }
                                                                                                for (ECS::ComponentTypeId addedId : ComponentTypeIdsForPreset(preset))
                                                                                                {
                                                                                                    std::vector<uint8_t> dummy;
                                                                                                    if (!m_World->CaptureComponentBytes(m_Entity, addedId, dummy))
                                                                                                        continue;
                                                                                                    if (std::find(m_ComponentOrder.begin(), m_ComponentOrder.end(), addedId) == m_ComponentOrder.end())
                                                                                                        m_ComponentOrder.push_back(addedId);
                                                                                                }
                                                                                                this->ShowEntity(m_World, m_Entity, /*force=*/true);
                                                                                            }
                                                                                        });

                                                       m_ContextMenu->SetStateProvider([this](uint32_t cmdId) -> MenuItemState
                                                                                       {
                                                                                           MenuItemState st{};
                                                                                           if (cmdId == kCmdInspectorRemoveComponent)
                                                                                               st.Enabled = IsRemovableComponent(m_ContextComponentTypeId);
                                                                                           return st;
                                                                                       });
                                                   }
                                               }

                                               if (!m_ContextMenu)
                                                   return;

                                               m_ContextMenu->Clear();
                                               ContextMenuBuilder builder;
                                               m_ContextSavedSettingsMenuIndices.clear();

                                               // Component settings snapshots + copy/paste (skip the pseudo material section, which isn't a real component).
                                               if (typeId != pseudoMaterialSectionId)
                                               {
                                                   const std::vector<ECS::ComponentTypeId> selection = GetContextCopySelection();
                                                   std::string clipTypeName;
                                                   std::vector<uint8_t> clipBytes;
                                                   ECS::ComponentTypeId clipTypeId{};
                                                   bool clipValid = false;
                                                   std::vector<ComponentClipboardEntry> clipEntries;
                                                   bool multiClipValid = false;
                                                   bool entityHasClipType = false;
                                                   // A peek-only read (this menu's Paste label and enabled-state, not a
                                                   // real paste) risks a permission prompt on web for every right-click.
                                                   // Skip it there: the label falls back to the generic "Paste
                                                   // Component" below, the item stays enabled, and
                                                   // kCmdInspectorPasteComponent's own read still validates for real.
                                                   const bool canPasteComponent = Platform::ClipboardReadCanPromptUser()
                                                       ? true
                                                       : [&] {
                                                             const std::string clip = Platform::GetClipboardText();
                                                             clipValid = DecodeComponentClipboard(clip, clipTypeName, clipBytes) &&
                                                                        TryFindComponentTypeIdByName(clipTypeName, clipTypeId);
                                                             multiClipValid = DecodeComponentsClipboard(clip, clipEntries);
                                                             if (clipValid)
                                                             {
                                                                 std::vector<uint8_t> dummy;
                                                                 entityHasClipType =
                                                                     m_World->CaptureComponentBytes(m_Entity, clipTypeId, dummy);
                                                             }
                                                             return clipValid || multiClipValid;
                                                         }();

                                                   builder.AddItem(BuildInspectorCopyMenuLabel(selection),
                                                                   kCmdInspectorCopyComponent,
                                                                   MenuItemFlag_None,
                                                                   -3,
                                                                   EditorIcons::kCopy);
                                                   const std::string pasteLabel = BuildInspectorPasteMenuLabel(multiClipValid,
                                                                                                               clipEntries.size(),
                                                                                                               clipValid,
                                                                                                               clipTypeName,
                                                                                                               entityHasClipType,
                                                                                                               clipTypeId);
                                                   builder.AddItem(pasteLabel,
                                                                   kCmdInspectorPasteComponent,
                                                                   canPasteComponent ? MenuItemFlag_None : MenuItemFlag_Disabled,
                                                                   -2,
                                                                   EditorIcons::kPaste);
                                               }

                                               if (typeId == pseudoMaterialSectionId)
                                                   builder.AddItem("Remove Material", kCmdInspectorClearMeshRendererMaterial, MenuItemFlag_None, 0, EditorIcons::kTrash);
                                               else
                                                   builder.AddItem(std::string("Remove ") + title + " Component", kCmdInspectorRemoveComponent, MenuItemFlag_None, 0, EditorIcons::kTrash);

                                               if (typeId != pseudoMaterialSectionId)
                                               {
                                                   std::vector<uint8_t> resetDefaults;
                                                   const bool canReset =
                                                       TryGetLoadedComponentValues(
                                                           m_World, m_Entity, typeId, resetDefaults) ||
                                                       ECS::ComponentFactory::GetDefaultBytes(typeId, resetDefaults);
                                                   builder.AddItem(std::string("Reset ") + title + " values",
                                                                   kCmdInspectorResetComponentValues,
                                                                   canReset ? MenuItemFlag_None : MenuItemFlag_Disabled,
                                                                   1,
                                                                   EditorIcons::kReset);
                                               }

                                               if (typeId != pseudoMaterialSectionId)
                                               {
                                                   int addPresetPriority = 10;
                                                   builder.AddItem("Add", 0, MenuItemFlag_None, 9,
                                                                   EditorIcons::kPlus);
                                                   std::set<std::string> presetCategories;
                                                   for (const auto& presetEntry : ComponentPresetCatalog())
                                                   {
                                                       if (presetCategories.insert(presetEntry.Category).second)
                                                           builder.AddItem("Add/" + presetEntry.Category, 0,
                                                                           MenuItemFlag_None, addPresetPriority,
                                                                           CategoryIcon(presetEntry.Category));
                                                       const uint32_t presetCmd =
                                                           kCmdInspectorAddPresetBase +
                                                           static_cast<uint32_t>(presetEntry.Preset);
                                                       builder.AddItem(std::string("Add/") + presetEntry.Category + "/" + presetEntry.Label,
                                                                       presetCmd,
                                                                       MenuItemFlag_None,
                                                                       addPresetPriority++,
                                                                       presetEntry.Icon);
                                                   }
                                               }

                                               if (typeId != pseudoMaterialSectionId)
                                               {
                                                   const auto savedIt = m_SavedComponentSettingsByType.find(typeId);
                                                   const bool hasSavedSettings =
                                                       (savedIt != m_SavedComponentSettingsByType.end() &&
                                                        !savedIt->second.empty());
                                                   builder.AddItem("Settings", 0, MenuItemFlag_None, 19,
                                                                   EditorIcons::kSettings);
                                                   builder.AddItem("Settings/Save Current Settings",
                                                                   kCmdInspectorSaveComponentSettings,
                                                                   MenuItemFlag_None,
                                                                   100,
                                                                   EditorIcons::kSave);
                                                   builder.AddItem("Settings/Restore Last Saved",
                                                                   kCmdInspectorRestoreLastSettings,
                                                                   hasSavedSettings ? MenuItemFlag_None : MenuItemFlag_Disabled,
                                                                   101,
                                                                   EditorIcons::kReset);
                                                   if (hasSavedSettings)
                                                   {
                                                       const auto& settingsList = savedIt->second;
                                                       const size_t startIndex =
                                                           settingsList.size() > kMaxSavedComponentSettingsMenuItems
                                                               ? settingsList.size() - kMaxSavedComponentSettingsMenuItems
                                                               : 0;
                                                       int savedPriority = 102;
                                                       for (size_t i = startIndex; i < settingsList.size(); ++i)
                                                       {
                                                           if (m_ContextSavedSettingsMenuIndices.size() >= kCmdInspectorRestoreSavedSettingsMax)
                                                               break;
                                                           const uint32_t cmdId =
                                                               kCmdInspectorRestoreSavedSettingsBase +
                                                               static_cast<uint32_t>(m_ContextSavedSettingsMenuIndices.size());
                                                           m_ContextSavedSettingsMenuIndices.push_back(i);
                                                           builder.AddItem(std::string("Settings/Saved/") + settingsList[i].Name,
                                                                           cmdId,
                                                                           MenuItemFlag_None,
                                                                           savedPriority++,
                                                                           EditorIcons::kSettings);
                                                       }
                                                   }
                                               }

                                               builder.Build(m_ContextMenu.get());
                                               m_ContextMenu->Show(m_Window, (int)x, (int)y); });
        if (m_SoloSectionsEnabled)
        {
            sectionRaw->SetOnCollapsedChanged([this](InspectorSection& self, bool collapsed)
                                              {
                                                  if (collapsed || !m_SoloSectionsEnabled || !m_ContentRoot)
                                                      return;

                                                  for (const auto& child : m_ContentRoot->GetChildren())
                                                  {
                                                      auto* other = dynamic_cast<InspectorSection*>(child.get());
                                                      if (other && other != &self && !other->IsCollapsed())
                                                      {
                                                          other->SetCollapsed(true);
                                                      }
                                                  } });
        }

        if (typeId != pseudoMaterialSectionId)
        {
            // Hosted sections drag-reorder among themselves inside the host's
            // content root; everything else reorders at the top level. Both
            // persist the combined visual order (and the StackOrder sync) the
            // same way.
            UIElement* reorderRoot = hostSection ? hostSection->GetContentRoot() : m_ContentRoot;
            BindSectionHeaderDragReorder(
                sectionRaw,
                reorderRoot,
                /*restrictToMaterialSlotSections=*/false,
                [this](UIElement*)
                { PersistComponentOrderFromSections(); });
        }

        if (hostSection)
            hostSection->GetContentRoot()->AddChild(std::move(section));
        else
            m_ContentRoot->AddChild(std::move(section));

        BuildComponentSectionBody(sectionRaw, typeId);

        // Restore the saved collapse state (else the per-component default) only
        // once the body exists: the header chevron reflects whether the section
        // has content, so restoring earlier would read an empty body.
        const EntityComponentStateKey collapseKey{m_World->GetWorldId(), entity.id, typeId};
        auto savedIt = m_SectionCollapseStates.find(collapseKey);
        if (savedIt != m_SectionCollapseStates.end())
            sectionRaw->SetCollapsed(savedIt->second);
        else if (ShouldCollapseByDefault(typeId))
            sectionRaw->SetCollapsed(true);

        ++shown;
    }

    // Preserved (unloaded) components: scene components whose native module wasn't
    // registered at load time. They live in the World's UnresolvedComponentStore (off
    // archetype storage) keyed by entity, so they only make sense in single-entity edit.
    // Rendered AFTER the known components as read/editable string rows; once the module
    // loads and the editor's re-apply pass instantiates the real type, the entry leaves
    // the store and the component renders normally on the next inspector rebuild
    // (ShowEntity runs on selection/refresh — no extra load-event hook needed here).
    if (!isMultiEdit)
    {
        const char* const kUnloadedTooltip =
            "No loaded code registers this component, so it does nothing. Its values are kept in "
            "the scene and applied when a module that registers it loads; remove it if none does.";
        if (const ECS::UnresolvedComponentStore* store = m_World->TryGetUnresolvedComponents())
        {
            const auto& table = store->Map();
            auto entry = table.find(entity);
            if (entry != table.end())
            {
                for (const ECS::PreservedComponent& preserved : entry->second)
                {
                    auto section = std::make_unique<InspectorSection>(preserved.Name);
                    InspectorSection* sectionRaw = section.get();
                    sectionRaw->SetHeaderTooltip(kUnloadedTooltip);
                    sectionRaw->SetEnabledTogglePlaceholder(true);  // no enabled-dot for unresolved types
                    // Warning icon + right-aligned tag so an unloaded component is scannable
                    // even when the section is collapsed.
                    sectionRaw->SetHeaderIconClass("inspector-section-icon-unloaded");
                    sectionRaw->SetHeaderIconTint(0xFFD7A13Bu);  // amber
                    sectionRaw->SetHeaderBadge("Not Registered", kUnloadedTooltip);
                    m_ContentRoot->AddChild(std::move(section));

                    UIElement* body = sectionRaw->GetContentRoot();

                    // Editable string rows for each preserved property. The write goes
                    // straight back into the store so the edit survives the save round-trip
                    // and is re-applied when the module loads.
                    if (preserved.Props.empty())
                    {
                        InspectorUI::AddLine(body, "(no preserved values)");
                    }
                    for (const auto& prop : preserved.Props)
                    {
                        UIElement* row = InspectorUI::AddRow(body);
                        if (!row)
                            continue;
                        InspectorUI::AddLabel(row, IdentifierToWords(prop.first), prop.first.c_str());
                        UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
                        auto text = std::make_unique<TextField>();
                        text->SetValue(prop.second);
                        text->AddClass("inspector-text-field");
                        TextField* tf = text.get();
                        fieldContainer->AddChild(std::move(text));

                        const std::string compName = preserved.Name;
                        const std::string propName = prop.first;
                        tf->SetOnCommit([this, tf, entity, compName, propName]() {
                            if (!m_World)
                                return;
                            m_World->GetUnresolvedComponents().SetProp(entity, compName, propName, tf->GetValue());
                        });
                    }
                    ++shown;
                }
            }
        }
    }

    // Enable drag-on-label for all FloatField/IntField rows in component sections
    InspectorDrag::AutoSetupDragForInspector(m_ContentRoot);

    if (m_SoloSectionsEnabled)
    {
        WireSoloHandlersForSections();
    }

    // Add Component button — opens a SearchDialog picker.
    if (m_BottomRoot)
    {
        auto addBtn = std::make_unique<Button>();
        addBtn->SetText("Add Component");
        addBtn->SetTooltip("Add a component to the selected entity.");
        addBtn->AddClass("inspector-add-component-button");
        addBtn->Overrides()
            .Set(Style::Width, StyleLength::Px(kInspectorPickerWidthPx))
            .Set(Style::AlignSelf, AlignItems::Center);

        addBtn->RegisterEventHandler(kEventButtonClick, [this, entity](UIEvent& e)
                           {
                               UIElement& btn = *e.CurrentTarget;
                               if (m_AddComponentDialog)
                                   return;

                               if (!m_World || !entity.IsValid() || !m_World->IsValid(entity))
                                   return;

                               UIManager* mgr = btn.GetOwnerManager();
                               if (!mgr)
                                   return;
                               UIElement* root = mgr->GetRootElement();
                               if (!root)
                                   return;

                               m_AddComponentProvider = std::make_shared<ComponentPresetSearchProvider>();

                               auto dialog = std::make_unique<SearchDialog>();
                               dialog->SetProvider(m_AddComponentProvider.get());
                               dialog->SetFilterOptions(m_AddComponentProvider->GetFilterOptions());
                               dialog->SetFixedHeight(true);

                               SearchDialog* dialogPtr = dialog.get();
                               m_AddComponentDialog = dialogPtr;

                               dialog->SetOnResult([this, entity, dialogPtr, root](const SearchResultItem& item) {
                                   // The picked item is either a curated multi-component preset or a
                                   // single reflection-discoverable component (chosen by type id).
                                   std::optional<ComponentPreset> preset;
                                   std::optional<ECS::ComponentTypeId> singleTypeId;
                                   if (const auto* p = std::any_cast<ComponentPreset>(&item.UserData))
                                       preset = *p;
                                   else if (const auto* s = std::any_cast<AddSingleComponentChoice>(&item.UserData))
                                       singleTypeId = s->TypeId;
                                   else
                                       return;

                                   const std::string label = item.Label;

                                   PostSafeAction([this, entity, dialogPtr, root, preset, singleTypeId, label]() {
                                       const auto cleanup = [&]() {
                                           if (m_AddComponentDialog == dialogPtr)
                                               m_AddComponentDialog = nullptr;
                                           m_AddComponentProvider.reset();
                                           if (root)
                                               root->RemoveChild(dialogPtr);
                                       };

                                       if (!m_World || !entity.IsValid() || !m_World->IsValid(entity))
                                       {
                                           cleanup();
                                           return;
                                       }

                                       std::string name = std::string("Add ") + label;

                                       std::vector<ECS::ComponentTypeId> addedIds;
                                       if (preset)
                                       {
                                           if (m_Undo)
                                               m_Undo->Execute(std::make_unique<AddPresetComponentsCommand>(
                                                   std::move(name), m_World, m_ChangeNotifications, entity, *preset));
                                           else
                                           {
                                               AddPresetComponentsCommand cmd(
                                                   std::move(name), m_World, m_ChangeNotifications, entity, *preset);
                                               cmd.Redo();
                                           }
                                           addedIds = ComponentTypeIdsForPreset(*preset);
                                       }
                                       else if (singleTypeId)
                                       {
                                           if (m_Undo)
                                               m_Undo->Execute(std::make_unique<AddSingleComponentCommand>(
                                                   std::move(name), m_World, m_ChangeNotifications, entity, *singleTypeId));
                                           else
                                           {
                                               AddSingleComponentCommand cmd(
                                                   std::move(name), m_World, m_ChangeNotifications, entity, *singleTypeId);
                                               cmd.Do();
                                           }
                                           addedIds = {*singleTypeId};
                                       }

                                       if (m_ComponentOrderEntity != entity)
                                       {
                                           m_ComponentOrderEntity = entity;
                                           m_ComponentOrder.clear();
                                       }
                                       if (m_ComponentOrder.empty() && m_ContentRoot)
                                       {
                                           const ECS::ComponentTypeId pseudoMaterialId =
                                               ECS::GetComponentTypeId<Editor::MeshRendererMaterialInspectorSection>();
                                           for (const auto& child : m_ContentRoot->GetChildren())
                                           {
                                               if (auto* sec = dynamic_cast<InspectorSection*>(child.get()))
                                               {
                                                   auto it = m_SectionTypeIds.find(sec);
                                                   if (it != m_SectionTypeIds.end() && it->second != pseudoMaterialId)
                                                       m_ComponentOrder.push_back(it->second);
                                               }
                                           }
                                       }
                                       for (ECS::ComponentTypeId addedId : addedIds)
                                       {
                                           std::vector<uint8_t> dummy;
                                           if (!m_World->CaptureComponentBytes(entity, addedId, dummy))
                                               continue;
                                           if (std::find(m_ComponentOrder.begin(), m_ComponentOrder.end(), addedId) == m_ComponentOrder.end())
                                               m_ComponentOrder.push_back(addedId);
                                       }

                                       cleanup();
                                       ShowEntity(m_World, entity, /*force=*/true);
                                   });
                               });

                               dialog->SetOnCancel([this, dialogPtr, root]() {
                                   m_AddComponentDialog = nullptr;
                                   m_AddComponentProvider.reset();
                                   root->RemoveChild(dialogPtr);
                               });

                               // Drops below the button, horizontally centred on it; SearchDialog
                               // flips it above only when the panel does not fit below — which is
                               // the usual case here, the button sitting in the panel's bottom bar.
                               const float btnCenterX = btn.GetLayoutX() + btn.GetLayoutWidth() * 0.5f;
                               const float anchorX = btnCenterX - kInspectorPickerWidthPx * 0.5f;
                               const float anchorY = btn.GetLayoutY() + btn.GetLayoutHeight();
                               root->AddChild(std::move(dialog));
                               dialogPtr->SetAnchorPosition(anchorX, anchorY, SearchDialogHorizontalAnchor::LeadingLeft,
                                                            btn.GetLayoutHeight());
                               dialogPtr->Show(); });

        m_BottomRoot->AddChild(std::move(addBtn));
    }

    if (shown == 0)
    {
        m_SelectedComponentTypes.clear();
        m_ComponentSelectionAnchor = {};
        auto label = std::make_unique<Label>();
        label->AddClass("inspector-text");
        label->SetText("(No visible components)");
        m_ContentRoot->AddChild(std::move(label));
    }
    else
    {
        RefreshComponentSectionSelectionVisuals();
    }

    if (m_VcsController)
        m_VcsController->ApplyDecorations();
}

void InspectorPanel::SetSceneDiffProvider(
    std::function<std::vector<Editor::SceneObjectDiff>()> provider)
{
    if (m_VcsController)
        m_VcsController->SetSceneDiffProvider(std::move(provider));
}

void InspectorPanel::RefreshSceneDiffDecorations()
{
    if (m_VcsController)
        m_VcsController->ApplyDecorations();
}

void InspectorPanel::ShowScriptVariables(const std::filesystem::path& scriptPath,
                                         const std::vector<ScriptVariable>& variables,
                                         const std::vector<ScriptMethod>& methods)
{
    if (UIElement::IsInEventDispatch())
    {
        // Make a copy for the deferred action
        std::filesystem::path pathCopy = scriptPath;
        std::vector<ScriptVariable> varsCopy = variables;
        std::vector<ScriptMethod> methodsCopy = methods;
        const bool suppress = m_SuppressInspectorHistory;
        this->PostAction([this, pathCopy, varsCopy, methodsCopy, suppress]()
                         {
            const bool prev = m_SuppressInspectorHistory;
            m_SuppressInspectorHistory = suppress;
            this->ShowScriptVariables(pathCopy, varsCopy, methodsCopy);
            m_SuppressInspectorHistory = prev; });
        return;
    }

    // Clear entity/asset selection state
    m_World = nullptr;
    m_Entity = EntityHandle{};
    m_SelectedAsset.reset();
    m_LastSelectedAssetPaths.clear();
    m_CurrentScriptPath = scriptPath;
    m_ScriptVariableFields.clear();
    ClearVariableFieldHighlight();
    m_LeftMouseDragging = false;
    m_DraggingLabel = nullptr;
    m_DraggingField = nullptr;

    if (!m_SuppressInspectorHistory && !m_Locked)
    {
        InspectorSelectionHistoryEntry entry{};
        entry.EntryKind = InspectorSelectionHistoryEntry::Kind::Script;
        entry.ScriptPath = scriptPath;
        RecordInspectorHistory(std::move(entry));
    }

    ClearContent();
    if (!m_ContentRoot)
    {
        return;
    }

    // Same fixed-top-strip header as other inspector modes so the title sits
    // at the same Y as the entity/asset inspectors.
    // On-disk case, matching the Script Editor's tab: this header names the same file, and a
    // registry-resolved path reaches here case-folded.
    BuildSimpleTopHeader(Editor::OnDiskFileName(scriptPath), "inspector-header-kind-script");

    // Functions section — shown above Variables so users can jump to any method
    if (!methods.empty())
    {
        // Preserve expanded state when rebuilding for the same script; reset only on script change
        if (m_FunctionsSectionScriptPath != scriptPath)
        {
            m_FunctionsSectionExpanded = false;
            m_FunctionsSectionScriptPath = scriptPath;
        }
        const bool startExpanded = m_FunctionsSectionExpanded;

        auto funcSection = std::make_unique<InspectorSection>("Functions");
        funcSection->AddClass("script-functions-section");
        funcSection->SetCollapsed(!startExpanded);
        funcSection->SetShowEnabledToggle(false); // no blue dot

        // Track user-driven collapse changes so rebuilds restore the state
        funcSection->SetOnCollapsedChanged([this](InspectorSection&, bool collapsed)
                                           { m_FunctionsSectionExpanded = !collapsed; });

        UIElement* funcContent = funcSection->GetContentRoot();

        // Shared state for keyboard navigation: element pointer + its source line number + name
        struct NavItem
        {
            UIElement* El;
            size_t LineNum;
            std::string Name;
        };
        auto navItems = std::make_shared<std::vector<NavItem>>();

        for (const auto& method : methods)
        {
            auto row = std::make_unique<UIElement>();
            row->AddClass("script-function-row");

            // Line number prefix
            auto lineLabel = std::make_unique<Label>();
            lineLabel->AddClass("script-function-line");
            lineLabel->SetText(std::to_string(method.LineNumber));
            row->AddChild(std::move(lineLabel));

            auto nameLabel = std::make_unique<Label>();
            nameLabel->AddClass("script-function-label");
            nameLabel->SetText(method.Name);
            nameLabel->SetFocusable(true);

            const size_t lineNum = method.LineNumber;
            const std::string methodName = method.Name;
            const int myIdx = static_cast<int>(navItems->size());

            // Handle click on the full row so gaps between labels still register
            row->RegisterEventHandler(kEventMouseDown, [this, lineNum, methodName, myIdx, navItems](UIEvent& e)
                                      {
                if (e.Button != 0) return;
                if (m_OnScriptMethodNavigate)
                    m_OnScriptMethodNavigate(lineNum, methodName);
                const NavItem& cur = (*navItems)[myIdx];
                if (auto* mgr = cur.El->GetOwnerManager())
                    mgr->FocusElement(cur.El);
                e.Stop(); });

            nameLabel->RegisterEventHandler(kEventKeyDown, [this, myIdx, navItems](UIEvent& e)
                                            {
                if (e.Key == Input::kKeyCode_Up || e.Key == Input::kKeyCode_Down)
                {
                    const int next = myIdx + (e.Key == Input::kKeyCode_Down ? 1 : -1);
                    if (next >= 0 && next < static_cast<int>(navItems->size()))
                    {
                        const NavItem& target = (*navItems)[next];
                        if (auto* mgr = target.El->GetOwnerManager())
                            mgr->FocusElement(target.El);
                        if (m_OnScriptMethodNavigate)
                            m_OnScriptMethodNavigate(target.LineNum, target.Name);
                    }
                    e.Stop();
                }
                else if (e.Key == Input::kKeyCode_Enter)
                {
                    const NavItem& cur = (*navItems)[myIdx];
                    if (m_OnScriptMethodNavigate)
                        m_OnScriptMethodNavigate(cur.LineNum, cur.Name);
                    e.Stop();
                } });

            navItems->push_back({nameLabel.get(), lineNum, methodName});
            row->AddChild(std::move(nameLabel));
            funcContent->AddChild(std::move(row));
        }

        m_ContentRoot->AddChild(std::move(funcSection));
    }

    if (variables.empty())
    {
        auto label = std::make_unique<Label>();
        label->AddClass("inspector-text");
        label->SetText(Editor::EmptyScriptVariablesText(scriptPath));

        auto column = MakeInspectorPaddedContentColumn();
        column->AddChild(std::move(label));
        m_ContentRoot->AddChild(std::move(column));
        return;
    }

    // Create a section for variables
    auto section = std::make_unique<InspectorSection>("Variables");
    section->AddClass("script-variables-section");
    section->SetShowEnabledToggle(false); // no dot, matches Functions section
    UIElement* sectionContent = section->GetContentRoot();
    m_ContentRoot->AddChild(std::move(section));

    // Add each variable as an editable field: label | value | type hint on one row (matches early script inspector layout).
    for (const auto& var : variables)
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("inspector-row");

        // Variable name label (with drag support for value adjustment)
        auto nameLabel = std::make_unique<Label>();
        nameLabel->AddClass("inspector-label");
        nameLabel->SetText(var.Name);
        UIElement* nameLabelPtr = nameLabel.get();
        row->AddChild(std::move(nameLabel));

        auto valueField = std::make_unique<TextField>();
        valueField->AddClass("inspector-field");
        valueField->SetId(std::string("inspector-script-var-") + var.Name);
        valueField->SetValue(StripValueSuffix(var.Value));
        valueField->SetFocusable(true);

        TextField* fieldPtr = valueField.get();
        m_ScriptVariableFields[var.Name] = fieldPtr;

        // Store variable name for callbacks
        std::string varName = var.Name;

        // Update script in real-time as user types
        fieldPtr->SetOnValueChanging([this, varName](const std::string& newValue)
                                     {
            if (!m_UpdatingFromScript && m_OnVariableEdited) {
                m_OnVariableEdited(varName, newValue);
            } });

        fieldPtr->SetOnValueChanged([this, varName](const std::string& newValue)
                                    {
            if (!m_UpdatingFromScript && m_OnVariableEdited) {
                m_OnVariableEdited(varName, newValue);
            } });

        fieldPtr->RegisterEventHandler(kEventFocusIn, [this, varName](UIEvent&)
                                       {
            // Clear any previous script editor highlighting first
            if (m_OnVariableUnfocused) {
                m_OnVariableUnfocused();
            }
            // Highlight this field in the Inspector
            HighlightVariableField(varName);
            // Highlight this variable in the script editor
            if (m_OnVariableFocused) {
                m_OnVariableFocused(varName);
            } });

        fieldPtr->RegisterEventHandler(kEventFocusOut, [this](UIEvent&)
                                       {
            ClearVariableFieldHighlight();
            if (m_OnVariableUnfocused) {
                m_OnVariableUnfocused();
            } });

        fieldPtr->RegisterEventHandler(kEventMouseDown, [this, varName](UIEvent& e)
                                       {
            if (e.Button == 0) { // Left mouse button
                // Clear any previous script editor highlighting
                if (m_OnVariableUnfocused) {
                    m_OnVariableUnfocused();
                }
                // Highlight this field in the Inspector (blue background)
                HighlightVariableField(varName);
                // Highlight this variable in the script editor
                if (m_OnVariableFocused) {
                    m_OnVariableFocused(varName);
                }
            } });

        row->AddChild(std::move(valueField));

        auto typeLabel = std::make_unique<Label>();
        typeLabel->AddClass("inspector-type-hint");
        typeLabel->SetText(var.TypeName);
        row->AddChild(std::move(typeLabel));

        // Left mouse drag on variable name to adjust value (left/right)
        nameLabelPtr->RegisterEventHandler(kEventMouseDown, [this, varName, fieldPtr, nameLabelPtr](UIEvent& e)
                                           {
            if (e.Button == 0) { // Left mouse button
                m_LeftMouseDragging = true;
                m_DraggingLabel = nameLabelPtr;
                m_DraggingField = fieldPtr;
                m_DragStartX = e.X;
                m_DragStartValue = fieldPtr->GetValue();
                m_DragVariableName = varName;
                
                // Highlight the field when dragging starts
                HighlightVariableField(varName);
                
                // Also highlight in script editor
                if (m_OnVariableFocused) {
                    m_OnVariableFocused(varName);
                }
                
                e.Capture(nameLabelPtr);
                e.Stop();
            } });

        // Handle mouse move for left mouse drag on name
        nameLabelPtr->RegisterEventHandler(kEventMouseMove, [this, varName, fieldPtr, nameLabelPtr](UIEvent& e)
                                           {
            if (m_LeftMouseDragging && m_DraggingLabel == nameLabelPtr && m_DraggingField == fieldPtr) {
                float totalDeltaX = e.X - m_DragStartX;
                AdjustValueByDrag(varName, fieldPtr, totalDeltaX);
                HighlightVariableField(varName);
            } });

        // Handle mouse up to stop dragging and detect double-click for reset
        auto lastClickTime = std::make_shared<std::chrono::steady_clock::time_point>();
        std::string varTypeName = var.TypeName;
        nameLabelPtr->RegisterEventHandler(kEventMouseUp, [this, nameLabelPtr, fieldPtr, varName, varTypeName, lastClickTime](UIEvent& e)
                                           {
            if (e.Button == 0) {
                // Check for double-click to reset to default
                if (!m_LeftMouseDragging || m_DraggingLabel != nameLabelPtr) {
                    auto now = std::chrono::steady_clock::now();
                    
                    if ((now - *lastClickTime) < GameEngine::Platform::GetDoubleClickInterval()) {
                        // Reset to default value based on type
                        std::string defaultValue = GetDefaultValueForType(varTypeName);
                        fieldPtr->SetValue(defaultValue);
                        if (m_OnVariableEdited) {
                            m_OnVariableEdited(varName, defaultValue);
                        }
                        *lastClickTime = {}; // Reset to prevent triple-click
                        e.Stop();
                        return;
                    }
                    *lastClickTime = now;
                }
                
                // Stop dragging if active
                if (m_LeftMouseDragging && m_DraggingLabel == nameLabelPtr) {
                    m_LeftMouseDragging = false;
                    m_DraggingLabel = nullptr;
                    m_DraggingField = nullptr;
                }
                e.Stop();
            } });

        sectionContent->AddChild(std::move(row));
    }
}

void InspectorPanel::UpdateScriptVariableValues(const std::vector<ScriptVariable>& variables)
{
    // Only update if we're currently showing script variables
    if (m_CurrentScriptPath.empty() || m_ScriptVariableFields.empty())
    {
        return;
    }

    // Set flag to prevent callbacks from triggering script updates
    m_UpdatingFromScript = true;

    // Update each variable field if it exists
    for (const auto& var : variables)
    {
        auto it = m_ScriptVariableFields.find(var.Name);
        if (it != m_ScriptVariableFields.end() && it->second)
        {
            TextField* field = it->second;
            std::string displayValue = StripValueSuffix(var.Value);
            // Only update if the value actually changed
            if (field->GetValue() != displayValue)
            {
                field->SetValue(displayValue);
            }
        }
    }

    m_UpdatingFromScript = false;
}

void InspectorPanel::HighlightVariableField(const std::string& varName)
{
    if (m_HighlightedScriptValueChrome)
    {
        m_HighlightedScriptValueChrome->RemoveClass("inspector-field-highlighted");
        m_HighlightedScriptValueChrome = nullptr;
    }

    if (varName.empty())
    {
        return;
    }

    auto it = m_ScriptVariableFields.find(varName);
    if (it == m_ScriptVariableFields.end() || !it->second)
    {
        return;
    }

    m_HighlightedScriptValueChrome = it->second;
    m_HighlightedScriptValueChrome->AddClass("inspector-field-highlighted");
}

void InspectorPanel::ClearVariableFieldHighlight()
{
    if (m_HighlightedScriptValueChrome)
    {
        m_HighlightedScriptValueChrome->RemoveClass("inspector-field-highlighted");
        m_HighlightedScriptValueChrome = nullptr;
    }
}

void InspectorPanel::FocusScriptVariableField(const std::string& varName)
{
    if (varName.empty())
    {
        return;
    }

    auto apply = [this, varName]()
    {
        HighlightVariableField(varName);
        auto it = m_ScriptVariableFields.find(varName);
        if (it == m_ScriptVariableFields.end() || !it->second)
        {
            return;
        }
        TextField* field = it->second;
        const std::string& id = field->GetId();
        if (id.empty())
        {
            return;
        }
        if (UIManager* ui = field->GetOwnerManager())
        {
            ui->SetFocusById(id);
        }
    };

    if (UIElement::IsInEventDispatch())
    {
        PostAction(std::move(apply));
    }
    else
    {
        apply();
    }
}

void InspectorPanel::AdjustValueByDrag(const std::string& varName, TextField* field, float totalDeltaX)
{
    if (!field || m_DragStartValue.empty())
        return;

    // Calculate adjustment: positive deltaX (moving right) increases value
    float adjustment = totalDeltaX * InspectorDrag::kInspectorDragFloatSensitivity;

    // Try to parse as integer first
    try
    {
        int intValue = std::stoi(m_DragStartValue);
        int intAdjustment = static_cast<int>(std::round(adjustment));
        int newValue = intValue + intAdjustment;
        std::string newValueStr = std::to_string(newValue);
        field->SetValue(newValueStr);
        if (m_OnVariableEdited)
        {
            m_OnVariableEdited(varName, newValueStr);
        }
        return;
    }
    catch (...)
    {
        // Not an integer, try float
    }

    // Try to parse as float
    try
    {
        float floatValue = std::stof(m_DragStartValue);
        float newValue = floatValue + adjustment;

        // Format with appropriate precision
        char buffer[64];
        if (std::abs(newValue) < 1000.0f && std::abs(newValue) > 0.01f)
        {
            std::snprintf(buffer, sizeof(buffer), "%.2f", newValue);
        }
        else
        {
            std::snprintf(buffer, sizeof(buffer), "%.6g", newValue);
        }

        std::string newValueStr = buffer;
        // Remove trailing zeros after decimal point
        if (newValueStr.find('.') != std::string::npos)
        {
            while (newValueStr.back() == '0' && newValueStr.size() > 1)
            {
                newValueStr.pop_back();
            }
            if (newValueStr.back() == '.')
            {
                newValueStr.pop_back();
            }
        }

        field->SetValue(newValueStr);
        if (m_OnVariableEdited)
        {
            m_OnVariableEdited(varName, newValueStr);
        }
    }
    catch (...)
    {
        // Not a number, can't adjust
    }
}

std::string InspectorPanel::StripValueSuffix(const std::string& value)
{
    if (value.empty())
    {
        return value;
    }

    std::string result = value;

    // Remove quotes (single or double) from character/string literals
    if ((result.front() == '\'' && result.back() == '\'') ||
        (result.front() == '"' && result.back() == '"'))
    {
        if (result.size() >= 2)
        {
            result = result.substr(1, result.size() - 2);
        }
    }

    // Check if value ends with 'f' or 'F' (float suffix)
    if (!result.empty() && (result.back() == 'f' || result.back() == 'F'))
    {
        if (result.size() > 1)
        {
            char prevChar = result[result.size() - 2];
            if (std::isdigit(prevChar) || prevChar == '.' || prevChar == 'e' || prevChar == 'E')
            {
                result = result.substr(0, result.size() - 1);
            }
        }
    }

    // Check if value ends with 'L' or 'l' (long suffix)
    if (!result.empty() && (result.back() == 'L' || result.back() == 'l'))
    {
        if (result.size() > 1)
        {
            char prevChar = result[result.size() - 2];
            if (std::isdigit(prevChar))
            {
                result = result.substr(0, result.size() - 1);
            }
        }
    }

    return result;
}

void InspectorPanel::ShowSmartFolder(const std::string& smartFolderId, SmartFolderManager* manager)
{
    if (m_Locked)
        return;

    if (UIElement::IsInEventDispatch())
    {
        std::string idCopy = smartFolderId;
        const bool suppress = m_SuppressInspectorHistory;
        this->PostAction([this, idCopy, manager, suppress]()
                         {
            const bool prev = m_SuppressInspectorHistory;
            m_SuppressInspectorHistory = suppress;
            this->ShowSmartFolder(idCopy, manager);
            m_SuppressInspectorHistory = prev; });
        return;
    }

    // Clear other selection states
    m_World = nullptr;
    m_Entity = EntityHandle{};
    m_SelectedAsset.reset();
    m_LastSelectedAssetPaths.clear();

    if (!m_SuppressInspectorHistory && !m_Locked && manager && !smartFolderId.empty())
    {
        InspectorSelectionHistoryEntry entry{};
        entry.EntryKind = InspectorSelectionHistoryEntry::Kind::SmartFolder;
        entry.SmartFolderId = smartFolderId;
        entry.SmartFolderMgr = manager;
        RecordInspectorHistory(std::move(entry));
    }

    ClearContent();
    if (!m_ContentRoot || !manager)
    {
        return;
    }

    // Create and configure the SmartFolderInspector widget
    auto inspector = std::make_unique<SmartFolderInspector>();
    SmartFolderInspector* inspectorPtr = inspector.get();

    std::string headerTitle = "Smart Folder";
    if (SmartFolder* folder = manager->GetById(smartFolderId))
        headerTitle = folder->Name;

    BuildSimpleTopHeader(headerTitle, "inspector-header-kind-folder", [inspectorPtr](const std::string& newName)
                         {
                             if (inspectorPtr)
                                 inspectorPtr->SetNameValueFromExternal(newName); }, [manager, smartFolderId, inspectorPtr](const std::string& newName)
                         {
                             if (SmartFolder* folder = manager->GetById(smartFolderId))
                             {
                                 folder->Name = newName;
                                 manager->Update(*folder);
                             }
                             if (inspectorPtr)
                                 inspectorPtr->SetNameValueFromExternal(newName); });

    TextField* headerTitleField = m_HeaderTitleField;
    inspector->SetUndoRedoService(m_Undo);
    inspector->SetOnNameChanging([headerTitleField](const std::string& newName)
                                 {
        if (headerTitleField && headerTitleField->GetValue() != newName)
            headerTitleField->SetValueWithoutNotify(newName); });
    inspector->SetOnNameChanged([headerTitleField](const std::string& newName)
                                {
        if (headerTitleField && headerTitleField->GetValue() != newName)
            headerTitleField->SetValueWithoutNotify(newName); });
    inspector->SetOnDeleted([this]()
                            { this->ClearContent(); });
    inspector->SetSmartFolder(smartFolderId, manager);

    m_SmartFolderInspector = inspector.get();
    m_ContentRoot->AddChild(std::move(inspector));
}

void InspectorPanel::ShowCustomInspector(const std::string& title,
                                         const std::string& iconClass,
                                         std::function<void(UIElement*)> buildContent)
{
    if (m_Locked || !m_ContentRoot || !m_TopRoot)
        return;

    m_World = nullptr;
    m_Entity = EntityHandle{};
    m_Entities.clear();
    m_SelectedAsset.reset();
    m_LastSelectedAssetPaths.clear();

    auto rebuild = [this, title, iconClass, buildContent = std::move(buildContent)]() mutable
    {
        ClearContent();
        BuildSimpleTopHeader(title, iconClass);
        if (buildContent && m_ContentRoot)
            buildContent(m_ContentRoot);
        WireSoloHandlersForSections();
        if (m_ContentRoot)
            m_ContentRoot->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    };

    if (UIElement::IsInEventDispatch())
        PostAction(std::move(rebuild));
    else
        rebuild();
}

void InspectorPanel::ShowGraphNode(const std::string& nodeId, const std::string& nodeTypeId,
                                   const std::string& displayName,
                                   const std::unordered_map<std::string, std::string>& parameters,
                                   std::string_view kindId)
{
    const std::string graphKindId(kindId);

    if (!m_ContentRoot)
    {
        Logger::Log::Error("[Inspector] ShowGraphNode: m_ContentRoot is NULL!");
        return;
    }

    if (!m_TopRoot)
    {
        Logger::Log::Error("[Inspector] ShowGraphNode: m_TopRoot is NULL!");
        return;
    }

    // Clear other selection states
    m_World = nullptr;
    m_Entity = EntityHandle{};
    m_SelectedAsset.reset();
    m_LastSelectedAssetPaths.clear();
    m_GraphTransitionLinkId.clear();

    // IMPORTANT: Don't call ClearContent() directly during event dispatch as it will defer
    // and clear content we're about to add. Instead, rebuild everything in one PostAction.
    PostAction([this, nodeId, nodeTypeId, displayName, parameters, graphKindId]()
               {
        // Now safe to clear
        ClearContent();
        m_ShowingGraphNode = true;

        // Build header with node name
        BuildSimpleTopHeader(displayName, "inspector-header-kind-node");

        // Create properties section
        auto section = std::make_unique<InspectorSection>("Properties");
        UIElement* sectionContent = section->GetContentRoot();
            m_ContentRoot->AddChild(std::move(section));

        // Node Type row
        {
            auto row = std::make_unique<UIElement>();
            row->AddClass("inspector-row");

            auto label = std::make_unique<Label>();
            label->AddClass("inspector-label");
            label->SetText("Type");
            row->AddChild(std::move(label));

            auto value = std::make_unique<TextField>();
            value->SetValue(nodeTypeId);
            value->AddClass("inspector-field");
            row->AddChild(std::move(value));

            sectionContent->AddChild(std::move(row));
        }

        // Node ID row
        {
            auto row = std::make_unique<UIElement>();
            row->AddClass("inspector-row");

            auto label = std::make_unique<Label>();
            label->AddClass("inspector-label");
            label->SetText("ID");
            row->AddChild(std::move(label));

            auto value = std::make_unique<TextField>();
            value->SetValue(nodeId);
            value->AddClass("inspector-field");
            row->AddChild(std::move(value));

            sectionContent->AddChild(std::move(row));
        }

        // Parameters rows — widgets come from NodeTypeMeta.Parameters / live keys,
        // not a typeId special-case list (a user kind with r,g,b gets a swatch).
        const NodeTypeMeta* schema = FindNodeTypeMeta(graphKindId, nodeTypeId);
        const std::vector<GraphInspectorRow> planned = PlanGraphInspectorRows(parameters, schema);
        auto varIt = parameters.find("variableName");
        const std::string linkedVariableName =
            (varIt != parameters.end()) ? varIt->second : std::string();
        size_t paramCount = 0;
        for (const GraphInspectorRow& plannedRow : planned)
        {
            switch (plannedRow.Kind)
            {
            case GraphInspectorRowKind::VariableName:
                AddGraphVariableNameRow(sectionContent, linkedVariableName);
                break;
            case GraphInspectorRowKind::ColorSwatch:
                AddGraphColorParameterRow(sectionContent, nodeId, parameters, m_OpenColorPickerWindow,
                                          m_OnGraphNodeParameterChanged);
                break;
            case GraphInspectorRowKind::VariableColor:
            {
                const float r = ParseGraphColorParam(parameters, "r", 1.0f);
                const float g = ParseGraphColorParam(parameters, "g", 1.0f);
                const float b = ParseGraphColorParam(parameters, "b", 1.0f);
                AddGraphVariableColorRow(sectionContent, linkedVariableName, r, g, b, m_OpenColorPickerWindow,
                    [this](const std::string& variableName, const std::string& value, bool commitUndo) {
                        if (m_OnGraphVariableValueChanged)
                            m_OnGraphVariableValueChanged(variableName, value, commitUndo);
                    });
                break;
            }
            case GraphInspectorRowKind::VariableFloat:
            {
                float floatValue = 0.0f;
                if (auto valueIt = parameters.find("value"); valueIt != parameters.end())
                    (void)TryParseGraphFloatParam(valueIt->second, floatValue);
                auto applyFloat = [this, linkedVariableName](float newValue)
                {
                    if (m_OnGraphVariableValueChanged)
                        m_OnGraphVariableValueChanged(linkedVariableName, FormatGraphFloatParam(newValue), true);
                };
                auto previewFloat = [this, linkedVariableName](float newValue)
                {
                    if (m_OnGraphVariableValueChanged)
                        m_OnGraphVariableValueChanged(linkedVariableName, FormatGraphFloatParam(newValue), false);
                };
                InspectorDrag::AddFloatRowWithDrag(sectionContent, plannedRow.Label, floatValue, previewFloat,
                                                   applyFloat, floatValue);
                break;
            }
            case GraphInspectorRowKind::VariableVec:
            {
                static const char* kKeys[] = {"x", "y", "z", "w"};
                const int componentCount = plannedRow.VecCount;
                const int axis = plannedRow.VecAxis;
                float axisValue = 0.0f;
                if (auto valueIt = parameters.find(plannedRow.Key); valueIt != parameters.end())
                    (void)TryParseGraphFloatParam(valueIt->second, axisValue);
                auto applyFloat = [this, linkedVariableName, parameters, componentCount, axis](float newValue)
                {
                    if (!m_OnGraphVariableValueChanged)
                        return;
                    std::vector<float> values(static_cast<size_t>(componentCount), 0.0f);
                    for (int i = 0; i < componentCount; ++i)
                    {
                        if (auto valueIt = parameters.find(kKeys[i]); valueIt != parameters.end())
                            (void)TryParseGraphFloatParam(valueIt->second, values[static_cast<size_t>(i)]);
                    }
                    values[static_cast<size_t>(axis)] = newValue;
                    m_OnGraphVariableValueChanged(linkedVariableName, FormatGraphVariableComponents(values),
                                                  true);
                };
                auto previewFloat = [this, linkedVariableName, parameters, componentCount, axis](float newValue)
                {
                    if (!m_OnGraphVariableValueChanged)
                        return;
                    std::vector<float> values(static_cast<size_t>(componentCount), 0.0f);
                    for (int i = 0; i < componentCount; ++i)
                    {
                        if (auto valueIt = parameters.find(kKeys[i]); valueIt != parameters.end())
                            (void)TryParseGraphFloatParam(valueIt->second, values[static_cast<size_t>(i)]);
                    }
                    values[static_cast<size_t>(axis)] = newValue;
                    m_OnGraphVariableValueChanged(linkedVariableName, FormatGraphVariableComponents(values),
                                                  false);
                };
                InspectorDrag::AddFloatRowWithDrag(sectionContent, plannedRow.Label, axisValue, previewFloat,
                                                   applyFloat, axisValue);
                break;
            }
            case GraphInspectorRowKind::NodeEnum:
            {
                const std::string key = plannedRow.Key;
                auto valueIt = parameters.find(key);
                const std::string current =
                    (valueIt != parameters.end()) ? valueIt->second : std::string();
                std::vector<Dropdown::Option> options;
                if (const NodeParamSpec* spec = FindSchemaParam(schema, key))
                {
                    options.reserve(spec->Options.size());
                    for (const Graph::NodeParamOption& option : spec->Options)
                        options.push_back({option.Value, option.Label});
                }
                int selected = 0;
                for (size_t i = 0; i < options.size(); ++i)
                {
                    if (options[i].value == current)
                        selected = static_cast<int>(i);
                }

                auto row = std::make_unique<UIElement>();
                row->AddClass("inspector-row");
                auto label = std::make_unique<Label>();
                label->AddClass("inspector-label");
                label->SetText(plannedRow.Label);
                row->AddChild(std::move(label));

                auto dropdown = std::make_unique<Dropdown>();
                dropdown->AddClass("inspector-dropdown");
                dropdown->SetOptions(options, selected);
                dropdown->SetOnValueChanged([this, nodeId, key](const std::string& v)
                {
                    if (m_OnGraphNodeParameterChanged)
                        m_OnGraphNodeParameterChanged(nodeId, key, v, true);
                });
                row->AddChild(std::move(dropdown));
                sectionContent->AddChild(std::move(row));
                break;
            }
            case GraphInspectorRowKind::NodeAsset:
            {
                const std::string& key = plannedRow.Key;
                auto valueIt = parameters.find(key);
                const std::string value = (valueIt != parameters.end()) ? valueIt->second : std::string();
                AssetManager& am = EngineCore::GetInstance().GetAssetManager();
                InspectorUI::AddAssetFieldRow(
                    sectionContent, plannedRow.Label, GUID(value), {AssetType::Animation},
                    &am.GetRegistry(),
                    [this, nodeId, key](const GUID& picked)
                    {
                        const std::string text = picked.IsNull() ? std::string{} : picked.ToString();
                        if (m_OnGraphNodeParameterChanged)
                            m_OnGraphNodeParameterChanged(nodeId, key, text, true);
                    },
                    nullptr, "Animation clip");
                break;
            }
            case GraphInspectorRowKind::NodeInt:
            case GraphInspectorRowKind::NodeFloat:
            case GraphInspectorRowKind::NodeText:
            {
                const std::string& key = plannedRow.Key;
                auto valueIt = parameters.find(key);
                const std::string value = (valueIt != parameters.end()) ? valueIt->second : std::string();
                if (plannedRow.Kind == GraphInspectorRowKind::NodeInt)
                {
                    int intValue = 0;
                    (void)TryParseGraphIntParam(value, intValue);
                    auto applyInt = [this, nodeId, key](int newValue)
                    {
                        if (m_OnGraphNodeParameterChanged)
                            m_OnGraphNodeParameterChanged(nodeId, key, std::to_string(newValue), true);
                    };
                    auto previewInt = [this, nodeId, key](int newValue)
                    {
                        if (m_OnGraphNodeParameterChanged)
                            m_OnGraphNodeParameterChanged(nodeId, key, std::to_string(newValue), false);
                    };
                    InspectorDrag::AddIntRowWithDrag(sectionContent, plannedRow.Label, intValue, previewInt,
                                                     applyInt, intValue);
                }
                else if (plannedRow.Kind == GraphInspectorRowKind::NodeFloat)
                {
                    float floatValue = 0.0f;
                    (void)TryParseGraphFloatParam(value, floatValue);
                    auto applyFloat = [this, nodeId, key](float newValue)
                    {
                        if (m_OnGraphNodeParameterChanged)
                            m_OnGraphNodeParameterChanged(nodeId, key, FormatGraphFloatParam(newValue), true);
                    };
                    auto previewFloat = [this, nodeId, key](float newValue)
                    {
                        if (m_OnGraphNodeParameterChanged)
                            m_OnGraphNodeParameterChanged(nodeId, key, FormatGraphFloatParam(newValue), false);
                    };
                    const std::optional<NodeValueRange> range = FindNodeValueRange(schema, key);
                    InspectorDrag::AddFloatRowWithDrag(
                        sectionContent, plannedRow.Label, floatValue, previewFloat, applyFloat,
                        floatValue, nullptr,
                        range ? range->Min : -std::numeric_limits<float>::infinity(),
                        range ? range->Max : std::numeric_limits<float>::infinity());
                }
                else
                {
                    auto row = std::make_unique<UIElement>();
                    row->AddClass("inspector-row");

                    auto label = std::make_unique<Label>();
                    label->AddClass("inspector-label");
                    label->SetText(plannedRow.Label);
                    row->AddChild(std::move(label));

                    auto valField = std::make_unique<TextField>();
                    valField->SetValue(value);
                    valField->AddClass("inspector-field");
                    valField->SetOnValueChanged([this, nodeId, key](const std::string& newValue) {
                        if (m_OnGraphNodeParameterChanged)
                            m_OnGraphNodeParameterChanged(nodeId, key, newValue, true);
                    });
                    row->AddChild(std::move(valField));

                    sectionContent->AddChild(std::move(row));
                }
                break;
            }
            }
            paramCount++;
        }
        

        // Mark dirty to trigger layout update
        if (m_ContentRoot) {
                    m_ContentRoot->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        }
            MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        
        });
}

void InspectorPanel::ClearGraphNodeIfShown()
{
    // Deliberately ignores m_Locked: a lock pins a live object, but this node
    // is gone from the canvas — its rows would edit whatever node in the new
    // graph happens to share the id.
    if (!m_ShowingGraphNode)
        return;
    ClearContent();
}

void InspectorPanel::CommitGraphTransition(bool commitUndo)
{
    if (m_GraphTransitionLinkId.empty() || !m_OnGraphTransitionChanged)
        return;
    m_OnGraphTransitionChanged(m_GraphTransitionLinkId, m_GraphTransitionDesc, commitUndo);
}

void InspectorPanel::ShowGraphTransition(const std::string& linkId,
                                         const std::string& fromName,
                                         const std::string& toName,
                                         const GraphTransitionDesc& desc)
{
    if (!m_ContentRoot || !m_TopRoot)
        return;

    if (linkId.empty())
    {
        if (m_GraphTransitionLinkId.empty())
            return;
        m_GraphTransitionLinkId.clear();
        m_GraphTransitionFromName.clear();
        m_GraphTransitionToName.clear();
        m_GraphTransitionDesc = {};
        auto rebuild = [this]()
        {
            if (!m_GraphTransitionLinkId.empty())
                return;
            ClearContent();
            MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        };
        if (UIElement::IsInEventDispatch())
            PostAction(std::move(rebuild));
        else
            rebuild();
        return;
    }

    m_World = nullptr;
    m_Entity = EntityHandle{};
    m_SelectedAsset.reset();
    m_LastSelectedAssetPaths.clear();
    m_GraphTransitionLinkId = linkId;
    m_GraphTransitionFromName = fromName;
    m_GraphTransitionToName = toName;
    m_GraphTransitionDesc = desc;

    auto rebuild = [this, linkId]()
    {
        if (m_GraphTransitionLinkId != linkId)
            return;
        ClearContent();

        std::string header = "Transition";
        if (!m_GraphTransitionFromName.empty() || !m_GraphTransitionToName.empty())
            header = m_GraphTransitionFromName + " → " + m_GraphTransitionToName;
        BuildSimpleTopHeader(header, "inspector-header-kind-node");

        auto section = std::make_unique<InspectorSection>("Transition");
        UIElement* sectionContent = section->GetContentRoot();
        m_ContentRoot->AddChild(std::move(section));

        InspectorDrag::AddFloatRowWithDrag(
            sectionContent, "Duration", m_GraphTransitionDesc.Duration,
            [this](float value)
            {
                m_GraphTransitionDesc.Duration = value;
                CommitGraphTransition(false);
            },
            [this](float value)
            {
                m_GraphTransitionDesc.Duration = value;
                CommitGraphTransition(true);
            },
            GraphTransitionStore::kDefaultDuration, "Blend duration in seconds.", 0.0f);

        auto conditionsSection = std::make_unique<InspectorSection>("Conditions");
        UIElement* conditionsContent = conditionsSection->GetContentRoot();
        m_ContentRoot->AddChild(std::move(conditionsSection));

        if (m_GraphTransitionDesc.Conditions.empty())
        {
            auto hint = std::make_unique<Label>();
            hint->AddClass("inspector-transition-hint");
            hint->SetText("Add at least one condition. Compile skips unconditioned transitions.");
            conditionsContent->AddChild(std::move(hint));
        }

        for (size_t i = 0; i < m_GraphTransitionDesc.Conditions.size(); ++i)
        {
            auto headerRow = std::make_unique<UIElement>();
            headerRow->AddClass("inspector-row");
            auto headerLabel = std::make_unique<Label>();
            headerLabel->AddClass("inspector-label");
            headerLabel->SetText("Condition " + std::to_string(i + 1));
            headerRow->AddChild(std::move(headerLabel));
            conditionsContent->AddChild(std::move(headerRow));

            GraphTransitionConditionDesc& cond = m_GraphTransitionDesc.Conditions[i];

            {
                auto row = std::make_unique<UIElement>();
                row->AddClass("inspector-row");
                auto label = std::make_unique<Label>();
                label->AddClass("inspector-label");
                label->SetText("Param");
                row->AddChild(std::move(label));
                auto field = std::make_unique<TextField>();
                field->SetValue(cond.Param);
                field->AddClass("inspector-field");
                field->SetOnValueChanged([this, i](const std::string& text)
                {
                    if (i >= m_GraphTransitionDesc.Conditions.size())
                        return;
                    m_GraphTransitionDesc.Conditions[i].Param = text;
                    CommitGraphTransition(true);
                });
                row->AddChild(std::move(field));
                conditionsContent->AddChild(std::move(row));
            }

            std::vector<Dropdown::Option> opOptions;
            opOptions.reserve(std::size(kGraphTransitionCompareOps));
            int selectedOp = 0;
            for (size_t oi = 0; oi < std::size(kGraphTransitionCompareOps); ++oi)
            {
                opOptions.push_back({kGraphTransitionCompareOps[oi].Value, kGraphTransitionCompareOps[oi].Label, {}});
                if (cond.Op == kGraphTransitionCompareOps[oi].Value)
                    selectedOp = static_cast<int>(oi);
            }
            Dropdown* opDropdown = InspectorUI::AddDropdownRow(conditionsContent, "Op", opOptions, selectedOp,
                                                                 "Comparison used at runtime.");
            opDropdown->SetOnValueChanged([this, i](const std::string& value)
            {
                if (i >= m_GraphTransitionDesc.Conditions.size())
                    return;
                m_GraphTransitionDesc.Conditions[i].Op = value;
                CommitGraphTransition(true);
            });

            {
                auto row = std::make_unique<UIElement>();
                row->AddClass("inspector-row");
                auto label = std::make_unique<Label>();
                label->AddClass("inspector-label");
                label->SetText("Value");
                row->AddChild(std::move(label));
                auto field = std::make_unique<TextField>();
                field->SetValue(GraphTransitionStore::ValueToText(cond.Value));
                field->AddClass("inspector-field");
                field->SetOnValueChanged([this, i](const std::string& text)
                {
                    if (i >= m_GraphTransitionDesc.Conditions.size())
                        return;
                    Graph::GraphValue parsed;
                    if (!GraphTransitionStore::TryParseValueText(text, parsed))
                        return;
                    m_GraphTransitionDesc.Conditions[i].Value = std::move(parsed);
                    CommitGraphTransition(true);
                });
                row->AddChild(std::move(field));
                conditionsContent->AddChild(std::move(row));
            }

            auto removeBtn = std::make_unique<Button>();
            removeBtn->SetText("Remove");
            removeBtn->AddClass("inspector-button");
            removeBtn->RegisterEventHandler(kEventButtonClick, [this, i](UIEvent&)
            {
                if (i >= m_GraphTransitionDesc.Conditions.size())
                    return;
                m_GraphTransitionDesc.Conditions.erase(m_GraphTransitionDesc.Conditions.begin() +
                                                       static_cast<std::ptrdiff_t>(i));
                CommitGraphTransition(true);
                ShowGraphTransition(m_GraphTransitionLinkId, m_GraphTransitionFromName, m_GraphTransitionToName,
                                    m_GraphTransitionDesc);
            });
            conditionsContent->AddChild(std::move(removeBtn));
        }

        auto addBtn = std::make_unique<Button>();
        addBtn->SetText("Add Condition");
        addBtn->AddClass("inspector-button");
        addBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&)
        {
            m_GraphTransitionDesc.Conditions.push_back(GraphTransitionStore::MakeDefaultCondition());
            CommitGraphTransition(true);
            ShowGraphTransition(m_GraphTransitionLinkId, m_GraphTransitionFromName, m_GraphTransitionToName,
                                m_GraphTransitionDesc);
        });
        conditionsContent->AddChild(std::move(addBtn));

        if (m_ContentRoot)
            m_ContentRoot->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
    };

    if (UIElement::IsInEventDispatch())
        PostAction(std::move(rebuild));
    else
        rebuild();
}

void InspectorPanel::ClearSearchHighlights()
{
    for (UIElement* el : m_SearchHighlightedElements)
    {
        if (el)
            el->RemoveClass("search-match");
    }
    m_SearchHighlightedElements.clear();
    for (UIElement* el : m_SearchFilteredElements)
    {
        if (el)
            el->RemoveClass("search-filtered-out");
    }
    m_SearchFilteredElements.clear();
}

void InspectorPanel::ApplySearchFilter(const std::string& searchText)
{
    // Clear previous highlights
    ClearSearchHighlights();
    m_CurrentSearchText = searchText;

    if (searchText.empty() || !m_ContentRoot)
        return;

    // Convert search text to lowercase for case-insensitive matching
    std::string searchLower = searchText;
    std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(), ::tolower);

    std::unordered_map<UIElement*, bool> subtreeMatches;
    std::unordered_set<UIElement*> directMatches;

    // Recursively find matches first. Visibility is applied in a second pass so
    // parents remain available when only a descendant matches.
    std::function<bool(UIElement*)> searchInElement = [&](UIElement* el) -> bool
    {
        if (!el)
            return false;

        bool hasMatchInSubtree = false;

        // Check ordinary labels when the selected scope includes them.
        if (m_SearchFieldScope != "section")
        {
            if (auto* label = dynamic_cast<Label*>(el))
            {
                std::string textLower = label->GetText();
                std::transform(textLower.begin(), textLower.end(), textLower.begin(), ::tolower);

                if (textLower.find(searchLower) != std::string::npos)
                {
                    label->AddClass("search-match");
                    m_SearchHighlightedElements.push_back(label);
                    directMatches.insert(label);
                    hasMatchInSubtree = true;
                }
            }
        }

        // Check titled sections when the selected scope includes them.
        if (m_SearchFieldScope != "label")
        {
            if (auto* foldout = dynamic_cast<Foldout*>(el))
            {
                std::string titleLower = foldout->GetTitle();
                std::transform(titleLower.begin(), titleLower.end(), titleLower.begin(), ::tolower);

                if (titleLower.find(searchLower) != std::string::npos)
                {
                    foldout->AddClass("search-match");
                    m_SearchHighlightedElements.push_back(foldout);
                    directMatches.insert(foldout);
                    hasMatchInSubtree = true;
                }
            }
        }

        // Recursively search children
        for (const auto& child : el->GetChildren())
        {
            if (searchInElement(child.get()))
                hasMatchInSubtree = true;
        }

        // If there's a match in the subtree and this is a Foldout, expand it
        if (hasMatchInSubtree)
        {
            if (auto* foldout = dynamic_cast<Foldout*>(el))
            {
                if (!foldout->IsExpanded())
                    foldout->SetExpanded(true);
            }
        }

        subtreeMatches[el] = hasMatchInSubtree;
        return hasMatchInSubtree;
    };

    // Start search from content root
    searchInElement(m_ContentRoot);

    std::function<void(UIElement*, bool)> applyVisibility = [&](UIElement* el, bool showWholeSubtree)
    {
        if (!el)
            return;
        const bool directMatch = directMatches.count(el) != 0;
        const bool keepWholeSubtree = showWholeSubtree || directMatch;
        for (const auto& child : el->GetChildren())
            applyVisibility(child.get(), keepWholeSubtree);

        if (el == m_ContentRoot)
            return;
        const bool filterable = dynamic_cast<Foldout*>(el) != nullptr ||
                                el->HasClass("inspector-row") ||
                                el->GetParent() == m_ContentRoot;
        const bool hasMatch = subtreeMatches.count(el) != 0 && subtreeMatches[el];
        if (filterable && !keepWholeSubtree && !hasMatch)
        {
            el->AddClass("search-filtered-out");
            m_SearchFilteredElements.push_back(el);
        }
    };
    applyVisibility(m_ContentRoot, false);
}

void InspectorPanel::SyncEntityActivityPresentation()
{
    const bool active = m_EntityActivity.Present(*m_World, m_Entity).ActiveInHierarchy;
    if (active == m_PresentedEntityActive)
        return;
    m_PresentedEntityActive = active;
    for (const auto& [section, typeId] : m_SectionTypeIds)
        section->SetInactive(!active);
}

void InspectorPanel::PollPendingAssetLoad()
{
    if (!m_PendingAssetLoad)
        return;
    if (m_LastSelectedAssetPaths.size() != 1 || m_LastSelectedAssetPaths.front() != m_PendingAssetPath)
    {
        m_PendingAssetLoad.reset();
        return;
    }
    if (!m_PendingAssetLoad->Valid() ||
        m_PendingAssetLoad->wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        return;
    // Held until the presentation below takes it from the asset manager.
    const SharedPtr<Asset> landed = m_PendingAssetLoad->get();
    m_PendingAssetLoad.reset();
    if (!landed)
        m_FailedAssetLoad = EngineCore::GetInstance().GetAssetManager().ResolveAssetGuid(m_PendingAssetPath);
    // Through RefreshCurrentTarget, which lifts the lock around the re-presentation: a lock taken
    // while "Loading" shows pins this asset, so its landed load replaces the placeholder.
    RefreshCurrentTarget();
}

void InspectorPanel::TickSimulationRefresh()
{
    PollPendingAssetLoad();
    if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
        return;

    // The derived state lands with the hierarchy pass, a frame after the switch that caused it.
    SyncEntityActivityPresentation();

    for (auto& entry : m_SectionRefreshCallbacks)
    {
        for (auto& cb : entry.second.Frame)
            cb();
    }

    auto now = std::chrono::steady_clock::now();
    constexpr auto kSimRefreshInterval = std::chrono::milliseconds(100); // ~10 Hz
    if ((now - m_LastSimulationRefresh) < kSimRefreshInterval)
        return;
    m_LastSimulationRefresh = now;

    // Rebuild when a component is added/removed on the selected entity through a
    // path that did not fire a change notification (e.g. the debug/MCP
    // set_component flow, or any direct ECS mutation). The change-notification
    // subscription covers notified paths; this poll covers the rest so newly
    // added component settings appear immediately instead of after a reselect.
    {
        std::vector<ECS::ComponentTypeId> current =
            CollectVisibleComponentSignature(m_World, m_Entity);
        if (current != m_LastComponentSignature)
        {
            m_LastComponentSignature = std::move(current);
            ShowEntity(m_World, m_Entity, /*force=*/true);
            return;
        }
    }

    for (auto& entry : m_SectionRefreshCallbacks)
    {
        for (auto& cb : entry.second.Simulation)
            cb();
    }
}

} // namespace GameEngine
