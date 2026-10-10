#include "Panels/SettingsPanel.h"
#include "Platform/SystemMetrics.h"
#include "AssetCore/SharedFileRead.h"
#include "Panels/BuildPanel.h"
#include "Panels/HierarchyPanel.h"
#include "EditorApplication.h"
#include "EditorContext.h"
#include "Editor/Assets/AssetRelativePath.h"
#include "Editor/Settings/BuildFolderDefaults.h"
#include "Editor/Shortcuts/EditorShortcuts.h"
#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/SvgRasterSettings.h"
#include "Editor/Settings/BuildRenderPipelineSettings.h"
#include "Editor/Settings/DirectionalShadowSettingsPage.h"
#include "Editor/Settings/EditorHiDpiPlatformSettings.h"
#include "Editor/EditorTreeTitleIconVars.h"
#include "Components/Rendering/PostProcessVolume.h"
#include "UI/AnimationWindowSettings.h"
#include "UI/EditorIcons.h"
#include "UI/UIAccentStyleHelper.h"
#include "UI/SyntaxHighlightSettings.h"
#include "UI/Controls/DockspaceElement.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "Core/Engine.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Editor/Settings/CurveEditorSettings.h"
#include "Editor/Settings/SettingsStore.h"
#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "VCSIntegration/IVCSIntegration.h"
#include "Types/StringId.h"
#include "UI/UIEvents.h"
#include "Input/KeyCodes.h"
#include "Input/InputSystem.h"
#include "EditorInputActions.h"
#include "Assets/AssetManager.h"
#include "Assets/MeshLODGenerator.h"
#include "UI/EditorTags.h"
#include "UI/EditorUIFontSettings.h"
#include "Engine/UI/FontResolver.h"
#include "Rendering/Text/FontAtlas.h"
#include "EditorContextMenu/UIContextMenu.h"
#include "Platform/ContextMenu.h"
#include "UndoRedo/UndoRedoService.h"
#include "UndoRedo/TagCommands.h"
#include "Assets/AssetRegistry.h"
#include "AssetCore/AssetTypes.h"
#include "Thumbnails/ModelThumbnailHandler.h"
#include "Platform/Display.h"
#include "Platform/Window.h"
#include "Rendering/Core/Device.h"
#include "UI/Controls/CurveField.h"
#include "UI/Controls/CubicBezierField.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Slider.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/Dropdown.h"

namespace {
GameEngine::Dropdown* g_ActivePipelineDropdown = nullptr;
bool g_SuppressActivePipelineDropdownCallback = false;
constexpr const char* kPrefKeyTabRightClickContextMenu = "ui.tabRightClickContextMenu";

constexpr const char* kPrefKeyInspectorBigNumberSpacing = "ui.inspectorBigNumberSpacing";
constexpr bool kInspectorBigNumberSpacingDefault = true;
}
#include "UI/Controls/TextField.h"
#include "UI/Controls/Toggle.h"
#include "UI/Controls/Checkbox.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/TreeView.h"
#include "UI/Controls/SplitView.h"
#include "UI/Controls/Splitter.h"
#include "UI/Controls/WeightedPane.h"
#include "UI/Controls/ScrollView.h"
#include "UI/Controls/Foldout.h"
#include "UI/UIManager.h"
#include "UI/UIElement.h"
#include "UI/StyleProperties.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/Assets/UILayoutAsset.h"
#include "UI/Assets/UIStyleAsset.h"
#include "Core/Application.h"
#include "Editor/Settings/ScriptEditorSettings.h"
#include "Editor/Settings/UniversalSearchSettings.h"
#include "Editor/Assets/ShaderGlslOpen.h"
#include "Editor/Settings/FbxImportSettings.h"
#include "Editor/Settings/SettingsStore.h"
#include "Editor/Settings/DynamicResolutionProjectSettings.h"
#include "Editor/Settings/RenderProjectSettings.h"
#include "Editor/Settings/SceneViewSettings.h"
#include "Editor/Settings/ToggleAppearanceSettings.h"
#include "Editor/Settings/TooltipSettings.h"
#include "Assets/SvgRasterizer.h"
#include "Editor/DragDropPayloads.h"
#include "Engine/Build/AppIconGenerator.h"
#include "Engine/Build/BuildPlatforms.h"
#include "Engine/Build/PlayerBuildConfig.h"
#include "Panels/BookmarksPanel.h"
#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "UI/Interaction/DropTarget.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/Controls/IntField.h"
#include "Physics/PhysicsTypes.h"
#include "Editor/Settings/PhysicsProjectSettings.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "UI/Controls/CollapsibleInfoCard.h"
#include "UI/InfoCard.h"
#include "Platform/Shell.h"

#include <fstream>

namespace GameEngine
{
namespace
{

// Info text as a card that collapses into itself. Settings pages attach these
// under the control they explain.
std::unique_ptr<UIElement> MakeSettingsInfoCard(const std::string& text)
{
    return std::make_unique<EditorUI::CollapsibleInfoCard>(text);
}

void MarkCurveIndicatorControlsDirty(UIElement* root)
{
    if (!root)
        return;

    if (dynamic_cast<CurveField*>(root) || dynamic_cast<CubicBezierField*>(root))
        root->MarkDirty(UIElement::VisualDirty);

    for (const auto& child : root->GetChildren())
        MarkCurveIndicatorControlsDirty(child.get());
}

} // namespace

void SettingsPanel::UpdateSwatchStyle(UIElement* swatch, uint32_t colorRgb)
{
    if (!swatch)
        return;

    constexpr float kSwatchSize = 16.0f;
    constexpr float kSwatchBorderRadius = 2.0f;
    constexpr uint32_t kSwatchBorderColor = 0xFF555555u;

    const uint32_t swatchArgb = 0xFF000000u | (colorRgb & 0x00FFFFFFu);
    swatch->Overrides()
        .Set(Style::Width, StyleLength::Px(kSwatchSize))
        .Set(Style::Height, StyleLength::Px(kSwatchSize))
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{kSwatchBorderRadius, kSwatchBorderRadius, kSwatchBorderRadius, kSwatchBorderRadius})
        .Set(Style::BorderWidth, Box4{1.0f, 1.0f, 1.0f, 1.0f})
        .Set(Style::BorderColor, BorderColorsTRBL{kSwatchBorderColor, kSwatchBorderColor, kSwatchBorderColor, kSwatchBorderColor})
        .Set(Style::BackgroundColor, swatchArgb)
        .Set(Style::Cursor, CursorStyle::Pointer);
}
namespace
{
static constexpr const char* kRenderingSettingsKey = "rendering";
static constexpr const char* kMsaaSettingKey = "msaa";
static constexpr const char* kMsaaAutoValue = "auto";
// Settings colours are SDR: the picker's HDR intensity multiplier stays at 1.
static constexpr float kSettingsColorIntensity = 1.0f;

// The project's AA + render-scale settings are edited as one snapshot:
// Editor::RenderProjectSettings owns their serialized shape (through the
// engine's AntiAliasingProjectSettings), so this page only decides what the
// user's choice means for the struct. Returns false when there is no
// workspace root to write to.
template <typename Mutate>
static bool EditProjectAASettings(Mutate&& mutate)
{
    const auto& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
    Rendering::AntiAliasingProjectSettings settings =
        Editor::LoadProjectAntiAliasingSettings(workspaceRoot);
    mutate(settings);
    return Editor::SaveProjectAntiAliasingSettings(workspaceRoot, settings);
}

// Any render scale above this counts as a deliberate supersample rather than a
// leftover slider value near native.
static constexpr float kSupersampleScaleThreshold = 1.01f;

// The Anti-Aliasing dropdown entry for a stored snapshot. SSAA is not a
// persisted mode — it IS engine mode Off with a supersampling render scale, so
// the entry is derived rather than stored.
static std::string AAModeDropdownValue(const Rendering::AntiAliasingProjectSettings& settings)
{
    if (settings.AAMode == Engine::Renderer::AntiAliasingMode::Off &&
        settings.RenderScale > kSupersampleScaleThreshold)
        return "ssaa";
    // Both FXAA modes share the one dropdown entry; the FXAA Frames row below
    // is where single-frame vs two-frame is chosen.
    if (settings.AAMode == Engine::Renderer::AntiAliasingMode::TemporalFXAA)
        return "fxaa";
    return Rendering::ToAAModeToken(settings.AAMode);
}

using Editor::TryMakeAssetRelativePathString;

std::string FormatPxValue(float px)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.0fpx", std::round(px));
    return buffer;
}

// Adds double-click-to-reset behavior to a label. The onReset callback fires on double-click.
void AddDoubleClickReset(Label* label, std::function<void()> onReset)
{
    if (!label || !onReset)
        return;

    auto lastClickTime = std::make_shared<std::chrono::steady_clock::time_point>();
    label->RegisterEventHandler(kEventMouseUp,
        [lastClickTime, onReset](UIEvent& e) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *lastClickTime);
            if (elapsed < GameEngine::Platform::GetDoubleClickInterval())
            {
                onReset();
                *lastClickTime = {};
            }
            else
            {
                *lastClickTime = now;
            }
            e.Stop();
        });
}

EditorContext* g_SettingsUndoContext = nullptr;
std::weak_ptr<bool> g_SettingsPanelLifetime;

bool IsSettingsPanelAlive()
{
    auto alive = g_SettingsPanelLifetime.lock();
    return alive && *alive;
}

class SettingsValueCommand final : public Editor::IEditorCommand
{
public:
    SettingsValueCommand(std::string name, std::function<void()> undo, std::function<void()> redo)
        : m_Name(std::move(name)), m_Undo(std::move(undo)), m_Redo(std::move(redo))
    {
    }

    const char* GetName() const override { return m_Name.c_str(); }
    void Do() override { if (m_Redo) m_Redo(); }
    void Undo() override { if (m_Undo) m_Undo(); }
    void Redo() override { if (m_Redo) m_Redo(); }

private:
    std::string m_Name;
    std::function<void()> m_Undo;
    std::function<void()> m_Redo;
};

void CommitSettingsFloatUndo(EditorContext* context,
                             const char* actionName,
                             float before,
                             float after,
                             std::function<void(float)> apply)
{
    if (!context || !context->UndoRedo || !apply)
        return;

    if (std::abs(before - after) <= 0.0001f)
        return;

    context->UndoRedo->CommitAlreadyApplied(std::make_unique<SettingsValueCommand>(
        actionName ? actionName : "Change Setting",
        [apply, before]() { apply(before); },
        [apply, after]() { apply(after); }));
}

void CommitSettingsIntUndo(EditorContext* context,
                           const char* actionName,
                           int before,
                           int after,
                           std::function<void(int)> apply)
{
    if (!context || !context->UndoRedo || !apply)
        return;

    if (before == after)
        return;

    context->UndoRedo->CommitAlreadyApplied(std::make_unique<SettingsValueCommand>(
        actionName ? actionName : "Change Setting",
        [apply, before]() { apply(before); },
        [apply, after]() { apply(after); }));
}

void WireSettingsFloatLabelDrag(EditorContext* context,
                                Label* label,
                                FloatField* field,
                                const char* actionName,
                                float defaultValue,
                                float minValue,
                                float maxValue,
                                std::function<float(float)> normalize,
                                std::function<void(float)> applyValue)
{
    if (!label || !field || !applyValue)
        return;

    if (!normalize)
    {
        normalize = [minValue, maxValue](float v) {
            return std::clamp(v, minValue, maxValue);
        };
    }

    auto lastCommitted = std::make_shared<float>(normalize(field->GetValue()));
    auto undoName = std::make_shared<std::string>(actionName ? actionName : "Change Setting");
    field->SetValue(*lastCommitted);

    auto preview = [field, normalize, applyValue]() {
        const float value = normalize(field->GetValue());
        field->SetValue(value);
        applyValue(value);
    };

    auto commit = [context, field, undoName, normalize, applyValue, lastCommitted]() {
        const float before = *lastCommitted;
        const float after = normalize(field->GetValue());
        field->SetValue(after);
        applyValue(after);
        *lastCommitted = after;

        CommitSettingsFloatUndo(
            context,
            undoName->c_str(),
            before,
            after,
            [applyValue, normalize, lastCommitted](float value) {
                const float normalized = normalize(value);
                applyValue(normalized);
                *lastCommitted = normalized;
            });
    };

    field->SetOnValueChanging([preview](const float&) { preview(); });
    field->SetOnValueChanged([commit](const float&) { commit(); });

    InspectorDrag::SetupLabelDragFloat(
        label,
        field,
        preview,
        commit,
        normalize(defaultValue),
        minValue,
        maxValue);
}

void WireSettingsIntLabelDrag(EditorContext* context,
                              Label* label,
                              IntField* field,
                              const char* actionName,
                              int defaultValue,
                              int minValue,
                              int maxValue,
                              std::function<int(int)> normalize,
                              std::function<void(int)> applyValue)
{
    if (!label || !field || !applyValue)
        return;

    if (!normalize)
    {
        normalize = [minValue, maxValue](int v) {
            return std::clamp(v, minValue, maxValue);
        };
    }

    auto lastCommitted = std::make_shared<int>(normalize(field->GetValue()));
    auto undoName = std::make_shared<std::string>(actionName ? actionName : "Change Setting");
    field->SetValue(*lastCommitted);

    auto preview = [field, normalize, applyValue]() {
        const int value = normalize(field->GetValue());
        field->SetValue(value);
        applyValue(value);
    };

    auto commit = [context, field, undoName, normalize, applyValue, lastCommitted]() {
        const int before = *lastCommitted;
        const int after = normalize(field->GetValue());
        field->SetValue(after);
        applyValue(after);
        *lastCommitted = after;

        CommitSettingsIntUndo(
            context,
            undoName->c_str(),
            before,
            after,
            [applyValue, normalize, lastCommitted](int value) {
                const int normalized = normalize(value);
                applyValue(normalized);
                *lastCommitted = normalized;
            });
    };

    field->SetOnValueChanging([preview](const int&) { preview(); });
    field->SetOnValueChanged([commit](const int&) { commit(); });

    InspectorDrag::SetupLabelDragInt(
        label,
        field,
        preview,
        commit,
        normalize(defaultValue));
}

/// Build one of the independent SVG rasterization defaults. UI SVGs are
/// personal appearance state; texture import is a project-wide authoring rule.
static std::unique_ptr<UIElement> BuildSvgRasterSizeRow(Editor::SvgRasterSettingsScope scope)
{
    constexpr int kDefaultExp = 8;
    constexpr int kMinExp = 4;
    constexpr int kMaxExp = 14;

    auto row = std::make_unique<UIElement>();
    row->AddClass("settings-row");

    auto label = std::make_unique<Label>();
    label->SetText(scope == Editor::SvgRasterSettingsScope::EditorUi
        ? "UI SVG Raster Size"
        : "Default Raster Size");
    label->SetTooltip(scope == Editor::SvgRasterSettingsScope::EditorUi
        ? "Default raster bounds for SVGs shipped with the editor UI. Aspect ratio is preserved."
        : "Default raster bounds for project SVG textures without a per-asset override. Aspect ratio is preserved.");
    label->AddClass("settings-row-label");
    Label* labelPtr = label.get();

    const std::filesystem::path workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
    std::string loadError;
    const uint32 initialPixels = Editor::SvgRasterSettings::Load(scope, workspaceRoot, &loadError);
    if (!loadError.empty())
        Logger::Log::Warning("SvgRasterSettings: failed to load setting: {}", loadError);
    int exp = kMinExp;
    while (exp < kMaxExp && (1u << exp) < initialPixels)
        ++exp;

    std::vector<Dropdown::Option> options;
    options.reserve(static_cast<size_t>(kMaxExp - kMinExp + 1));
    for (int optionExp = kMinExp; optionExp <= kMaxExp; ++optionExp)
    {
        const int pixels = 1 << optionExp;
        const std::string value = std::to_string(pixels);
        options.push_back({value, value + "x" + value});
    }

    auto dropdown = std::make_unique<Dropdown>();
    dropdown->AddClass("settings-row-field");
    dropdown->SetAutoWidthPopup(true);
    dropdown->SetOptions(options, exp - kMinExp);
    Dropdown* dropdownPtr = dropdown.get();

    auto applySelection = [scope](const std::string& value) {
        uint32 pixels = 0;
        if (!ParseSvgRasterSizeMeta(value, pixels))
            return;
        std::string err;
        if (!Editor::SvgRasterSettings::SaveAndApply(
                scope, EngineCore::GetInstance().GetWorkspaceRoot(), pixels, true, &err))
            Logger::Log::Warning("SvgRasterSettings: failed to save setting: {}", err);
    };

    dropdown->SetOnValueChanged(applySelection);
    AddDoubleClickReset(labelPtr, [dropdownPtr, applySelection]() {
        const std::string defaultValue = std::to_string(1 << kDefaultExp);
        if (dropdownPtr->GetSelectedValue() == defaultValue)
            applySelection(defaultValue);
        else
            dropdownPtr->SetSelectedValue(defaultValue);
    });

    row->AddChild(std::move(label));
    row->AddChild(std::move(dropdown));
    return row;
}

// Helper struct for creating settings toggle rows with automatic reset-on-double-click
struct SettingsToggleConfig
{
    std::string labelText;
    bool defaultValue;
    std::string prefKey;
    std::function<void(bool)> onValueChanged;
    std::string extraRowClass; // optional: added to the .settings-row section element
    // When set, the toggle's initial state shows this live value instead of the
    // saved preference — for settings where an env override or runtime change
    // can make the actual state differ from what was persisted (e.g. VSync,
    // which GE_VSYNC can flip at launch).
    std::optional<bool> initialValueOverride;
    // Shown on the label; required when labelText is shortened to fit the
    // 37% label column — carry the full wording here.
    std::string tooltip;
};

// Creates a settings row with toggle and double-click-to-reset on the label
void CreateSettingsToggleRow(
    const SettingsToggleConfig& config,
    ScrollView* contentBody,
    Toggle** outToggle = nullptr)
{
    auto section = std::make_unique<UIElement>();
    section->AddClass("settings-row");
    if (!config.extraRowClass.empty())
        section->AddClass(config.extraRowClass);

    auto label = std::make_unique<Label>();
    label->SetText(config.labelText);
    if (!config.tooltip.empty())
        label->SetTooltip(config.tooltip);
    label->AddClass("settings-row-label");
    Label* labelPtr = label.get();

    auto toggle = std::make_unique<Toggle>();
    Toggle* togglePtr = toggle.get();
    toggle->AddClass("settings-toggle");

    // Load from preferences or use default. A live override (e.g. the actual
    // device VSync state, which an env var may have flipped at launch) wins so
    // the toggle reflects reality rather than a stale saved value.
    bool currentValue = config.defaultValue;
    if (!config.prefKey.empty())
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        prefs.TryGetBool(config.prefKey, currentValue);
    }
    if (config.initialValueOverride.has_value())
    {
        currentValue = *config.initialValueOverride;
    }
    
    toggle->SetChecked(currentValue);
    
    // Initial callback
    if (config.onValueChanged)
        config.onValueChanged(currentValue);

    // Wire up value changed
    std::string prefKey = config.prefKey;
    auto onChanged = config.onValueChanged;
    
    toggle->SetOnValueChanged([onChanged, prefKey](const bool& value) {
        if (onChanged)
            onChanged(value);
        // Save to preferences
        if (!prefKey.empty())
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool(prefKey, value);
            prefs.Save(&err);
        }
    });

    // Double-click on label resets to default
    bool defaultVal = config.defaultValue;
    AddDoubleClickReset(labelPtr, [togglePtr, onChanged, prefKey, defaultVal]() {
        togglePtr->SetChecked(defaultVal);
        if (onChanged)
            onChanged(defaultVal);
        if (!prefKey.empty())
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool(prefKey, defaultVal);
            prefs.Save(&err);
        }
    });

    section->AddChild(std::move(label));
    section->AddChild(std::move(toggle));
    if (outToggle)
        *outToggle = togglePtr;
    if (contentBody)
        contentBody->AddContent(std::move(section));
}

void CreateSettingsToggleRow(
    const SettingsToggleConfig& config,
    UIElement* parent,
    Toggle** outToggle = nullptr)
{
    auto section = std::make_unique<UIElement>();
    section->AddClass("settings-row");
    if (!config.extraRowClass.empty())
        section->AddClass(config.extraRowClass);

    auto label = std::make_unique<Label>();
    label->SetText(config.labelText);
    if (!config.tooltip.empty())
        label->SetTooltip(config.tooltip);
    label->AddClass("settings-row-label");
    Label* labelPtr = label.get();

    auto toggle = std::make_unique<Toggle>();
    Toggle* togglePtr = toggle.get();
    toggle->AddClass("settings-toggle");

    bool currentValue = config.defaultValue;
    if (!config.prefKey.empty())
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        prefs.TryGetBool(config.prefKey, currentValue);
    }
    toggle->SetChecked(currentValue);
    if (config.onValueChanged)
        config.onValueChanged(currentValue);

    std::string prefKey = config.prefKey;
    auto onChanged = config.onValueChanged;
    toggle->SetOnValueChanged([onChanged, prefKey](const bool& value) {
        if (onChanged)
            onChanged(value);
        if (!prefKey.empty())
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool(prefKey, value);
            prefs.Save(&err);
        }
    });

    bool defaultVal = config.defaultValue;
    AddDoubleClickReset(labelPtr, [togglePtr, onChanged, prefKey, defaultVal]() {
        togglePtr->SetChecked(defaultVal);
        if (onChanged)
            onChanged(defaultVal);
        if (!prefKey.empty())
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool(prefKey, defaultVal);
            prefs.Save(&err);
        }
    });

    section->AddChild(std::move(label));
    section->AddChild(std::move(toggle));
    if (outToggle)
        *outToggle = togglePtr;
    parent->AddChild(std::move(section));
}

// Helper struct for creating settings slider rows with automatic reset-on-double-click
struct SettingsSliderConfig
{
    std::string labelText;
    float defaultValue;
    std::optional<float> initialValue;
    float minValue;
    float maxValue;
    float step;
    std::string prefKey; // Preference key for persistence
    std::function<void(float)> onValueChanged;
    std::function<void(float)> onValueChanging;
    // Applies the initial value to live systems when the row is built. A row
    // that persists inside onValueChanged must supply this: the row seeds
    // itself from stored data, and handing that value back to the persisting
    // callback writes the file on page open. Rows the factory persists for
    // (prefKey set) leave it empty and get onValueChanged fired once instead.
    std::function<void(float)> onSeed;
    // Shown on the label; required when labelText is shortened to fit the
    // 37% label column — carry the full wording here.
    std::string tooltip;
};

struct SettingsSliderRow
{
    UIElement* section = nullptr;
    Slider* slider = nullptr;
    FloatField* valueField = nullptr;  // Editable value on the right of the slider
};

// Creates a settings row with slider and editable value on the right; double-click label resets to default
SettingsSliderRow CreateSettingsSliderRow(
    const SettingsSliderConfig& config,
    ScrollView* contentBody)
{
    SettingsSliderRow result;
    
    auto section = std::make_unique<UIElement>();
    section->AddClass("settings-row");
    result.section = section.get();

    auto label = std::make_unique<Label>();
    label->SetText(config.labelText);
    if (!config.tooltip.empty())
        label->SetTooltip(config.tooltip);
    label->AddClass("settings-row-label");
    Label* labelPtr = label.get();

    auto slider = std::make_unique<Slider>();
    result.slider = slider.get();
    slider->AddClass("settings-row-slider");
    slider->SetTrackPaddingPx(0.0f);
    slider->SetShowValueBubble(true);
    slider->SetMin(config.minValue);
    slider->SetMax(config.maxValue);
    slider->SetStep(config.step);

    auto valueField = std::make_unique<FloatField>();
    result.valueField = valueField.get();
    valueField->AddClass("settings-row-value");
    InspectorDrag::ApplySliderFloatValueFieldStyle(valueField.get());

    // Load from preferences or use default
    float currentValue = config.initialValue.value_or(config.defaultValue);
    if (!config.prefKey.empty())
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        double stored = currentValue;
        if (prefs.TryGetDouble(config.prefKey, stored))
            currentValue = static_cast<float>(stored);
    }
    currentValue = std::clamp(currentValue, config.minValue, config.maxValue);
    
    slider->SetValue(currentValue);
    valueField->SetValue(currentValue);
    
    // Seed live state from the stored value. Opening a page is not an edit, so
    // a row that persists inside onValueChanged routes its apply through
    // onSeed and is never asked to write here.
    if (config.onSeed)
        config.onSeed(currentValue);
    else if (config.onValueChanged)
        config.onValueChanged(currentValue);

    Slider* sliderPtr = slider.get();
    FloatField* valueFieldPtr = valueField.get();
    auto onChanging = config.onValueChanging;
    auto onChanged = config.onValueChanged;
    std::string prefKey = config.prefKey;
    const float minVal = config.minValue;
    const float maxVal = config.maxValue;
    const float defaultVal = config.defaultValue;
    const std::string undoName = "Change " + config.labelText;
    auto lastCommitted = std::make_shared<float>(currentValue);

    auto persistValue = [prefKey](float value) {
        if (!prefKey.empty())
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetDouble(prefKey, value);
            prefs.Save(&err);
        }
    };

    auto applyCommittedValue = [sliderPtr, valueFieldPtr, onChanged, persistValue, minVal, maxVal](float value) {
        const float clamped = std::clamp(value, minVal, maxVal);
        if (IsSettingsPanelAlive())
        {
            sliderPtr->SetValue(clamped);
            valueFieldPtr->SetValue(clamped);
        }
        if (onChanged)
            onChanged(clamped);
        persistValue(clamped);
        return clamped;
    };

    // Slider -> field (value on the right stays in sync). Only the lightweight
    // live callback runs per drag tick; onValueChanged is reserved for commit
    // (release/field entry), so persistence and undo work cannot stall the thumb.
    slider->SetOnValueChanging([valueFieldPtr, onChanging](const float& value) {
        valueFieldPtr->SetValue(value);
        if (onChanging)
            onChanging(value);
    });
    
    slider->SetOnValueChanged([applyCommittedValue, lastCommitted, undoName](const float& value) {
        const float before = *lastCommitted;
        const float after = applyCommittedValue(value);
        *lastCommitted = after;
        CommitSettingsFloatUndo(
            g_SettingsUndoContext,
            undoName.c_str(),
            before,
            after,
            [applyCommittedValue, lastCommitted](float undoValue) {
                *lastCommitted = applyCommittedValue(undoValue);
            });
    });

    // Editable field -> slider (live: move the slider visually as the user
    // types, without committing to preferences or firing onChanged)
    valueField->SetOnValueChanging([sliderPtr, minVal, maxVal](const float& value) {
        float clamped = std::max(minVal, std::min(maxVal, value));
        sliderPtr->SetValue(clamped);
    });

    // Editable field -> slider (on commit: parse, clamp, apply, save)
    valueField->SetOnValueChanged([applyCommittedValue, lastCommitted, undoName](const float& value) {
        const float before = *lastCommitted;
        const float after = applyCommittedValue(value);
        *lastCommitted = after;
        CommitSettingsFloatUndo(
            g_SettingsUndoContext,
            undoName.c_str(),
            before,
            after,
            [applyCommittedValue, lastCommitted](float undoValue) {
                *lastCommitted = applyCommittedValue(undoValue);
            });
    });

    InspectorDrag::SetupLabelDragSlider(labelPtr, sliderPtr, nullptr, nullptr, defaultVal);

    // Order: label | slider | value (value on the right, editable)
    section->AddChild(std::move(label));
    section->AddChild(std::move(slider));
    section->AddChild(std::move(valueField));
    
    if (contentBody)
        contentBody->AddContent(std::move(section));
    
    return result;
}

// Overload for UIElement parent (uses AddChild instead of AddContent)
SettingsSliderRow CreateSettingsSliderRow(
    const SettingsSliderConfig& config,
    UIElement* parent)
{
    SettingsSliderRow result;
    
    auto section = std::make_unique<UIElement>();
    section->AddClass("settings-row");
    result.section = section.get();

    auto label = std::make_unique<Label>();
    label->SetText(config.labelText);
    if (!config.tooltip.empty())
        label->SetTooltip(config.tooltip);
    label->AddClass("settings-row-label");
    Label* labelPtr = label.get();

    auto slider = std::make_unique<Slider>();
    result.slider = slider.get();
    slider->AddClass("settings-row-slider");
    slider->SetTrackPaddingPx(0.0f);
    slider->SetShowValueBubble(true);
    slider->SetMin(config.minValue);
    slider->SetMax(config.maxValue);
    slider->SetStep(config.step);

    auto valueField = std::make_unique<FloatField>();
    result.valueField = valueField.get();
    valueField->AddClass("settings-row-value");
    InspectorDrag::ApplySliderFloatValueFieldStyle(valueField.get());

    // Load from preferences or use default
    float currentValue = config.initialValue.value_or(config.defaultValue);
    if (!config.prefKey.empty())
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        double stored = currentValue;
        if (prefs.TryGetDouble(config.prefKey, stored))
            currentValue = static_cast<float>(stored);
    }
    currentValue = std::clamp(currentValue, config.minValue, config.maxValue);
    
    slider->SetValue(currentValue);
    valueField->SetValue(currentValue);
    
    // Seed live state from the stored value; see the ScrollView overload above.
    if (config.onSeed)
        config.onSeed(currentValue);
    else if (config.onValueChanged)
        config.onValueChanged(currentValue);

    Slider* sliderPtr = slider.get();
    FloatField* valueFieldPtr = valueField.get();
    auto onChanging = config.onValueChanging;
    auto onChanged = config.onValueChanged;
    std::string prefKey = config.prefKey;
    const float minVal = config.minValue;
    const float maxVal = config.maxValue;
    const float defaultVal = config.defaultValue;
    const std::string undoName = "Change " + config.labelText;
    auto lastCommitted = std::make_shared<float>(currentValue);

    auto persistValue = [prefKey](float value) {
        if (!prefKey.empty())
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetDouble(prefKey, value);
            prefs.Save(&err);
        }
    };

    auto applyCommittedValue = [sliderPtr, valueFieldPtr, onChanged, persistValue, minVal, maxVal](float value) {
        const float clamped = std::clamp(value, minVal, maxVal);
        if (IsSettingsPanelAlive())
        {
            sliderPtr->SetValue(clamped);
            valueFieldPtr->SetValue(clamped);
        }
        if (onChanged)
            onChanged(clamped);
        persistValue(clamped);
        return clamped;
    };

    slider->SetOnValueChanging([valueFieldPtr, onChanging](const float& value) {
        valueFieldPtr->SetValue(value);
        if (onChanging)
            onChanging(value);
    });
    
    slider->SetOnValueChanged([applyCommittedValue, lastCommitted, undoName](const float& value) {
        const float before = *lastCommitted;
        const float after = applyCommittedValue(value);
        *lastCommitted = after;
        CommitSettingsFloatUndo(
            g_SettingsUndoContext,
            undoName.c_str(),
            before,
            after,
            [applyCommittedValue, lastCommitted](float undoValue) {
                *lastCommitted = applyCommittedValue(undoValue);
            });
    });

    valueField->SetOnValueChanging([sliderPtr, minVal, maxVal](const float& value) {
        float clamped = std::clamp(value, minVal, maxVal);
        sliderPtr->SetValue(clamped);
    });

    valueField->SetOnValueChanged([applyCommittedValue, lastCommitted, undoName](const float& value) {
        const float before = *lastCommitted;
        const float after = applyCommittedValue(value);
        *lastCommitted = after;
        CommitSettingsFloatUndo(
            g_SettingsUndoContext,
            undoName.c_str(),
            before,
            after,
            [applyCommittedValue, lastCommitted](float undoValue) {
                *lastCommitted = applyCommittedValue(undoValue);
            });
    });

    InspectorDrag::SetupLabelDragSlider(labelPtr, sliderPtr, nullptr, nullptr, defaultVal);

    section->AddChild(std::move(label));
    section->AddChild(std::move(slider));
    section->AddChild(std::move(valueField));
    
    if (parent)
        parent->AddChild(std::move(section));
    
    return result;
}

}

// ============================================================================
// SettingsTreeDataProvider Implementation
// ============================================================================

SettingsTreeDataProvider::SettingsTreeDataProvider()
{
    // Build the tree structure
    // Two root categories shown at top level
    
    // Project Settings with children
    m_Nodes[static_cast<TreeId>(SettingsCategory::ProjectSettings)] = {
        "Project Settings",
        {
            static_cast<TreeId>(SettingsCategory::AudioSettings),
            static_cast<TreeId>(SettingsCategory::Camera),
            static_cast<TreeId>(SettingsCategory::Animation),
            static_cast<TreeId>(SettingsCategory::Input),
            static_cast<TreeId>(SettingsCategory::Physics),
            static_cast<TreeId>(SettingsCategory::AssetImport),
            static_cast<TreeId>(SettingsCategory::Rendering),
            static_cast<TreeId>(SettingsCategory::HDROutput),
            static_cast<TreeId>(SettingsCategory::Scene),
            static_cast<TreeId>(SettingsCategory::Tags),
            static_cast<TreeId>(SettingsCategory::VersionControl),
        },
        true  // has children, expandable
    };
    
    // Version Control children are dynamic: one tab per registered VCS
    // provider (EditorVcsProviderRegistry), refreshed on every tree query so
    // package-provided tabs appear after project open.
    m_Nodes[static_cast<TreeId>(SettingsCategory::VersionControl)] = {
        "Version Control",
        {},
        true  // has children, expandable
    };
    RefreshVcsProviderNodes();
    
    // User Settings with children
    m_Nodes[static_cast<TreeId>(SettingsCategory::UserSettings)] = {
        "User Settings",
        {
            static_cast<TreeId>(SettingsCategory::UI),
            static_cast<TreeId>(SettingsCategory::Script),
            static_cast<TreeId>(SettingsCategory::Shortcuts),
            static_cast<TreeId>(SettingsCategory::Gizmo),
            static_cast<TreeId>(SettingsCategory::GridAndSnapping),
            static_cast<TreeId>(SettingsCategory::Performance),
        },
        true  // has children, expandable
    };
    
    // Project Settings children (leaf nodes)
    m_Nodes[static_cast<TreeId>(SettingsCategory::AudioSettings)] = {"Audio Settings", {}, false};
    m_Nodes[static_cast<TreeId>(SettingsCategory::Physics)] = {"Physics", {}, false};
    m_Nodes[static_cast<TreeId>(SettingsCategory::Camera)] = {"Scene View", {}, false};
    m_Nodes[static_cast<TreeId>(SettingsCategory::Animation)] = {"Animation", {}, false};
    m_Nodes[static_cast<TreeId>(SettingsCategory::Input)] = {"Input", {}, false};
    m_Nodes[static_cast<TreeId>(SettingsCategory::AssetImport)] = {"Asset Import", {}, false};
    m_Nodes[static_cast<TreeId>(SettingsCategory::Rendering)] = {"Rendering", {}, false};
    m_Nodes[static_cast<TreeId>(SettingsCategory::HDROutput)] = {"HDR Output", {}, false};
    m_Nodes[static_cast<TreeId>(SettingsCategory::Scene)] = {"Scene", {}, false};
    m_Nodes[static_cast<TreeId>(SettingsCategory::Tags)] = {"Tags", {}, false};
        // User Settings children
    m_Nodes[static_cast<TreeId>(SettingsCategory::Script)] = {"Script", {}, false};
    m_Nodes[static_cast<TreeId>(SettingsCategory::UI)] = {
        "UI",
        {
            static_cast<TreeId>(SettingsCategory::UIAppearance),
            static_cast<TreeId>(SettingsCategory::UIFontSizes),
            static_cast<TreeId>(SettingsCategory::UITrees),
            static_cast<TreeId>(SettingsCategory::UIAssets),
            static_cast<TreeId>(SettingsCategory::UIInspector),
            static_cast<TreeId>(SettingsCategory::UIHiDpi),
        },
        true
    };
    m_Nodes[static_cast<TreeId>(SettingsCategory::UIAppearance)] = {"Appearance", {}, false};
    m_Nodes[static_cast<TreeId>(SettingsCategory::UIFontSizes)] = {"Font Rendering", {}, false};
    m_Nodes[static_cast<TreeId>(SettingsCategory::UITrees)] = {"Trees", {}, false};
    m_Nodes[static_cast<TreeId>(SettingsCategory::UIAssets)] = {"Assets", {}, false};
    m_Nodes[static_cast<TreeId>(SettingsCategory::UIInspector)] = {"Inspector", {}, false};
    m_Nodes[static_cast<TreeId>(SettingsCategory::UIHiDpi)] = {"HiDPI", {}, false};
    m_Nodes[static_cast<TreeId>(SettingsCategory::Shortcuts)] = {"Keyboard Shortcuts", {}, false};
    m_Nodes[static_cast<TreeId>(SettingsCategory::Gizmo)] = {"Gizmo", {}, false};
    m_Nodes[static_cast<TreeId>(SettingsCategory::GridAndSnapping)] = {"Grid & Snapping", {}, false};
    m_Nodes[static_cast<TreeId>(SettingsCategory::Performance)] = {"Performance", {}, false};

    // Root items shown at top level of tree
    m_Roots = {
        static_cast<TreeId>(SettingsCategory::ProjectSettings),
        static_cast<TreeId>(SettingsCategory::UserSettings)
    };

    // Sort all children and roots alphabetically by label, except User Settings —
    // there UI is pinned as the first child and the rest follow alphabetically.
    // RefreshVcsProviderNodes mutates m_Nodes, so the comparator must not call
    // GetLabel: comparing two dynamic VCS nodes would refresh twice and could
    // invalidate the first returned c_str() before strcmp reads it.
    auto byLabel = [this](TreeId a, TreeId b)
    {
        const auto aIt = m_Nodes.find(a);
        const auto bIt = m_Nodes.find(b);
        const std::string_view aLabel = aIt != m_Nodes.end() ? aIt->second.Label : std::string_view{};
        const std::string_view bLabel = bIt != m_Nodes.end() ? bIt->second.Label : std::string_view{};
        return aLabel < bLabel;
    };
    const TreeId userSettingsId = static_cast<TreeId>(SettingsCategory::UserSettings);
    const TreeId uiId = static_cast<TreeId>(SettingsCategory::UI);
    for (auto& pair : m_Nodes)
    {
        if (pair.first == userSettingsId)
        {
            auto& children = pair.second.Children;
            auto uiIt = std::find(children.begin(), children.end(), uiId);
            if (uiIt != children.end())
                children.erase(uiIt);
            std::sort(children.begin(), children.end(), byLabel);
            children.insert(children.begin(), uiId);
        }
        else
        {
            std::sort(pair.second.Children.begin(), pair.second.Children.end(), byLabel);
        }
    }
    std::sort(m_Roots.begin(), m_Roots.end(), byLabel);

    // Categories registered through Editor::EditorSettingsRegistry merge into
    // the root children on every refresh; capture the hand-built children
    // first so refreshes can recompose them.
    m_LegacyRootChildren[static_cast<TreeId>(SettingsCategory::ProjectSettings)] =
        m_Nodes[static_cast<TreeId>(SettingsCategory::ProjectSettings)].Children;
    m_LegacyRootChildren[static_cast<TreeId>(SettingsCategory::UserSettings)] =
        m_Nodes[static_cast<TreeId>(SettingsCategory::UserSettings)].Children;
    m_LegacyRootChildren[static_cast<TreeId>(SettingsCategory::UI)] =
        m_Nodes[static_cast<TreeId>(SettingsCategory::UI)].Children;
    RefreshRegistryNodes();
}

int SettingsTreeDataProvider::GetRootCount() const
{
    return static_cast<int>(m_Roots.size());
}

TreeId SettingsTreeDataProvider::GetRootId(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_Roots.size()))
        return 0;
    return m_Roots[index];
}

void SettingsTreeDataProvider::RefreshVcsProviderNodes() const
{
    const auto providers = Editor::EditorVcsProviderRegistry::Get().Snapshot();
    const size_t count =
        std::min(providers.size(), static_cast<size_t>(kVcsProviderCategoryMaxCount));

    std::vector<TreeId> children;
    children.reserve(count);
    for (size_t i = 0; i < count; ++i)
        children.push_back(static_cast<TreeId>(kVcsProviderCategoryBase + i));

    // Provider tabs are this root's hand-built half; RefreshRegistryNodes
    // composes them with any provider-neutral pages registered against the
    // Version Control group.
    auto& providerChildren =
        m_LegacyRootChildren[static_cast<TreeId>(SettingsCategory::VersionControl)];
    if (providerChildren != children)
    {
        for (TreeId stale : providerChildren)
            m_Nodes.erase(stale);
        providerChildren = children;
    }
    // Labels can change on module replace-forward; refresh unconditionally.
    for (size_t i = 0; i < count; ++i)
        m_Nodes[children[i]] = {providers[i].DisplayName, {}, false};
    RefreshRegistryNodes();
}

void SettingsTreeDataProvider::RefreshRegistryNodes() const
{
    const auto categories = Editor::EditorSettingsRegistry::Get().Snapshot();
    const size_t count =
        std::min(categories.size(), static_cast<size_t>(kRegistrySettingsCategoryMaxCount));

    std::unordered_map<std::string_view, TreeId> idByCategoryId;
    m_RegistryRowClasses.resize(count);
    for (size_t i = 0; i < count; ++i)
    {
        idByCategoryId.emplace(categories[i].CategoryId,
                               static_cast<TreeId>(kRegistrySettingsCategoryBase + i));
        m_RegistryRowClasses[i] = categories[i].TreeRowClass;
    }

    // A category with a ParentCategoryId hangs under that page instead of the
    // group root. An unresolved parent is not an error — the parent may simply
    // not have registered yet — so the child falls back to the group root and
    // moves the next time this runs.
    std::unordered_map<TreeId, std::vector<TreeId>> registeredPerRoot;
    std::unordered_map<TreeId, std::vector<TreeId>> registeredPerParent;
    for (size_t i = 0; i < count; ++i)
    {
        const TreeId id = static_cast<TreeId>(kRegistrySettingsCategoryBase + i);
        // Labels can change on module replace-forward; refresh unconditionally.
        m_Nodes[id] = {categories[i].Title, {}, false};
        // VersionControl- and UIAppearance-group categories render inline on
        // their host pages rather than as tree children; they contribute no node.
        if (categories[i].Group == Editor::SettingsCategoryGroup::VersionControl ||
            categories[i].Group == Editor::SettingsCategoryGroup::UIAppearance)
            continue;

        TreeId parent = 0;
        if (categories[i].Group == Editor::SettingsCategoryGroup::UI)
            parent = static_cast<TreeId>(SettingsCategory::UI);
        if (!categories[i].ParentCategoryId.empty())
        {
            const auto parentIt = idByCategoryId.find(categories[i].ParentCategoryId);
            if (parentIt != idByCategoryId.end() && parentIt->second != id)
                parent = parentIt->second;
        }
        if (parent != 0)
        {
            // UI-group pages compose with the hand-built UI children (Appearance,
            // Font, …) via m_LegacyRootChildren[UI], same as root merge. Other
            // ParentCategoryId pages replace that parent's children.
            if (parent == static_cast<TreeId>(SettingsCategory::UI) &&
                categories[i].ParentCategoryId.empty())
            {
                registeredPerRoot[parent].push_back(id);
                continue;
            }
            registeredPerParent[parent].push_back(id);
            continue;
        }

        const TreeId root = static_cast<TreeId>(
            categories[i].Group == Editor::SettingsCategoryGroup::ProjectSettings
                ? SettingsCategory::ProjectSettings
                : SettingsCategory::UserSettings);
        registeredPerRoot[root].push_back(id);
    }

    // Recompose each root's children: hand-built + registered, re-sorted with
    // the construction-time rules (UI stays pinned first under User Settings).
    auto byLabel = [this](TreeId a, TreeId b)
    {
        const auto aIt = m_Nodes.find(a);
        const auto bIt = m_Nodes.find(b);
        const std::string_view aLabel = aIt != m_Nodes.end() ? aIt->second.Label : std::string_view{};
        const std::string_view bLabel = bIt != m_Nodes.end() ? bIt->second.Label : std::string_view{};
        return aLabel < bLabel;
    };
    const TreeId uiId = static_cast<TreeId>(SettingsCategory::UI);
    for (const auto& [root, legacy] : m_LegacyRootChildren)
    {
        std::vector<TreeId> children = legacy;
        if (const auto registered = registeredPerRoot.find(root); registered != registeredPerRoot.end())
            children.insert(children.end(), registered->second.begin(), registered->second.end());
        const bool pinUi = root == static_cast<TreeId>(SettingsCategory::UserSettings);
        if (pinUi)
        {
            if (auto uiIt = std::find(children.begin(), children.end(), uiId); uiIt != children.end())
                children.erase(uiIt);
        }
        std::sort(children.begin(), children.end(), byLabel);
        if (pinUi)
            children.insert(children.begin(), uiId);
        m_Nodes[root].Children = std::move(children);
    }

    // Registered sub-pages: the parent becomes expandable and owns them in
    // registration order, which is how the owning module orders its platforms
    // or providers — alphabetical sorting belongs to the group roots only.
    for (auto& [parent, children] : registeredPerParent)
    {
        auto& node = m_Nodes[parent];
        node.Children = std::move(children);
        node.Expandable = !node.Children.empty();
    }
}

int SettingsTreeDataProvider::GetChildCount(TreeId parent) const
{
    if (parent == static_cast<TreeId>(SettingsCategory::VersionControl))
        RefreshVcsProviderNodes();
    if (parent == static_cast<TreeId>(SettingsCategory::ProjectSettings) ||
        parent == static_cast<TreeId>(SettingsCategory::UserSettings) ||
        parent == static_cast<TreeId>(SettingsCategory::UI) ||
        IsRegistrySettingsCategory(static_cast<SettingsCategory>(parent)))
        RefreshRegistryNodes();
    auto it = m_Nodes.find(parent);
    if (it == m_Nodes.end())
        return 0;
    return static_cast<int>(it->second.Children.size());
}

TreeId SettingsTreeDataProvider::GetChildId(TreeId parent, int index) const
{
    if (parent == static_cast<TreeId>(SettingsCategory::VersionControl))
        RefreshVcsProviderNodes();
    if (parent == static_cast<TreeId>(SettingsCategory::ProjectSettings) ||
        parent == static_cast<TreeId>(SettingsCategory::UserSettings) ||
        parent == static_cast<TreeId>(SettingsCategory::UI) ||
        IsRegistrySettingsCategory(static_cast<SettingsCategory>(parent)))
        RefreshRegistryNodes();
    auto it = m_Nodes.find(parent);
    if (it == m_Nodes.end())
        return 0;
    if (index < 0 || index >= static_cast<int>(it->second.Children.size()))
        return 0;
    return it->second.Children[index];
}

const char* SettingsTreeDataProvider::GetLabel(TreeId id) const
{
    if (IsVcsProviderCategory(static_cast<SettingsCategory>(id)))
        RefreshVcsProviderNodes();
    if (IsRegistrySettingsCategory(static_cast<SettingsCategory>(id)))
        RefreshRegistryNodes();
    auto it = m_Nodes.find(id);
    if (it == m_Nodes.end())
        return "";
    return it->second.Label.c_str();
}

bool SettingsTreeDataProvider::IsExpandable(TreeId id) const
{
    if (IsRegistrySettingsCategory(static_cast<SettingsCategory>(id)))
        RefreshRegistryNodes();
    auto it = m_Nodes.find(id);
    if (it == m_Nodes.end())
        return false;
    return it->second.Expandable;
}

std::unordered_set<TreeId> SettingsTreeDataProvider::GetAncestorIdsToExpandForSearch(const std::string& query) const
{
    std::unordered_set<TreeId> toExpand;
    if (query.empty())
        return toExpand;
    std::string lowerQuery = query;
    for (char& c : lowerQuery) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // Build parent map (child id -> parent id)
    std::unordered_map<TreeId, TreeId> parentOf;
    for (const auto& pair : m_Nodes)
    {
        for (TreeId childId : pair.second.Children)
            parentOf[childId] = pair.first;
    }
    for (TreeId rootId : m_Roots)
        parentOf[rootId] = 0; // roots have no parent

    // Find all node ids whose label contains the query
    std::vector<TreeId> matches;
    for (const auto& pair : m_Nodes)
    {
        std::string lowerLabel = pair.second.Label;
        for (char& c : lowerLabel) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lowerLabel.find(lowerQuery) != std::string::npos)
            matches.push_back(pair.first);
    }

    // For each match, add all ancestors to toExpand so the match is visible
    for (TreeId id : matches)
    {
        for (TreeId p = parentOf.count(id) ? parentOf.find(id)->second : 0; p != 0; )
        {
            toExpand.insert(p);
            auto it = parentOf.find(p);
            p = (it != parentOf.end()) ? it->second : 0;
        }
    }
    // Also expand matching nodes that have children so their children are visible
    for (TreeId id : matches)
    {
        if (IsExpandable(id))
            toExpand.insert(id);
    }
    return toExpand;
}

std::unordered_set<TreeId> SettingsTreeDataProvider::GetAncestorIdsToExpandForNodes(const std::unordered_set<TreeId>& nodeIds) const
{
    std::unordered_set<TreeId> toExpand;
    if (nodeIds.empty())
        return toExpand;
    // Build parent map (child id -> parent id)
    std::unordered_map<TreeId, TreeId> parentOf;
    for (const auto& pair : m_Nodes)
    {
        for (TreeId childId : pair.second.Children)
            parentOf[childId] = pair.first;
    }
    for (TreeId rootId : m_Roots)
        parentOf[rootId] = 0;
    // For each node, add all ancestors so the node is visible
    for (TreeId id : nodeIds)
    {
        for (TreeId p = parentOf.count(id) ? parentOf.find(id)->second : 0; p != 0; )
        {
            toExpand.insert(p);
            auto it = parentOf.find(p);
            p = (it != parentOf.end()) ? it->second : 0;
        }
    }
    return toExpand;
}

void SettingsTreeDataProvider::AppendCategoriesMatchingTreeLabels(const std::string& lowerQuery,
                                                                std::vector<SettingsCategory>& outCategories,
                                                                std::unordered_set<SettingsCategory>& seen) const
{
    if (lowerQuery.empty())
        return;

    std::function<void(TreeId)> visit = [&](TreeId id)
    {
        auto it = m_Nodes.find(id);
        if (it == m_Nodes.end())
            return;
        std::string lowerLabel = it->second.Label;
        for (char& c : lowerLabel)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lowerLabel.find(lowerQuery) != std::string::npos)
        {
            const auto cat = static_cast<SettingsCategory>(id);
            if (seen.insert(cat).second)
                outCategories.push_back(cat);
        }
        for (TreeId childId : it->second.Children)
            visit(childId);
    };

    for (TreeId rootId : m_Roots)
        visit(rootId);
}

// ============================================================================
// SettingsPanel Implementation
// ============================================================================

SettingsPanel::SettingsPanel()
    : DockPanel("Settings")
    , m_UIAccentColor(UI::AccentStyleHelper::kDefaultAccentColor)
{
    g_SettingsPanelLifetime = m_LifetimeToken;
    AddClass("settings-panel");

    // Create data providers
    m_TreeDataProvider = std::make_unique<SettingsTreeDataProvider>();
    m_TreeSelectionModel = std::make_unique<UI::Interaction::SelectionModel>();
    // Default selection is the UI > Appearance category.
    m_TreeSelectionModel->SetSingle(static_cast<TreeId>(SettingsCategory::UIAppearance));
    // Populate searchable items so search (e.g. "font") can open the right section and highlight.
    RegisterSearchableItems();

    // Categories registered through Editor::EditorSettingsRegistry can arrive
    // after construction (package modules load at project open). The observer
    // replays existing registrations on attach, so ordering never matters.
    Editor::EditorSettingsRegistry::Get().SetRegistrationObserver(
        [this, weak = std::weak_ptr<bool>(m_LifetimeToken)](const Editor::SettingsCategoryDescriptor&)
        {
            const auto alive = weak.lock();
            if (!alive || !*alive)
                return;
            RegisterSearchableItems();
            if (m_TreeView)
                m_TreeView->RefreshFromProvider();
        });

    m_SceneViewCameraSettingsListenerId =
        Editor::SceneViewSettings::Get().AddCameraSettingsChangedListener([this]()
        {
            SyncCameraSettingsControls();
        });

    // Restore user-set vertical tree pane height (0 = auto-size to content).
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        double stored = 0.0;
        if (prefs.TryGetDouble("ui.settings.treePaneHeightPx", stored) && stored > 0.0)
            m_UserTreePaneHeightPx = static_cast<float>(stored);
    }
}

SettingsPanel::~SettingsPanel()
{
    Editor::SceneViewSettings::Get().RemoveCameraSettingsChangedListener(
        m_SceneViewCameraSettingsListenerId);
    if (m_LifetimeToken)
        *m_LifetimeToken = false;
    if (g_SettingsUndoContext == m_EditorContext)
        g_SettingsUndoContext = nullptr;

    // Ensure we don't leave stale pointers in the global registry when this panel is destroyed.
    if (m_SearchBar)
        UnregisterSearchBar(m_SearchBar);
}

void SettingsPanel::CreateTreeView(UIElement* parent)
{
    auto treePane = std::make_unique<UIElement>();
    treePane->AddClass("settings-tree-pane");
    if (m_CurrentLayoutMode == LayoutMode::Horizontal)
    {
        treePane->AddClass("settings-tree-pane-horizontal");
    }
    m_TreePane = treePane.get();
    
    auto tree = std::make_unique<TreeView>();
    m_TreeView = tree.get();
    m_TreeView->AddClass("tree");
    m_TreeView->SetShowRoot(true);  // Show the root items (Project Settings, User Settings)
    m_TreeView->SetSelectionModel(m_TreeSelectionModel.get());
    m_TreeView->SetDataProvider(m_TreeDataProvider.get());
    constexpr float kSettingsNavigationTreeIconSizePx = 20.0f;
    ApplyTreeTitleIconLayoutVars(m_TreeView, kSettingsNavigationTreeIconSizePx);
    m_TreeView->SetIconSize(kSettingsNavigationTreeIconSizePx);
    // Category picker: arrow keys always move single selection, no Shift-range.
    m_TreeView->SetKeyboardSingleSelectOnly(true);
    
    // Handle selection changes
    m_TreeView->SetOnSelectionChanged([this](TreeId id) {
        OnCategorySelected(id);
    });
    
    // Add custom icon classes based on category
    auto getIconClass = [](TreeId id, const std::vector<std::string>& registryRowClasses) -> std::string {
        if (id == static_cast<TreeId>(SettingsCategory::ProjectSettings)) return "project-settings-row";
        if (id == static_cast<TreeId>(SettingsCategory::UserSettings)) return "user-settings-row";
        if (id == static_cast<TreeId>(SettingsCategory::AudioSettings)) return "audio-settings-row";
        if (id == static_cast<TreeId>(SettingsCategory::Camera)) return "camera-row";
        if (id == static_cast<TreeId>(SettingsCategory::Animation)) return "animation-row";
        if (id == static_cast<TreeId>(SettingsCategory::Input)) return "input-row";
        if (id == static_cast<TreeId>(SettingsCategory::Physics)) return "physics-row";
        if (id == static_cast<TreeId>(SettingsCategory::AssetImport)) return "asset-import-row";
        if (id == static_cast<TreeId>(SettingsCategory::Rendering)) return "rendering-row";
        if (id == static_cast<TreeId>(SettingsCategory::HDROutput)) return "hdr-row";
        if (id == static_cast<TreeId>(SettingsCategory::Scene)) return "scene-row";
        if (id == static_cast<TreeId>(SettingsCategory::Tags)) return "tags-row";
        if (id == static_cast<TreeId>(SettingsCategory::VersionControl)) return "version-control-row";
        if (IsVcsProviderCategory(static_cast<SettingsCategory>(id)))
        {
            const size_t index = static_cast<size_t>(id - kVcsProviderCategoryBase);
            const auto providers = Editor::EditorVcsProviderRegistry::Get().Snapshot();
            if (index < providers.size() && !providers[index].SettingsRowClass.empty())
            {
                static std::string s_RowClass;
                s_RowClass = providers[index].SettingsRowClass;
                return s_RowClass.c_str();
            }
            return "version-control-row";
        }
        if (id == static_cast<TreeId>(SettingsCategory::Script)) return "script-row";
        if (id == static_cast<TreeId>(SettingsCategory::UI)) return "ui-row";
        if (id == static_cast<TreeId>(SettingsCategory::UIAppearance)) return "ui-appearance-row";
        if (id == static_cast<TreeId>(SettingsCategory::UIFontSizes)) return "ui-font-rendering-row";
        if (id == static_cast<TreeId>(SettingsCategory::UITrees)) return "ui-trees-row";
        if (id == static_cast<TreeId>(SettingsCategory::UIAssets)) return "ui-assets-row";
        if (id == static_cast<TreeId>(SettingsCategory::UIInspector)) return "ui-inspector-row";
        if (id == static_cast<TreeId>(SettingsCategory::UIHiDpi)) return "ui-hidpi-row";
        if (id == static_cast<TreeId>(SettingsCategory::Shortcuts)) return "shortcuts-row";
        if (id == static_cast<TreeId>(SettingsCategory::Gizmo)) return "gizmo-row";
        if (id == static_cast<TreeId>(SettingsCategory::GridAndSnapping)) return "grid-snapping-row";
        if (id == static_cast<TreeId>(SettingsCategory::Performance)) return "performance-row";
        if (IsRegistrySettingsCategory(static_cast<SettingsCategory>(id)))
        {
            // Registry categories are created after the static Settings UXML
            // is loaded, so their owner supplies the row class at runtime.
            // The class only selects the image; the image itself remains
            // authored in SettingsPanel.css.
            const size_t index = static_cast<size_t>(id - kRegistrySettingsCategoryBase);
            if (index < registryRowClasses.size())
                return registryRowClasses[index];
        }
        return {};
    };
    
    m_TreeView->SetOnRowBound([this, getIconClass](TreeId id, UIElement* row) {
        if (!row) return;

        // TreeView recycles row elements across different categories. Always
        // strip any previous icon class on bind — otherwise a row pool slot
        // can carry over e.g. `user-settings-row` (which paints bold white)
        // onto an unrelated category like Online Assets. The built-in
        // categories' and the VCS provider tabs' classes are listed here;
        // registry categories' classes come from the data provider's cached
        // registry list, so a registered page needs no entry here.
        static const char* iconClasses[] = {
            "project-settings-row", "user-settings-row", "audio-settings-row",
            "camera-row", "animation-row", "input-row", "asset-import-row", "rendering-row", "hdr-row",
            "scene-row", "tags-row",
            "version-control-row", "svn-row", "git-row", "diversion-row", "lore-row",
            "physics-row", "script-row", "ui-row", "ui-appearance-row", "ui-font-rendering-row", "ui-trees-row",
            "ui-assets-row", "ui-inspector-row", "ui-hidpi-row",
            "shortcuts-row", "gizmo-row", "grid-snapping-row", "performance-row"
        };
        const std::vector<std::string>& registryRowClasses = m_TreeDataProvider->GetRegistryRowClasses();
        const std::string newClass = getIconClass(id, registryRowClasses);
        for (const char* cls : iconClasses)
        {
            if (cls == newClass) continue;
            if (row->HasClass(cls))
                row->RemoveClass(cls);
        }
        for (const std::string& rowClass : registryRowClasses)
        {
            if (rowClass.empty() || rowClass == newClass) continue;
            if (row->HasClass(rowClass))
                row->RemoveClass(rowClass);
        }
        if (!newClass.empty() && !row->HasClass(newClass))
            row->AddClass(newClass);
        // Inspector-style search: highlight tree row if label contains current query
        if (m_TreeDataProvider)
        {
            if (m_CurrentSearchQuery.empty())
                row->RemoveClass("search-match");
            else
            {
                const char* label = m_TreeDataProvider->GetLabel(id);
                if (label && label[0] != '\0')
                {
                    std::string lowerLabel = label;
                    std::string lowerQuery = m_CurrentSearchQuery;
                    for (char& c : lowerLabel) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    for (char& c : lowerQuery) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (lowerLabel.find(lowerQuery) != std::string::npos)
                        row->AddClass("search-match");
                    else
                        row->RemoveClass("search-match");
                }
                else
                    row->RemoveClass("search-match");
            }
        }
    });
    
    // Expand the branches leading to the default selection (UI > Appearance)
    // so the highlighted row is actually visible when Settings first opens.
    m_TreeView->RefreshFromProvider();
    m_TreeView->SetExpanded(static_cast<TreeId>(SettingsCategory::ProjectSettings), true);
    m_TreeView->SetExpanded(static_cast<TreeId>(SettingsCategory::UserSettings), true);
    m_TreeView->SetExpanded(static_cast<TreeId>(SettingsCategory::UI), true);
    m_TreeView->RefreshFromProvider();

    // Forward arrow/page/home/end keys to the category tree from anywhere in the
    // Settings panel. Key events bubble from the hovered element, so without this
    // a user hovering over the content pane would get no tree navigation until
    // they moved the cursor into the left tree pane.
    TreeView* treePtr = m_TreeView;
    this->RegisterEventHandler(kEventKeyDown, [treePtr](UIEvent& e) {
        if (!treePtr)
            return;
        // Don't steal arrows from text fields (search bar, inline rename, etc.).
        for (UIElement* p = e.Target; p; p = p->GetParent())
        {
            if (dynamic_cast<TextField*>(p))
                return;
        }
        switch (e.Key)
        {
            case Input::kKeyCode_Up:
            case Input::kKeyCode_Down:
            case Input::kKeyCode_Left:
            case Input::kKeyCode_Right:
            case Input::kKeyCode_PageUp:
            case Input::kKeyCode_PageDown:
            case Input::kKeyCode_Home:
            case Input::kKeyCode_End:
                break;
            default:
                return;
        }
        treePtr->OnEvent(e);
    });

    treePane->AddChild(std::move(tree));
    parent->AddChild(std::move(treePane));
}

void SettingsPanel::CreateContentPane(UIElement* parent)
{
    auto contentPane = std::make_unique<UIElement>();
    contentPane->AddClass("settings-content-pane");
    if (m_CurrentLayoutMode == LayoutMode::Horizontal)
    {
        contentPane->AddClass("settings-content-pane-horizontal");
    }
    m_ContentPane = contentPane.get();
    
    // Content header
    auto contentHeader = std::make_unique<Label>();
    contentHeader->AddClass("settings-content-header");
    contentHeader->SetText("Select a category");
    m_ContentHeader = contentHeader.get();
    contentPane->AddChild(std::move(contentHeader));
    
    // Content body (scrollable area for settings)
    auto contentBody = std::make_unique<ScrollView>();
    m_ContentBody = contentBody.get();
    contentBody->AddClass("settings-content-body");
    contentPane->AddChild(std::move(contentBody));
    
    parent->AddChild(std::move(contentPane));
}

void SettingsPanel::CreateSearchBar(UIElement* parent)
{
    auto searchBar = std::make_unique<UIElement>();
    searchBar->AddClass("settings-search-bar");
    
    m_SearchBar = searchBar.get();
    
    // Capture search bar pointer for callbacks
    UIElement* searchBarPtr = m_SearchBar;
    
    // Create text field first so it's on top
    auto searchField = std::make_unique<TextField>();
    m_SearchField = searchField.get();
    searchField->AddClass("settings-search-field");
    searchField->SetValue("");
    
    // Add text field to search bar first
    searchBar->AddChild(std::move(searchField));
    
    // Create icon element after field so it appears behind
    auto searchIcon = std::make_unique<UIElement>();
    searchIcon->AddClass("settings-search-icon");
    UIElement* searchIconPtr = searchIcon.get();
    searchBar->AddChild(std::move(searchIcon));

    auto clearButton = std::make_unique<Button>();
    clearButton->AddClass("icon-button");
    clearButton->AddClass("settings-search-clear");
    clearButton->AddClass("xclose-icon");
    clearButton->AddClass("hidden");
    clearButton->SetFocusable(false);
    clearButton->SetTooltip("Clear search");
    Button* clearButtonPtr = clearButton.get();
    searchBar->AddChild(std::move(clearButton));
    
    // Show background when typing, hide when cleared, and perform search
    m_SearchField->SetOnValueChanging([this, searchBarPtr, searchIconPtr, clearButtonPtr](const std::string& value) {
        if (!value.empty()) {
            searchBarPtr->AddClass("has-query");
            clearButtonPtr->RemoveClass("hidden");
            // Has text - show background and make icon fully opaque
            if (searchBarPtr->HasClass("hidden")) {
                searchBarPtr->RemoveClass("hidden");
            }
            if (!searchBarPtr->HasClass("active")) {
                searchBarPtr->AddClass("active");
            }
            if (!searchIconPtr->HasClass("icon-active")) {
                searchIconPtr->AddClass("icon-active");
            }
            // Perform search
            PerformSearch(value);
        } else {
            searchBarPtr->RemoveClass("has-query");
            clearButtonPtr->AddClass("hidden");
            // Empty - hide background again if search bars are globally hidden
            if (!SettingsPanel::GetSearchBarsVisible() && !searchBarPtr->HasClass("hidden")) {
                searchBarPtr->AddClass("hidden");
            }
            searchBarPtr->RemoveClass("active");
            searchIconPtr->RemoveClass("icon-active");
            // Clear search results and show normal category
            ClearSearchResults();
        }
    });

    clearButtonPtr->RegisterEventHandler(kEventButtonClick, [this, searchBarPtr, searchIconPtr, clearButtonPtr](UIEvent&) {
        if (!m_SearchField || m_SearchField->GetValue().empty())
            return;
        m_SearchField->SetValue("");
        if (!SettingsPanel::GetSearchBarsVisible())
            searchBarPtr->AddClass("hidden");
        searchBarPtr->RemoveClass("has-query");
        searchBarPtr->RemoveClass("active");
        searchIconPtr->RemoveClass("icon-active");
        clearButtonPtr->AddClass("hidden");
        ClearSearchResults();
    });
    
    // Show background when field gains focus (clicking in it)
    m_SearchField->RegisterEventHandler(kEventFocusIn, [searchBarPtr, searchIconPtr](UIEvent&) {
        if (searchBarPtr->HasClass("hidden")) {
            searchBarPtr->RemoveClass("hidden");
        }
        if (!searchBarPtr->HasClass("active")) {
            searchBarPtr->AddClass("active");
        }
        if (!searchIconPtr->HasClass("icon-active")) {
            searchIconPtr->AddClass("icon-active");
        }
    });
    
    // Hide background when clicking away (losing focus) if empty
    m_SearchField->RegisterEventHandler(kEventFocusOut, [searchBarPtr, searchIconPtr, searchField = m_SearchField](UIEvent&) {
        if (searchField && searchField->GetValue().empty()) {
            if (!SettingsPanel::GetSearchBarsVisible() && !searchBarPtr->HasClass("hidden")) {
                searchBarPtr->AddClass("hidden");
            }
            searchBarPtr->RemoveClass("active");
            searchIconPtr->RemoveClass("icon-active");
        }
    });

    parent->AddChild(std::move(searchBar));

    // Register after mounting so the shared placement preference can also style
    // the container that reserves space above or below the content pane.
    RegisterSearchBar(m_SearchBar, searchIconPtr);
}

void SettingsPanel::BuildUI()
{
    if (m_UIBuilt)
        return;
    m_UIBuilt = true;
    
    // Determine initial layout based on aspect ratio
    float width = GetLayoutWidth();
    float height = GetLayoutHeight();
    float aspectRatio = (height > 0) ? (width / height) : 1.0f;
    m_LastAspectRatio = aspectRatio;
    
    // Choose layout mode based on aspect ratio
    LayoutMode initialMode = (aspectRatio > 1.0f) ? LayoutMode::Horizontal : LayoutMode::Vertical;
    m_CurrentLayoutMode = initialMode;
    
    RebuildLayout(initialMode);
}

void SettingsPanel::RebuildLayout(LayoutMode newMode)
{
    m_RebuildScheduled = false;
    
    // Store current category to restore after rebuild
    SettingsCategory savedCategory = m_CurrentCategory;

    // If we previously registered a search bar for this panel, unregister before destroying UI.
    if (m_SearchBar)
        UnregisterSearchBar(m_SearchBar);
    
    // Clear all children safely
    std::vector<UIElement*> childrenToRemove;
    for (const auto& child : GetChildren())
    {
        childrenToRemove.push_back(child.get());
    }
    for (auto* child : childrenToRemove)
    {
        RemoveChild(child);
    }
    m_PageColorPickers.CloseAll();
    
    // Reset pointers
    m_TreeView = nullptr;
    m_TreePane = nullptr;
    m_ContentPane = nullptr;
    m_VerticalSplitter = nullptr;
    m_TreeSplitterDragging = false;
    m_ContentHeader = nullptr;
    m_ContentBody = nullptr;
    m_SearchField = nullptr;
    m_SearchBar = nullptr;
    m_RegistryButtonRows.clear();
    m_HierarchyTreeChildIndentSlider = nullptr;
    m_HierarchyTreeChildIndentLabel = nullptr;
    m_HierarchyTreeRowHeightSlider = nullptr;
    m_HierarchyTreeRowHeightLabel = nullptr;
    m_HierarchyTreeIconSizeSlider = nullptr;
    m_HierarchyTreeIconSizeLabel = nullptr;
    m_HierarchyTreeIconSizeField = nullptr;
    m_AssetsTreeChildIndentSlider = nullptr;
    m_AssetsTreeChildIndentLabel = nullptr;
    m_AssetsTreeRowHeightSlider = nullptr;
    m_AssetsTreeRowHeightLabel = nullptr;
    m_AssetsTreeIconSizeSlider = nullptr;
    m_AssetsTreeIconSizeLabel = nullptr;
    m_AssetsTreeIconSizeField = nullptr;
    m_GridIconSizeSlider = nullptr;
    m_GridIconSizeField = nullptr;
    m_SearchBarsToggle = nullptr;
    m_CheckmarkTogglesToggle = nullptr;
    m_GraySlidersToggle = nullptr;
    m_TabIconsToggle = nullptr;
    m_FontSmoothingField = nullptr;
    m_FontSizeBaseSlider = nullptr;
    m_FontSizeTreeSlider = nullptr;
    m_FontSizeSmallSlider = nullptr;
    m_FontSizeHeaderSlider = nullptr;
    m_UIAccentColorSwatch = nullptr;
    m_UIAssetIconTintSwatch = nullptr;
    m_UISmartFolderIconTintSwatch = nullptr;
    m_SelectionBoxColorSwatch = nullptr;
    m_SelectionOutlineColorSwatch = nullptr;
    m_SceneViewBackgroundColorSwatch = nullptr;
    m_RulerIndicatorColorSwatch = nullptr;
    m_MeasureColorSwatch = nullptr;
    m_GridColor3DSwatch = nullptr;
    m_GridColor2DSwatch = nullptr;
    m_UIScrollbarColorSwatch = nullptr;

    m_CurrentLayoutMode = newMode;
    
    if (newMode == LayoutMode::Horizontal)
    {
        // Horizontal layout: tree on left, content on right
        auto splitView = std::make_unique<SplitView>();
        splitView->AddClass("dock-split");
        splitView->AddClass("row");  // Horizontal orientation
        
        // Left pane: tree
        auto leftPane = std::make_unique<WeightedPane>(0.25f);  // 25% for tree
        leftPane->AddClass("pane");
        CreateTreeView(leftPane.get());
        splitView->AddChild(std::move(leftPane));
        
        // Splitter
        auto splitter = std::make_unique<Splitter>();
        splitter->SetId("settings-splitter");
        splitter->AddClass("splitter");
        splitter->AddClass("row");
        splitView->AddChild(std::move(splitter));
        
        // Right pane: content + search bar
        auto rightPane = std::make_unique<WeightedPane>(0.75f);  // 75% for content
        rightPane->AddClass("pane");
        
        // Create a container for content and search bar
        auto rightContainer = std::make_unique<UIElement>();
        rightContainer->AddClass("settings-right-container");
        
        CreateContentPane(rightContainer.get());
        CreateSearchBar(rightContainer.get());
        
        rightPane->AddChild(std::move(rightContainer));
        splitView->AddChild(std::move(rightPane));
        
        this->AddChild(std::move(splitView));
    }
    else
    {
        // Vertical layout: tree on top, draggable splitter, content below.
        CreateTreeView(this);
        UpdateTreePaneHeight();

        auto splitter = std::make_unique<UIElement>();
        splitter->AddClass("splitter");
        splitter->AddClass("col");
        splitter->AddClass("settings-vertical-splitter");
        m_VerticalSplitter = splitter.get();

        m_VerticalSplitter->RegisterEventHandler(kEventMouseDown, [this](UIEvent& e) {
            if (e.Button != 0 || !m_TreePane)
                return;
            m_TreeSplitterDragging = true;
            m_TreeSplitterDragStartY = e.Y;
            m_TreeSplitterDragStartHeight = m_TreePane->GetLayoutHeight();
            e.Capture(m_VerticalSplitter);
            e.Stop();
        });

        m_VerticalSplitter->RegisterEventHandler(kEventMouseMove, [this](UIEvent& e) {
            if (!m_TreeSplitterDragging || !m_TreePane)
                return;
            const float delta = e.Y - m_TreeSplitterDragStartY;
            float newHeight = m_TreeSplitterDragStartHeight + delta;
            const float panelHeight = GetLayoutHeight();
            const float maxHeight = panelHeight > 0 ? panelHeight - 120.0f : 600.0f;
            newHeight = std::clamp(newHeight, 200.0f, std::max(200.0f, maxHeight));
            m_UserTreePaneHeightPx = newHeight;
            UpdateTreePaneHeight();
            e.Stop();
        });

        m_VerticalSplitter->RegisterEventHandler(kEventMouseUp, [this](UIEvent& e) {
            if (!m_TreeSplitterDragging)
                return;
            m_TreeSplitterDragging = false;
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetDouble("ui.settings.treePaneHeightPx", static_cast<double>(m_UserTreePaneHeightPx));
            prefs.Save(&err);
            e.Stop();
        });

        this->AddChild(std::move(splitter));

        CreateContentPane(this);
        CreateSearchBar(this);
    }
    
    // Force layout update
    MarkDirty(LayoutDirty | VisualDirty | ChildrenDirty);
    
    // Restore the current category content
    PostAction([this, savedCategory]() {
        m_CurrentCategory = savedCategory;
        ShowCategoryContent(savedCategory);
    });
}

void SettingsPanel::SetContext(EditorContext* context)
{
    m_EditorContext = context;
    g_SettingsUndoContext = context;
}

void SettingsPanel::OpenColorPicker(uint32_t initialArgb,
                                    std::function<void(uint32_t)> onApply,
                                    std::function<void()> onCancel,
                                    std::function<void(uint32_t)> onValueChanging)
{
    if (!m_OpenColorPickerWindow)
    {
        Logger::Log::Error("SettingsPanel: no color picker to open; the host must call SetOpenColorPickerWindow");
        return;
    }

    ColorPickerCallbacks callbacks;
    if (onApply)
        callbacks.onApply = [fn = std::move(onApply)](uint32_t argb, float) { fn(argb); };
    if (onCancel)
        callbacks.onCancel = std::move(onCancel);
    if (onValueChanging)
        callbacks.onValueChanging = [fn = std::move(onValueChanging)](uint32_t argb, float) { fn(argb); };
    // Page callbacks capture the page's elements, so the picker closes with the page.
    callbacks.scope = &m_PageColorPickers;
    m_OpenColorPickerWindow(initialArgb, kSettingsColorIntensity, std::move(callbacks));
}

void SettingsPanel::ApplyAccentColorToUI()
{
    UIManager* ui = GetOwnerManager();
    if (!ui)
        return;

    // Replace this panel's previous runtime accent stylesheet instead of
    // appending unbounded global styles. Keeping the block single-entry avoids
    // cross-window/style accumulation artifacts during live preview updates.
    std::vector<const Stylesheet*> oldBlock;
    if (m_UIAccentStylesheet)
        oldBlock.push_back(m_UIAccentStylesheet.get());

    if (auto newSheet = UI::AccentStyleHelper::BuildAccentColorStylesheet(m_UIAccentColor))
    {
        StylesheetHandle newHandle = newSheet;
        ui->ReplaceGlobalStylesheetBlock(oldBlock, std::vector<StylesheetHandle>{newHandle});
        m_UIAccentStylesheet = std::move(newHandle);
    }
    else
    {
        ui->ReplaceGlobalStylesheetBlock(oldBlock, {});
        m_UIAccentStylesheet.reset();
    }
    
    // Forward accent color to dock overlays
    if (auto* root = ui->GetRootElement())
    {
        if (auto* ds = dynamic_cast<DockspaceElement*>(root->FindById("dock")))
            ds->SetAccentColor(m_UIAccentColor);
    }

    // Force full style recalculation for all elements to pick up new CSS variables
    ui->MarkStyleDirtyAll();
}

namespace
{
    const char* kPrefKeyCompactComponentHeaders = "ui.inspectorCompactComponentHeaders";
    StylesheetHandle s_CompactComponentHeadersStylesheet;
    const char* kPrefKeyInspectorFilledSections = "ui.inspectorFilledSections";
    const char* kPrefKeyInspectorShowInfoCards = "ui.inspectorShowInfoCards";

    const char* kPrefKeyHierarchyIconsColored = "ui.hierarchyIconsColored";
    const char* kPrefKeyHierarchyModelThumbsAlwaysColored = "ui.hierarchyModelThumbsAlwaysColored";
    const char* kPrefKeyGraySliders = "ui.graySliders";
    const char* kPrefKeyCheckmarkToggles = "ui.checkmarkToggles";
    const char* kPrefKeyTextSubpixelAA = "text.subpixelAA";
    const char* kPrefKeyTextContrast = "text.contrast";
    const char* kPrefKeyTextSmoothingGamma = "text.smoothingGamma";
    constexpr float kTextContrastDefault = 1.0f;
    constexpr float kTextContrastMin = 0.0f;
    constexpr float kTextContrastMax = 1.0f;
    constexpr float kTextSmoothingGammaDefault = 0.7f;
    constexpr float kTextSmoothingGammaMin = 0.5f;
    constexpr float kTextSmoothingGammaMax = 2.0f;

    const char* kPrefKeyTabIcons = "ui.tabIcons";

    const char* kPrefKeyDropdownShadow = "ui.shadows.dropdown";
    const char* kPrefKeyPickerShadow = "ui.shadows.picker";
    const char* kPrefKeyCompletionShadow = "ui.shadows.completion";
    const char* kPrefKeyTooltipShadow = "ui.shadows.tooltip";
    const char* kPrefKeyShadowOffsetX = "ui.shadows.offsetX";
    const char* kPrefKeyShadowOffsetY = "ui.shadows.offsetY";
    const char* kPrefKeyShadowBlur = "ui.shadows.blur";
    const char* kPrefKeyShadowOpacity = "ui.shadows.opacity";

    const char* kPrefKeyValueBoxHeight = "ui.valueBoxHeight";
    constexpr float kDefaultValueBoxHeight = 26.0f;

    // Default popup shadow (matches the editor's modal dialogs). Opacity is
    // tuned for encoded-space (sRGB) UI blending, where black lands heavier
    // than the same alpha in linear light; keep the widgets.css
    // var(--popup-shadow) fallbacks in sync.
    constexpr float kDefaultShadowOffsetX = 0.0f;
    constexpr float kDefaultShadowOffsetY = -1.0f;
    constexpr float kDefaultShadowBlur = 24.0f;
    constexpr float kDefaultShadowOpacity = 0.25f;

    // Last-applied popup shadow parameters. Cached so live slider drags can update
    // a single field and re-apply without a prefs round-trip (the slider helper fires
    // its callback before persisting). Process-global: all windows share one look.
    struct PopupShadowState
    {
        float offsetX = kDefaultShadowOffsetX;
        float offsetY = kDefaultShadowOffsetY;
        float blur = kDefaultShadowBlur;
        float opacity = kDefaultShadowOpacity;
        bool dropdown = true;
        bool picker = true;
        bool completion = true;
        bool tooltip = true;
    };
    PopupShadowState g_PopupShadow;

    // Push the cached shadow state into a UIManager: a `--popup-shadow` custom
    // property on the root (consumed by `box-shadow: var(--popup-shadow)` in
    // widgets.css) plus the per-popup opt-out root classes.
    void ApplyPopupShadowToManager(UIManager* ui)
    {
        if (!ui)
            return;
        UIElement* root = ui->GetRootElement();
        if (!root)
            return;

        const float opacity = std::clamp(g_PopupShadow.opacity, 0.0f, 1.0f);
        char shadow[96];
        std::snprintf(shadow, sizeof(shadow), "%.1fpx %.1fpx %.1fpx rgba(0, 0, 0, %.3f)",
                      g_PopupShadow.offsetX, g_PopupShadow.offsetY,
                      std::max(0.0f, g_PopupShadow.blur), opacity);
        root->Overrides().SetCustom(HashStringId("--popup-shadow"), shadow);

        auto optOut = [root](bool enabled, const char* cls) {
            if (enabled)
                root->RemoveClass(cls);
            else
                root->AddClass(cls);
        };
        optOut(g_PopupShadow.dropdown, "no-dropdown-shadow");
        optOut(g_PopupShadow.picker, "no-picker-shadow");
        optOut(g_PopupShadow.completion, "no-completion-shadow");
        optOut(g_PopupShadow.tooltip, "no-tooltip-shadow");

        ui->MarkStyleDirtyAll();
    }

}

void SettingsPanel::ApplySavedCompactComponentHeadersStyle(UIManager* ui)
{
    if (!ui)
        return;

    bool compact = true;
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);
    prefs.TryGetBool(kPrefKeyCompactComponentHeaders, compact);

    std::vector<const Stylesheet*> oldBlock;
    if (s_CompactComponentHeadersStylesheet)
        oldBlock.push_back(s_CompactComponentHeadersStylesheet.get());

    // Both heading levels, from one setting. A component header and the section headers inside it
    // are read as one ladder, so a section bar left at the default height while the component bar
    // above it compacts reads as a different kind of thing rather than a nested one.
    const char* css = compact
        ? ".inspector-section-header-row { height: 24px; min-height: 24px; max-height: 24px; }"
          ".rp-foldout.inspector-component-section > .foldout-header"
          " { height: 24px; min-height: 24px; max-height: 24px; }"
        : ".inspector-section-header-row { height: 28px; min-height: 28px; max-height: 28px; }"
          ".rp-foldout.inspector-component-section > .foldout-header"
          " { height: 28px; min-height: 28px; max-height: 28px; }";

    auto newSheet = std::make_shared<Stylesheet>();
    if (UIParsing::CSSParser::ParseStylesFromString(css, *newSheet))
    {
        StylesheetHandle newHandle = newSheet;
        ui->ReplaceGlobalStylesheetBlock(oldBlock, std::vector<StylesheetHandle>{newHandle});
        s_CompactComponentHeadersStylesheet = std::move(newHandle);
        ui->MarkStyleDirtyAll();
    }
}

void SettingsPanel::ApplySavedHierarchyIconsColoredStyle(UIManager* ui)
{
    if (!ui)
        return;

    bool colored = true;
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);
    prefs.TryGetBool(kPrefKeyHierarchyIconsColored, colored);

    auto* root = ui->GetRootElement();
    if (!root)
        return;

    if (colored)
        root->AddClass("hierarchy-icons-colored");
    else
        root->RemoveClass("hierarchy-icons-colored");
    ui->MarkStyleDirtySubtree(root);
}

void SettingsPanel::ApplySavedHierarchyModelThumbsAlwaysColoredStyle(UIManager* ui)
{
    if (!ui)
        return;

    bool colored = true;
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);
    prefs.TryGetBool(kPrefKeyHierarchyModelThumbsAlwaysColored, colored);

    auto* root = ui->GetRootElement();
    if (!root)
        return;

    if (colored)
        root->AddClass("hierarchy-model-thumbs-colored");
    else
        root->RemoveClass("hierarchy-model-thumbs-colored");
    ui->MarkStyleDirtySubtree(root);
}

void SettingsPanel::ApplySavedTextSubpixelAA(UIManager* ui)
{
    if (!ui)
        return;

    // The Font Rendering settings content (which also applies this preference)
    // builds lazily on first view — a saved opt-in must engage at startup and
    // for new windows without the settings page ever being opened.
    bool enabled = false;
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);
    prefs.TryGetBool(kPrefKeyTextSubpixelAA, enabled);
    ui->SetTextSubpixelAA(enabled);
}

void SettingsPanel::ApplySavedInspectorBigNumberSpacing()
{
    bool enabled = kInspectorBigNumberSpacingDefault;
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);
    prefs.TryGetBool(kPrefKeyInspectorBigNumberSpacing, enabled);
    FloatField::SetBigNumberSpacingEnabled(enabled);
}

void SettingsPanel::ApplySavedTextContrast(UIManager* ui)
{
    if (!ui)
        return;

    // Same lazy-build constraint as the subpixel opt-in: the Font Rendering
    // page also applies this, but only once it has been opened (#714).
    float value = kTextContrastDefault;
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);
    double stored = value;
    if (prefs.TryGetDouble(kPrefKeyTextContrast, stored))
        value = std::clamp(static_cast<float>(stored), kTextContrastMin, kTextContrastMax);
    ui->SetTextContrast(value);
}

void SettingsPanel::ApplySavedTextSmoothingGamma(UIManager* ui)
{
    if (!ui)
        return;

    float value = kTextSmoothingGammaDefault;
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);
    double stored = value;
    if (prefs.TryGetDouble(kPrefKeyTextSmoothingGamma, stored))
        value = std::clamp(static_cast<float>(stored), kTextSmoothingGammaMin,
                           kTextSmoothingGammaMax);
    ui->SetTextSmoothingGamma(value);
}

void SettingsPanel::ApplySavedGraySlidersStyle(UIManager* ui)
{
    if (!ui)
        return;

    bool gray = false;
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);
    prefs.TryGetBool(kPrefKeyGraySliders, gray);

    UIElement* root = ui->GetRootElement();
    if (!root)
        return;

    if (gray)
        root->AddClass("gray-sliders");
    else
        root->RemoveClass("gray-sliders");

    ui->MarkStyleDirtyAll();
}

void SettingsPanel::ApplyGraySlidersStyleToUI()
{
    ApplySavedGraySlidersStyle(GetOwnerManager());
}

void SettingsPanel::ApplySavedToggleStyle(UIManager* ui)
{
    if (!ui)
        return;

    bool useCheckmarks = false;
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);
    prefs.TryGetBool(kPrefKeyCheckmarkToggles, useCheckmarks);

    ApplyToggleStyle(ui, useCheckmarks);
    Editor::ToggleAppearanceSettings::Get().ApplyTo(ui);
}

void SettingsPanel::ApplyToggleStyle(UIManager* ui, bool useCheckmarks)
{
    if (!ui)
        return;

    Toggle::SetCheckmarkPresentationEnabled(useCheckmarks);

    std::unordered_set<UIElement*> visited;
    std::function<void(UIElement*)> updateToggles = [&](UIElement* element) {
        if (!element || !visited.insert(element).second)
            return;

        if (auto* toggle = dynamic_cast<Toggle*>(element))
        {
            if (useCheckmarks)
                toggle->AddClass("toggle-checkmark");
            else
                toggle->RemoveClass("toggle-checkmark");
        }

        for (const auto& child : element->GetChildren())
            updateToggles(child.get());
        updateToggles(element->GetMountTarget());
    };
    updateToggles(ui->GetRootElement());

    ui->MarkStyleDirtyAll();
    ui->RequestRelayout();
}

void SettingsPanel::ApplySavedTabIconsStyle(UIManager* ui)
{
    if (!ui)
        return;

    bool enabled = true;
    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);
    prefs.TryGetBool(kPrefKeyTabIcons, enabled);

    auto* root = ui->GetRootElement();
    if (!root)
        return;

    if (enabled)
        root->RemoveClass("no-tab-icons");
    else
        root->AddClass("no-tab-icons");
    ui->MarkStyleDirtySubtree(root);
}

void SettingsPanel::ApplySavedPopupShadowStyle(UIManager* ui)
{
    if (!ui)
        return;

    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);

    prefs.TryGetBool(kPrefKeyDropdownShadow, g_PopupShadow.dropdown);
    prefs.TryGetBool(kPrefKeyPickerShadow, g_PopupShadow.picker);
    prefs.TryGetBool(kPrefKeyCompletionShadow, g_PopupShadow.completion);
    prefs.TryGetBool(kPrefKeyTooltipShadow, g_PopupShadow.tooltip);

    auto loadFloat = [&prefs](const char* key, float& out) {
        double stored = out;
        if (prefs.TryGetDouble(key, stored))
            out = static_cast<float>(stored);
    };
    loadFloat(kPrefKeyShadowOffsetX, g_PopupShadow.offsetX);
    loadFloat(kPrefKeyShadowOffsetY, g_PopupShadow.offsetY);
    loadFloat(kPrefKeyShadowBlur, g_PopupShadow.blur);
    loadFloat(kPrefKeyShadowOpacity, g_PopupShadow.opacity);

    ApplyPopupShadowToManager(ui);
}

void SettingsPanel::ApplySavedValueBoxHeightStyle(UIManager* ui)
{
    if (!ui)
        return;
    UIElement* root = ui->GetRootElement();
    if (!root)
        return;

    float heightPx = kDefaultValueBoxHeight;
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        double stored = heightPx;
        if (prefs.TryGetDouble(kPrefKeyValueBoxHeight, stored))
            heightPx = static_cast<float>(stored);
    }

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.0fpx", heightPx);
    root->Overrides().SetCustom(HashStringId("--value_box_height"), buf);
    ui->MarkStyleDirtyAll();
}

void SettingsPanel::ApplySavedNodeGraphHeaderAlignmentStyle(UIManager* ui)
{
    if (!ui)
        return;

    auto* root = ui->GetRootElement();
    if (!root)
        return;

    auto* canvas = root->FindById("NodeGraphCanvas");
    if (!canvas)
        return;

    canvas->MarkDirty(UIElement::VisualDirty);
}

namespace
{

// Size the script face's line metrics are probed at. A `normal` line box is
// rounded to whole pixels per size, so the ratio derived below is an
// approximation of the box at the stylesheet's size rather than an identity;
// probing large keeps the rounding a small fraction of the result.
constexpr unsigned kScriptFontMetricProbePx = 64u;

// The line-height slider steps by 0.05, so no legitimate setting lands this
// close to 1.0 without meaning it.
constexpr float kScriptLineHeightNormalEpsilon = 0.001f;

// Font families resolve on the job system, so the startup apply can run before
// the selected script face exists. Re-resolve for a bounded number of drains
// rather than baking the fallback atlas's metrics for the rest of the session.
constexpr int kScriptFontResolveDrains = 120;

// What an apply needs, resolved once from disk by the public entry point so the
// re-resolve chain below never touches the preferences file again. Seq retires
// superseded chains: a later apply bumps it, and the older chain's next drain
// sees the mismatch and stops.
struct ScriptLineHeightApply
{
    float LineHeight = 1.0f;
    std::shared_ptr<const std::vector<std::string>> Families;
    int DrainsLeft = 0;
    uint32_t Seq = 0;
};
// Entries live only for the duration of a retry chain — a finished or
// exhausted chain erases its own, so a closed window's UIManager* cannot
// accumulate here.
std::unordered_map<UIManager*, ScriptLineHeightApply> s_ScriptLineHeightApplyByUi;
// Monotonic across entries rather than per-entry: entries are erased, so a
// per-entry counter would restart at 1 and a retired chain's queued callback
// could match a fresh chain's Seq.
uint32_t s_ScriptLineHeightApplySeq = 0;

struct ScriptFontNormalLine
{
    // Normal line height divided by font size for the resolved script face.
    float Ratio = 0.0f;
    // False while the selected family is still streaming in and
    // ResolveFontForStyle is answering with the default UI atlas.
    bool FromScriptFace = false;
};

ScriptFontNormalLine ResolveScriptFontNormalLine(
    UIManager* ui, std::shared_ptr<const std::vector<std::string>> families)
{
    ScriptFontNormalLine out;

    // ResolveFontForStyle keys on the family stack and the weight/style/variant
    // defaults the script rule inherits; the size only matters to the metric
    // query below.
    ResolvedStyle style;
    style.Visual.FontFamily = std::move(families);

    Rendering::Text::FontAtlas* atlas = ui->ResolveFontForStyle(style);
    if (!atlas)
        return out;

    const auto metrics = atlas->GetFontLineMetrics(kScriptFontMetricProbePx);
    out.Ratio = metrics.height / static_cast<float>(kScriptFontMetricProbePx);
    // A script face that genuinely shares the UI face's bytes aliases to the
    // same atlas and simply re-derives the same ratio, so treating it as
    // unresolved only costs a few idempotent re-applies.
    out.FromScriptFace = (atlas != ui->GetDefaultFontAtlas());
    return out;
}

// Returns true when the emitted value is provisional because the script face
// has not resolved yet.
bool ApplyScriptLineHeightStyle(UIManager* ui, const ScriptLineHeightApply& apply)
{
    UIElement* root = ui->GetRootElement();
    if (!root)
        return false;

    const float lineHeight = apply.LineHeight;

    // The saved value means "multiples of a normal line". CSS resolves a
    // unitless line-height against the font SIZE, so a fixed multiplier only
    // means one line for a face whose metric ratio happens to match it;
    // `normal` is the resolved face's own line height, which is exactly what
    // 1.0 has to reproduce for every selectable script font.
    std::string lineHeightCss = "normal";
    bool provisional = false;
    if (std::abs(lineHeight - 1.0f) > kScriptLineHeightNormalEpsilon)
    {
        const ScriptFontNormalLine normalLine = ResolveScriptFontNormalLine(ui, apply.Families);
        if (normalLine.Ratio > 0.0f)
        {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.4f", lineHeight * normalLine.Ratio);
            lineHeightCss = buf;
        }
        provisional = !normalLine.FromScriptFace;
    }

    // Keep script-variable input rows in Inspector visually in sync with the
    // Script Editor line-height slider. These track the multiplier itself, not
    // the script face — they are UI chrome, not script text.
    constexpr float kScriptInspectorBaseRowHeightPx = 27.0f;
    const float scriptInspectorRowHeightPx =
        std::clamp(kScriptInspectorBaseRowHeightPx * lineHeight, 16.0f, 128.0f);
    char rowBuf[32];
    std::snprintf(rowBuf, sizeof(rowBuf), "%.1fpx", scriptInspectorRowHeightPx);
    const std::string rowHeightCss = rowBuf;

    auto setCustom = [root](StringId name, const std::string& value)
    {
        const std::string* existing = root->Overrides().FindCustom(name);
        if (existing && *existing == value)
            return false;
        root->Overrides().SetCustom(name, value);
        return true;
    };

    bool changed = setCustom(HashStringId("--script_line_height"), lineHeightCss);
    changed |= setCustom(HashStringId("--script_inspector_row_height"), rowHeightCss);
    changed |= setCustom(HashStringId("--inspector-row-height"), rowHeightCss);
    if (changed)
        ui->MarkStyleDirtySubtree(root);

    return provisional;
}

// Both terminal paths below erase the map entry. The one case that does not is
// a window closing mid-chain: the UIManager's dispatcher dies with it, so the
// queued callback never runs and its entry outlives it. That leaks one struct
// per window closed inside the retry window (at most kScriptFontResolveDrains
// drains, ~2s), and cannot be fixed here — it needs a UIManager teardown
// notification, which the editor does not have. s_FontPrefsSheetByUi has the
// same residual for the same reason. Address reuse is safe: every field is
// overwritten on the next apply, and Seq is module-monotonic so a retired
// callback can never match a fresh chain.
void PumpScriptLineHeightRetry(UIManager* ui, uint32_t seq)
{
    ui->PostToUI(
        [ui, seq]()
        {
            auto it = s_ScriptLineHeightApplyByUi.find(ui);
            if (it == s_ScriptLineHeightApplyByUi.end() || it->second.Seq != seq ||
                it->second.DrainsLeft <= 0)
                return;

            --it->second.DrainsLeft;
            if (!ApplyScriptLineHeightStyle(ui, it->second))
            {
                s_ScriptLineHeightApplyByUi.erase(it);
                return;
            }
            if (it->second.DrainsLeft <= 0)
            {
                // Out of drains with the script face still unresolved: the
                // default UI atlas's ratio is what stays applied for the
                // session, so say so rather than leaving a silently wrong
                // line height.
                Logger::Log::Warning(
                    "SettingsPanel: script font did not resolve within {} drains; script line "
                    "height stays on the default UI face's metrics",
                    kScriptFontResolveDrains);
                s_ScriptLineHeightApplyByUi.erase(it);
                return;
            }
            PumpScriptLineHeightRetry(ui, seq);
        });
}

} // namespace

void SettingsPanel::ApplySavedScriptLineHeightStyle(UIManager* ui)
{
    if (!ui)
        return;

    float lineHeight = 1.0f;
    std::string scriptFontPreset;
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        double stored = lineHeight;
        if (prefs.TryGetDouble("script.lineHeight", stored))
            lineHeight = static_cast<float>(std::clamp(stored, 0.5, 4.0));
        (void)prefs.TryGetString(Editor::kPrefScriptFontPreset, scriptFontPreset);
    }

    auto& apply = s_ScriptLineHeightApplyByUi[ui];
    apply.LineHeight = lineHeight;
    apply.Families = std::make_shared<const std::vector<std::string>>(
        Editor::GetScriptFontFamilyStack(scriptFontPreset));
    apply.Seq = ++s_ScriptLineHeightApplySeq;
    if (!ApplyScriptLineHeightStyle(ui, apply))
    {
        s_ScriptLineHeightApplyByUi.erase(ui);
        return;
    }
    apply.DrainsLeft = kScriptFontResolveDrains;
    PumpScriptLineHeightRetry(ui, apply.Seq);
}

void SettingsPanel::ApplyScrollbarColorToUI()
{
    UIManager* ui = GetOwnerManager();
    if (!ui)
        return;

    uint8_t r = (m_UIScrollbarColor >> 16) & 0xFF;
    uint8_t g = (m_UIScrollbarColor >> 8) & 0xFF;
    uint8_t b = m_UIScrollbarColor & 0xFF;

    uint8_t rHover = static_cast<uint8_t>(std::min(255, static_cast<int>(r) + 24));
    uint8_t gHover = static_cast<uint8_t>(std::min(255, static_cast<int>(g) + 24));
    uint8_t bHover = static_cast<uint8_t>(std::min(255, static_cast<int>(b) + 24));

    char css[512];
    std::snprintf(css, sizeof(css),
        ".scrollbar-thumb { background-color: #%02X%02X%02X; } "
        ".scrollbar:hover .scrollbar-thumb { background-color: #%02X%02X%02X; } "
        ".scrollbar.active .scrollbar-thumb { background-color: #%02X%02X%02X; }",
        r, g, b,
        rHover, gHover, bHover,
        rHover, gHover, bHover);

    std::vector<const Stylesheet*> oldBlock;
    if (m_UIScrollbarStylesheet)
        oldBlock.push_back(m_UIScrollbarStylesheet.get());

    auto newSheet = std::make_shared<Stylesheet>();
    if (UIParsing::CSSParser::ParseStylesFromString(css, *newSheet))
    {
        StylesheetHandle newHandle = newSheet;
        ui->ReplaceGlobalStylesheetBlock(oldBlock, std::vector<StylesheetHandle>{newHandle});
        m_UIScrollbarStylesheet = std::move(newHandle);
    }
    else
    {
        ui->ReplaceGlobalStylesheetBlock(oldBlock, {});
        m_UIScrollbarStylesheet.reset();
    }

    ui->MarkStyleDirtyAll();
}

void SettingsPanel::ApplySceneToolActiveColor(const std::string& mode)
{
    UIManager* ui = GetOwnerManager();
    if (!ui)
        return;

    std::vector<const Stylesheet*> oldBlock;
    if (m_UISceneToolStylesheet)
        oldBlock.push_back(m_UISceneToolStylesheet.get());

    if (mode == "gray")
    {
        const char* css =
            ":root { "
            "--ui_color_scene_tool_active: #6C6C6C; "
            "--ui_color_scene_tool_active_hover: #7E7E7E; "
            "}";
        auto newSheet = std::make_shared<Stylesheet>();
        if (UIParsing::CSSParser::ParseStylesFromString(css, *newSheet))
        {
            StylesheetHandle newHandle = newSheet;
            ui->ReplaceGlobalStylesheetBlock(oldBlock, std::vector<StylesheetHandle>{newHandle});
            m_UISceneToolStylesheet = std::move(newHandle);
        }
    }
    else
    {
        // "accent" — remove override, fallback to var(--ui_color_accent_blue) in CSS
        ui->ReplaceGlobalStylesheetBlock(oldBlock, {});
        m_UISceneToolStylesheet.reset();
    }

    ui->MarkStyleDirtyAll();
}

namespace
{
StylesheetHandle& InspectorRowStylesheetFor(UIManager* ui)
{
    static std::unordered_map<UIManager*, StylesheetHandle> sheets;
    return sheets[ui];
}

StylesheetHandle& SettingsRowStylesheetFor(UIManager* ui)
{
    static std::unordered_map<UIManager*, StylesheetHandle> sheets;
    return sheets[ui];
}

void ReplaceGeneratedStylesheet(UIManager* ui, StylesheetHandle& current, const char* css)
{
    if (!ui)
        return;

    std::vector<const Stylesheet*> oldBlock;
    if (current)
        oldBlock.push_back(current.get());

    auto newSheet = std::make_shared<Stylesheet>();
    if (UIParsing::CSSParser::ParseStylesFromString(css, *newSheet))
    {
        StylesheetHandle newHandle = newSheet;
        ui->ReplaceGlobalStylesheetBlock(oldBlock, std::vector<StylesheetHandle>{newHandle});
        current = std::move(newHandle);
    }
    else
    {
        ui->ReplaceGlobalStylesheetBlock(oldBlock, {});
        current.reset();
    }

    ui->MarkStyleDirtyAll();
}

void ApplyInspectorRowStyle(UIManager* ui, float indent, float gap, float labelWidthPercent)
{
    if (!ui)
        return;

    char css[4096];
    std::snprintf(css, sizeof(css),
        ".inspector-panel { --inspector-row-gap: %.0fpx; }"
        ".inspector-section:not(.inspector-section-transform) .inspector-section-body .inspector-row {"
        " padding-left: %.0fpx; }"
        ".inspector-section:not(.inspector-section-transform) .inspector-section-body .inspector-row .inspector-label-cell {"
        " flex: 0 0 %.0f%%; width: %.0f%%; min-width: 0; max-width: %.0f%%; }"
        ".inspector-section:not(.inspector-section-transform) .inspector-section-body .inspector-row > .inspector-label {"
        " flex: 0 0 %.0f%%; width: %.0f%%; min-width: 0; max-width: %.0f%%; }"
        ".inspector-asset-content > .inspector-row {"
        " padding-left: %.0fpx; }"
        ".inspector-asset-content > .inspector-row .inspector-label-cell {"
        " flex: 0 0 %.0f%%; width: %.0f%%; min-width: 0; max-width: %.0f%%; }"
        ".inspector-asset-content > .inspector-row > .inspector-label {"
        " flex: 0 0 %.0f%%; width: %.0f%%; min-width: 0; max-width: %.0f%%; }"
        ".material-inspector .inspector-row {"
        " padding-left: %.0fpx; }"
        ".material-properties-foldout .material-inspector .inspector-row .inspector-label-cell {"
        " flex: 0 0 %.0f%%; width: %.0f%%; min-width: 0; max-width: %.0f%%; }"
        ".material-properties-foldout .material-inspector .inspector-row > .inspector-label {"
        " flex: 0 0 %.0f%%; width: %.0f%%; min-width: 0; max-width: %.0f%%; }"
        ".rp-foldout .settings-row-label {"
        " flex: 0 0 %.0f%%; width: %.0f%%; min-width: 0; }"
        ".rp-foldout .inspector-row .inspector-label-cell {"
        " flex: 0 0 %.0f%%; width: %.0f%%; min-width: 0; max-width: %.0f%%; }"
        ".rp-foldout .inspector-row > .inspector-label {"
        " flex: 0 0 %.0f%%; width: %.0f%%; min-width: 0; max-width: %.0f%%; }",
        gap, indent,
        labelWidthPercent, labelWidthPercent, labelWidthPercent,
        labelWidthPercent, labelWidthPercent, labelWidthPercent,
        indent,
        labelWidthPercent, labelWidthPercent, labelWidthPercent,
        labelWidthPercent, labelWidthPercent, labelWidthPercent,
        indent,
        labelWidthPercent, labelWidthPercent, labelWidthPercent,
        labelWidthPercent, labelWidthPercent, labelWidthPercent,
        labelWidthPercent, labelWidthPercent,
        labelWidthPercent, labelWidthPercent, labelWidthPercent,
        labelWidthPercent, labelWidthPercent, labelWidthPercent);
    ReplaceGeneratedStylesheet(ui, InspectorRowStylesheetFor(ui), css);
}

void ApplySettingsRowGap(UIManager* ui, float gapPx)
{
    if (!ui)
        return;

    const float rowPaddingY = gapPx * 0.25f;
    const float rowMarginBottom = gapPx * 0.5f;
    const float headerPaddingY = gapPx * 0.25f;

    char css[2048];
    std::snprintf(css, sizeof(css),
        ".settings-panel .settings-content-body .scroll-content { gap: %.2fpx !important; }"
        ".settings-panel .settings-content { gap: %.2fpx !important; }"
        ".settings-panel .settings-row { margin-bottom: %.2fpx !important; padding-top: %.2fpx !important; padding-bottom: %.2fpx !important; }"
        ".settings-panel .settings-section-header { margin-top: %.2fpx !important; margin-bottom: %.2fpx !important; padding-top: %.2fpx !important; padding-bottom: %.2fpx !important; }"
        ".settings-panel .settings-subsection-header { margin-top: %.2fpx !important; margin-bottom: %.2fpx !important; padding-top: %.2fpx !important; padding-bottom: %.2fpx !important; }"
        ".settings-panel .settings-build-scene-row { margin-top: %.2fpx !important; }"
        ".settings-panel .settings-build-drop-zone { margin-bottom: %.2fpx !important; }",
        rowMarginBottom,
        rowMarginBottom,
        rowMarginBottom,
        rowPaddingY,
        rowPaddingY,
        rowMarginBottom,
        rowMarginBottom,
        headerPaddingY,
        headerPaddingY,
        rowMarginBottom,
        rowMarginBottom,
        headerPaddingY,
        headerPaddingY,
        rowMarginBottom,
        rowMarginBottom);
    ReplaceGeneratedStylesheet(ui, SettingsRowStylesheetFor(ui), css);
}

} // namespace

void SettingsPanel::ApplySavedRowGapStyles(UIManager* ui)
{
    if (!ui)
        return;

    auto prefs = Editor::OpenEditorPreferences();
    std::string err;
    prefs.Load(&err);

    double indent = 0.0;
    double gap = 4.0;
    double labelWidth = 37.0;
    prefs.TryGetDouble("ui.inspectorRowIndent", indent);
    prefs.TryGetDouble("ui.inspectorRowGap", gap);
    prefs.TryGetDouble("ui.inspectorLabelWidth", labelWidth);
    ApplyInspectorRowStyle(ui, static_cast<float>(indent), static_cast<float>(gap),
                           static_cast<float>(labelWidth));
    ApplySettingsRowGap(ui, static_cast<float>(gap));
}

void SettingsPanel::ApplyInspectorRowStyleToUI(float indent, float gap, float labelWidthPercent)
{
    UIManager* ui = GetOwnerManager();
    if (!ui)
        return;

    auto applyFn = [this, indent, gap, labelWidthPercent]()
    {
        ApplyInspectorRowStyle(GetOwnerManager(), indent, gap, labelWidthPercent);
        ApplySettingsRowGap(GetOwnerManager(), gap);
    };

    if (UIElement::IsInEventDispatch())
        PostAction(std::move(applyFn));
    else
        applyFn();
}

void SettingsPanel::SetUIManager(UIManager* uiManager)
{
    m_UIManager = uiManager;

    if (m_UIManager)
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        double hStored = m_HierarchyTreeIconSizeValue;
        prefs.TryGetDouble("ui.hierarchyTreeIconSize", hStored);
        const float hIcon = std::clamp(static_cast<float>(hStored), kMinEditorTreeIconSizePx,
                                       kMaxEditorHierarchyTreeIconSizePx);
        SetHierarchyTreeIconSizeValue(hIcon);

        double aStored = m_AssetsTreeIconSizeValue;
        prefs.TryGetDouble("ui.assetsTreeIconSize", aStored);
        const float aIcon = std::clamp(static_cast<float>(aStored), kMinEditorTreeIconSizePx,
                                       kMaxEditorAssetsTreeIconSizePx);
        SetAssetsTreeIconSizeValue(aIcon);
    }
}

void SettingsPanel::SetGridIconSizeValue(float px)
{
    m_GridIconSizeUpdating = true;
    m_GridIconSizeValue = px;
    if (m_GridIconSizeSlider)
        m_GridIconSizeSlider->SetValueWithoutNotify(px);
    if (m_GridIconSizeField)
        m_GridIconSizeField->SetValueWithoutNotify(px);
    m_GridIconSizeUpdating = false;
}

void SettingsPanel::SetSmartFoldersAtTopValue(bool atTop)
{
    if (m_SmartFoldersAtTopToggle)
        m_SmartFoldersAtTopToggle->SetChecked(atTop);
}

void SettingsPanel::SetSmartFoldersExpandedOnStartupValue(bool expanded)
{
    if (m_SmartFoldersExpandedOnStartupToggle)
        m_SmartFoldersExpandedOnStartupToggle->SetChecked(expanded);
}

void SettingsPanel::SetAssetsFoldersFirstValue(bool foldersFirst)
{
    if (m_AssetsFoldersFirstToggle)
        m_AssetsFoldersFirstToggle->SetChecked(foldersFirst);
}

void SettingsPanel::SetAssetsExpandFoldersOnLoadValue(bool expand)
{
    if (m_AssetsExpandFoldersOnLoadToggle)
        m_AssetsExpandFoldersOnLoadToggle->SetChecked(expand);
}

void SettingsPanel::SetAssetsExtraBottomViewToolbarValue(bool enabled)
{
    if (m_AssetsExtraBottomViewToolbarToggle)
        m_AssetsExtraBottomViewToolbarToggle->SetChecked(enabled);
}

void SettingsPanel::SetAssetsSingleViewToggleIconValue(bool enabled)
{
    if (m_AssetsSingleViewToggleIconToggle)
        m_AssetsSingleViewToggleIconToggle->SetChecked(enabled);
}

void SettingsPanel::SetAssetsBottomToolbarZoomSliderVisibleValue(bool visible)
{
    if (m_AssetsBottomToolbarZoomSliderVisibleToggle)
        m_AssetsBottomToolbarZoomSliderVisibleToggle->SetChecked(visible);
}

void SettingsPanel::SetInspectorCollapseArrowVisibleValue(bool visible)
{
    if (m_InspectorCollapseArrowVisibleToggle)
        m_InspectorCollapseArrowVisibleToggle->SetChecked(visible);
}

void SettingsPanel::SetInspectorComponentIconsVisibleValue(bool visible)
{
    if (m_InspectorComponentIconsVisibleToggle)
        m_InspectorComponentIconsVisibleToggle->SetChecked(visible);
}

void SettingsPanel::SetInspectorFilledSectionsValue(bool enabled)
{
    if (m_InspectorFilledSectionsToggle)
        m_InspectorFilledSectionsToggle->SetChecked(enabled);
}

void SettingsPanel::SetInspectorInfoCardsVisibleValue(bool visible)
{
    if (m_InspectorInfoCardsToggle)
        m_InspectorInfoCardsToggle->SetChecked(visible);
}

void SettingsPanel::SetInspectorBigNumberSpacingValue(bool enabled)
{
    if (m_InspectorBigNumberSpacingToggle)
        m_InspectorBigNumberSpacingToggle->SetChecked(enabled);
}

void SettingsPanel::SetInspectorSoloSectionsValue(bool enabled)
{
    if (m_InspectorSoloSectionsToggle)
        m_InspectorSoloSectionsToggle->SetChecked(enabled);
}

void SettingsPanel::SetInspectorSoloKeepTransformValue(bool enabled)
{
    if (m_InspectorSoloKeepTransformToggle)
        m_InspectorSoloKeepTransformToggle->SetChecked(enabled);
}

void SettingsPanel::SetOnInspectorToggleAlignChanged(std::function<void(const std::string&)> cb)
{
    m_OnInspectorToggleAlignChanged = std::move(cb);
    if (!m_OnInspectorToggleAlignChanged)
        return;
    std::string current = "left";
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        prefs.TryGetString("ui.inspectorToggleAlign", current);
        if (current != "left" && current != "middle" && current != "right")
            current = "left";
    }
    m_OnInspectorToggleAlignChanged(current);
    SetInspectorToggleAlignValue(current);
}

void SettingsPanel::SetInspectorToggleAlignValue(const std::string& value)
{
    const std::string v = (value == "left" || value == "middle" || value == "right") ? value : "left";
    if (m_InspectorToggleAlignLeft)
        m_InspectorToggleAlignLeft->SetChecked(v == "left");
    if (m_InspectorToggleAlignMiddle)
        m_InspectorToggleAlignMiddle->SetChecked(v == "middle");
    if (m_InspectorToggleAlignRight)
        m_InspectorToggleAlignRight->SetChecked(v == "right");
}

void SettingsPanel::OnPostLayout()
{
    // Build UI on first layout if not done yet
    if (!m_UIBuilt)
    {
        BuildUI();
        // Load accent color from preferences before applying
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            int64_t stored = static_cast<int64_t>(m_UIAccentColor);
            if (prefs.TryGetInt64(UI::AccentStyleHelper::kPrefKeyAccentColor, stored))
                m_UIAccentColor = static_cast<uint32_t>(stored);
        }
        // Apply saved accent color to UI on first build
        ApplyAccentColorToUI();

        // Load scrollbar thumb color from preferences before applying
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            int64_t stored = static_cast<int64_t>(m_UIScrollbarColor);
            if (prefs.TryGetInt64("ui.scrollbarThumbColor", stored))
                m_UIScrollbarColor = static_cast<uint32_t>(stored);
        }
        // Apply saved scrollbar thumb color to UI on first build
        ApplyScrollbarColorToUI();

        // Apply saved inspector row indent/gap/label-width on first build
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            double indent = 0.0;
            double gap = 4.0;
            double labelWidth = 37.0;
            prefs.TryGetDouble("ui.inspectorRowIndent", indent);
            prefs.TryGetDouble("ui.inspectorRowGap", gap);
            prefs.TryGetDouble("ui.inspectorLabelWidth", labelWidth);
            ApplyInspectorRowStyleToUI(static_cast<float>(indent), static_cast<float>(gap), static_cast<float>(labelWidth));
        }

        // Apply saved scene tool overlay active color on first build
        {
            std::string sceneToolColor = "accent";
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetString("ui.sceneToolActiveColor", sceneToolColor);
            ApplySceneToolActiveColor(sceneToolColor);
        }

        // Apply saved gray sliders style on first build
        ApplyGraySlidersStyleToUI();
    }
    
    // Take keyboard focus on first layout so the user can drive the category
    // tree with arrows without having to hover or click first. The key dispatch
    // "light path" prefers focused element over hovered element.
    if (m_UIBuilt && !m_InitialTreeFocusApplied)
    {
        if (auto* mgr = GetOwnerManager())
        {
            mgr->FocusElement(this);
            m_InitialTreeFocusApplied = true;
        }
    }

    // Scroll the category tree to the initially-selected row once the tree has
    // real layout. CreateTreeView runs during BuildUI and cannot compute a
    // meaningful scroll target until Yoga has sized the tree, so we defer the
    // scroll to here. (TreeView::ScrollToItem ensures the ScrollView content
    // size is set for this first-frame case where virtualization hasn't run.)
    if (m_UIBuilt && !m_InitialTreeScrollApplied && m_TreeView && m_TreeSelectionModel &&
        m_TreeView->GetLayoutHeight() > 0.0f)
    {
        const TreeId anchor = m_TreeSelectionModel->GetAnchor();
        if (anchor != 0)
        {
            m_TreeView->ScrollToItem(anchor);
            m_InitialTreeScrollApplied = true;
        }
    }

    if (m_UIBuilt && !m_RebuildScheduled)
    {
        // Check for aspect ratio changes and rebuild layout if needed
        float width = GetLayoutWidth();
        float height = GetLayoutHeight();
        
        if (width > 0 && height > 0)
        {
            float aspectRatio = width / height;
            
            // Determine the appropriate layout mode for current aspect ratio
            LayoutMode desiredMode = (aspectRatio > 1.0f) ? LayoutMode::Horizontal : LayoutMode::Vertical;
            
            // Only rebuild if mode changed (with a threshold to avoid flickering)
            if (desiredMode != m_CurrentLayoutMode && std::abs(aspectRatio - m_LastAspectRatio) > 0.15f)
            {
                m_LastAspectRatio = aspectRatio;
                m_RebuildScheduled = true;
                PostAction([this, desiredMode]() {
                    RebuildLayout(desiredMode);
                });
            }
            else if (m_CurrentLayoutMode == LayoutMode::Vertical)
            {
                // Update tree pane height for vertical layout (responsive to panel size changes)
                if (std::abs(height - m_LastPanelHeight) > 1.0f)
                {
                    m_LastPanelHeight = height;
                    UpdateTreePaneHeight();
                }
            }
        }
    }

    // A search reveal parks its scroll here: this is the first point where the
    // rebuilt page has real geometry to center against.
    ScrollPendingSearchRowIntoView();

    // Button rows whose owner made them conditional: the condition can change
    // while the page stays open (the Build panel becoming the active tab), so
    // it is polled rather than read once at build time.
    for (const auto& [row, isVisible] : m_RegistryButtonRows)
    {
        if (!row || !isVisible)
            continue;
        row->Overrides().Set(Style::Display, isVisible() ? DisplayMode::Flex : DisplayMode::None);
    }


    // Defer binding to a safe point (dispatcher drain) to avoid mutating the UI
    // tree during layout/geometry traversal.
    if (m_BindApplied || m_BindScheduled)
        return;

    if (!GetOwnerManager())
        return;

    m_BindScheduled = true;
    this->PostAction([this]()
                     { this->BindFromAssetsDeferred(); });
}

void SettingsPanel::BindFromAssetsDeferred()
{
    m_BindScheduled = false;

    UIManager* ui = GetOwnerManager();
    if (!ui)
        return;

    auto& am = EngineCore::GetInstance().GetAssetManager();

    // Load and attach the stylesheet
    const std::filesystem::path styleAssetPath = std::filesystem::path("UI") / "panels" / "SettingsPanel.css";
    const GUID styleGuid = am.ResolveAssetGuid(styleAssetPath, GameEngine::kAssetSourceAliasEditor);

    if (!styleGuid.IsNull())
    {
        // Waits on purpose: the editor's own panel .css, small and never cooked.
        auto f = am.LoadAssetAsync(styleGuid);
        auto a = f.get();
        if (a && a->GetType() == AssetType::UIStyle)
        {
            (void)ui->AttachStyleToSubtreeFromAsset(this, *static_cast<UIStyleAsset*>(a.get()));
        }
    }

    m_BindApplied = true;

    // Refresh tree display
    if (m_TreeView)
    {
        m_TreeView->RefreshFromProvider();
    }
}

namespace
{
// Snapshot index behind a registry category id, or nullopt when the id is not
// a registry category or points past the current snapshot.
std::optional<size_t> RegistryIndex(SettingsCategory category,
                                    const std::vector<Editor::SettingsCategoryDescriptor>& categories)
{
    if (!IsRegistrySettingsCategory(category))
        return std::nullopt;
    const size_t index =
        static_cast<size_t>(static_cast<uint64_t>(category) - kRegistrySettingsCategoryBase);
    if (index >= categories.size())
        return std::nullopt;
    return index;
}

// Group root (Project Settings / User Settings) for a registered category id,
// or nullopt when the id is not a registry category or out of range.
std::optional<SettingsCategory> RegistryGroupRoot(SettingsCategory category)
{
    const auto categories = Editor::EditorSettingsRegistry::Get().Snapshot();
    const auto index = RegistryIndex(category, categories);
    if (!index)
        return std::nullopt;
    return categories[*index].Group == Editor::SettingsCategoryGroup::ProjectSettings
               ? SettingsCategory::ProjectSettings
               : SettingsCategory::UserSettings;
}

// The registered page a registered sub-page hangs under, or nullopt when it
// sits directly on a group root.
std::optional<SettingsCategory> RegistryParent(SettingsCategory category)
{
    const auto categories = Editor::EditorSettingsRegistry::Get().Snapshot();
    const auto index = RegistryIndex(category, categories);
    if (!index || categories[*index].ParentCategoryId.empty())
        return std::nullopt;
    for (size_t i = 0; i < categories.size(); ++i)
    {
        if (i != *index && categories[i].CategoryId == categories[*index].ParentCategoryId)
            return static_cast<SettingsCategory>(kRegistrySettingsCategoryBase + i);
    }
    return std::nullopt;
}

// First registered sub-page of a registered page, in registration order.
std::optional<SettingsCategory> RegistryFirstChild(SettingsCategory category)
{
    const auto categories = Editor::EditorSettingsRegistry::Get().Snapshot();
    const auto index = RegistryIndex(category, categories);
    if (!index)
        return std::nullopt;
    for (size_t i = 0; i < categories.size(); ++i)
    {
        if (i != *index && categories[i].ParentCategoryId == categories[*index].CategoryId)
            return static_cast<SettingsCategory>(kRegistrySettingsCategoryBase + i);
    }
    return std::nullopt;
}
} // namespace

void SettingsPanel::OnCategorySelected(TreeId id)
{
    if (id == 0)
        return;  // Invalid selection
        
    auto category = static_cast<SettingsCategory>(id);
    
    // Parent categories: expand on first click when collapsed so the children become visible.
    // Collapsing is left to the chevron and TreeView's own double-click handler — clicking
    // an already-selected parent row must not silently fold it back up.
    if (IsParentCategory(category))
    {
        if (m_TreeView && !m_TreeView->IsExpanded(id))
        {
            m_TreeView->SetExpanded(id, true);
            m_TreeView->RefreshFromProvider();
            if (m_CurrentLayoutMode == LayoutMode::Vertical)
                UpdateTreePaneHeight();
        }
    }
    
    // Track which child was selected for each parent
    // Project Settings children: AudioSettings, Camera, Animation, Input, VersionControl
    if (category == SettingsCategory::AudioSettings ||
        category == SettingsCategory::Camera ||
        category == SettingsCategory::Animation ||
        category == SettingsCategory::Input ||
        category == SettingsCategory::Physics ||
        category == SettingsCategory::AssetImport ||
        category == SettingsCategory::Rendering ||
        category == SettingsCategory::HDROutput ||
        category == SettingsCategory::Scene ||
        category == SettingsCategory::Tags ||
        category == SettingsCategory::VersionControl)
    {
        m_LastUsedChild[SettingsCategory::ProjectSettings] = category;
    }
    // Version Control children: one dynamic tab per registered provider
    if (IsVcsProviderCategory(category))
    {
        m_LastUsedChild[SettingsCategory::VersionControl] = category;
    }
    // Registered categories: track under their parent page, and track that
    // parent under the group root, so a sub-page reopens its whole chain.
    else if (const auto groupRoot = RegistryGroupRoot(category))
    {
        if (const auto parent = RegistryParent(category))
        {
            m_LastUsedChild[*parent] = category;
            m_LastUsedChild[*groupRoot] = *parent;
        }
        else
        {
            m_LastUsedChild[*groupRoot] = category;
        }
    }
    // UI children: Colors, Appearance, FontSizes, Trees, Assets, Inspector, HiDPI
    if (category == SettingsCategory::UIAppearance ||
        category == SettingsCategory::UIFontSizes ||
        category == SettingsCategory::UITrees ||
        category == SettingsCategory::UIAssets ||
        category == SettingsCategory::UIInspector ||
        category == SettingsCategory::UIHiDpi)
    {
        m_LastUsedChild[SettingsCategory::UI] = category;
        m_LastUsedChild[SettingsCategory::UserSettings] = SettingsCategory::UI;
    }
    // User Settings children: Script, UI, Shortcuts, Gizmo, Tooltips
    else if (category == SettingsCategory::Script ||
             category == SettingsCategory::UI ||
             category == SettingsCategory::Shortcuts ||
             category == SettingsCategory::Gizmo ||
             category == SettingsCategory::Performance)
    {
        m_LastUsedChild[SettingsCategory::UserSettings] = category;
    }
    
    m_CurrentCategory = category;
    
    // Defer content update to avoid infinite loop during event dispatch
    // (RemoveChild defers when called during dispatch, causing the clear loop to never terminate)
    this->PostAction([this, category]() {
        ShowCategoryContent(category);
    });
}

bool SettingsPanel::IsParentCategory(SettingsCategory category) const
{
    if (IsRegistrySettingsCategory(category))
        return RegistryFirstChild(category).has_value();
    return category == SettingsCategory::ProjectSettings ||
           category == SettingsCategory::UserSettings ||
           category == SettingsCategory::VersionControl ||
           category == SettingsCategory::UI;
}

SettingsCategory SettingsPanel::GetDefaultChild(SettingsCategory parent) const
{
    if (IsRegistrySettingsCategory(parent))
        return RegistryFirstChild(parent).value_or(parent);

    switch (parent)
    {
        case SettingsCategory::ProjectSettings:
            return SettingsCategory::AudioSettings;  // First child
        case SettingsCategory::UserSettings:
            return SettingsCategory::Script;  // First child
        case SettingsCategory::VersionControl:
        {
            // First registered provider tab; the parent page when none exist.
            if (!Editor::EditorVcsProviderRegistry::Get().Snapshot().empty())
                return static_cast<SettingsCategory>(kVcsProviderCategoryBase);
            return SettingsCategory::VersionControl;
        }
        case SettingsCategory::UI:
            return SettingsCategory::UIAppearance;  // First child
        default:
            return parent;
    }
}

void SettingsPanel::ClearContentPane()
{
    if (!m_ContentBody)
        return;

    /* Clear search highlight state before destroying content so we never touch destroyed elements in ClearSearchHighlights. */
    m_RegistryButtonRows.clear();
    // Any reveal still waiting to scroll belongs to the page being torn down.
    m_PendingSearchScroll.reset();
    m_SearchHighlightedElements.clear();
    m_SearchFilteredElements.clear();
    m_SearchSegmentReplacements.clear();
    
    UIElement* viewport = m_ContentBody->GetViewport();
    if (viewport)
    {
        // Removals can be deferred during event dispatch. Snapshotting avoids
        // waiting for the live child list to shrink synchronously.
        std::vector<UIElement*> toRemove;
        toRemove.reserve(viewport->GetChildren().size());
        for (const auto& child : viewport->GetChildren())
        {
            if (child)
                toRemove.push_back(child.get());
        }
        for (UIElement* child : toRemove)
            viewport->RemoveChild(child);
    }
    m_PageColorPickers.CloseAll();
    
    // Clear element pointers
    m_HierarchyTreeChildIndentSlider = nullptr;
    m_HierarchyTreeChildIndentLabel = nullptr;
    m_HierarchyTreeRowHeightSlider = nullptr;
    m_HierarchyTreeRowHeightLabel = nullptr;
    m_HierarchyTreeIconSizeSlider = nullptr;
    m_HierarchyTreeIconSizeLabel = nullptr;
    m_HierarchyTreeIconSizeField = nullptr;
    m_AssetsTreeChildIndentSlider = nullptr;
    m_AssetsTreeChildIndentLabel = nullptr;
    m_AssetsTreeRowHeightSlider = nullptr;
    m_AssetsTreeRowHeightLabel = nullptr;
    m_AssetsTreeIconSizeSlider = nullptr;
    m_AssetsTreeIconSizeLabel = nullptr;
    m_AssetsTreeIconSizeField = nullptr;
    m_GridIconSizeSlider = nullptr;
    m_GridIconSizeField = nullptr;
    m_SearchBarsToggle = nullptr;
    m_CheckmarkTogglesToggle = nullptr;
    m_GraySlidersToggle = nullptr;
    m_TabIconsToggle = nullptr;
    m_FontSmoothingField = nullptr;
    m_FontSizeBaseSlider = nullptr;
    m_FontSizeTreeSlider = nullptr;
    m_FontSizeSmallSlider = nullptr;
    m_FontSizeHeaderSlider = nullptr;
    
    // Clear syntax highlighting field pointers
    m_SyntaxKeywordColorField = nullptr;
    m_SyntaxStringColorField = nullptr;
    m_SyntaxCommentColorField = nullptr;
    m_SyntaxNumberColorField = nullptr;
    m_SyntaxTypeColorField = nullptr;
    m_SyntaxDefaultColorField = nullptr;
    
    // Clear script settings pointers
    m_ScriptOpenInInspectorToggle = nullptr;
    m_ShaderGraphGlslOpenToggle = nullptr;
    
    m_UIAccentColorSwatch = nullptr;
    m_UIAssetIconTintSwatch = nullptr;
    m_UISmartFolderIconTintSwatch = nullptr;
    m_UIScrollbarColorSwatch = nullptr;
    m_SelectionBoxColorSwatch = nullptr;
    m_SelectionOutlineColorSwatch = nullptr;
    m_SceneViewBackgroundColorSwatch = nullptr;
    m_RulerIndicatorColorSwatch = nullptr;
    m_MeasureColorSwatch = nullptr;
    m_GridColor3DSwatch = nullptr;
    m_GridColor2DSwatch = nullptr;
    g_ActivePipelineDropdown = nullptr;

    // Clear gizmo settings pointers
    m_GizmoThicknessSection = nullptr;
    m_GizmoConstantSizeSection = nullptr;
    m_TranslateGizmoThicknessSlider = nullptr;
    m_RotateGizmoThicknessSlider = nullptr;
    m_ScaleGizmoThicknessSlider = nullptr;
    m_TranslateGizmoScaleSlider = nullptr;
    m_RotateGizmoScaleSlider = nullptr;
    m_ScaleGizmoScaleSlider = nullptr;
    m_TranslateConstantThicknessSlider = nullptr;
    m_RotateConstantThicknessSlider = nullptr;
    m_ScaleConstantThicknessSlider = nullptr;
}

void SettingsPanel::RequestShowCategory(SettingsCategory category)
{
    // Expand parent(s) so the category row is visible in the tree
    if (m_TreeView)
    {
        if (category == SettingsCategory::AudioSettings || category == SettingsCategory::Camera ||
            category == SettingsCategory::Animation || category == SettingsCategory::Input ||
            category == SettingsCategory::Physics ||
            category == SettingsCategory::AssetImport ||
            category == SettingsCategory::Rendering || category == SettingsCategory::HDROutput ||
            category == SettingsCategory::Scene ||
            category == SettingsCategory::Tags ||
            category == SettingsCategory::VersionControl)
        {
            m_TreeView->SetExpanded(static_cast<TreeId>(SettingsCategory::ProjectSettings), true);
        }
        if (IsVcsProviderCategory(category))
        {
            m_TreeView->SetExpanded(static_cast<TreeId>(SettingsCategory::ProjectSettings), true);
            m_TreeView->SetExpanded(static_cast<TreeId>(SettingsCategory::VersionControl), true);
        }
        if (const auto groupRoot = RegistryGroupRoot(category))
        {
            m_TreeView->SetExpanded(static_cast<TreeId>(*groupRoot), true);
            const auto categories = Editor::EditorSettingsRegistry::Get().Snapshot();
            const auto index = RegistryIndex(category, categories);
            if (index && categories[*index].Group == Editor::SettingsCategoryGroup::UI)
                m_TreeView->SetExpanded(static_cast<TreeId>(SettingsCategory::UI), true);
        }
        if (category == SettingsCategory::UIAppearance ||
            category == SettingsCategory::UIFontSizes || category == SettingsCategory::UITrees ||
            category == SettingsCategory::UIAssets || category == SettingsCategory::UIInspector ||
            category == SettingsCategory::UIHiDpi)
        {
            m_TreeView->SetExpanded(static_cast<TreeId>(SettingsCategory::UserSettings), true);
            m_TreeView->SetExpanded(static_cast<TreeId>(SettingsCategory::UI), true);
        }
        else if (category == SettingsCategory::Script || category == SettingsCategory::UI ||
            category == SettingsCategory::Shortcuts || category == SettingsCategory::Gizmo ||
            category == SettingsCategory::GridAndSnapping)
        {
            m_TreeView->SetExpanded(static_cast<TreeId>(SettingsCategory::UserSettings), true);
        }
        m_TreeView->RefreshFromProvider();
    }
    // Select the category in the tree and show its content
    if (m_TreeSelectionModel)
        m_TreeSelectionModel->SetSingle(static_cast<TreeId>(category));
    if (m_TreeView)
    {
        m_TreeView->RefreshFromProvider();
        m_TreeView->ScrollToItem(static_cast<TreeId>(category));
    }
    OnCategorySelected(static_cast<TreeId>(category));
}

void SettingsPanel::RequestShowRegistryCategory(std::string_view categoryId)
{
    const auto categories = Editor::EditorSettingsRegistry::Get().Snapshot();
    for (size_t i = 0; i < categories.size(); ++i)
    {
        if (categories[i].CategoryId == categoryId)
        {
            SettingsCategory target =
                static_cast<SettingsCategory>(kRegistrySettingsCategoryBase + i);
            if (categories[i].Group == Editor::SettingsCategoryGroup::VersionControl)
                target = SettingsCategory::VersionControl;
            else if (categories[i].Group == Editor::SettingsCategoryGroup::UIAppearance)
                target = SettingsCategory::UIAppearance;
            RequestShowCategory(target);
            return;
        }
    }
}

void SettingsPanel::ShowCategoryContent(SettingsCategory category)
{
    ClearContentPane();
    
    if (!m_ContentHeader || !m_ContentBody)
        return;

    // Update header
    const char* headerText = "Settings";

    // Dynamic per-provider VCS tabs dispatch through the provider registry.
    if (IsVcsProviderCategory(category))
    {
        const size_t index =
            static_cast<size_t>(static_cast<uint64_t>(category) - kVcsProviderCategoryBase);
        const auto providers = Editor::EditorVcsProviderRegistry::Get().Snapshot();
        std::string dynamicHeader = "Project Settings - Version Control";
        if (index < providers.size())
        {
            dynamicHeader = "Project Settings - " + providers[index].DisplayName;
            CreateVcsProviderContent(providers[index].TypeId);
        }
        if (auto* label = dynamic_cast<Label*>(m_ContentHeader))
            label->SetText(dynamicHeader);
        if (m_ContentBody)
            m_ContentBody->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty |
                                     UIElement::ChildrenDirty);
        return;
    }

    // Categories registered through Editor::EditorSettingsRegistry render
    // straight from their descriptors.
    if (IsRegistrySettingsCategory(category))
    {
        const size_t index =
            static_cast<size_t>(static_cast<uint64_t>(category) - kRegistrySettingsCategoryBase);
        const auto categories = Editor::EditorSettingsRegistry::Get().Snapshot();
        std::string dynamicHeader = "Settings";
        if (index < categories.size())
        {
            const Editor::SettingsCategoryDescriptor& descriptor = categories[index];
            if (descriptor.Group == Editor::SettingsCategoryGroup::ProjectSettings)
                dynamicHeader = "Project Settings - " + descriptor.Title;
            else if (descriptor.Group == Editor::SettingsCategoryGroup::UI)
                dynamicHeader = "UI - " + descriptor.Title;
            else
                dynamicHeader = "User Settings - " + descriptor.Title;
            CreateRegistrySettingsContent(descriptor);
        }
        if (auto* label = dynamic_cast<Label*>(m_ContentHeader))
            label->SetText(dynamicHeader);
        if (m_ContentBody)
            m_ContentBody->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty |
                                     UIElement::ChildrenDirty);
        return;
    }

    switch (category)
    {
        case SettingsCategory::ProjectSettings:
            headerText = "Project Settings";
            CreateProjectSettingsContent();
            break;
        case SettingsCategory::UserSettings:
            headerText = "User Settings";
            CreateUserSettingsContent();
            break;
        // Project Settings children
        case SettingsCategory::AudioSettings:
            headerText = "Project Settings - Audio";
            CreateAudioSettingsContent();
            break;
        case SettingsCategory::Camera:
            headerText = "Project Settings - Camera";
            CreateCameraSettingsContent();
            break;
        case SettingsCategory::Animation:
            headerText = "Project Settings - Animation";
            CreateAnimationSettingsContent();
            break;
        case SettingsCategory::Input:
            headerText = "Project Settings - Input";
            CreateInputSettingsContent();
            break;
        case SettingsCategory::Physics:
            headerText = "Project Settings - Physics";
            CreatePhysicsSettingsContent();
            break;
        case SettingsCategory::AssetImport:
            headerText = "Project Settings - Asset Import";
            CreateAssetImportSettingsContent();
            break;
        case SettingsCategory::Rendering:
            headerText = "Project Settings - Rendering";
            CreateRenderingSettingsContent();
            break;
        case SettingsCategory::HDROutput:
            headerText = "Project Settings - HDR Output";
            CreateHDROutputSettingsContent();
            break;
        case SettingsCategory::Scene:
            headerText = "Project Settings - Scene";
            CreateSceneSettingsContent();
            break;
        case SettingsCategory::VersionControl:
            headerText = "Project Settings - Version Control";
            CreateVersionControlContent();
            break;
        case SettingsCategory::Tags:
            headerText = "Project Settings - Tags";
            CreateTagsSettingsContent();
            break;
        // User Settings children
        case SettingsCategory::Script:
            headerText = "User Settings - Script";
            CreateScriptSettingsContent();
            break;
        case SettingsCategory::UI:
            headerText = "User Settings - UI";
            CreateUISettingsContent();
            break;
        case SettingsCategory::UIAppearance:
            headerText = "UI - Appearance";
            CreateUIAppearanceContent();
            break;
        case SettingsCategory::UIFontSizes:
            headerText = "UI - Font Rendering";
            CreateUIFontSizesContent();
            break;
        case SettingsCategory::UITrees:
            headerText = "UI - Trees";
            CreateUITreesContent();
            break;
        case SettingsCategory::UIAssets:
            headerText = "UI - Assets";
            CreateUIAssetsContent();
            break;
        case SettingsCategory::UIHiDpi:
            headerText = "UI - HiDPI";
            CreateUIHiDpiContent();
            break;
        case SettingsCategory::UIInspector:
            headerText = "UI - Inspector";
            CreateUIInspectorContent();
            break;
        case SettingsCategory::Shortcuts:
            headerText = "User Settings - Keyboard Shortcuts";
            CreateShortcutsSettingsContent();
            break;
        case SettingsCategory::Gizmo:
            headerText = "User Settings - Gizmo";
            CreateGizmoSettingsContent();
            break;
        case SettingsCategory::GridAndSnapping:
            headerText = "User Settings - Grid & Snapping";
            CreateGridAndSnappingContent();
            break;
        case SettingsCategory::Performance:
            headerText = "User Settings - Performance";
            CreatePerformanceSettingsContent();
            break;
        default:
            break;
    }
    
    if (auto* label = dynamic_cast<Label*>(m_ContentHeader))
    {
        label->SetText(headerText);
    }
    
    // Request relayout
    if (m_ContentBody)
    {
        m_ContentBody->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty | UIElement::ChildrenDirty);
    }

    // When search is active, highlight the query in the newly shown content.
    if (!m_CurrentSearchQuery.empty())
        ApplySearchFilterToContent(m_CurrentSearchQuery);
}

void SettingsPanel::CreateUISettingsContent()
{
    if (!m_ContentBody)
        return;

    auto desc = MakeSettingsInfoCard(
        "Customize the editor's visual style, fonts, layout, and controls. Select a sub-category "
        "from the tree to configure specific settings.");
    desc->AddClass("settings-overview-text");
    m_ContentBody->AddContent(std::move(desc));
}

void SettingsPanel::CreateUIColorsContent()
{
    if (!m_ContentBody)
        return;

    // Load accent color from preferences
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        int64_t stored = static_cast<int64_t>(m_UIAccentColor);
        if (prefs.TryGetInt64(UI::AccentStyleHelper::kPrefKeyAccentColor, stored))
            m_UIAccentColor = static_cast<uint32_t>(stored);
    }
    
    // Accent Color: 16x16 clickable swatch that opens the color picker
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        auto label = std::make_unique<Label>();
        label->SetText("Accent Color");
        label->AddClass("settings-row-label");
        Label* accentLabelPtr = label.get();
        section->AddChild(std::move(label));
        auto openColorPicker = [this](UIEvent& e) {
            if (e.Button != 0)
                return;
            const uint32_t original = m_UIAccentColor;
            OpenColorPicker(
                original,
                [this](uint32_t argb)
                {
                    m_UIAccentColor = argb;
                    auto prefs = Editor::OpenEditorPreferences();
                    std::string err;
                    prefs.Load(&err);
                    prefs.SetInt64(UI::AccentStyleHelper::kPrefKeyAccentColor, static_cast<int64_t>(m_UIAccentColor));
                    prefs.Save(&err);
                    UpdateAccentSwatchStyle();
                    ApplyAccentColorToUI();
                },
                [this, original]()
                {
                    m_UIAccentColor = original;
                    UpdateAccentSwatchStyle();
                    ApplyAccentColorToUI();
                },
                [this](uint32_t argb)
                {
                    m_UIAccentColor = argb;
                    UpdateAccentSwatchStyle();
                    ApplyAccentColorToUI();
                });
            e.Stop();
        };
        auto swatch = std::make_unique<UIElement>();
        swatch->SetId("settings-accent-color-swatch");
        m_UIAccentColorSwatch = swatch.get();
        UpdateAccentSwatchStyle();
        swatch->RegisterEventHandler(kEventMouseDown, openColorPicker);
        section->AddChild(std::move(swatch));
        AddDoubleClickReset(accentLabelPtr, [this]() {
            m_UIAccentColor = UI::AccentStyleHelper::kDefaultAccentColor;
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetInt64(UI::AccentStyleHelper::kPrefKeyAccentColor, static_cast<int64_t>(m_UIAccentColor));
            prefs.Save(&err);
            UpdateAccentSwatchStyle();
            ApplyAccentColorToUI();
        });
        m_ContentBody->AddContent(std::move(section));
    }

    // Curve Time Dot: the dot rendered on reusable inspector curve time indicators.
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Curve Time Dot");
        label->AddClass("settings-row-label");
        Label* curveDotLabelPtr = label.get();
        section->AddChild(std::move(label));

        auto swatch = std::make_unique<UIElement>();
        swatch->SetId("settings-curve-current-time-dot-swatch");
        UIElement* curveDotSwatch = swatch.get();
        UpdateSwatchStyle(curveDotSwatch, Editor::CurveEditorSettings::Get().PlaybackIndicatorDotColor);

        auto applyCurveDotColor = [this, curveDotSwatch](uint32_t argb, bool save)
        {
            auto& settings = Editor::CurveEditorSettings::Get();
            settings.PlaybackIndicatorDotColor = 0xFF000000u | (argb & 0x00FFFFFFu);
            if (save)
                settings.Save();
            UpdateSwatchStyle(curveDotSwatch, settings.PlaybackIndicatorDotColor);
            if (m_UIManager)
                MarkCurveIndicatorControlsDirty(m_UIManager->GetRootElement());
        };

        swatch->RegisterEventHandler(kEventMouseDown, [this, applyCurveDotColor](UIEvent& e)
        {
            if (e.Button != 0)
                return;

            const uint32_t original = Editor::CurveEditorSettings::Get().PlaybackIndicatorDotColor;
            OpenColorPicker(
                original,
                [applyCurveDotColor](uint32_t argb) { applyCurveDotColor(argb, true); },
                [applyCurveDotColor, original]() { applyCurveDotColor(original, false); },
                [applyCurveDotColor](uint32_t argb) { applyCurveDotColor(argb, false); });
            e.Stop();
        });
        section->AddChild(std::move(swatch));

        AddDoubleClickReset(curveDotLabelPtr, [applyCurveDotColor]()
        {
            applyCurveDotColor(Editor::CurveEditorSettings::kDefaultPlaybackIndicatorDotColor, true);
        });

        m_ContentBody->AddContent(std::move(section));
    }

    // Asset Icon Tint: 16x16 clickable swatch that opens the color picker
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Asset Icon Tint");
        label->AddClass("settings-row-label");
        Label* iconTintLabelPtr = label.get();
        section->AddChild(std::move(label));

        // Load initial value from preferences (default: #AAAAAA)
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            int64_t stored = static_cast<int64_t>(m_UIAssetIconTint);
            if (prefs.TryGetInt64("ui.assetIconTint", stored))
                m_UIAssetIconTint = static_cast<uint32_t>(stored);
        }

        auto openAssetIconTintPicker = [this](UIEvent& e)
        {
            if (e.Button != 0)
                return;
            const uint32_t original = m_UIAssetIconTint;
            OpenColorPicker(
                original,
                [this](uint32_t argb)
                {
                    m_UIAssetIconTint = argb;
                    auto prefs = Editor::OpenEditorPreferences();
                    std::string err;
                    prefs.Load(&err);
                    prefs.SetInt64("ui.assetIconTint", static_cast<int64_t>(m_UIAssetIconTint));
                    prefs.Save(&err);
                    UpdateAssetIconTintSwatchStyle();

                    if (m_UIManager)
                    {
                        if (auto* root = m_UIManager->GetRootElement())
                        {
                            // Update CSS variable used by views.css for icon tints.
                            char hex[16];
                            std::snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                                          (m_UIAssetIconTint >> 16) & 0xFF,
                                          (m_UIAssetIconTint >> 8) & 0xFF,
                                          m_UIAssetIconTint & 0xFF);
                            root->Overrides().SetCustom(
                                HashStringId("--ui_asset_icon_tint"),
                                std::string(hex));
                            m_UIManager->MarkStyleDirtySubtree(root);
                        }
                    }
                },
                [this, original]()
                {
                    m_UIAssetIconTint = original;
                    UpdateAssetIconTintSwatchStyle();

                    if (m_UIManager)
                    {
                        if (auto* root = m_UIManager->GetRootElement())
                        {
                            char hex[16];
                            std::snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                                          (m_UIAssetIconTint >> 16) & 0xFF,
                                          (m_UIAssetIconTint >> 8) & 0xFF,
                                          m_UIAssetIconTint & 0xFF);
                            root->Overrides().SetCustom(
                                HashStringId("--ui_asset_icon_tint"),
                                std::string(hex));
                            m_UIManager->MarkStyleDirtySubtree(root);
                        }
                    }
                },
                [this](uint32_t argb)
                {
                    m_UIAssetIconTint = argb;
                    UpdateAssetIconTintSwatchStyle();

                    if (m_UIManager)
                    {
                        if (auto* root = m_UIManager->GetRootElement())
                        {
                            char hex[16];
                            std::snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                                          (m_UIAssetIconTint >> 16) & 0xFF,
                                          (m_UIAssetIconTint >> 8) & 0xFF,
                                          m_UIAssetIconTint & 0xFF);
                            root->Overrides().SetCustom(
                                HashStringId("--ui_asset_icon_tint"),
                                std::string(hex));
                            m_UIManager->MarkStyleDirtySubtree(root);
                        }
                    }
                });
            e.Stop();
        };

        auto swatch = std::make_unique<UIElement>();
        swatch->SetId("settings-asset-icon-tint-swatch");
        m_UIAssetIconTintSwatch = swatch.get();
        UpdateAssetIconTintSwatchStyle();
        swatch->RegisterEventHandler(kEventMouseDown, openAssetIconTintPicker);
        section->AddChild(std::move(swatch));

        const uint32_t kDefaultAssetIconTint = 0xFFAAAAAA;
        AddDoubleClickReset(iconTintLabelPtr, [this, kDefaultAssetIconTint]()
        {
            m_UIAssetIconTint = kDefaultAssetIconTint;
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetInt64("ui.assetIconTint", static_cast<int64_t>(m_UIAssetIconTint));
            prefs.Save(&err);
            UpdateAssetIconTintSwatchStyle();

            if (m_UIManager)
            {
                if (auto* root = m_UIManager->GetRootElement())
                {
                    char hex[16];
                    std::snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                                  (m_UIAssetIconTint >> 16) & 0xFF,
                                  (m_UIAssetIconTint >> 8) & 0xFF,
                                  m_UIAssetIconTint & 0xFF);
                    root->Overrides().SetCustom(
                        HashStringId("--ui_asset_icon_tint"),
                        std::string(hex));
                    m_UIManager->MarkStyleDirtySubtree(root);
                }
            }
        });
        m_ContentBody->AddContent(std::move(section));
    }

    // Colored Hierarchy Icons: toggle between grayscale-tinted and natural-color hierarchy icons
    {
        SettingsToggleConfig cfg;
        cfg.labelText = "Colored Hierarchy Icons";
        cfg.defaultValue = true;
        cfg.prefKey = kPrefKeyHierarchyIconsColored;
        cfg.onValueChanged = [](bool v)
        {
            HierarchyPanel::NotifyColoredIconsChanged(v);
        };
        CreateSettingsToggleRow(cfg, m_ContentBody);
    }

    // 3D Model Thumbnails Always Colored: keep hierarchy thumbnails for models in natural color
    // even when Colored Hierarchy Icons is off (the rest of the tree stays desaturated).
    {
        SettingsToggleConfig cfg;
        cfg.labelText = "Model Thumbs Always Colored";
        cfg.tooltip = "3D Model Thumbnails Always Colored";
        cfg.defaultValue = true;
        cfg.prefKey = kPrefKeyHierarchyModelThumbsAlwaysColored;
        cfg.onValueChanged = [](bool v)
        {
            HierarchyPanel::NotifyModelThumbsAlwaysColoredChanged(v);
        };
        CreateSettingsToggleRow(cfg, m_ContentBody);
    }

    // Smart Folder Icon Tint: 16x16 clickable swatch that opens the color picker
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Smart Folder Icon Tint");
        label->AddClass("settings-row-label");
        Label* smartLabelPtr = label.get();
        section->AddChild(std::move(label));

        // Load initial value from preferences (default: smartfolders base blue)
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            int64_t stored = static_cast<int64_t>(m_UISmartFolderIconTint);
            if (prefs.TryGetInt64("ui.smartFolderIconTint", stored))
                m_UISmartFolderIconTint = static_cast<uint32_t>(stored);
        }

        auto openSmartFolderTintPicker = [this](UIEvent& e)
        {
            if (e.Button != 0)
                return;
            const uint32_t original = m_UISmartFolderIconTint;
            OpenColorPicker(
                original,
                [this](uint32_t argb)
                {
                    m_UISmartFolderIconTint = argb;
                    auto prefs = Editor::OpenEditorPreferences();
                    std::string err;
                    prefs.Load(&err);
                    prefs.SetInt64("ui.smartFolderIconTint", static_cast<int64_t>(m_UISmartFolderIconTint));
                    prefs.Save(&err);
                    UpdateSmartFolderIconTintSwatchStyle();

                    if (m_UIManager)
                    {
                        if (auto* root = m_UIManager->GetRootElement())
                        {
                            char hex[16];
                            std::snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                                          (m_UISmartFolderIconTint >> 16) & 0xFF,
                                          (m_UISmartFolderIconTint >> 8) & 0xFF,
                                          m_UISmartFolderIconTint & 0xFF);
                            root->Overrides().SetCustom(
                                HashStringId("--ui_smartfolder_icon_tint"),
                                std::string(hex));
                            m_UIManager->MarkStyleDirtySubtree(root);
                        }
                    }
                },
                [this, original]()
                {
                    m_UISmartFolderIconTint = original;
                    UpdateSmartFolderIconTintSwatchStyle();

                    if (m_UIManager)
                    {
                        if (auto* root = m_UIManager->GetRootElement())
                        {
                            char hex[16];
                            std::snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                                          (m_UISmartFolderIconTint >> 16) & 0xFF,
                                          (m_UISmartFolderIconTint >> 8) & 0xFF,
                                          m_UISmartFolderIconTint & 0xFF);
                            root->Overrides().SetCustom(
                                HashStringId("--ui_smartfolder_icon_tint"),
                                std::string(hex));
                            m_UIManager->MarkStyleDirtySubtree(root);
                        }
                    }
                },
                [this](uint32_t argb)
                {
                    m_UISmartFolderIconTint = argb;
                    UpdateSmartFolderIconTintSwatchStyle();

                    if (m_UIManager)
                    {
                        if (auto* root = m_UIManager->GetRootElement())
                        {
                            char hex[16];
                            std::snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                                          (m_UISmartFolderIconTint >> 16) & 0xFF,
                                          (m_UISmartFolderIconTint >> 8) & 0xFF,
                                          m_UISmartFolderIconTint & 0xFF);
                            root->Overrides().SetCustom(
                                HashStringId("--ui_smartfolder_icon_tint"),
                                std::string(hex));
                            m_UIManager->MarkStyleDirtySubtree(root);
                        }
                    }
                });
            e.Stop();
        };

        auto swatch = std::make_unique<UIElement>();
        swatch->SetId("settings-smart-folder-icon-tint-swatch");
        m_UISmartFolderIconTintSwatch = swatch.get();
        UpdateSmartFolderIconTintSwatchStyle();
        swatch->RegisterEventHandler(kEventMouseDown, openSmartFolderTintPicker);
        section->AddChild(std::move(swatch));

        const uint32_t kDefaultSmartFolderIconTint = 0xFF6BA4F8;
        AddDoubleClickReset(smartLabelPtr, [this, kDefaultSmartFolderIconTint]()
        {
            m_UISmartFolderIconTint = kDefaultSmartFolderIconTint;
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetInt64("ui.smartFolderIconTint", static_cast<int64_t>(m_UISmartFolderIconTint));
            prefs.Save(&err);
            UpdateSmartFolderIconTintSwatchStyle();

            if (m_UIManager)
            {
                if (auto* root = m_UIManager->GetRootElement())
                {
                    char hex[16];
                    std::snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                                  (m_UISmartFolderIconTint >> 16) & 0xFF,
                                  (m_UISmartFolderIconTint >> 8) & 0xFF,
                                  m_UISmartFolderIconTint & 0xFF);
                    root->Overrides().SetCustom(
                        HashStringId("--ui_smartfolder_icon_tint"),
                        std::string(hex));
                    m_UIManager->MarkStyleDirtySubtree(root);
                }
            }
        });
        m_ContentBody->AddContent(std::move(section));
    }

    // Scrollbar Thumb Color: 16x16 clickable swatch that opens the color picker
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Scrollbar Thumb Color");
        label->AddClass("settings-row-label");
        Label* scrollbarLabelPtr = label.get();
        section->AddChild(std::move(label));

        auto openScrollbarColorPicker = [this](UIEvent& e) {
            if (e.Button != 0)
                return;
            const uint32_t original = m_UIScrollbarColor;
            OpenColorPicker(
                original,
                // Apply
                [this](uint32_t argb)
                {
                    m_UIScrollbarColor = argb;
                    auto prefs = Editor::OpenEditorPreferences();
                    std::string err;
                    prefs.Load(&err);
                    prefs.SetInt64("ui.scrollbarThumbColor", static_cast<int64_t>(m_UIScrollbarColor));
                    prefs.Save(&err);

                    ApplyScrollbarColorToUI();

                    if (m_UIScrollbarColorSwatch)
                    {
                        m_UIScrollbarColorSwatch->Overrides()
                            .Set(Style::Width, StyleLength::Px(16.0f))
                            .Set(Style::Height, StyleLength::Px(16.0f))
                            .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{2, 2, 2, 2})
                            .Set(Style::BorderWidth, Box4{1, 1, 1, 1})
                            .Set(Style::BorderColor, BorderColorsTRBL{0xFF3E3E3Eu, 0xFF3E3E3Eu, 0xFF3E3E3Eu, 0xFF3E3E3Eu})
                            .Set(Style::BackgroundColor, m_UIScrollbarColor);
                    }
                },
                // Cancel: revert to original color and undo any live previews
                [this, original]()
                {
                    m_UIScrollbarColor = original;
                    ApplyScrollbarColorToUI();

                    if (m_UIScrollbarColorSwatch)
                    {
                        m_UIScrollbarColorSwatch->Overrides()
                            .Set(Style::BackgroundColor, m_UIScrollbarColor);
                    }
                },
                // Live preview
                [this](uint32_t argb)
                {
                    m_UIScrollbarColor = argb;
                    ApplyScrollbarColorToUI();

                    if (m_UIScrollbarColorSwatch)
                    {
                        m_UIScrollbarColorSwatch->Overrides()
                            .Set(Style::BackgroundColor, m_UIScrollbarColor);
                    }
                });
            e.Stop();
        };

        auto swatch = std::make_unique<UIElement>();
        swatch->SetId("settings-scrollbar-color-swatch");
        m_UIScrollbarColorSwatch = swatch.get();
        swatch->Overrides()
            .Set(Style::Width, StyleLength::Px(16.0f))
            .Set(Style::Height, StyleLength::Px(16.0f))
            .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{2, 2, 2, 2})
            .Set(Style::BorderWidth, Box4{1, 1, 1, 1})
            .Set(Style::BorderColor, BorderColorsTRBL{0xFF3E3E3Eu, 0xFF3E3E3Eu, 0xFF3E3E3Eu, 0xFF3E3E3Eu})
            .Set(Style::BackgroundColor, m_UIScrollbarColor);
        swatch->RegisterEventHandler(kEventMouseDown, openScrollbarColorPicker);
        section->AddChild(std::move(swatch));
        const uint32_t kDefaultScrollbarColor = 0xFF383838;
        AddDoubleClickReset(scrollbarLabelPtr, [this]() {
            m_UIScrollbarColor = kDefaultScrollbarColor;
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetInt64("ui.scrollbarThumbColor", static_cast<int64_t>(m_UIScrollbarColor));
            prefs.Save(&err);
            ApplyScrollbarColorToUI();
            if (m_UIScrollbarColorSwatch)
            {
                m_UIScrollbarColorSwatch->Overrides()
                    .Set(Style::BackgroundColor, m_UIScrollbarColor);
            }
        });
        m_ContentBody->AddContent(std::move(section));
    }

}

void SettingsPanel::CreateUIAppearanceContent()
{
    if (!m_ContentBody)
        return;

    CreateUIColorsContent();

    m_ContentBody->AddContent(BuildSvgRasterSizeRow(Editor::SvgRasterSettingsScope::EditorUi));

    // Editor-wide boolean control presentation.
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");

        constexpr bool kDefaultUseCheckmarks = false;
        bool useCheckmarks = kDefaultUseCheckmarks;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetBool(kPrefKeyCheckmarkToggles, useCheckmarks);
        }

        auto toggle = std::make_unique<Toggle>();
        m_CheckmarkTogglesToggle = toggle.get();
        toggle->AddClass("settings-toggle");
        toggle->SetChecked(useCheckmarks);
        toggle->SetTooltip("Display boolean toggles as square checkmark boxes instead of sliding switches.");
        toggle->SetOnValueChanged([this](const bool& enabled) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool(kPrefKeyCheckmarkToggles, enabled);
            prefs.Save(&err);

            ApplyToggleStyle(m_UIManager ? m_UIManager : GetOwnerManager(), enabled);
            if (m_OnToggleStyleChanged)
                m_OnToggleStyleChanged(enabled);
        });
        section->AddChild(std::move(toggle));

        auto label = std::make_unique<Label>();
        label->SetText("Use Checkmarks for Toggles");
        label->SetTooltip("Display boolean toggles as square checkmark boxes instead of sliding switches.");
        label->AddClass("settings-row-label");
        Label* labelPtr = label.get();
        section->InsertChild(0, std::move(label));

        AddDoubleClickReset(labelPtr, [this]() {
            constexpr bool kDefaultUseCheckmarks = false;
            if (m_CheckmarkTogglesToggle)
                m_CheckmarkTogglesToggle->SetChecked(kDefaultUseCheckmarks);

            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool(kPrefKeyCheckmarkToggles, kDefaultUseCheckmarks);
            prefs.Save(&err);

            ApplyToggleStyle(m_UIManager ? m_UIManager : GetOwnerManager(), kDefaultUseCheckmarks);
            if (m_OnToggleStyleChanged)
                m_OnToggleStyleChanged(kDefaultUseCheckmarks);
        });

        m_ContentBody->AddContent(std::move(section));
    }

    // Global value box height (inspector fields + all settings inputs).
    {
        SettingsSliderConfig cfg;
        cfg.labelText = "Value Box Height";
        cfg.defaultValue = kDefaultValueBoxHeight;
        cfg.minValue = 18.0f;
        cfg.maxValue = 40.0f;
        cfg.step = 1.0f;
        cfg.prefKey = kPrefKeyValueBoxHeight;
        auto applyHeight = [this](float v) {
            UIManager* ui = GetOwnerManager();
            if (!ui)
                return;
            UIElement* root = ui->GetRootElement();
            if (!root)
                return;
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%.0fpx", v);
            root->Overrides().SetCustom(HashStringId("--value_box_height"), buf);
            ui->MarkStyleDirtyAll();
        };
        cfg.onValueChanged = applyHeight;
        cfg.onValueChanging = applyHeight;
        CreateSettingsSliderRow(cfg, m_ContentBody);
    }

    // (Slug text weight threshold moved to the Font Rendering tab.)

    // Search Bars toggle
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");

        auto toggle = std::make_unique<Toggle>();
        m_SearchBarsToggle = toggle.get();
        toggle->AddClass("settings-toggle");
        toggle->SetChecked(EditorSearchBars::GetVisible());

        toggle->SetOnValueChanged([](const bool& visible) {
            EditorSearchBars::SetVisible(visible);
        });

        section->AddChild(std::move(toggle));

        auto label = std::make_unique<Label>();
        label->SetText("Filled Search Bar");
        label->AddClass("settings-row-label");
        section->InsertChild(0, std::move(label));

        m_ContentBody->AddContent(std::move(section));
    }

    // Search bar placement toggle
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");

        auto toggle = std::make_unique<Toggle>();
        toggle->AddClass("settings-toggle");
        toggle->SetChecked(EditorSearchBars::GetAtTop());
        toggle->SetOnValueChanged([](const bool& atTop) {
            EditorSearchBars::SetAtTop(atTop);
        });
        section->AddChild(std::move(toggle));

        auto label = std::make_unique<Label>();
        label->SetText("Place Search Bars at Top");
        label->AddClass("settings-row-label");
        section->InsertChild(0, std::move(label));

        m_ContentBody->AddContent(std::move(section));
    }

    // Accent-colored focus outline for editor search fields.
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");

        auto toggle = std::make_unique<Toggle>();
        toggle->AddClass("settings-toggle");
        toggle->SetChecked(EditorSearchBars::GetAccentFocusOutline());
        toggle->SetOnValueChanged([](const bool& enabled) {
            EditorSearchBars::SetAccentFocusOutline(enabled);
        });
        section->AddChild(std::move(toggle));

        auto label = std::make_unique<Label>();
        label->SetText("Accent Search Focus Outline");
        label->AddClass("settings-row-label");
        section->InsertChild(0, std::move(label));

        m_ContentBody->AddContent(std::move(section));
    }

    // Tab Icons toggle
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");

        bool tabIconsEnabled = true;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetBool(kPrefKeyTabIcons, tabIconsEnabled);
        }

        auto toggle = std::make_unique<Toggle>();
        m_TabIconsToggle = toggle.get();
        toggle->AddClass("settings-toggle");
        toggle->SetChecked(tabIconsEnabled);
        toggle->SetOnValueChanged([this](const bool& enabled) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool(kPrefKeyTabIcons, enabled);
            prefs.Save(&err);
            ApplySavedTabIconsStyle(GetOwnerManager());
        });
        section->AddChild(std::move(toggle));

        auto label = std::make_unique<Label>();
        label->SetText("Panel Tab Icons");
        label->AddClass("settings-row-label");
        section->InsertChild(0, std::move(label));

        m_ContentBody->AddContent(std::move(section));
    }

    // Tab right-click context menu toggle
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");

        bool contextMenuEnabled = true;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetBool(kPrefKeyTabRightClickContextMenu, contextMenuEnabled);
        }

        auto toggle = std::make_unique<Toggle>();
        toggle->AddClass("settings-toggle");
        toggle->SetChecked(contextMenuEnabled);
        toggle->SetOnValueChanged([](const bool& enabled) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool(kPrefKeyTabRightClickContextMenu, enabled);
            prefs.Save(&err);
        });
        section->AddChild(std::move(toggle));

        auto label = std::make_unique<Label>();
        label->SetText("Tab Right-Click Menu");
        label->AddClass("settings-row-label");
        section->InsertChild(0, std::move(label));

        m_ContentBody->AddContent(std::move(section));
    }

    // Scene Tool Overlay active color (Accent / Gray)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Scene Tool Active Color");
        label->AddClass("settings-row-label");
        Label* labelPtr = label.get();
        section->AddChild(std::move(label));

        std::string currentValue = "accent";
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetString("ui.sceneToolActiveColor", currentValue);
        }

        auto dd = std::make_unique<Dropdown>();
        dd->AddClass("settings-row-field");
        dd->SetAutoWidthPopup(true);
        std::vector<Dropdown::Option> options = {
            {"accent", "Accent Color"},
            {"gray", "Gray"},
        };
        dd->SetOptions(options, 0);
        dd->SetSelectedValue(currentValue);
        dd->SetOnValueChanged([this](const std::string& value) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetString("ui.sceneToolActiveColor", value);
            prefs.Save(&err);
            ApplySceneToolActiveColor(value);
        });
        section->AddChild(std::move(dd));

        AddDoubleClickReset(labelPtr, [this]() {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetString("ui.sceneToolActiveColor", "accent");
            prefs.Save(&err);
            ApplySceneToolActiveColor("accent");
        });

        m_ContentBody->AddContent(std::move(section));

        // Apply the saved value on startup
        ApplySceneToolActiveColor(currentValue);
    }

    // Popup drop shadows (dropdowns, asset/component pickers, code completion).
    // Shadows are on by default in the theme; each toggle adds an opt-out root class,
    // and the geometry sliders drive the shared `--popup-shadow` custom property.
    {
        auto header = std::make_unique<Label>();
        header->SetText("Popup Shadows");
        header->AddClass("settings-section-header");
        header->Overrides()
            .Set(Style::MarginTop, StyleLength::Px(10.0f))
            .Set(Style::MarginBottom, StyleLength::Px(4.0f));
        m_ContentBody->AddContent(std::move(header));

        auto addShadowToggle = [this](const char* prefKey, const char* labelText) {
            auto section = std::make_unique<UIElement>();
            section->AddClass("settings-row");
            section->AddClass("settings-toggle-row");

            bool enabled = true;
            {
                auto prefs = Editor::OpenEditorPreferences();
                std::string err;
                prefs.Load(&err);
                prefs.TryGetBool(prefKey, enabled);
            }

            auto toggle = std::make_unique<Toggle>();
            toggle->AddClass("settings-toggle");
            toggle->SetChecked(enabled);
            toggle->SetOnValueChanged([this, prefKey](const bool& on) {
                auto prefs = Editor::OpenEditorPreferences();
                std::string err;
                prefs.Load(&err);
                prefs.SetBool(prefKey, on);
                prefs.Save(&err);
                ApplySavedPopupShadowStyle(GetOwnerManager());
            });
            section->AddChild(std::move(toggle));

            auto label = std::make_unique<Label>();
            label->SetText(labelText);
            label->AddClass("settings-row-label");
            section->InsertChild(0, std::move(label));

            m_ContentBody->AddContent(std::move(section));
        };

        addShadowToggle(kPrefKeyDropdownShadow, "Dropdown Shadows");
        addShadowToggle(kPrefKeyPickerShadow, "Picker Shadows");
        addShadowToggle(kPrefKeyCompletionShadow, "Code Completion Shadows");
        addShadowToggle(kPrefKeyTooltipShadow, "Tooltip Shadows");

        // Shadow geometry sliders. Each updates the cached state field and re-applies
        // (live during drag; the slider helper persists to prefs on commit).
        auto addShadowSlider = [this](const char* prefKey, const char* labelText,
                                      float defaultValue, float minValue, float maxValue,
                                      float step, float PopupShadowState::* field) {
            SettingsSliderConfig cfg;
            cfg.labelText = labelText;
            cfg.defaultValue = defaultValue;
            cfg.minValue = minValue;
            cfg.maxValue = maxValue;
            cfg.step = step;
            cfg.prefKey = prefKey;
            cfg.onValueChanged = [this, field](float v) {
                g_PopupShadow.*field = v;
                ApplyPopupShadowToManager(GetOwnerManager());
            };
            cfg.onValueChanging = [this, field](float v) {
                g_PopupShadow.*field = v;
                ApplyPopupShadowToManager(GetOwnerManager());
            };
            CreateSettingsSliderRow(cfg, m_ContentBody);
        };

        addShadowSlider(kPrefKeyShadowOffsetX, "Shadow Offset X",
                        kDefaultShadowOffsetX, -32.0f, 32.0f, 1.0f, &PopupShadowState::offsetX);
        addShadowSlider(kPrefKeyShadowOffsetY, "Shadow Offset Y",
                        kDefaultShadowOffsetY, -32.0f, 32.0f, 1.0f, &PopupShadowState::offsetY);
        addShadowSlider(kPrefKeyShadowBlur, "Shadow Blur",
                        kDefaultShadowBlur, 0.0f, 64.0f, 1.0f, &PopupShadowState::blur);
        addShadowSlider(kPrefKeyShadowOpacity, "Shadow Opacity",
                        kDefaultShadowOpacity, 0.0f, 1.0f, 0.05f, &PopupShadowState::opacity);
    }

    // Pages registered against the UIAppearance group render here, after the
    // hand-built rows.
    for (const auto& category : Editor::EditorSettingsRegistry::Get().Snapshot())
        if (category.Group == Editor::SettingsCategoryGroup::UIAppearance)
            CreateRegistrySettingsContent(category);
}

void SettingsPanel::CreateUIHiDpiContent()
{
    if (!m_ContentBody)
        return;

    // Apply platform clipboard / content-scale changes after the current UI
    // event finishes. Syncing during Dropdown value notifications runs inside
    // pointer dispatch; an immediate relayout can strand popup state and make
    // the control stop responding until the panel is rebuilt.
    auto scheduleHiDpiPlatformSync = [this]() {
        auto run = [this]() {
            if (m_OnHiDpiPlatformSettingsChanged)
                m_OnHiDpiPlatformSettingsChanged();
        };
        if (m_UIManager)
        {
            if (UIElement* root = m_UIManager->GetRootElement())
            {
                root->PostAction(std::move(run));
                return;
            }
        }
        run();
    };

    SettingsToggleConfig useSystemScaleCfg;
    useSystemScaleCfg.labelText = "Use system display scaling";
    useSystemScaleCfg.defaultValue = true;
    useSystemScaleCfg.prefKey = "ui.hidpiUseSystemScale";
    useSystemScaleCfg.onValueChanged = [scheduleHiDpiPlatformSync](bool /*v*/) { scheduleHiDpiPlatformSync(); };
    CreateSettingsToggleRow(useSystemScaleCfg, m_ContentBody);

    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-hidpi-scale-row");

        auto label = std::make_unique<Label>();
        label->SetText("Additional UI scale");
        label->AddClass("settings-row-label");
        Label* scaleLabelPtr = label.get();
        section->AddChild(std::move(label));

        double storedMult = 1.0;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            (void)prefs.TryGetDouble("ui.hidpiContentScaleMultiplier", storedMult);
        }

        struct UiScalePreset
        {
            double mult;
            const char* label;
        };
        static constexpr UiScalePreset kUiScalePresets[] = {
            {1.0, "100%"},
            {1.25, "125%"},
            {1.5, "150%"},
            {2.0, "200%"},
        };

        auto presetRow = std::make_unique<UIElement>();
        presetRow->AddClass("settings-scale-preset-group");

        auto presetButtons = std::make_shared<std::array<Button*, std::size(kUiScalePresets)>>();
        presetButtons->fill(nullptr);

        auto syncPresetHighlight = [presetButtons](double mult) {
            const float normalized = Editor::NormalizeHiDpiContentScaleMultiplier(mult);
            for (size_t i = 0; i < presetButtons->size(); ++i)
            {
                Button* b = (*presetButtons)[i];
                if (!b)
                    continue;
                const bool on = std::abs(kUiScalePresets[i].mult - static_cast<double>(normalized)) < 1e-5;
                if (on)
                    b->AddClass("settings-scale-preset-selected");
                else
                    b->RemoveClass("settings-scale-preset-selected");
            }
        };

        for (size_t i = 0; i < std::size(kUiScalePresets); ++i)
        {
            const UiScalePreset& preset = kUiScalePresets[i];
            auto btn = std::make_unique<Button>();
            btn->SetText(preset.label);
            btn->AddClass("button");
            btn->AddClass("small");
            btn->AddClass("settings-button");
            btn->AddClass("settings-scale-preset");
            Button* btnPtr = btn.get();
            (*presetButtons)[i] = btnPtr;
            const double mult = preset.mult;
            btn->RegisterEventHandler(kEventButtonClick, [mult, syncPresetHighlight, scheduleHiDpiPlatformSync](UIEvent&) {
                auto prefs = Editor::OpenEditorPreferences();
                std::string err;
                prefs.Load(&err);
                prefs.SetDouble("ui.hidpiContentScaleMultiplier", mult);
                prefs.Save(&err);
                syncPresetHighlight(mult);
                scheduleHiDpiPlatformSync();
            });
            presetRow->AddChild(std::move(btn));
        }

        syncPresetHighlight(storedMult);

        section->AddChild(std::move(presetRow));

        AddDoubleClickReset(scaleLabelPtr, [syncPresetHighlight, scheduleHiDpiPlatformSync]() {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetDouble("ui.hidpiContentScaleMultiplier", 1.0);
            prefs.Save(&err);
            syncPresetHighlight(1.0);
            scheduleHiDpiPlatformSync();
        });

        m_ContentBody->AddContent(std::move(section));
        scheduleHiDpiPlatformSync();
    }
}

void SettingsPanel::CreateUIFontSizesContent()
{
    if (!m_ContentBody)
        return;
    
    // Helper to update a single CSS variable in the root element's style
    auto updateCssVar = [](UIElement* root, const std::string& varName, int valuePx) {
        if (!root)
            return;
        root->Overrides().SetCustom(HashStringId(varName), FormatPxValue(static_cast<float>(valuePx)));
    };
    
    // Helper to update spacing CSS variables proportionally based on base font size
    // Default base font is 14px, spacing scales linearly with font size
    auto updateSpacingVars = [updateCssVar](UIElement* root, float baseFontSize) {
        const float scale = baseFontSize / 14.0f;
        // Default spacing values at 14px font: xs=4, sm=6, md=8, lg=12, xl=16
        updateCssVar(root, "--ui_spacing_xs", (int)std::round(4.0f * scale));
        updateCssVar(root, "--ui_spacing_sm", (int)std::round(6.0f * scale));
        updateCssVar(root, "--ui_spacing_md", (int)std::round(8.0f * scale));
        updateCssVar(root, "--ui_spacing_lg", (int)std::round(12.0f * scale));
        updateCssVar(root, "--ui_spacing_xl", (int)std::round(16.0f * scale));
    };
    
    // Helper lambda to create a font size slider setting
    auto createFontSizeSetting = [this, updateCssVar, updateSpacingVars](const char* labelText, const char* prefKey, const char* cssVar,
                                        Slider*& sliderPtr, float defaultValue, bool isBaseFontSize = false) {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText(labelText);
        label->AddClass("settings-row-label");
        Label* fontSizeLabelPtr = label.get();
        section->AddChild(std::move(label));
        
        auto slider = std::make_unique<Slider>();
        sliderPtr = slider.get();
        slider->AddClass("settings-row-slider");
        slider->SetTrackPaddingPx(0.0f);
        slider->SetShowValueBubble(true);
        slider->SetMin(12.0f);
        slider->SetMax(32.0f);
        slider->SetStep(1.0f);
        
        // Load from preferences
        float fontSize = defaultValue;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            double stored = fontSize;
            if (prefs.TryGetDouble(prefKey, stored))
                fontSize = std::clamp(static_cast<float>(stored), 12.0f, 32.0f);
        }
        slider->SetValue(fontSize);
        
        // Apply initial value to CSS variable (and spacing if base font)
        if (m_UIManager)
        {
            if (auto* root = m_UIManager->GetRootElement())
            {
                updateCssVar(root, cssVar, (int)fontSize);
                if (isBaseFontSize)
                    updateSpacingVars(root, fontSize);
                m_UIManager->MarkStyleDirtySubtree(root);
            }
        }
        
        std::string cssVarStr = cssVar;
        std::string prefKeyStr = prefKey;
        
        auto applyFontSize = [this, cssVarStr, isBaseFontSize, updateCssVar, updateSpacingVars](float value) {
            if (!m_UIManager)
                return;
            auto* root = m_UIManager->GetRootElement();
            if (!root)
                return;
            
            updateCssVar(root, cssVarStr, (int)value);
            if (isBaseFontSize)
            {
                updateSpacingVars(root, value);
                if (m_OnBaseFontSizeChanged)
                    m_OnBaseFontSizeChanged(value);
            }
            m_UIManager->MarkStyleDirtySubtree(root);
            m_UIManager->RequestRelayout();
        };

        auto valueField = std::make_unique<FloatField>();
        valueField->AddClass("settings-row-value");
        InspectorDrag::ApplySliderFloatValueFieldStyle(valueField.get());
        valueField->SetValue(fontSize);
        FloatField* valueFieldPtr = valueField.get();

        slider->SetOnValueChanging([valueFieldPtr, applyFontSize](const float& value) {
            valueFieldPtr->SetValue(value);
            applyFontSize(value);
        });
        slider->SetOnValueChanged([valueFieldPtr, applyFontSize, prefKeyStr](const float& value) {
            valueFieldPtr->SetValue(value);
            applyFontSize(value);
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetDouble(prefKeyStr, value);
            prefs.Save(&err);
        });
        valueField->SetOnValueChanged([sliderPtr, valueFieldPtr, applyFontSize, prefKeyStr](const float& value) {
            float clamped = std::max(12.0f, std::min(32.0f, value));
            sliderPtr->SetValue(clamped);
            valueFieldPtr->SetValue(clamped);
            applyFontSize(clamped);
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetDouble(prefKeyStr, clamped);
            prefs.Save(&err);
        });
        InspectorDrag::SetupLabelDragSlider(
            fontSizeLabelPtr, sliderPtr, nullptr, nullptr, defaultValue);
        section->AddChild(std::move(slider));
        section->AddChild(std::move(valueField));
        return section;
    };
    
    // UI Text Scale - adjusts spacing proportionally (12px = compact, 16px = spacious)
    // Note: CSS variable font sizes don't update live in this UI system,
    // so this slider only affects spacing variables which do update properly.
    m_ContentBody->AddContent(createFontSizeSetting(
        "UI Text Scale", "ui.fontSizeBase", "--ui_font_size_base",
        m_FontSizeBaseSlider, 14.0f, true));

    {
        auto prefsLoad = Editor::OpenEditorPreferences();
        std::string prefsErr;
        prefsLoad.Load(&prefsErr);
        std::string uiPreset = "default";
        std::string scriptPreset = "default";
        (void)prefsLoad.TryGetString(Editor::kPrefUiFontPreset, uiPreset);
        (void)prefsLoad.TryGetString(Editor::kPrefScriptFontPreset, scriptPreset);

        auto addFontPresetRow = [this](const char* rowLabel,
                                       const char* prefKey,
                                       std::vector<Dropdown::Option> options,
                                       const std::string& selectedValue)
        {
            auto row = std::make_unique<UIElement>();
            row->AddClass("settings-row");
            auto label = std::make_unique<Label>();
            label->SetText(rowLabel);
            label->AddClass("settings-row-label");
            row->AddChild(std::move(label));

            auto dd = std::make_unique<Dropdown>();
            dd->AddClass("settings-row-field");
            dd->SetAutoWidthPopup(true);
            dd->SetOptions(options, 0);
            dd->SetSelectedValue(selectedValue);
            dd->SetOnValueChanged([this, prefKey](const std::string& value)
                                  {
                                      auto prefs = Editor::OpenEditorPreferences();
                                      std::string err;
                                      prefs.Load(&err);
                                      prefs.SetString(prefKey, value);
                                      prefs.Save(&err);
                                      if (m_OnEditorFontPreferencesChanged)
                                          m_OnEditorFontPreferencesChanged();
                                      else if (m_UIManager)
                                      {
                                          Editor::ApplySavedEditorFontPreferences(m_UIManager);
                                          // The line height is a multiple of the script face's
                                          // own line, so a new face needs a new multiplier.
                                          ApplySavedScriptLineHeightStyle(m_UIManager);
                                      }
                                  });
            row->AddChild(std::move(dd));
            m_ContentBody->AddContent(std::move(row));
        };

        GameEngine::Engine::UI::FontResolver fontProbe(EngineCore::GetInstance().GetAssetManager().GetRegistry(),
                                                         &EngineCore::GetInstance().GetJobSystem());
        fontProbe.RebuildIndex();

        std::vector<Editor::EditorFontDropdownOption> uiBuilt = Editor::BuildEditorUiFontDropdownOptions(fontProbe);
        std::vector<Editor::EditorFontDropdownOption> scriptBuilt = Editor::BuildScriptFontDropdownOptions(fontProbe);

        const std::string uiSanitized = Editor::SanitizeFontPresetAgainstOptions(uiPreset, uiBuilt);
        const std::string scriptSanitized = Editor::SanitizeFontPresetAgainstOptions(scriptPreset, scriptBuilt);
        if (uiSanitized != uiPreset || scriptSanitized != scriptPreset)
        {
            auto prefsFix = Editor::OpenEditorPreferences();
            std::string err;
            prefsFix.Load(&err);
            if (uiSanitized != uiPreset)
                prefsFix.SetString(Editor::kPrefUiFontPreset, uiSanitized);
            if (scriptSanitized != scriptPreset)
                prefsFix.SetString(Editor::kPrefScriptFontPreset, scriptSanitized);
            prefsFix.Save(&err);
            if (m_OnEditorFontPreferencesChanged)
                m_OnEditorFontPreferencesChanged();
            else if (m_UIManager)
            {
                Editor::ApplySavedEditorFontPreferences(m_UIManager);
                ApplySavedScriptLineHeightStyle(m_UIManager);
            }
        }

        std::vector<Dropdown::Option> uiFontOptions;
        uiFontOptions.reserve(uiBuilt.size());
        for (const auto& o : uiBuilt)
            uiFontOptions.push_back(Dropdown::Option{o.value, o.label});

        std::vector<Dropdown::Option> scriptFontOptions;
        scriptFontOptions.reserve(scriptBuilt.size());
        for (const auto& o : scriptBuilt)
            scriptFontOptions.push_back(Dropdown::Option{o.value, o.label});

        addFontPresetRow("Editor UI Font", Editor::kPrefUiFontPreset, std::move(uiFontOptions), uiSanitized);
        addFontPresetRow("Script Editor Font", Editor::kPrefScriptFontPreset, std::move(scriptFontOptions), scriptSanitized);
    }

    // Text contrast: the colour-keyed coverage boost (Skia SK_GAMMA_CONTRAST).
    static constexpr const char* kTextContrastTooltip =
        "Adds weight to partially covered glyph pixels, keyed on the text colour: "
        "full strength for dark text on light backgrounds, tapering to nothing for "
        "near-white text. Compensates for source-over blending averaging ink and "
        "paper physically rather than the way 8-bit text stacks do. 0 disables it; "
        "default 1.0 (what Chrome ships on Windows).";
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->SetTooltip(kTextContrastTooltip);

        auto label = std::make_unique<Label>();
        label->SetText("Text Contrast");
        label->AddClass("settings-row-label");
        label->SetTooltip(kTextContrastTooltip);
        Label* labelPtr = label.get();
        section->AddChild(std::move(label));

        auto slider = std::make_unique<Slider>();
        Slider* sliderPtr = slider.get();
        slider->AddClass("settings-row-slider");
        slider->SetTrackPaddingPx(0.0f);
        slider->SetShowValueBubble(true);
        slider->SetMin(0.0f);
        slider->SetMax(1.0f);
        slider->SetStep(0.05f);
        slider->SetTooltip(kTextContrastTooltip);

        float contrastValue = kTextContrastDefault;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            double stored = contrastValue;
            if (prefs.TryGetDouble(kPrefKeyTextContrast, stored))
                contrastValue =
                    std::clamp(static_cast<float>(stored), kTextContrastMin, kTextContrastMax);
        }
        slider->SetValue(contrastValue);

        auto valueField = std::make_unique<FloatField>();
        valueField->AddClass("settings-row-value");
        InspectorDrag::ApplySliderFloatValueFieldStyle(valueField.get());
        valueField->SetValue(contrastValue);
        FloatField* valueFieldPtr = valueField.get();

        auto applyContrast = [this](float value) {
            if (m_UIManager)
                m_UIManager->SetTextContrast(value);
            // Fan out to every editor window's UIManager (main + floating).
            if (m_OnTextContrastChanged)
                m_OnTextContrastChanged(value);
        };

        // Apply the loaded preference to ALL windows, not just the main one.
        // This ensures floating windows that are already open see the saved
        // setting on startup instead of waiting for the user to touch the slider.
        applyContrast(contrastValue);

        slider->SetOnValueChanging([valueFieldPtr, applyContrast](const float& value) {
            valueFieldPtr->SetValue(value);
            applyContrast(value);
        });
        slider->SetOnValueChanged([valueFieldPtr, applyContrast](const float& value) {
            valueFieldPtr->SetValue(value);
            applyContrast(value);
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetDouble(kPrefKeyTextContrast, static_cast<double>(value));
            prefs.Save(&err);
        });
        valueField->SetOnValueChanged([sliderPtr, applyContrast](const float& value) {
            float clamped = std::clamp(value, 0.0f, 1.0f);
            sliderPtr->SetValue(clamped);
            applyContrast(clamped);
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetDouble(kPrefKeyTextContrast, static_cast<double>(clamped));
            prefs.Save(&err);
        });
        InspectorDrag::SetupLabelDragSlider(
            labelPtr, sliderPtr, nullptr, nullptr, kTextContrastDefault);

        section->AddChild(std::move(slider));
        section->AddChild(std::move(valueField));
        m_ContentBody->AddContent(std::move(section));
    }

    // Subpixel text AA (ClearType-style). Opt-in: renders glyph coverage per
    // display subpixel for ~3x horizontal text resolution on RGB-stripe LCDs.
    // The toggle is a request — it auto-falls back to grayscale in HDR output
    // modes or when the device lacks dual-source blending.
    static constexpr const char* kSubpixelTooltip =
        "Renders text at subpixel resolution on RGB-stripe LCD panels "
        "(ClearType-style). Sharper small text; glyph edges carry faint colour "
        "fringes by design, and screenshots include them. Automatically falls "
        "back to grayscale AA in HDR output modes. Not recommended on OLED or "
        "rotated panels. Default off.";
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");
        section->SetTooltip(kSubpixelTooltip);

        constexpr bool kDefaultSubpixel = false;
        bool subpixelValue = kDefaultSubpixel;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetBool(kPrefKeyTextSubpixelAA, subpixelValue);
        }

        auto applySubpixel = [this](bool enabled) {
            if (m_UIManager)
                m_UIManager->SetTextSubpixelAA(enabled);
            // Fan out to every editor window's UIManager (main + floating).
            if (m_OnTextSubpixelAAChanged)
                m_OnTextSubpixelAAChanged(enabled);
        };

        // Apply the loaded preference to ALL windows on startup, like the
        // contrast row above.
        applySubpixel(subpixelValue);

        auto toggle = std::make_unique<Toggle>();
        toggle->AddClass("settings-toggle");
        toggle->SetTooltip(kSubpixelTooltip);
        toggle->SetChecked(subpixelValue);
        toggle->SetOnValueChanged([applySubpixel](const bool& enabled) {
            applySubpixel(enabled);
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool(kPrefKeyTextSubpixelAA, enabled);
            prefs.Save(&err);
        });
        section->AddChild(std::move(toggle));

        auto label = std::make_unique<Label>();
        label->SetText("Subpixel Text AA (RGB LCD)");
        label->AddClass("settings-row-label");
        label->SetTooltip(kSubpixelTooltip);
        section->InsertChild(0, std::move(label));

        m_ContentBody->AddContent(std::move(section));
    }

    // Text smoothing (gamma). A symmetric power remap of coverage around 0.5,
    // applied after the colour-keyed contrast correction above. Endpoints and
    // midpoint are fixed, so it trades edge softness without moving ink.
    static constexpr const char* kSmoothingTooltip =
        "Brightness-neutral edge softness. Above 1.0 softens edges (wider "
        "transition zone). Below 1.0 sharpens. Endpoints and midpoint are "
        "preserved — overall text weight stays constant, only edge smoothness "
        "changes. Text weight itself is the Text Contrast row above. Default 1.0.";
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->SetTooltip(kSmoothingTooltip);

        auto label = std::make_unique<Label>();
        label->SetText("Text Smoothing (gamma)");
        label->AddClass("settings-row-label");
        label->SetTooltip(kSmoothingTooltip);
        Label* labelPtr = label.get();
        section->AddChild(std::move(label));

        auto slider = std::make_unique<Slider>();
        Slider* sliderPtr = slider.get();
        slider->AddClass("settings-row-slider");
        slider->SetTrackPaddingPx(0.0f);
        slider->SetShowValueBubble(true);
        slider->SetMin(0.5f);
        slider->SetMax(2.0f);
        slider->SetStep(0.05f);
        slider->SetTooltip(kSmoothingTooltip);

        float gammaValue = kTextSmoothingGammaDefault;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            double stored = gammaValue;
            if (prefs.TryGetDouble(kPrefKeyTextSmoothingGamma, stored))
                gammaValue = std::clamp(static_cast<float>(stored), kTextSmoothingGammaMin,
                                        kTextSmoothingGammaMax);
        }
        slider->SetValue(gammaValue);

        auto valueField = std::make_unique<FloatField>();
        valueField->AddClass("settings-row-value");
        InspectorDrag::ApplySliderFloatValueFieldStyle(valueField.get());
        valueField->SetValue(gammaValue);
        FloatField* valueFieldPtr = valueField.get();

        auto applyGamma = [this](float value) {
            if (m_UIManager)
                m_UIManager->SetTextSmoothingGamma(value);
            if (m_OnTextSmoothingGammaChanged)
                m_OnTextSmoothingGammaChanged(value);
        };

        // Apply the loaded preference to ALL windows on startup.
        applyGamma(gammaValue);

        slider->SetOnValueChanging([valueFieldPtr, applyGamma](const float& value) {
            valueFieldPtr->SetValue(value);
            applyGamma(value);
        });
        slider->SetOnValueChanged([valueFieldPtr, applyGamma](const float& value) {
            valueFieldPtr->SetValue(value);
            applyGamma(value);
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetDouble(kPrefKeyTextSmoothingGamma, static_cast<double>(value));
            prefs.Save(&err);
        });
        valueField->SetOnValueChanged([sliderPtr, applyGamma](const float& value) {
            float clamped = std::clamp(value, 0.5f, 2.0f);
            sliderPtr->SetValue(clamped);
            applyGamma(clamped);
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetDouble(kPrefKeyTextSmoothingGamma, static_cast<double>(clamped));
            prefs.Save(&err);
        });
        InspectorDrag::SetupLabelDragSlider(
            labelPtr, sliderPtr, nullptr, nullptr, kTextSmoothingGammaDefault);

        section->AddChild(std::move(slider));
        section->AddChild(std::move(valueField));
        m_ContentBody->AddContent(std::move(section));
    }
}

void SettingsPanel::CreateUITreesContent()
{
    if (!m_ContentBody)
        return;

    CreateTreeScalingSettingsForTarget(true);
    CreateTreeScalingSettingsForTarget(false);
}

void SettingsPanel::CreateUIAssetsContent()
{
    if (!m_ContentBody)
        return;

    // Default Assets View (toggle: off=List, on=Grid)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");
        
        auto toggle = std::make_unique<Toggle>();
        toggle->AddClass("settings-toggle");
        
        // Load from preferences (default is list view = false)
        bool useGridView = false;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetBool("ui.assetsDefaultGridView", useGridView);
        }
        toggle->SetChecked(useGridView);
        
        toggle->SetOnValueChanged([](const bool& enabled) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.assetsDefaultGridView", enabled);
            prefs.Save(&err);
        });
        Toggle* gridViewTogglePtr = toggle.get();
        section->AddChild(std::move(toggle));
        
        auto label = std::make_unique<Label>();
        label->SetText("Default to Grid View");
        label->AddClass("settings-row-label");
        Label* gridViewLabelPtr = label.get();
        section->InsertChild(0, std::move(label));
        AddDoubleClickReset(gridViewLabelPtr, [gridViewTogglePtr]() {
            const bool kDefault = false;
            gridViewTogglePtr->SetChecked(kDefault);
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.assetsDefaultGridView", kDefault);
            prefs.Save(&err);
        });
        m_ContentBody->AddContent(std::move(section));
    }

    // Display folders first in the Assets panel
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");

        auto toggle = std::make_unique<Toggle>();
        m_AssetsFoldersFirstToggle = toggle.get();
        toggle->AddClass("settings-toggle");

        bool foldersFirst = true;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetBool("ui.assets.foldersFirst", foldersFirst);
        }
        toggle->SetChecked(foldersFirst);

        toggle->SetOnValueChanged([this](const bool& enabled) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.assets.foldersFirst", enabled);
            prefs.Save(&err);

            if (m_OnAssetsFoldersFirstChanged)
                m_OnAssetsFoldersFirstChanged(enabled);
        });

        section->AddChild(std::move(toggle));

        auto label = std::make_unique<Label>();
        label->SetText("Display Folders First");
        label->AddClass("settings-row-label");
        Label* foldersFirstLabelPtr = label.get();
        section->InsertChild(0, std::move(label));
        AddDoubleClickReset(foldersFirstLabelPtr, [this]() {
            constexpr bool kDefault = true;
            if (m_AssetsFoldersFirstToggle)
                m_AssetsFoldersFirstToggle->SetChecked(kDefault);

            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.assets.foldersFirst", kDefault);
            prefs.Save(&err);

            if (m_OnAssetsFoldersFirstChanged)
                m_OnAssetsFoldersFirstChanged(kDefault);
        });

        m_ContentBody->AddContent(std::move(section));
    }

    // Expand the Assets folder tree when the panel loads
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");

        auto toggle = std::make_unique<Toggle>();
        m_AssetsExpandFoldersOnLoadToggle = toggle.get();
        toggle->AddClass("settings-toggle");

        bool expandOnLoad = false;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetBool("ui.assets.expandFoldersOnLoad", expandOnLoad);
        }
        toggle->SetChecked(expandOnLoad);

        toggle->SetOnValueChanged([this](const bool& enabled) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.assets.expandFoldersOnLoad", enabled);
            prefs.Save(&err);

            if (m_OnAssetsExpandFoldersOnLoadChanged)
                m_OnAssetsExpandFoldersOnLoadChanged(enabled);
        });

        section->AddChild(std::move(toggle));

        auto label = std::make_unique<Label>();
        label->SetText("Expand Folders On Load");
        label->SetTooltip("Expand Asset Folders On Load");
        label->AddClass("settings-row-label");
        Label* expandFoldersLabelPtr = label.get();
        section->InsertChild(0, std::move(label));
        AddDoubleClickReset(expandFoldersLabelPtr, [this]() {
            constexpr bool kDefault = false;
            if (m_AssetsExpandFoldersOnLoadToggle)
                m_AssetsExpandFoldersOnLoadToggle->SetChecked(kDefault);

            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.assets.expandFoldersOnLoad", kDefault);
            prefs.Save(&err);

            if (m_OnAssetsExpandFoldersOnLoadChanged)
                m_OnAssetsExpandFoldersOnLoadChanged(kDefault);
        });

        m_ContentBody->AddContent(std::move(section));
    }

    // Assets Grid Icon Size setting
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Assets Grid Icon Size");
        label->AddClass("settings-row-label");
        Label* gridIconSizeLabelPtr = label.get();
        section->AddChild(std::move(label));

        auto slider = std::make_unique<Slider>();
        m_GridIconSizeSlider = slider.get();
        slider->AddClass("settings-row-slider");
        slider->SetTrackPaddingPx(0.0f);
        slider->SetShowValueBubble(true);
        slider->SetMin(32.0f);
        slider->SetMax(4096.0f);
        slider->SetStep(16.0f);
        slider->SetValue(m_GridIconSizeValue);

        auto valueField = std::make_unique<FloatField>();
        m_GridIconSizeField = valueField.get();
        valueField->AddClass("settings-row-value");
        InspectorDrag::ApplySliderFloatValueFieldStyle(valueField.get());
        valueField->SetValue(m_GridIconSizeValue);
        FloatField* valueFieldPtr = valueField.get();
        Slider* sliderPtr = slider.get();

        auto applySize = [this, valueFieldPtr](float value)
        {
            const float clamped = std::clamp(value, 32.0f, 4096.0f);
            if (valueFieldPtr)
                valueFieldPtr->SetValueWithoutNotify(clamped);
            if (m_GridIconSizeUpdating)
                return clamped;
            m_GridIconSizeValue = clamped;
            if (m_OnGridIconSizeChanged)
                m_OnGridIconSizeChanged(clamped);
            return clamped;
        };
        slider->SetOnValueChanging([applySize](const float& value) { applySize(value); });
        slider->SetOnValueChanged([this, applySize](const float& value) { 
            const float clamped = applySize(value);
            // Save only on drag end
            if (m_OnGridIconSizeFinalized)
                m_OnGridIconSizeFinalized(clamped);
        });
        valueField->SetOnValueChanging([sliderPtr, applySize](const float& value) {
            const float clamped = std::clamp(value, 32.0f, 4096.0f);
            if (sliderPtr)
                sliderPtr->SetValueWithoutNotify(clamped);
            applySize(clamped);
        });
        valueField->SetOnValueChanged([this, sliderPtr, valueFieldPtr, applySize](const float& value) {
            const float clamped = std::clamp(value, 32.0f, 4096.0f);
            if (sliderPtr)
                sliderPtr->SetValueWithoutNotify(clamped);
            if (valueFieldPtr)
                valueFieldPtr->SetValueWithoutNotify(clamped);
            applySize(clamped);
            if (m_OnGridIconSizeFinalized)
                m_OnGridIconSizeFinalized(clamped);
        });
        // Match GridView default icon size and SettingsPanel::m_GridIconSizeValue initial value.
        const float kDefaultGridIconSize = 80.0f;
        InspectorDrag::SetupLabelDragSlider(
            gridIconSizeLabelPtr, m_GridIconSizeSlider, nullptr, nullptr, kDefaultGridIconSize);
        section->AddChild(std::move(slider));
        section->AddChild(std::move(valueField));
        m_ContentBody->AddContent(std::move(section));
    }

    // Show the assets view toggle in a dedicated bottom toolbar.
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");

        auto toggle = std::make_unique<Toggle>();
        m_AssetsExtraBottomViewToolbarToggle = toggle.get();
        toggle->AddClass("settings-toggle");

        bool enabled = true;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetBool("ui.assetsExtraBottomViewToolbar", enabled);
        }
        toggle->SetChecked(enabled);

        toggle->SetOnValueChanged([this](const bool& value) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.assetsExtraBottomViewToolbar", value);
            prefs.Save(&err);

            if (m_OnAssetsExtraBottomViewToolbarChanged)
                m_OnAssetsExtraBottomViewToolbarChanged(value);
        });
        section->AddChild(std::move(toggle));

        auto label = std::make_unique<Label>();
        label->SetText("Asset Bottom Toolbar");
        label->AddClass("settings-row-label");
        Label* extraToolbarLabelPtr = label.get();
        section->InsertChild(0, std::move(label));
        AddDoubleClickReset(extraToolbarLabelPtr, [this]() {
            constexpr bool kDefault = true;
            if (m_AssetsExtraBottomViewToolbarToggle)
                m_AssetsExtraBottomViewToolbarToggle->SetChecked(kDefault);

            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.assetsExtraBottomViewToolbar", kDefault);
            prefs.Save(&err);

            if (m_OnAssetsExtraBottomViewToolbarChanged)
                m_OnAssetsExtraBottomViewToolbarChanged(kDefault);
        });

        m_ContentBody->AddContent(std::move(section));
    }

    // Assets view toggle icon style: one switching icon or two fixed icons.
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");

        auto toggle = std::make_unique<Toggle>();
        m_AssetsSingleViewToggleIconToggle = toggle.get();
        toggle->AddClass("settings-toggle");

        bool enabled = true;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetBool("ui.assetsSingleViewToggleIcon", enabled);
        }
        toggle->SetChecked(enabled);

        toggle->SetOnValueChanged([this](const bool& value) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.assetsSingleViewToggleIcon", value);
            prefs.Save(&err);

            if (m_OnAssetsSingleViewToggleIconChanged)
                m_OnAssetsSingleViewToggleIconChanged(value);
        });
        section->AddChild(std::move(toggle));

        auto label = std::make_unique<Label>();
        label->SetText("Single View Toggle Icon");
        label->AddClass("settings-row-label");
        Label* singleToggleIconLabelPtr = label.get();
        section->InsertChild(0, std::move(label));
        AddDoubleClickReset(singleToggleIconLabelPtr, [this]() {
            constexpr bool kDefault = true;
            if (m_AssetsSingleViewToggleIconToggle)
                m_AssetsSingleViewToggleIconToggle->SetChecked(kDefault);

            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.assetsSingleViewToggleIcon", kDefault);
            prefs.Save(&err);

            if (m_OnAssetsSingleViewToggleIconChanged)
                m_OnAssetsSingleViewToggleIconChanged(kDefault);
        });

        m_ContentBody->AddContent(std::move(section));
    }

    // Show zoom slider on the assets bottom toolbar (grid/list size).
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");

        auto toggle = std::make_unique<Toggle>();
        m_AssetsBottomToolbarZoomSliderVisibleToggle = toggle.get();
        toggle->AddClass("settings-toggle");

        bool visible = true;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetBool("ui.assets.bottomToolbarShowZoomSlider", visible);
        }
        toggle->SetChecked(visible);

        toggle->SetOnValueChanged([this](const bool& value) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.assets.bottomToolbarShowZoomSlider", value);
            prefs.Save(&err);

            if (m_OnAssetsBottomToolbarZoomSliderVisibleChanged)
                m_OnAssetsBottomToolbarZoomSliderVisibleChanged(value);
        });
        section->AddChild(std::move(toggle));

        auto label = std::make_unique<Label>();
        label->SetText("Show Assets Zoom Slider");
        label->AddClass("settings-row-label");
        Label* zoomSliderLabelPtr = label.get();
        section->InsertChild(0, std::move(label));
        AddDoubleClickReset(zoomSliderLabelPtr, [this]() {
            constexpr bool kDefault = true;
            if (m_AssetsBottomToolbarZoomSliderVisibleToggle)
                m_AssetsBottomToolbarZoomSliderVisibleToggle->SetChecked(kDefault);

            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.assets.bottomToolbarShowZoomSlider", kDefault);
            prefs.Save(&err);

            if (m_OnAssetsBottomToolbarZoomSliderVisibleChanged)
                m_OnAssetsBottomToolbarZoomSliderVisibleChanged(kDefault);
        });

        m_ContentBody->AddContent(std::move(section));
    }

    // Rotate 3D Model Previews toggle
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");

        auto toggle = std::make_unique<Toggle>();
        toggle->AddClass("settings-toggle");

        // Load from preferences (default: enabled)
        bool rotatePreviews = true;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetBool("ui.rotateModelPreviews", rotatePreviews);
        }
        toggle->SetChecked(rotatePreviews);
        ModelThumbnailHandler::SetRotatePreviewsEnabled(rotatePreviews);

        toggle->SetOnValueChanged([](const bool& enabled) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.rotateModelPreviews", enabled);
            prefs.Save(&err);
            ModelThumbnailHandler::SetRotatePreviewsEnabled(enabled);
        });

        Toggle* rotateTogglePtr = toggle.get();
        section->AddChild(std::move(toggle));

        auto label = std::make_unique<Label>();
        label->SetText("Rotate 3D Model Previews");
        label->AddClass("settings-row-label");
        Label* rotateLabelPtr = label.get();
        section->InsertChild(0, std::move(label));

        AddDoubleClickReset(rotateLabelPtr, [rotateTogglePtr]() {
            const bool kDefault = true;
            rotateTogglePtr->SetChecked(kDefault);
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.rotateModelPreviews", kDefault);
            prefs.Save(&err);
        });

        m_ContentBody->AddContent(std::move(section));
    }

    // 3D Preview Rotation Speed (affects orbit velocity in model thumbnails and asset view)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("3D Preview Rotation Speed");
        label->AddClass("settings-row-label");
        Label* labelPtr = label.get();

        auto slider = std::make_unique<Slider>();
        slider->AddClass("settings-row-slider");
        slider->SetTrackPaddingPx(0.0f);
        slider->SetShowValueBubble(true);
        slider->SetMin(0.25f);
        slider->SetMax(4.0f);
        slider->SetStep(0.05f);
        Slider* sliderPtr = slider.get();

        auto valueField = std::make_unique<FloatField>();
        valueField->AddClass("settings-row-value");
        InspectorDrag::ApplySliderFloatValueFieldStyle(valueField.get());
        FloatField* valueFieldPtr = valueField.get();

        float current = 1.0f;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            double stored = static_cast<double>(current);
            if (prefs.TryGetDouble("ui.previewOrbitSpeed", stored))
            {
                current = static_cast<float>(stored);
            }
            current = std::clamp(current, 0.25f, 4.0f);
        }

        slider->SetValue(current);
        valueField->SetValue(current);
        ModelThumbnailHandler::SetOrbitSpeedScale(current);

        auto applyPreviewOrbitSpeed = [sliderPtr, valueFieldPtr](const float& v) {
            float clamped = std::clamp(v, 0.25f, 4.0f);
            if (sliderPtr)
                sliderPtr->SetValue(clamped);
            if (valueFieldPtr)
                valueFieldPtr->SetValue(clamped);

            ModelThumbnailHandler::SetOrbitSpeedScale(clamped);
        };

        slider->SetOnValueChanging([applyPreviewOrbitSpeed](const float& v) {
            applyPreviewOrbitSpeed(v);
        });

        slider->SetOnValueChanged([applyPreviewOrbitSpeed](const float& v) {
            applyPreviewOrbitSpeed(v);
            float clamped = std::clamp(v, 0.25f, 4.0f);

            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetDouble("ui.previewOrbitSpeed", static_cast<double>(clamped));
            prefs.Save(&err);
        });

        valueField->SetOnValueChanging([sliderPtr](const float& value) {
            sliderPtr->SetValueWithoutNotify(std::clamp(value, 0.25f, 4.0f));
        });
        valueField->SetOnValueChanged([sliderPtr](const float& value) {
            sliderPtr->SetValueWithoutNotify(std::clamp(value, 0.25f, 4.0f));
            sliderPtr->NotifyValueChanged();
        });

        InspectorDrag::SetupLabelDragSlider(labelPtr, sliderPtr, nullptr, nullptr, 1.0f);

        section->AddChild(std::move(label));
        section->AddChild(std::move(slider));
        section->AddChild(std::move(valueField));
        m_ContentBody->AddContent(std::move(section));
    }

    // 3D Preview Brightness (model thumbnails and asset 3D preview only)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("3D Preview Brightness");
        label->AddClass("settings-row-label");
        Label* labelPtr = label.get();

        auto slider = std::make_unique<Slider>();
        slider->AddClass("settings-row-slider");
        slider->SetTrackPaddingPx(0.0f);
        slider->SetShowValueBubble(true);
        // UI value is a normalized brightness multiplier where 1.0 represents
        // the default preview intensity (internally mapped to 2.5x).
        slider->SetMin(0.25f);
        slider->SetMax(4.0f);
        slider->SetStep(0.05f);
        Slider* sliderPtr = slider.get();

        auto valueField = std::make_unique<FloatField>();
        valueField->AddClass("settings-row-value");
        InspectorDrag::ApplySliderFloatValueFieldStyle(valueField.get());
        FloatField* valueFieldPtr = valueField.get();

        // UI-facing value maps directly to the directional key-light intensity.
        float current = 1.2f;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            double stored = static_cast<double>(current);
            if (prefs.TryGetDouble("ui.previewLightIntensity", stored))
            {
                current = static_cast<float>(stored);
            }
            current = std::clamp(current, 0.25f, 4.0f);
        }

        slider->SetValue(current);
        valueField->SetValue(current);
        // UI value maps directly to light intensity; 1.0 matches the scene's directional default.
        ModelThumbnailHandler::SetPreviewLightIntensity(current);

        auto applyPreviewBrightness = [sliderPtr, valueFieldPtr](const float& v) {
            float clamped = std::clamp(v, 0.25f, 4.0f);
            if (sliderPtr)
                sliderPtr->SetValue(clamped);
            if (valueFieldPtr)
                valueFieldPtr->SetValue(clamped);

            // UI value maps directly to light intensity.
            ModelThumbnailHandler::SetPreviewLightIntensity(clamped);
        };

        slider->SetOnValueChanging([applyPreviewBrightness](const float& v) {
            applyPreviewBrightness(v);
        });

        slider->SetOnValueChanged([applyPreviewBrightness](const float& v) {
            applyPreviewBrightness(v);
            float clamped = std::clamp(v, 0.25f, 4.0f);

            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetDouble("ui.previewLightIntensity", static_cast<double>(clamped));
            prefs.Save(&err);
        });

        valueField->SetOnValueChanging([sliderPtr](const float& value) {
            sliderPtr->SetValueWithoutNotify(std::clamp(value, 0.25f, 4.0f));
        });
        valueField->SetOnValueChanged([sliderPtr](const float& value) {
            sliderPtr->SetValueWithoutNotify(std::clamp(value, 0.25f, 4.0f));
            sliderPtr->NotifyValueChanged();
        });

        InspectorDrag::SetupLabelDragSlider(labelPtr, sliderPtr, nullptr, nullptr, 1.2f);

        section->AddChild(std::move(label));
        section->AddChild(std::move(slider));
        section->AddChild(std::move(valueField));
        m_ContentBody->AddContent(std::move(section));
    }

    // 3D Preview Ambient fill (model thumbnails and asset 3D preview only)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("3D Preview Ambient");
        label->AddClass("settings-row-label");
        Label* labelPtr = label.get();

        auto slider = std::make_unique<Slider>();
        slider->AddClass("settings-row-slider");
        slider->SetTrackPaddingPx(0.0f);
        slider->SetShowValueBubble(true);
        slider->SetMin(0.0f);
        slider->SetMax(0.5f);
        slider->SetStep(0.01f);
        Slider* sliderPtr = slider.get();

        auto valueField = std::make_unique<FloatField>();
        valueField->AddClass("settings-row-value");
        InspectorDrag::ApplySliderFloatValueFieldStyle(valueField.get());
        FloatField* valueFieldPtr = valueField.get();

        // UI value maps directly to the flat ambient-fill intensity.
        float current = 0.15f;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            double stored = static_cast<double>(current);
            if (prefs.TryGetDouble("ui.previewAmbientIntensity", stored))
            {
                current = static_cast<float>(stored);
            }
            current = std::clamp(current, 0.0f, 0.5f);
        }

        slider->SetValue(current);
        valueField->SetValue(current);
        ModelThumbnailHandler::SetPreviewAmbientIntensity(current);

        auto applyPreviewAmbient = [sliderPtr, valueFieldPtr](const float& v) {
            float clamped = std::clamp(v, 0.0f, 0.5f);
            if (sliderPtr)
                sliderPtr->SetValue(clamped);
            if (valueFieldPtr)
                valueFieldPtr->SetValue(clamped);

            ModelThumbnailHandler::SetPreviewAmbientIntensity(clamped);
        };

        slider->SetOnValueChanging([applyPreviewAmbient](const float& v) {
            applyPreviewAmbient(v);
        });

        slider->SetOnValueChanged([applyPreviewAmbient](const float& v) {
            applyPreviewAmbient(v);
            float clamped = std::clamp(v, 0.0f, 0.5f);

            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetDouble("ui.previewAmbientIntensity", static_cast<double>(clamped));
            prefs.Save(&err);
        });

        valueField->SetOnValueChanging([sliderPtr](const float& value) {
            sliderPtr->SetValueWithoutNotify(std::clamp(value, 0.0f, 0.5f));
        });
        valueField->SetOnValueChanged([sliderPtr](const float& value) {
            sliderPtr->SetValueWithoutNotify(std::clamp(value, 0.0f, 0.5f));
            sliderPtr->NotifyValueChanged();
        });

        InspectorDrag::SetupLabelDragSlider(labelPtr, sliderPtr, nullptr, nullptr, 0.15f);

        section->AddChild(std::move(label));
        section->AddChild(std::move(slider));
        section->AddChild(std::move(valueField));
        m_ContentBody->AddContent(std::move(section));
    }

    // IBL for 3D previews (model thumbnails and asset 3D preview only)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");

        auto toggle = std::make_unique<Toggle>();
        toggle->AddClass("settings-toggle");

        bool iblEnabled = false;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetBool("ui.previewIblEnabled", iblEnabled);
        }
        toggle->SetChecked(iblEnabled);
        ModelThumbnailHandler::SetPreviewIblEnabled(iblEnabled);

        toggle->SetOnValueChanged([this](const bool& enabled) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.previewIblEnabled", enabled);
            prefs.Save(&err);
            ModelThumbnailHandler::SetPreviewIblEnabled(enabled);
            if (m_OnPreviewThumbnailsChanged)
                m_OnPreviewThumbnailsChanged();
        });

        Toggle* iblTogglePtr = toggle.get();
        section->AddChild(std::move(toggle));

        auto label = std::make_unique<Label>();
        label->SetText("Use IBL in 3D Previews");
        label->AddClass("settings-row-label");
        Label* iblLabelPtr = label.get();
        section->InsertChild(0, std::move(label));

        AddDoubleClickReset(iblLabelPtr, [this, iblTogglePtr]() {
            constexpr bool kDefault = false;
            if (iblTogglePtr)
                iblTogglePtr->SetChecked(kDefault);

            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.previewIblEnabled", kDefault);
            prefs.Save(&err);
            ModelThumbnailHandler::SetPreviewIblEnabled(kDefault);
            if (m_OnPreviewThumbnailsChanged)
                m_OnPreviewThumbnailsChanged();
        });

        m_ContentBody->AddContent(std::move(section));
    }

    // Model thumbnail resolution (256px / 512px / 1024px)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Thumbnail Resolution");
        label->AddClass("settings-row-label");
        Label* labelPtr = label.get();
        section->AddChild(std::move(label));

        int current = 1024;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            int64_t stored = 0;
            if (prefs.TryGetInt64("ui.modelThumbnailResolution", stored))
            {
                if (stored == 256 || stored == 512 || stored == 1024)
                    current = static_cast<int>(stored);
            }
        }
        ModelThumbnailHandler::SetThumbnailResolution(current);

        auto applyValue = [this](int value)
        {
            const int clamped = value <= 256 ? 256 : (value <= 512 ? 512 : 1024);

            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetInt64("ui.modelThumbnailResolution", clamped);
            prefs.Save(&err);

            ModelThumbnailHandler::SetThumbnailResolution(clamped);
            if (m_OnPreviewThumbnailsChanged)
                m_OnPreviewThumbnailsChanged();
        };

        auto dropdown = std::make_unique<Dropdown>();
        dropdown->SetId("model-thumbnail-resolution");
        dropdown->AddClass("settings-row-field");
        dropdown->SetAutoWidthPopup(true);
        dropdown->SetOptions({
            {"256", "256x256 (standard)"},
            {"512", "512x512"},
            {"1024", "1024x1024 (high-res)"},
        }, current == 256 ? 0 : (current == 512 ? 1 : 2));
        Dropdown* dropdownPtr = dropdown.get();
        dropdown->SetOnValueChanged([applyValue](const std::string& value) {
            applyValue(value == "256" ? 256 : (value == "512" ? 512 : 1024));
        });

        AddDoubleClickReset(labelPtr, [dropdownPtr]() {
            dropdownPtr->SetSelectedValue("1024");
        });

        section->AddChild(std::move(dropdown));
        m_ContentBody->AddContent(std::move(section));
    }

    // Assets List Columns: choose which columns are visible in the Assets list view.
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Assets List Columns");
        label->AddClass("settings-row-label");
        section->AddChild(std::move(label));

        auto toggleList = std::make_unique<UIElement>();
        toggleList->AddClass("settings-column-toggle-list");
        UIElement* toggleListPtr = toggleList.get();
        section->AddChild(std::move(toggleList));

        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);

        struct ColumnToggle
        {
            const char* id;
            const char* text;
            const char* prefKey;
        };

        const ColumnToggle toggles[] = {
            {"assets-list-col-type",       "Type",        "ui.assets.list.showType"},
            {"assets-list-col-size",       "Size",        "ui.assets.list.showSize"},
            {"assets-list-col-dimensions", "Dimensions",  "ui.assets.list.showDimensions"},
            {"assets-list-col-modified",   "Modified",    "ui.assets.list.showModified"},
            {"assets-list-col-git",        "Git Status",  "ui.assets.list.showGit"},
            {"assets-list-col-tag",        "Tag",         "ui.assets.list.showTag"},
            {"assets-list-col-referenced", "Referenced",  "ui.assets.list.showReferenced"},
            {"assets-list-col-custom",     "Custom Prop", "ui.assets.list.showCustom"},
            {"assets-list-col-creator",    "Creator",     "ui.assets.list.showCreator"},
        };

        auto makeToggleRow = [this, toggleListPtr, &prefs](const ColumnToggle& cfg)
        {
            auto row = std::make_unique<UIElement>();
            row->AddClass("settings-radio-option");

            auto cb = std::make_unique<Checkbox>();
            cb->SetId(cfg.id);
            cb->AddClass("settings-radio");

            bool visible = true;
            prefs.TryGetBool(cfg.prefKey, visible);
            cb->SetChecked(visible);

            Checkbox* rawCb = cb.get();
            row->AddChild(std::move(cb));

            auto lbl = std::make_unique<Label>();
            lbl->SetText(cfg.text);
            lbl->AddClass("settings-radio-label");
            row->AddChild(std::move(lbl));

            rawCb->SetOnValueChanged([this, prefKey = std::string(cfg.prefKey)](const bool& enabled)
            {
                auto p = Editor::OpenEditorPreferences();
                std::string e2;
                p.Load(&e2);
                p.SetBool(prefKey.c_str(), enabled);
                p.Save(&e2);
                if (m_OnAssetsListColumnsChanged)
                    m_OnAssetsListColumnsChanged();
            });

            toggleListPtr->AddChild(std::move(row));
        };

        for (const ColumnToggle& cfg : toggles)
            makeToggleRow(cfg);

        m_ContentBody->AddContent(std::move(section));
    }

    // Text Truncation toggle
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");
        
        auto toggle = std::make_unique<Toggle>();
        toggle->AddClass("settings-toggle");
        
        // Load from preferences
        bool truncationEnabled = true;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetBool("ui.textTruncation", truncationEnabled);
        }
        toggle->SetChecked(truncationEnabled);
        
        toggle->SetOnValueChanged([this](const bool& enabled) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.textTruncation", enabled);
            if (!prefs.Save(&err) && !err.empty())
            {
                Logger::Log::Error("Failed to save text truncation setting: {}", err);
            }
            
            if (m_OnTruncationEnabledChanged)
                m_OnTruncationEnabledChanged(enabled);
        });

        Toggle* truncationTogglePtr = toggle.get();
        section->AddChild(std::move(toggle));
        
        auto label = std::make_unique<Label>();
        label->SetText("Text Truncation");
        label->AddClass("settings-row-label");
        Label* truncationLabelPtr = label.get();
        section->InsertChild(0, std::move(label));
        AddDoubleClickReset(truncationLabelPtr, [this, truncationTogglePtr]() {
            const bool kDefault = true;
            truncationTogglePtr->SetChecked(kDefault);
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.textTruncation", kDefault);
            prefs.Save(&err);
            if (m_OnTruncationEnabledChanged)
                m_OnTruncationEnabledChanged(kDefault);
        });
        m_ContentBody->AddContent(std::move(section));
    }
    
    // Truncation Threshold setting
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        
        auto label = std::make_unique<Label>();
        label->SetText("Truncation Threshold");
        label->AddClass("settings-row-label");
        Label* truncationThresholdLabelPtr = label.get();
        section->AddChild(std::move(label));
        
        auto slider = std::make_unique<Slider>();
        Slider* truncationThresholdSliderPtr = slider.get();
        slider->AddClass("settings-row-slider");
        slider->SetTrackPaddingPx(0.0f);
        slider->SetShowValueBubble(true);
        slider->SetMin(64.0f);
        slider->SetMax(256.0f);
        slider->SetStep(8.0f);
        
        // Load from preferences
        float threshold = 128.0f;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            double stored = threshold;
            if (prefs.TryGetDouble("ui.gridTruncationThreshold", stored))
                threshold = static_cast<float>(stored);
        }
        slider->SetValue(threshold);

        auto valueField = std::make_unique<FloatField>();
        valueField->AddClass("settings-row-value");
        InspectorDrag::ApplySliderFloatValueFieldStyle(valueField.get());
        valueField->SetValue(threshold);
        FloatField* valueFieldPtr = valueField.get();

        auto applyThreshold = [this](float value) {
            const float clamped = std::clamp(value, 64.0f, 256.0f);
            if (m_OnTruncationThresholdChanged)
                m_OnTruncationThresholdChanged(clamped);
            return clamped;
        };

        auto saveThreshold = [](float value) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetDouble("ui.gridTruncationThreshold", value);
            if (!prefs.Save(&err) && !err.empty())
                Logger::Log::Error("Failed to save truncation threshold: {}", err);
        };

        // Live update during drag via callback (no disk write)
        slider->SetOnValueChanging([valueFieldPtr, applyThreshold](const float& value) {
            const float clamped = applyThreshold(value);
            if (valueFieldPtr)
                valueFieldPtr->SetValueWithoutNotify(clamped);
        });

        // Save to disk on release
        slider->SetOnValueChanged([valueFieldPtr, applyThreshold, saveThreshold](const float& value) {
            const float clamped = applyThreshold(value);
            if (valueFieldPtr)
                valueFieldPtr->SetValueWithoutNotify(clamped);
            saveThreshold(clamped);
        });
        valueField->SetOnValueChanging([truncationThresholdSliderPtr, applyThreshold](const float& value) {
            const float clamped = applyThreshold(value);
            if (truncationThresholdSliderPtr)
                truncationThresholdSliderPtr->SetValueWithoutNotify(clamped);
        });
        valueField->SetOnValueChanged(
            [truncationThresholdSliderPtr, valueFieldPtr, applyThreshold, saveThreshold](const float& value) {
                const float clamped = applyThreshold(value);
                if (truncationThresholdSliderPtr)
                    truncationThresholdSliderPtr->SetValueWithoutNotify(clamped);
                if (valueFieldPtr)
                    valueFieldPtr->SetValueWithoutNotify(clamped);
                saveThreshold(clamped);
        });
        const float kDefaultTruncationThreshold = 128.0f;
        InspectorDrag::SetupLabelDragSlider(
            truncationThresholdLabelPtr,
            truncationThresholdSliderPtr,
            nullptr,
            nullptr,
            kDefaultTruncationThreshold);
        section->AddChild(std::move(slider));
        section->AddChild(std::move(valueField));
        m_ContentBody->AddContent(std::move(section));
    }
    
    // Smart Folders At Top toggle
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");
        
        auto toggle = std::make_unique<Toggle>();
        m_SmartFoldersAtTopToggle = toggle.get();
        toggle->AddClass("settings-toggle");
        
        // Load preference
        bool atTop = true;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetBool("ui.smartFoldersAtTop", atTop);
        }
        toggle->SetChecked(atTop);
        
        // Hook up callback
        toggle->SetOnValueChanged([this](const bool& atTop) {
            // Save preference
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.smartFoldersAtTop", atTop);
            prefs.Save(&err);
            
            // Notify listeners
            if (m_OnSmartFoldersAtTopChanged)
                m_OnSmartFoldersAtTopChanged(atTop);
        });
        
        section->AddChild(std::move(toggle));
        
        auto label = std::make_unique<Label>();
        label->SetText("Smart Folders At Top");
        label->AddClass("settings-row-label");
        Label* smartFoldersLabelPtr = label.get();
        section->InsertChild(0, std::move(label));
        AddDoubleClickReset(smartFoldersLabelPtr, [this]() {
            const bool kDefault = true;
            if (m_SmartFoldersAtTopToggle)
                m_SmartFoldersAtTopToggle->SetChecked(kDefault);
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.smartFoldersAtTop", kDefault);
            prefs.Save(&err);
            if (m_OnSmartFoldersAtTopChanged)
                m_OnSmartFoldersAtTopChanged(kDefault);
        });
        m_ContentBody->AddContent(std::move(section));
    }

    // Expand Smart Folders On Startup toggle
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");

        auto toggle = std::make_unique<Toggle>();
        m_SmartFoldersExpandedOnStartupToggle = toggle.get();
        toggle->AddClass("settings-toggle");

        bool expanded = false;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetBool("ui.assets.smartFoldersExpandedOnStartup", expanded);
        }
        toggle->SetChecked(expanded);

        toggle->SetOnValueChanged([this](const bool& enabled) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.assets.smartFoldersExpandedOnStartup", enabled);
            prefs.Save(&err);

            if (m_OnSmartFoldersExpandedOnStartupChanged)
                m_OnSmartFoldersExpandedOnStartupChanged(enabled);
        });

        section->AddChild(std::move(toggle));

        auto label = std::make_unique<Label>();
        label->SetText("Expand Smart Folders");
        label->SetTooltip("Expand Smart Folders On Startup");
        label->AddClass("settings-row-label");
        Label* smartFoldersStartupLabelPtr = label.get();
        section->InsertChild(0, std::move(label));
        AddDoubleClickReset(smartFoldersStartupLabelPtr, [this]() {
            constexpr bool kDefault = false;
            if (m_SmartFoldersExpandedOnStartupToggle)
                m_SmartFoldersExpandedOnStartupToggle->SetChecked(kDefault);
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.assets.smartFoldersExpandedOnStartup", kDefault);
            prefs.Save(&err);
            if (m_OnSmartFoldersExpandedOnStartupChanged)
                m_OnSmartFoldersExpandedOnStartupChanged(kDefault);
        });
        m_ContentBody->AddContent(std::move(section));
    }

}

void SettingsPanel::CreateUIInspectorContent()
{
    if (!m_ContentBody)
        return;

    // Compact component headers (24px instead of 28px)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");

        bool compact = true;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetBool(kPrefKeyCompactComponentHeaders, compact);
        }

        auto toggle = std::make_unique<Toggle>();
        m_CompactComponentHeadersToggle = toggle.get();
        toggle->AddClass("settings-toggle");
        toggle->SetChecked(compact);
        toggle->SetOnValueChanged([this](const bool& enabled) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool(kPrefKeyCompactComponentHeaders, enabled);
            prefs.Save(&err);
            ApplySavedCompactComponentHeadersStyle(GetOwnerManager());
        });
        section->AddChild(std::move(toggle));

        auto label = std::make_unique<Label>();
        label->SetText("Compact Headers (24px)");
        label->SetTooltip("Compact Component Headers (24px)");
        label->AddClass("settings-row-label");
        Label* compactLabelPtr = label.get();
        section->InsertChild(0, std::move(label));
        AddDoubleClickReset(compactLabelPtr, [this]() {
            const bool kDefault = true;
            if (m_CompactComponentHeadersToggle)
                m_CompactComponentHeadersToggle->SetChecked(kDefault);
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool(kPrefKeyCompactComponentHeaders, kDefault);
            prefs.Save(&err);
            ApplySavedCompactComponentHeadersStyle(GetOwnerManager());
        });
        m_ContentBody->AddContent(std::move(section));
    }

    // Filled inspector sections
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");

        bool enabled = false;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetBool(kPrefKeyInspectorFilledSections, enabled);
        }

        auto toggle = std::make_unique<Toggle>();
        m_InspectorFilledSectionsToggle = toggle.get();
        toggle->AddClass("settings-toggle");
        toggle->SetChecked(enabled);
        toggle->SetOnValueChanged([this](const bool& filled) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool(kPrefKeyInspectorFilledSections, filled);
            prefs.Save(&err);
            if (m_OnInspectorFilledSectionsChanged)
                m_OnInspectorFilledSectionsChanged(filled);
        });
        section->AddChild(std::move(toggle));

        auto label = std::make_unique<Label>();
        label->SetText("Filled Inspector Sections");
        label->AddClass("settings-row-label");
        Label* filledSectionsLabelPtr = label.get();
        section->InsertChild(0, std::move(label));
        AddDoubleClickReset(filledSectionsLabelPtr, [this]() {
            const bool kDefault = false;
            if (m_InspectorFilledSectionsToggle)
                m_InspectorFilledSectionsToggle->SetChecked(kDefault);
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool(kPrefKeyInspectorFilledSections, kDefault);
            prefs.Save(&err);
            if (m_OnInspectorFilledSectionsChanged)
                m_OnInspectorFilledSectionsChanged(kDefault);
        });
        m_ContentBody->AddContent(std::move(section));
    }

    // Show explanatory info cards inside inspectors.
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");

        bool visible = true;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetBool(kPrefKeyInspectorShowInfoCards, visible);
        }

        auto toggle = std::make_unique<Toggle>();
        m_InspectorInfoCardsToggle = toggle.get();
        toggle->AddClass("settings-toggle");
        toggle->SetChecked(visible);
        toggle->SetOnValueChanged([this](const bool& showCards) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool(kPrefKeyInspectorShowInfoCards, showCards);
            prefs.Save(&err);
            if (m_OnInspectorInfoCardsChanged)
                m_OnInspectorInfoCardsChanged(showCards);
        });
        section->AddChild(std::move(toggle));

        auto label = std::make_unique<Label>();
        label->SetText("Show Inspector Info Cards");
        label->AddClass("settings-row-label");
        Label* infoCardsLabelPtr = label.get();
        section->InsertChild(0, std::move(label));
        AddDoubleClickReset(infoCardsLabelPtr, [this]() {
            constexpr bool kDefault = true;
            if (m_InspectorInfoCardsToggle)
                m_InspectorInfoCardsToggle->SetChecked(kDefault);
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool(kPrefKeyInspectorShowInfoCards, kDefault);
            prefs.Save(&err);
            if (m_OnInspectorInfoCardsChanged)
                m_OnInspectorInfoCardsChanged(kDefault);
        });
        m_ContentBody->AddContent(std::move(section));
    }

    // Global visibility for component arrows and nested inspector foldout chevrons.
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");

        auto toggle = std::make_unique<Toggle>();
        m_InspectorCollapseArrowVisibleToggle = toggle.get();
        toggle->AddClass("settings-toggle");

        bool visible = false;  // default: arrows hidden
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetBool("ui.inspectorShowCollapseArrow", visible);
        }
        toggle->SetChecked(visible);

        toggle->SetOnValueChanged([this](const bool& visible) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.inspectorShowCollapseArrow", visible);
            prefs.Save(&err);
            if (m_OnInspectorCollapseArrowVisibilityChanged)
                m_OnInspectorCollapseArrowVisibilityChanged(visible);
        });

        section->AddChild(std::move(toggle));

        auto label = std::make_unique<Label>();
        label->SetText("Show Inspector Chevrons");
        label->SetTooltip("Show expand/collapse chevrons in all inspectors, including nested sections. Headers remain clickable when chevrons are hidden.");
        label->AddClass("settings-row-label");
        Label* collapseArrowLabelPtr = label.get();
        section->InsertChild(0, std::move(label));
        AddDoubleClickReset(collapseArrowLabelPtr, [this]() {
            const bool kDefault = false;
            if (m_InspectorCollapseArrowVisibleToggle)
                m_InspectorCollapseArrowVisibleToggle->SetChecked(kDefault);
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.inspectorShowCollapseArrow", kDefault);
            prefs.Save(&err);
            if (m_OnInspectorCollapseArrowVisibilityChanged)
                m_OnInspectorCollapseArrowVisibilityChanged(kDefault);
        });
        m_ContentBody->AddContent(std::move(section));
    }

    // Show component icons in inspector section headers
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");

        auto toggle = std::make_unique<Toggle>();
        m_InspectorComponentIconsVisibleToggle = toggle.get();
        toggle->AddClass("settings-toggle");

        bool visible = true; // default: icons shown
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetBool("ui.inspectorShowComponentIcons", visible);
        }
        toggle->SetChecked(visible);

        toggle->SetOnValueChanged([this](const bool& visibleValue) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.inspectorShowComponentIcons", visibleValue);
            prefs.Save(&err);
            if (m_OnInspectorComponentIconsVisibilityChanged)
                m_OnInspectorComponentIconsVisibilityChanged(visibleValue);
        });

        section->AddChild(std::move(toggle));

        auto label = std::make_unique<Label>();
        label->SetText("Show Component Icons");
        label->AddClass("settings-row-label");
        Label* iconsLabelPtr = label.get();
        section->InsertChild(0, std::move(label));
        AddDoubleClickReset(iconsLabelPtr, [this]() {
            const bool kDefault = true;
            if (m_InspectorComponentIconsVisibleToggle)
                m_InspectorComponentIconsVisibleToggle->SetChecked(kDefault);
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.inspectorShowComponentIcons", kDefault);
            prefs.Save(&err);
            if (m_OnInspectorComponentIconsVisibilityChanged)
                m_OnInspectorComponentIconsVisibilityChanged(kDefault);
        });
        m_ContentBody->AddContent(std::move(section));
    }

    // Big number spacing in inspector numeric fields (50 000 000).
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");

        auto toggle = std::make_unique<Toggle>();
        m_InspectorBigNumberSpacingToggle = toggle.get();
        toggle->AddClass("settings-toggle");

        toggle->SetChecked(FloatField::IsBigNumberSpacingEnabled());

        toggle->SetOnValueChanged([this](const bool& enabledValue) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool(kPrefKeyInspectorBigNumberSpacing, enabledValue);
            prefs.Save(&err);
            FloatField::SetBigNumberSpacingEnabled(enabledValue);
            if (m_OnInspectorBigNumberSpacingChanged)
                m_OnInspectorBigNumberSpacingChanged(enabledValue);
        });

        section->AddChild(std::move(toggle));

        auto label = std::make_unique<Label>();
        label->SetText("Big Number Spacing");
        label->AddClass("settings-toggle-label");
        Label* spacingLabelPtr = label.get();
        section->AddChild(std::move(label));
        AddDoubleClickReset(spacingLabelPtr, [this]() {
            if (m_InspectorBigNumberSpacingToggle)
                m_InspectorBigNumberSpacingToggle->SetChecked(kInspectorBigNumberSpacingDefault);
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool(kPrefKeyInspectorBigNumberSpacing, kInspectorBigNumberSpacingDefault);
            prefs.Save(&err);
            FloatField::SetBigNumberSpacingEnabled(kInspectorBigNumberSpacingDefault);
            if (m_OnInspectorBigNumberSpacingChanged)
                m_OnInspectorBigNumberSpacingChanged(kInspectorBigNumberSpacingDefault);
        });
        m_ContentBody->AddContent(std::move(section));
    }

    // Inspector solo sections (only one section expanded at a time)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");

        auto toggle = std::make_unique<Toggle>();
        m_InspectorSoloSectionsToggle = toggle.get();
        toggle->AddClass("settings-toggle");

        bool enabled = false; // default: solo mode off
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetBool("ui.inspectorSoloSections", enabled);
        }
        toggle->SetChecked(enabled);

        toggle->SetOnValueChanged([this](const bool& enabledValue) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.inspectorSoloSections", enabledValue);
            prefs.Save(&err);
            if (m_OnInspectorSoloSectionsChanged)
                m_OnInspectorSoloSectionsChanged(enabledValue);
        });

        section->AddChild(std::move(toggle));

        auto label = std::make_unique<Label>();
        label->SetText("Solo Section Mode");
        label->SetTooltip("Only one inspector section expanded at a time");
        label->AddClass("settings-row-label");
        Label* soloLabelPtr = label.get();
        section->InsertChild(0, std::move(label));

        AddDoubleClickReset(soloLabelPtr, [this]() {
            const bool kDefault = false;
            if (m_InspectorSoloSectionsToggle)
                m_InspectorSoloSectionsToggle->SetChecked(kDefault);
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.inspectorSoloSections", kDefault);
            prefs.Save(&err);
            if (m_OnInspectorSoloSectionsChanged)
                m_OnInspectorSoloSectionsChanged(kDefault);
        });

        m_ContentBody->AddContent(std::move(section));
    }

    // Inspector solo mode: always keep Transform section expanded
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");

        auto toggle = std::make_unique<Toggle>();
        m_InspectorSoloKeepTransformToggle = toggle.get();
        toggle->AddClass("settings-toggle");

        bool enabled = false; // default: off
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetBool("ui.inspectorSoloKeepTransform", enabled);
        }
        toggle->SetChecked(enabled);

        toggle->SetOnValueChanged([this](const bool& enabledValue) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.inspectorSoloKeepTransform", enabledValue);
            prefs.Save(&err);
            if (m_OnInspectorSoloKeepTransformChanged)
                m_OnInspectorSoloKeepTransformChanged(enabledValue);
        });

        section->AddChild(std::move(toggle));

        auto label = std::make_unique<Label>();
        label->SetText("Solo: Keep Transform Open");
        label->SetTooltip("In solo mode, always keep Transform section expanded");
        label->AddClass("settings-row-label");
        Label* soloKeepTransformLabelPtr = label.get();
        section->InsertChild(0, std::move(label));

        AddDoubleClickReset(soloKeepTransformLabelPtr, [this]() {
            const bool kDefault = false;
            if (m_InspectorSoloKeepTransformToggle)
                m_InspectorSoloKeepTransformToggle->SetChecked(kDefault);
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool("ui.inspectorSoloKeepTransform", kDefault);
            prefs.Save(&err);
            if (m_OnInspectorSoloKeepTransformChanged)
                m_OnInspectorSoloKeepTransformChanged(kDefault);
        });

        m_ContentBody->AddContent(std::move(section));
    }

    // Inspector row indent (padding-left for non-Transform sections)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Inspector Row Indent");
        label->AddClass("settings-row-label");
        Label* indentLabelPtr = label.get();
        section->AddChild(std::move(label));

        auto slider = std::make_unique<Slider>();
        m_InspectorRowIndentSlider = slider.get();
        slider->AddClass("settings-row-slider");
        slider->SetTrackPaddingPx(0.0f);
        slider->SetShowValueBubble(true);
        slider->SetMin(0.0f);
        slider->SetMax(40.0f);
        slider->SetStep(1.0f);

        const float kDefaultIndent = 0.0f;
        float indent = kDefaultIndent;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            double stored = indent;
            if (prefs.TryGetDouble("ui.inspectorRowIndent", stored))
                indent = static_cast<float>(stored);
        }
        slider->SetValue(indent);

        float gap = 4.0f;
        float labelWidth = 37.0f;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            double storedGap = gap;
            if (prefs.TryGetDouble("ui.inspectorRowGap", storedGap))
                gap = static_cast<float>(storedGap);
            double storedLW = labelWidth;
            if (prefs.TryGetDouble("ui.inspectorLabelWidth", storedLW))
                labelWidth = static_cast<float>(storedLW);
        }
        ApplyInspectorRowStyleToUI(indent, gap, labelWidth);

        auto valueField = std::make_unique<FloatField>();
        valueField->AddClass("settings-row-value");
        InspectorDrag::ApplySliderFloatValueFieldStyle(valueField.get());
        valueField->SetValue(indent);
        FloatField* valueFieldPtr = valueField.get();
        Slider* sliderPtr = slider.get();

        auto getLabelWidth = [this]() {
            return m_InspectorLabelWidthSlider ? m_InspectorLabelWidthSlider->GetValue() : 37.0f;
        };

        auto applyIndent = [this, getLabelWidth](float value) {
            const float clamped = std::clamp(value, 0.0f, 40.0f);
            float currentGap = 4.0f;
            if (m_InspectorRowGapSlider)
                currentGap = m_InspectorRowGapSlider->GetValue();
            ApplyInspectorRowStyleToUI(clamped, currentGap, getLabelWidth());
            return clamped;
        };
        auto saveIndent = [](float value) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetDouble("ui.inspectorRowIndent", static_cast<double>(value));
            prefs.Save(&err);
        };

        slider->SetOnValueChanging([valueFieldPtr, applyIndent](const float& value) {
            const float clamped = applyIndent(value);
            if (valueFieldPtr)
                valueFieldPtr->SetValueWithoutNotify(clamped);
        });
        slider->SetOnValueChanged([valueFieldPtr, applyIndent, saveIndent](const float& value) {
            const float clamped = applyIndent(value);
            if (valueFieldPtr)
                valueFieldPtr->SetValueWithoutNotify(clamped);
            saveIndent(clamped);
        });
        valueField->SetOnValueChanging([sliderPtr, applyIndent](const float& value) {
            const float clamped = applyIndent(value);
            if (sliderPtr)
                sliderPtr->SetValueWithoutNotify(clamped);
        });
        valueField->SetOnValueChanged([sliderPtr, valueFieldPtr, applyIndent, saveIndent](const float& value) {
            const float clamped = applyIndent(value);
            if (sliderPtr)
                sliderPtr->SetValueWithoutNotify(clamped);
            if (valueFieldPtr)
                valueFieldPtr->SetValueWithoutNotify(clamped);
            saveIndent(clamped);
        });

        InspectorDrag::SetupLabelDragSlider(
            indentLabelPtr, m_InspectorRowIndentSlider, nullptr, nullptr, kDefaultIndent);

        section->AddChild(std::move(slider));
        section->AddChild(std::move(valueField));
        m_ContentBody->AddContent(std::move(section));
    }

    // Inspector row vertical gap (margin-bottom)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Inspector Row Gap");
        label->AddClass("settings-row-label");
        Label* gapLabelPtr = label.get();
        section->AddChild(std::move(label));

        auto slider = std::make_unique<Slider>();
        m_InspectorRowGapSlider = slider.get();
        slider->AddClass("settings-row-slider");
        slider->SetTrackPaddingPx(0.0f);
        slider->SetShowValueBubble(true);
        slider->SetMin(0.0f);
        slider->SetMax(16.0f);
        slider->SetStep(1.0f);

        const float kDefaultGap = 4.0f;
        float gap = kDefaultGap;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            double stored = gap;
            if (prefs.TryGetDouble("ui.inspectorRowGap", stored))
                gap = static_cast<float>(stored);
        }
        slider->SetValue(gap);

        auto valueField = std::make_unique<FloatField>();
        valueField->AddClass("settings-row-value");
        InspectorDrag::ApplySliderFloatValueFieldStyle(valueField.get());
        valueField->SetValue(gap);
        FloatField* valueFieldPtr = valueField.get();
        Slider* sliderPtr = slider.get();

        auto getLabelWidth2 = [this]() {
            return m_InspectorLabelWidthSlider ? m_InspectorLabelWidthSlider->GetValue() : 37.0f;
        };

        auto applyGap = [this, getLabelWidth2](float value) {
            const float clamped = std::clamp(value, 0.0f, 16.0f);
            float currentIndent = 0.0f;
            if (m_InspectorRowIndentSlider)
                currentIndent = m_InspectorRowIndentSlider->GetValue();
            ApplyInspectorRowStyleToUI(currentIndent, clamped, getLabelWidth2());
            return clamped;
        };
        auto saveGap = [](float value) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetDouble("ui.inspectorRowGap", static_cast<double>(value));
            prefs.Save(&err);
        };

        slider->SetOnValueChanging([valueFieldPtr, applyGap](const float& value) {
            const float clamped = applyGap(value);
            if (valueFieldPtr)
                valueFieldPtr->SetValueWithoutNotify(clamped);
        });
        slider->SetOnValueChanged([valueFieldPtr, applyGap, saveGap](const float& value) {
            const float clamped = applyGap(value);
            if (valueFieldPtr)
                valueFieldPtr->SetValueWithoutNotify(clamped);
            saveGap(clamped);
        });
        valueField->SetOnValueChanging([sliderPtr, applyGap](const float& value) {
            const float clamped = applyGap(value);
            if (sliderPtr)
                sliderPtr->SetValueWithoutNotify(clamped);
        });
        valueField->SetOnValueChanged([sliderPtr, valueFieldPtr, applyGap, saveGap](const float& value) {
            const float clamped = applyGap(value);
            if (sliderPtr)
                sliderPtr->SetValueWithoutNotify(clamped);
            if (valueFieldPtr)
                valueFieldPtr->SetValueWithoutNotify(clamped);
            saveGap(clamped);
        });

        InspectorDrag::SetupLabelDragSlider(
            gapLabelPtr, m_InspectorRowGapSlider, nullptr, nullptr, kDefaultGap);

        section->AddChild(std::move(slider));
        section->AddChild(std::move(valueField));
        m_ContentBody->AddContent(std::move(section));
    }

    // Inspector label width (percentage of row)
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Inspector Label Width");
        label->AddClass("settings-row-label");
        Label* labelWidthLabelPtr = label.get();
        section->AddChild(std::move(label));

        auto slider = std::make_unique<Slider>();
        m_InspectorLabelWidthSlider = slider.get();
        slider->AddClass("settings-row-slider");
        slider->SetTrackPaddingPx(0.0f);
        slider->SetShowValueBubble(true);
        slider->SetMin(20.0f);
        slider->SetMax(60.0f);
        slider->SetStep(1.0f);

        const float kDefaultLabelWidth = 37.0f;
        float labelWidth = kDefaultLabelWidth;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            double stored = labelWidth;
            if (prefs.TryGetDouble("ui.inspectorLabelWidth", stored))
                labelWidth = static_cast<float>(stored);
        }
        slider->SetValue(labelWidth);

        auto valueField = std::make_unique<FloatField>();
        valueField->AddClass("settings-row-value");
        InspectorDrag::ApplySliderFloatValueFieldStyle(valueField.get());
        valueField->SetValue(labelWidth);
        FloatField* valueFieldPtr = valueField.get();
        Slider* sliderPtr = slider.get();

        auto applyLabelWidth = [this](float value) {
            const float clamped = std::clamp(value, 20.0f, 60.0f);
            float currentIndent = 0.0f;
            if (m_InspectorRowIndentSlider)
                currentIndent = m_InspectorRowIndentSlider->GetValue();
            float currentGap = 4.0f;
            if (m_InspectorRowGapSlider)
                currentGap = m_InspectorRowGapSlider->GetValue();
            ApplyInspectorRowStyleToUI(currentIndent, currentGap, clamped);
            return clamped;
        };
        auto saveLabelWidth = [](float value) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetDouble("ui.inspectorLabelWidth", static_cast<double>(value));
            prefs.Save(&err);
        };

        slider->SetOnValueChanging([valueFieldPtr, applyLabelWidth](const float& value) {
            const float clamped = applyLabelWidth(value);
            if (valueFieldPtr)
                valueFieldPtr->SetValueWithoutNotify(clamped);
        });
        slider->SetOnValueChanged([valueFieldPtr, applyLabelWidth, saveLabelWidth](const float& value) {
            const float clamped = applyLabelWidth(value);
            if (valueFieldPtr)
                valueFieldPtr->SetValueWithoutNotify(clamped);
            saveLabelWidth(clamped);
        });
        valueField->SetOnValueChanging([sliderPtr, applyLabelWidth](const float& value) {
            const float clamped = applyLabelWidth(value);
            if (sliderPtr)
                sliderPtr->SetValueWithoutNotify(clamped);
        });
        valueField->SetOnValueChanged(
            [sliderPtr, valueFieldPtr, applyLabelWidth, saveLabelWidth](const float& value) {
                const float clamped = applyLabelWidth(value);
                if (sliderPtr)
                    sliderPtr->SetValueWithoutNotify(clamped);
                if (valueFieldPtr)
                    valueFieldPtr->SetValueWithoutNotify(clamped);
                saveLabelWidth(clamped);
        });

        InspectorDrag::SetupLabelDragSlider(
            labelWidthLabelPtr,
            m_InspectorLabelWidthSlider,
            nullptr,
            nullptr,
            kDefaultLabelWidth);

        section->AddChild(std::move(slider));
        section->AddChild(std::move(valueField));
        m_ContentBody->AddContent(std::move(section));
    }

    // Inspector toggle alignment (Left / Middle / Right) — radio behaviour
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-radio-group");

        auto label = std::make_unique<Label>();
        label->SetText("Toggle alignment");
        label->AddClass("settings-row-label");
        section->AddChild(std::move(label));

        UIElement* sectionPtr = section.get();
        auto makeRadio = [sectionPtr](const char* id, const char* text) -> Checkbox* {
            auto row = std::make_unique<UIElement>();
            row->AddClass("settings-radio-option");
            auto cb = std::make_unique<Checkbox>();
            cb->SetId(id);
            cb->AddClass("settings-radio");
            Checkbox* raw = cb.get();
            row->AddChild(std::move(cb));
            auto lbl = std::make_unique<Label>();
            lbl->SetText(text);
            lbl->AddClass("settings-radio-label");
            row->AddChild(std::move(lbl));
            sectionPtr->AddChild(std::move(row));
            return raw;
        };

        Checkbox* leftCb = makeRadio("inspector-toggle-align-left", "Left");
        Checkbox* middleCb = makeRadio("inspector-toggle-align-middle", "Middle");
        Checkbox* rightCb = makeRadio("inspector-toggle-align-right", "Right");
        m_InspectorToggleAlignLeft = leftCb;
        m_InspectorToggleAlignMiddle = middleCb;
        m_InspectorToggleAlignRight = rightCb;

        auto syncFromValue = [leftCb, middleCb, rightCb](const std::string& value) {
            leftCb->SetChecked(value == "left");
            middleCb->SetChecked(value == "middle");
            rightCb->SetChecked(value == "right");
        };

        std::string current = "left";
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetString("ui.inspectorToggleAlign", current);
            if (current != "left" && current != "middle" && current != "right")
                current = "left";
        }
        syncFromValue(current);

        auto onPicked = [this, leftCb, middleCb, rightCb, syncFromValue](const std::string& value) {
            syncFromValue(value);
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetString("ui.inspectorToggleAlign", value);
            prefs.Save(&err);
            if (m_OnInspectorToggleAlignChanged)
                m_OnInspectorToggleAlignChanged(value);
        };

        // Derive selected value from checkbox state and notify once (robust to event order / double-firing)
        auto applyFromSelection = [onPicked, leftCb, middleCb, rightCb]() {
            const std::string value = middleCb->IsChecked() ? "middle" : (rightCb->IsChecked() ? "right" : "left");
            onPicked(value);
        };

        leftCb->SetOnValueChanged([applyFromSelection, middleCb, rightCb](const bool& checked) {
            if (checked) {
                middleCb->SetChecked(false);
                rightCb->SetChecked(false);
            }
            applyFromSelection();
        });
        middleCb->SetOnValueChanged([applyFromSelection, leftCb, rightCb](const bool& checked) {
            if (checked) {
                leftCb->SetChecked(false);
                rightCb->SetChecked(false);
            }
            applyFromSelection();
        });
        rightCb->SetOnValueChanged([applyFromSelection, leftCb, middleCb](const bool& checked) {
            if (checked) {
                leftCb->SetChecked(false);
                middleCb->SetChecked(false);
            }
            applyFromSelection();
        });

        m_ContentBody->AddContent(std::move(section));
    }

    // Gray Sliders toggle
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        bool graySliders = false;
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetBool(kPrefKeyGraySliders, graySliders);
        }

        auto toggle = std::make_unique<Toggle>();
        toggle->AddClass("settings-toggle");
        toggle->SetChecked(graySliders);
        toggle->SetOnValueChanged([this](const bool& checked) {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool(kPrefKeyGraySliders, checked);
            prefs.Save(&err);
            ApplyGraySlidersStyleToUI();
        });
        m_GraySlidersToggle = toggle.get();
        section->AddChild(std::move(toggle));

        auto label = std::make_unique<Label>();
        label->SetText("Gray Sliders");
        label->AddClass("settings-row-label");
        Label* graySlidersLabelPtr = label.get();
        section->InsertChild(0, std::move(label));
        AddDoubleClickReset(graySlidersLabelPtr, [this]() {
            constexpr bool kDefault = false;
            if (m_GraySlidersToggle)
                m_GraySlidersToggle->SetChecked(kDefault);
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetBool(kPrefKeyGraySliders, kDefault);
            prefs.Save(&err);
            ApplyGraySlidersStyleToUI();
        });
        m_ContentBody->AddContent(std::move(section));
    }
}

void SettingsPanel::CreatePhysicsSettingsContent()
{
    if (!m_ContentBody)
        return;

    const Physics::PhysicsWorldSettings s =
        Editor::PhysicsProjectSettings::Load(EngineCore::GetInstance().GetWorkspaceRoot());

    // Persists the four keys this page owns, merging them into the stored block.
    // The new values reach the physics world the next time it is created --
    // PhysicsWorldBootstrapSystem reads PhysicsWorldSettingsComponent only at
    // PhysicsWorld construction, so there is no live-apply path to call here.
    auto save = [](const Physics::PhysicsWorldSettings& settings) {
        (void)Editor::PhysicsProjectSettings::SaveSimulationSettings(
            EngineCore::GetInstance().GetWorkspaceRoot(), settings);
    };

    // Helper: single-row float field
    auto addFloatRow = [&](const char* labelText, float initialValue,
                           float defaultValue,
                           float minValue,
                           float maxValue,
                           std::function<void(float)> onCommit) {
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");
        auto label = std::make_unique<Label>();
        label->SetText(labelText);
        label->AddClass("settings-row-label");
        Label* labelPtr = label.get();
        row->AddChild(std::move(label));
        auto field = std::make_unique<FloatField>();
        field->AddClass("settings-row-value");
        FloatField* fieldPtr = field.get();
        field->SetValue(initialValue);
        WireSettingsFloatLabelDrag(
            m_EditorContext,
            labelPtr,
            fieldPtr,
            (std::string("Change Physics ") + labelText).c_str(),
            defaultValue,
            minValue,
            maxValue,
            [minValue, maxValue](float value) { return std::clamp(value, minValue, maxValue); },
            std::move(onCommit));
        row->AddChild(std::move(field));
        m_ContentBody->AddContent(std::move(row));
    };

    // Helper: single-row int field
    auto addIntRow = [&](const char* labelText, int initialValue,
                         int defaultValue,
                         int minValue,
                         int maxValue,
                         std::function<void(int)> onCommit) {
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");
        auto label = std::make_unique<Label>();
        label->SetText(labelText);
        label->AddClass("settings-row-label");
        Label* labelPtr = label.get();
        row->AddChild(std::move(label));
        auto field = std::make_unique<IntField>();
        field->AddClass("settings-row-value");
        IntField* fieldPtr = field.get();
        field->SetValue(initialValue);
        WireSettingsIntLabelDrag(
            m_EditorContext,
            labelPtr,
            fieldPtr,
            (std::string("Change Physics ") + labelText).c_str(),
            defaultValue,
            minValue,
            maxValue,
            [minValue, maxValue](int value) { return std::clamp(value, minValue, maxValue); },
            std::move(onCommit));
        row->AddChild(std::move(field));
        m_ContentBody->AddContent(std::move(row));
    };

    // Gravity section
    {
        auto header = std::make_unique<Label>();
        header->SetText("Gravity");
        header->AddClass("settings-section-header");
        m_ContentBody->AddContent(std::move(header));
    }

    auto currentSettings = std::make_shared<Physics::PhysicsWorldSettings>(s);
    const Physics::PhysicsWorldSettings defaults{};

    addFloatRow("X", s.gravity.x, defaults.gravity.x, -1000.0f, 1000.0f, [currentSettings, save](float v) {
        currentSettings->gravity.x = v;
        save(*currentSettings);
    });
    addFloatRow("Y", s.gravity.y, defaults.gravity.y, -1000.0f, 1000.0f, [currentSettings, save](float v) {
        currentSettings->gravity.y = v;
        save(*currentSettings);
    });
    addFloatRow("Z", s.gravity.z, defaults.gravity.z, -1000.0f, 1000.0f, [currentSettings, save](float v) {
        currentSettings->gravity.z = v;
        save(*currentSettings);
    });

    // Simulation section
    {
        auto header = std::make_unique<Label>();
        header->SetText("Simulation");
        header->AddClass("settings-section-header");
        m_ContentBody->AddContent(std::move(header));
    }

    addFloatRow("Fixed Time Step", s.fixedTimeStep, defaults.fixedTimeStep, 0.0001f, 1.0f, [currentSettings, save](float v) {
        currentSettings->fixedTimeStep = std::max(0.0001f, v);
        save(*currentSettings);
    });
    addIntRow("Max Sub Steps", s.maxSubSteps, defaults.maxSubSteps, 1, 128, [currentSettings, save](int v) {
        currentSettings->maxSubSteps = std::max(1, v);
        save(*currentSettings);
    });
    addIntRow("Collision Steps", s.collisionSteps, defaults.collisionSteps, 1, 128, [currentSettings, save](int v) {
        currentSettings->collisionSteps = std::max(1, v);
        save(*currentSettings);
    });
}

void SettingsPanel::CreateAudioSettingsContent()
{
    if (!m_ContentBody)
        return;

    SettingsToggleConfig config;
    config.labelText = "Auto-play On Select";
    config.tooltip = "Auto-play audio when file is selected";
    config.defaultValue = true;
    config.prefKey = "audio.autoPlayOnSelection";
    config.onValueChanged = nullptr;
    CreateSettingsToggleRow(config, m_ContentBody);
}

void SettingsPanel::CreateSceneSettingsContent()
{
    if (!m_ContentBody)
        return;

    {
        auto sectionHeader = std::make_unique<Label>();
        sectionHeader->SetText("Auto-save");
        sectionHeader->AddClass("settings-section-header");
        m_ContentBody->AddContent(std::move(sectionHeader));
    }

    {
        SettingsToggleConfig config;
        config.labelText = "Auto-save Backup Copy";
        config.tooltip = "Enable auto-save (backup copy)";
        config.defaultValue = true;
        config.prefKey = "editor.autoSaveEnabled";
        config.onValueChanged = nullptr;
        CreateSettingsToggleRow(config, m_ContentBody);
    }

    {
        float intervalSec = 120.0f;
        {
            Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
            std::string err;
            (void)prefs.Load(&err);
            double stored = 120.0;
            if (prefs.TryGetDouble("editor.autoSaveIntervalSeconds", stored))
                intervalSec = static_cast<float>(std::clamp(stored, 30.0, 300.0));
        }
        SettingsSliderConfig config;
        config.labelText = "Interval (seconds)";
        config.defaultValue = intervalSec;
        config.minValue = 30.0f;
        config.maxValue = 300.0f;
        config.step = 1.0f;
        config.prefKey = "editor.autoSaveIntervalSeconds";
        config.onValueChanged = nullptr;
        CreateSettingsSliderRow(config, m_ContentBody);
    }

    {
        auto description = std::make_unique<Label>();
        m_ContentBody->AddContent(MakeSettingsInfoCard(
            "While editing, saves a backup copy next to your scene: <name>.backup.scene "
            "(minimum 30s, maximum 5 minutes). Requires the scene to be saved to disk at least "
            "once. Disabled during Play Mode."));
    }
}

void SettingsPanel::CreateCameraSettingsContent()
{
    if (!m_ContentBody)
        return;

    using GameEngine::Editor::SceneViewSettings;

    SceneViewSettings& settings = SceneViewSettings::Get();

    auto createCameraSliderRow = [this](const SettingsSliderConfig& config, const char* idPrefix)
    {
        SettingsSliderRow row = CreateSettingsSliderRow(config, m_ContentBody);
        if (row.slider)
            row.slider->SetId(std::string(idPrefix) + "Slider");
        if (row.valueField)
            row.valueField->SetId(std::string(idPrefix) + "Field");
        return row;
    };

    // Scene View Camera section header
    {
        auto sectionHeader = std::make_unique<Label>();
        sectionHeader->SetText("Scene View Camera");
        sectionHeader->AddClass("settings-section-header");
        m_ContentBody->AddContent(std::move(sectionHeader));
    }

    {
        auto sectionHeader = std::make_unique<Label>();
        sectionHeader->SetText("Quality");
        sectionHeader->AddClass("settings-section-header");
        m_ContentBody->AddContent(std::move(sectionHeader));
    }

    {
        const auto& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
        const Rendering::AntiAliasingProjectSettings aaSettings =
            Editor::LoadProjectAntiAliasingSettings(workspaceRoot);
        const std::string aaModeValue = AAModeDropdownValue(aaSettings);

        // MSAA sample-count row + description, placed directly under the
        // Anti-Aliasing row and shown only while the mode is MSAA. Built
        // before the mode dropdown so its handler can capture the elements
        // it toggles.
        auto msaaRow = std::make_unique<UIElement>();
        msaaRow->AddClass("settings-row");

        auto msaaLabel = std::make_unique<Label>();
        msaaLabel->SetText("MSAA Samples");
        msaaLabel->AddClass("settings-row-label");
        msaaRow->AddChild(std::move(msaaLabel));

        // Explicit counts only, and only counts that actually multisample: the
        // Anti-Aliasing mode above owns on/off, so no Off entry, and every entry
        // is a number the project stores verbatim. Counts above the device cap
        // are not offered (Apple GPUs cap at 4; an 8x target there fails
        // creation). The Camera inspector keeps the full list — a game camera's
        // setting may target other devices.
        uint32_t deviceMaxSamples = 8u;
        if (auto* renderServices = EngineCore::GetInstance().GetRenderServices())
        {
            if (auto* device = renderServices->GetDevice())
                deviceMaxSamples = std::max(1u, device->GetCapabilities().maxMSAASamples);
        }

        std::vector<Dropdown::Option> msaaOptions;
        for (uint32_t samples = 2u; samples <= deviceMaxSamples; samples *= 2u)
            msaaOptions.push_back(Dropdown::Option{std::to_string(samples),
                                                   std::to_string(samples) + "x"});

        // Displayed count: the stored samples clamped to this device; a legacy
        // "off"/1 stored value shows as 2x, the smallest count that
        // multisamples. Stored settings are only rewritten when the user picks.
        uint32_t displaySamples = std::max(2u, aaSettings.MsaaSamples);
        while (displaySamples > deviceMaxSamples && displaySamples > 2u)
            displaySamples /= 2u;

        auto msaaDd = std::make_unique<Dropdown>();
        msaaDd->AddClass("settings-row-field");
        msaaDd->SetOptions(msaaOptions, 0);
        msaaDd->SetSelectedValue(std::to_string(displaySamples));
        msaaDd->SetOnValueChanged([this](const std::string& value)
        {
            const uint32_t samples = static_cast<uint32_t>(std::atoi(value.c_str()));
            if (!EditProjectAASettings([samples](Rendering::AntiAliasingProjectSettings& s)
                                       { s.MsaaSamples = samples; }))
            {
                Logger::Log::Warning("SettingsPanel: cannot save Scene View MSAA setting -- no workspace root");
                return;
            }

            NotifyProjectRenderSettingsChanged();
            auto* renderServices = EngineCore::GetInstance().GetRenderServices();
            Logger::Log::Info("SettingsPanel: applied Scene View MSAA setting '{}' ({} samples)",
                              value,
                              renderServices ? renderServices->GetDefaultMSAASampleCount() : samples);
        });
        msaaRow->AddChild(std::move(msaaDd));

        auto msaaDescription = MakeSettingsInfoCard(
            "Controls Scene View multisampling. Game cameras configure MSAA per camera in the "
            "Camera component inspector.");

        UIElement* msaaRowPtr = msaaRow.get();
        UIElement* msaaDescriptionPtr = msaaDescription.get();
        if (aaModeValue != "msaa")
        {
            msaaRow->AddClass("hidden");
            msaaDescription->AddClass("hidden");
        }

        // FXAA quality row, gated the same way: visible in either FXAA mode.
        auto fxaaRow = std::make_unique<UIElement>();
        fxaaRow->AddClass("settings-row");

        auto fxaaLabel = std::make_unique<Label>();
        fxaaLabel->SetText("FXAA Quality");
        fxaaLabel->AddClass("settings-row-label");
        fxaaRow->AddChild(std::move(fxaaLabel));

        std::vector<Dropdown::Option> fxaaOptions = {
            Dropdown::Option{"quality", "Quality"},
            Dropdown::Option{"fast", "Fast"},
        };

        auto fxaaDd = std::make_unique<Dropdown>();
        fxaaDd->AddClass("settings-row-field");
        fxaaDd->SetOptions(fxaaOptions, 0);
        fxaaDd->SetSelectedValue(
            aaSettings.FxaaQualityValue == Engine::Renderer::FxaaQuality::Fast ? "fast"
                                                                              : "quality");
        fxaaDd->SetOnValueChanged([this](const std::string& value)
        {
            const auto quality = value == "fast" ? Engine::Renderer::FxaaQuality::Fast
                                                 : Engine::Renderer::FxaaQuality::Quality;
            if (!EditProjectAASettings([quality](Rendering::AntiAliasingProjectSettings& s)
                                       { s.FxaaQualityValue = quality; }))
            {
                Logger::Log::Warning(
                    "SettingsPanel: cannot save FXAA quality setting -- no workspace root");
                return;
            }

            NotifyProjectRenderSettingsChanged();
            Logger::Log::Info("SettingsPanel: applied FXAA quality setting '{}'", value);
        });
        fxaaRow->AddChild(std::move(fxaaDd));

        auto fxaaDescription = MakeSettingsInfoCard(
            "Quality runs the FXAA 3.11 edge search (better long edges); Fast is the 5-tap "
            "filter. In the two-frame mode it feeds the temporal resolve.");

        UIElement* fxaaRowPtr = fxaaRow.get();
        UIElement* fxaaDescriptionPtr = fxaaDescription.get();
        if (aaModeValue != "fxaa")
        {
            fxaaRow->AddClass("hidden");
            fxaaDescription->AddClass("hidden");
        }

        // FXAA frames row: single-frame (engine mode FXAA) or two-frame
        // (engine mode TemporalFXAA). Same gate as the quality row.
        auto fxaaFramesRow = std::make_unique<UIElement>();
        fxaaFramesRow->AddClass("settings-row");

        auto fxaaFramesLabel = std::make_unique<Label>();
        fxaaFramesLabel->SetText("FXAA Frames");
        fxaaFramesLabel->AddClass("settings-row-label");
        fxaaFramesRow->AddChild(std::move(fxaaFramesLabel));

        std::vector<Dropdown::Option> fxaaFramesOptions = {
            Dropdown::Option{"single", "Single-frame"},
            Dropdown::Option{"two", "Two-frame (temporal)"},
        };

        auto fxaaFramesDd = std::make_unique<Dropdown>();
        fxaaFramesDd->AddClass("settings-row-field");
        fxaaFramesDd->SetOptions(fxaaFramesOptions, 0);
        fxaaFramesDd->SetSelectedValue(
            aaSettings.AAMode == Engine::Renderer::AntiAliasingMode::TemporalFXAA ? "two"
                                                                                  : "single");
        fxaaFramesDd->SetOnValueChanged([this](const std::string& value)
        {
            const auto mode = value == "two" ? Engine::Renderer::AntiAliasingMode::TemporalFXAA
                                             : Engine::Renderer::AntiAliasingMode::FXAA;
            if (!EditProjectAASettings([mode](Rendering::AntiAliasingProjectSettings& s)
                                       {
                                           s.ModeChosen = true;
                                           s.AAMode = mode;
                                       }))
            {
                Logger::Log::Warning(
                    "SettingsPanel: cannot save FXAA frames setting -- no workspace root");
                return;
            }

            NotifyProjectRenderSettingsChanged();
            Logger::Log::Info("SettingsPanel: applied FXAA frames setting '{}'", value);
        });
        fxaaFramesRow->AddChild(std::move(fxaaFramesDd));

        auto fxaaFramesDescription = MakeSettingsInfoCard(
            "Single-frame is classic FXAA: one frame in, one frame out, bit-stable when nothing "
            "moves. Two-frame jitters the raster between two sub-pixel positions and blends "
            "consecutive frames 50/50, reconstructing sub-pixel detail at a small cost in "
            "motion.");

        UIElement* fxaaFramesRowPtr = fxaaFramesRow.get();
        UIElement* fxaaFramesDescriptionPtr = fxaaFramesDescription.get();
        if (aaModeValue != "fxaa")
        {
            fxaaFramesRow->AddClass("hidden");
            fxaaFramesDescription->AddClass("hidden");
        }

        // SSAA scale row, gated the same way: visible only in SSAA mode. SSAA
        // is the render-scale axis worn as a mode — entering it drives the
        // Fixed render scale above 1.0 (the project setting for players, the
        // SceneViewSettings per-view scale for editor views); leaving returns
        // both to native.
        auto ssaaRow = std::make_unique<UIElement>();
        ssaaRow->AddClass("settings-row");

        auto ssaaLabel = std::make_unique<Label>();
        ssaaLabel->SetText("SSAA Scale");
        ssaaLabel->AddClass("settings-row-label");
        ssaaRow->AddChild(std::move(ssaaLabel));

        std::vector<Dropdown::Option> ssaaOptions = {
            Dropdown::Option{"1.25", "1.25x"},
            Dropdown::Option{"1.5", "1.5x"},
            Dropdown::Option{"2", "2x"},
        };

        const char* ssaaSelected = aaSettings.RenderScale >= 1.99f   ? "2"
                                   : aaSettings.RenderScale >= 1.49f ? "1.5"
                                                                     : "1.25";

        auto ssaaDd = std::make_unique<Dropdown>();
        ssaaDd->AddClass("settings-row-field");
        ssaaDd->SetOptions(ssaaOptions, 1);
        if (aaSettings.RenderScale > kSupersampleScaleThreshold)
            ssaaDd->SetSelectedValue(ssaaSelected);
        ssaaDd->SetOnValueChanged([this](const std::string& value)
        {
            const float scale = std::clamp(static_cast<float>(std::atof(value.c_str())),
                                           1.25f, 2.0f);
            if (!EditProjectAASettings(
                    [scale](Rendering::AntiAliasingProjectSettings& s)
                    {
                        s.RenderScale = scale;
                        s.ScaleMode = Engine::Renderer::DynamicResolutionMode::Fixed;
                    }))
            {
                Logger::Log::Warning(
                    "SettingsPanel: cannot save SSAA scale -- no workspace root");
                return;
            }
            NotifyProjectRenderSettingsChanged();
            Logger::Log::Info("SettingsPanel: applied SSAA scale {}", scale);
        });
        ssaaRow->AddChild(std::move(ssaaDd));

        auto ssaaDescription = MakeSettingsInfoCard(
            "Supersamples at this scale and filters back down. Quadratic cost: 2x is 4x the "
            "work.");

        UIElement* ssaaRowPtr = ssaaRow.get();
        UIElement* ssaaDescriptionPtr = ssaaDescription.get();
        if (aaModeValue != "ssaa")
        {
            ssaaRow->AddClass("hidden");
            ssaaDescription->AddClass("hidden");
        }

        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Anti-Aliasing");
        label->AddClass("settings-row-label");
        row->AddChild(std::move(label));

        std::vector<Dropdown::Option> options = {
            Dropdown::Option{"off", "Off"},
            Dropdown::Option{"msaa", "MSAA"},
            Dropdown::Option{"taa", "TAA"},
            Dropdown::Option{"fxaa", "FXAA"},
            Dropdown::Option{"smaa", "SMAA"},
            Dropdown::Option{"ssaa", "SSAA"},
        };

        auto dd = std::make_unique<Dropdown>();
        dd->AddClass("settings-row-field");
        dd->SetOptions(options, 0);
        dd->SetSelectedValue(aaModeValue);
        dd->SetOnValueChanged([this, msaaRowPtr, msaaDescriptionPtr, fxaaRowPtr,
                               fxaaDescriptionPtr, fxaaFramesRowPtr, fxaaFramesDescriptionPtr,
                               ssaaRowPtr, ssaaDescriptionPtr](const std::string& value)
        {
            // SSAA is the render-scale axis worn as a mode: entering drives
            // the Fixed render scale above 1.0, leaving it returns to native.
            // Editor viewports follow the engine default (their per-view
            // override is nullopt at the scene-view slider's default 1.0), so
            // the project scale is the single lever. Transitions not involving
            // SSAA leave the scale alone — a deliberate TAA + supersample
            // combination set on the Render Scale slider is not this
            // dropdown's to undo.
            if (!EditProjectAASettings(
                    [&value](Rendering::AntiAliasingProjectSettings& s)
                    {
                        using Engine::Renderer::AntiAliasingMode;
                        using Engine::Renderer::DynamicResolutionMode;
                        const bool wasSsaa = s.AAMode == AntiAliasingMode::Off &&
                                             s.RenderScale > kSupersampleScaleThreshold;
                        s.ModeChosen = true;
                        if (value == "ssaa")
                        {
                            s.AAMode = AntiAliasingMode::Off;
                            // Snap to a real SSAA step: a leftover near-native
                            // slider value (e.g. 1.03) is not a supersample choice.
                            if (s.RenderScale < 1.24f)
                                s.RenderScale = 1.5f;
                            s.ScaleMode = DynamicResolutionMode::Fixed;
                            return;
                        }
                        // "fxaa" covers both engine FXAA modes; keep a stored
                        // two-frame choice rather than silently dropping it.
                        const bool keepTemporal =
                            value == "fxaa" && s.AAMode == AntiAliasingMode::TemporalFXAA;
                        if (!keepTemporal)
                            s.AAMode = Rendering::ParseAAModeToken(value);
                        if (wasSsaa)
                        {
                            s.RenderScale = 1.0f;
                            s.ScaleMode = DynamicResolutionMode::Off;
                        }
                    }))
            {
                Logger::Log::Warning(
                    "SettingsPanel: cannot save Anti-Aliasing setting -- no workspace root");
                return;
            }

            const auto setHidden = [](UIElement* rowEl, UIElement* descEl, bool hidden)
            {
                if (hidden)
                {
                    rowEl->AddClass("hidden");
                    descEl->AddClass("hidden");
                }
                else
                {
                    rowEl->RemoveClass("hidden");
                    descEl->RemoveClass("hidden");
                }
            };
            setHidden(msaaRowPtr, msaaDescriptionPtr, value != "msaa");
            setHidden(fxaaRowPtr, fxaaDescriptionPtr, value != "fxaa");
            setHidden(fxaaFramesRowPtr, fxaaFramesDescriptionPtr, value != "fxaa");
            setHidden(ssaaRowPtr, ssaaDescriptionPtr, value != "ssaa");

            NotifyProjectRenderSettingsChanged();
            Logger::Log::Info("SettingsPanel: applied Anti-Aliasing setting '{}'", value);
        });

        row->AddChild(std::move(dd));
        m_ContentBody->AddContent(std::move(row));
        m_ContentBody->AddContent(std::move(msaaRow));
        m_ContentBody->AddContent(std::move(fxaaRow));
        m_ContentBody->AddContent(std::move(fxaaFramesRow));
        m_ContentBody->AddContent(std::move(ssaaRow));

        m_ContentBody->AddContent(MakeSettingsInfoCard(
            "MSAA multisamples geometry edges using the sample count above. "
            "TAA accumulates jittered frames over time (sharp stills, stable edges in motion). "
            "FXAA smooths edges in post (cheapest, softens detail); the FXAA Frames row picks "
            "single-frame or two-frame temporal. "
            "SMAA reconstructs edge shapes in post without jitter (temporally stable). "
            "SSAA supersamples at the scale above (best quality, quadratic cost). "
            "The modes are mutually exclusive. A new project starts on the best mode its "
            "machine can run -- 4x MSAA, else 2x, else TAA -- and keeps whatever is chosen "
            "here from then on."));
        m_ContentBody->AddContent(std::move(msaaDescription));
        m_ContentBody->AddContent(std::move(fxaaDescription));
        m_ContentBody->AddContent(std::move(fxaaFramesDescription));
        m_ContentBody->AddContent(std::move(ssaaDescription));
    }

    // Render-scale mode: Native / Fixed / Dynamic. The slider below stays
    // meaningful and prominent — it is the Fixed mode's value.
    {
        const auto& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
        // ReadFrom already migrated a project saved before the mode key existed.
        const auto startupMode = Editor::LoadProjectAntiAliasingSettings(workspaceRoot).ScaleMode;

        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Render Scale Mode");
        label->AddClass("settings-row-label");
        row->AddChild(std::move(label));

        std::vector<Dropdown::Option> options = {
            Dropdown::Option{"off", "Native (1.0)"},
            Dropdown::Option{"fixed", "Fixed scale"},
            Dropdown::Option{"dynamic", "Dynamic (hold target FPS)"},
        };

        auto dd = std::make_unique<Dropdown>();
        dd->AddClass("settings-row-field");
        dd->SetOptions(options, 0);
        dd->SetSelectedValue(Engine::Renderer::ToString(startupMode));
        dd->SetOnValueChanged([this](const std::string& value)
        {
            const auto mode = Engine::Renderer::ResolveStartupDynamicResolutionMode(value, 1.0f);
            if (!EditProjectAASettings([mode](Rendering::AntiAliasingProjectSettings& s)
                                       { s.ScaleMode = mode; }))
            {
                Logger::Log::Warning(
                    "SettingsPanel: cannot save render scale mode -- no workspace root");
                return;
            }
            // The mode was persisted first, so the total re-apply lands every
            // window on the same file-backed scale/mode pair in one pass.
            NotifyProjectRenderSettingsChanged();
            Logger::Log::Info("SettingsPanel: applied render scale mode '{}'",
                              Engine::Renderer::ToString(mode));
        });

        row->AddChild(std::move(dd));
        m_ContentBody->AddContent(std::move(row));
    }

    // Project render scale, directly under its mode: internal-resolution
    // rendering. The crossing back to display resolution is the temporal
    // resolve under TAA and the RenderScaleUpscale node otherwise.
    {
        const auto& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();

        SettingsSliderConfig config;
        config.labelText = "Render Scale";
        config.defaultValue = Editor::LoadProjectAntiAliasingSettings(workspaceRoot).RenderScale;
        config.minValue = 0.5f;
        config.maxValue = 2.0f;
        config.step = 0.01f;
        config.prefKey.clear(); // Persisted via project settings (rendering.taaRenderScale).

        // Live preview while dragging: apply straight to the renderer (the
        // pipeline pre-pass re-derives the internal extent next frame).
        // Release persists and fans out to every window below.
        config.onValueChanging = [](float value)
        {
            if (auto* renderServices = EngineCore::GetInstance().GetRenderServices())
                renderServices->SetDefaultRenderScale(value);
        };
        config.onValueChanged = [this](float value)
        {
            if (!EditProjectAASettings(
                    [value](Rendering::AntiAliasingProjectSettings& s)
                    {
                        s.RenderScale = value;
                        // A dragged non-native scale should take effect: Native
                        // pins the scale to 1.0 on apply, so move an explicit
                        // Native mode to Fixed. Dynamic keeps its mode — the
                        // slider is its starting point.
                        if (std::abs(value - 1.0f) > 0.005f &&
                            s.ScaleMode == Engine::Renderer::DynamicResolutionMode::Off)
                            s.ScaleMode = Engine::Renderer::DynamicResolutionMode::Fixed;
                    }))
            {
                Logger::Log::Warning(
                    "SettingsPanel: cannot save render scale -- no workspace root");
                return;
            }
            NotifyProjectRenderSettingsChanged();
        };
        CreateSettingsSliderRow(config, m_ContentBody);
    }

    {
        m_ContentBody->AddContent(MakeSettingsInfoCard(
            "The Fixed mode's value, and the starting point for Dynamic. Below 1.0 the world "
            "renders smaller and is upscaled back -- faster, softer. 1.0 is native. Above 1.0 it "
            "is supersampled (SSAA): the best anti-aliasing available, at quadratic cost (2.0 is "
            "4x the fill and shading work). The crossing back to display resolution is the "
            "temporal resolve under TAA and a spatial filter otherwise. Later post-processing, "
            "the UI, and gizmos stay at full resolution, and texture sharpness compensation "
            "(mip bias) applies live."));
    }

    {
        m_ContentBody->AddContent(MakeSettingsInfoCard(
            "Native renders the world at full viewport resolution. Fixed renders it at the scale "
            "above and resamples back. Dynamic moves that scale automatically to hold the target "
            "below, and gives the resolution back when it measures that scaling is not buying "
            "frames (a geometry-bound view, for instance). Both work with Anti-Aliasing off, "
            "TAA, FXAA or SMAA; MSAA rules them out, because a multisampled view cannot be "
            "scaled."));
    }

    // Dynamic-mode target.
    {
        const auto& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();

        auto applyTargetFps = [](float value)
        {
            if (auto* renderServices = EngineCore::GetInstance().GetRenderServices())
            {
                auto drsConfig = renderServices->GetDynamicResolutionConfig();
                drsConfig.TargetGpuMs = 1000.0f / std::max(value, 1.0f);
                renderServices->SetDynamicResolutionConfig(drsConfig);
            }
        };

        SettingsSliderConfig config;
        config.labelText = "Dynamic Target FPS";
        config.defaultValue = Editor::LoadProjectAntiAliasingSettings(workspaceRoot).DrsTargetFps;
        config.minValue = Editor::kMinDrsTargetFps;
        config.maxValue = Editor::kMaxDrsTargetFps;
        config.step = 1.0f;
        config.prefKey.clear(); // Persisted via project settings (rendering.drsTargetFps).
        // Seeding applies to the main RenderServices only, and deliberately does
        // not fan out: opening the page is not an edit, and the total re-apply
        // would restart a running Dynamic controller from the file's scale.
        config.onSeed = applyTargetFps;
        config.onValueChanged = [this](float value)
        {
            if (!EditProjectAASettings([value](Rendering::AntiAliasingProjectSettings& s)
                                       { s.DrsTargetFps = value; }))
            {
                Logger::Log::Warning(
                    "SettingsPanel: cannot save dynamic target FPS -- no workspace root");
                return;
            }
            // Persisted first, so the total re-apply carries the new target to
            // every window in one pass. applyTargetFps is seed-only: opening
            // the page does not restart a running Dynamic controller.
            NotifyProjectRenderSettingsChanged();
        };
        CreateSettingsSliderRow(config, m_ContentBody);
    }

    {
        m_ContentBody->AddContent(MakeSettingsInfoCard(
            "Dynamic mode only. The target is GPU time, so hitting it does not by itself guarantee "
            "this frame rate — CPU cost and present pacing sit outside what render scale can "
            "control."));
    }


    // Field of view
    {
        SettingsSliderConfig config;
        config.labelText = "Field of View";
        config.defaultValue = SceneViewSettings::kDefaultFieldOfViewDeg;
        config.initialValue = settings.GetFieldOfViewDeg();
        config.minValue = SceneViewSettings::kMinFieldOfViewDeg;
        config.maxValue = SceneViewSettings::kMaxFieldOfViewDeg;
        config.step = 1.0f;
        config.prefKey.clear(); // Use SceneViewSettings for persistence
        config.onValueChanging = [](float value) {
            SceneViewSettings::Get().SetFieldOfViewDeg(value, false);
        };
        config.onValueChanged = [](float value) {
            SceneViewSettings::Get().SetFieldOfViewDeg(value);
        };

        createCameraSliderRow(config, "SettingsSceneCameraFov");
    }

    // Near clip plane
    {
        SettingsSliderConfig config;
        config.labelText = "Near Clip Plane";
        config.defaultValue = SceneViewSettings::kDefaultNearClip;
        config.initialValue = settings.GetNearClip();
        config.minValue = SceneViewSettings::kMinNearClip;
        config.maxValue = SceneViewSettings::kMaxNearClip;
        config.step = 0.01f;
        config.prefKey.clear(); // Use SceneViewSettings for persistence
        config.onValueChanging = [](float value) {
            SceneViewSettings::Get().SetNearClip(value, false);
        };
        config.onValueChanged = [](float value) {
            SceneViewSettings::Get().SetNearClip(value);
        };

        createCameraSliderRow(config, "SettingsSceneCameraNearClip");
    }

    // Far clip plane
    {
        SettingsSliderConfig config;
        config.labelText = "Far Clip Plane";
        config.defaultValue = SceneViewSettings::kDefaultFarClip;
        config.initialValue = settings.GetFarClip();
        config.minValue = SceneViewSettings::kMinFarClip;
        config.maxValue = SceneViewSettings::kMaxFarClip;
        config.step = 10.0f;
        config.prefKey.clear(); // Use SceneViewSettings for persistence
        config.onValueChanging = [](float value) {
            SceneViewSettings::Get().SetFarClip(value, false);
        };
        config.onValueChanged = [](float value) {
            SceneViewSettings::Get().SetFarClip(value);
        };

        createCameraSliderRow(config, "SettingsSceneCameraFarClip");
    }

    // Base move speed
    {
        SettingsSliderConfig config;
        config.labelText = "Move Speed";
        config.defaultValue = SceneViewSettings::kDefaultMoveSpeed;
        config.initialValue = settings.GetMoveSpeed();
        config.minValue = SceneViewSettings::kMinMoveSpeed;
        config.maxValue = SceneViewSettings::kMaxMoveSpeed;
        config.step = 0.1f;
        config.prefKey.clear(); // Use SceneViewSettings for persistence
        config.onValueChanging = [](float value) {
            SceneViewSettings::Get().SetMoveSpeed(value, false);
        };
        config.onValueChanged = [](float value) {
            SceneViewSettings::Get().SetMoveSpeed(value);
        };

        createCameraSliderRow(config, "SettingsSceneCameraMoveSpeed");
    }

    // Fast move multiplier (Shift)
    {
        SettingsSliderConfig config;
        config.labelText = "Fast Move Multiplier";
        config.defaultValue = SceneViewSettings::kDefaultFastMoveMultiplier;
        config.initialValue = settings.GetFastMoveMultiplier();
        config.minValue = SceneViewSettings::kMinFastMoveMultiplier;
        config.maxValue = SceneViewSettings::kMaxFastMoveMultiplier;
        config.step = 0.5f;
        config.prefKey.clear(); // Use SceneViewSettings for persistence
        config.onValueChanging = [](float value) {
            SceneViewSettings::Get().SetFastMoveMultiplier(value, false);
        };
        config.onValueChanged = [](float value) {
            SceneViewSettings::Get().SetFastMoveMultiplier(value);
        };

        createCameraSliderRow(config, "SettingsSceneCameraFastMultiplier");
    }

    // Fly-camera acceleration (seconds to reach target speed; 0 = instant)
    {
        SettingsSliderConfig config;
        config.labelText = "Move Acceleration";
        config.defaultValue = SceneViewSettings::kDefaultMoveAccelerationTime;
        config.initialValue = settings.GetMoveAccelerationTime();
        config.minValue = 0.0f;
        config.maxValue = SceneViewSettings::kMaxMoveAccelerationTime;
        config.step = 0.01f;
        config.prefKey.clear(); // Use SceneViewSettings for persistence
        config.onValueChanging = [](float value) {
            SceneViewSettings::Get().SetMoveAccelerationTime(value, false);
        };
        config.onValueChanged = [](float value) {
            SceneViewSettings::Get().SetMoveAccelerationTime(value);
        };

        createCameraSliderRow(config, "SettingsSceneCameraAcceleration");
    }

    // Scene View pinned exposure (used while its Auto Exposure toggle is off)
    {
        SettingsSliderConfig config;
        config.labelText = "Fixed Exposure (EV100)";
        config.defaultValue = Components::kDefaultManualExposureEv;
        config.initialValue = settings.GetPostFxFixedExposureEv();
        config.minValue = SceneViewSettings::kMinExposureEv;
        config.maxValue = SceneViewSettings::kMaxExposureEv;
        config.step = 0.1f;
        config.prefKey.clear(); // Use SceneViewSettings for persistence
        config.onValueChanging = [](float value) {
            SceneViewSettings::Get().SetPostFxFixedExposureEv(value, false);
        };
        config.onValueChanged = [](float value) {
            SceneViewSettings::Get().SetPostFxFixedExposureEv(value);
        };

        createCameraSliderRow(config, "SettingsSceneCameraFixedEv");
    }

    {
        m_ContentBody->AddContent(MakeSettingsInfoCard(
            "EV100 the Scene View is pinned to while its Auto Exposure toggle (viewport "
            "post-fx menu or camera popup) is off."));
    }

    // Scene View auto-metering tune (applies while Auto Exposure is on)
    {
        SettingsSliderConfig config;
        config.labelText = "Auto Exposure Min EV100";
        config.defaultValue = SceneViewSettings::kDefaultAutoExposureMinEv;
        config.initialValue = settings.GetPostFxAutoExposureMinEv();
        config.minValue = SceneViewSettings::kMinExposureEv;
        config.maxValue = SceneViewSettings::kMaxExposureEv;
        config.step = 0.1f;
        config.prefKey.clear(); // Use SceneViewSettings for persistence
        config.onValueChanging = [](float value) {
            SceneViewSettings::Get().SetPostFxAutoExposureMinEv(value, false);
        };
        config.onValueChanged = [](float value) {
            SceneViewSettings::Get().SetPostFxAutoExposureMinEv(value);
        };

        createCameraSliderRow(config, "SettingsSceneCameraAutoMinEv");
    }

    {
        SettingsSliderConfig config;
        config.labelText = "Auto Exposure Max EV100";
        config.defaultValue = SceneViewSettings::kDefaultAutoExposureMaxEv;
        config.initialValue = settings.GetPostFxAutoExposureMaxEv();
        config.minValue = SceneViewSettings::kMinExposureEv;
        config.maxValue = SceneViewSettings::kMaxExposureEv;
        config.step = 0.1f;
        config.prefKey.clear(); // Use SceneViewSettings for persistence
        config.onValueChanging = [](float value) {
            SceneViewSettings::Get().SetPostFxAutoExposureMaxEv(value, false);
        };
        config.onValueChanged = [](float value) {
            SceneViewSettings::Get().SetPostFxAutoExposureMaxEv(value);
        };

        createCameraSliderRow(config, "SettingsSceneCameraAutoMaxEv");
    }

    {
        SettingsSliderConfig config;
        config.labelText = "Exposure Compensation";
        config.defaultValue = 0.0f;
        config.initialValue = settings.GetPostFxExposureCompensation();
        config.minValue = -SceneViewSettings::kMaxExposureCompensation;
        config.maxValue = SceneViewSettings::kMaxExposureCompensation;
        config.step = 0.1f;
        config.prefKey.clear(); // Use SceneViewSettings for persistence
        config.onValueChanging = [](float value) {
            SceneViewSettings::Get().SetPostFxExposureCompensation(value, false);
        };
        config.onValueChanged = [](float value) {
            SceneViewSettings::Get().SetPostFxExposureCompensation(value);
        };

        createCameraSliderRow(config, "SettingsSceneCameraExposureComp");
    }

    // Max orbit distance
    {
        SettingsSliderConfig config;
        config.labelText = "Max Orbit Distance";
        config.defaultValue = settings.GetMaxDistance();
        config.minValue = 10.0f;
        config.maxValue = 100000.0f;
        config.step = 10.0f;
        config.prefKey.clear(); // Use SceneViewSettings for persistence
        config.onValueChanging = [](float value) {
            SceneViewSettings::Get().SetMaxDistance(value, false);
        };
        config.onValueChanged = [](float value) {
            SceneViewSettings::Get().SetMaxDistance(value);
        };

        CreateSettingsSliderRow(config, m_ContentBody);
    }

    // Background color (used when no SkyEnvironment renders the background).
    {
        m_SceneViewBackgroundColor = settings.GetBackgroundColor();

        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Background Color");
        label->AddClass("settings-row-label");
        Label* labelPtr = label.get();
        section->AddChild(std::move(label));

        auto applyColor = [this](uint32_t argb) {
            m_SceneViewBackgroundColor = argb;
            SceneViewSettings::Get().SetBackgroundColor(argb);
            UpdateSceneViewBackgroundColorSwatchStyle();
        };

        auto openColorPicker = [this, applyColor](UIEvent& e) {
            if (e.Button != 0)
                return;
            const uint32_t original = m_SceneViewBackgroundColor;
            OpenColorPicker(
                original,
                applyColor,
                [applyColor, original]() { applyColor(original); },
                applyColor);
            e.Stop();
        };

        auto swatch = std::make_unique<UIElement>();
        swatch->SetId("settings-scene-view-background-color-swatch");
        m_SceneViewBackgroundColorSwatch = swatch.get();
        UpdateSceneViewBackgroundColorSwatchStyle();
        swatch->RegisterEventHandler(kEventMouseDown, openColorPicker);
        section->AddChild(std::move(swatch));

        constexpr uint32_t kDefaultBackgroundColor = 0xFF1A1A1Au;
        AddDoubleClickReset(labelPtr, [this, kDefaultBackgroundColor]() {
            m_SceneViewBackgroundColor = kDefaultBackgroundColor;
            SceneViewSettings::Get().SetBackgroundColor(kDefaultBackgroundColor);
            UpdateSceneViewBackgroundColorSwatchStyle();
        });

        m_ContentBody->AddContent(std::move(section));
    }

    // Scroll wheel dolly (enable/disable)
    {
        SettingsToggleConfig config;
        config.labelText = "Scroll Wheel Dolly";
        config.defaultValue = settings.GetScrollWheelDollyEnabled();
        config.prefKey.clear();
        config.onValueChanged = [](bool value) {
            SceneViewSettings::Get().SetScrollWheelDollyEnabled(value);
        };
        CreateSettingsToggleRow(config, m_ContentBody);
    }

    // Scroll wheel direction (reverse)
    {
        SettingsToggleConfig config;
        config.labelText = "Reverse Scroll Wheel Dolly";
        config.defaultValue = settings.GetScrollWheelDollyReversed();
        config.prefKey.clear();
        config.onValueChanged = [](bool value) {
            SceneViewSettings::Get().SetScrollWheelDollyReversed(value);
        };
        CreateSettingsToggleRow(config, m_ContentBody);
    }

    // (Grid options moved to Settings → User Settings → Grid and Snapping.)

    // Scene View pick mode: exact entity under the cursor vs model-instance root.
    {
        SettingsToggleConfig config;
        config.labelText = "Exact Pick Mode (T)";
        config.defaultValue = settings.GetExactPickMode();

        config.prefKey.clear();
        config.onValueChanged = [](bool value) {
            SceneViewSettings::Get().SetExactPickMode(value);
        };
        CreateSettingsToggleRow(config, m_ContentBody);
    }

    // Show viewport rotation gizmo (top-right axis orientation widget)
    {
        SettingsToggleConfig config;
        config.labelText = "Show Rotation Gizmo";
        config.defaultValue = settings.GetShowRotationGizmo();
        config.prefKey.clear();
        config.onValueChanged = [](bool value) {
            SceneViewSettings::Get().SetShowRotationGizmo(value);
        };
        CreateSettingsToggleRow(config, m_ContentBody);
    }

    // Rotation gizmo corner placement (top-right / top-left / bottom-right / bottom-left)
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");
        auto label = std::make_unique<Label>();
        label->SetText("Rotation Gizmo Corner");
        label->AddClass("settings-row-label");
        row->AddChild(std::move(label));

        std::vector<Dropdown::Option> options = {
            {"0", "Top Right"},
            {"1", "Top Left"},
            {"2", "Bottom Right"},
            {"3", "Bottom Left"}
        };

        auto dd = std::make_unique<Dropdown>();
        dd->AddClass("settings-row-field");
        dd->SetOptions(options, 0);
        dd->SetSelectedValue(std::to_string(static_cast<int>(settings.GetRotationGizmoCorner())));
        dd->SetOnValueChanged([](const std::string& value) {
            int cornerInt = std::stoi(value);
            SceneViewSettings::Get().SetRotationGizmoCorner(
                static_cast<Editor::RotationGizmoCorner>(cornerInt));
        });
        row->AddChild(std::move(dd));
        m_ContentBody->AddContent(std::move(row));
    }

    // Borderless floating tool overlay
    {
        SettingsToggleConfig config;
        config.labelText = "Borderless Tool Overlay";
        config.defaultValue = settings.GetToolOverlayBorderless();
        config.prefKey.clear();
        config.onValueChanged = [](bool value) {
            SceneViewSettings::Get().SetToolOverlayBorderless(value);
        };
        CreateSettingsToggleRow(config, m_ContentBody);
    }

    // Game UI overlay: composite the world's UIDocument HUDs over the Scene View
    {
        SettingsToggleConfig config;
        config.labelText = "Show Game UI";
        config.defaultValue = settings.GetShowGameUI();
        config.prefKey.clear();
        config.onValueChanged = [](bool value) {
            SceneViewSettings::Get().SetShowGameUI(value);
        };
        CreateSettingsToggleRow(config, m_ContentBody);
    }

    // 2D ruler bands toggle
    {
        SettingsToggleConfig config;
        config.labelText = "Show Rulers (2D)";
        config.defaultValue = settings.GetShowRulers();
        config.prefKey.clear();
        config.onValueChanged = [](bool value) {
            SceneViewSettings::Get().SetShowRulers(value);
        };
        CreateSettingsToggleRow(config, m_ContentBody);
    }

    // Ruler opacity (alpha multiplier applied to every ruler primitive)
    {
        SettingsSliderConfig config;
        config.labelText = "Ruler Opacity";
        config.defaultValue = settings.GetRulerOpacity();
        config.minValue = 0.0f;
        config.maxValue = 1.0f;
        config.step = 0.05f;
        config.prefKey.clear();
        config.onValueChanged = [](float value) {
            SceneViewSettings::Get().SetRulerOpacity(value);
        };
        config.onValueChanging = [](float value) {
            // Live preview while dragging — Tick() in the overlay watches the
            // setting and re-emits primitives when it changes.
            SceneViewSettings::Get().SetRulerOpacity(value);
        };
        CreateSettingsSliderRow(config, m_ContentBody);
    }

    // Ruler indicator (cursor line) thickness
    {
        SettingsSliderConfig config;
        config.labelText = "Ruler Indicator Thickness";
        config.defaultValue = settings.GetRulerIndicatorThickness();
        config.minValue = 0.5f;
        config.maxValue = 6.0f;
        config.step = 0.25f;
        config.prefKey.clear();
        config.onValueChanged = [](float value) {
            SceneViewSettings::Get().SetRulerIndicatorThickness(value);
        };
        config.onValueChanging = [](float value) {
            SceneViewSettings::Get().SetRulerIndicatorThickness(value);
        };
        CreateSettingsSliderRow(config, m_ContentBody);
    }

    // Ruler indicator (cursor line) color — swatch opens hosted color picker.
    {
        m_RulerIndicatorColor = settings.GetRulerIndicatorColor();

        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Ruler Indicator Color");
        label->AddClass("settings-row-label");
        Label* labelPtr = label.get();
        section->AddChild(std::move(label));

        auto applyColor = [this](uint32_t argb) {
            m_RulerIndicatorColor = argb;
            SceneViewSettings::Get().SetRulerIndicatorColor(argb);
            UpdateRulerIndicatorColorSwatchStyle();
        };

        auto openColorPicker = [this, applyColor](UIEvent& e) {
            if (e.Button != 0)
                return;
            const uint32_t original = m_RulerIndicatorColor;
            OpenColorPicker(
                original,
                applyColor,
                [applyColor, original]() { applyColor(original); },
                applyColor);
            e.Stop();
        };

        auto swatch = std::make_unique<UIElement>();
        swatch->SetId("settings-scene-view-ruler-indicator-color-swatch");
        m_RulerIndicatorColorSwatch = swatch.get();
        UpdateRulerIndicatorColorSwatchStyle();
        swatch->RegisterEventHandler(kEventMouseDown, openColorPicker);
        section->AddChild(std::move(swatch));

        constexpr uint32_t kDefaultIndicatorColor = 0xF2F2664Du;
        AddDoubleClickReset(labelPtr, [this, kDefaultIndicatorColor]() {
            m_RulerIndicatorColor = kDefaultIndicatorColor;
            SceneViewSettings::Get().SetRulerIndicatorColor(kDefaultIndicatorColor);
            UpdateRulerIndicatorColorSwatchStyle();
        });

        m_ContentBody->AddContent(std::move(section));
    }

    // Measure color — used by quick drag measure and as the default for new measure entities.
    {
        m_MeasureColor = settings.GetMeasureColor();

        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Measure Color");
        label->AddClass("settings-row-label");
        Label* labelPtr = label.get();
        section->AddChild(std::move(label));

        auto applyColor = [this](uint32_t argb) {
            m_MeasureColor = argb;
            SceneViewSettings::Get().SetMeasureColor(argb);
            UpdateMeasureColorSwatchStyle();
        };

        auto openColorPicker = [this, applyColor](UIEvent& e) {
            if (e.Button != 0)
                return;
            const uint32_t original = m_MeasureColor;
            OpenColorPicker(
                original,
                applyColor,
                [applyColor, original]() { applyColor(original); },
                applyColor);
            e.Stop();
        };

        auto swatch = std::make_unique<UIElement>();
        swatch->SetId("settings-scene-view-measure-color-swatch");
        m_MeasureColorSwatch = swatch.get();
        UpdateMeasureColorSwatchStyle();
        swatch->RegisterEventHandler(kEventMouseDown, openColorPicker);
        section->AddChild(std::move(swatch));

        constexpr uint32_t kDefaultMeasureColor = 0xFFFFC738u;
        AddDoubleClickReset(labelPtr, [this, kDefaultMeasureColor]() {
            m_MeasureColor = kDefaultMeasureColor;
            SceneViewSettings::Get().SetMeasureColor(kDefaultMeasureColor);
            UpdateMeasureColorSwatchStyle();
        });

        m_ContentBody->AddContent(std::move(section));
    }

    // Hover highlight mode
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");
        auto label = std::make_unique<Label>();
        label->SetText("Hover Highlight");
        label->AddClass("settings-row-label");
        row->AddChild(std::move(label));

        std::vector<Dropdown::Option> options = {
            {"0", "Always"},
            {"1", "With Modifier Key"},
            {"2", "Never"}
        };

        auto dd = std::make_unique<Dropdown>();
        dd->AddClass("settings-row-field");
        dd->SetOptions(options, 0);
        dd->SetSelectedValue(std::to_string(static_cast<int>(settings.GetHoverHighlightMode())));
        dd->SetOnValueChanged([](const std::string& value) {
            int modeInt = std::stoi(value);
            SceneViewSettings::Get().SetHoverHighlightMode(static_cast<Editor::HoverHighlightMode>(modeInt));
        });
        row->AddChild(std::move(dd));
        m_ContentBody->AddContent(std::move(row));
    }

    // Hover highlight modifier (when "With Modifier Key" is selected)
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");
        auto label = std::make_unique<Label>();
        label->SetText("Hover Highlight Modifier");
        label->AddClass("settings-row-label");
        row->AddChild(std::move(label));

        std::vector<Dropdown::Option> modOptions = {
            {"0", "Control"},
            {"1", "Shift"},
            {"2", "` (Backtick)"}
        };

        auto dd = std::make_unique<Dropdown>();
        dd->AddClass("settings-row-field");
        dd->SetOptions(modOptions, 0);
        dd->SetSelectedValue(std::to_string(static_cast<int>(settings.GetHoverHighlightModifier())));
        dd->SetOnValueChanged([](const std::string& value) {
            const int modInt = std::stoi(value);
            SceneViewSettings::Get().SetHoverHighlightModifier(
                static_cast<Editor::HoverHighlightModifier>(modInt));
        });
        row->AddChild(std::move(dd));
        m_ContentBody->AddContent(std::move(row));
    }

    // Reveal-hidden hover modifier: hold to outline disabled objects on hover.
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");
        auto label = std::make_unique<Label>();
        label->SetText("Reveal Hidden Modifier");
        label->AddClass("settings-row-label");
        row->AddChild(std::move(label));

        std::vector<Dropdown::Option> modOptions = {
            {"0", "Control"},
            {"1", "Shift"},
            {"2", "` (Backtick)"}
        };

        auto dd = std::make_unique<Dropdown>();
        dd->AddClass("settings-row-field");
        dd->SetOptions(modOptions, 0);
        dd->SetSelectedValue(std::to_string(static_cast<int>(settings.GetRevealHiddenHoverModifier())));
        dd->SetOnValueChanged([](const std::string& value) {
            const int modInt = std::stoi(value);
            SceneViewSettings::Get().SetRevealHiddenHoverModifier(
                static_cast<Editor::HoverHighlightModifier>(modInt));
        });
        row->AddChild(std::move(dd));
        m_ContentBody->AddContent(std::move(row));
    }

    // Hover name pill (shows entity name while scene hover highlight is active).
    {
        SettingsToggleConfig config;
        config.labelText = "Show Hover Name Pill";
        config.defaultValue = settings.GetHoverNamePillEnabled();
        config.prefKey.clear();
        config.onValueChanged = [](bool value) {
            SceneViewSettings::Get().SetHoverNamePillEnabled(value);
        };
        CreateSettingsToggleRow(config, m_ContentBody);
    }
    {
        SettingsToggleConfig config;
        config.labelText = "Hover Name Pill";
        config.tooltip = "Hover Name Pill Near Cursor";
        config.defaultValue = settings.GetHoverNamePillNearCursor();
        config.prefKey.clear();
        config.onValueChanged = [](bool value) {
            SceneViewSettings::Get().SetHoverNamePillNearCursor(value);
        };
        CreateSettingsToggleRow(config, m_ContentBody);
    }

    // Selection highlight section header
    {
        auto sectionHeader = std::make_unique<Label>();
        sectionHeader->SetText("Selection Highlight");
        sectionHeader->AddClass("settings-section-header");
        m_ContentBody->AddContent(std::move(sectionHeader));
    }

    // Highlight Style: Box / Outline / Both
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");
        auto label = std::make_unique<Label>();
        label->SetText("Highlight Style");
        label->AddClass("settings-row-label");
        row->AddChild(std::move(label));

        std::vector<Dropdown::Option> options = {
            {"0", "Bounding Box"},
            {"1", "Silhouette Outline"},
            {"2", "Both"}
        };

        auto dd = std::make_unique<Dropdown>();
        dd->AddClass("settings-row-field");
        dd->SetOptions(options, 0);
        dd->SetSelectedValue(std::to_string(static_cast<int>(settings.GetSelectionHighlightStyle())));
        dd->SetOnValueChanged([](const std::string& value) {
            const int styleInt = std::stoi(value);
            SceneViewSettings::Get().SetSelectionHighlightStyle(
                static_cast<Editor::SelectionHighlightStyle>(styleInt));
        });
        row->AddChild(std::move(dd));
        m_ContentBody->AddContent(std::move(row));
    }

    // Bounding Box Color swatch
    {
        m_SelectionBoxColor = settings.GetSelectionBoxColor();

        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Box Color");
        label->AddClass("settings-row-label");
        Label* labelPtr = label.get();
        section->AddChild(std::move(label));

        auto openColorPicker = [this](UIEvent& e) {
            if (e.Button != 0)
                return;
            const uint32_t original = m_SelectionBoxColor;
            OpenColorPicker(
                original,
                [this](uint32_t argb) {
                    m_SelectionBoxColor = argb;
                    SceneViewSettings::Get().SetSelectionBoxColor(argb);
                    UpdateSelectionBoxColorSwatchStyle();
                },
                [this, original]() {
                    m_SelectionBoxColor = original;
                    SceneViewSettings::Get().SetSelectionBoxColor(original);
                    UpdateSelectionBoxColorSwatchStyle();
                },
                [this](uint32_t argb) {
                    m_SelectionBoxColor = argb;
                    SceneViewSettings::Get().SetSelectionBoxColor(argb);
                    UpdateSelectionBoxColorSwatchStyle();
                });
            e.Stop();
        };

        auto swatch = std::make_unique<UIElement>();
        swatch->SetId("settings-selection-box-color-swatch");
        m_SelectionBoxColorSwatch = swatch.get();
        UpdateSelectionBoxColorSwatchStyle();
        swatch->RegisterEventHandler(kEventMouseDown, openColorPicker);
        section->AddChild(std::move(swatch));

        constexpr uint32_t kDefaultBoxColor = 0x80FFFFFFu;
        AddDoubleClickReset(labelPtr, [this]() {
            m_SelectionBoxColor = kDefaultBoxColor;
            SceneViewSettings::Get().SetSelectionBoxColor(kDefaultBoxColor);
            UpdateSelectionBoxColorSwatchStyle();
        });

        m_ContentBody->AddContent(std::move(section));
    }

    // Bounding Box Thickness
    {
        SettingsSliderConfig config;
        config.labelText = "Box Thickness";
        config.defaultValue = settings.GetSelectionBoxThickness();
        config.minValue = 0.5f;
        config.maxValue = 10.0f;
        config.step = 0.1f;
        config.prefKey.clear();
        config.onValueChanged = [](float value) {
            SceneViewSettings::Get().SetSelectionBoxThickness(value);
        };
        CreateSettingsSliderRow(config, m_ContentBody);
    }

    // Outline Color swatch
    {
        m_SelectionOutlineColor = settings.GetSelectionOutlineColor();

        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Outline Color");
        label->AddClass("settings-row-label");
        Label* labelPtr = label.get();
        section->AddChild(std::move(label));

        auto openColorPicker = [this](UIEvent& e) {
            if (e.Button != 0)
                return;
            const uint32_t original = m_SelectionOutlineColor;
            OpenColorPicker(
                original,
                [this](uint32_t argb) {
                    m_SelectionOutlineColor = argb;
                    SceneViewSettings::Get().SetSelectionOutlineColor(argb);
                    UpdateSelectionOutlineColorSwatchStyle();
                },
                [this, original]() {
                    m_SelectionOutlineColor = original;
                    SceneViewSettings::Get().SetSelectionOutlineColor(original);
                    UpdateSelectionOutlineColorSwatchStyle();
                },
                [this](uint32_t argb) {
                    m_SelectionOutlineColor = argb;
                    SceneViewSettings::Get().SetSelectionOutlineColor(argb);
                    UpdateSelectionOutlineColorSwatchStyle();
                });
            e.Stop();
        };

        auto swatch = std::make_unique<UIElement>();
        swatch->SetId("settings-selection-outline-color-swatch");
        m_SelectionOutlineColorSwatch = swatch.get();
        UpdateSelectionOutlineColorSwatchStyle();
        swatch->RegisterEventHandler(kEventMouseDown, openColorPicker);
        section->AddChild(std::move(swatch));

        constexpr uint32_t kDefaultOutlineColor = 0xFFFFB300u;
        AddDoubleClickReset(labelPtr, [this]() {
            m_SelectionOutlineColor = kDefaultOutlineColor;
            SceneViewSettings::Get().SetSelectionOutlineColor(kDefaultOutlineColor);
            UpdateSelectionOutlineColorSwatchStyle();
        });

        m_ContentBody->AddContent(std::move(section));
    }

    // Outline Thickness
    {
        SettingsSliderConfig config;
        config.labelText = "Outline Thickness";
        config.defaultValue = settings.GetSelectionOutlineThickness();
        config.minValue = 0.25f;
        config.maxValue = 5.0f;
        config.step = 0.05f;
        config.prefKey.clear();
        config.onValueChanged = [](float value) {
            SceneViewSettings::Get().SetSelectionOutlineThickness(value);
        };
        CreateSettingsSliderRow(config, m_ContentBody);
    }

    // Description
    {
        m_ContentBody->AddContent(MakeSettingsInfoCard(
            "These settings control the editor Scene View camera (FOV, clip planes, movement "
            "speed, max orbit distance, and scroll wheel dolly)."));
    }

    SyncCameraSettingsControls();
}

void SettingsPanel::SyncCameraSettingsControls()
{
    if (m_CurrentCategory != SettingsCategory::Camera)
        return;

    const Editor::SceneViewSettings& settings = Editor::SceneViewSettings::Get();
    auto sync = [this](const char* idPrefix, float value)
    {
        if (auto* slider = dynamic_cast<Slider*>(FindById(std::string(idPrefix) + "Slider")))
            slider->SetValueWithoutNotify(value);
        if (auto* field = dynamic_cast<FloatField*>(FindById(std::string(idPrefix) + "Field")))
            field->SetValueWithoutNotify(value);
    };

    sync("SettingsSceneCameraFov", settings.GetFieldOfViewDeg());
    sync("SettingsSceneCameraNearClip", settings.GetNearClip());
    sync("SettingsSceneCameraFarClip", settings.GetFarClip());
    sync("SettingsSceneCameraMoveSpeed", settings.GetMoveSpeed());
    sync("SettingsSceneCameraFastMultiplier", settings.GetFastMoveMultiplier());
    sync("SettingsSceneCameraAcceleration", settings.GetMoveAccelerationTime());
    sync("SettingsSceneCameraFixedEv", settings.GetPostFxFixedExposureEv());
    sync("SettingsSceneCameraAutoMinEv", settings.GetPostFxAutoExposureMinEv());
    sync("SettingsSceneCameraAutoMaxEv", settings.GetPostFxAutoExposureMaxEv());
    sync("SettingsSceneCameraExposureComp", settings.GetPostFxExposureCompensation());
}


namespace
{
struct SettingsDropdownConfig
{
    std::string labelText;
    std::string tooltip;
    std::vector<std::string> options;
    // Value/label pairs, for options whose stored value differs from what the
    // row shows. Wins over `options` when non-empty.
    std::vector<Dropdown::Option> optionPairs;
    std::string defaultValue;
    std::string prefKey;
    std::function<std::string()> get;
    std::function<void(const std::string&)> onValueChanged;
};

void CreateSettingsDropdownRow(const SettingsDropdownConfig& config, ScrollView* contentBody)
{
    auto row = std::make_unique<UIElement>();
    row->AddClass("settings-row");

    auto label = std::make_unique<Label>();
    label->SetText(config.labelText);
    if (!config.tooltip.empty())
        label->SetTooltip(config.tooltip);
    label->AddClass("settings-row-label");
    row->AddChild(std::move(label));

    auto dropdown = std::make_unique<Dropdown>();
    dropdown->AddClass("settings-dropdown");
    if (!config.optionPairs.empty())
        dropdown->SetOptions(config.optionPairs, 0);
    else
        dropdown->SetOptionsFromLabels(config.options, 0);

    std::string currentValue = config.defaultValue;
    if (config.get)
    {
        currentValue = config.get();
    }
    else if (!config.prefKey.empty())
    {
        auto prefs = Editor::OpenEditorPreferences();
        prefs.TryGetString(config.prefKey, currentValue);
    }
    dropdown->SetSelectedValue(currentValue);

    dropdown->SetOnValueChanged(
        [onChanged = config.onValueChanged, prefKey = config.prefKey](const std::string& value)
        {
            if (onChanged)
                onChanged(value);
            if (!prefKey.empty())
            {
                auto prefs = Editor::OpenEditorPreferences();
                prefs.SetString(prefKey, value);
                std::string err;
                prefs.Save(&err);
            }
        });

    row->AddChild(std::move(dropdown));
    contentBody->AddContent(std::move(row));
}

struct SettingsTextFieldConfig
{
    std::string labelText;
    std::string tooltip;
    std::string defaultValue;
    std::string prefKey;
    std::function<std::string()> get;
    std::function<void(const std::string&)> onValueChanged;
    // When set, a Browse button opens the native picker of this kind and writes
    // the chosen path into the field.
    std::optional<Editor::SettingsFieldDescriptor::PathField::Kind> browseKind;
};

void CreateSettingsTextFieldRow(const SettingsTextFieldConfig& config, ScrollView* contentBody)
{
    auto row = std::make_unique<UIElement>();
    row->AddClass("settings-row");

    auto label = std::make_unique<Label>();
    label->SetText(config.labelText);
    if (!config.tooltip.empty())
        label->SetTooltip(config.tooltip);
    label->AddClass("settings-row-label");
    Label* labelPtr = label.get();
    row->AddChild(std::move(label));

    std::string currentValue = config.defaultValue;
    if (config.get)
    {
        currentValue = config.get();
    }
    else if (!config.prefKey.empty())
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        prefs.TryGetString(config.prefKey, currentValue);
    }

    auto field = std::make_unique<TextField>();
    field->SetValue(currentValue);
    if (!config.tooltip.empty())
        field->SetTooltip(config.tooltip);
    field->AddClass("settings-row-field");
    TextField* fieldPtr = field.get();

    // One commit path for typing, focus-out, Browse and double-click-to-reset,
    // so a value can never reach the callback without reaching the store.
    //
    // Focus-out fires whether or not the user typed anything, so an unchanged
    // value must not write: tabbing through a page is not an edit, and writing
    // there would persist the row's own default and stop that key tracking the
    // default from then on. Same obligation the toggle and slider rows carry.
    auto committed = std::make_shared<std::string>(currentValue);
    auto commit = [onChanged = config.onValueChanged, prefKey = config.prefKey,
                   committed](const std::string& value)
    {
        if (*committed == value)
            return;
        *committed = value;
        if (onChanged)
            onChanged(value);
        if (!prefKey.empty())
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetString(prefKey, value);
            prefs.Save(&err);
        }
    };

    fieldPtr->SetOnValueChanged(commit);
    fieldPtr->RegisterEventHandler(kEventFocusOut,
                                   [fieldPtr, commit](UIEvent&) { commit(fieldPtr->GetValue()); });
    row->AddChild(std::move(field));

    if (config.browseKind.has_value())
    {
        using PathKind = Editor::SettingsFieldDescriptor::PathField::Kind;
        const PathKind kind = *config.browseKind;

        auto browse = std::make_unique<Button>();
        browse->SetText("Browse...");
        browse->AddClass("small");
        browse->AddClass("secondary");
        browse->SetTooltip(kind == PathKind::Directory ? "Choose a folder." : "Choose a file.");
        browse->RegisterEventHandler(kEventButtonClick, [fieldPtr, commit, kind](UIEvent&)
        {
            // Seed the picker from the current value when it names a real
            // location, else from the open project.
            std::filesystem::path initial = EngineCore::GetInstance().GetWorkspaceRoot();
            const std::filesystem::path current(fieldPtr->GetValue());
            std::error_code ec;
            if (!current.empty() && std::filesystem::exists(current, ec))
                initial = current;

            const std::filesystem::path picked = kind == PathKind::Directory
                                                     ? Platform::SelectFolder(initial)
                                                     : Platform::SelectFile(initial);
            if (picked.empty())
                return; // cancelled
            const std::string value = picked.string();
            fieldPtr->SetValue(value);
            commit(value);
        });
        row->AddChild(std::move(browse));
    }

    AddDoubleClickReset(labelPtr, [fieldPtr, commit, defaultValue = config.defaultValue]()
    {
        fieldPtr->SetValue(defaultValue);
        commit(defaultValue);
    });

    contentBody->AddContent(std::move(row));
}

// Subsection break above a field, for pages whose rows fall into named groups.
void AppendSettingsSectionBreak(const Editor::SettingsFieldDescriptor& field,
                                ScrollView* contentBody)
{
    if (!field.SectionHeader.empty())
    {
        auto header = std::make_unique<Label>();
        header->SetText(field.SectionHeader);
        header->AddClass("settings-subsection-header");
        contentBody->AddContent(std::move(header));
    }
    if (!field.SectionDescription.empty())
    {
        auto description = std::make_unique<Label>();
        description->SetText(field.SectionDescription);
        description->AddClass("settings-description");
        contentBody->AddContent(std::move(description));
    }
}
} // namespace

void SettingsPanel::CreateRegistrySettingsContent(const Editor::SettingsCategoryDescriptor& descriptor)
{
    if (!m_ContentBody)
        return;

    using Editor::SettingsFieldDescriptor;

    Editor::SettingsCategoryDescriptor page = descriptor;
    if (page.PrepareFields)
        page.PrepareFields(page);

    if (page.CreateContent)
    {
        if (auto content = page.CreateContent())
            m_ContentBody->AddContent(std::move(content));
        return;
    }

    {
        auto header = std::make_unique<Label>();
        header->SetText(page.Title);
        header->AddClass("settings-section-header");
        m_ContentBody->AddContent(std::move(header));
    }

    for (const SettingsFieldDescriptor& field : page.Fields)
    {
        AppendSettingsSectionBreak(field, m_ContentBody);

        if (const auto* toggle = std::get_if<SettingsFieldDescriptor::ToggleField>(&field.Control))
        {
            SettingsToggleConfig config;
            config.labelText = field.Label;
            config.tooltip = field.Tooltip;
            config.defaultValue = toggle->DefaultValue;
            config.prefKey = field.PrefKey;
            if (toggle->Get)
                config.initialValueOverride = toggle->Get();
            config.onValueChanged = toggle->Set;
            CreateSettingsToggleRow(config, m_ContentBody);
        }
        else if (const auto* slider = std::get_if<SettingsFieldDescriptor::SliderField>(&field.Control))
        {
            SettingsSliderConfig config;
            config.labelText = field.Label;
            config.tooltip = field.Tooltip;
            config.defaultValue = slider->DefaultValue;
            if (slider->Get)
                config.initialValue = slider->Get();
            config.minValue = slider->MinValue;
            config.maxValue = slider->MaxValue;
            config.step = slider->Step;
            config.prefKey = field.PrefKey;
            config.onValueChanged = slider->Set;
            config.onValueChanging = slider->Set;
            CreateSettingsSliderRow(config, m_ContentBody);
        }
        else if (const auto* dropdown = std::get_if<SettingsFieldDescriptor::DropdownField>(&field.Control))
        {
            SettingsDropdownConfig config;
            config.labelText = field.Label;
            config.tooltip = field.Tooltip;
            config.options = dropdown->Options;
            if (dropdown->OptionsProvider)
            {
                for (const auto& option : dropdown->OptionsProvider())
                {
                    config.optionPairs.push_back(Dropdown::Option{
                        option.Value, option.Label.empty() ? option.Value : option.Label});
                }
            }
            config.defaultValue = dropdown->DefaultValue;
            config.prefKey = field.PrefKey;
            config.get = dropdown->Get;
            config.onValueChanged = dropdown->Set;
            CreateSettingsDropdownRow(config, m_ContentBody);
        }
        else if (const auto* text = std::get_if<SettingsFieldDescriptor::StringField>(&field.Control))
        {
            SettingsTextFieldConfig config;
            config.labelText = field.Label;
            config.tooltip = field.Tooltip;
            config.defaultValue = text->DefaultValue;
            config.prefKey = field.PrefKey;
            config.get = text->Get;
            config.onValueChanged = text->Set;
            CreateSettingsTextFieldRow(config, m_ContentBody);
        }
        else if (const auto* path = std::get_if<SettingsFieldDescriptor::PathField>(&field.Control))
        {
            SettingsTextFieldConfig config;
            config.labelText = field.Label;
            config.tooltip = field.Tooltip;
            config.defaultValue = path->DefaultValue;
            config.prefKey = field.PrefKey;
            config.get = path->Get;
            config.onValueChanged = path->Set;
            config.browseKind = path->PathKind;
            CreateSettingsTextFieldRow(config, m_ContentBody);
        }
        else if (const auto* button = std::get_if<SettingsFieldDescriptor::ButtonField>(&field.Control))
        {
            auto row = std::make_unique<UIElement>();
            row->AddClass("settings-row");
            if (button->IsVisible)
            {
                // Seed from the condition so a hidden row never flashes before
                // the first post-layout poll.
                row->Overrides().Set(Style::Display,
                                     button->IsVisible() ? DisplayMode::Flex : DisplayMode::None);
                m_RegistryButtonRows.push_back({row.get(), button->IsVisible});
            }

            if (!field.Label.empty())
            {
                auto label = std::make_unique<Label>();
                label->SetText(field.Label);
                if (!field.Tooltip.empty())
                    label->SetTooltip(field.Tooltip);
                label->AddClass("settings-row-label");
                row->AddChild(std::move(label));
            }

            auto action = std::make_unique<Button>();
            action->SetText(button->ButtonText);
            action->AddClass("small");
            action->AddClass("secondary");
            if (!field.Tooltip.empty())
                action->SetTooltip(field.Tooltip);
            action->RegisterEventHandler(kEventButtonClick, [onClick = button->OnClick](UIEvent&)
            {
                if (onClick)
                    onClick();
            });
            row->AddChild(std::move(action));

            m_ContentBody->AddContent(std::move(row));
        }
        else if (const auto* custom = std::get_if<SettingsFieldDescriptor::CustomField>(&field.Control))
        {
            if (auto element = custom->CreateRow())
                m_ContentBody->AddContent(std::move(element));
        }
        else if (const auto* toggleColor =
                     std::get_if<SettingsFieldDescriptor::ToggleColorField>(&field.Control))
        {
            auto section = std::make_unique<UIElement>();
            section->AddClass("settings-row");
            section->AddClass("settings-toggle-row");

            auto label = std::make_unique<Label>();
            label->SetText(field.Label);
            if (!field.Tooltip.empty())
                label->SetTooltip(field.Tooltip);
            label->AddClass("settings-row-label");
            Label* labelPtr = label.get();

            const bool enabled =
                toggleColor->GetEnabled ? toggleColor->GetEnabled() : toggleColor->DefaultEnabled;
            auto enabledToggle = std::make_unique<Toggle>();
            Toggle* togglePtr = enabledToggle.get();
            enabledToggle->AddClass("settings-toggle");
            enabledToggle->SetChecked(enabled);
            if (toggleColor->SetEnabled)
                toggleColor->SetEnabled(enabled);
            enabledToggle->SetOnValueChanged([setEnabled = toggleColor->SetEnabled](const bool& on)
            {
                if (setEnabled)
                    setEnabled(on);
            });

            auto currentArgb = std::make_shared<uint32_t>(
                toggleColor->GetColor ? toggleColor->GetColor() : toggleColor->DefaultArgb);
            auto swatch = std::make_unique<UIElement>();
            swatch->AddClass("settings-color-swatch");
            UIElement* swatchPtr = swatch.get();
            UpdateSwatchStyle(swatchPtr, *currentArgb);

            auto applyColor =
                [this, swatchPtr, currentArgb, setColor = toggleColor->SetColor](uint32_t argb,
                                                                                 bool persist)
            {
                *currentArgb = 0xFF000000u | (argb & 0x00FFFFFFu);
                if (setColor)
                    setColor(*currentArgb, persist);
                UpdateSwatchStyle(swatchPtr, *currentArgb);
            };

            swatch->RegisterEventHandler(kEventMouseDown, [this, applyColor, currentArgb](UIEvent& e)
            {
                if (e.Button != 0)
                    return;
                const uint32_t original = *currentArgb;
                OpenColorPicker(
                    original,
                    [applyColor](uint32_t argb) { applyColor(argb, true); },
                    [applyColor, original]() { applyColor(original, false); },
                    [applyColor](uint32_t argb) { applyColor(argb, false); });
                e.Stop();
            });

            AddDoubleClickReset(labelPtr,
                                [togglePtr, applyColor, defaultEnabled = toggleColor->DefaultEnabled,
                                 defaultArgb = toggleColor->DefaultArgb,
                                 setEnabled = toggleColor->SetEnabled]()
            {
                togglePtr->SetChecked(defaultEnabled);
                if (setEnabled)
                    setEnabled(defaultEnabled);
                applyColor(defaultArgb, true);
            });

            section->AddChild(std::move(label));
            section->AddChild(std::move(enabledToggle));
            section->AddChild(std::move(swatch));
            m_ContentBody->AddContent(std::move(section));
        }
        else if (const auto* color = std::get_if<SettingsFieldDescriptor::ColorField>(&field.Control))
        {
            // Color rows are singleton-backed (Get/Set); registration validation
            // guarantees Get is present.
            auto section = std::make_unique<UIElement>();
            section->AddClass("settings-row");

            auto label = std::make_unique<Label>();
            label->SetText(field.Label);
            if (!field.Tooltip.empty())
                label->SetTooltip(field.Tooltip);
            label->AddClass("settings-row-label");
            Label* labelPtr = label.get();
            section->AddChild(std::move(label));

            auto currentArgb = std::make_shared<uint32_t>(color->Get ? color->Get() : color->DefaultArgb);
            auto swatch = std::make_unique<UIElement>();
            UIElement* swatchPtr = swatch.get();
            UpdateSwatchStyle(swatchPtr, *currentArgb);

            auto applyColor = [this, swatchPtr, currentArgb, set = color->Set](uint32_t argb)
            {
                *currentArgb = argb;
                if (set)
                    set(argb);
                UpdateSwatchStyle(swatchPtr, argb);
            };
            swatch->RegisterEventHandler(kEventMouseDown,
                                         [this, applyColor, currentArgb](UIEvent& e)
            {
                if (e.Button != 0)
                    return;
                const uint32_t original = *currentArgb;
                OpenColorPicker(original, applyColor,
                                [applyColor, original]() { applyColor(original); },
                                applyColor);
                e.Stop();
            });
            section->AddChild(std::move(swatch));

            AddDoubleClickReset(labelPtr, [applyColor, defaultArgb = color->DefaultArgb]()
            {
                applyColor(defaultArgb);
            });

            m_ContentBody->AddContent(std::move(section));
        }
    }

    if (!descriptor.Description.empty())
    {
        auto description = MakeSettingsInfoCard(descriptor.Description);
        if (descriptor.DescriptionAlwaysVisible)
            EditorUI::MarkInfoCardAlwaysVisible(description.get());
        m_ContentBody->AddContent(std::move(description));
    }
}

void SettingsPanel::CreateGizmoSettingsContent()
{
    if (!m_ContentBody)
        return;

    // Read initial constant screen size state from preferences
    bool constantSizeEnabled = true;
    {
        auto prefs = Editor::OpenEditorPreferences();
        std::string err;
        prefs.Load(&err);
        prefs.TryGetBool("gizmo.constantScreenSize", constantSizeEnabled);
    }

    // Constant Screen Size toggle at the top
    {
        SettingsToggleConfig config;
        config.labelText = "Constant Screen Size";
        config.defaultValue = true;
        config.prefKey = "gizmo.constantScreenSize";
        config.onValueChanged = [this](bool enabled) {
            if (m_OnGizmoConstantSizeChanged)
                m_OnGizmoConstantSizeChanged(enabled);
            // Show/hide appropriate sections using display style
            if (m_GizmoThicknessSection)
            {
                if (enabled)
                    m_GizmoThicknessSection->Overrides().Set(Style::Display, DisplayMode::None);
                else
                    m_GizmoThicknessSection->Overrides().Reset(Style::Display);
            }
            if (m_GizmoConstantSizeSection)
            {
                if (enabled)
                    m_GizmoConstantSizeSection->Overrides().Reset(Style::Display);
                else
                    m_GizmoConstantSizeSection->Overrides().Set(Style::Display, DisplayMode::None);
            }
        };
        CreateSettingsToggleRow(config, m_ContentBody);
    }

    // Enhanced rotate gizmo: front-face culled rings, a screen-space ("view")
    // rotation ring, and a smooth edge-on drag. Toggle off for the legacy gizmo.
    {
        SettingsToggleConfig config;
        config.labelText = "Enhanced Rotate Gizmo";
        config.defaultValue = false;
        config.prefKey = "gizmo.rotateEnhanced";
        config.onValueChanged = [this](bool enabled) {
            if (m_OnRotateGizmoEnhancedChanged)
                m_OnRotateGizmoEnhancedChanged(enabled);
        };
        CreateSettingsToggleRow(config, m_ContentBody);
    }

    // Light range/cone/direction details are separate from the always-visible icons.
    {
        SettingsToggleConfig config;
        config.labelText = "Directional Light Details";
        config.tooltip = "Directional Gizmo Details Only On Selection";
        config.defaultValue = Editor::SceneViewSettings::Get().GetDirectionalLightGizmoDetailsOnlyOnSelection();
        config.prefKey.clear();
        config.onValueChanged = [](bool enabled) {
            Editor::SceneViewSettings::Get().SetDirectionalLightGizmoDetailsOnlyOnSelection(enabled);
        };
        CreateSettingsToggleRow(config, m_ContentBody);
    }
    {
        SettingsToggleConfig config;
        config.labelText = "Point Light Details";
        config.tooltip = "Point Gizmo Details Only On Selection";
        config.defaultValue = Editor::SceneViewSettings::Get().GetPointLightGizmoDetailsOnlyOnSelection();
        config.prefKey.clear();
        config.onValueChanged = [](bool enabled) {
            Editor::SceneViewSettings::Get().SetPointLightGizmoDetailsOnlyOnSelection(enabled);
        };
        CreateSettingsToggleRow(config, m_ContentBody);
    }
    {
        SettingsToggleConfig config;
        config.labelText = "Spot Light Details";
        config.tooltip = "Spot Gizmo Details Only On Selection";
        config.defaultValue = Editor::SceneViewSettings::Get().GetSpotLightGizmoDetailsOnlyOnSelection();
        config.prefKey.clear();
        config.onValueChanged = [](bool enabled) {
            Editor::SceneViewSettings::Get().SetSpotLightGizmoDetailsOnlyOnSelection(enabled);
        };
        CreateSettingsToggleRow(config, m_ContentBody);
    }

    // === Normal Thickness Section (shown when constant size is OFF) ===
    {
        auto sectionContainer = std::make_unique<UIElement>();
        if (constantSizeEnabled)
        {
            sectionContainer->Overrides().Set(Style::Display, DisplayMode::None);
        }
        m_GizmoThicknessSection = sectionContainer.get();

        // Section header
        {
            auto header = std::make_unique<Label>();
            header->AddClass("settings-section-header");
            header->SetText("Gizmo Thickness");
            sectionContainer->AddChild(std::move(header));
        }

        // Translate Gizmo Thickness
        {
            SettingsSliderConfig config;
            config.labelText = "Translate";
            config.defaultValue = 1.5f;
            config.minValue = 0.5f;
            config.maxValue = 3.0f;
            config.step = 0.1f;
            config.prefKey = "gizmo.translateThickness";
            config.onValueChanged = [this](float value) {
                if (m_OnTranslateGizmoThicknessChanged)
                    m_OnTranslateGizmoThicknessChanged(value);
            };
            
            auto row = CreateSettingsSliderRow(config, sectionContainer.get());
            m_TranslateGizmoThicknessSlider = row.slider;
        }

        // Rotate Gizmo Thickness
        {
            SettingsSliderConfig config;
            config.labelText = "Rotate";
            config.defaultValue = 5.5f;
            config.minValue = 0.5f;
            config.maxValue = 10.0f;
            config.step = 0.1f;
            config.prefKey = "gizmo.rotateThickness";
            config.onValueChanged = [this](float value) {
                if (m_OnRotateGizmoThicknessChanged)
                    m_OnRotateGizmoThicknessChanged(value);
            };
            
            auto row = CreateSettingsSliderRow(config, sectionContainer.get());
            m_RotateGizmoThicknessSlider = row.slider;
        }

        // Scale Gizmo Thickness
        {
            SettingsSliderConfig config;
            config.labelText = "Scale";
            config.defaultValue = 1.5f;
            config.minValue = 0.5f;
            config.maxValue = 5.0f;
            config.step = 0.1f;
            config.prefKey = "gizmo.scaleThickness";
            config.onValueChanged = [this](float value) {
                if (m_OnScaleGizmoThicknessChanged)
                    m_OnScaleGizmoThicknessChanged(value);
            };
            
            auto row = CreateSettingsSliderRow(config, sectionContainer.get());
            m_ScaleGizmoThicknessSlider = row.slider;
        }

        m_ContentBody->AddContent(std::move(sectionContainer));
    }

    // === Constant Screen Size Section (shown when constant size is ON) ===
    {
        auto sectionContainer = std::make_unique<UIElement>();
        if (!constantSizeEnabled)
        {
            sectionContainer->Overrides().Set(Style::Display, DisplayMode::None);
        }
        m_GizmoConstantSizeSection = sectionContainer.get();

        // Sub-header for gizmo scale
        {
            auto header = std::make_unique<Label>();
            header->AddClass("settings-section-header");
            header->SetText("Gizmo Scale");
            sectionContainer->AddChild(std::move(header));
        }

        // Translate Gizmo Scale
        {
            SettingsSliderConfig config;
            config.labelText = "Translate";
            config.defaultValue = 1.0f;
            config.minValue = 0.25f;
            config.maxValue = 3.0f;
            config.step = 0.05f;
            config.prefKey = "gizmo.translateScale";
            config.onValueChanged = [this](float value) {
                if (m_OnTranslateGizmoScaleChanged)
                    m_OnTranslateGizmoScaleChanged(value);
            };
            
            auto row = CreateSettingsSliderRow(config, sectionContainer.get());
            m_TranslateGizmoScaleSlider = row.slider;
        }

        // Rotate Gizmo Scale
        {
            SettingsSliderConfig config;
            config.labelText = "Rotate";
            config.defaultValue = 1.5f;
            config.minValue = 0.25f;
            config.maxValue = 3.0f;
            config.step = 0.05f;
            config.prefKey = "gizmo.rotateScale";
            config.onValueChanged = [this](float value) {
                if (m_OnRotateGizmoScaleChanged)
                    m_OnRotateGizmoScaleChanged(value);
            };
            
            auto row = CreateSettingsSliderRow(config, sectionContainer.get());
            m_RotateGizmoScaleSlider = row.slider;
        }

        // Scale Gizmo Scale
        {
            SettingsSliderConfig config;
            config.labelText = "Scale";
            config.defaultValue = 1.0f;
            config.minValue = 0.25f;
            config.maxValue = 3.0f;
            config.step = 0.05f;
            config.prefKey = "gizmo.scaleGizmoScale";
            config.onValueChanged = [this](float value) {
                if (m_OnScaleGizmoScaleChanged)
                    m_OnScaleGizmoScaleChanged(value);
            };
            
            auto row = CreateSettingsSliderRow(config, sectionContainer.get());
            m_ScaleGizmoScaleSlider = row.slider;
        }

        // Sub-header for constant size thickness
        {
            auto header = std::make_unique<Label>();
            header->AddClass("settings-section-header");
            header->SetText("Gizmo Thickness");
            sectionContainer->AddChild(std::move(header));
        }

        // Translate Constant Thickness
        {
            SettingsSliderConfig config;
            config.labelText = "Translate";
            config.defaultValue = 0.8f;
            config.minValue = 0.5f;
            config.maxValue = 5.0f;
            config.step = 0.1f;
            config.prefKey = "gizmo.translateConstantThickness";
            config.onValueChanged = [this](float value) {
                if (m_OnTranslateConstantThicknessChanged)
                    m_OnTranslateConstantThicknessChanged(value);
            };
            
            auto row = CreateSettingsSliderRow(config, sectionContainer.get());
            m_TranslateConstantThicknessSlider = row.slider;
        }

        // Rotate Constant Thickness
        {
            SettingsSliderConfig config;
            config.labelText = "Rotate";
            config.defaultValue = 4.2f;
            config.minValue = 0.5f;
            config.maxValue = 8.0f;
            config.step = 0.1f;
            config.prefKey = "gizmo.rotateConstantThickness";
            config.onValueChanged = [this](float value) {
                if (m_OnRotateConstantThicknessChanged)
                    m_OnRotateConstantThicknessChanged(value);
            };
            
            auto row = CreateSettingsSliderRow(config, sectionContainer.get());
            m_RotateConstantThicknessSlider = row.slider;
        }

        // Scale Constant Thickness
        {
            SettingsSliderConfig config;
            config.labelText = "Scale";
            config.defaultValue = 1.5f;
            config.minValue = 0.5f;
            config.maxValue = 5.0f;
            config.step = 0.1f;
            config.prefKey = "gizmo.scaleConstantThickness";
            config.onValueChanged = [this](float value) {
                if (m_OnScaleConstantThicknessChanged)
                    m_OnScaleConstantThicknessChanged(value);
            };
            
            auto row = CreateSettingsSliderRow(config, sectionContainer.get());
            m_ScaleConstantThicknessSlider = row.slider;
        }

        m_ContentBody->AddContent(std::move(sectionContainer));
    }
}

void SettingsPanel::CreateGridAndSnappingContent()
{
    if (!m_ContentBody)
        return;

    using GameEngine::Editor::SceneViewSettings;
    auto& svSettings = SceneViewSettings::Get();

    // Show grid on startup (moved from Scene View Settings).
    {
        SettingsToggleConfig config;
        config.labelText = "Show Grid on Startup";
        config.defaultValue = svSettings.GetGridVisibleOnStartup();
        config.prefKey.clear();
        config.onValueChanged = [](bool value) {
            SceneViewSettings::Get().SetGridVisibleOnStartup(value);
        };
        CreateSettingsToggleRow(config, m_ContentBody);
    }

    auto buildColorRow = [this](const char* labelText,
                                uint32_t initialArgb,
                                uint32_t defaultArgb,
                                UIElement** outSwatchPtr,
                                uint32_t* outColorMember,
                                std::function<void(uint32_t)> setSetting,
                                std::function<void()> updateStyle)
    {
        *outColorMember = initialArgb;

        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText(labelText);
        label->AddClass("settings-row-label");
        Label* labelPtr = label.get();
        section->AddChild(std::move(label));

        auto applyColor = [outColorMember, setSetting, updateStyle](uint32_t argb) {
            *outColorMember = argb;
            setSetting(argb);
            updateStyle();
        };

        auto openColorPicker = [this, applyColor, outColorMember](UIEvent& e) {
            if (e.Button != 0)
                return;
            const uint32_t original = *outColorMember;
            OpenColorPicker(
                original,
                applyColor,
                [applyColor, original]() { applyColor(original); },
                applyColor);
            e.Stop();
        };

        auto swatch = std::make_unique<UIElement>();
        swatch->AddClass("settings-color-swatch");
        *outSwatchPtr = swatch.get();
        updateStyle();
        swatch->RegisterEventHandler(kEventMouseDown, openColorPicker);
        section->AddChild(std::move(swatch));

        AddDoubleClickReset(labelPtr, [outColorMember, setSetting, updateStyle, defaultArgb]() {
            *outColorMember = defaultArgb;
            setSetting(defaultArgb);
            updateStyle();
        });

        m_ContentBody->AddContent(std::move(section));
    };

    // Grid Color (3D) — non-axis lines on the Y=0 plane.
    buildColorRow(
        "Grid Color (3D)",
        svSettings.GetGridColor3D(),
        0xFF000000u,
        &m_GridColor3DSwatch,
        &m_GridColor3D,
        [](uint32_t argb) { SceneViewSettings::Get().SetGridColor3D(argb); },
        [this]() { UpdateGridColor3DSwatchStyle(); });

    // Grid Color (2D) — non-axis lines on the Z=0 plane.
    // Default is semi-transparent white (#7AFFFFFF) so the grid sits gently
    // over the 2D backdrop without overpowering content.
    buildColorRow(
        "Grid Color (2D)",
        svSettings.GetGridColor2D(),
        0x7AFFFFFFu,
        &m_GridColor2DSwatch,
        &m_GridColor2D,
        [](uint32_t argb) { SceneViewSettings::Get().SetGridColor2D(argb); },
        [this]() { UpdateGridColor2DSwatchStyle(); });

    // Grid Opacity (3D)
    {
        SettingsSliderConfig config;
        config.labelText = "Opacity (3D)";
        config.defaultValue = svSettings.GetGridOpacity3D();
        config.minValue = 0.0f;
        config.maxValue = 1.0f;
        config.step = 0.05f;
        config.prefKey.clear();
        config.onValueChanged = [](float value) {
            SceneViewSettings::Get().SetGridOpacity3D(value);
        };
        config.onValueChanging = [](float value) {
            SceneViewSettings::Get().SetGridOpacity3D(value);
        };
        CreateSettingsSliderRow(config, m_ContentBody);
    }

    // Grid Opacity (2D)
    {
        SettingsSliderConfig config;
        config.labelText = "Opacity (2D)";
        config.defaultValue = svSettings.GetGridOpacity2D();
        config.minValue = 0.0f;
        config.maxValue = 1.0f;
        config.step = 0.05f;
        config.prefKey.clear();
        config.onValueChanged = [](float value) {
            SceneViewSettings::Get().SetGridOpacity2D(value);
        };
        config.onValueChanging = [](float value) {
            SceneViewSettings::Get().SetGridOpacity2D(value);
        };
        CreateSettingsSliderRow(config, m_ContentBody);
    }

    // Grid Snap Size (used in 3D mode; 2D adapts automatically)
    {
        SettingsSliderConfig config;
        config.labelText = "Snap Size";
        config.defaultValue = 0.5f;
        config.minValue = 0.1f;
        config.maxValue = 10.0f;
        config.step = 0.1f;
        config.prefKey = "grid.snapSize";
        config.onValueChanged = [this](float value) {
            if (m_OnGridSnapSizeChanged)
                m_OnGridSnapSizeChanged(value);
        };

        CreateSettingsSliderRow(config, m_ContentBody);
    }
}

void SettingsPanel::CreateAnimationSettingsContent()
{
    if (!m_ContentBody)
        return;

    auto& animSettings = AnimationWindowSettings::Get();

    // Helper: set hex text field from ARGB
    auto setHexField = [](TextField* f, uint32_t argb) {
        if (!f) return;
        char buf[16] = {};
        std::snprintf(buf, sizeof(buf), "#%06X", static_cast<unsigned int>(argb & 0xFFFFFFu));
        f->SetValue(buf);
    };

    // Helper: create a color setting row — label + swatch + hex field
    auto createColorRow = [this, setHexField](
        const char* labelText, uint32_t* settingPtr, uint32_t defaultArgb,
        std::function<void()> onSave) -> std::unique_ptr<UIElement>
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText(labelText);
        label->AddClass("settings-row-label");
        Label* labelPtr = label.get();
        section->AddChild(std::move(label));

        auto swatch = std::make_unique<UIElement>();
        swatch->AddClass("settings-color-swatch");
        UIElement* swatchPtr = swatch.get();
        swatch->Overrides()
            .Set(Style::Width, StyleLength::Px(16.0f))
            .Set(Style::Height, StyleLength::Px(16.0f))
            .Set(Style::FlexShrink, 0.0f)
            .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{2.0f, 2.0f, 2.0f, 2.0f})
            .Set(Style::BorderWidth, Box4{1.0f, 1.0f, 1.0f, 1.0f})
            .Set(Style::BorderColor, BorderColorsTRBL{0xFF3E3E3Eu, 0xFF3E3E3Eu, 0xFF3E3E3Eu, 0xFF3E3E3Eu})
            .Set(Style::BackgroundColor, *settingPtr)
            .Set(Style::Cursor, CursorStyle::Pointer);

        auto field = std::make_unique<TextField>();
        TextField* fieldPtr = field.get();
        setHexField(fieldPtr, *settingPtr);

        swatch->RegisterEventHandler(kEventMouseDown, [this, settingPtr, swatchPtr, fieldPtr, setHexField, onSave](UIEvent& e) {
            if (e.Button != 0) return;
            const uint32_t original = *settingPtr;
            OpenColorPicker(
                original,
                [settingPtr, swatchPtr, fieldPtr, setHexField, onSave](uint32_t argb) {
                    *settingPtr = argb;
                    onSave();
                    if (swatchPtr) swatchPtr->Overrides().Set(Style::BackgroundColor, argb);
                    setHexField(fieldPtr, argb);
                },
                [settingPtr, swatchPtr, original, fieldPtr, setHexField]() {
                    *settingPtr = original;
                    AnimationWindowSettings::Get().NotifyChanged();
                    if (swatchPtr) swatchPtr->Overrides().Set(Style::BackgroundColor, original);
                    setHexField(fieldPtr, original);
                },
                [settingPtr, swatchPtr, fieldPtr, setHexField](uint32_t argb) {
                    *settingPtr = argb;
                    AnimationWindowSettings::Get().NotifyChanged();
                    if (swatchPtr) swatchPtr->Overrides().Set(Style::BackgroundColor, argb);
                    setHexField(fieldPtr, argb);
                });
            e.Stop();
        });
        section->AddChild(std::move(swatch));

        field->AddClass("settings-row-field");
        field->AddClass("settings-color-field");
        field->SetOnValueChanged([settingPtr, swatchPtr, onSave](const std::string& value) {
            std::string hex = value;
            if (!hex.empty() && hex[0] == '#') hex = hex.substr(1);
            if (hex.length() == 6) {
                try {
                    uint32_t rgb = static_cast<uint32_t>(std::stoul(hex, nullptr, 16));
                    *settingPtr = 0xFF000000u | rgb;
                    onSave();
                    if (swatchPtr) swatchPtr->Overrides().Set(Style::BackgroundColor, *settingPtr);
                } catch (...) {}
            }
        });
        section->AddChild(std::move(field));

        AddDoubleClickReset(labelPtr, [settingPtr, swatchPtr, fieldPtr, defaultArgb, setHexField, onSave]() {
            *settingPtr = defaultArgb;
            onSave();
            if (swatchPtr) swatchPtr->Overrides().Set(Style::BackgroundColor, defaultArgb);
            setHexField(fieldPtr, defaultArgb);
        });
        return section;
    };

    // ── Curve Appearance ────────────────────────────────────────────────────
    {
        auto hdr = std::make_unique<Label>();
        hdr->SetText("Curve Appearance");
        hdr->AddClass("settings-section-header");
        m_ContentBody->AddContent(std::move(hdr));
    }

    auto saveAnimSettings = []() { AnimationWindowSettings::Get().Save(); AnimationWindowSettings::Get().NotifyChanged(); };
    m_ContentBody->AddContent(createColorRow("Keyframe Color",          &animSettings.KeyframeColor,         0xFF33CC80u, saveAnimSettings));
    m_ContentBody->AddContent(createColorRow("Keyframe Selected Color", &animSettings.KeyframeSelectedColor, 0xFFFF9933u, saveAnimSettings));
    m_ContentBody->AddContent(createColorRow("X Axis Color",  &animSettings.ColorX, 0xFFF25959u, saveAnimSettings));
    m_ContentBody->AddContent(createColorRow("Y Axis Color",  &animSettings.ColorY, 0xFF59D966u, saveAnimSettings));
    m_ContentBody->AddContent(createColorRow("Z Axis Color",  &animSettings.ColorZ, 0xFF598FF2u, saveAnimSettings));
    m_ContentBody->AddContent(createColorRow("W Axis Color",  &animSettings.ColorW, 0xFFF2CC59u, saveAnimSettings));
    m_ContentBody->AddContent(createColorRow("Grid H-Line Color", &animSettings.GridHLineColor, 0xFF808080u, saveAnimSettings));
    m_ContentBody->AddContent(createColorRow("Grid V-Line Color", &animSettings.GridVLineColor, 0xFFA4A4A4u, saveAnimSettings));
    m_ContentBody->AddContent(createColorRow("Baseline Color",   &animSettings.BaselineColor,  0xFF787878u, saveAnimSettings));

    {
        SettingsSliderConfig cfg;
        cfg.labelText = "Grid Line Thickness";
        cfg.minValue = 0.5f;
        cfg.maxValue = 4.0f;
        cfg.step = 0.5f;
        cfg.defaultValue = 1.0f;
        cfg.prefKey = "anim.gridLineThickness";
        cfg.onValueChanged = [](float v) {
            AnimationWindowSettings::Get().GridLineThickness = v;
            AnimationWindowSettings::Get().Save();
            AnimationWindowSettings::Get().NotifyChanged();
        };
        CreateSettingsSliderRow(cfg, m_ContentBody);
    }

    {
        SettingsSliderConfig cfg;
        cfg.labelText = "Baseline Thickness";
        cfg.minValue = 1.0f;
        cfg.maxValue = 6.0f;
        cfg.step = 0.5f;
        cfg.defaultValue = 2.0f;
        cfg.prefKey = "anim.baselineThickness";
        cfg.onValueChanged = [](float v) {
            AnimationWindowSettings::Get().BaselineThickness = v;
            AnimationWindowSettings::Get().Save();
            AnimationWindowSettings::Get().NotifyChanged();
        };
        CreateSettingsSliderRow(cfg, m_ContentBody);
    }

    {
        SettingsSliderConfig cfg;
        cfg.labelText = "Curve Line Width";
        cfg.minValue = 1.0f;
        cfg.maxValue = 5.0f;
        cfg.step = 0.5f;
        cfg.defaultValue = 2.0f;
        cfg.prefKey = "anim.curveLineWidth";
        cfg.onValueChanged = [](float v) {
            AnimationWindowSettings::Get().CurveLineWidth = v;
            AnimationWindowSettings::Get().Save();
            AnimationWindowSettings::Get().NotifyChanged();
        };
        CreateSettingsSliderRow(cfg, m_ContentBody);
    }

    // ── Timeline ────────────────────────────────────────────────────────────
    {
        auto hdr = std::make_unique<Label>();
        hdr->SetText("Timeline");
        hdr->AddClass("settings-section-header");
        m_ContentBody->AddContent(std::move(hdr));
    }

    {
        SettingsSliderConfig cfg;
        cfg.labelText = "Default FPS";
        cfg.minValue = 1.0f;
        cfg.maxValue = 240.0f;
        cfg.step = 1.0f;
        cfg.defaultValue = 60.0f;
        cfg.prefKey = "anim.defaultFps";
        cfg.onValueChanged = [](float v) {
            AnimationWindowSettings::Get().DefaultFps = v;
            AnimationWindowSettings::Get().Save();
            AnimationWindowSettings::Get().NotifyChanged();
        };
        CreateSettingsSliderRow(cfg, m_ContentBody);
    }

    // ── Defaults ────────────────────────────────────────────────────────────
    {
        auto hdr = std::make_unique<Label>();
        hdr->SetText("Defaults");
        hdr->AddClass("settings-section-header");
        m_ContentBody->AddContent(std::move(hdr));
    }

    {
        SettingsToggleConfig cfg;
        cfg.labelText = "Show Grid";
        cfg.defaultValue = true;
        cfg.prefKey = "anim.showGrid";
        cfg.onValueChanged = [](bool v) {
            AnimationWindowSettings::Get().ShowGrid = v;
            AnimationWindowSettings::Get().Save();
            AnimationWindowSettings::Get().NotifyChanged();
        };
        CreateSettingsToggleRow(cfg, m_ContentBody);
    }

    {
        SettingsToggleConfig cfg;
        cfg.labelText = "Show Bone Icons";
        cfg.defaultValue = true;
        cfg.prefKey = "anim.showBoneIcons";
        cfg.onValueChanged = [](bool v) {
            AnimationWindowSettings::Get().ShowBoneIcons = v;
            AnimationWindowSettings::Get().Save();
            AnimationWindowSettings::Get().NotifyChanged();
        };
        CreateSettingsToggleRow(cfg, m_ContentBody);
    }

    {
        SettingsToggleConfig cfg;
        cfg.labelText = "Snap Time";
        cfg.defaultValue = false;
        cfg.prefKey = "anim.snapTime";
        cfg.onValueChanged = [](bool v) {
            AnimationWindowSettings::Get().SnapTime = v;
            AnimationWindowSettings::Get().Save();
            AnimationWindowSettings::Get().NotifyChanged();
        };
        CreateSettingsToggleRow(cfg, m_ContentBody);
    }

    {
        SettingsToggleConfig cfg;
        cfg.labelText = "Snap Value";
        cfg.defaultValue = false;
        cfg.prefKey = "anim.snapValue";
        cfg.onValueChanged = [](bool v) {
            AnimationWindowSettings::Get().SnapValue = v;
            AnimationWindowSettings::Get().Save();
            AnimationWindowSettings::Get().NotifyChanged();
        };
        CreateSettingsToggleRow(cfg, m_ContentBody);
    }

    {
        SettingsToggleConfig cfg;
        cfg.labelText = "Properties Pane on Right";
        cfg.defaultValue = false;
        cfg.prefKey = "anim.propertiesPaneOnRight";
        cfg.onValueChanged = [](bool v) {
            AnimationWindowSettings::Get().PropertiesPaneOnRight = v;
            AnimationWindowSettings::Get().Save();
            AnimationWindowSettings::Get().NotifyChanged();
        };
        CreateSettingsToggleRow(cfg, m_ContentBody);
    }

    {
        SettingsToggleConfig cfg;
        cfg.labelText = "Ruler at Top";
        cfg.defaultValue = true;
        cfg.prefKey = "anim.rulerAtTop";
        cfg.onValueChanged = [](bool v) {
            AnimationWindowSettings::Get().RulerAtTop = v;
            AnimationWindowSettings::Get().Save();
            AnimationWindowSettings::Get().NotifyChanged();
        };
        CreateSettingsToggleRow(cfg, m_ContentBody);
    }
}

void SettingsPanel::CreateInputSettingsContent()
{
    if (!m_ContentBody)
        return;

    using GameEngine::Editor::SceneViewSettings;

    SceneViewSettings& settings = SceneViewSettings::Get();

    {
        auto sectionHeader = std::make_unique<Label>();
        sectionHeader->SetText("Scene View Mouse");
        sectionHeader->AddClass("settings-section-header");
        m_ContentBody->AddContent(std::move(sectionHeader));
    }

    {
        SettingsSliderConfig config;
        config.labelText = "Look Sensitivity";
        config.defaultValue = settings.GetLookSensitivity();
        config.minValue = 0.01f;
        config.maxValue = 2.0f;
        config.step = 0.01f;
        config.prefKey.clear();
        config.onValueChanged = [](float value) {
            SceneViewSettings::Get().SetLookSensitivity(value);
        };
        CreateSettingsSliderRow(config, m_ContentBody);
    }

    {
        SettingsToggleConfig config;
        config.labelText = "Invert Y Axis";
        config.defaultValue = settings.GetInvertY();
        config.prefKey.clear();
        config.onValueChanged = [](bool value) {
            SceneViewSettings::Get().SetInvertY(value);
        };
        CreateSettingsToggleRow(config, m_ContentBody);
    }
}

void SettingsPanel::CreateAssetImportSettingsContent()
{
    if (!m_ContentBody)
        return;

    const auto workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
    Editor::FbxImportSettings settings = Editor::FbxImportSettings::Load(workspaceRoot);

    {
        auto sectionHeader = std::make_unique<Label>();
        sectionHeader->SetText("SVG Texture Import");
        sectionHeader->AddClass("settings-section-header");
        m_ContentBody->AddContent(std::move(sectionHeader));
    }
    m_ContentBody->AddContent(BuildSvgRasterSizeRow(Editor::SvgRasterSettingsScope::TextureImport));

    {
        auto sectionHeader = std::make_unique<Label>();
        sectionHeader->SetText("FBX Import");
        sectionHeader->AddClass("settings-section-header");
        m_ContentBody->AddContent(std::move(sectionHeader));
    }

    auto addToggleRow = [this, workspaceRoot](const std::string& labelText,
                                               bool value,
                                               bool defaultValue,
                                               std::function<void(Editor::FbxImportSettings&, bool)> assign,
                                               const char* tooltip)
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText(labelText);
        label->SetTooltip(tooltip);
        label->AddClass("settings-row-label");
        Label* labelPtr = label.get();

        auto toggle = std::make_unique<Toggle>();
        toggle->AddClass("settings-toggle");
        Toggle* togglePtr = toggle.get();
        toggle->SetChecked(value);
        toggle->SetTooltip(tooltip);

        auto saveValue = [workspaceRoot, assign](bool newValue)
        {
            Editor::FbxImportSettings next = Editor::FbxImportSettings::Load(workspaceRoot);
            assign(next, newValue);
            next.Save(workspaceRoot);
        };

        toggle->SetOnValueChanged([saveValue](const bool& checked)
        {
            saveValue(checked);
        });

        AddDoubleClickReset(labelPtr, [togglePtr, saveValue, defaultValue]()
        {
            togglePtr->SetChecked(defaultValue);
            saveValue(defaultValue);
        });

        row->AddChild(std::move(label));
        row->AddChild(std::move(toggle));
        m_ContentBody->AddContent(std::move(row));
    };

    addToggleRow("Scene Extras", settings.ImportSceneExtras, true,
                 [](Editor::FbxImportSettings& s, bool value) { s.ImportSceneExtras = value; },
                 "Import auxiliary FBX scene data such as cameras, lights, and helper nodes.");
    addToggleRow("Cameras", settings.ImportCameras, true,
                 [](Editor::FbxImportSettings& s, bool value) { s.ImportCameras = value; },
                 "Create camera entities from imported FBX cameras.");
    addToggleRow("Lights", settings.ImportLights, true,
                 [](Editor::FbxImportSettings& s, bool value) { s.ImportLights = value; },
                 "Create light entities from imported FBX lights.");
    addToggleRow("Helper Nodes", settings.ImportHelperNodes, true,
                 [](Editor::FbxImportSettings& s, bool value) { s.ImportHelperNodes = value; },
                 "Preserve helper/null nodes from the authored FBX hierarchy.");
    addToggleRow("Generate Missing Tangents", settings.GenerateMissingTangents, true,
                 [](Editor::FbxImportSettings& s, bool value) { s.GenerateMissingTangents = value; },
                 "Generate tangent vectors for meshes that need normal mapping but do not provide tangents.");
    addToggleRow("Clean Skin Weights", settings.CleanSkinWeights, true,
                 [](Editor::FbxImportSettings& s, bool value) { s.CleanSkinWeights = value; },
                 "Remove invalid, zero, and negative skin weights during FBX import.");
    addToggleRow("Adjust Pivots", settings.AdjustPivots, true,
                 [](Editor::FbxImportSettings& s, bool value) { s.AdjustPivots = value; },
                 "Apply authored FBX pivot handling during import.");
    addToggleRow("Preserve Geo Transforms", settings.PreserveGeometryTransforms, true,
                 [](Editor::FbxImportSettings& s, bool value) { s.PreserveGeometryTransforms = value; },
                 "Keep authored geometry transforms instead of baking them into mesh vertices.");
    addToggleRow("Import Embedded Textures", settings.ImportEmbeddedTextures, true,
                 [](Editor::FbxImportSettings& s, bool value) { s.ImportEmbeddedTextures = value; },
                 "Read texture image data embedded directly in FBX files.");

    {
        auto sectionHeader = std::make_unique<Label>();
        sectionHeader->SetText("Automatic LODs");
        sectionHeader->AddClass("settings-section-header");
        m_ContentBody->AddContent(std::move(sectionHeader));
    }

    // Project-scoped (like the FBX import toggles above) so the whole team gets
    // uniform import behavior. The callback persists to project settings and
    // pushes the live policy down to the engine import path (which has no editor
    // dependency). When off, LODs are still available from the model inspector
    // and the API.
    {
        bool autoLODs = true; // default-on for static meshes when the project has no explicit key
        {
            Editor::SettingsStore store = Editor::OpenProjectSettings(workspaceRoot);
            std::string err;
            (void)store.Load(&err);
            store.TryGetBool("import.autoGenerateLODs", autoLODs);
        }

        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Auto-Generate LODs");
        label->SetTooltip("Auto-Generate LODs On Import: generate simplified mesh LODs automatically when a model is imported into this project. "
                          "When off, LODs can still be generated from the model inspector or via the API.");
        label->AddClass("settings-row-label");
        Label* labelPtr = label.get();

        auto toggle = std::make_unique<Toggle>();
        toggle->AddClass("settings-toggle");
        toggle->SetChecked(autoLODs);
        Toggle* togglePtr = toggle.get();

        auto apply = [workspaceRoot](bool value) {
            Editor::SettingsStore store = Editor::OpenProjectSettings(workspaceRoot);
            std::string err;
            (void)store.Load(&err);
            store.SetBool("import.autoGenerateLODs", value);
            (void)store.Save(&err);
            GameEngine::LODImportSettings lod = GameEngine::GetLODImportSettings();
            lod.AutoGenerateOnImport = value;
            GameEngine::SetLODImportSettings(lod);
        };

        toggle->SetOnValueChanged([apply](const bool& checked) { apply(checked); });
        AddDoubleClickReset(labelPtr, [togglePtr, apply]() {
            togglePtr->SetChecked(false);
            apply(false);
        });

        row->AddChild(std::move(label));
        row->AddChild(std::move(toggle));
        m_ContentBody->AddContent(std::move(row));
    }
}

void SettingsPanel::NotifyProjectRenderSettingsChanged()
{
    if (m_OnProjectRenderSettingsChanged)
    {
        m_OnProjectRenderSettingsChanged();
        return;
    }

    // Standalone/test hosts may not install the application fan-out. Keep the
    // edited window live in that case without duplicating work in the normal
    // multi-window editor path.
    if (auto* renderServices = EngineCore::GetInstance().GetRenderServices())
        Editor::ApplyProjectRenderSettings(
            *renderServices, EngineCore::GetInstance().GetWorkspaceRoot());
}

void SettingsPanel::CreateRenderingSettingsContent()
{
    if (!m_ContentBody)
        return;

    // Section header
    {
        auto sectionHeader = std::make_unique<Label>();
        sectionHeader->SetText("Render Pipeline");
        sectionHeader->AddClass("settings-section-header");
        m_ContentBody->AddContent(std::move(sectionHeader));
    }

    // Load current value from project settings (fallback to current RenderServices state).
    std::string pipelinePathStr;
    {
        const auto& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
        Editor::SettingsStore store = Editor::OpenProjectSettings(workspaceRoot);
        std::string err;
        (void)store.Load(&err);

        try
        {
            const auto& root = store.Json();
            if (root.is_object())
            {
                const auto itR = root.find("rendering");
                if (itR != root.end() && itR->is_object())
                {
                    const auto itP = itR->find("activeRenderPipeline");
                    if (itP != itR->end() && itP->is_string())
                        pipelinePathStr = itP->get<std::string>();
                }
            }
        }
        catch (...)
        {
        }

        if (pipelinePathStr.empty())
        {
            if (auto* rs = EngineCore::GetInstance().GetRenderServices())
            {
                pipelinePathStr = rs->Spine().GetActiveRenderPipelinePath().generic_string();
            }
        }
        if (pipelinePathStr.empty())
        {
            pipelinePathStr = "RenderPipelines/ForwardPlus.rendergraph";
        }
    }

    Dropdown* pipelineDropdown = nullptr;

    // Active pipeline (dropdown)
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Active Pipeline");
        label->AddClass("settings-row-label");
        row->AddChild(std::move(label));

        // Enumerate pipelines from the asset registry.
        auto& am = EngineCore::GetInstance().GetAssetManager();
        auto& reg = am.GetRegistry();

        std::vector<Dropdown::Option> options;
        {
            const auto guids = reg.GetAssetsByType(AssetType::RenderPipeline);
            options.reserve((size_t)guids.size() + 2);

            for (const GUID& g : guids)
            {
                AssetMetadata md{};
                if (!reg.TryGetAssetMetadata(g, md))
                    continue;
                if (md.Type != AssetType::RenderPipeline)
                    continue;
                const std::string rel = TryMakeAssetRelativePathString(am, md.Path);
                if (rel.empty())
                    continue;
                // Display label: strip directory prefix and .rendergraph extension.
                std::filesystem::path relPath(rel);
                std::string displayName = relPath.stem().string();
                // Active pipeline in white, others in gray.
                std::string color = (rel == pipelinePathStr) ? "#FFFFFF" : "#888888";
                options.push_back(Dropdown::Option{rel, displayName, color});
            }

            // Deduplicate by value (same file may have multiple GUIDs).
            std::sort(options.begin(), options.end(),
                      [](const Dropdown::Option& a, const Dropdown::Option& b)
                      { return a.value < b.value; });
            options.erase(std::unique(options.begin(), options.end(),
                                      [](const Dropdown::Option& a, const Dropdown::Option& b)
                                      { return a.value == b.value; }),
                          options.end());
            std::sort(options.begin(), options.end(),
                      [](const Dropdown::Option& a, const Dropdown::Option& b)
                      { return a.label < b.label; });
        }

        // Ensure current selection exists even if missing/unregistered.
        {
            bool found = false;
            for (const auto& o : options)
            {
                if (o.value == pipelinePathStr)
                {
                    found = true;
                    break;
                }
            }
            if (!found && !pipelinePathStr.empty())
            {
                std::string missingName = std::filesystem::path(pipelinePathStr).stem().string() + " (missing)";
                options.insert(options.begin(), Dropdown::Option{pipelinePathStr, missingName});
            }
            if (options.empty())
            {
                // Extremely defensive fallback.
                options.push_back(Dropdown::Option{"RenderPipelines/ForwardPlus.rendergraph", "ForwardPlus"});
            }
        }

        auto dd = std::make_unique<Dropdown>();
        dd->AddClass("settings-row-field");
        dd->SetOptions(options, 0);
        dd->SetSelectedValue(pipelinePathStr);
        pipelineDropdown = dd.get();
        g_ActivePipelineDropdown = pipelineDropdown;

        dd->SetOnValueChanged([](const std::string& value) {
            if (g_SuppressActivePipelineDropdownCallback)
                return;
            const auto& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
            if (workspaceRoot.empty())
            {
                Logger::Log::Warning("SettingsPanel: cannot save render pipeline setting -- no workspace root");
                return;
            }

            Editor::SettingsStore store = Editor::OpenProjectSettings(workspaceRoot);
            std::string err;
            (void)store.Load(&err);

            auto& root = store.Json();
            if (!root.is_object())
                root = nlohmann::json::object();

            auto& r = root["rendering"];
            if (!r.is_object())
                r = nlohmann::json::object();
            r["activeRenderPipeline"] = value;

            if (!store.Save(&err))
            {
                Logger::Log::Error("SettingsPanel: failed to save render pipeline setting: {}", err);
            }
            else
            {
                Logger::Log::Info("SettingsPanel: saved active render pipeline: {}", value);
            }

            // Apply live.
            if (auto* rs = EngineCore::GetInstance().GetRenderServices())
            {
                rs->Spine().SetActiveRenderPipelinePath(std::filesystem::path(value));
            }
        });

        row->AddChild(std::move(dd));

        // Ping button — locate the active pipeline in the Assets panel.
        if (m_PingAsset)
        {
            auto pingBtn = std::make_unique<Button>();
            pingBtn->AddClass("icon-button");
            pingBtn->AddClass("link-icon");
            auto pingAsset = m_PingAsset;
            auto* ddPtr = pipelineDropdown;
            pingBtn->RegisterEventHandler(kEventButtonClick, [pingAsset, ddPtr](UIEvent&) {
                if (!ddPtr)
                    return;
                const std::string value = ddPtr->GetSelectedValue();
                if (value.empty())
                    return;
                const auto& assetRoot = EngineCore::GetInstance().GetAssetManager().GetAssetRoot();
                if (assetRoot.empty())
                    return;
                pingAsset(assetRoot / value);
            });
            row->AddChild(std::move(pingBtn));
        }

        m_ContentBody->AddContent(std::move(row));
    }

    // Reset button
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");

        auto spacer = std::make_unique<Label>();
        spacer->SetText(""); // aligns with label column
        spacer->AddClass("settings-row-label");
        row->AddChild(std::move(spacer));

        auto btn = std::make_unique<Button>();
        btn->SetText("Reset to Default");
        btn->AddClass("small");
        btn->AddClass("secondary");
        btn->RegisterEventHandler(kEventButtonClick, [pipelineDropdown](UIEvent&) {
            const std::string value = "RenderPipelines/ForwardPlus.rendergraph";
            const auto& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
            if (workspaceRoot.empty())
            {
                Logger::Log::Warning("SettingsPanel: cannot save render pipeline setting -- no workspace root");
                return;
            }

            Editor::SettingsStore store = Editor::OpenProjectSettings(workspaceRoot);
            std::string err;
            (void)store.Load(&err);

            auto& root = store.Json();
            if (!root.is_object())
                root = nlohmann::json::object();
            auto& r = root["rendering"];
            if (!r.is_object())
                r = nlohmann::json::object();
            r["activeRenderPipeline"] = value;

            if (!store.Save(&err))
            {
                Logger::Log::Error("SettingsPanel: failed to save render pipeline reset: {}", err);
            }
            else
            {
                Logger::Log::Info("SettingsPanel: reset active render pipeline to: {}", value);
            }

            if (auto* rs = EngineCore::GetInstance().GetRenderServices())
            {
                rs->Spine().SetActiveRenderPipelinePath(std::filesystem::path(value));
            }

            if (pipelineDropdown)
                pipelineDropdown->SetSelectedValue(value);
        });

        row->AddChild(std::move(btn));
        m_ContentBody->AddContent(std::move(row));
    }

    CreateRegistrySettingsContent(Editor::CreateDirectionalShadowSettingsSection([this]()
    {
        NotifyProjectRenderSettingsChanged();
    }));

}

void SettingsPanel::CreateHDROutputSettingsContent()
{
    if (!m_ContentBody)
        return;

    {
        auto sectionHeader = std::make_unique<Label>();
        sectionHeader->SetText("HDR Output");
        sectionHeader->AddClass("settings-section-header");
        m_ContentBody->AddContent(std::move(sectionHeader));
    }

    const auto& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
    bool hdrEnabled = false;
    std::string hdrMode = Rendering::HdrOutputModeToString(Rendering::HdrOutputMode::Auto);
    int hdrSwapchainBitDepth = 10;
    int hdrTargetDisplay = -1;
    Rendering::HdrStaticMetadata hdrMetadata{};
    // Whether the user has explicitly overridden paper-white / max-luminance (a saved value), vs.
    // letting them auto-detect from the display. Drives whether the field shows the override or the
    // detected value, and whether "Reset to detected" has anything to clear.
    bool paperWhiteHasOverride = false;
    bool maxLumHasOverride = false;
    float hdrUiPaperWhiteNits = 120.0f;
    bool hdrUiPaperWhiteHasOverride = false;
    float hdrUiDarkOffset = 0.0f;
    bool hdrUiDarkOffsetHasOverride = false;
    {
        Editor::SettingsStore store = Editor::OpenProjectSettings(workspaceRoot);
        std::string err;
        (void)store.Load(&err);
        const auto& root = store.Json();
        if (root.is_object())
        {
            const auto itR = root.find("rendering");
            if (itR != root.end() && itR->is_object())
            {
                const auto itH = itR->find("hdr");
                if (itH != itR->end() && itH->is_object())
                {
                    const auto& h = *itH;
                    if (h.contains("enabled") && h["enabled"].is_boolean())
                        hdrEnabled = h["enabled"].get<bool>();
                    if (h.contains("mode") && h["mode"].is_string())
                        hdrMode = h["mode"].get<std::string>();
                    if (h.contains("swapchainBitDepth"))
                    {
                        if (h["swapchainBitDepth"].is_number_integer())
                            hdrSwapchainBitDepth = h["swapchainBitDepth"].get<int>() == 16 ? 16 : 10;
                        else if (h["swapchainBitDepth"].is_string())
                            hdrSwapchainBitDepth = h["swapchainBitDepth"].get<std::string>() == "16" ? 16 : 10;
                    }
                    if (h.contains("targetDisplay") && h["targetDisplay"].is_number_integer())
                        hdrTargetDisplay = h["targetDisplay"].get<int>();
                    if (h.contains("uiPaperWhiteNits") && h["uiPaperWhiteNits"].is_number())
                    {
                        hdrUiPaperWhiteNits = std::clamp(h["uiPaperWhiteNits"].get<float>(), 40.0f, 350.0f);
                        hdrUiPaperWhiteHasOverride = true;
                    }
                    if (h.contains("uiBlackLiftNits") && h["uiBlackLiftNits"].is_number())
                    {
                        hdrUiDarkOffset = std::clamp(h["uiBlackLiftNits"].get<float>(),
                                                     GameEngine::UIManager::kHdrUiBlackLiftMinNits,
                                                     GameEngine::UIManager::kHdrUiBlackLiftMaxNits);
                        hdrUiDarkOffsetHasOverride = true;
                    }
                    if (h.contains("metadata") && h["metadata"].is_object())
                    {
                        const auto& m = h["metadata"];
                        if (m.contains("maxMasteringLuminance") && m["maxMasteringLuminance"].is_number())
                        {
                            hdrMetadata.maxMasteringLuminance = m["maxMasteringLuminance"].get<float>();
                            maxLumHasOverride = true;
                        }
                        if (m.contains("minMasteringLuminance") && m["minMasteringLuminance"].is_number())
                            hdrMetadata.minMasteringLuminance = m["minMasteringLuminance"].get<float>();
                        if (m.contains("maxContentLightLevel") && m["maxContentLightLevel"].is_number())
                            hdrMetadata.maxContentLightLevel = m["maxContentLightLevel"].get<float>();
                        if (m.contains("maxFrameAverageLightLevel") && m["maxFrameAverageLightLevel"].is_number())
                            hdrMetadata.maxFrameAverageLightLevel = m["maxFrameAverageLightLevel"].get<float>();
                        if (m.contains("paperWhiteNits") && m["paperWhiteNits"].is_number())
                        {
                            hdrMetadata.paperWhiteNits = m["paperWhiteNits"].get<float>();
                            paperWhiteHasOverride = true;
                        }
                    }
                    else
                    {
                        if (h.contains("maxLuminanceNits") && h["maxLuminanceNits"].is_number())
                        {
                            const float maxLuminance = h["maxLuminanceNits"].get<float>();
                            hdrMetadata.maxMasteringLuminance = maxLuminance;
                            hdrMetadata.maxContentLightLevel = maxLuminance;
                            maxLumHasOverride = true;
                        }
                        if (h.contains("referenceLuminanceNits") && h["referenceLuminanceNits"].is_number())
                        {
                            hdrMetadata.paperWhiteNits = h["referenceLuminanceNits"].get<float>();
                            paperWhiteHasOverride = true;
                        }
                    }
                }
            }
        }
    }

    auto saveHdrSetting = [](auto&& edit) {
        const auto& rootPath = EngineCore::GetInstance().GetWorkspaceRoot();
        if (rootPath.empty())
        {
            Logger::Log::Warning("SettingsPanel: cannot save HDR setting -- no workspace root");
            return;
        }
        Editor::SettingsStore store = Editor::OpenProjectSettings(rootPath);
        std::string err;
        (void)store.Load(&err);
        auto& root = store.Json();
        if (!root.is_object())
            root = nlohmann::json::object();
        auto& rendering = root["rendering"];
        if (!rendering.is_object())
            rendering = nlohmann::json::object();
        auto& hdr = rendering["hdr"];
        if (!hdr.is_object())
            hdr = nlohmann::json::object();
        edit(hdr);
        if (!store.Save(&err))
            Logger::Log::Error("SettingsPanel: failed to save HDR setting: {}", err);
    };

    std::weak_ptr<bool> panelLifetime = m_LifetimeToken;

    auto panelAlive = [panelLifetime]() {
        auto alive = panelLifetime.lock();
        return alive && *alive;
    };

    // Rebuild the HDR content (re-reads the override flags) after a toggle / reset / enable changes them.
    auto rebuildHdrContent = [this, panelAlive]() {
        if (!panelAlive())
            return;
        this->PostAction([this, panelAlive]() {
            if (panelAlive() && m_CurrentCategory == SettingsCategory::HDROutput)
                ShowCategoryContent(SettingsCategory::HDROutput);
        });
    };

    auto requestHdrOutputRefresh = [this, panelAlive]() {
        if (panelAlive() && m_OnHdrOutputSettingsChanged)
            m_OnHdrOutputSettingsChanged();
    };

    auto readHdrFloatSetting = [](const char* key, float fallback, float minValue, float maxValue) {
        Editor::SettingsStore store = Editor::OpenProjectSettings(EngineCore::GetInstance().GetWorkspaceRoot());
        std::string err;
        (void)store.Load(&err);
        const auto& root = store.Json();
        if (root.is_object())
        {
            const auto itR = root.find("rendering");
            if (itR != root.end() && itR->is_object())
            {
                const auto itH = itR->find("hdr");
                if (itH != itR->end() && itH->is_object())
                {
                    const auto& h = *itH;
                    if (h.contains(key) && h[key].is_number())
                        return std::clamp(h[key].get<float>(), minValue, maxValue);
                }
            }
        }
        return std::clamp(fallback, minValue, maxValue);
    };

    auto applyHdrUiMapping = [this, panelAlive](float paperWhite, float darkOffset) {
        if (!panelAlive() || !m_UIManager)
            return;
        auto cfg = m_UIManager->GetRenderRuntimeConfig();
        cfg.HdrUiPaperWhiteNits = paperWhite > 0.0f ? std::clamp(paperWhite, 40.0f, 350.0f) : 0.0f;
        // Black-floor LIFT in nits (0 = faithful, no subtraction). See ApplyUiRuntimeConfig.
        // Range-clamped authoritatively in UIManager::ApplyRenderRuntimeConfig.
        cfg.HdrUiBlackLiftNits = darkOffset;
        m_UIManager->ApplyRenderRuntimeConfig(cfg);
    };

    auto hdrDependentRows = std::make_shared<std::vector<UIElement*>>();
    auto setHdrControlsVisible = [hdrDependentRows, contentBody = m_ContentBody](bool visible) {
        for (UIElement* row : *hdrDependentRows)
        {
            if (!row)
                continue;
            if (visible)
                row->Overrides().Set(Style::Display, DisplayMode::Flex);
            else
                row->Overrides().Set(Style::Display, DisplayMode::None);
            row->MarkDirty(UIElement::StyleDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
        }
        if (contentBody)
            contentBody->MarkDirty(UIElement::ChildrenDirty | UIElement::LayoutDirty | UIElement::VisualDirty);
    };
    auto addHdrDependentRow = [this, hdrDependentRows, hdrEnabled](std::unique_ptr<UIElement> row) {
        UIElement* rowPtr = row.get();
        hdrDependentRows->push_back(rowPtr);
        rowPtr->Overrides().Set(Style::Display, hdrEnabled ? DisplayMode::Flex : DisplayMode::None);
        m_ContentBody->AddContent(std::move(row));
    };

    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");
        auto label = std::make_unique<Label>();
        label->SetText("Enable HDR");
        label->AddClass("settings-row-label");
        row->AddChild(std::move(label));

        auto toggle = std::make_unique<Toggle>();
        toggle->SetChecked(hdrEnabled);
        Editor::SceneViewSettings::Get().SetHdrOutputEnabled(hdrEnabled);
        toggle->SetOnValueChanged([this, saveHdrSetting, requestHdrOutputRefresh, setHdrControlsVisible, rebuildHdrContent](const bool& enabled) {
            saveHdrSetting([enabled](nlohmann::json& hdr)
            {
                hdr["enabled"] = enabled;
                if (enabled)
                {
                    Rendering::HdrOutputMode mode = Rendering::HdrOutputMode::Off;
                    if (hdr.contains("mode") && hdr["mode"].is_string())
                        mode = Rendering::HdrOutputModeFromString(hdr["mode"].get<std::string>());
                    if (mode == Rendering::HdrOutputMode::Off)
                        hdr["mode"] = Rendering::HdrOutputModeToString(Rendering::HdrOutputMode::Auto);
                }
            });
            Editor::SceneViewSettings::Get().SetHdrOutputEnabled(enabled);
            setHdrControlsVisible(enabled);
            rebuildHdrContent();
            requestHdrOutputRefresh();
        });
        row->AddChild(std::move(toggle));
        m_ContentBody->AddContent(std::move(row));
    }

    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");
        auto label = std::make_unique<Label>();
        label->SetText("Scene View HDR Test");
        label->AddClass("settings-row-label");
        row->AddChild(std::move(label));

        auto toggle = std::make_unique<Toggle>();
        toggle->SetChecked(Editor::SceneViewSettings::Get().GetHdrTestPatternEnabled());
        toggle->SetOnValueChanged([](const bool& enabled) {
            Editor::SceneViewSettings::Get().SetHdrTestPatternEnabled(enabled);
        });
        row->AddChild(std::move(toggle));
        addHdrDependentRow(std::move(row));
    }

    // Gate the HDR Mode / Bit Depth controls to what the active display actually supports.
    // On MoltenVK the driver advertises ST2084/HLG on every surface regardless of the panel
    // (static colorspace enumeration), so IDevice's capability flags filter the spurious modes
    // out — leaving Auto + scRGB. With no device wired we cannot query caps, so fall back to
    // offering every mode (the previous behaviour) rather than silently hiding scRGB.
    //
    // Caps come from the main window's device (its current monitor). On MoltenVK every display
    // resolves the same, so this is exact; on conformant WSI it only diverges if a different
    // Target Monitor with different HDR support is selected. The gate is evaluated once when
    // this content is (re)built — navigating back to the category re-runs it.
    const Rendering::HdrDisplayInfo hdrCaps =
        m_HdrDevice ? m_HdrDevice->GetHdrOutputState().display : Rendering::HdrDisplayInfo{};
    const bool haveHdrCaps = m_HdrDevice != nullptr;
    // No device → offer everything; otherwise gate on the scanned capability.
    const auto modeSupported = [&](bool cap) { return !haveHdrCaps || cap; };
    // HDR10+ rides the same ST2084 colorspace as plain HDR10 PQ (it only layers dynamic metadata
    // on top), so it's available exactly when PQ is — there is no separate scanned capability.
    const bool integerHdrSupported = modeSupported(hdrCaps.supportsHDR10_PQ || hdrCaps.supportsHLG);

    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");
        auto label = std::make_unique<Label>();
        label->SetText("Mode");
        label->AddClass("settings-row-label");
        row->AddChild(std::move(label));

        std::vector<Dropdown::Option> modeOptions;
        modeOptions.push_back(Dropdown::Option{"Auto", "Auto"});
        if (modeSupported(hdrCaps.supportsScRGB))
            modeOptions.push_back(Dropdown::Option{"scRGB", "scRGB / EDR"});
        if (modeSupported(hdrCaps.supportsHDR10_PQ))
            modeOptions.push_back(Dropdown::Option{"HDR10_PQ", "HDR10 PQ / ST.2084"});
        if (modeSupported(hdrCaps.supportsHLG))
            modeOptions.push_back(Dropdown::Option{"HLG", "HLG"});
        if (modeSupported(hdrCaps.supportsHDR10_PQ))
            modeOptions.push_back(Dropdown::Option{"HDR10Plus", "HDR10+ metadata"});

        auto dd = std::make_unique<Dropdown>();
        dd->AddClass("settings-row-field");
        dd->SetOptions(modeOptions, 0);
        // SetSelectedValue is a silent no-op when the saved mode isn't in the offered set, so an
        // unsupported saved mode (e.g. HDR10_PQ on macOS) correctly leaves the selection on Auto.
        dd->SetSelectedValue(Rendering::HdrOutputModeToString(Rendering::HdrOutputModeFromString(hdrMode)));
        dd->SetOnValueChanged([saveHdrSetting, requestHdrOutputRefresh](const std::string& value) {
            saveHdrSetting([value](nlohmann::json& hdr) { hdr["mode"] = value; });
            requestHdrOutputRefresh();
        });
        row->AddChild(std::move(dd));
        addHdrDependentRow(std::move(row));
    }

    // Swapchain bit depth only affects the integer HDR encodings (PQ/HLG/HDR10+). scRGB is always
    // FP16 extended-linear, so on a display that supports no integer HDR colorspace (e.g. macOS EDR)
    // the control is meaningless and is omitted.
    if (integerHdrSupported)
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");
        auto label = std::make_unique<Label>();
        label->SetText("Swapchain Bit Depth");
        label->AddClass("settings-row-label");
        row->AddChild(std::move(label));

        auto dd = std::make_unique<Dropdown>();
        dd->AddClass("settings-row-field");
        dd->SetOptions({
            Dropdown::Option{"10", "Bit Depth 10"},
            Dropdown::Option{"16", "Bit Depth 16"},
        }, 0);
        dd->SetSelectedValue(hdrSwapchainBitDepth == 16 ? "16" : "10");
        dd->SetOnValueChanged([saveHdrSetting, requestHdrOutputRefresh](const std::string& value) {
            const int bitDepth = value == "16" ? 16 : 10;
            saveHdrSetting([bitDepth](nlohmann::json& hdr) { hdr["swapchainBitDepth"] = bitDepth; });
            requestHdrOutputRefresh();
        });
        row->AddChild(std::move(dd));
        addHdrDependentRow(std::move(row));
    }

    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");
        auto label = std::make_unique<Label>();
        label->SetText("Target Monitor");
        label->AddClass("settings-row-label");
        row->AddChild(std::move(label));

        auto dd = std::make_unique<Dropdown>();
        dd->AddClass("settings-row-field");
        std::vector<Dropdown::Option> options;
        options.push_back(Dropdown::Option{"-1", "Active window monitor"});
        options.push_back(Dropdown::Option{"-2", "Primary monitor"});
        for (const Platform::MonitorInfo& monitor : Platform::EnumerateMonitors())
        {
            std::ostringstream labelText;
            labelText << monitor.name << " (" << monitor.width << "x" << monitor.height;
            if (monitor.refreshRate > 0.0)
                labelText << " @" << static_cast<int>(monitor.refreshRate) << " Hz";
            labelText << ")";
            options.push_back(Dropdown::Option{std::to_string(monitor.index), labelText.str()});
        }
        dd->SetOptions(options, 0);
        dd->SetSelectedValue(std::to_string(hdrTargetDisplay));
        dd->SetOnValueChanged([saveHdrSetting, requestHdrOutputRefresh](const std::string& value) {
            int target = -1;
            try { target = std::stoi(value); } catch (...) { target = -1; }
            saveHdrSetting([target](nlohmann::json& hdr) { hdr["targetDisplay"] = target; });
            requestHdrOutputRefresh();
        });
        row->AddChild(std::move(dd));
        addHdrDependentRow(std::move(row));
    }

    // Paper White / Max Luminance default to the values reported by the target display (the same
    // detection that seeds the runtime HDR metadata): paper-white from the OS SDR-content-brightness,
    // max-luminance from the panel's peak. Each has an Override toggle — off uses the reported value
    // with the field disabled; on enables a custom value. "Reset to detected" clears all overrides.
    float detectedPaperWhite = Rendering::HdrStaticMetadata{}.paperWhiteNits;
    float detectedMaxLum = Rendering::HdrStaticMetadata{}.maxMasteringLuminance;
    {
        // Resolve the target monitor the same way the runtime does: a specific index, the active-window
        // monitor (-1, the editor default), or the primary (-2 / fallback). Collapsing -1 to primary
        // would show the wrong display's values when the editor sits on a non-primary HDR screen.
        int targetIndex = hdrTargetDisplay;
        if (hdrTargetDisplay == -1 && m_EditorContext && m_EditorContext->MainWindow)
            targetIndex = m_EditorContext->MainWindow->GetActiveMonitorIndex();
        const std::vector<Platform::MonitorInfo> mons = Platform::EnumerateMonitors();
        const Platform::MonitorInfo* target = nullptr;
        for (const Platform::MonitorInfo& mon : mons)
        {
            if (targetIndex >= 0 ? (mon.index == targetIndex) : mon.primary)
            {
                target = &mon;
                break;
            }
        }
        if (!target && !mons.empty())
            target = &mons.front();
        if (target)
        {
            if (target->sdrWhiteLevelNits > 0.0f)
                detectedPaperWhite = std::clamp(target->sdrWhiteLevelNits,
                                                Rendering::kSdrWhiteToPaperWhiteMinNits,
                                                Rendering::kSdrWhiteToPaperWhiteMaxNits);
            if (target->maxLuminance > 0.0f)
                detectedMaxLum = std::clamp(target->maxLuminance, 100.0f, 10000.0f);
        }
    }

    // One detected/override nit row: label + Override toggle + value field (disabled unless overridden)
    // + units. writeOverride/clearOverride edit the saved JSON for this value.
    auto addHdrOverrideRow = [&](const char* labelText, bool hasOverride, float detectedValue,
                                 float savedValue, const char* undoLabel, float defaultV, float minV,
                                 float maxV, std::function<void(nlohmann::json&, float)> writeOverride,
                                 std::function<void(nlohmann::json&)> clearOverride) {
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");
        auto label = std::make_unique<Label>();
        label->SetText(labelText);
        label->AddClass("settings-row-label");
        Label* labelPtr = label.get();
        row->AddChild(std::move(label));

        auto toggle = std::make_unique<Toggle>();
        toggle->SetChecked(hasOverride);
        toggle->SetOnValueChanged([saveHdrSetting, requestHdrOutputRefresh, rebuildHdrContent,
                                   detectedValue, writeOverride, clearOverride](const bool& on) {
            saveHdrSetting([on, detectedValue, writeOverride, clearOverride](nlohmann::json& hdr) {
                if (on)
                    writeOverride(hdr, detectedValue);
                else
                    clearOverride(hdr);
            });
            requestHdrOutputRefresh();
            rebuildHdrContent();
        });
        row->AddChild(std::move(toggle));

        if (hasOverride)
        {
            // Override on: an editable field for the custom value.
            auto field = std::make_unique<FloatField>();
            field->AddClass("settings-row-value");
            field->AddClass("settings-hdr-number-field");
            FloatField* fieldPtr = field.get();
            field->SetValue(savedValue);
            WireSettingsFloatLabelDrag(
                m_EditorContext, labelPtr, fieldPtr, undoLabel, defaultV, minV, maxV,
                [minV, maxV](float value) { return std::clamp(value, minV, maxV); },
                [saveHdrSetting, requestHdrOutputRefresh, writeOverride, minV, maxV](float value) {
                    const float clamped = std::clamp(value, minV, maxV);
                    saveHdrSetting([clamped, writeOverride](nlohmann::json& hdr) { writeOverride(hdr, clamped); });
                    requestHdrOutputRefresh();
                });
            row->AddChild(std::move(field));
        }
        else
        {
            // Override off: show the detected value as muted read-only text (not an editable field) so
            // it reads clearly as the display-reported value rather than something you can type into.
            auto reported = std::make_unique<Label>();
            reported->SetText(std::to_string(static_cast<int>(detectedValue + 0.5f)));
            reported->AddClass("settings-hdr-value");
            reported->AddClass("settings-hdr-readonly-value");
            row->AddChild(std::move(reported));
        }

        auto units = std::make_unique<Label>();
        units->SetText("nits");
        units->AddClass("settings-hdr-value");
        units->AddClass("settings-hdr-units");
        row->AddChild(std::move(units));

        addHdrDependentRow(std::move(row));
    };

    {
        auto noteRow = std::make_unique<UIElement>();
        noteRow->AddClass("settings-row");
        noteRow->AddClass("settings-hdr-note-row");
        auto spacer = std::make_unique<Label>();
        spacer->SetText("");
        spacer->AddClass("settings-row-label");
        noteRow->AddChild(std::move(spacer));
        auto note = std::make_unique<Label>();
        note->SetText("Off uses the display-reported value.");
        note->AddClass("settings-row-field");
        note->AddClass("settings-hdr-note");
        noteRow->AddChild(std::move(note));
        addHdrDependentRow(std::move(noteRow));
    }

    addHdrOverrideRow("Paper White", paperWhiteHasOverride, detectedPaperWhite, hdrMetadata.paperWhiteNits,
        "Change HDR Paper White", 203.0f, 80.0f, 1000.0f,
        [](nlohmann::json& hdr, float v) {
            auto& m = hdr["metadata"];
            if (!m.is_object()) m = nlohmann::json::object();
            m["paperWhiteNits"] = v;
        },
        [](nlohmann::json& hdr) {
            if (hdr.contains("metadata") && hdr["metadata"].is_object())
                hdr["metadata"].erase("paperWhiteNits");
            hdr.erase("referenceLuminanceNits");
        });

    addHdrOverrideRow("Max Luminance", maxLumHasOverride, detectedMaxLum, hdrMetadata.maxMasteringLuminance,
        "Change HDR Max Luminance", 1000.0f, 100.0f, 10000.0f,
        [](nlohmann::json& hdr, float v) {
            auto& m = hdr["metadata"];
            if (!m.is_object()) m = nlohmann::json::object();
            m["maxMasteringLuminance"] = v;
            m["maxContentLightLevel"] = v;
        },
        [](nlohmann::json& hdr) {
            if (hdr.contains("metadata") && hdr["metadata"].is_object())
            {
                hdr["metadata"].erase("maxMasteringLuminance");
                hdr["metadata"].erase("maxContentLightLevel");
            }
            hdr.erase("maxLuminanceNits");
        });

    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");
        auto spacer = std::make_unique<Label>();
        spacer->SetText(""); // aligns with the label column
        spacer->AddClass("settings-row-label");
        row->AddChild(std::move(spacer));

        auto btn = std::make_unique<Button>();
        btn->SetText("Reset to detected");
        btn->AddClass("small");
        btn->AddClass("secondary");
        btn->RegisterEventHandler(kEventButtonClick, [saveHdrSetting, requestHdrOutputRefresh, rebuildHdrContent](UIEvent&) {
            // Clear all overrides so every value reverts to the reported display values.
            saveHdrSetting([](nlohmann::json& hdr) {
                hdr.erase("metadata");
                hdr.erase("maxLuminanceNits");
                hdr.erase("referenceLuminanceNits");
            });
            requestHdrOutputRefresh();
            rebuildHdrContent();
        });
        row->AddChild(std::move(btn));
        addHdrDependentRow(std::move(row));
    }

    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");
        auto label = std::make_unique<Label>();
        label->SetText("Editor UI White");
        label->AddClass("settings-row-label");
        Label* labelPtr = label.get();
        row->AddChild(std::move(label));

        auto field = std::make_unique<FloatField>();
        field->AddClass("settings-row-value");
        field->AddClass("settings-hdr-number-field");
        FloatField* fieldPtr = field.get();
        // Show the effective UI-white target: an explicit saved override wins,
        // otherwise the value the runtime actually resolved (the OS SDR-white level,
        // or the device paper-white when the OS value is unknown). Without this the
        // field shows a stale default while the editor renders at a different white.
        float displayUiWhite = hdrUiPaperWhiteNits;
        if (!hdrUiPaperWhiteHasOverride)
        {
            const float appliedUiWhite =
                m_UIManager ? m_UIManager->GetRenderRuntimeConfig().HdrUiPaperWhiteNits : 0.0f;
            displayUiWhite = appliedUiWhite > 0.0f ? appliedUiWhite : hdrMetadata.paperWhiteNits;
        }
        field->SetValue(displayUiWhite);
        WireSettingsFloatLabelDrag(
            m_EditorContext,
            labelPtr,
            fieldPtr,
            "Change HDR UI White",
            120.0f,
            40.0f,
            350.0f,
            [](float value) { return std::clamp(value, 40.0f, 350.0f); },
            [saveHdrSetting, applyHdrUiMapping, readHdrFloatSetting](float value) {
                const float uiWhite = std::clamp(value, 40.0f, 350.0f);
                saveHdrSetting([uiWhite](nlohmann::json& hdr) { hdr["uiPaperWhiteNits"] = uiWhite; });
                // Absent = auto (-1): don't turn the other field's auto into an
                // explicit value as a side effect of editing this one.
                const float darkOffset = readHdrFloatSetting("uiBlackLiftNits", -1.0f, -1.0f,
                                                             GameEngine::UIManager::kHdrUiBlackLiftMaxNits);
                applyHdrUiMapping(uiWhite, darkOffset);
            });
        row->AddChild(std::move(field));

        auto units = std::make_unique<Label>();
        units->SetText("nits");
        units->AddClass("settings-hdr-value");
        units->AddClass("settings-hdr-units");
        row->AddChild(std::move(units));

        addHdrDependentRow(std::move(row));
    }

    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");
        auto label = std::make_unique<Label>();
        label->SetText("Editor UI Dark Offset");
        label->AddClass("settings-row-label");
        Label* labelPtr = label.get();
        row->AddChild(std::move(label));

        auto field = std::make_unique<FloatField>();
        field->AddClass("settings-row-value");
        field->AddClass("settings-hdr-number-field");
        FloatField* fieldPtr = field.get();
        // No saved override = auto: show the derived value (the display's black
        // floor — ~0 on OLED) instead of a stale default.
        float displayDarkOffset = hdrUiDarkOffset;
        if (!hdrUiDarkOffsetHasOverride)
            displayDarkOffset = std::clamp(hdrMetadata.minMasteringLuminance,
                                           GameEngine::UIManager::kHdrUiBlackLiftMinNits,
                                           GameEngine::UIManager::kHdrUiBlackLiftMaxNits);
        field->SetValue(displayDarkOffset);
        WireSettingsFloatLabelDrag(
            m_EditorContext,
            labelPtr,
            fieldPtr,
            "Change HDR UI Dark Offset",
            GameEngine::UIManager::kHdrUiBlackLiftMinNits,
            GameEngine::UIManager::kHdrUiBlackLiftMinNits,
            GameEngine::UIManager::kHdrUiBlackLiftMaxNits,
            [](float value) { return std::clamp(value,
                                                 GameEngine::UIManager::kHdrUiBlackLiftMinNits,
                                                 GameEngine::UIManager::kHdrUiBlackLiftMaxNits); },
            [saveHdrSetting, applyHdrUiMapping, readHdrFloatSetting](float value) {
                const float darkOffset = std::clamp(value,
                                                    GameEngine::UIManager::kHdrUiBlackLiftMinNits,
                                                    GameEngine::UIManager::kHdrUiBlackLiftMaxNits);
                saveHdrSetting([darkOffset](nlohmann::json& hdr) { hdr["uiBlackLiftNits"] = darkOffset; });
                // Absent = auto (0): keep UI white tracking the OS SDR white.
                const float uiWhite = readHdrFloatSetting("uiPaperWhiteNits", 0.0f, 0.0f, 350.0f);
                applyHdrUiMapping(uiWhite, darkOffset);
            });
        row->AddChild(std::move(field));

        auto units = std::make_unique<Label>();
        units->SetText("nits");
        units->AddClass("settings-hdr-value");
        units->AddClass("settings-hdr-units");
        row->AddChild(std::move(units));

        addHdrDependentRow(std::move(row));
    }

    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");
        auto spacer = std::make_unique<Label>();
        spacer->SetText(""); // aligns with the label column
        spacer->AddClass("settings-row-label");
        row->AddChild(std::move(spacer));

        auto btn = std::make_unique<Button>();
        btn->SetText("Reset UI mapping to auto");
        btn->AddClass("small");
        btn->AddClass("secondary");
        btn->RegisterEventHandler(kEventButtonClick, [saveHdrSetting, applyHdrUiMapping, rebuildHdrContent](UIEvent&) {
            // Clear both UI overrides: white re-tracks the OS SDR white level,
            // dark offset re-derives from the display's black floor.
            saveHdrSetting([](nlohmann::json& hdr) {
                hdr.erase("uiPaperWhiteNits");
                hdr.erase("uiBlackLiftNits");
            });
            applyHdrUiMapping(0.0f, -1.0f);
            rebuildHdrContent();
        });
        row->AddChild(std::move(btn));
        addHdrDependentRow(std::move(row));
    }

    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");
        row->AddClass("settings-hdr-row");
        auto label = std::make_unique<Label>();
        label->SetText("HDR10 Metadata");
        label->AddClass("settings-row-label");
        row->AddChild(std::move(label));

        auto value = std::make_unique<Label>();
        // Show the effective values (detected unless overridden), matching the fields above.
        const float effPaperWhite = paperWhiteHasOverride ? hdrMetadata.paperWhiteNits : detectedPaperWhite;
        const float effMaxLum = maxLumHasOverride ? hdrMetadata.maxMasteringLuminance : detectedMaxLum;
        std::ostringstream text;
        text << "BT.2020/D65, max " << static_cast<int>(effMaxLum)
             << " nits, min " << hdrMetadata.minMasteringLuminance
             << ", MaxCLL " << static_cast<int>(effMaxLum)
             << ", MaxFALL " << static_cast<int>(hdrMetadata.maxFrameAverageLightLevel)
             << ", paper white " << static_cast<int>(effPaperWhite) << " nits";
        value->SetText(text.str());
        value->AddClass("settings-row-field");
        value->AddClass("settings-hdr-value");
        row->AddChild(std::move(value));
        addHdrDependentRow(std::move(row));
    }

    {
        auto statusHeader = std::make_unique<Label>();
        statusHeader->SetText("Detected HDR Support");
        statusHeader->AddClass("settings-subsection-header");
        m_ContentBody->AddContent(std::move(statusHeader));

        const std::vector<Platform::MonitorInfo> monitors = Platform::EnumerateMonitors();
        if (monitors.empty())
        {
            auto none = std::make_unique<Label>();
            none->SetText("No monitors reported by the platform display layer.");
            none->AddClass("settings-row-field");
            m_ContentBody->AddContent(std::move(none));
        }
        for (const Platform::MonitorInfo& monitor : monitors)
        {
            auto row = std::make_unique<UIElement>();
            row->AddClass("settings-row");
            row->AddClass("settings-hdr-row");
            auto name = std::make_unique<Label>();
            name->SetText(monitor.primary ? monitor.name + " (primary)" : monitor.name);
            name->AddClass("settings-row-label");
            name->AddClass("settings-hdr-monitor-label");
            row->AddChild(std::move(name));

            auto status = std::make_unique<Label>();
            std::ostringstream text;
            text << monitor.width << "x" << monitor.height;
            if (monitor.refreshRate > 0.0)
                text << " @" << static_cast<int>(monitor.refreshRate) << " Hz";
            // DXGI reports luminance only for an output currently in the HDR colour space, so a
            // non-zero peak means the OS is driving this monitor in HDR right now.
            const bool osHdr = monitor.hdrAvailable || monitor.maxLuminance > 0.0f;
            text << " | platform HDR: " << (osHdr ? "active" : "not reported");
            if (monitor.maxLuminance > 0.0f)
                text << " | peak " << static_cast<int>(monitor.maxLuminance) << " nits";
            if (monitor.sdrWhiteLevelNits > 0.0f)
                text << " | SDR white " << static_cast<int>(monitor.sdrWhiteLevelNits) << " nits";
            if (monitor.supportsHDR10_PQ) text << " | HDR10 PQ";
            if (monitor.supportsHLG) text << " | HLG";
            if (monitor.supportsScRGB) text << " | scRGB";
            if (!monitor.diagnosticHints.empty())
                text << " | " << monitor.diagnosticHints.front();
            status->SetText(text.str());
            status->AddClass("settings-row-field");
            status->AddClass("settings-hdr-value");
            row->AddChild(std::move(status));
            m_ContentBody->AddContent(std::move(row));
        }
    }
}

void SettingsPanel::CreatePerformanceSettingsContent()
{
    if (!m_ContentBody)
        return;

    {
        auto header = std::make_unique<Label>();
        header->SetText("Performance");
        header->AddClass("settings-section-header");
        m_ContentBody->AddContent(std::move(header));
    }

    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Graphics API");
        label->AddClass("settings-row-label");
        Label* labelPtr = label.get();
        row->AddChild(std::move(label));

        std::vector<Dropdown::Option> options;
        options.push_back({"auto", "Auto"});
        if (Rendering::DeviceFactory::IsAPISupported(Rendering::GraphicsAPI::Vulkan))
            options.push_back({"vulkan", "Vulkan"});
        if (Rendering::DeviceFactory::IsAPISupported(Rendering::GraphicsAPI::Metal))
            options.push_back({"metal", "Metal"});
        if (Rendering::DeviceFactory::IsAPISupported(Rendering::GraphicsAPI::DirectX12))
            options.push_back({"dx12", "DirectX 12"});

        auto dropdown = std::make_unique<Dropdown>();
        dropdown->AddClass("settings-dropdown");
        dropdown->SetOptions(options, 0);

        std::string selectedApi = "auto";
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.TryGetString("renderer.graphicsApi", selectedApi);
        }
        dropdown->SetSelectedValue(selectedApi);

        auto saveApiPreference = [](const std::string& value)
        {
            auto prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetString("renderer.graphicsApi", value);
            prefs.Save(&err);
        };

        Dropdown* dropdownPtr = dropdown.get();
        dropdown->SetOnValueChanged(saveApiPreference);

        AddDoubleClickReset(labelPtr, [dropdownPtr, saveApiPreference]()
        {
            dropdownPtr->SetSelectedValue("auto");
            saveApiPreference("auto");
        });

        row->AddChild(std::move(dropdown));
        m_ContentBody->AddContent(std::move(row));
    }

    {
        const char* activeApi = "unknown";
        if (auto* renderServices = EngineCore::GetInstance().GetRenderServices())
        {
            if (auto* device = renderServices->GetDevice())
            {
                switch (device->GetAPI())
                {
                    case Rendering::GraphicsAPI::Vulkan: activeApi = "Vulkan"; break;
                    case Rendering::GraphicsAPI::Metal: activeApi = "Metal"; break;
                    case Rendering::GraphicsAPI::DirectX12: activeApi = "DirectX 12"; break;
                    case Rendering::GraphicsAPI::Auto: activeApi = "Auto"; break;
                }
            }
        }
        m_ContentBody->AddContent(MakeSettingsInfoCard(
            std::string("Currently running: ") + activeApi +
            ". Graphics backend used for rendering; takes effect after restarting the editor. "
            "The GE_GFX_API environment variable overrides this setting."));
    }

    {
        SettingsToggleConfig config;
        config.labelText = "VSync";
        config.defaultValue = true;
        config.prefKey = "performance.vsync";
        // Show the vsync the device is actually running (startup config + the
        // GE_VSYNC env override + any runtime toggle), not just the saved
        // preference, so the toggle matches how the editor started.
        if (auto* renderServices = EngineCore::GetInstance().GetRenderServices())
        {
            if (auto* device = renderServices->GetDevice())
                config.initialValueOverride = device->IsVsyncEnabled();
        }
        config.onValueChanged = [this](bool value) {
            if (m_OnVsyncChanged)
                m_OnVsyncChanged(value);
        };
        CreateSettingsToggleRow(config, m_ContentBody);
    }

    {
        m_ContentBody->AddContent(MakeSettingsInfoCard(
            "Synchronize rendering to the display refresh rate. Limits the editor to your "
            "monitor's refresh rate but eliminates screen tearing."));
    }

    // Scene View Render Scale
    {
        using GameEngine::Editor::SceneViewSettings;

        SettingsSliderConfig config;
        config.labelText = "Scene View Render Scale";
        config.defaultValue = SceneViewSettings::Get().GetRenderScale();
        config.minValue = 0.25f;
        config.maxValue = 2.0f; // above 1.0 = supersampling (SSAA)
        config.step = 0.05f;
        config.prefKey.clear(); // Persisted via SceneViewSettings (sceneView.renderScale).
        // Live while dragging (the view controllers republish the override
        // every frame); release commits the pref.
        config.onValueChanging = [](float value) {
            SceneViewSettings::Get().SetRenderScale(value, false);
        };
        config.onValueChanged = [](float value) {
            SceneViewSettings::Get().SetRenderScale(value);
        };
        CreateSettingsSliderRow(config, m_ContentBody);
    }

    {
        m_ContentBody->AddContent(MakeSettingsInfoCard(
            "Editor-side override of the project Render Scale for the Scene and Game views. "
            "1.0 = no override, and the views follow the project setting. Lower values trade "
            "image sharpness for GPU performance (most noticeable on macOS at 2x Retina); above "
            "1.0 supersamples (SSAA). Clustered lighting and depth-min/max compute follow the "
            "scaled extent so lighting stays correct."));
    }
}

void SettingsPanel::NotifyActivePipelineChanged(const std::string& assetRelativePath)
{
    if (!g_ActivePipelineDropdown)
        return;
    if (g_ActivePipelineDropdown->GetSelectedValue() == assetRelativePath)
        return;
    g_SuppressActivePipelineDropdownCallback = true;
    g_ActivePipelineDropdown->SetSelectedValue(assetRelativePath);
    g_SuppressActivePipelineDropdownCallback = false;
}

namespace
{
uint32_t TagHexToArgb(const std::string& hex)
{
    if (hex.size() >= 7 && hex[0] == '#')
    {
        unsigned int r = 0, g = 0, b = 0;
        if (sscanf(hex.c_str() + 1, "%02x%02x%02x", &r, &g, &b) == 3)
            return (0xFFu << 24) | ((r & 0xFFu) << 16) | ((g & 0xFFu) << 8) | (b & 0xFFu);
    }
    return (0xFFu << 24) | (0x80u << 16) | (0x80u << 8) | 0x80u; // gray
}
std::string TagArgbToHex(uint32_t argb)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "#%02x%02x%02x",
             (argb >> 16) & 0xFFu, (argb >> 8) & 0xFFu, argb & 0xFFu);
    return std::string(buf);
}

// Builds the Reassign / Swap / Add alternate / Cancel confirmation overlay
// and parents it to \p host. The overlay removes itself on click or Esc.
// Callbacks fire exactly once; the overlay owns its own lifetime. Pass an
// empty std::function for \p onAddAlternate or \p onSwap to hide the
// corresponding button (used when the conflicting action is hardcoded and
// can't coexist, or when there is no previous binding to trade).
void ShowShortcutConflictModal(UIElement* host,
                               const std::string& title,
                               const std::string& message,
                               const std::string& reassignLabel,
                               std::function<void()> onReassign,
                               std::function<void()> onSwap,
                               std::function<void()> onAddAlternate,
                               std::function<void()> onCancel)
{
    if (!host)
        return;

    auto overlay = std::make_unique<UIElement>();
    UIElement* overlayRaw = overlay.get();
    overlay->SetOverlayLayer(OverlayLayer::BlockingDialog);
    overlay->Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PositionLeft, StyleLength::Px(0.0f))
        .Set(Style::PositionTop, StyleLength::Px(0.0f))
        .Set(Style::Width, StyleLength::Percent(100.0f))
        .Set(Style::Height, StyleLength::Percent(100.0f))
        .Set(Style::ZIndex, 10000)
        .Set(Style::BackgroundColor, (uint32_t)0x99000000)
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::JustifyContent, JustifyContent::Center)
        .Set(Style::PointerEvents, true);

    auto window = std::make_unique<UIElement>();
    window->AddClass("shortcut-conflict-window");
    window->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::BackgroundColor, (uint32_t)0xFF252526)
        .Set(Style::BorderWidth, Box4{1, 1, 1, 1})
        .Set(Style::BorderColor, BorderColorsTRBL{0xFF3A3A3A, 0xFF3A3A3A, 0xFF3A3A3A, 0xFF3A3A3A})
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{10, 10, 10, 10})
        .Set(Style::Width, StyleLength::Px(500.0f))
        .Set(Style::MaxWidth, StyleLength::Percent(85.0f))
        .Set(Style::OverflowProp, Overflow::Hidden);

    // Header
    {
        auto header = std::make_unique<UIElement>();
        header->AddClass("shortcut-conflict-header");
        header->Overrides()
            .Set(Style::PaddingTop, StyleLength::Px(16.0f))
            .Set(Style::PaddingRight, StyleLength::Px(20.0f))
            .Set(Style::PaddingBottom, StyleLength::Px(14.0f))
            .Set(Style::PaddingLeft, StyleLength::Px(20.0f))
            .Set(Style::BackgroundColor, (uint32_t)0xFF1E1E1E)
            .Set(Style::BorderBottomWidth, 1.0f)
            .Set(Style::BorderBottomColor, (uint32_t)0xFF2E2E2E);
        auto titleLabel = std::make_unique<Label>();
        titleLabel->SetText(title);
        titleLabel->Overrides()
            .Set(Style::Color, (uint32_t)0xFFEDEDED)
            .Set(Style::FontSize, StyleLength::Px(16.0f))
            .Set(Style::FontWeight, 600);
        header->AddChild(std::move(titleLabel));
        window->AddChild(std::move(header));
    }

    // Body
    {
        auto content = std::make_unique<UIElement>();
        content->AddClass("shortcut-conflict-body");
        content->Overrides()
            .Set(Style::PaddingTop, StyleLength::Px(20.0f))
            .Set(Style::PaddingRight, StyleLength::Px(20.0f))
            .Set(Style::PaddingBottom, StyleLength::Px(20.0f))
            .Set(Style::PaddingLeft, StyleLength::Px(20.0f));
        auto msg = std::make_unique<Label>();
        msg->SetText(message);
        msg->Overrides()
            .Set(Style::Color, (uint32_t)0xFFCFCFCF)
            .Set(Style::FontSize, StyleLength::Px(13.0f));
        content->AddChild(std::move(msg));
        window->AddChild(std::move(content));
    }

    auto sharedOnReassign = std::make_shared<std::function<void()>>(std::move(onReassign));
    auto sharedOnSwap = std::make_shared<std::function<void()>>(std::move(onSwap));
    auto sharedOnAdd = std::make_shared<std::function<void()>>(std::move(onAddAlternate));
    auto sharedOnCancel = std::make_shared<std::function<void()>>(std::move(onCancel));
    auto fired = std::make_shared<bool>(false);

    auto close = [overlayRaw, fired](const std::shared_ptr<std::function<void()>>& cb)
    {
        if (*fired)
            return;
        *fired = true;
        if (cb && *cb)
            (*cb)();
        if (auto* parent = overlayRaw->GetParent())
            parent->RemoveChild(overlayRaw);
    };

    const bool showAdd = sharedOnAdd && *sharedOnAdd;
    const bool showSwap = sharedOnSwap && *sharedOnSwap;

    // Footer
    {
        auto footer = std::make_unique<UIElement>();
        footer->AddClass("shortcut-conflict-footer");
        footer->Overrides()
            .Set(Style::Display, DisplayMode::Flex)
            .Set(Style::FlexDir, FlexDirection::Row)
            .Set(Style::JustifyContent, JustifyContent::FlexEnd)
            .Set(Style::Gap, StyleLength::Px(8.0f))
            .Set(Style::PaddingTop, StyleLength::Px(14.0f))
            .Set(Style::PaddingRight, StyleLength::Px(20.0f))
            .Set(Style::PaddingBottom, StyleLength::Px(14.0f))
            .Set(Style::PaddingLeft, StyleLength::Px(20.0f))
            .Set(Style::BackgroundColor, (uint32_t)0xFF1E1E1E)
            .Set(Style::BorderTopWidth, 1.0f)
            .Set(Style::BorderTopColor, (uint32_t)0xFF2E2E2E);

        auto cancel = std::make_unique<Button>();
        cancel->SetText("Cancel");
        cancel->AddClass("secondary");
        cancel->RegisterEventHandler(kEventButtonClick, [close, sharedOnCancel](UIEvent&) { close(sharedOnCancel); });
        footer->AddChild(std::move(cancel));

        if (showSwap)
        {
            auto swap = std::make_unique<Button>();
            swap->SetText("Swap");
            swap->AddClass("secondary");
            swap->RegisterEventHandler(kEventButtonClick, [close, sharedOnSwap](UIEvent&) { close(sharedOnSwap); });
            footer->AddChild(std::move(swap));
        }

        if (showAdd)
        {
            auto add = std::make_unique<Button>();
            add->SetText("Add alternate");
            add->AddClass("secondary");
            add->RegisterEventHandler(kEventButtonClick, [close, sharedOnAdd](UIEvent&) { close(sharedOnAdd); });
            footer->AddChild(std::move(add));
        }

        auto reassign = std::make_unique<Button>();
        reassign->SetText(reassignLabel.empty() ? std::string("Reassign") : reassignLabel);
        reassign->AddClass("primary");
        reassign->RegisterEventHandler(kEventButtonClick, [close, sharedOnReassign](UIEvent&) { close(sharedOnReassign); });
        footer->AddChild(std::move(reassign));

        window->AddChild(std::move(footer));
    }

    overlay->RegisterEventHandler(kEventKeyDown, [close, sharedOnReassign, sharedOnCancel](UIEvent& e)
    {
        if (e.Key == Input::kKeyCode_Escape)
        {
            e.Handled = true;
            close(sharedOnCancel);
        }
        else if (e.Key == Input::kKeyCode_Enter)
        {
            e.Handled = true;
            close(sharedOnReassign);
        }
    });

    overlay->AddChild(std::move(window));
    host->AddChild(std::move(overlay));
}

// Small Button subclass that captures the next key event when armed and
// reports the captured binding via a callback. Clicking the button arms it;
// the next non-modifier key press fires the callback and disarms.
class ShortcutCaptureButton : public Button
{
  public:
    using CaptureHandler = std::function<void(int key, int mods)>;

    // Disarm on destruction: the armed state increments a global suppression
    // counter, and the row can be destroyed while listening (switching the
    // Settings category rebuilds the content). Leaking the increment would
    // suppress every editor shortcut until restart.
    ~ShortcutCaptureButton() override { Disarm(); }

    void SetOnCapture(CaptureHandler h) { m_OnCapture = std::move(h); }
    bool IsArmed() const { return m_Armed; }

    void Arm()
    {
        if (!m_Armed)
            Editor::SetShortcutCaptureActive(true);
        m_Armed = true;
        AddClass("listening");
        if (auto* mgr = GetOwnerManager())
            mgr->FocusElement(this);
    }

    void Disarm()
    {
        if (m_Armed)
            Editor::SetShortcutCaptureActive(false);
        m_Armed = false;
        RemoveClass("listening");
    }

    void OnEvent(UIEvent& e) override
    {
        if (m_Armed && e.Id == kEventKeyDown)
        {
            if (e.Key == Input::kKeyCode_Escape)
            {
                Disarm();
                e.Stop();
                return;
            }
            if (Editor::IsModifierKey(e.Key))
            {
                // Still composing — wait for the non-modifier key.
                e.Stop();
                return;
            }
            const int key = e.Key;
            const int mods = e.Mods & Input::kModShortcutMask;
            // At most two modifiers + one key (e.g. Shift+Cmd+S). Extra
            // modifiers are ignored so the row stays listening.
            if (Editor::CountShortcutModifiers(mods) > 2)
            {
                e.Stop();
                return;
            }
            Editor::NoteShortcutCaptureConsumedKey();
            Disarm();
            if (m_OnCapture)
                m_OnCapture(key, mods);
            e.Stop();
            return;
        }
        if (m_Armed && e.Id == kEventScroll && std::fabs(e.ScrollY) >= 1e-3f)
        {
            const int mods = e.Mods & Input::kModShortcutMask;
            if (Editor::CountShortcutModifiers(mods) > 2)
            {
                e.Stop();
                return;
            }
            Editor::NoteShortcutCaptureConsumedKey();
            Disarm();
            if (m_OnCapture)
                m_OnCapture(Editor::kShortcutKeyMouseScroll, mods);
            e.Stop();
            return;
        }
        if (m_Armed && e.Id == kEventFocusOut)
        {
            Disarm();
        }
        Button::OnEvent(e);
    }

  private:
    bool m_Armed = false;
    CaptureHandler m_OnCapture;
};

} // namespace

void SettingsPanel::CreateTagsSettingsContent()
{
    if (!m_ContentBody)
        return;

    const bool hasProject = !EngineCore::GetInstance().GetWorkspaceRoot().empty();

    auto container = std::make_unique<UIElement>();
    container->Overrides().Set(Style::MarginLeft, StyleLength::Px(16.0f));

    // Section header with right-click context menu (Reset to defaults)
    {
        auto sectionHeader = std::make_unique<Label>();
        sectionHeader->SetText("Project Tags");
        sectionHeader->AddClass("settings-section-header");
        sectionHeader->SetFocusable(true);
        constexpr uint32_t kCmdResetTagsToDefaults = 1;
        sectionHeader->RegisterEventHandler(kEventMouseDown, [this, kCmdResetTagsToDefaults](UIEvent& e) {
            if (e.Button != 1 || !m_EditorContext || !m_EditorContext->MainWindow)
                return;
            e.Stop();
            auto menu = CreateContextMenu();
            if (!menu)
                return;
            menu->SetCommandHandler([this](uint32_t cmd) {
                if (cmd != 1u)
                    return;
                SettingsPanel* panel = this;
                PostAction([panel]() {
                    const std::vector<EditorTagDefinition> defaults = EditorTags::GetDefaults();
                    if (!EditorTags::Save(defaults))
                        return;
                    panel->ShowCategoryContent(SettingsCategory::Tags);

                    panel->PostAction([panel, defaults]() {
                        std::unordered_set<std::string> defaultNamesLower;
                        for (const auto& d : defaults) {
                            std::string lower;
                            lower.reserve(d.Name.size());
                            for (unsigned char c : d.Name)
                                lower.push_back(static_cast<char>(std::tolower(c)));
                            defaultNamesLower.insert(std::move(lower));
                        }
                        std::vector<std::filesystem::path> paths =
                            EngineCore::GetInstance().GetAssetManager().GetRegistry().GetRegisteredAssetPaths();
                        const size_t batchSize = 25u;
                        auto nextIndex = std::make_shared<size_t>(0u);
                        auto processBatchRef = std::make_shared<std::function<void()>>();
                        *processBatchRef = [processBatchRef, panel, paths = std::move(paths),
                                           defaultNamesLower = std::move(defaultNamesLower),
                                           nextIndex, batchSize]() {
                            AssetRegistry& registry = EngineCore::GetInstance().GetAssetManager().GetRegistry();
                            const size_t start = *nextIndex;
                            const size_t end = (std::min)(start + batchSize, paths.size());
                            for (size_t i = start; i < end; ++i) {
                                const std::filesystem::path& path = paths[i];
                                std::string meta;
                                if (!registry.TryGetMetaValue(path, "tags", meta) || meta.empty())
                                    continue;
                                std::vector<std::string> kept;
                                for (size_t j = 0; j < meta.size(); ) {
                                    size_t k = meta.find(',', j);
                                    if (k == std::string::npos) k = meta.size();
                                    std::string part = meta.substr(j, k - j);
                                    const size_t s = part.find_first_not_of(" \t");
                                    if (s != std::string::npos) {
                                        size_t e2 = part.find_last_not_of(" \t");
                                        part = part.substr(s, (e2 == std::string::npos ? part.size() : e2 + 1) - s);
                                    }
                                    if (!part.empty()) {
                                        std::string lower;
                                        lower.reserve(part.size());
                                        for (unsigned char c : part)
                                            lower.push_back(static_cast<char>(std::tolower(c)));
                                        if (defaultNamesLower.count(lower))
                                            kept.push_back(part);
                                    }
                                    j = k + (k < meta.size() ? 1u : 0u);
                                }
                                std::string newMeta;
                                for (size_t k = 0; k < kept.size(); ++k) {
                                    if (k) newMeta += ',';
                                    newMeta += kept[k];
                                }
                                registry.SetMetaValue(path, "tags", newMeta);
                            }
                            *nextIndex = end;
                            if (*nextIndex < paths.size())
                                panel->PostAction(*processBatchRef);
                        };
                        panel->PostAction(*processBatchRef);
                    });
                });
            });
            ContextMenuBuilder builder;
            builder.AddItem("Reset to defaults", kCmdResetTagsToDefaults, MenuItemFlag_None, 0, EditorIcons::kReset);
            builder.Build(menu.get());
            menu->Show(m_EditorContext->MainWindow, static_cast<int>(e.X), static_cast<int>(e.Y));
        });
        container->AddChild(std::move(sectionHeader));
    }

    // Description
    {
        auto desc = MakeSettingsInfoCard(
            "Tags are used in the Assets panel to label assets.\n"
            "Each project has its own tag list.");
        desc->AddClass("settings-tags-description");
        container->AddChild(std::move(desc));
    }

    std::vector<EditorTagDefinition> tags = EditorTags::Load();

    for (size_t i = 0; i < tags.size(); ++i)
    {
        const size_t tagIndex = i;
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");
        row->AddClass("settings-tag-row");
        row->Overrides()
            .Set(Style::Display, DisplayMode::Flex)
            .Set(Style::FlexDir, FlexDirection::Row)
            .Set(Style::AlignItems, AlignItems::Center)
            .Set(Style::Gap, StyleLength::Px(8.0f));

        auto swatch = std::make_unique<UIElement>();
        std::string bgColor = tags[tagIndex].Color.empty() ? "#444" : tags[tagIndex].Color;
        const uint32_t swatchArgb = TagHexToArgb(bgColor);
        swatch->Overrides()
            .Set(Style::Width, StyleLength::Px(20.0f)).Set(Style::Height, StyleLength::Px(20.0f)).Set(Style::MinWidth, StyleLength::Px(20.0f)).Set(Style::MinHeight, StyleLength::Px(20.0f))
            .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{4.0f, 4.0f, 4.0f, 4.0f}).Set(Style::BorderWidth, Box4{1.0f, 1.0f, 1.0f, 1.0f}).Set(Style::BorderColor, BorderColorsTRBL{0xFF555555u, 0xFF555555u, 0xFF555555u, 0xFF555555u})
            .Set(Style::BackgroundColor, (uint32_t)swatchArgb).Set(Style::FlexShrink, 0.0f).Set(Style::Cursor, CursorStyle::Pointer);
        UIElement* swatchPtr = swatch.get();

        swatch->RegisterEventHandler(kEventMouseDown, [this, tagIndex, swatchPtr, hasProject](UIEvent& e) {
            if (e.Button != 0 || !hasProject) return;
            std::vector<EditorTagDefinition> current = EditorTags::Load();
            if (tagIndex >= current.size()) return;
            uint32_t argb = TagHexToArgb(current[tagIndex].Color.empty() ? "#95a5a6" : current[tagIndex].Color);
            OpenColorPicker(
                argb,
                [this, tagIndex](uint32_t newArgb)
                {
                    std::string hexColor = TagArgbToHex(newArgb);
                    this->PostAction([this, tagIndex, hexColor]()
                    {
                        std::vector<EditorTagDefinition> t = EditorTags::Load();
                        if (tagIndex < t.size())
                        {
                            t[tagIndex].Color = hexColor;
                            EditorTags::Save(t);
                            ShowCategoryContent(SettingsCategory::Tags);
                        }
                    });
                },
                []() {});
            e.Stop();
        });
        row->AddChild(std::move(swatch));

        auto nameField = std::make_unique<TextField>();
        nameField->SetValue(tags[tagIndex].Name);
        nameField->AddClass("settings-row-field");
        nameField->AddClass("settings-tag-name-field");
        nameField->SetOnValueChanged([this, tagIndex, hasProject](const std::string& newName) {
            if (!hasProject || newName.empty()) return;
            std::string nameCopy = newName;
            this->PostAction([this, tagIndex, nameCopy]() {
                std::vector<EditorTagDefinition> t = EditorTags::Load();
                if (tagIndex < t.size()) {
                    t[tagIndex].Name = nameCopy;
                    EditorTags::Save(t);
                }
            });
        });
        row->AddChild(std::move(nameField));

        auto delBtn = std::make_unique<Button>();
        delBtn->SetText("");
        delBtn->AddClass("small");
        delBtn->AddClass("secondary");
        delBtn->AddClass("icon-button");
        delBtn->AddClass("trash-icon");
        delBtn->Overrides().Set(Style::FlexShrink, 0.0f);
        delBtn->RegisterEventHandler(kEventButtonClick, [this, tagIndex, hasProject](UIEvent&) {
            if (!hasProject) return;
            this->PostAction([this, tagIndex]() {
                std::vector<EditorTagDefinition> t = EditorTags::Load();
                if (tagIndex >= t.size()) return;
                EditorTagDefinition removedTag = t[tagIndex];
                auto refreshTags = [this]() { ShowCategoryContent(SettingsCategory::Tags); };
                auto getPathsWithTag = [](const std::string& tagName) -> std::vector<std::filesystem::path> {
                    std::vector<std::filesystem::path> out;
                    if (tagName.empty()) return out;
                    auto& reg = EngineCore::GetInstance().GetAssetManager().GetRegistry();
                    const std::vector<std::filesystem::path> paths = reg.GetRegisteredAssetPaths();
                    for (const auto& p : paths) {
                        std::string existing;
                        if (!reg.TryGetMetaValue(p, "tags", existing)) continue;
                        for (size_t i = 0; i < existing.size(); ) {
                            size_t j = existing.find(',', i);
                            if (j == std::string::npos) j = existing.size();
                            std::string part = existing.substr(i, j - i);
                            const size_t s = part.find_first_not_of(" \t\r\n");
                            if (s != std::string::npos) {
                                size_t e = part.find_last_not_of(" \t\r\n");
                                part = part.substr(s, e == std::string::npos ? part.size() - s : e - s + 1);
                            } else part.clear();
                            if (part == tagName) { out.push_back(p); break; }
                            i = j + (j < existing.size() ? 1 : 0);
                        }
                    }
                    return out;
                };
                auto stripTagFromAllAssets = [](const std::string& tagName) {
                    if (tagName.empty()) return;
                    auto& reg = EngineCore::GetInstance().GetAssetManager().GetRegistry();
                    const std::vector<std::filesystem::path> paths = reg.GetRegisteredAssetPaths();
                    for (const auto& p : paths) {
                        std::string existing;
                        if (!reg.TryGetMetaValue(p, "tags", existing)) continue;
                        std::vector<std::string> parts;
                        for (size_t i = 0; i < existing.size(); ) {
                            size_t j = existing.find(',', i);
                            if (j == std::string::npos) j = existing.size();
                            std::string part = existing.substr(i, j - i);
                            const size_t s = part.find_first_not_of(" \t\r\n");
                            if (s != std::string::npos) {
                                size_t e = part.find_last_not_of(" \t\r\n");
                                part = part.substr(s, e == std::string::npos ? part.size() - s : e - s + 1);
                            } else part.clear();
                            if (!part.empty() && part != tagName && std::find(parts.begin(), parts.end(), part) == parts.end())
                                parts.push_back(part);
                            i = j + (j < existing.size() ? 1 : 0);
                        }
                        std::string combined;
                        for (size_t i = 0; i < parts.size(); ++i)
                            combined += (i ? "," : "") + parts[i];
                        if (combined != existing)
                            reg.SetMetaValue(p, "tags", combined);
                    }
                };
                auto restoreTagToPaths = [](const std::string& tagName, const std::vector<std::filesystem::path>& pathsToRestore) {
                    if (tagName.empty() || pathsToRestore.empty()) return;
                    auto& reg = EngineCore::GetInstance().GetAssetManager().GetRegistry();
                    for (const auto& p : pathsToRestore) {
                        std::string existing;
                        reg.TryGetMetaValue(p, "tags", existing);
                        std::vector<std::string> parts;
                        for (size_t i = 0; i < existing.size(); ) {
                            size_t j = existing.find(',', i);
                            if (j == std::string::npos) j = existing.size();
                            std::string part = existing.substr(i, j - i);
                            const size_t s = part.find_first_not_of(" \t\r\n");
                            if (s != std::string::npos) {
                                size_t e = part.find_last_not_of(" \t\r\n");
                                part = part.substr(s, e == std::string::npos ? part.size() - s : e - s + 1);
                            } else part.clear();
                            if (!part.empty() && std::find(parts.begin(), parts.end(), part) == parts.end())
                                parts.push_back(part);
                            i = j + (j < existing.size() ? 1 : 0);
                        }
                        if (std::find(parts.begin(), parts.end(), tagName) == parts.end())
                            parts.push_back(tagName);
                        std::string combined;
                        for (size_t i = 0; i < parts.size(); ++i)
                            combined += (i ? "," : "") + parts[i];
                        reg.SetMetaValue(p, "tags", combined);
                    }
                };
                if (m_EditorContext && m_EditorContext->UndoRedo)
                {
                    m_EditorContext->UndoRedo->Execute(
                        std::make_unique<Editor::RemoveTagCommand>(removedTag, tagIndex, refreshTags,
                            stripTagFromAllAssets, getPathsWithTag, restoreTagToPaths));
                    ShowCategoryContent(SettingsCategory::Tags);
                }
                else
                {
                    t.erase(t.begin() + static_cast<std::ptrdiff_t>(tagIndex));
                    EditorTags::Save(t);
                    stripTagFromAllAssets(removedTag.Name);
                    refreshTags();
                }
            });
        });
        row->AddChild(std::move(delBtn));

        container->AddChild(std::move(row));
    }

    // Add tag row
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");
        row->Overrides()
            .Set(Style::Display, DisplayMode::Flex).Set(Style::FlexDir, FlexDirection::Row)
            .Set(Style::AlignItems, AlignItems::Center).Set(Style::Gap, StyleLength::Px(8.0f)).Set(Style::MarginTop, StyleLength::Px(12.0f));

        auto newNameField = std::make_unique<TextField>();
        newNameField->SetValue("");
        newNameField->AddClass("settings-row-field");
        newNameField->AddClass("settings-tag-name-field");
        TextField* newNamePtr = newNameField.get();
        row->AddChild(std::move(newNameField));

        auto addBtn = std::make_unique<Button>();
        addBtn->SetText("Add");
        addBtn->AddClass("small");
        addBtn->AddClass("secondary");
        addBtn->RegisterEventHandler(kEventButtonClick, [this, newNamePtr, hasProject](UIEvent&) {
            if (!hasProject || !newNamePtr) return;
            std::string name = newNamePtr->GetValue();
            while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
                name.pop_back();
            for (size_t i = 0; i < name.size(); )
                if (name[i] == ' ' || name[i] == '\t') name.erase(i, 1);
                else ++i;
            if (name.empty()) return;
            newNamePtr->SetValue("");
            std::string nameCopy = name;
            this->PostAction([this, nameCopy]() {
                std::vector<EditorTagDefinition> t = EditorTags::Load();
                t.push_back(EditorTagDefinition{nameCopy, "#95a5a6"});
                if (EditorTags::Save(t))
                    ShowCategoryContent(SettingsCategory::Tags);
            });
        });
        row->AddChild(std::move(addBtn));
        container->AddChild(std::move(row));
    }

    if (!hasProject)
    {
        auto noProject = MakeSettingsInfoCard("Open a project to add or edit tags.");
        noProject->Overrides().Set(Style::Color, (uint32_t)0xFF888888u).Set(Style::MarginTop, StyleLength::Px(8.0f));
        container->AddChild(std::move(noProject));
    }

    m_ContentBody->AddContent(std::move(container));
}

void SettingsPanel::CreateVersionControlContent()
{
    if (!m_ContentBody)
        return;

    // Provider-neutral pages registered against the Version Control group
    // render here, on the page that covers every provider, rather than as
    // per-page tree children.
    for (const auto& category : Editor::EditorSettingsRegistry::Get().Snapshot())
        if (category.Group == Editor::SettingsCategoryGroup::VersionControl)
            CreateRegistrySettingsContent(category);

    auto description = MakeSettingsInfoCard("Select a version control system to configure:");
    description->AddClass("settings-overview-text");
    m_ContentBody->AddContent(std::move(description));

    // Registered providers with their detection state (registry-driven; the
    // list includes providers contributed by package modules).
    const auto providers = Editor::EditorVcsProviderRegistry::Get().Snapshot();
    const std::string activeTypeId = Editor::EditorVcsProviderRegistry::Get().ActiveTypeId();
    for (const auto& provider : providers)
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");

        auto name = std::make_unique<Label>();
        name->SetText(provider.DisplayName);
        name->AddClass("settings-row-label");
        row->AddChild(std::move(name));

        auto state = std::make_unique<Label>();
        state->AddClass("settings-row-value");
        state->SetText(provider.TypeId == activeTypeId ? "Active" : "Registered");
        row->AddChild(std::move(state));

        m_ContentBody->AddContent(std::move(row));
    }
}

void SettingsPanel::CreateVcsProviderContent(const std::string& providerTypeId)
{
    if (!m_ContentBody)
        return;

    Editor::EditorVcsProviderDescriptor provider;
    if (!Editor::EditorVcsProviderRegistry::Get().TryGet(providerTypeId, provider))
        return;
    if (provider.BuildSettingsContent)
        provider.BuildSettingsContent(*m_ContentBody);
}

void SettingsPanel::CreateScriptSettingsContent()
{
    if (!m_ContentBody)
        return;
    
    // ========================================================================
    // Script Editor Section
    // ========================================================================
    {
        auto sectionHeader = std::make_unique<Label>();
        sectionHeader->SetText("Script Editor");
        sectionHeader->AddClass("settings-section-header");
        m_ContentBody->AddContent(std::move(sectionHeader));
    }
    
    // Double-click behavior toggle: Script Inspector vs IDE
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");
        
        auto toggle = std::make_unique<Toggle>();
        m_ScriptOpenInInspectorToggle = toggle.get();
        toggle->AddClass("settings-toggle");
        toggle->SetChecked(Editor::ScriptEditorSettings::Get().GetOpenInScriptInspector());
        
        // Hook up callback to update settings
        toggle->SetOnValueChanged([](const bool& openInInspector) {
            Editor::ScriptEditorSettings::Get().SetOpenInScriptInspector(openInInspector);
        });
        
        section->AddChild(std::move(toggle));
        
        auto label = std::make_unique<Label>();
        label->SetText("Open in Script Inspector");
        label->SetTooltip("Double-click opens in Script Inspector");
        label->AddClass("settings-row-label");
        section->InsertChild(0, std::move(label));
        
        m_ContentBody->AddContent(std::move(section));
    }
    
    // Description label
    {
        m_ContentBody->AddContent(MakeSettingsInfoCard(
            "When disabled, double-clicking a script or log error opens in your default IDE."));
    }

    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");
        section->AddClass("settings-toggle-row");

        auto toggle = std::make_unique<Toggle>();
        m_ShaderGraphGlslOpenToggle = toggle.get();
        toggle->AddClass("settings-toggle");
        toggle->SetChecked(Editor::GetOpenShaderGraphGlslInMaterialGraph());

        toggle->SetOnValueChanged([](const bool& openInGraph) {
            Editor::SetOpenShaderGraphGlslInMaterialGraph(openInGraph);
        });

        section->AddChild(std::move(toggle));

        auto label = std::make_unique<Label>();
        label->SetText(".glsl Opens Material Graph");
        label->SetTooltip("Open shader graph .glsl in Material Graph");
        label->AddClass("settings-row-label");
        section->InsertChild(0, std::move(label));

        m_ContentBody->AddContent(std::move(section));
    }

    {
        m_ContentBody->AddContent(MakeSettingsInfoCard(
            "When disabled, material graph .glsl files (with @sg-graph tags) open in the Script "
            "Editor instead. Other .glsl files always use the Script Editor."));
    }

    // Line height slider
    {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText("Line Height");
        label->AddClass("settings-row-label");
        Label* labelPtr = label.get();
        section->AddChild(std::move(label));

        auto slider = std::make_unique<Slider>();
        m_ScriptLineHeightSlider = slider.get();
        slider->AddClass("settings-row-slider");
        slider->SetTrackPaddingPx(0.0f);
        slider->SetShowValueBubble(true);
        slider->SetMin(0.5f);
        slider->SetMax(4.0f);
        slider->SetStep(0.05f);

        float lineHeight = Editor::ScriptEditorSettings::Get().GetLineHeight();
        slider->SetValue(lineHeight);

        section->AddChild(std::move(slider));

        auto valueField = std::make_unique<FloatField>();
        valueField->AddClass("settings-row-value");
        InspectorDrag::ApplySliderFloatValueFieldStyle(valueField.get());
        valueField->SetValue(lineHeight);
        FloatField* valueFieldPtr = valueField.get();

        auto applyLineHeight = [this](float value) {
            Editor::ScriptEditorSettings::Get().SetLineHeight(value);
            ApplySavedScriptLineHeightStyle(GetOwnerManager());
        };

        m_ScriptLineHeightSlider->SetOnValueChanging([valueFieldPtr, applyLineHeight](const float& value) {
            valueFieldPtr->SetValue(value);
            applyLineHeight(value);
        });

        m_ScriptLineHeightSlider->SetOnValueChanged([valueFieldPtr, applyLineHeight](const float& value) {
            valueFieldPtr->SetValue(value);
            applyLineHeight(value);
        });

        valueField->SetOnValueChanged([this, applyLineHeight](const float& value) {
            float clamped = std::clamp(value, 0.5f, 4.0f);
            if (m_ScriptLineHeightSlider)
                m_ScriptLineHeightSlider->SetValue(clamped);
            applyLineHeight(clamped);
        });

        section->AddChild(std::move(valueField));
        m_ContentBody->AddContent(std::move(section));

        InspectorDrag::SetupLabelDragSlider(
            labelPtr, m_ScriptLineHeightSlider, nullptr, nullptr, 1.0f);
    }

    // ========================================================================
    // Syntax Highlighting Colors Section
    // ========================================================================
    {
        auto sectionHeader = std::make_unique<Label>();
        sectionHeader->SetText("Syntax Highlighting");
        sectionHeader->AddClass("settings-section-header");
        m_ContentBody->AddContent(std::move(sectionHeader));
    }
    
    // Helper: update hex text field from ARGB
    auto setHexField = [](TextField* f, uint32_t argb) {
        if (!f) return;
        char buf[16] = {0};
        std::snprintf(buf, sizeof(buf), "#%06X", (unsigned int)(argb & 0xFFFFFFu));
        f->SetValue(buf);
    };

    // Helper to create a color setting row: label + clickable swatch (opens color picker) + hex field
    auto createColorRow = [this, setHexField](const char* labelText, TextField*& fieldPtr, uint32_t* settingPtr,
                                               uint32_t defaultArgb) {
        auto section = std::make_unique<UIElement>();
        section->AddClass("settings-row");

        auto label = std::make_unique<Label>();
        label->SetText(labelText);
        label->AddClass("settings-row-label");
        Label* labelPtr = label.get();
        section->AddChild(std::move(label));

        auto swatch = std::make_unique<UIElement>();
        swatch->AddClass("settings-color-swatch");
        UIElement* swatchPtr = swatch.get();
        swatch->Overrides()
            .Set(Style::Width, StyleLength::Px(16.0f))
            .Set(Style::Height, StyleLength::Px(16.0f))
            .Set(Style::FlexShrink, 0.0f)
            .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{2.0f, 2.0f, 2.0f, 2.0f})
            .Set(Style::BorderWidth, Box4{1.0f, 1.0f, 1.0f, 1.0f})
            .Set(Style::BorderColor, BorderColorsTRBL{0xFF3E3E3Eu, 0xFF3E3E3Eu, 0xFF3E3E3Eu, 0xFF3E3E3Eu})
            .Set(Style::BackgroundColor, *settingPtr)
            .Set(Style::Cursor, CursorStyle::Pointer);
        auto field = std::make_unique<TextField>();
        fieldPtr = field.get();
        TextField* fieldRaw = fieldPtr;
        swatch->RegisterEventHandler(kEventMouseDown, [this, settingPtr, swatchPtr, fieldRaw, setHexField](UIEvent& e) {
            if (e.Button != 0) return;
            const uint32_t original = *settingPtr;
            OpenColorPicker(
                original,
                [settingPtr, swatchPtr, fieldRaw, setHexField](uint32_t argb) {
                    *settingPtr = argb;
                    SyntaxHighlightSettings::Get().Save();
                    if (swatchPtr)
                        swatchPtr->Overrides().Set(Style::BackgroundColor, argb);
                    setHexField(fieldRaw, argb);
                },
                [settingPtr, swatchPtr, original, fieldRaw, setHexField]() {
                    *settingPtr = original;
                    if (swatchPtr)
                        swatchPtr->Overrides().Set(Style::BackgroundColor, original);
                    setHexField(fieldRaw, original);
                },
                [swatchPtr, fieldRaw, setHexField](uint32_t argb) {
                    if (swatchPtr)
                        swatchPtr->Overrides().Set(Style::BackgroundColor, argb);
                    setHexField(fieldRaw, argb);
                });
            e.Stop();
        });
        section->AddChild(std::move(swatch));
        field->AddClass("settings-row-field");
        field->AddClass("settings-color-field");
        setHexField(fieldPtr, *settingPtr);
        field->SetOnValueChanged([settingPtr, swatchPtr](const std::string& value) {
            std::string hex = value;
            if (!hex.empty() && hex[0] == '#') hex = hex.substr(1);
            if (hex.length() == 6) {
                try {
                    uint32_t rgb = static_cast<uint32_t>(std::stoul(hex, nullptr, 16));
                    *settingPtr = 0xFF000000u | rgb;
                    SyntaxHighlightSettings::Get().Save();
                    if (swatchPtr)
                        swatchPtr->Overrides().Set(Style::BackgroundColor, *settingPtr);
                } catch (...) {}
            }
        });
        section->AddChild(std::move(field));

        AddDoubleClickReset(labelPtr, [settingPtr, swatchPtr, defaultArgb, fieldRaw, setHexField]() {
            *settingPtr = defaultArgb;
            SyntaxHighlightSettings::Get().Save();
            if (swatchPtr)
                swatchPtr->Overrides().Set(Style::BackgroundColor, defaultArgb);
            setHexField(fieldRaw, defaultArgb);
        });
        return section;
    };

    auto& syntaxSettings = SyntaxHighlightSettings::Get();
    constexpr uint32_t kDefaultText = 0xFF999999u;
    constexpr uint32_t kDefaultKeyword = 0xFFBFBFBFu;
    constexpr uint32_t kDefaultString = 0xFF7ED321u;
    constexpr uint32_t kDefaultComment = 0xFFE5A54Bu;
    constexpr uint32_t kDefaultNumber = 0xFFB87FE8u;
    constexpr uint32_t kDefaultType = 0xFF5DADE2u;

    m_ContentBody->AddContent(createColorRow("Default Text", m_SyntaxDefaultColorField, &syntaxSettings.DefaultColor, kDefaultText));
    m_ContentBody->AddContent(createColorRow("Keywords", m_SyntaxKeywordColorField, &syntaxSettings.KeywordColor, kDefaultKeyword));
    m_ContentBody->AddContent(createColorRow("Strings", m_SyntaxStringColorField, &syntaxSettings.StringColor, kDefaultString));
    m_ContentBody->AddContent(createColorRow("Comments", m_SyntaxCommentColorField, &syntaxSettings.CommentColor, kDefaultComment));
    m_ContentBody->AddContent(createColorRow("Numbers", m_SyntaxNumberColorField, &syntaxSettings.NumberColor, kDefaultNumber));
    m_ContentBody->AddContent(createColorRow("Types", m_SyntaxTypeColorField, &syntaxSettings.TypeColor, kDefaultType));
}

void SettingsPanel::CreateShortcutsSettingsContent()
{
    if (!m_ContentBody)
        return;

    using Editor::ApplyShortcutBindings;
    using Editor::FindShortcutConflict;
    using Editor::FormatShortcutBinding;
    using Editor::GetShortcutCatalog;
    using Editor::LoadShortcutBindings;
    using Editor::SaveShortcutBindings;
    using Editor::ShortcutBinding;
    using Editor::ShortcutCatalogEntry;

    auto header = std::make_unique<Label>();
    header->AddClass("settings-placeholder");
    header->SetText(
        "Click a shortcut to rebind (up to two modifiers + one key). "
        "Press Esc to cancel. For Scroll bindings, click then scroll the wheel.");
    m_ContentBody->AddContent(std::move(header));

    Input::InputSystem* input = nullptr;
    if (auto* engine = &EngineCore::GetInstance())
        input = engine->GetInputSystem();

    auto prefs = Editor::OpenEditorPreferences();
    prefs.Load(nullptr);

    // Shortcut rows use the same vertical rhythm as Inspector properties,
    // independently of the broader Settings page spacing preference.
    double shortcutRowGap = 4.0;
    prefs.TryGetDouble("ui.inspectorRowGap", shortcutRowGap);
    shortcutRowGap = std::clamp(shortcutRowGap, 0.0, 16.0);

    const auto& catalog = GetShortcutCatalog();

    // Lowercased search query — when the search bar has a value and the
    // Shortcuts page is active, filter rows in place. Matches against
    // group label, display name, and the formatted binding (so the user can
    // search by binding text like "ctrl+s").
    std::string filter = m_CurrentSearchQuery;
    for (char& c : filter) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    auto entryMatchesFilter = [&](const ShortcutCatalogEntry& e) -> bool
    {
        if (filter.empty())
            return true;
        auto contains = [&](std::string s)
        {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s.find(filter) != std::string::npos;
        };
        if (contains(e.displayName))
            return true;
        if (contains(e.groupLabel))
            return true;
        if (e.readOnly)
        {
            if (contains(FormatShortcutBinding(e.defaultKey, e.defaultMods)))
                return true;
        }
        else
        {
            for (const auto& c : LoadShortcutBindings(prefs, e))
            {
                if (contains(FormatShortcutBinding(c.key, c.mods)))
                    return true;
            }
        }
        return false;
    };

    // Group catalog entries by group label, preserving the order in which
    // groups first appear. Filtered-out entries are skipped entirely, and
    // groups with no remaining entries collapse out of the UI.
    std::vector<std::string> groupOrder;
    std::unordered_map<std::string, std::vector<const ShortcutCatalogEntry*>> byGroup;
    for (const auto& e : catalog)
    {
        if (!entryMatchesFilter(e))
            continue;
        const std::string g = e.groupLabel;
        if (byGroup.find(g) == byGroup.end())
            groupOrder.push_back(g);
        byGroup[g].push_back(&e);
    }

    if (groupOrder.empty() && !filter.empty())
    {
        auto none = std::make_unique<Label>();
        none->AddClass("settings-placeholder");
        none->SetText("No shortcuts match the search.");
        m_ContentBody->AddContent(std::move(none));
        return;
    }

    for (const std::string& group : groupOrder)
    {
        auto groupLabel = std::make_unique<Label>();
        groupLabel->AddClass("settings-section-header");
        groupLabel->SetText(group);
        m_ContentBody->AddContent(std::move(groupLabel));

        auto groupRows = std::make_unique<UIElement>();
        groupRows->AddClass("shortcut-group-rows");
        groupRows->Overrides().Set(Style::Gap, StyleLength::Px(static_cast<float>(shortcutRowGap)));

        for (const ShortcutCatalogEntry* entry : byGroup[group])
        {
            auto row = std::make_unique<UIElement>();
            row->AddClass("shortcut-row");

            auto nameLabel = std::make_unique<Label>();
            nameLabel->AddClass("settings-row-label");
            nameLabel->SetText(entry->displayName);
            row->AddChild(std::move(nameLabel));

            auto trailing = std::make_unique<UIElement>();
            trailing->AddClass("shortcut-controls");
            trailing->Overrides()
                .Set(Style::Display, DisplayMode::Flex)
                .Set(Style::FlexDir, FlexDirection::Row)
                .Set(Style::AlignItems, AlignItems::Center);

            if (entry->readOnly)
            {
                // Hardcoded shortcut not yet wired through InputSystem — show
                // the binding as a static label so the user can still see the
                // binding, but disable rebind/reset until the call site is
                // migrated. An invisible spacer keeps the binding column
                // aligned with the editable rows' capture buttons.
                auto bindingLabel = std::make_unique<Label>();
                bindingLabel->AddClass("shortcut-capture-button");
                bindingLabel->AddClass("readonly");
                bindingLabel->SetText(FormatShortcutBinding(entry->defaultKey, entry->defaultMods));
                bindingLabel->Overrides()
                    .Set(Style::MinWidth, StyleLength::Px(96.0f));
                trailing->AddChild(std::move(bindingLabel));

                auto spacer = std::make_unique<UIElement>();
                spacer->Overrides()
                    .Set(Style::MarginLeft, StyleLength::Px(6.0f))
                    .Set(Style::Width, StyleLength::Px(24.0f));
                trailing->AddChild(std::move(spacer));
            }
            else
            {
                std::vector<ShortcutBinding> bindings = LoadShortcutBindings(prefs, *entry);
                if (bindings.empty())
                    bindings = Editor::DefaultShortcutBindings(*entry);
                const bool isDefault = bindings == Editor::DefaultShortcutBindings(*entry);

                const ShortcutCatalogEntry* entryPtr = entry;
                SettingsPanel* panel = this;

                // Rebuilds the entire shortcuts page from disk state. Used
                // after add / remove / reassign so the row always reflects
                // what's actually bound. Deferred via PostAction because
                // rebuilding destroys the element that's currently
                // dispatching this key event.
                auto refreshAll = [panel]()
                {
                    panel->PostAction([panel]()
                    {
                        panel->ShowCategoryContent(SettingsCategory::Shortcuts);
                    });
                };

                // Persists \p newChords for this entry and applies them to
                // the live InputSystem, then refreshes the UI.
                auto persistAndApply = [entryPtr, input, refreshAll](const std::vector<ShortcutBinding>& newChords)
                {
                    auto store = Editor::OpenEditorPreferences();
                    store.Load(nullptr);
                    SaveShortcutBindings(store, *entryPtr, newChords);
                    store.Save(nullptr);
                    if (input)
                        ApplyShortcutBindings(*input, *entryPtr, newChords);
                    refreshAll();
                };

                // Build one capture button per bound binding. The first one
                // carries the "modified" highlight when the row differs
                // from its default.
                for (size_t slotIdx = 0; slotIdx < bindings.size(); ++slotIdx)
                {
                    const ShortcutBinding slotChord = bindings[slotIdx];
                    const size_t capturedIdx = slotIdx;

                    auto button = std::make_unique<ShortcutCaptureButton>();
                    button->AddClass("shortcut-capture-button");
                    button->AddClass("small");
                    button->SetText(FormatShortcutBinding(slotChord.key, slotChord.mods));
                    button->Overrides()
                        .Set(Style::MinWidth, StyleLength::Px(96.0f));
                    if (slotIdx > 0)
                        button->Overrides().Set(Style::MarginLeft, StyleLength::Px(6.0f));
                    if (!isDefault)
                        button->AddClass("modified");

                    ShortcutCaptureButton* buttonRaw = button.get();

                    button->RegisterEventHandler(kEventButtonClick, [buttonRaw](UIEvent&)
                    {
                        if (buttonRaw->IsArmed())
                            return;
                        buttonRaw->SetText("Press a key...");
                        buttonRaw->Arm();
                    });

                    UIElement* modalHost = this;
                    button->SetOnCapture(
                        [buttonRaw, entryPtr, input, modalHost, capturedIdx, bindings, persistAndApply, refreshAll]
                        (int key, int mods)
                    {
                        if (entryPtr->scrollRequiresModifier &&
                            key == Editor::kShortcutKeyMouseScroll && mods == 0)
                        {
                            // Short enough for the capture button, like "Press a key...".
                            buttonRaw->SetText("Add a modifier");
                            buttonRaw->Arm();
                            return;
                        }

                        // Build the "current" list we're editing so we can
                        // decide whether the new binding duplicates another
                        // slot in the same action or collides elsewhere.
                        std::vector<ShortcutBinding> updated = bindings;
                        if (capturedIdx >= updated.size())
                            updated.push_back({key, mods});
                        else
                            updated[capturedIdx] = {key, mods};

                        // Same-action duplicate: silently drop the redundant
                        // slot and treat the action as having one binding.
                        for (size_t i = 0; i < updated.size(); ++i)
                        {
                            for (size_t j = i + 1; j < updated.size();)
                            {
                                if (updated[i] == updated[j])
                                    updated.erase(updated.begin() + static_cast<std::ptrdiff_t>(j));
                                else
                                    ++j;
                            }
                        }

                        const ShortcutCatalogEntry* conflict = FindShortcutConflict(*entryPtr, key, mods);
                        if (!conflict)
                        {
                            persistAndApply(updated);
                            return;
                        }

                        buttonRaw->SetText(FormatShortcutBinding(key, mods));

                        const std::string chordText = FormatShortcutBinding(key, mods);
                        std::string msg = chordText + " is already used by \"" +
                                          conflict->displayName + "\" in " + conflict->groupLabel + ".";
                        if (conflict->readOnly)
                            msg += "\n\nThat shortcut is hardcoded and can't be unassigned.";
                        else
                            msg += "\n\nWhat would you like to do?";

                        const ShortcutCatalogEntry* conflictPtr = conflict;
                        const bool conflictReadOnly = conflict->readOnly;

                        auto onCancel = [refreshAll]() { refreshAll(); };

                        if (conflictReadOnly)
                        {
                            ShowShortcutConflictModal(
                                modalHost,
                                "Shortcut already in use",
                                msg,
                                "OK",
                                onCancel,
                                /*onSwap=*/{},
                                /*onAddAlternate=*/{},
                                onCancel);
                            return;
                        }

                        auto onReassign = [persistAndApply, conflictPtr, input, updated]()
                        {
                            // Strip the binding from the other action: load its
                            // bindings, drop any that match the one we're
                            // stealing, then re-persist.
                            auto store = Editor::OpenEditorPreferences();
                            store.Load(nullptr);
                            auto otherChords = LoadShortcutBindings(store, *conflictPtr);
                            const ShortcutBinding stolen = updated.back();
                            otherChords.erase(
                                std::remove_if(otherChords.begin(), otherChords.end(),
                                               [&](const ShortcutBinding& c) { return c == stolen; }),
                                otherChords.end());
                            SaveShortcutBindings(store, *conflictPtr, otherChords);
                            store.Save(nullptr);
                            if (input)
                                ApplyShortcutBindings(*input, *conflictPtr, otherChords);
                            persistAndApply(updated);
                        };

                        auto onAddAlternate = [persistAndApply, bindings, key, mods]()
                        {
                            // Append the new binding as an additional slot on
                            // THIS action without touching the conflicting
                            // action — both actions end up firing on the
                            // same binding. This is the "two keys doing the
                            // same action" escape hatch.
                            std::vector<ShortcutBinding> withExtra = bindings;
                            withExtra.push_back({key, mods});
                            persistAndApply(withExtra);
                        };

                        // Swap is only meaningful when the slot being edited
                        // previously held a real binding we can hand over.
                        const bool canSwap = capturedIdx < bindings.size() &&
                                             !(bindings[capturedIdx].key == 0 && bindings[capturedIdx].mods == 0);
                        std::function<void()> onSwap;
                        if (canSwap)
                        {
                            const ShortcutBinding prevChord = bindings[capturedIdx];
                            onSwap = [persistAndApply, conflictPtr, input, updated, prevChord]()
                            {
                                // Steal the new binding from the conflicting
                                // action and give it our previous binding in
                                // return. The other action keeps any
                                // additional slots it already had.
                                auto store = Editor::OpenEditorPreferences();
                                store.Load(nullptr);
                                auto otherChords = LoadShortcutBindings(store, *conflictPtr);
                                const ShortcutBinding stolen = updated.back();
                                for (auto& c : otherChords)
                                {
                                    if (c == stolen)
                                        c = prevChord;
                                }
                                // Dedup in case the previous binding was
                                // already in the other action's list.
                                for (size_t i = 0; i < otherChords.size(); ++i)
                                {
                                    for (size_t j = i + 1; j < otherChords.size();)
                                    {
                                        if (otherChords[i] == otherChords[j])
                                            otherChords.erase(otherChords.begin() + static_cast<std::ptrdiff_t>(j));
                                        else
                                            ++j;
                                    }
                                }
                                SaveShortcutBindings(store, *conflictPtr, otherChords);
                                store.Save(nullptr);
                                if (input)
                                    ApplyShortcutBindings(*input, *conflictPtr, otherChords);
                                persistAndApply(updated);
                            };
                        }

                        ShowShortcutConflictModal(
                            modalHost,
                            "Shortcut already in use",
                            msg,
                            "Reassign",
                            onReassign,
                            onSwap,
                            onAddAlternate,
                            onCancel);
                    });

                    trailing->AddChild(std::move(button));

                    // Extra slots get a small × button to remove them.
                    if (slotIdx > 0)
                    {
                        auto removeButton = std::make_unique<Button>();
                        removeButton->AddClass("icon-button");
                        removeButton->AddClass("shortcut-remove-button");
                        removeButton->SetText("×");
                        removeButton->Overrides()
                            .Set(Style::MarginLeft, StyleLength::Px(2.0f));
                        removeButton->RegisterEventHandler(kEventButtonClick, [bindings, capturedIdx, persistAndApply](UIEvent&)
                        {
                            std::vector<ShortcutBinding> reduced = bindings;
                            if (capturedIdx < reduced.size())
                                reduced.erase(reduced.begin() + static_cast<std::ptrdiff_t>(capturedIdx));
                            if (reduced.empty())
                                reduced.push_back({0, 0});
                            persistAndApply(reduced);
                        });
                        trailing->AddChild(std::move(removeButton));
                    }
                }

                auto resetButton = std::make_unique<Button>();
                resetButton->AddClass("icon-button");
                resetButton->AddClass("shortcut-reset-button");
                resetButton->SetText("");
                resetButton->Overrides()
                    .Set(Style::MarginLeft, StyleLength::Px(6.0f));

                resetButton->RegisterEventHandler(kEventButtonClick, [entryPtr, persistAndApply](UIEvent&)
                {
                    persistAndApply(Editor::DefaultShortcutBindings(*entryPtr));
                });

                trailing->AddChild(std::move(resetButton));
            }

            row->AddChild(std::move(trailing));

            groupRows->AddChild(std::move(row));
        }

        m_ContentBody->AddContent(std::move(groupRows));
    }
}

void SettingsPanel::CreateUserSettingsContent()
{
    if (!m_ContentBody)
        return;

    auto placeholder = std::make_unique<Label>();
    placeholder->AddClass("settings-placeholder");
    placeholder->SetText("Global User-specific editor preferences");
    m_ContentBody->AddContent(std::move(placeholder));

    CreateSettingsToggleRow(
        SettingsToggleConfig{
            "Auto-load last project",
            /*defaultValue=*/true,
            "startup.autoLoadLastProject",
            /*onValueChanged=*/nullptr},
        m_ContentBody);

    // Export / Import
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");
        row->Overrides()
            .Set(Style::Display, DisplayMode::Flex)
            .Set(Style::FlexDir, FlexDirection::Row)
            .Set(Style::AlignItems, AlignItems::Center)
            .Set(Style::Gap, StyleLength::Px(8.0f))
            .Set(Style::MarginTop, StyleLength::Px(12.0f));

        auto label = std::make_unique<Label>();
        label->AddClass("settings-row-label");
        label->SetText("Settings transfer");
        row->AddChild(std::move(label));

        auto exportBtn = std::make_unique<Button>();
        exportBtn->SetText("Export…");
        exportBtn->AddClass("small");
        exportBtn->AddClass("secondary");
        exportBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) { ExportUserSettings(); });
        row->AddChild(std::move(exportBtn));

        auto importBtn = std::make_unique<Button>();
        importBtn->SetText("Import…");
        importBtn->AddClass("small");
        importBtn->AddClass("secondary");
        importBtn->RegisterEventHandler(kEventButtonClick, [this](UIEvent&) {
            const auto picked = Platform::SelectFile({}, "User Settings", "*.usersettings.json");
            if (!picked.empty())
                ShowImportUserSettingsModal(picked);
        });
        row->AddChild(std::move(importBtn));

        m_ContentBody->AddContent(std::move(row));
    }
}

void SettingsPanel::CreateProjectSettingsContent()
{
    if (!m_ContentBody)
        return;
    
    auto placeholder = std::make_unique<Label>();
    placeholder->AddClass("settings-placeholder");
    placeholder->AddClass("settings-overview-text");
    placeholder->SetText("Project specific settings");
    m_ContentBody->AddContent(std::move(placeholder));
}

int SettingsPanel::GetVisibleItemCount() const
{
    if (!m_TreeView || !m_TreeDataProvider)
        return 2;  // Default: just the two parent items
    
    // Walk the provider rather than counting categories by hand: registered
    // pages and their sub-pages appear and disappear with the loaded modules,
    // so any fixed number is wrong the moment a package registers a page.
    std::function<int(TreeId)> countVisible = [&](TreeId id) -> int
    {
        int visible = 1; // the row itself
        if (!m_TreeView->IsExpanded(id))
            return visible;
        const int children = m_TreeDataProvider->GetChildCount(id);
        for (int i = 0; i < children; ++i)
            visible += countVisible(m_TreeDataProvider->GetChildId(id, i));
        return visible;
    };

    int count = 0;
    for (int i = 0; i < m_TreeDataProvider->GetRootCount(); ++i)
        count += countVisible(m_TreeDataProvider->GetRootId(i));
    return count;
}

void SettingsPanel::UpdateTreePaneHeight()
{
    if (!m_TreePane)
        return;

    // Pixel height overrides belong only to the stacked layout. Tree scaling
    // callbacks can also reach this method while Settings is side-by-side; in
    // that case an inline half-panel height would override the horizontal
    // full-height CSS and visually cut the category tree in half.
    if (m_CurrentLayoutMode == LayoutMode::Horizontal)
    {
        m_TreePane->Overrides()
            .Reset(Style::Height)
            .Reset(Style::MinHeight)
            .Reset(Style::MaxHeight);
        m_TreePane->MarkDirty(UIElement::LayoutDirty | UIElement::StyleDirty);
        return;
    }

    const float panelHeight = GetLayoutHeight();

    float height;
    constexpr float kMinTreeHeight = 200.0f;
    if (m_UserTreePaneHeightPx > 0.0f)
    {
        // User has resized the splitter — honor their choice, only clamp to sane bounds.
        const float maxHeight = panelHeight > 0 ? std::max(kMinTreeHeight, panelHeight - 120.0f) : 600.0f;
        height = std::clamp(m_UserTreePaneHeightPx, kMinTreeHeight, maxHeight);
    }
    else
    {
        // Auto-size to visible row count, clamped between the min and half the panel.
        const float rowHeight = m_TreeView ? m_TreeView->GetRowHeight() : 20.0f;
        const int itemCount = GetVisibleItemCount();
        const float autoHeight = (float)itemCount * rowHeight + 8.0f;
        const float maxHeight = panelHeight > 0 ? std::max(kMinTreeHeight, panelHeight * 0.5f) : 300.0f;
        height = std::clamp(autoHeight, kMinTreeHeight, maxHeight);
    }

    m_TreePane->Overrides()
        .Set(Style::Height, StyleLength::Px(height))
        .Set(Style::MinHeight, StyleLength::Px(height))
        .Set(Style::MaxHeight, StyleLength::Px(height));
    m_TreePane->MarkDirty(UIElement::LayoutDirty | UIElement::StyleDirty);
}

void SettingsPanel::RegisterSearchableItems()
{
    m_SearchableItems.clear();
    
    // UI > Appearance
    m_SearchableItems.push_back({"Accent Color", "accent color theme picker", SettingsCategory::UIAppearance, nullptr});
    m_SearchableItems.push_back({"Asset Icon Tint", "asset icon tint color", SettingsCategory::UIAppearance, nullptr});
    m_SearchableItems.push_back({"Smart Folder Icon Tint", "smart folder icon tint color", SettingsCategory::UIAppearance, nullptr});
    m_SearchableItems.push_back({"Curve Time Dot", "curve playback current time indicator dot color", SettingsCategory::UIAppearance, nullptr});
    m_SearchableItems.push_back({"Scrollbar Thumb Color", "scrollbar thumb color", SettingsCategory::UIAppearance, nullptr});
    m_SearchableItems.push_back({"UI SVG Raster Size", "ui svg raster resolution size icons", SettingsCategory::UIAppearance, nullptr});
    m_SearchableItems.push_back({"Use Checkmarks for Toggles", "checkbox checkmark toggle switch boolean style", SettingsCategory::UIAppearance, nullptr});
    m_SearchableItems.push_back({"Use system display scaling", "hidpi dpi retina display scaling", SettingsCategory::UIHiDpi, nullptr});
    m_SearchableItems.push_back({"Additional UI scale", "hidpi dpi 100% 125% 150% 200% zoom", SettingsCategory::UIHiDpi, nullptr});
    // UI > Font Rendering / Font Sizes
    m_SearchableItems.push_back({"Text Weight Boost", "text weight boost ppem slug small text bold", SettingsCategory::UIFontSizes, nullptr});
    m_SearchableItems.push_back({"Text Smoothing", "text smoothing gamma font antialiasing aa smooth crisp sharp", SettingsCategory::UIFontSizes, nullptr});
    m_SearchableItems.push_back({"UI Text Scale", "font size base spacing", SettingsCategory::UIFontSizes, nullptr});
    m_SearchableItems.push_back({"Editor UI Font", "font family interface typeface inter roboto", SettingsCategory::UIFontSizes, nullptr});
    m_SearchableItems.push_back({"Script Editor Font",
                                 "font monospace code cascadia jetbrains fira source inconsolata ubuntu menlo monaco sf courier lucida",
                                 SettingsCategory::UIFontSizes,
                                 nullptr});
    // UI > Trees
    m_SearchableItems.push_back({"Hierarchy tree row height", "hierarchy row height scene tree", SettingsCategory::UITrees, nullptr});
    m_SearchableItems.push_back({"Hierarchy tree child indent", "hierarchy indent", SettingsCategory::UITrees, nullptr});
    m_SearchableItems.push_back({"Hierarchy tree icon size", "hierarchy icon folder", SettingsCategory::UITrees, nullptr});
    m_SearchableItems.push_back({"Assets browser tree row height", "assets tree row folder pane", SettingsCategory::UITrees, nullptr});
    m_SearchableItems.push_back({"Assets browser tree child indent", "assets tree indent", SettingsCategory::UITrees, nullptr});
    m_SearchableItems.push_back({"Assets browser tree icon size", "assets tree icon", SettingsCategory::UITrees, nullptr});
    // UI > Assets
    m_SearchableItems.push_back({"Default to Grid View", "assets default view grid list", SettingsCategory::UIAssets, nullptr});
    m_SearchableItems.push_back({"Asset Bottom Toolbar", "assets bottom toolbar grid list toggle", SettingsCategory::UIAssets, nullptr});
    m_SearchableItems.push_back({"Single View Toggle Icon", "assets single icon toggle grid list", SettingsCategory::UIAssets, nullptr});
    m_SearchableItems.push_back(
        {"Show Assets Zoom Slider", "assets bottom toolbar zoom grid list icon size slider", SettingsCategory::UIAssets, nullptr});
    m_SearchableItems.push_back({"Assets Grid Icon Size", "grid icon size assets thumbnail", SettingsCategory::UIAssets, nullptr});
    m_SearchableItems.push_back(
        {"Use IBL in 3D Previews", "ibl image based lighting environment model material preview thumbnails",
         SettingsCategory::UIAssets, nullptr});
    m_SearchableItems.push_back({"Text Truncation", "truncate text ellipsis", SettingsCategory::UIAssets, nullptr});
    m_SearchableItems.push_back({"Truncation Threshold", "truncation threshold icon size", SettingsCategory::UIAssets, nullptr});
    m_SearchableItems.push_back(
        {"Expand Smart Folders On Startup", "smart folders startup collapse expand assets", SettingsCategory::UIAssets, nullptr});
    m_SearchableItems.push_back(
        {"Filled Search Bar", "search bars filled background visibility show hide", SettingsCategory::UIAppearance, nullptr});
    m_SearchableItems.push_back(
        {"Place Search Bars at Top", "search bar placement position top bottom panels", SettingsCategory::UIAppearance, nullptr});
    m_SearchableItems.push_back(
        {"Accent Search Focus Outline", "search bar focus border accent color", SettingsCategory::UIAppearance, nullptr});
    m_SearchableItems.push_back({"Panel Tab Icons", "tab icons dock panel tab header", SettingsCategory::UIAppearance, nullptr});
    m_SearchableItems.push_back({"Tab Right-Click Menu", "tab right click context menu close", SettingsCategory::UIAppearance, nullptr});
    // UI > Inspector
    m_SearchableItems.push_back({"Inspector Label Width", "inspector label width", SettingsCategory::UIInspector, nullptr});
    m_SearchableItems.push_back({"Inspector Row Indent", "inspector row indent padding", SettingsCategory::UIInspector, nullptr});
    m_SearchableItems.push_back({"Inspector Row Gap", "inspector row gap margin", SettingsCategory::UIInspector, nullptr});
    m_SearchableItems.push_back({"Filled Inspector Sections", "inspector filled sections cards", SettingsCategory::UIInspector, nullptr});
    m_SearchableItems.push_back({"Show Inspector Info Cards", "inspector info cards help reference explanatory", SettingsCategory::UIInspector, nullptr});
    m_SearchableItems.push_back({"Inspector Collapse Arrow", "inspector collapse arrow section", SettingsCategory::UIInspector, nullptr});
    m_SearchableItems.push_back({"Show Component Icons", "inspector component icons section header", SettingsCategory::UIInspector, nullptr});
    m_SearchableItems.push_back({"Inspector Solo Sections", "inspector solo sections expand", SettingsCategory::UIInspector, nullptr});
    m_SearchableItems.push_back({"Inspector Toggle Alignment", "inspector toggle alignment left middle right", SettingsCategory::UIInspector, nullptr});
    
    // Script Settings
    m_SearchableItems.push_back({"External Script Editor", "script editor path external code", SettingsCategory::Script, nullptr});
    m_SearchableItems.push_back({"Open in Inspector", "script inspector open", SettingsCategory::Script, nullptr});
    m_SearchableItems.push_back({"Syntax Highlighting", "syntax color keyword string comment", SettingsCategory::Script, nullptr});
    
    // Version Control Settings
    m_SearchableItems.push_back({"Version Control", "version control vcs", SettingsCategory::VersionControl, nullptr});

    // One search entry per registered VCS provider tab (dynamic categories).
    {
        const auto providers = Editor::EditorVcsProviderRegistry::Get().Snapshot();
        const size_t count =
            std::min(providers.size(), static_cast<size_t>(kVcsProviderCategoryMaxCount));
        for (size_t i = 0; i < count; ++i)
        {
            std::string keywords = providers[i].TypeId + " " + providers[i].DisplayName +
                                   " settings version control vcs executable status";
            for (char& c : keywords)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            m_SearchableItems.push_back(
                {providers[i].DisplayName, keywords,
                 static_cast<SettingsCategory>(kVcsProviderCategoryBase + i), nullptr});
        }
    }

    // Audio Settings
    m_SearchableItems.push_back({"Audio Settings", "audio sound volume", SettingsCategory::AudioSettings, nullptr});
    
    // Scene View Settings
    m_SearchableItems.push_back({"Scene View Settings", "scene view camera fov", SettingsCategory::Camera, nullptr});
    m_SearchableItems.push_back({"Anti-Aliasing", "scene view anti aliasing taa msaa fxaa temporal two-frame smaa ssaa supersample multisample off 4x 2x", SettingsCategory::Camera, nullptr});
    m_SearchableItems.push_back({"SSAA Scale", "ssaa supersampling scale 1.25x 1.5x 2x anti aliasing", SettingsCategory::Camera, nullptr});
    m_SearchableItems.push_back({"MSAA Samples", "scene view multisample anti aliasing msaa samples 2x 4x 8x", SettingsCategory::Camera, nullptr});
    m_SearchableItems.push_back({"FXAA Quality", "scene view fxaa quality fast anti aliasing", SettingsCategory::Camera, nullptr});
    m_SearchableItems.push_back({"FXAA Frames", "scene view fxaa frames single two temporal anti aliasing", SettingsCategory::Camera, nullptr});
    m_SearchableItems.push_back({"Render Scale", "render scale supersampling ssaa upscale downsample internal resolution taau", SettingsCategory::Camera, nullptr});
    m_SearchableItems.push_back({"Measure Color", "measure ruler color scene view", SettingsCategory::Camera, nullptr});
    m_SearchableItems.push_back({"Allow Child Selection", "child selection scene pick parent hierarchy", SettingsCategory::Camera, nullptr});
    
    // Animation Settings
    m_SearchableItems.push_back({"Animation Settings", "animation keyframe timeline", SettingsCategory::Animation, nullptr});
    
    // Input Settings
    m_SearchableItems.push_back({"Input Settings", "input keyboard mouse controller", SettingsCategory::Input, nullptr});

    // Asset Import Settings
    m_SearchableItems.push_back({"SVG Texture Raster Size", "svg texture import raster resolution default", SettingsCategory::AssetImport, nullptr});
    m_SearchableItems.push_back({"FBX Scene Extras", "fbx import extras cameras lights lamps helper nodes empties", SettingsCategory::AssetImport, nullptr});
    m_SearchableItems.push_back({"FBX Cameras", "fbx import camera scene extras", SettingsCategory::AssetImport, nullptr});
    m_SearchableItems.push_back({"FBX Lights", "fbx import lights lamps scene extras", SettingsCategory::AssetImport, nullptr});
    m_SearchableItems.push_back({"FBX Helper Nodes", "fbx import helpers empties null nodes scene extras", SettingsCategory::AssetImport, nullptr});
    m_SearchableItems.push_back({"FBX Tangents", "fbx import generate missing tangents normal maps", SettingsCategory::AssetImport, nullptr});
    m_SearchableItems.push_back({"FBX Skin Weights", "fbx import clean skin weights bones influences", SettingsCategory::AssetImport, nullptr});
    m_SearchableItems.push_back({"FBX Pivots", "fbx import pivots geometry transforms embedded textures", SettingsCategory::AssetImport, nullptr});

    // Scene (auto-save backup)
    m_SearchableItems.push_back({"Scene auto-save", "auto save backup scene interval seconds", SettingsCategory::Scene, nullptr});

    // Physics Settings
    m_SearchableItems.push_back({"Gravity", "physics gravity simulation world", SettingsCategory::Physics, nullptr});
    m_SearchableItems.push_back({"Fixed Time Step", "physics time step simulation stepping", SettingsCategory::Physics, nullptr});
    m_SearchableItems.push_back({"Max Sub Steps", "physics substeps simulation", SettingsCategory::Physics, nullptr});
    m_SearchableItems.push_back({"Collision Steps", "physics collision iterations", SettingsCategory::Physics, nullptr});

    // Rendering Settings
    m_SearchableItems.push_back({"Active Render Pipeline", "render pipeline rendergraph passes forward clustered", SettingsCategory::Rendering, nullptr});
    for (const Editor::SettingsFieldDescriptor& field :
         Editor::CreateDirectionalShadowSettingsSection().Fields)
    {
        std::string keywords = "directional shadows " + field.SearchKeywords;
        m_SearchableItems.push_back({field.Label, std::move(keywords),
                                     SettingsCategory::Rendering, nullptr});
    }
    m_SearchableItems.push_back({"HDR Output", "hdr output hdr10 pq st2084 hlg scrgb monitor display metadata brightness paper white nits", SettingsCategory::HDROutput, nullptr});
    m_SearchableItems.push_back({"Detected HDR Support", "hdr monitor display tv lg oled c7 steam deck gamescope capability support", SettingsCategory::HDROutput, nullptr});

    // Performance Settings
    m_SearchableItems.push_back({"VSync", "vsync vertical sync frame rate display refresh", SettingsCategory::Performance, nullptr});

    // One search entry per field of every category registered through
    // Editor::EditorSettingsRegistry (dynamic categories).
    {
        const auto categories = Editor::EditorSettingsRegistry::Get().Snapshot();
        const size_t count =
            std::min(categories.size(), static_cast<size_t>(kRegistrySettingsCategoryMaxCount));
        for (size_t i = 0; i < count; ++i)
        {
            auto category = static_cast<SettingsCategory>(kRegistrySettingsCategoryBase + i);
            if (categories[i].Group == Editor::SettingsCategoryGroup::VersionControl)
                category = SettingsCategory::VersionControl;
            else if (categories[i].Group == Editor::SettingsCategoryGroup::UIAppearance)
                category = SettingsCategory::UIAppearance;
            if (!categories[i].SearchKeywords.empty())
            {
                std::string keywords = categories[i].Title + " " + categories[i].SearchKeywords;
                for (char& c : keywords)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                m_SearchableItems.push_back(
                    {categories[i].Title, keywords, category, nullptr});
            }
            for (const Editor::SettingsFieldDescriptor& field : categories[i].Fields)
            {
                std::string keywords = categories[i].Title + " " + field.SearchKeywords;
                for (char& c : keywords)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                m_SearchableItems.push_back({field.Label, keywords, category, nullptr});
            }
        }
    }
}

void SettingsPanel::ClearSearchHighlights()
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
    // Restore Labels that were replaced with segment containers for word highlighting
    for (SearchSegmentReplacement& r : m_SearchSegmentReplacements)
    {
        if (!r.parent || !r.segmentContainer || !r.OriginalLabel)
            continue;
        std::vector<UIElement*> ptrs;
        for (const auto& c : r.parent->GetChildren())
            ptrs.push_back(c.get());
        std::vector<std::unique_ptr<UIElement>> taken;
        for (UIElement* p : ptrs)
            taken.push_back(r.parent->TakeChild(p));
        for (size_t i = 0; i < taken.size(); ++i)
        {
            if (taken[i].get() == r.segmentContainer)
            {
                taken[i] = std::move(r.OriginalLabel);
                break;
            }
        }
        for (auto& c : taken)
            r.parent->AddChild(std::move(c));
    }
    m_SearchSegmentReplacements.clear();
}

void SettingsPanel::ApplySearchFilterToContent(const std::string& searchText)
{
    ClearSearchHighlights();
    if (searchText.empty() || !m_ContentBody)
        return;
    UIElement* viewport = m_ContentBody->GetViewport();
    if (!viewport)
        return;
    std::string searchLower = searchText;
    for (char& c : searchLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    struct PendingReplacement { Label* label; UIElement* parent; std::string before; std::string match; std::string after; };
    std::vector<PendingReplacement> pending;
    std::unordered_map<UIElement*, bool> subtreeMatches;
    std::unordered_set<UIElement*> directMatches;

    std::function<bool(UIElement*)> searchInElement = [&](UIElement* el) -> bool {
        if (!el)
            return false;
        bool hasMatchInSubtree = false;
        if (auto* label = dynamic_cast<Label*>(el))
        {
            std::string text = label->GetText();
            std::string textLower = text;
            for (char& c : textLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            size_t pos = textLower.find(searchLower);
            if (pos != std::string::npos)
            {
                hasMatchInSubtree = true;
                directMatches.insert(label);
                UIElement* parent = label->GetParent();
                if (parent && text.size() < 200)
                    pending.push_back({label, parent, text.substr(0, pos), text.substr(pos, searchText.size()), text.substr(pos + searchText.size())});
                else
                {
                    label->AddClass("search-match");
                    m_SearchHighlightedElements.push_back(label);
                }
            }
        }
        if (auto* infoCard = dynamic_cast<EditorUI::CollapsibleInfoCard*>(el))
        {
            // Collapsed cards show only their preview line, so match against the
            // full text and expand the card the hit is in.
            std::string cardLower = infoCard->GetFullText();
            for (char& c : cardLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (cardLower.find(searchLower) != std::string::npos)
            {
                infoCard->SetExpanded(true);
                infoCard->AddClass("search-match");
                m_SearchHighlightedElements.push_back(infoCard);
                directMatches.insert(infoCard);
                hasMatchInSubtree = true;
            }
        }
        if (auto* foldout = dynamic_cast<Foldout*>(el))
        {
            std::string title = foldout->GetTitle();
            std::string titleLower = title;
            for (char& c : titleLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (titleLower.find(searchLower) != std::string::npos)
            {
                foldout->AddClass("search-match");
                m_SearchHighlightedElements.push_back(foldout);
                directMatches.insert(foldout);
                hasMatchInSubtree = true;
            }
        }
        for (const auto& child : el->GetChildren())
        {
            if (searchInElement(child.get()))
                hasMatchInSubtree = true;
        }
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
    searchInElement(viewport);

    std::function<void(UIElement*, bool)> applyVisibility = [&](UIElement* el, bool showWholeSubtree)
    {
        if (!el)
            return;
        const bool directMatch = directMatches.count(el) != 0;
        const bool keepWholeSubtree = showWholeSubtree || directMatch;
        for (const auto& child : el->GetChildren())
            applyVisibility(child.get(), keepWholeSubtree);

        if (el == viewport)
            return;
        const bool filterable = dynamic_cast<Foldout*>(el) != nullptr ||
                                el->HasClass("settings-row") ||
                                el->GetParent() == viewport;
        const bool hasMatch = subtreeMatches.count(el) != 0 && subtreeMatches[el];
        if (filterable && !keepWholeSubtree && !hasMatch)
        {
            el->AddClass("search-filtered-out");
            m_SearchFilteredElements.push_back(el);
        }
    };
    applyVisibility(viewport, false);

    for (const PendingReplacement& pr : pending)
    {
        Label* label = pr.label;
        UIElement* parent = pr.parent;
        if (!parent || parent->GetChildren().empty())
            continue;
        auto container = std::make_unique<UIElement>();
        container->Overrides().Set(Style::Display, DisplayMode::Flex).Set(Style::FlexDir, FlexDirection::Row);
        container->AddClass("settings-search-word-highlight");
        if (!pr.before.empty())
        {
            auto beforeLabel = std::make_unique<Label>();
            beforeLabel->SetText(pr.before);
            container->AddChild(std::move(beforeLabel));
        }
        auto matchLabel = std::make_unique<Label>();
        matchLabel->SetText(pr.match);
        matchLabel->AddClass("search-match");
        m_SearchHighlightedElements.push_back(matchLabel.get());
        container->AddChild(std::move(matchLabel));
        if (!pr.after.empty())
        {
            auto afterLabel = std::make_unique<Label>();
            afterLabel->SetText(pr.after);
            container->AddChild(std::move(afterLabel));
        }
        UIElement* containerRaw = container.get();
        std::vector<UIElement*> ptrs;
        for (const auto& c : parent->GetChildren())
            ptrs.push_back(c.get());
        std::vector<std::unique_ptr<UIElement>> taken;
        for (UIElement* p : ptrs)
            taken.push_back(parent->TakeChild(p));
        std::unique_ptr<UIElement> originalLabel;
        for (size_t i = 0; i < taken.size(); ++i)
        {
            if (taken[i].get() == label)
            {
                originalLabel = std::move(taken[i]);
                taken[i] = std::move(container);
                break;
            }
        }
        if (originalLabel)
        {
            m_SearchSegmentReplacements.push_back({parent, std::move(originalLabel), containerRaw});
            for (auto& c : taken)
                parent->AddChild(std::move(c));
        }
    }
}

void SettingsPanel::PerformSearch(const std::string& query)
{
    if (query.empty())
    {
        ClearSearchResults();
        return;
    }

    m_CurrentSearchQuery = query;
    m_ShowingSearchResults = true;
    RequestApplySearchState();
}

void SettingsPanel::RequestApplySearchState()
{
    if (UIElement::IsInEventDispatch())
    {
        if (m_SearchApplyPosted)
            return;

        m_SearchApplyPosted = true;
        this->PostAction([this]()
        {
            m_SearchApplyPosted = false;
            this->ApplySearchStateNow();
        });
        return;
    }

    ApplySearchStateNow();
}

void SettingsPanel::ApplySearchStateNow()
{
    if (UIElement::IsInEventDispatch())
    {
        RequestApplySearchState();
        return;
    }

    if (m_CurrentSearchQuery.empty())
    {
        m_ShowingSearchResults = false;
        m_SearchMatchCategories.clear();
        ClearSearchHighlights();
        if (m_TreeView)
        {
            m_TreeView->SetVisibilityFilter({});
            m_TreeView->RefreshFromProvider();
        }
        if (m_PendingSearchContextReveal)
        {
            PendingSearchContextReveal reveal = std::move(*m_PendingSearchContextReveal);
            m_PendingSearchContextReveal.reset();
            RequestShowCategory(reveal.Category);
            PostAction([this, label = std::move(reveal.Label), query = std::move(reveal.Query)]()
            {
                HighlightSearchResultInContext(label, query);
            });
            return;
        }
        if (m_CurrentCategory == SettingsCategory::Shortcuts)
            ShowCategoryContent(SettingsCategory::Shortcuts);
        return;
    }

    m_ShowingSearchResults = true;

    // When the Shortcuts page is open, search only that page (filter rows in-place).
    // Do not build cross-tree match lists — avoids Up/Down cycling other sections.
    if (m_CurrentCategory == SettingsCategory::Shortcuts)
    {
        m_SearchMatchCategories.clear();
        if (m_TreeView)
        {
            auto visibleIds = std::make_shared<std::unordered_set<TreeId>>();
            visibleIds->insert(static_cast<TreeId>(SettingsCategory::UserSettings));
            visibleIds->insert(static_cast<TreeId>(SettingsCategory::Shortcuts));
            m_TreeView->SetExpanded(static_cast<TreeId>(SettingsCategory::UserSettings), true);
            m_TreeView->SetVisibilityFilter([visibleIds](TreeId id)
                                            { return visibleIds->count(id) != 0; });
        }
        ShowCategoryContent(SettingsCategory::Shortcuts);
        return;
    }

    // Build list of categories that have at least one match, in order of first occurrence in m_SearchableItems.
    std::string lowerQuery = m_CurrentSearchQuery;
    for (char& c : lowerQuery) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    m_SearchMatchCategories.clear();
    std::unordered_set<SettingsCategory> seen;
    for (const auto& item : m_SearchableItems)
    {
        std::string lowerLabel = item.Label;
        std::string lowerKeywords = item.Keywords;
        for (char& c : lowerLabel) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (char& c : lowerKeywords) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lowerLabel.find(lowerQuery) != std::string::npos || lowerKeywords.find(lowerQuery) != std::string::npos)
        {
            if (seen.insert(item.Category).second)
                m_SearchMatchCategories.push_back(item.Category);
        }
    }
    // Every tree section title participates in global search (curated list above is additive).
    // Shortcuts stays page-local via the early return above when that category is active.
    if (m_TreeDataProvider)
        m_TreeDataProvider->AppendCategoriesMatchingTreeLabels(lowerQuery, m_SearchMatchCategories, seen);
    // Expand tree so all matching nodes are visible: (1) nodes whose label matches the query, (2) ancestors of sections that have content matches.
    if (m_TreeDataProvider && m_TreeView)
    {
        std::unordered_set<TreeId> toExpand = m_TreeDataProvider->GetAncestorIdsToExpandForSearch(m_CurrentSearchQuery);
        auto visibleIds = std::make_shared<std::unordered_set<TreeId>>();
        if (!m_SearchMatchCategories.empty())
        {
            std::unordered_set<TreeId> matchIds;
            for (SettingsCategory cat : m_SearchMatchCategories)
                matchIds.insert(static_cast<TreeId>(cat));
            std::unordered_set<TreeId> ancestors = m_TreeDataProvider->GetAncestorIdsToExpandForNodes(matchIds);
            for (TreeId id : ancestors)
                toExpand.insert(id);
            visibleIds->insert(matchIds.begin(), matchIds.end());
            visibleIds->insert(ancestors.begin(), ancestors.end());
        }
        for (TreeId id : toExpand)
            m_TreeView->SetExpanded(id, true);
        visibleIds->insert(toExpand.begin(), toExpand.end());
        m_TreeView->SetVisibilityFilter([visibleIds](TreeId id)
                                        { return visibleIds->count(id) != 0; });
    }

    // Present every category match in one aggregate view. The filtered tree
    // remains visible as context and each result can reveal its full subsection.
    ShowSearchResults(m_CurrentSearchQuery);
}

void SettingsPanel::ShowSearchResults(const std::string& query)
{
    if (!m_ContentBody)
        return;
    
    // Clear existing content
    ClearContentPane();
    m_ShowingSearchResults = true;
    
    // Update header
    if (m_ContentHeader)
        m_ContentHeader->SetText("Search Results");
    
    // Convert query to lowercase for case-insensitive matching
    std::string lowerQuery = query;
    for (char& c : lowerQuery) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    
    // Find matching items
    std::vector<const SearchableSettingItem*> matches;
    for (const auto& item : m_SearchableItems)
    {
        std::string lowerLabel = item.Label;
        std::string lowerKeywords = item.Keywords;
        for (char& c : lowerLabel) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (char& c : lowerKeywords) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (lowerLabel.find(lowerQuery) != std::string::npos ||
            lowerKeywords.find(lowerQuery) != std::string::npos)
        {
            matches.push_back(&item);
        }
    }
    
    if (matches.empty() && m_SearchMatchCategories.empty())
    {
        // No results message
        auto noResults = MakeSettingsInfoCard("No settings found matching \"" + query + "\"");
        {
            noResults->Overrides()
                .Set(Style::PaddingTop, StyleLength::Px(20.0f))
                .Set(Style::PaddingRight, StyleLength::Px(20.0f))
                .Set(Style::PaddingBottom, StyleLength::Px(20.0f))
                .Set(Style::PaddingLeft, StyleLength::Px(20.0f))
                .Set(Style::TextAlignProp, TextAlign::Center);
        }
        m_ContentBody->AddContent(std::move(noResults));
        return;
    }
    
    // Group matches by category
    std::unordered_map<SettingsCategory, std::vector<const SearchableSettingItem*>> groupedMatches;
    for (const auto* match : matches)
    {
        groupedMatches[match->Category].push_back(match);
    }
    
    // Category names
    auto getCategoryName = [](SettingsCategory cat) -> const char* {
        if (IsVcsProviderCategory(cat))
        {
            const size_t index =
                static_cast<size_t>(static_cast<uint64_t>(cat) - kVcsProviderCategoryBase);
            const auto providers = Editor::EditorVcsProviderRegistry::Get().Snapshot();
            if (index < providers.size())
            {
                static std::string s_VcsCategoryName;
                s_VcsCategoryName = providers[index].DisplayName + " Settings";
                return s_VcsCategoryName.c_str();
            }
            return "Version Control Settings";
        }
        if (IsRegistrySettingsCategory(cat))
        {
            const size_t index =
                static_cast<size_t>(static_cast<uint64_t>(cat) - kRegistrySettingsCategoryBase);
            const auto categories = Editor::EditorSettingsRegistry::Get().Snapshot();
            if (index < categories.size())
            {
                static std::string s_RegistryCategoryName;
                s_RegistryCategoryName = categories[index].Title + " Settings";
                return s_RegistryCategoryName.c_str();
            }
            return "Settings";
        }
        switch (cat)
        {
        case SettingsCategory::UI: return "UI Settings";
        case SettingsCategory::UIAppearance: return "UI - Appearance";
        case SettingsCategory::UIFontSizes: return "UI - Font Rendering";
        case SettingsCategory::UITrees: return "UI - Trees";
        case SettingsCategory::UIAssets: return "UI - Assets";
        case SettingsCategory::UIHiDpi: return "UI - HiDPI";
        case SettingsCategory::UIInspector: return "UI - Inspector";
        case SettingsCategory::Script: return "Script Settings";
        case SettingsCategory::VersionControl:
            return "Version Control Settings";
        case SettingsCategory::AudioSettings: return "Audio Settings";
        case SettingsCategory::Camera: return "Scene View Settings";
        case SettingsCategory::Animation: return "Animation Settings";
        case SettingsCategory::Input: return "Input Settings";
        case SettingsCategory::Physics: return "Physics Settings";
        case SettingsCategory::AssetImport: return "Asset Import Settings";
        case SettingsCategory::Scene: return "Scene Settings";
        case SettingsCategory::Rendering: return "Rendering Settings";
        case SettingsCategory::HDROutput: return "HDR Output Settings";
        case SettingsCategory::Tags: return "Tags Settings";
        case SettingsCategory::Gizmo: return "Gizmo Settings";
        case SettingsCategory::GridAndSnapping: return "Grid & Snapping";
        case SettingsCategory::Performance: return "Performance Settings";
        case SettingsCategory::Shortcuts: return "Keyboard Shortcuts";
        default:
            return "Other";
        }
    };
    
    auto summary = std::make_unique<Label>();
    summary->SetText(std::to_string(matches.size()) + " matching setting" +
                     (matches.size() == 1 ? "" : "s") + " across " +
                     std::to_string(m_SearchMatchCategories.size()) + " subsection" +
                     (m_SearchMatchCategories.size() == 1 ? "" : "s"));
    summary->AddClass("settings-search-results-summary");
    m_ContentBody->AddContent(std::move(summary));

    auto addResultRow = [this, &query](SettingsCategory category,
                                      const std::string& labelText)
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("settings-row");
        row->AddClass("settings-search-result");

        auto label = std::make_unique<Label>();
        label->SetText(labelText);
        label->AddClass("settings-search-result-label");
        std::string lowerLabel = labelText;
        std::string lowerQuery = query;
        for (char& c : lowerLabel) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (char& c : lowerQuery) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lowerLabel.find(lowerQuery) != std::string::npos)
            label->AddClass("search-match");
        row->AddChild(std::move(label));

        auto context = std::make_unique<Label>();
        context->SetText("Show in context");
        context->AddClass("settings-search-result-context");
        row->AddChild(std::move(context));

        row->RegisterEventHandler(kEventMouseUp,
            [this, category, labelText, query](UIEvent& e)
            {
                OpenSearchResultInContext(category, labelText, query);
                e.Stop();
            });
        m_ContentBody->AddContent(std::move(row));
    };

    // Preserve the tree/search registration order rather than relying on the
    // iteration order of the grouping map.
    for (SettingsCategory category : m_SearchMatchCategories)
    {
        auto sectionHeader = std::make_unique<Label>();
        sectionHeader->SetText(getCategoryName(category));
        sectionHeader->AddClass("settings-section-header");
        sectionHeader->AddClass("settings-search-results-section-header");
        m_ContentBody->AddContent(std::move(sectionHeader));

        auto groupedIt = groupedMatches.find(category);
        if (groupedIt == groupedMatches.end() || groupedIt->second.empty())
            addResultRow(category, getCategoryName(category));
        else
            for (const SearchableSettingItem* item : groupedIt->second)
                addResultRow(category, item->Label);
    }
}

namespace
{
UIElement* FindSettingsSearchContextTarget(UIElement* root,
                                           const std::string& label,
                                           const std::string& query)
{
    auto lower = [](std::string value)
    {
        for (char& c : value)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return value;
    };

    const std::string wantedLabel = lower(label);
    const std::string wantedQuery = lower(query);
    UIElement* labelContains = nullptr;
    UIElement* queryContains = nullptr;
    UIElement* exact = nullptr;
    std::function<void(UIElement*)> visit = [&](UIElement* element)
    {
        if (!element || exact)
            return;
        if (auto* candidate = dynamic_cast<Label*>(element))
        {
            const std::string text = lower(candidate->GetText());
            if (!wantedLabel.empty() && text == wantedLabel)
            {
                exact = candidate;
                return;
            }
            if (!labelContains && !wantedLabel.empty() && text.find(wantedLabel) != std::string::npos)
                labelContains = candidate;
            if (!queryContains && !wantedQuery.empty() && text.find(wantedQuery) != std::string::npos)
                queryContains = candidate;
        }
        for (const auto& child : element->GetChildren())
            visit(child.get());
    };
    visit(root);
    return exact ? exact : (labelContains ? labelContains : queryContains);
}
}

void SettingsPanel::OpenSearchResultInContext(SettingsCategory category,
                                              const std::string& label,
                                              const std::string& query)
{
    m_PendingSearchContextReveal = PendingSearchContextReveal{category, label, query};
    if (m_SearchField)
        m_SearchField->SetValue("");
    ClearSearchResults();
}

void SettingsPanel::HighlightSearchResultInContext(const std::string& label,
                                                   const std::string& query)
{
    if (!m_ContentBody)
        return;
    UIElement* viewport = m_ContentBody->GetViewport();
    UIElement* target = FindSettingsSearchContextTarget(viewport, label, query);
    if (!target)
        return;

    target->AddClass("search-match");
    m_SearchHighlightedElements.push_back(target);

    // Centering needs the row's laid-out position, which does not exist until
    // layout has run over the freshly rebuilt page. Hand it to OnPostLayout,
    // which is the point where that is guaranteed.
    m_PendingSearchScroll = PendingSearchContextReveal{m_CurrentCategory, label, query};
}

void SettingsPanel::ScrollPendingSearchRowIntoView()
{
    if (!m_PendingSearchScroll || !m_ContentBody)
        return;

    UIElement* viewport = m_ContentBody->GetViewport();
    UIElement* target =
        FindSettingsSearchContextTarget(viewport, m_PendingSearchScroll->Label,
                                        m_PendingSearchScroll->Query);
    if (!viewport || !target)
    {
        // The page has no such row (a stale reveal, or one whose category never
        // opened) — drop it rather than re-scrolling on every later layout.
        m_PendingSearchScroll.reset();
        return;
    }

    UIElement* scrollTarget = target;
    for (UIElement* parent = target->GetParent(); parent && parent != viewport;
         parent = parent->GetParent())
    {
        if (parent->HasClass("settings-row"))
        {
            scrollTarget = parent;
            break;
        }
    }

    const float contentY =
        scrollTarget->GetLayoutY() - viewport->GetLayoutY() + m_ContentBody->GetScrollY();
    const float centeredY = contentY - std::max(0.0f, m_ContentBody->GetViewportHeight() * 0.35f);
    m_ContentBody->SetScrollY(centeredY);
    m_PendingSearchScroll.reset();
}

void SettingsPanel::ClearSearchResults()
{
    if (!m_ShowingSearchResults && m_CurrentSearchQuery.empty())
        return;

    m_ShowingSearchResults = false;
    m_CurrentSearchQuery.clear();
    m_SearchMatchCategories.clear();
    RequestApplySearchState();
}

// ---------------------------------------------------------------------------
// User Settings export / import
// ---------------------------------------------------------------------------

namespace
{

struct SettingsExportGroup
{
    const char* label;
    const char* storeKey;      // "editorPreferences" or "userProjectSettings"
    std::vector<std::string> prefixes;
    bool exactMatch = false;   // match full key instead of prefix when true
};

static const SettingsExportGroup kExportGroups[] = {
    {"UI Appearance",    "editorPreferences",    {"ui.", "curveEditor."}},
    {"Scene View",       "editorPreferences",    {"sceneView."}},
    {"Gizmo Settings",   "editorPreferences",    {"gizmo."}},
    {"Node Graph",       "editorPreferences",    {"nodeGraph."}},
    {"Keyboard Shortcuts", "editorPreferences", {"shortcuts."}},
    {"Script Editor",    "editorPreferences",    {"script."}},
    {"Tooltips & Text",  "editorPreferences",    {"tooltip.", "text.", "graphics."}},
    {"General",          "editorPreferences",    {"startup.", "editor.", "onlineAssets."}},
    {"Camera Bookmarks", "userProjectSettings",  {"cameraBookmarks", "defaultCameraBookmark"}, true},
};

static bool KeyMatchesGroup(const std::string& key, const SettingsExportGroup& group)
{
    for (const auto& prefix : group.prefixes)
    {
        if (group.exactMatch ? (key == prefix) : key.starts_with(prefix))
            return true;
    }
    return false;
}

} // namespace

void SettingsPanel::ExportUserSettings()
{
    const auto savePath = Platform::SaveFile("UserSettings.usersettings.json",
                                             "User Settings", "*.usersettings.json");
    if (savePath.empty())
        return;

    nlohmann::json exportDoc;
    exportDoc["exportVersion"] = 1;

    // Editor-global preferences
    {
        auto prefs = Editor::OpenEditorPreferences();
        prefs.Load();
        nlohmann::json prefsJson = nlohmann::json::object();
        for (auto& [k, v] : prefs.Json().items())
        {
            if (k == "schemaVersion")
                continue;
            prefsJson[k] = v;
        }
        exportDoc["editorPreferences"] = std::move(prefsJson);
    }

    // Per-project user settings (camera bookmarks, etc.)
    const auto& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
    if (!workspaceRoot.empty())
    {
        auto userProj = Editor::OpenUserProjectSettings(workspaceRoot);
        userProj.Load();
        nlohmann::json userProjJson = nlohmann::json::object();
        for (auto& [k, v] : userProj.Json().items())
        {
            if (k == "schemaVersion")
                continue;
            userProjJson[k] = v;
        }
        exportDoc["userProjectSettings"] = std::move(userProjJson);
    }

    try
    {
        std::ofstream out(savePath, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            LOG_ERROR("SettingsPanel: failed to open export file: {}", savePath.string());
            return;
        }
        out << exportDoc.dump(2);
    }
    catch (const std::exception& ex)
    {
        LOG_ERROR("SettingsPanel: export failed: {}", ex.what());
    }
}

void SettingsPanel::ShowImportUserSettingsModal(const std::filesystem::path& importPath)
{
    nlohmann::json exportDoc;
    try
    {
        GameEngine::String text;
        if (!GameEngine::ReadFileTextShared(importPath, text))
        {
            LOG_ERROR("SettingsPanel: cannot open import file: {}", importPath.string());
            return;
        }
        exportDoc = nlohmann::json::parse(text, nullptr, /*exceptions=*/true, /*ignore_comments=*/true);
    }
    catch (const std::exception& ex)
    {
        LOG_ERROR("SettingsPanel: import file parse error: {}", ex.what());
        return;
    }

    // Determine which groups have data in the file
    struct PresentGroup
    {
        size_t index;   // into kExportGroups
        bool enabled;   // checkbox state
    };
    std::vector<PresentGroup> presentGroups;

    for (size_t i = 0; i < std::size(kExportGroups); ++i)
    {
        const auto& group = kExportGroups[i];
        if (!exportDoc.contains(group.storeKey))
            continue;
        const auto& storeJson = exportDoc[group.storeKey];
        if (!storeJson.is_object())
            continue;
        for (auto& [key, val] : storeJson.items())
        {
            if (KeyMatchesGroup(key, group))
            {
                presentGroups.push_back({i, true});
                break;
            }
        }
    }

    if (presentGroups.empty())
        return;

    // ---- Build modal overlay ------------------------------------------------
    UIElement* panelHost = this;

    auto overlay = std::make_unique<UIElement>();
    UIElement* overlayRaw = overlay.get();
    overlay->SetOverlayLayer(OverlayLayer::BlockingDialog);
    overlay->Overrides()
        .Set(Style::Position, PositionType::Absolute)
        .Set(Style::PositionLeft, StyleLength::Px(0.0f))
        .Set(Style::PositionTop, StyleLength::Px(0.0f))
        .Set(Style::Width, StyleLength::Percent(100.0f))
        .Set(Style::Height, StyleLength::Percent(100.0f))
        .Set(Style::ZIndex, 10000)
        .Set(Style::BackgroundColor, (uint32_t)0x99000000)
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::JustifyContent, JustifyContent::Center)
        .Set(Style::PointerEvents, true);

    auto window = std::make_unique<UIElement>();
    window->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Column)
        .Set(Style::BackgroundColor, (uint32_t)0xFF252526)
        .Set(Style::BorderWidth, Box4{1, 1, 1, 1})
        .Set(Style::BorderColor, BorderColorsTRBL{0xFF3A3A3A, 0xFF3A3A3A, 0xFF3A3A3A, 0xFF3A3A3A})
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{10, 10, 10, 10})
        .Set(Style::Width, StyleLength::Px(400.0f))
        .Set(Style::MaxWidth, StyleLength::Percent(85.0f))
        .Set(Style::OverflowProp, Overflow::Hidden);

    // Header
    {
        auto header = std::make_unique<UIElement>();
        header->Overrides()
            .Set(Style::PaddingTop, StyleLength::Px(16.0f))
            .Set(Style::PaddingRight, StyleLength::Px(20.0f))
            .Set(Style::PaddingBottom, StyleLength::Px(14.0f))
            .Set(Style::PaddingLeft, StyleLength::Px(20.0f))
            .Set(Style::BackgroundColor, (uint32_t)0xFF1E1E1E)
            .Set(Style::BorderBottomWidth, 1.0f)
            .Set(Style::BorderBottomColor, (uint32_t)0xFF2E2E2E);
        auto titleLabel = std::make_unique<Label>();
        titleLabel->SetText("Import Settings");
        titleLabel->Overrides()
            .Set(Style::Color, (uint32_t)0xFFEDEDED)
            .Set(Style::FontSize, StyleLength::Px(16.0f))
            .Set(Style::FontWeight, 600);
        header->AddChild(std::move(titleLabel));
        window->AddChild(std::move(header));
    }

    // Scrollable checkbox list
    auto listScroll = std::make_unique<ScrollView>();
    listScroll->Overrides()
        .Set(Style::MaxHeight, StyleLength::Px(280.0f))
        .Set(Style::PaddingTop, StyleLength::Px(8.0f))
        .Set(Style::PaddingBottom, StyleLength::Px(8.0f))
        .Set(Style::PaddingLeft, StyleLength::Px(20.0f))
        .Set(Style::PaddingRight, StyleLength::Px(20.0f));

    // Keep live checkbox pointers so the footer can read them
    std::vector<Checkbox*> checkboxPtrs;
    checkboxPtrs.reserve(presentGroups.size());

    for (auto& pg : presentGroups)
    {
        const auto& group = kExportGroups[pg.index];

        auto row = std::make_unique<UIElement>();
        row->Overrides()
            .Set(Style::Display, DisplayMode::Flex)
            .Set(Style::FlexDir, FlexDirection::Row)
            .Set(Style::AlignItems, AlignItems::Center)
            .Set(Style::PaddingTop, StyleLength::Px(6.0f))
            .Set(Style::PaddingBottom, StyleLength::Px(6.0f));

        auto cb = std::make_unique<Checkbox>();
        cb->SetText(group.label);
        cb->SetChecked(true);
        checkboxPtrs.push_back(cb.get());
        row->AddChild(std::move(cb));
        listScroll->AddContent(std::move(row));
    }
    window->AddChild(std::move(listScroll));

    // Footer
    auto fired = std::make_shared<bool>(false);
    auto close = [overlayRaw, fired]()
    {
        if (*fired) return;
        *fired = true;
        if (auto* parent = overlayRaw->GetParent())
            parent->RemoveChild(overlayRaw);
    };

    // Capture data needed by the import action
    auto sharedDoc     = std::make_shared<nlohmann::json>(std::move(exportDoc));
    auto sharedGroups  = std::make_shared<std::vector<PresentGroup>>(std::move(presentGroups));
    auto sharedCbs     = std::make_shared<std::vector<Checkbox*>>(std::move(checkboxPtrs));

    {
        auto footer = std::make_unique<UIElement>();
        footer->Overrides()
            .Set(Style::Display, DisplayMode::Flex)
            .Set(Style::FlexDir, FlexDirection::Row)
            .Set(Style::JustifyContent, JustifyContent::FlexEnd)
            .Set(Style::Gap, StyleLength::Px(8.0f))
            .Set(Style::PaddingTop, StyleLength::Px(14.0f))
            .Set(Style::PaddingRight, StyleLength::Px(20.0f))
            .Set(Style::PaddingBottom, StyleLength::Px(14.0f))
            .Set(Style::PaddingLeft, StyleLength::Px(20.0f))
            .Set(Style::BackgroundColor, (uint32_t)0xFF1E1E1E)
            .Set(Style::BorderTopWidth, 1.0f)
            .Set(Style::BorderTopColor, (uint32_t)0xFF2E2E2E);

        auto cancelBtn = std::make_unique<Button>();
        cancelBtn->SetText("Cancel");
        cancelBtn->AddClass("secondary");
        cancelBtn->RegisterEventHandler(kEventButtonClick, [close](UIEvent&) { close(); });
        footer->AddChild(std::move(cancelBtn));

        auto importBtn = std::make_unique<Button>();
        importBtn->SetText("Import");
        importBtn->RegisterEventHandler(kEventButtonClick, [this, close, sharedDoc, sharedGroups, sharedCbs](UIEvent&)
        {
            close();

            auto prefsStore = Editor::OpenEditorPreferences();
            prefsStore.Load();
            const auto& workspaceRoot = EngineCore::GetInstance().GetWorkspaceRoot();
            auto userProjStore = workspaceRoot.empty()
                                     ? std::optional<Editor::SettingsStore>{}
                                     : std::optional<Editor::SettingsStore>{Editor::OpenUserProjectSettings(workspaceRoot)};
            if (userProjStore)
                userProjStore->Load();

            bool prefsModified    = false;
            bool userProjModified = false;

            for (size_t i = 0; i < sharedGroups->size(); ++i)
            {
                if (!(*sharedCbs)[i]->IsChecked())
                    continue;

                const auto& group = kExportGroups[(*sharedGroups)[i].index];
                if (!sharedDoc->contains(group.storeKey))
                    continue;
                const auto& storeJson = (*sharedDoc)[group.storeKey];
                if (!storeJson.is_object())
                    continue;

                const bool inUserProj = (std::string_view(group.storeKey) == "userProjectSettings");

                for (auto& [key, val] : storeJson.items())
                {
                    if (!KeyMatchesGroup(key, group))
                        continue;

                    if (inUserProj)
                    {
                        if (userProjStore)
                        {
                            userProjStore->SetJson(key, val);
                            userProjModified = true;
                        }
                    }
                    else
                    {
                        prefsStore.SetJson(key, val);
                        prefsModified = true;
                    }
                }
            }

            if (prefsModified)
                prefsStore.Save();
            if (userProjModified && userProjStore)
                userProjStore->Save();

            // Refresh current panel view so controls show imported values
            ShowCategoryContent(m_CurrentCategory);
        });
        footer->AddChild(std::move(importBtn));

        window->AddChild(std::move(footer));
    }

    overlay->AddChild(std::move(window));
    panelHost->AddChild(std::move(overlay));
}

} // namespace GameEngine
