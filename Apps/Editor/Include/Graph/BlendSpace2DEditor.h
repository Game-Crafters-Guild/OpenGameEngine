#pragma once

#include "Graph/GraphBlendSpace2DStore.h"
#include "UI/UIElement.h"

#include <functional>
#include <string>
#include <vector>

namespace GameEngine {

class AssetField;

class BlendSpace2DEditor : public UIElement
{
public:
    using UndoScopeFn = std::function<void(const std::string& actionName, std::function<void()> mutate)>;

    BlendSpace2DEditor();

    void SetSamples(std::vector<BlendSpace2DSampleDesc> samples);
    const std::vector<BlendSpace2DSampleDesc>& GetSamples() const { return m_Samples; }
    void SetParameterNameX(std::string name);
    void SetParameterNameY(std::string name);
    const std::string& GetParameterNameX() const { return m_ParameterNameX; }
    const std::string& GetParameterNameY() const { return m_ParameterNameY; }
    void SetPreviewPosition(bool active, float x, float y);
    void SetUndoScope(UndoScopeFn fn) { m_UndoScope = std::move(fn); }
    void SetOnChanged(std::function<void()> fn) { m_OnChanged = std::move(fn); }

    void OnEvent(UIEvent& e) override;
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx, const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

private:
    struct PadLayout
    {
        float Left = 0.f;
        float Top = 0.f;
        float Width = 0.f;
        float Height = 0.f;
    };

    PadLayout MakePadLayout() const;
    BlendSpace2DAxis MakeAxisX() const;
    BlendSpace2DAxis MakeAxisY() const;
    float ClipStripHeight() const;
    bool HitClipStrip(float localY) const;
    bool HitPad(float localX, float localY, const PadLayout& pad) const;
    int HitSample(float localX, float localY, const PadLayout& pad) const;
    void SampleToPad(const BlendSpace2DSampleDesc& sample, const PadLayout& pad,
                     const BlendSpace2DAxis& axisX, const BlendSpace2DAxis& axisY, float& outX,
                     float& outY) const;
    void ParameterToPad(float x, float y, const PadLayout& pad, const BlendSpace2DAxis& axisX,
                        const BlendSpace2DAxis& axisY, float& outX, float& outY) const;
    void PadToSample(float localX, float localY, const PadLayout& pad, float& outX, float& outY) const;
    void NotifyChanged();
    void CommitSamples(const std::string& actionName, std::function<void()> mutate);
    void SyncClipField();
    void EnsureClipFieldRegistry();

    std::vector<BlendSpace2DSampleDesc> m_Samples;
    std::string m_ParameterNameX{"Speed"};
    std::string m_ParameterNameY{"Direction"};
    bool m_PreviewActive = false;
    float m_PreviewX = 0.f;
    float m_PreviewY = 0.f;
    UndoScopeFn m_UndoScope;
    std::function<void()> m_OnChanged;
    AssetField* m_ClipField = nullptr;
    int m_SelectedIndex = -1;
    int m_PressedIndex = -1;
    float m_PressX = 0.f;
    float m_PressY = 0.f;
    float m_DragStartX = 0.f;
    float m_DragStartY = 0.f;
    bool m_Dragging = false;
    bool m_PendingAdd = false;
    bool m_UpdatingClipField = false;
};

} // namespace GameEngine
