#pragma once

#include <cstdint>
#include <memory>

namespace GameEngine {
namespace Platform {

// Lightweight OS overlay window used for ghost/preview rectangles during drags.
// Implemented as a top-most, click-through, non-activating layered window on Win32.
class OverlayWindow {
public:
    static std::unique_ptr<OverlayWindow> Create();
    virtual ~OverlayWindow() = default;

    // Visibility
    virtual void Show() = 0;
    virtual void Hide() = 0;

    // Position and size in screen coordinates
    virtual void SetBounds(int x, int y, int w, int h) = 0;

    // Style (0xAARRGGBB). outlinePx may be 0 for no outline.
    virtual void SetStyle(uint32_t fillARGB, uint32_t outlineARGB, int outlinePx) = 0;

protected:
    OverlayWindow() = default;
};

} // namespace Platform
} // namespace GameEngine

