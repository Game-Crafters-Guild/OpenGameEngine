#include "Panels/MixerPanel.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "AssetCore/AssetTypes.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "Editor/Settings/SettingsStore.h"
#include "Audio/AudioSystem.h"
#include "UI/Assets/UIStyleAsset.h"
#include "UI/UIElement.h"
#include "UI/Controls/Label.h"
#include "UI/UIStyle.h"
#include "UI/UIManager.h"
#include "UI/UIEvents.h"
#include "UI/StyleProperties.h"

#include <cmath>
#include <algorithm>
#include <filesystem>
#include <string>

namespace GameEngine {

namespace {

// Strip count and order: first 5 are sub-buses, last is Master. Must match MiniaudioBackend bus IDs.
constexpr int kMixerStripCount = 6;
// Labels match backend bus names: 0=Master, 1=Music, 2=SFX, 3=UI, 4=VO, 5=Aux
const char* kStripLabels[] = { "Music", "SFX", "UI", "VO", "Aux", "Master" };
// Strip index i -> AudioBusId. Order: Music(1), SFX(2), UI(3), VO(4), Aux(5), Master(0)
const Audio::AudioBusId kStripBusIds[] = { 1, 2, 3, 4, 5, 0 };

// Default levels: 0 dB (unity gain). Linear 0..1 from dB: 10^(dB/20), so 0 dB = 1.0
const float kDefaultLinearLevels[] = {
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f  // Music, SFX, UI, VO, Aux, Master
};

std::string MixerBusPrefKey(int stripIndex)
{
    return "mixer.bus." + std::to_string(stripIndex);
}

} // namespace

MixerPanel::MixerPanel()
    : DockPanel("Mixer")
{
    BuildUI();
}

MixerPanel::~MixerPanel()
{
    // The pending asset callbacks capture this panel; withdraw them before it goes
    // away (destroying an AssetLoadHandle does not).
    if (m_PanelStyleLoadHandle)
        m_PanelStyleLoadHandle->Cancel();
}

void MixerPanel::OnPostLayout()
{
    if (!m_StyleAttached && !m_StyleLoadScheduled && GetOwnerManager())
    {
        m_StyleLoadScheduled = true;
        PostAction([this]() { LoadAndAttachPanelStyle(); });
    }
    if (!m_InitialSyncDone)
        m_InitialSyncDone = true;
    SyncFadersFromAudio();  // meter (VU bar) + volume (thumb)
    // Start recurring refresh once so VU bar updates in real time (decay visible)
    if (!m_VuRefreshChainStarted)
    {
        m_VuRefreshChainStarted = true;
        PostAction([this]() {
            if (GetOwnerManager())
                ScheduleNextVuRefresh();
        });
    }
}

void MixerPanel::ScheduleNextVuRefresh()
{
    SyncFadersFromAudio();
    MarkDirty(UIElement::VisualDirty);
    if (GetOwnerManager())
        PostAction([this]() { ScheduleNextVuRefresh(); });
}

void MixerPanel::LoadAndAttachPanelStyle()
{
    m_StyleLoadScheduled = false;
    UIManager* ui = GetOwnerManager();
    if (!ui)
        return;
    auto& am = EngineCore::GetInstance().GetAssetManager();
    const std::filesystem::path styleAssetPath = std::filesystem::path("UI") / "panels" / "MixerPanel.css";
    const GUID styleGuid = am.ResolveAssetGuid(styleAssetPath, GameEngine::kAssetSourceAliasEditor);
    if (styleGuid.IsNull() || m_PanelStyleLoadHandle)
        return;
    m_PanelStyleLoadHandle = std::make_unique<AssetLoadHandle>(
        am.LoadAsset(styleGuid,
                    [this, post = GetPostHandle(), styleGuid](Result<SharedPtr<Asset>, AssetError> r)
                    {
                        if (!r.IsOk() || !r.Value() || r.Value()->GetType() != AssetType::UIStyle)
                        {
                            post.Post([this]() { m_PanelStyleLoadHandle.reset(); });
                            return;
                        }
                        post.Post([this, styleGuid]()
                        {
                            UIManager* ui2 = GetOwnerManager();
                            if (!ui2)
                                return;
                            auto& am2 = EngineCore::GetInstance().GetAssetManager();
                            auto a2 = am2.GetAsset(styleGuid);
                            if (a2 && a2->GetType() == AssetType::UIStyle)
                            {
                                (void)ui2->AttachStyleToSubtreeFromAsset(
                                    this, *static_cast<UIStyleAsset*>(a2.get()));
                                m_StyleAttached = true;
                            }
                        });
                    },
                    AssetLoadPriority::High));
}

namespace {
constexpr float kVuThumbHeightPx = 8.0f;
constexpr float kVuDefaultTrackHeightPx = 220.0f;  // fallback when layout height not yet available
} // namespace

void MixerPanel::SyncFadersFromAudio()
{
    Audio::AudioSystem* audio = EngineCore::GetInstance().GetAudioSystem();
    if (!audio || !audio->IsInitialized())
        return;
    for (size_t i = 0; i < m_VuMeterFills.size() && i < m_FaderBusIds.size(); ++i)
    {
        const Audio::AudioBusId bus = m_FaderBusIds[i];
        const float meter = std::clamp(audio->GetBusMeterLevel(bus), 0.0f, 1.0f);
        const float volume = std::clamp(audio->GetBusVolume(bus), 0.0f, 1.0f);

        // Use actual track height so VU and thumb scale when panel is resized vertically
        UIElement* fill = m_VuMeterFills[i];
        UIElement* track = fill ? fill->GetParent() : nullptr;
        const float trackH = (track && track->GetLayoutHeight() > 0.0f)
            ? track->GetLayoutHeight()
            : kVuDefaultTrackHeightPx;
        const float thumbRange = std::max(0.0f, trackH - kVuThumbHeightPx);

        if (fill)
        {
            const float fillPct = meter * 100.0f;  // fill height as % of track (track is parent)
            fill->Overrides().Set(Style::Height, StyleLength::Percent(fillPct));
            fill->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        }
        if (i < m_VuMeterStrips.size() && m_VuMeterStrips[i])
        {
            const float stripPct = (meter > 0.001f) ? (100.0f / meter) : 1000.0f;
            m_VuMeterStrips[i]->Overrides().Set(Style::Height, StyleLength::Percent(stripPct));
            m_VuMeterStrips[i]->MarkDirty(UIElement::VisualDirty);
        }
        if (i < m_VuMeterThumbs.size() && m_VuMeterThumbs[i])
        {
            UIElement* thumb = m_VuMeterThumbs[i];
            thumb->Overrides().Set(Style::MarginTop, StyleLength::Px((1.0f - volume) * thumbRange));
            thumb->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
            // Thumb overlay layer must match track height so it overlaps correctly when resized
            UIElement* thumbLayer = thumb->GetParent();
            if (thumbLayer)
            {
                thumbLayer->Overrides().Set(Style::Height, StyleLength::Px(trackH)).Set(Style::MarginTop, StyleLength::Px(-trackH));
                thumbLayer->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
            }
        }
    }
}

void MixerPanel::BuildUI()
{
    auto container = std::make_unique<UIElement>();
    container->AddClass("mixer-panel");
    m_MainContainer = container.get();

    auto row = std::make_unique<UIElement>();
    row->AddClass("mixer-strips-row");
    row->Overrides()
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::FlexDir, FlexDirection::Row)
        .Set(Style::AlignItems, AlignItems::Stretch)
        .Set(Style::JustifyContent, JustifyContent::FlexStart)
        .Set(Style::Gap, StyleLength::Px(10.0f))
        .Set(Style::FlexGrow, 1.0f)
        .Set(Style::MinHeight, StyleLength::Px(220.0f));

