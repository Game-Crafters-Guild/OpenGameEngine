#include "UI/Controls/DockOverlay.h"
#include "UI/UIPrimitive.h"
#include "UI/ResolvedStyle.h"
#include <cstdlib>

using namespace GameEngine;

// static
bool DockOverlay::s_DebugZonesEnabled = false;
void DockOverlay::SetDebugZonesEnabled(bool enabled) { s_DebugZonesEnabled = enabled; }
bool DockOverlay::IsDebugZonesEnabled() { return s_DebugZonesEnabled; }

void DockOverlay::OnGeneratePrimitives(UI::PrimitiveEmitContext& ctx,
                                       const ResolvedStyle& /*style*/,
                                       float x, float y, float /*w*/, float /*h*/) {
    if (m_Zones.empty()) return;

    // (x, y) arrive physical; parent GetLayoutX/Y is logical. Zone rects were
    // computed against logical container dimensions in SetTargetZones. Scale
    // everything to physical for emission.
    const float cs = ctx.ContentScale;
    float px = x, py = y;
    if (const UIElement* p = GetParent()) {
        px = p->GetLayoutX() * cs;
        py = p->GetLayoutY() * cs;
    }

    for (const auto& zr : m_Zones) {
        const float r = zr.Radius * cs;
        const float border = zr.Border * cs;
        uint32_t fill = UI::PackColor(zr.Color.r, zr.Color.g, zr.Color.b, zr.Color.a);
        auto prim = UI::MakeRect(px + zr.X * cs, py + zr.Y * cs,
                                 zr.W * cs, zr.H * cs, fill, r, r, r, r);
        if (border > 0.0f) {
            uint32_t bc = UI::PackColor(zr.BorderColor.r, zr.BorderColor.g, zr.BorderColor.b, zr.BorderColor.a);
            UI::AddBorder(prim, border, bc);
        }
        ctx.Emit(prim);
    }
}

