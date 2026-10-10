// Display_mac.mm - macOS EDR (Extended Dynamic Range) display capability query.
#import <AppKit/AppKit.h>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "MacScreenBridge.h"

namespace {

// Read an NSScreen's EDR headroom multipliers, defaulting to 1.0 (HDR off) when the
// screen is nil or the API is unavailable. Single definition of the "1.0 fallback"
// contract shared by every EDR query below.
//
//   currentEdr   (maximumExtendedDynamicRangeColorComponentValue): LIVE headroom — it
//                fluctuates with brightness/content/thermal (often ~1.0 at boot, rising
//                once EDR is engaged). Only trustworthy for "is EDR engaged right now".
//   potentialEdr (maximumPotentialExtendedDynamicRangeColorComponentValue): the STABLE
//                max the panel can ever reach (e.g. 16.0 on a Liquid Retina XDR) — the
//                correct signal for "does this display support EDR".
//
// Both are multipliers relative to SDR white, not absolute nits; macOS has no public
// absolute-nits SDR-white API.
void ReadScreenEdr(NSScreen* screen, float* outCurrentEdr, float* outPotentialEdr)
{
    float currentEdr = 1.0f;
    float potentialEdr = 1.0f;
    if (screen)
    {
        if (@available(macOS 10.15, *))
        {
            currentEdr = (float)screen.maximumExtendedDynamicRangeColorComponentValue;
            potentialEdr = (float)screen.maximumPotentialExtendedDynamicRangeColorComponentValue;
        }
    }
    if (outCurrentEdr)
        *outCurrentEdr = currentEdr;
    if (outPotentialEdr)
        *outPotentialEdr = potentialEdr;
}

} // namespace

extern "C" {

bool GE_MacOS_GetScreenEdrInfo(uint32_t displayId, float* outCurrentEdr, float* outPotentialEdr)
{
    @autoreleasepool {
        for (NSScreen* screen in [NSScreen screens]) {
            // NSScreenNumber is the long-standing key that maps an NSScreen to its
            // CGDirectDisplayID (the same id glfwGetCocoaMonitor returns). Not a
            // formally-public constant, but the standard match GLFW itself uses.
            NSNumber* screenNumber = screen.deviceDescription[@"NSScreenNumber"];
            if (!screenNumber || screenNumber.unsignedIntValue != displayId)
                continue;
            ReadScreenEdr(screen, outCurrentEdr, outPotentialEdr);
            return true;
        }
        return false;
    }
}

bool GE_MacOS_GetWindowScreenEdrInfo(void* nsWindow, float* outCurrentEdr, float* outPotentialEdr)
{
    @autoreleasepool {
        // Prefer the screen the window is actually on; fall back to the main (key-window)
        // screen when the window is off-screen / not yet placed. (ARC is disabled for this
        // target, so a plain cast — no __bridge — is correct.)
        NSWindow* window = (NSWindow*)nsWindow;
        NSScreen* screen = window ? window.screen : nil;
        if (!screen)
            screen = [NSScreen mainScreen];
        if (!screen)
            return false;
        ReadScreenEdr(screen, outCurrentEdr, outPotentialEdr);
        return true;
    }
}

// Enumerate displays straight from NSScreen, bypassing GLFW's monitor cache. GLFW 3.4
// only refreshes its monitor list at init and from the AppKit
// NSApplicationDidChangeScreenParameters notification (it has no CGDisplay reconfiguration
// callback); if the initial poll happens before the display list is ready and no
// screen-parameters event follows, glfwGetMonitors() can stay empty for the whole session,
// which collapses window->monitor resolution to -1 and silently disables HDR. This gives
// Display.cpp a direct OS fallback for that case. Writes up to maxCount entries and returns
// the number written.
int GE_MacOS_EnumerateScreens(GE_MacOS_ScreenInfo* outScreens, int maxCount)
{
    @autoreleasepool {
        if (!outScreens || maxCount <= 0)
            return 0;

        NSArray<NSScreen*>* screens = [NSScreen screens];
        const NSUInteger total = screens.count;
        if (total == 0)
            return 0;

        // The global coordinate space origin (0,0) is the bottom-left of screens[0]
        // (the menu-bar / primary screen). Flip to GLFW's top-left origin against it.
        const CGFloat primaryTop = NSMaxY([[screens objectAtIndex:0] frame]);

        int written = 0;
        for (NSUInteger i = 0; i < total && written < maxCount; ++i)
        {
            NSScreen* screen = [screens objectAtIndex:i];
            const NSRect frame = [screen frame];
            // visibleFrame excludes the menu bar and Dock — matches what GLFW reports via
            // glfwGetMonitorWorkarea, keeping the fallback work area a faithful substitute.
            const NSRect visible = [screen visibleFrame];

            GE_MacOS_ScreenInfo info{};
            info.x = (int)std::llround(frame.origin.x);
            info.y = (int)std::llround(primaryTop - NSMaxY(frame));
            info.width = (int)std::llround(frame.size.width);
            info.height = (int)std::llround(frame.size.height);
            info.workX = (int)std::llround(visible.origin.x);
            info.workY = (int)std::llround(primaryTop - NSMaxY(visible));
            info.workWidth = (int)std::llround(visible.size.width);
            info.workHeight = (int)std::llround(visible.size.height);
            ReadScreenEdr(screen, &info.currentEdr, &info.potentialEdr);
            info.refreshRate = 0.0;
            if (@available(macOS 12.0, *))
                info.refreshRate = (double)screen.maximumFramesPerSecond;
            info.isPrimary = (i == 0) ? 1 : 0;

            const char* name = nullptr;
            if (@available(macOS 10.15, *))
                name = screen.localizedName.UTF8String;
            std::snprintf(info.name, sizeof(info.name), "%s", name ? name : "Display");

            outScreens[written++] = info;
        }
        return written;
    }
}

} // extern "C"