    m_VuMeterFills.clear();
    m_VuMeterStrips.clear();
    m_VuMeterThumbs.clear();
    m_FaderBusIds.clear();

    for (int i = 0; i < kMixerStripCount; ++i)
    {
        bool isMaster = (i == kMixerStripCount - 1);
        Audio::AudioBusId busId = kStripBusIds[i];

        auto strip = std::make_unique<UIElement>();
        strip->AddClass("mixer-strip");
        if (i == 0)
            strip->AddClass("mixer-strip--first");
        if (isMaster)
            strip->AddClass("mixer-strip--master");

        // Strip fills row height (flex grow); min height so faders stay usable.
        constexpr float kStripMinHeightPx = 220.0f;
        strip->Overrides()
            .Set(Style::Display, DisplayMode::Flex)
            .Set(Style::FlexDir, FlexDirection::Column)
            .Set(Style::AlignItems, AlignItems::Center)
            .Set(Style::Gap, StyleLength::Px(4.0f))
            .Set(Style::PaddingTop, StyleLength::Px(24.0f))
            .Set(Style::Width, StyleLength::Px(72.0f))
            .Set(Style::MinWidth, StyleLength::Px(72.0f))
            .Set(Style::MaxWidth, StyleLength::Px(72.0f))
            .Set(Style::FlexGrow, 1.0f)
            .Set(Style::MinHeight, StyleLength::Px(kStripMinHeightPx))
            .Set(Style::Height, StyleLength::Auto());

        // Row: dB scale + VU meter + fader; fills strip height (flex grow).
        auto scaleAndFaderRow = std::make_unique<UIElement>();
        scaleAndFaderRow->AddClass("mixer-scale-fader-row");
        scaleAndFaderRow->Overrides()
            .Set(Style::Display, DisplayMode::Flex)
            .Set(Style::FlexDir, FlexDirection::Row)
            .Set(Style::AlignItems, AlignItems::Stretch)
            .Set(Style::Gap, StyleLength::Px(4.0f))
            .Set(Style::FlexGrow, 1.0f)
            .Set(Style::MinHeight, StyleLength::Px(220.0f))
            .Set(Style::Height, StyleLength::Auto());

        // dB scale column: 0, -5, ... -50. Fixed width so labels don't shift; stretches to same height as VU and fader.
        auto scaleCol = std::make_unique<UIElement>();
        scaleCol->AddClass("mixer-db-scale");
        scaleCol->Overrides()
            .Set(Style::Display, DisplayMode::Flex)
            .Set(Style::FlexDir, FlexDirection::Column)
            .Set(Style::JustifyContent, JustifyContent::SpaceBetween)
            .Set(Style::PaddingLeft, StyleLength::Px(4.0f))
            .Set(Style::PaddingRight, StyleLength::Px(4.0f))
            .Set(Style::Width, StyleLength::Px(24.0f))
            .Set(Style::MinWidth, StyleLength::Px(24.0f))
            .Set(Style::MaxWidth, StyleLength::Px(24.0f))
            .Set(Style::FlexGrow, 0.0f)
            .Set(Style::MinHeight, StyleLength::Px(220.0f))
            .Set(Style::Height, StyleLength::Auto());
        const int dBValues[] = { 0, -5, -10, -15, -20, -25, -30, -35, -40, -45, -50 };
        for (int d : dBValues)
        {
            auto lbl = std::make_unique<Label>();
            lbl->AddClass("mixer-db-label");
            if (d % 10 == 0)
                lbl->AddClass("mixer-db-label-major");  // 0, -10, -20, -30, -40, -50 = white
            else
                lbl->AddClass("mixer-db-label-minor");  // -5, -15, -25, -35, -45 = gray
            lbl->SetText(std::to_string(d));
            scaleCol->AddChild(std::move(lbl));
        }
        scaleAndFaderRow->AddChild(std::move(scaleCol));

        // VU meter (narrow bar, same height as fader). Thumb at fill level; drag thumb/bar to set volume.
        auto vuContainer = std::make_unique<UIElement>();
        UIElement* vuContainerRaw = vuContainer.get();
        vuContainer->AddClass("mixer-vu-container");
        if (isMaster)
            vuContainer->AddClass("mixer-vu-container--master");
        // Fill anchored at bottom (FlexEnd); track height is flexible, initial values use default
        constexpr float kVuThumbPx = 8.0f;
        constexpr float kVuThumbWidthPx = 16.0f;
        vuContainer->Overrides()
            .Set(Style::Display, DisplayMode::Flex)
            .Set(Style::FlexDir, FlexDirection::Column)
            .Set(Style::JustifyContent, JustifyContent::FlexEnd)
            .Set(Style::AlignItems, AlignItems::Center)
            .Set(Style::OverflowProp, Overflow::Visible)
            .Set(Style::Width, StyleLength::Px(24.0f))
            .Set(Style::MinWidth, StyleLength::Px(24.0f))
            .Set(Style::MaxWidth, StyleLength::Px(24.0f))
            .Set(Style::FlexGrow, 0.0f)
            .Set(Style::MinHeight, StyleLength::Px(220.0f))
            .Set(Style::Height, StyleLength::Auto());
        float initialLevel = kDefaultLinearLevels[i];
        {
            Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            double saved = 0.0;
            if (prefs.TryGetDouble(MixerBusPrefKey(i), saved))
                initialLevel = static_cast<float>(std::clamp(saved, 0.0, 1.0));
        }
        float initialMeter = 0.0f;

        // Layer 1: track (meter bar only) – fills container height so it resizes with panel
        auto vuTrack = std::make_unique<UIElement>();
        vuTrack->Overrides()
            .Set(Style::Display, DisplayMode::Flex)
            .Set(Style::FlexDir, FlexDirection::Column)
            .Set(Style::JustifyContent, JustifyContent::FlexEnd)
            .Set(Style::Width, StyleLength::Px(6.0f))
            .Set(Style::MinWidth, StyleLength::Px(6.0f))
            .Set(Style::MinHeight, StyleLength::Px(0.0f))
            .Set(Style::FlexGrow, 1.0f)
            .Set(Style::BackgroundColor, (uint32_t)0xFF151515u)
            .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{3.0f, 3.0f, 3.0f, 3.0f});
        auto vuClip = std::make_unique<UIElement>();
        vuClip->AddClass("mixer-vu-clip");
        const float fillHeightPct = initialMeter * 100.0f;  // % of track (track fills container)
        vuClip->Overrides()
            .Set(Style::Display, DisplayMode::Flex)
            .Set(Style::FlexDir, FlexDirection::Column)
            .Set(Style::JustifyContent, JustifyContent::FlexEnd)
            .Set(Style::OverflowProp, Overflow::Hidden)
            .Set(Style::Width, StyleLength::Px(6.0f))
            .Set(Style::MinWidth, StyleLength::Px(6.0f))
            .Set(Style::Height, StyleLength::Percent(fillHeightPct))
            .Set(Style::MinHeight, StyleLength::Px(2.0f));
        const float initialStripPct = (initialMeter > 0.001f) ? (100.0f / initialMeter) : 1000.0f;
        auto gradientStrip = std::make_unique<UIElement>();
        gradientStrip->AddClass("mixer-vu-strip");
        gradientStrip->Overrides()
            .Set(Style::Display, DisplayMode::Flex)
            .Set(Style::FlexDir, FlexDirection::Column)
            .Set(Style::JustifyContent, JustifyContent::FlexEnd)
            .Set(Style::Width, StyleLength::Px(6.0f))
            .Set(Style::MinWidth, StyleLength::Px(6.0f))
            .Set(Style::Height, StyleLength::Percent(initialStripPct))
            .Set(Style::FlexGrow, 0.0f);
        auto segRed = std::make_unique<UIElement>();
        segRed->AddClass("mixer-vu-segment");
        segRed->Overrides().Set(Style::Height, StyleLength::Percent(6.0f)).Set(Style::FlexGrow, 0.0f).Set(Style::BackgroundColor, (uint32_t)0xFFef4444u);
        auto segYellow = std::make_unique<UIElement>();
        segYellow->AddClass("mixer-vu-segment");
        segYellow->Overrides().Set(Style::Height, StyleLength::Percent(24.0f)).Set(Style::FlexGrow, 0.0f).Set(Style::BackgroundColor, (uint32_t)0xFFeab308u);
        auto segGreen = std::make_unique<UIElement>();
        segGreen->AddClass("mixer-vu-segment");
        segGreen->Overrides().Set(Style::Height, StyleLength::Percent(70.0f)).Set(Style::FlexGrow, 0.0f).Set(Style::BackgroundColor, (uint32_t)0xFF22c55eu);
        gradientStrip->AddChild(std::move(segRed));
        gradientStrip->AddChild(std::move(segYellow));
        gradientStrip->AddChild(std::move(segGreen));
        UIElement* gradientStripPtr = gradientStrip.get();
        vuClip->AddChild(std::move(gradientStrip));
        UIElement* vuClipPtr = vuClip.get();
        vuTrack->AddChild(std::move(vuClip));
        vuContainer->AddChild(std::move(vuTrack));