void DockOverlay::SetTargetZones(const DockDropTarget& target, const DockingManager& dm, float containerW, float containerH) {
    std::vector<ZoneRect> zones;           // final draw order (debug fills, then debug lines, active on top)
    std::vector<ZoneRect> activeZones;      // active highlight zones (draw last)
    std::vector<ZoneRect> debugFills;       // debug fills (draw first)
    std::vector<ZoneRect> debugLines;       // debug boundary lines (draw above fills)
    // Gather rects
    std::vector<DockingManager::NodeRect> rects;
    dm.ComputeNodeRects(containerW, containerH, rects);

    auto findRect = [&](const std::string& path)->DockingManager::NodeRect {
        for (const auto& r : rects) { if (r.path == path) return r; }
        DockingManager::NodeRect root; root.x = 0; root.y = 0; root.width = containerW; root.height = containerH; root.path = ""; root.isLeaf = false; return root;
    };

    auto makeColor = [](float r, float g, float b, float a)->Rendering::Geometry::ColorF { return {r,g,b,a}; };

    // Extract accent color components from ARGB
    const float accentR = ((m_AccentColor >> 16) & 0xFF) / 255.0f;
    const float accentG = ((m_AccentColor >> 8) & 0xFF) / 255.0f;
    const float accentB = (m_AccentColor & 0xFF) / 255.0f;

    auto pushRectTo = [&](std::vector<ZoneRect>& dst, float x, float y, float w, float h, const Rendering::Geometry::ColorF& fill, const Rendering::Geometry::ColorF& border, float borderW = 3.0f, float radius = 6.0f){
        ZoneRect zr; zr.X = x; zr.Y = y; zr.W = w; zr.H = h; zr.Color = fill; zr.BorderColor = border; zr.Border = borderW; zr.Radius = radius; dst.push_back(zr);
    };
    auto pushActive = [&](float x, float y, float w, float h, const Rendering::Geometry::ColorF& fill, const Rendering::Geometry::ColorF& border){
        pushRectTo(activeZones, x, y, w, h, fill, border, 4.0f, 8.0f);
    };
    auto pushDebug = [&](float x, float y, float w, float h, const Rendering::Geometry::ColorF& fill, const Rendering::Geometry::ColorF& border){
        pushRectTo(debugFills, x, y, w, h, fill, border);
    };
    auto pushDebugLine = [&](float x, float y, float w, float h, const Rendering::Geometry::ColorF& fill){
        // Line helper: draw a thin filled rectangle with no border
        ZoneRect zr; zr.X = x; zr.Y = y; zr.W = w; zr.H = h; zr.Color = fill; zr.BorderColor = makeColor(0,0,0,0); zr.Border = 0.0f; zr.Radius = 0.0f; debugLines.push_back(zr);
    };


    // Enhanced visual feedback with more vivid colors and better contrast
    switch (target.Kind) {
        case DockDropTarget::TargetKind::Tab: {
            auto r = findRect(target.Path);
            float x = r.x + 4.0f;
            float y = r.y + 4.0f;
            float w = r.width - 8.0f;
            float h = r.height - 8.0f;
            // Dark overlay for "undock / tear off"
            pushActive(x, y, w, h, makeColor(0.0f, 0.0f, 0.0f, 0.40f), makeColor(0.3f, 0.3f, 0.3f, 1.0f));
            break;
        }
        case DockDropTarget::TargetKind::TabBar: {
            auto r = findRect(target.Path);
            constexpr float kTabBarH = 28.0f;
            float stripH = std::min(kTabBarH, r.height);
            float x = r.x + 2.0f;
            float y = r.y + 2.0f;
            float w = r.width - 4.0f;
            float h = stripH - 4.0f;
            // Thin horizontal strip for "dock as tab"
            pushActive(x, y, w, h, makeColor(accentR, accentG, accentB, 0.45f), makeColor(accentR, accentG, accentB, 1.0f));
            break;
        }
        case DockDropTarget::TargetKind::LeafSplit:
        case DockDropTarget::TargetKind::RegionSplit: {
            // Full-area preview: show where the new panel would be after a 50/50 split of the target rect
            auto r = findRect(target.Path);
            const float kPrev = 0.5f;
            float x = r.x, y = r.y, w = r.width, h = r.height;
            switch (target.Edge) {
                case DockPosition::Left:   w = r.width * kPrev; break;
                case DockPosition::Right:  x = r.x + r.width * (1.0f - kPrev); w = r.width * kPrev; break;
                case DockPosition::Top:    h = r.height * kPrev; break;
                case DockPosition::Bottom: y = r.y + r.height * (1.0f - kPrev); h = r.height * kPrev; break;
                default: break;
            }
            x += 4.0f; y += 4.0f; w -= 8.0f; h -= 8.0f;
            pushActive(x, y, w, h, makeColor(accentR, accentG, accentB, 0.30f), makeColor(accentR, accentG, accentB, 0.9f));
            break;
        }
        case DockDropTarget::TargetKind::RootSplit: {
            const float kPrev = 0.5f;
            float x = 0.0f, y = 0.0f, w = containerW, h = containerH;
            switch (target.Edge) {
                case DockPosition::Left:   w = containerW * kPrev; break;
                case DockPosition::Right:  x = containerW * (1.0f - kPrev); w = containerW * kPrev; break;
                case DockPosition::Top:    h = containerH * kPrev; break;
                case DockPosition::Bottom: y = containerH * (1.0f - kPrev); h = containerH * kPrev; break;
                default: break;
            }
            x += 4.0f; y += 4.0f; w -= 8.0f; h -= 8.0f;
            pushActive(x, y, w, h, makeColor(accentR, accentG, accentB, 0.30f), makeColor(accentR, accentG, accentB, 0.9f));
            break;
        }
        default: break;
    }

    // Debug: optionally overlay all hit-test bands to visualize sizes (hover-insensitive)
    if (DockOverlay::IsDebugZonesEnabled()) {
        // Root edge-lock strips first (beneath region/leaf bands)
        const float rlX = DockingHitTest::kOuterEdgeLockPx;
        const float rlY = DockingHitTest::kOuterEdgeLockPx;
        const auto rootFill = makeColor(1.0f, 0.0f, 0.0f, 0.14f);
        const auto rootBorder = makeColor(1.0f, 0.0f, 0.0f, 0.45f);
        // Avoid corner overlap by clipping vertical bands between top/bottom bands
        float vertH = std::max(0.0f, containerH - 2.0f * rlY);
        if (vertH > 0.0f) {
            pushDebug(0, rlY, rlX, vertH, rootFill, rootBorder);
            pushDebug(containerW - rlX, rlY, rlX, vertH, rootFill, rootBorder);
        } else {
            // Fallback for very small containers
            pushDebug(0, 0, rlX, containerH, rootFill, rootBorder);
            pushDebug(containerW - rlX, 0, rlX, containerH, rootFill, rootBorder);
        }
        pushDebug(0, 0, containerW, rlY, rootFill, rootBorder);
        pushDebug(0, containerH - rlY, containerW, rlY, rootFill, rootBorder);
        // Root transition lines (exact switch from Root->others)
        pushDebugLine(rlX - 1.0f, 0, 3.0f, containerH, makeColor(1.0f, 0.0f, 0.0f, 0.95f));
        pushDebugLine(containerW - rlX - 1.0f, 0, 3.0f, containerH, makeColor(1.0f, 0.0f, 0.0f, 0.95f));
        pushDebugLine(0, rlY - 1.0f, containerW, 3.0f, makeColor(1.0f, 0.0f, 0.0f, 0.95f));
        pushDebugLine(0, containerH - rlY - 1.0f, containerW, 3.0f, makeColor(1.0f, 0.0f, 0.0f, 0.95f));

        // Draw region edge-lock strips (all ancestors) before leaf bands for clearer layering
        for (const auto& r : rects) {
            if (!r.isLeaf) {
                // Prune region bands that equal the container (would duplicate Root)
                if (r.x <= 0.5f && r.y <= 0.5f && r.width >= containerW - 0.5f && r.height >= containerH - 0.5f) continue;
                const float lockX = DockingHitTest::kRegionEdgeLockPx;
                const float lockY = DockingHitTest::kRegionEdgeLockPx;
                // use outer rlX/rlY defined earlier in the debug block to avoid shadowing
                const auto regionFill = makeColor(1.0f, 0.5f, 0.0f, 0.16f);
                const auto regionBorder = makeColor(1.0f, 0.5f, 0.0f, 0.50f);
                const bool coincLeft   = (r.x <= 0.5f);
                const bool coincRight  = (r.x + r.width  >= containerW - 0.5f);
                const bool coincTop    = (r.y <= 0.5f);
                const bool coincBottom = (r.y + r.height >= containerH - 0.5f);

                // Edge equivalence pruning: skip sides that are equivalent to root due to full-orthogonal span
                const bool spansFullHeight = (r.y <= 0.5f) && (r.y + r.height >= containerH - 0.5f);
                const bool spansFullWidth  = (r.x <= 0.5f) && (r.x + r.width  >= containerW - 0.5f);
                const bool drawLeft   = !(coincLeft   && spansFullHeight);
                const bool drawRight  = !(coincRight  && spansFullHeight);
                const bool drawTop    = !(coincTop    && spansFullWidth);
                const bool drawBottom = !(coincBottom && spansFullWidth);

                // Per-side proportional compression when coincident with root and region is shallow
                float rootL = rlX, rootR = rlX, rootT = rlY, rootB = rlY;
                float regL  = lockX, regR  = lockX, regT  = lockY, regB  = lockY;
                if (coincLeft)  { const float along = r.width;  const float total = rlX + lockX; if (along < total) { const float s = std::max(0.0f, along / std::max(1e-3f, total)); rootL = rlX * s; regL = lockX * s; } }
                if (coincRight) { const float along = r.width;  const float total = rlX + lockX; if (along < total) { const float s = std::max(0.0f, along / std::max(1e-3f, total)); rootR = rlX * s; regR = lockX * s; } }
                if (coincTop)   { const float along = r.height; const float total = rlY + lockY; if (along < total) { const float s = std::max(0.0f, along / std::max(1e-3f, total)); rootT = rlY * s; regT = lockY * s; } }
                if (coincBottom){ const float along = r.height; const float total = rlY + lockY; if (along < total) { const float s = std::max(0.0f, along / std::max(1e-3f, total)); rootB = rlY * s; regB = lockY * s; } }

                float effLeft   = drawLeft   ? (coincLeft   ? std::max(0.0f, std::min(regL, r.width  - rootL))  : std::min(regL, r.width))   : 0.0f;
                float effRight  = drawRight  ? (coincRight  ? std::max(0.0f, std::min(regR, r.width  - rootR))  : std::min(regR, r.width))   : 0.0f;
                float effTop    = drawTop    ? (coincTop    ? std::max(0.0f, std::min(regT, r.height - rootT)) : std::min(regT, r.height))  : 0.0f;
                float effBottom = drawBottom ? (coincBottom ? std::max(0.0f, std::min(regB, r.height - rootB)) : std::min(regB, r.height))  : 0.0f;

                // Clip vertical bands by horizontal bands to avoid corner overlap
                float leftW = std::min(effLeft, r.width - (coincLeft ? rootL : 0.0f));
                float rightW = std::min(effRight, r.width - (coincRight ? rootR : 0.0f));
                float topH = std::min(effTop, r.height - (coincTop ? rootT : 0.0f));
                float bottomH = std::min(effBottom, r.height - (coincBottom ? rootB : 0.0f));

                float leftX = r.x + (coincLeft ? rootL : 0.0f);
                float rightX = r.x + r.width - (coincRight ? rootR : 0.0f) - rightW;
                float leftY = r.y + topH;
                float rightY = r.y + topH;
                float leftH = std::max(0.0f, r.height - topH - bottomH);
                float rightH = leftH;
                if (leftW > 0.0f && leftH > 0.0f)  pushDebug(leftX, leftY, leftW, leftH, regionFill, regionBorder);
                if (rightW > 0.0f && rightH > 0.0f) pushDebug(rightX, rightY, rightW, rightH, regionFill, regionBorder);

                // Clip horizontal bands by vertical bands to avoid corner overlap
                float topY = r.y + (coincTop ? rootT : 0.0f);
                float topX = r.x + leftW;
                float topW = std::max(0.0f, r.width - leftW - rightW);
                if (topW > 0.0f && topH > 0.0f) pushDebug(topX, topY, topW, topH, regionFill, regionBorder);

                float bottomY = r.y + r.height - (coincBottom ? rootB : 0.0f) - bottomH;
                float bottomX = r.x + leftW;
                float bottomW = std::max(0.0f, r.width - leftW - rightW);
                if (bottomW > 0.0f && bottomH > 0.0f) pushDebug(bottomX, bottomY, bottomW, bottomH, regionFill, regionBorder);

                // Transition lines at the ends of bands (post-clipping)
                if (leftW > 0.0f)  pushDebugLine(leftX + leftW - 1.0f, leftY, 3.0f, leftH, makeColor(1.0f, 0.5f, 0.0f, 0.95f));
                if (rightW > 0.0f) pushDebugLine(rightX + rightW - 1.0f, rightY, 3.0f, rightH, makeColor(1.0f, 0.5f, 0.0f, 0.95f));
                if (topH > 0.0f)   pushDebugLine(topX, topY + topH - 1.0f, topW, 3.0f, makeColor(1.0f, 0.5f, 0.0f, 0.95f));
                if (bottomH > 0.0f)pushDebugLine(bottomX, bottomY + bottomH - 1.0f, bottomW, 3.0f, makeColor(1.0f, 0.5f, 0.0f, 0.95f));
            }
        }
        // Draw leaf bands after region rings
        for (const auto& r : rects) {
            if (r.isLeaf) {
                const float innerX = std::max(DockingHitTest::kInnerEdgePx, r.width  * DockingHitTest::kInnerEdgeFrac);
                const float innerY = std::max(DockingHitTest::kInnerEdgePx, r.height * DockingHitTest::kInnerEdgeFrac);
                // use outer rlX/rlY defined earlier in the debug block to avoid shadowing
                const float regX = DockingHitTest::kRegionEdgeLockPx;
                const float regY = DockingHitTest::kRegionEdgeLockPx;

                // Compute per-side offsets to layer Leaf after Root/Region where applicable
                float offL = 0.0f, offR = 0.0f, offT = 0.0f, offB = 0.0f;
                // Root coincidence
                if (r.x <= 0.5f) offL += rlX;
                if (r.x + r.width >= containerW - 0.5f) offR += rlX;
                if (r.y <= 0.5f) offT += rlY;
                if (r.y + r.height >= containerH - 0.5f) offB += rlY;
                // Region coincidence: check ancestors of this leaf
                auto isAncestor = [&](const DockingManager::NodeRect& a){
                    return !a.isLeaf && r.path.size() >= a.path.size() && r.path.compare(0, a.path.size(), a.path) == 0;
                };
                bool addL=false, addR=false, addT=false, addB=false;
                for (const auto& a : rects) {
                    if (!isAncestor(a)) continue;
                    if (!addL && std::abs(r.x - a.x) <= 0.5f) { offL += regX; addL = true; }
                    if (!addR && std::abs((r.x + r.width) - (a.x + a.width)) <= 0.5f) { offR += regX; addR = true; }
                    if (!addT && std::abs(r.y - a.y) <= 0.5f) { offT += regY; addT = true; }
                    if (!addB && std::abs((r.y + r.height) - (a.y + a.height)) <= 0.5f) { offB += regY; addB = true; }
                    if (addL && addR && addT && addB) break;
                }

                const auto greenFill = makeColor(0.0f, 1.0f, 0.0f, 0.08f);
                const auto greenBorder = makeColor(0.0f, 1.0f, 0.0f, 0.5f);

                // Left/Right bands (leaf) – clipped by outer/region offsets and by top/bottom green to avoid corner overlap
                float leftW = std::max(0.0f, innerX - offL);
                float rightW = std::max(0.0f, innerX - offR);
                float topH = std::max(0.0f, innerY - offT);
                float botH = std::max(0.0f, innerY - offB);

                float leafVertH = std::max(0.0f, r.height - topH - botH);
                if (leftW > 0.0f && leafVertH > 0.0f)  pushDebug(r.x + offL, r.y + topH, leftW,  leafVertH, greenFill, greenBorder);
                if (rightW > 0.0f && leafVertH > 0.0f) pushDebug(r.x + r.width - offR - rightW, r.y + topH, rightW, leafVertH, greenFill, greenBorder);

                // Top/Bottom bands (leaf) – clipped by left/right
                float horizW = std::max(0.0f, r.width - leftW - rightW);
                if (topH > 0.0f && horizW > 0.0f) pushDebug(r.x + leftW, r.y + offT, horizW, topH, greenFill, greenBorder);
                if (botH > 0.0f && horizW > 0.0f) pushDebug(r.x + leftW, r.y + r.height - offB - botH, horizW, botH, greenFill, greenBorder);

                // Center inset (leaf)
                const float cix = r.width  * DockingHitTest::kCenterInsetFrac;
                const float ciy = r.height * DockingHitTest::kCenterInsetFrac;
                const float cx = r.x + cix;
                const float cy = r.y + ciy;
                const float cw = r.width  - 2.0f * cix;
                const float ch = r.height - 2.0f * ciy;
                if (cw > 0 && ch > 0) pushDebug(cx, cy, cw, ch, makeColor(0.0f, 0.6f, 1.0f, 0.12f), makeColor(0.0f, 0.6f, 1.0f, 0.35f));

                // Leaf inner boundary lines (clip out the blue center so lines don't draw over it)
                const auto lineCol = makeColor(0.0f, 1.0f, 0.0f, 0.90f);
                // Left inner line
                {
                    float lx = r.x + innerX - 1.0f;
                    if (cw > 0 && ch > 0) {
                        if (cy > r.y) pushDebugLine(lx, r.y, 2.0f, cy - r.y, lineCol);
                        if (cy + ch < r.y + r.height) pushDebugLine(lx, cy + ch, 2.0f, (r.y + r.height) - (cy + ch), lineCol);
                    } else {
                        pushDebugLine(lx, r.y, 2.0f, r.height, lineCol);
                    }
                }
                // Right inner line
                {
                    float lx = r.x + r.width - innerX - 1.0f;
                    if (cw > 0 && ch > 0) {
                        if (cy > r.y) pushDebugLine(lx, r.y, 2.0f, cy - r.y, lineCol);
                        if (cy + ch < r.y + r.height) pushDebugLine(lx, cy + ch, 2.0f, (r.y + r.height) - (cy + ch), lineCol);
                    } else {
                        pushDebugLine(lx, r.y, 2.0f, r.height, lineCol);
                    }
                }
                // Top inner line
                {
                    float ly = r.y + innerY - 1.0f;
                    if (cw > 0 && ch > 0) {
                        if (cx > r.x) pushDebugLine(r.x, ly, cx - r.x, 2.0f, lineCol);
                        if (cx + cw < r.x + r.width) pushDebugLine(cx + cw, ly, (r.x + r.width) - (cx + cw), 2.0f, lineCol);
                    } else {
                        pushDebugLine(r.x, ly, r.width, 2.0f, lineCol);
                    }
                }
                // Bottom inner line
                {
                    float ly = r.y + r.height - innerY - 1.0f;
                    if (cw > 0 && ch > 0) {
                        if (cx > r.x) pushDebugLine(r.x, ly, cx - r.x, 2.0f, lineCol);
                        if (cx + cw < r.x + r.width) pushDebugLine(cx + cw, ly, (r.x + r.width) - (cx + cw), 2.0f, lineCol);
                    } else {
                        pushDebugLine(r.x, ly, r.width, 2.0f, lineCol);
                    }
                }
            }
        }
    }

    // Compose final draw order: debug fills, then boundary lines, then active on top
    zones.reserve(debugFills.size() + debugLines.size() + activeZones.size());
    zones.insert(zones.end(), debugFills.begin(), debugFills.end());
    zones.insert(zones.end(), debugLines.begin(), debugLines.end());
    zones.insert(zones.end(), activeZones.begin(), activeZones.end());

    SetZones(std::move(zones));
}


