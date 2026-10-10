#pragma once

#include <cstdint>

// Interop contract shared by Display.cpp (C++) and Display_mac.mm (Objective-C++).
// Keeping the struct layout and the GE_MacOS_* prototypes in one header gives the ABI
// across that boundary a single source of truth — a field reorder/type change would
// otherwise corrupt geometry/EDR with no compile error.
//
// All GE_MacOS_* functions must be called on the main thread (AppKit NSScreen accessors
// are main-thread-only).

// One display's geometry + EDR, in the units GLFW reports: screen-coordinate points,
// top-left origin with the primary screen's top-left at (0,0).
struct GE_MacOS_ScreenInfo
{
    int x;
    int y;
    int width;
    int height;
    int workX; // visibleFrame (menu bar / Dock excluded), same convention as the frame
    int workY;
    int workWidth;
    int workHeight;
    float currentEdr;   // live headroom multiplier (fluctuates) — diagnostic only
    float potentialEdr; // stable max headroom — the capability signal
    double refreshRate; // Hz, 0 when unavailable
    int isPrimary;
    char name[128];
};

extern "C" {

// EDR headroom for a specific CoreGraphics display id (the value glfwGetCocoaMonitor
// returns). Returns false when no screen matches displayId.
bool GE_MacOS_GetScreenEdrInfo(uint32_t displayId, float* outCurrentEdr, float* outPotentialEdr);

// EDR headroom for the screen owning a given Cocoa NSWindow* (passed as void*), falling
// back to the main screen when the window has no screen. Lets the toggle-dim probe
// report the EDR of the window actually being recreated rather than the key window's.
bool GE_MacOS_GetWindowScreenEdrInfo(void* nsWindow, float* outCurrentEdr, float* outPotentialEdr);

// Enumerate displays straight from NSScreen, bypassing GLFW's (sometimes empty) monitor
// cache. Writes up to maxCount entries and returns the number written.
int GE_MacOS_EnumerateScreens(GE_MacOS_ScreenInfo* outScreens, int maxCount);

} // extern "C"