        // Layer 2: thumb overlay – height/margin set in SyncFadersFromAudio from track height for responsiveness
        auto vuThumbLayer = std::make_unique<UIElement>();
        vuThumbLayer->Overrides()
            .Set(Style::Display, DisplayMode::Flex)
            .Set(Style::FlexDir, FlexDirection::Column)
            .Set(Style::JustifyContent, JustifyContent::FlexStart)
            .Set(Style::MarginTop, StyleLength::Px(-kVuDefaultTrackHeightPx))
            .Set(Style::Width, StyleLength::Px(6.0f))
            .Set(Style::MinWidth, StyleLength::Px(6.0f))
            .Set(Style::Height, StyleLength::Px(kVuDefaultTrackHeightPx))
            .Set(Style::MinHeight, StyleLength::Px(kVuDefaultTrackHeightPx))
            .Set(Style::FlexGrow, 0.0f);
        const float initialThumbRange = kVuDefaultTrackHeightPx - kVuThumbPx;
        auto vuThumb = std::make_unique<UIElement>();
        UIElement* vuThumbPtr = vuThumb.get();
        vuThumb->AddClass("mixer-vu-thumb");
        vuThumb->Overrides()
            .Set(Style::Height, StyleLength::Px(kVuThumbPx))
            .Set(Style::MinHeight, StyleLength::Px(kVuThumbPx))
            .Set(Style::FlexGrow, 0.0f)
            .Set(Style::Width, StyleLength::Px(kVuThumbWidthPx))
            .Set(Style::MinWidth, StyleLength::Px(kVuThumbWidthPx))
            .Set(Style::MarginTop, StyleLength::Px((1.0f - initialLevel) * initialThumbRange))
            .Set(Style::BackgroundColor, (uint32_t)0xFFE6E6E6u)
            .Set(Style::BackgroundImage, BackgroundImageSource{BackgroundImageSource::SourceKind::Path, GUID::Null(), "Icons/horizontalthumb.png"})
            .Set(Style::BackgroundTint, (uint32_t)0xFFE6E6E6u)
            .Set(Style::BackgroundSize, BackgroundSizeValue{BackgroundSizeMode::Explicit, kVuThumbWidthPx, false, kVuThumbPx, false})
            .Set(Style::BackgroundRepeatProp, BackgroundRepeat::NoRepeat)
            .Set(Style::BackgroundPosition, BackgroundPositionValue{50.0f, true, 50.0f, true});
        vuThumbLayer->AddChild(std::move(vuThumb));
        vuContainer->AddChild(std::move(vuThumbLayer));

