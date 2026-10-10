#pragma once

#include "UI/Controls/DockPanel.h"
#include "Audio/AudioTypes.h"
#include <memory>
#include <vector>

namespace GameEngine {

struct AssetLoadHandle;
namespace Audio { class AudioSystem; }
class UIElement;
class Label;

class MixerPanel : public DockPanel
{
public:
    std::string_view DeclaredTabIconClass() const override { return "mixer-icon"; }

    MixerPanel();
    ~MixerPanel() override;

    void OnPostLayout() override;

private:
    void BuildUI();
    void SyncFadersFromAudio();
    void ScheduleNextVuRefresh();  // sync meter/thumb then re-post so VU updates in real time
    void LoadAndAttachPanelStyle();

    UIElement* m_MainContainer = nullptr;
    std::vector<UIElement*> m_VuMeterFills;   // clip: height from meter level (real metering)
    std::vector<UIElement*> m_VuMeterStrips;  // gradient strip inside clip
    std::vector<UIElement*> m_VuMeterThumbs;  // thumb: position from volume (user-set)
    std::vector<Audio::AudioBusId> m_FaderBusIds;
    bool m_InitialSyncDone = false;
    bool m_VuRefreshChainStarted = false;
    bool m_StyleAttached = false;
    bool m_StyleLoadScheduled = false;
    std::unique_ptr<AssetLoadHandle> m_PanelStyleLoadHandle;
};

} // namespace GameEngine
