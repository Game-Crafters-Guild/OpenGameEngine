#pragma once

#include "Graph/GraphBlendSpace1DStore.h"
#include "UI/UIElement.h"

#include <functional>
#include <string>
#include <vector>

namespace GameEngine {

class AssetField;

class BlendSpace1DEditor : public UIElement
{
public:
    using UndoScopeFn = std::function<void(const std::string& actionName, std::function<void()> mutate)>;

    BlendSpace1DEditor();

    void SetSamples(std::vector<BlendSpace1DSampleDesc> samples);
    const std::vector<BlendSpace1DSampleDesc>& GetSamples() const { return m_Samples; }
    void SetParameterName(std::string name);
    const std::string& GetParameterName() const { return m_ParameterName; }
    void SetPreviewPosition(bool active, float position);
    void SetUndoScope(UndoScopeFn fn) { m_UndoScope = std::move(fn); }
    void SetOnChanged(std::function<void()> fn) { m_OnChanged = std::move(fn); }
    int GetSelectedIndex() const { return m_SelectedIndex; }

    void OnEvent(UIEvent& e) override;
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

private:
    struct TrackLayout
    {
        float Left = 0.f;
        float Width = 0.f;
        float CenterY = 0.f;
    };

    TrackLayout MakeTrackLayout() const;
    float ClipStripHeight() const;
    bool HitClipStrip(float localY) const;
    const BlendSpace1DAxis& ActiveAxis() const;
    int HitSample(float localX, float localY, const TrackLayout& track) const;
    bool HitTrack(float localX, float localY, const TrackLayout& track) const;
    void NotifyChanged();
    void CommitSamples(const std::string& actionName, std::function<void()> mutate);
    void RecomputeDisplayAxis();
    void SyncClipField();
    void EnsureClipFieldRegistry();

    std::vector<BlendSpace1DSampleDesc> m_Samples;
    BlendSpace1DAxis m_DisplayAxis;
    BlendSpace1DAxis m_DragAxis;
    std::string m_ParameterName{"Speed"};
    bool m_PreviewActive = false;
    float m_PreviewPosition = 0.f;
    UndoScopeFn m_UndoScope;
    std::function<void()> m_OnChanged;
    AssetField* m_ClipField = nullptr;
    int m_SelectedIndex = -1;
    int m_PressedIndex = -1;
    bool m_Dragging = false;
    bool m_PendingAdd = false;
    bool m_UpdatingClipField = false;
    float m_PressX = 0.f;
    float m_DragStartPosition = 0.f;
};

} // namespace GameEngine