        // Drag VU to set volume (thumb position); use current track height so position is correct when resized
        const Audio::AudioBusId capturedBus = busId;
        const int capturedStripIndex = i;
        auto applyLevel = [capturedBus, capturedStripIndex, vuThumbPtr, vuContainerRaw](UIElement*, float level)
        {
            float clamped = std::clamp(level, 0.0f, 1.0f);
            Audio::AudioSystem* sys = EngineCore::GetInstance().GetAudioSystem();
            if (sys && sys->IsInitialized())
                sys->SetBusVolume(capturedBus, clamped);
            Editor::SettingsStore prefs = Editor::OpenEditorPreferences();
            std::string err;
            prefs.Load(&err);
            prefs.SetDouble(MixerBusPrefKey(capturedStripIndex), static_cast<double>(clamped));
            prefs.Save(&err);
            if (vuThumbPtr)
            {
                UIElement* layer = vuThumbPtr->GetParent();
                float trackH = (layer && layer->GetLayoutHeight() > 0.0f) ? layer->GetLayoutHeight() : kVuDefaultTrackHeightPx;
                float range = std::max(0.0f, trackH - kVuThumbPx);
                vuThumbPtr->Overrides().Set(Style::MarginTop, StyleLength::Px((1.0f - clamped) * range));
                vuThumbPtr->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
            }
            if (vuContainerRaw)
                vuContainerRaw->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        };
        vuContainerRaw->RegisterEventHandler(kEventMouseDown, [applyLevel, vuContainerRaw](UIEvent& e)
        {
            if (e.Button != 0) return;
            float y = vuContainerRaw->GetLayoutY();
            float H = vuContainerRaw->GetLayoutHeight();
            if (H <= 0.0f) return;
            float localY = e.Y - y;
            float level = 1.0f - std::clamp(localY / H, 0.0f, 1.0f);
            applyLevel(vuContainerRaw, level);
            e.Capture(vuContainerRaw);
            e.Stop();
        });
        vuContainerRaw->RegisterEventHandler(kEventMouseMove, [applyLevel, vuContainerRaw](UIEvent& e)
        {
            UIManager* mgr = vuContainerRaw->GetOwnerManager();
            if (!mgr || !mgr->IsMouseDown()) return;
            float y = vuContainerRaw->GetLayoutY();
            float H = vuContainerRaw->GetLayoutHeight();
            if (H <= 0.0f) return;
            float localY = e.Y - y;
            float level = 1.0f - std::clamp(localY / H, 0.0f, 1.0f);
            applyLevel(vuContainerRaw, level);
            e.Stop();
        });
        vuContainerRaw->RegisterEventHandler(kEventMouseUp, [](UIEvent& e) { e.Stop(); });
        vuContainerRaw->RegisterEventHandler(kEventScroll, [applyLevel, capturedBus, vuContainerRaw](UIEvent& e)
        {
            constexpr float kScrollStep = 0.02f;
            Audio::AudioSystem* sys = EngineCore::GetInstance().GetAudioSystem();
            if (!sys || !sys->IsInitialized()) return;
            float current = sys->GetBusVolume(capturedBus);
            // scrollY is negative when scrolling up (content direction), so negate for volume
            float delta = (e.ScrollY < 0.0f) ? kScrollStep : -kScrollStep;
            float level = std::clamp(current + delta, 0.0f, 1.0f);
            applyLevel(vuContainerRaw, level);
            e.Stop();
        });

        scaleAndFaderRow->AddChild(std::move(vuContainer));
        m_VuMeterFills.push_back(vuClipPtr);
        m_VuMeterStrips.push_back(gradientStripPtr);
        m_VuMeterThumbs.push_back(vuThumbPtr);
        m_FaderBusIds.push_back(busId);

        // Set initial bus volume
        Audio::AudioSystem* audio = EngineCore::GetInstance().GetAudioSystem();
        if (audio && audio->IsInitialized())
            audio->SetBusVolume(busId, initialLevel);

        strip->AddChild(std::move(scaleAndFaderRow));

        // Channel label below the scale+fader row
        auto label = std::make_unique<Label>();
        label->AddClass("mixer-label");
        label->SetText(kStripLabels[i]);
        strip->AddChild(std::move(label));

        row->AddChild(std::move(strip));
    }

    container->AddChild(std::move(row));
    AddChild(std::move(container));
}

} // namespace GameEngine
