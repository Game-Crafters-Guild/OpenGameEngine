#include "Editor/Settings/LodSettingsPage.h"

#include "Core/Engine.h"
#include "Editor/Settings/EditorSettingsRegistry.h"
#include "Editor/Settings/LodProjectSettingsWriter.h"
#include "Engine/Rendering/LodProjectSettings.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"

#include <string>
#include <utility>

namespace GameEngine::Editor
{
namespace
{
// Dropdown labels double as the stored value handed to Set, so the file token
// mapping happens here rather than in the panel.
constexpr const char* kOffLabel = "Off";
constexpr const char* kSseLabel = "Screen-Space Error";
constexpr const char* kCoverageLabel = "Coverage (screen fraction)";

constexpr float kErrorBudgetStepPx = 0.5f;
constexpr float kSkinnedBudgetScaleStep = 0.01f;
constexpr float kBudgetPercentStep = 5.0f;
constexpr float kCrossfadeDurationStep = 0.05f;
constexpr float kHysteresisBandStep = 0.05f;

const char* ModeLabel(Rendering::LodSelectionMode mode)
{
    switch (mode)
    {
        case Rendering::LodSelectionMode::Off: return kOffLabel;
        case Rendering::LodSelectionMode::Coverage: return kCoverageLabel;
        case Rendering::LodSelectionMode::Sse: break;
    }
    return kSseLabel;
}

Rendering::LodSelectionMode ModeFromLabel(const std::string& label)
{
    if (label == kOffLabel)
        return Rendering::LodSelectionMode::Off;
    if (label == kCoverageLabel)
        return Rendering::LodSelectionMode::Coverage;
    return Rendering::LodSelectionMode::Sse;
}

const std::filesystem::path& WorkspaceRoot()
{
    return EngineCore::GetInstance().GetWorkspaceRoot();
}

// Persist the mutated settings and push them at the live renderer. Every row
// funnels through here so the file and the running frame can never disagree
// about what the page last committed.
void CommitAndApply(const Rendering::LodProjectSettings& settings, const char* what)
{
    if (!SaveLodProjectSettings(WorkspaceRoot(), settings))
        Logger::Log::Warning("LodSettings: cannot save {} -- no writable project settings", what);
    if (auto* renderServices = EngineCore::GetInstance().GetRenderServices())
        settings.ApplyTo(*renderServices);
}

// The enable + percent row pair for one view class. Both classes take the same
// shape; only the label and which member they read and write differ.
//
// There is no asset-preview / thumbnail class here, and that is not an
// oversight: a thumbnail scales the MODEL to fit the frame at a fixed camera
// distance, so its projected coverage is exactly the framing multiplier
// (1.0 in the grid, 1.2 in the preview pane) whatever the mesh's real size.
// That sits above kLodThresholdCeil, so LOD0 wins at every budget and a
// percent knob there would move nothing.
void AppendViewClassRows(SettingsCategoryDescriptor& category, const char* className,
                         Rendering::LodViewBudgetOverride Rendering::LodProjectSettings::*member,
                         const char* searchKeywords)
{
    SettingsFieldDescriptor enabled;
    enabled.Label = std::string("Override ") + className + " Budget";
    enabled.Tooltip =
        std::string("Screen-Space Error only. When off, ") + className +
        " spends the shared LOD Error Budget above. When on, it spends the percentage of that "
        "budget set below, so the two can hold different amounts of detail in the same scene.";
    enabled.SearchKeywords = searchKeywords;
    SettingsFieldDescriptor::ToggleField enabledToggle;
    enabledToggle.DefaultValue = false;
    enabledToggle.Get = [member]()
    { return (Rendering::LodProjectSettings::Load(WorkspaceRoot()).*member).Enabled; };
    enabledToggle.Set = [member](bool value)
    {
        Rendering::LodProjectSettings settings = Rendering::LodProjectSettings::Load(WorkspaceRoot());
        // The row fires once with the value it was seeded from, so an unchanged
        // value must not write the file: opening the page is not an edit.
        if ((settings.*member).Enabled == value)
            return;
        (settings.*member).Enabled = value;
        CommitAndApply(settings, "per-view LOD budget override");
    };
    enabled.Control = enabledToggle;
    category.Fields.push_back(std::move(enabled));

    SettingsFieldDescriptor percent;
    percent.Label = std::string(className) + " Budget Percent";
    percent.Tooltip =
        std::string("Screen-Space Error only, and only while the override above is on. ") +
        "100% spends the shared LOD Error Budget unchanged; lower keeps more detail in " +
        className + ", higher coarsens it sooner.";
    percent.SearchKeywords = searchKeywords;
    SettingsFieldDescriptor::SliderField percentSlider;
    percentSlider.DefaultValue = Rendering::kDefaultLodBudgetPercent;
    percentSlider.MinValue = Rendering::LodProjectSettings::kMinBudgetPercent;
    percentSlider.MaxValue = Rendering::LodProjectSettings::kMaxBudgetPercent;
    percentSlider.Step = kBudgetPercentStep;
    percentSlider.Get = [member]()
    { return (Rendering::LodProjectSettings::Load(WorkspaceRoot()).*member).BudgetPercent; };
    percentSlider.Set = [member](float value)
    {
        Rendering::LodProjectSettings settings = Rendering::LodProjectSettings::Load(WorkspaceRoot());
        if ((settings.*member).BudgetPercent == value)
            return;
        (settings.*member).BudgetPercent = value;
        CommitAndApply(settings, "per-view LOD budget percent");
    };
    percent.Control = percentSlider;
    category.Fields.push_back(std::move(percent));
}
} // namespace

void RegisterLodSettingsCategory()
{
    SettingsCategoryDescriptor lod;
    lod.CategoryId = "levelOfDetail";
    lod.Title = "Level of Detail";
    lod.Group = SettingsCategoryGroup::ProjectSettings;
    lod.TreeRowClass = "level-of-detail-row";
    lod.Description =
        "Screen-Space Error picks each mesh level from the on-screen size of the error that level "
        "would introduce, so a level engages at the same pixel size whatever the viewport "
        "resolution. Coverage picks levels from the fraction of the screen a mesh covers, without "
        "regard for how much error the level actually introduces; it is kept so the two can be "
        "compared in one session. Off draws LOD0 everywhere, directional shadow cascades "
        "included, which costs full geometry in the shadow passes as well as the camera. "
        "Already-cached point-light shadow faces are the exception: they keep the levels they "
        "were last drawn with until something else re-renders them. Off does not affect HLOD, "
        "which swaps far-field proxy entities before rendering ever sees them. All three apply "
        "immediately, and switching re-derives every registered mesh row and re-uploads the mesh "
        "table -- there is no per-frame cost that depends on which one is selected. "
        "The Game View and Scene View overrides below layer on top: while an override is off "
        "that view spends the shared error budget unchanged, and while it is on it spends the "
        "given percentage of it. Scene View means every scene viewport -- the main one, quad "
        "panes and floating windows alike. Asset previews and thumbnails are not listed because "
        "they frame each model to fill the view, which already pins them to LOD0 at any budget.";

    SettingsFieldDescriptor mode;
    mode.Label = "Mesh LOD Selection";
    mode.Tooltip =
        "Which mapping seeds each mesh's LOD switch points. Off pins every mesh to LOD0 in every "
        "view. This is not the same as an error budget of 0, which only keeps detail on levels "
        "that carry a screen-space-error metric.";
    mode.SearchKeywords = "lod level of detail mesh sse screen space error coverage off disable";
    // No PrefKey: this is a project setting (rendering.lodMode), not an editor
    // preference, so the panel must not mirror it into Preferences.json.
    SettingsFieldDescriptor::DropdownField modeDropdown;
    modeDropdown.Options = {kOffLabel, kSseLabel, kCoverageLabel};
    modeDropdown.DefaultValue = kSseLabel;
    modeDropdown.Get = []() -> std::string
    { return ModeLabel(Rendering::LodProjectSettings::Load(WorkspaceRoot()).SelectionMode); };
    modeDropdown.Set = [](const std::string& label)
    {
        Rendering::LodProjectSettings settings = Rendering::LodProjectSettings::Load(WorkspaceRoot());
        const auto mode = ModeFromLabel(label);
        // Registry contract: a persisting Set returns without writing when the
        // value it is handed already matches what is stored.
        if (settings.SelectionMode == mode)
            return;
        settings.SelectionMode = mode;
        CommitAndApply(settings, "LOD selection mode");
    };
    mode.Control = modeDropdown;
    lod.Fields.push_back(std::move(mode));

    SettingsFieldDescriptor budget;
    budget.Label = "LOD Error Budget (px)";
    budget.Tooltip =
        "Screen-Space Error only. How many render-target pixels of projected simplification error "
        "a mesh may spend before switching to the next level -- raise it to coarsen everything "
        "proportionally. 0 keeps full detail on error-metric levels but still lets authored and "
        "sloppy levels switch; use Off for a true LOD0-everywhere.";
    budget.SearchKeywords = "lod error budget pixels sse screen space quality distance";
    SettingsFieldDescriptor::SliderField budgetSlider;
    budgetSlider.DefaultValue = Rendering::kDefaultLodErrorBudgetPx;
    budgetSlider.MinValue = Rendering::LodProjectSettings::kMinErrorBudgetPx;
    budgetSlider.MaxValue = Rendering::LodProjectSettings::kMaxErrorBudgetPx;
    budgetSlider.Step = kErrorBudgetStepPx;
    budgetSlider.Get = []()
    { return Rendering::LodProjectSettings::Load(WorkspaceRoot()).EffectiveErrorBudgetPx(); };
    budgetSlider.Set = [](float value)
    {
        Rendering::LodProjectSettings settings = Rendering::LodProjectSettings::Load(WorkspaceRoot());
        // The row fires its callback once with the value it was seeded from, so
        // an unchanged value must not write the file or overwrite a runtime
        // override: opening the page is not an edit.
        if (settings.EffectiveErrorBudgetPx() == value)
            return;
        settings.ErrorBudgetPx = value;
        CommitAndApply(settings, "LOD error budget");
    };
    budget.Control = budgetSlider;
    lod.Fields.push_back(std::move(budget));

    SettingsFieldDescriptor skinned;
    skinned.Label = "Skinned Budget Scale";
    skinned.Tooltip =
        "Screen-Space Error only. Multiplies the error budget for skinned and character meshes, "
        "whose silhouette erosion reads well before a prop's does. Lower keeps characters "
        "detailed for longer.";
    skinned.SearchKeywords = "lod skinned character budget scale sse tight class";
    SettingsFieldDescriptor::SliderField skinnedSlider;
    skinnedSlider.DefaultValue = Rendering::kDefaultLodSkinnedBudgetScale;
    skinnedSlider.MinValue = Rendering::LodProjectSettings::kMinSkinnedBudgetScale;
    skinnedSlider.MaxValue = Rendering::LodProjectSettings::kMaxSkinnedBudgetScale;
    skinnedSlider.Step = kSkinnedBudgetScaleStep;
    skinnedSlider.Get = []()
    { return Rendering::LodProjectSettings::Load(WorkspaceRoot()).EffectiveSkinnedBudgetScale(); };
    skinnedSlider.Set = [](float value)
    {
        Rendering::LodProjectSettings settings = Rendering::LodProjectSettings::Load(WorkspaceRoot());
        if (settings.EffectiveSkinnedBudgetScale() == value)
            return;
        settings.SkinnedBudgetScale = value;
        CommitAndApply(settings, "skinned LOD budget scale");
    };
    skinned.Control = skinnedSlider;
    lod.Fields.push_back(std::move(skinned));

    // Scene View covers EVERY scene viewport (main, quad panes, floating
    // windows) — that is the granularity Rendering::ViewPurpose distinguishes.
    AppendViewClassRows(lod, "Game View", &Rendering::LodProjectSettings::GameViewBudget,
                        "lod game view budget override percent sse screen space error per view");
    AppendViewClassRows(lod, "Scene View", &Rendering::LodProjectSettings::SceneViewBudget,
                        "lod scene view budget override percent sse screen space error per view");

    SettingsFieldDescriptor crossfade;
    crossfade.Label = "LOD Crossfade (s)";
    crossfade.Tooltip =
        "How long a mesh takes to dissolve between levels instead of popping: both levels draw "
        "for this many seconds, each keeping a complementary half of a screen-space dither "
        "pattern. 0 is off and costs nothing -- no extra records, no extra shader variant. "
        "A transitioning mesh runs that same dither in the depth prepass too, so it stays "
        "correct for everything that reads scene depth (ambient occlusion, fog, reflections); "
        "for the length of its fade it loses early-Z and shades both levels. Shadow casters "
        "never crossfade: a dithered caster punches holes in its shadow map, which the filter "
        "averages into a washed-out shadow.";
    crossfade.SearchKeywords = "lod crossfade fade dither transition pop popping blend dissolve";
    SettingsFieldDescriptor::SliderField crossfadeSlider;
    crossfadeSlider.DefaultValue = Rendering::LodProjectSettings::kDefaultCrossfadeDuration;
    crossfadeSlider.MinValue = Rendering::LodProjectSettings::kMinCrossfadeDuration;
    crossfadeSlider.MaxValue = Rendering::LodProjectSettings::kMaxCrossfadeDuration;
    crossfadeSlider.Step = kCrossfadeDurationStep;
    crossfadeSlider.Get = []()
    { return Rendering::LodProjectSettings::Load(WorkspaceRoot()).EffectiveCrossfadeDuration(); };
    crossfadeSlider.Set = [](float value)
    {
        Rendering::LodProjectSettings settings = Rendering::LodProjectSettings::Load(WorkspaceRoot());
        if (settings.EffectiveCrossfadeDuration() == value)
            return;
        settings.CrossfadeDuration = value;
        CommitAndApply(settings, "LOD crossfade duration");
    };
    crossfade.Control = crossfadeSlider;
    lod.Fields.push_back(std::move(crossfade));

    SettingsFieldDescriptor hysteresis;
    hysteresis.Label = "LOD Hysteresis Band";
    hysteresis.Tooltip =
        "Dead band for LOD selection, as a fraction of each switch point: gaining detail requires "
        "the on-screen size to clear a level's threshold by this much extra, while holding or "
        "losing detail requires only the threshold. Stops a camera hovering at a threshold from "
        "flipping an object between two levels every frame; a single switch is unaffected. 0 is "
        "off (stateless selection); 0.25 is the measured sweet spot. Camera views only -- shadows "
        "keep stateless selection. Note: a non-zero band makes selection depend on the previous "
        "frame, so captures are no longer reproducible from camera pose alone.";
    hysteresis.SearchKeywords = "lod hysteresis dwell band thrash flicker ping pong oscillate stability";
    SettingsFieldDescriptor::SliderField hysteresisSlider;
    hysteresisSlider.DefaultValue = Rendering::LodProjectSettings::kDefaultHysteresisBand;
    hysteresisSlider.MinValue = Rendering::LodProjectSettings::kMinHysteresisBand;
    hysteresisSlider.MaxValue = Rendering::LodProjectSettings::kMaxHysteresisBand;
    hysteresisSlider.Step = kHysteresisBandStep;
    hysteresisSlider.Get = []()
    { return Rendering::LodProjectSettings::Load(WorkspaceRoot()).EffectiveHysteresisBand(); };
    hysteresisSlider.Set = [](float value)
    {
        Rendering::LodProjectSettings settings = Rendering::LodProjectSettings::Load(WorkspaceRoot());
        if (settings.EffectiveHysteresisBand() == value)
            return;
        settings.HysteresisBand = value;
        CommitAndApply(settings, "LOD hysteresis band");
    };
    hysteresis.Control = hysteresisSlider;
    lod.Fields.push_back(std::move(hysteresis));

    EditorSettingsRegistry::Get().RegisterCategory(std::move(lod));
}

} // namespace GameEngine::Editor
