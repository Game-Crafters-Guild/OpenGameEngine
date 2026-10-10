#pragma once

#include "UI/UIElement.h"

#include <chrono>
#include <cstdint>
#include <functional>

namespace GameEngine
{

struct CubicBezierShape
{
    float C1X = 0.25f;
    float C1Y = 0.10f;
    float C2X = 0.25f;
    float C2Y = 1.00f;
    float AnchorStartY = 0.0f;
    float AnchorEndY = 1.0f;
};

class CubicBezierField final : public UIElement
{
  public:
    struct Config
    {
        float TimeMin = 0.0f;
        float TimeMax = 1.0f;
        float ValueMin = 0.0f;
        float ValueMax = 1.0f;
        bool ShowScrubber = false;
        bool ShowPlaybackIndicator = false;
        bool AllowPlaybackScrub = false;
    };

    CubicBezierField();

    void SetConfig(const Config& config);
    void SetShape(const CubicBezierShape& shape);
    const CubicBezierShape& GetShape() const { return m_Shape; }

    void SetScrubberTime(float time);
    void SetPlaybackIndicator(float time, float value, bool visible = true);

    void SetOnChanging(std::function<void(const CubicBezierShape&)> callback);
    void SetOnChanged(std::function<void(const CubicBezierShape&)> callback);
    /// Fired continuously while the playback indicator is scrubbed (time in curve time units).
    void SetOnPlaybackScrub(std::function<void(float)> callback);
    /// Fired when the user starts/stops dragging empty graph space to scrub playback time.
    void SetOnPlaybackScrubActiveChanged(std::function<void(bool)> callback);
    /// Fired when the curve surface is double-clicked. Hosts decide what "expand" means.
    void SetOnCurveDoubleClick(std::function<void()> callback);

    static float EvaluateShape(const CubicBezierShape& shape, float normalizedTime);

    void OnEvent(UIEvent& e) override;
    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                              const ResolvedStyle& style,
                              float x,
                              float y,
                              float w,
                              float h) override;

  private:
    struct GraphRect
    {
        float X = 0.0f;
        float Y = 0.0f;
        float W = 0.0f;
        float H = 0.0f;
    };

    GraphRect CurrentGraphRect() const;
    static GraphRect ComputeGraphRect(float x, float y, float w, float h, float scale = 1.0f);
    static bool PointInGraph(const GraphRect& r, float x, float y);

    float ToNormalizedTime(float time) const;
    float FromNormalizedTime(float normalizedTime) const;
    float ToScreenX(const GraphRect& r, float normalizedTime) const;
    float ToScreenY(const GraphRect& r, float value) const;
    float FromScreenX(const GraphRect& r, float px) const;
    float FromScreenY(const GraphRect& r, float py) const;

    float SampleAt(float normalizedTime) const;
    void HandleScreen(const GraphRect& r, int handle, float& outX, float& outY) const;
    int HitTestHandle(float globalX, float globalY) const;
    bool HandleIsActive(int handle) const;
    void UpdateHover(float globalX, float globalY);
    void UpdateCursor();
    bool TryHandleCurveDoubleClick(float globalX, float globalY);
    void ApplyPlaybackScrub(float globalX);
    void ApplyDrag(float globalX, float globalY);
    void DrawPlaybackIndicator(UI::PrimitiveEmitContext& ctx, const GraphRect& r) const;
    void DrawScrubber(UI::PrimitiveEmitContext& ctx, const GraphRect& r) const;
    static void DrawHandle(UI::PrimitiveEmitContext& ctx, float x, float y, uint32_t color, bool active,
                           float maxSizePx = 0.0f);

    Config m_Config;
    CubicBezierShape m_Shape;
    int m_Hover = -1;
    int m_Drag = -1;
    bool m_DragMoved = false;
    float m_DragStartX = 0.0f;
    float m_DragStartY = 0.0f;
    float m_GrabOffsetX = 0.0f;
    float m_GrabOffsetY = 0.0f;
    float m_HudTime = 0.0f;
    float m_HudValue = 0.0f;
    float m_DragHudX = 0.0f;
    float m_DragHudY = 0.0f;
    float m_ScrubberTime = 0.0f;
    float m_PlaybackTime = 0.0f;
    float m_PlaybackValue = 0.0f;
    bool m_PlaybackIndicatorVisible = false;
    bool m_PlaybackScrubActive = false;
    bool m_PlaybackScrubStarted = false;
    float m_PlaybackScrubStartX = 0.0f;
    float m_PlaybackScrubStartY = 0.0f;
    bool m_HasLastCurveClick = false;
    std::chrono::steady_clock::time_point m_LastCurveClickTime{};
    float m_LastCurveClickX = 0.0f;
    float m_LastCurveClickY = 0.0f;

    std::function<void(const CubicBezierShape&)> m_OnChanging;
    std::function<void(const CubicBezierShape&)> m_OnChanged;
    std::function<void(float)> m_OnPlaybackScrub;
    std::function<void(bool)> m_OnPlaybackScrubActiveChanged;
    std::function<void()> m_OnCurveDoubleClick;
};

} // namespace GameEngine
