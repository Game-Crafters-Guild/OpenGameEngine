#pragma once

#include "UI/UIElement.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace GameEngine
{

// Lightweight gradient strip for sky day keys (0h / 6h / 12h / 18h).
// The bar renders a continuous color ramp and exposes clickable key points.
class SkyDayKeyGradientBar : public UIElement
{
  public:
    SkyDayKeyGradientBar();

    void SetStopColor(size_t stopIndex, uint32_t argb);
    void SetStopPosition(size_t stopIndex, float normalized01);
    void SetSelectedStop(size_t stopIndex);
    void ClearSelectedStop();
    size_t GetSelectedStop() const { return m_SelectedStop; }

    void SetOnStopSelected(std::function<void(size_t)> callback)
    {
        m_OnStopSelected = std::move(callback);
    }
    void SetOnStopActivated(std::function<void(size_t)> callback)
    {
        m_OnStopActivated = std::move(callback);
    }
    void SetOnStopPositionChanging(std::function<void(size_t, float)> callback)
    {
        m_OnStopPositionChanging = std::move(callback);
    }
    void SetOnStopPositionChanged(std::function<void(size_t, float)> callback)
    {
        m_OnStopPositionChanged = std::move(callback);
    }
    void SetOnContextMenu(std::function<void(float, float, size_t, float)> callback)
    {
        m_OnContextMenu = std::move(callback);
    }

    void OnEvent(UIEvent& e) override;
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                              const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

  private:
    size_t GetNearestStopIndex(float globalX) const;
    size_t GetHoveredStopIndex(float globalX, float globalY) const;
    float GetNormalizedPosition(float globalX) const;
    static constexpr size_t kStopCount = 4;

    std::array<uint32_t, kStopCount> m_StopColors = {
        0xFF202020u, 0xFF505050u, 0xFF808080u, 0xFFA0A0A0u};
    std::array<float, kStopCount> m_StopPositions = {0.0f, 0.25f, 0.5f, 0.75f};
    size_t m_SelectedStop = kStopCount;
    std::function<void(size_t)> m_OnStopSelected;
    std::function<void(size_t)> m_OnStopActivated;
    std::function<void(size_t, float)> m_OnStopPositionChanging;
    std::function<void(size_t, float)> m_OnStopPositionChanged;
    std::function<void(float, float, size_t, float)> m_OnContextMenu;
    bool m_PrimaryDown = false;
    bool m_Dragged = false;
    float m_DownX = 0.0f;
    float m_DownY = 0.0f;
    size_t m_DownStop = 0;
    size_t m_HoveredStop = kStopCount;
};

} // namespace GameEngine
