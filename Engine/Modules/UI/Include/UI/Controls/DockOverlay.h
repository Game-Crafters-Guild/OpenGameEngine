#pragma once

#include <vector>
#include <cstdint>
#include "UI/UIElement.h"
#include "UI/UIPrimitive.h"
#include "UI/ResolvedStyle.h"
#include "Rendering/Geometry/ShapeBuilder.h"
#include "UI/Layout/Docking.h"
#include "UI/Layout/DockingHitTest.h"

namespace GameEngine {

// Debug-only overlay for visualizing docking zones and previews.
class DockOverlay : public UIElement {
public:
    static void SetDebugZonesEnabled(bool enabled);
    static bool IsDebugZonesEnabled();
private:
public:
    struct ZoneRect {
        float X = 0, Y = 0, W = 0, H = 0;
        Rendering::Geometry::ColorF Color {0, 0.5f, 1, 0.25f}; // default semi-transparent
        float Border = 1.0f;
        Rendering::Geometry::ColorF BorderColor {0, 0.5f, 1, 0.9f};
        float Radius = 0.0f;
    };

    DockOverlay() = default;

    // Zones are in the overlay's local space. Caller ensures the overlay covers the dockspace region.
    void SetZones(std::vector<ZoneRect> zones) { m_Zones = std::move(zones); MarkDirty(VisualDirty); }
    const std::vector<ZoneRect>& GetZones() const { return m_Zones; }

    void Clear() { m_Zones.clear(); MarkDirty(VisualDirty); }

    // Convenience: compute and set zones from a DockDropTarget and DockingManager
    void SetTargetZones(const DockDropTarget& target, const DockingManager& dm, float containerW, float containerH);

    // Set accent color used for dock drop overlays (ARGB: 0xAARRGGBB).
    void SetAccentColor(uint32_t argb) { m_AccentColor = argb; }
    uint32_t GetAccentColor() const { return m_AccentColor; }

    void OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                              const ResolvedStyle& style,
                              float x, float y, float w, float h) override;

private:
    static bool s_DebugZonesEnabled;
    std::vector<ZoneRect> m_Zones;
    uint32_t m_AccentColor = 0xFF3A8FFF; // default blue
};

} // namespace GameEngine

