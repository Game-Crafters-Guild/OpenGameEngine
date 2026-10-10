#pragma once

#include "UI/UIElement.h"

#include <array>
#include <cstdint>

namespace GameEngine
{

class SceneViewController;

class SceneViewMeasureOverlay final : public UIElement
{
public:
    enum class UnitSystem : std::uint8_t
    {
        Metric = 0,
        Imperial
    };

    enum class TwoDMode : std::uint8_t
    {
        Triangle = 0,
        Points
    };

    SceneViewMeasureOverlay();
    ~SceneViewMeasureOverlay() override = default;

    void SetSceneController(SceneViewController* controller) { m_Controller = controller; }
    void Tick();
    void SetMeasureToolEnabled(bool enabled);
    bool IsMeasureToolEnabled() const { return m_ToolEnabled; }
    void SetUnitSystem(UnitSystem unitSystem);
    UnitSystem GetUnitSystem() const { return m_UnitSystem; }
    void SetTwoDMode(TwoDMode mode);
    TwoDMode GetTwoDMode() const { return m_TwoDMode; }

    void BeginMeasure(float viewX,
                      float viewY,
                      const std::array<float, 3>& world,
                      bool is2D,
                      bool showComponents);
    void UpdateMeasure(float viewX,
                       float viewY,
                       const std::array<float, 3>& world,
                       bool showComponents);
    void EndMeasure();
    void CancelMeasure();

    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                              const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

private:
    // Hash of every input the committed-pill emit depends on (camera pose,
    // projection mode, FOV, measure-entity endpoints). Tick re-emits only
    // when it changes — see the comment there.
    uint64_t ComputeEmitKey() const;
    bool ProjectWorldToView(const std::array<float, 3>& world,
                            float w,
                            float h,
                            float& outX,
                            float& outY) const;
    void RenderPersistentMeasurePills(UI::PrimitiveEmitContext& ctx,
                                      float x,
                                      float y,
                                      float w,
                                      float h);

    SceneViewController* m_Controller = nullptr; // not owned
    bool m_ToolEnabled = false;
    bool m_Active = false;
    bool m_Is2D = true;
    bool m_ShowComponents = false;
    UnitSystem m_UnitSystem = UnitSystem::Metric;
    TwoDMode m_TwoDMode = TwoDMode::Triangle;

    uint64_t m_LastEmitKey = 0;
    // Live drag endpoints in logical CSS px relative to the viewport top-left
    // (fed from pointer events by BeginMeasure/UpdateMeasure).
    float m_StartViewX = 0.0f;
    float m_StartViewY = 0.0f;
    float m_EndViewX = 0.0f;
    float m_EndViewY = 0.0f;
    std::array<float, 3> m_StartWorld{0.0f, 0.0f, 0.0f};
    std::array<float, 3> m_EndWorld{0.0f, 0.0f, 0.0f};
};

} // namespace GameEngine
