#pragma once

#include <string>
#include <vector>
#include "UI/Layout/Docking.h"

namespace GameEngine {

// Drop target classification for docking hit-testing
struct DockDropTarget {
    enum class TargetKind { None, Tab, TabBar, LeafSplit, RegionSplit, RootSplit };
    TargetKind Kind = TargetKind::None;
    std::string Path;         // For LeafSplit/RegionSplit: node path (sequence of '0'/'1'); empty for root
    DockPosition Edge = DockPosition::Center; // For *Split kinds: Left/Right/Top/Bottom
};

class DockingHitTest {
public:
    // Compute a drop target given the dockspace container size and cursor position (in container-local coords)
    // - containerW/H: dockspace content width/height in pixels
    // - x/y: cursor position in container-local coordinates
    static DockDropTarget Compute(const DockingManager& dm, float containerW, float containerH, float x, float y);

    // Convenience: map DockPosition to a canonical Left/Right/Top/Bottom for split direction
    static DockPosition CanonicalEdgeFromPoint(float x, float y, float left, float top, float width, float height);

public:
    // Tunables (kept here for now; could be exposed later)
    // Pixel baselines (minimum thickness)
    static constexpr float kTabbarPx = 28.0f;        // Tab bar height approximation
    static constexpr float kInnerEdgePx = 56.0f;     // Min inner edge band thickness inside a leaf (LeafSplit)
    static constexpr float kRegionRingPx = 48.0f;    // Debug-only: visual ring thickness for RegionSplit
    static constexpr float kRootRingPx = 44.0f;      // Debug-only: visual ring thickness for RootSplit
    // Fractional thickness relative to the target rect (applied per-axis, with min px above)
    static constexpr float kInnerEdgeFrac = 0.25f;   // ~25% bands inside a leaf
    static constexpr float kRegionRingFrac = 0.32f;  // Debug-only: ~32% visual bands on ancestor regions
    static constexpr float kRootRingFrac = 0.26f;    // Debug-only: ~26% visual bands on the root space
    // Edge lock margins: within these distances from an edge, force Region/Root selection
    static constexpr float kOuterEdgeLockPx = 40.0f;  // Root (dockspace) outer lock
    static constexpr float kRegionEdgeLockPx = 40.0f; // Ancestor (region) inner lock
    // Center area inset fraction (larger number => smaller blue zone)
    static constexpr float kCenterInsetFrac = 0.22f;
};

} // namespace GameEngine

