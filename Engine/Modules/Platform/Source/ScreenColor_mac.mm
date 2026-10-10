// ScreenColor_mac.mm - macOS screen color sampling and global mouse events
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <dlfcn.h>
#include <GLFW/glfw3.h>

// Global event monitor handle for eyedropper
static id s_globalMouseMonitor = nil;
static id s_localMouseMonitor = nil;

extern "C" {

// Function pointer type for CGWindowListCreateImage
typedef CGImageRef (*CGWindowListCreateImageFunc)(CGRect, CGWindowListOption, CGWindowID, CGWindowImageOption);

// Sample screen color at given screen coordinates (top-left origin, like CG coordinates)
// Returns ARGB (0xAARRGGBB)
uint32_t GE_MacOS_GetScreenPixelColor(int screenX, int screenY)
{
    @autoreleasepool {
        // Try to dynamically load CGWindowListCreateImage to bypass compile-time unavailability
        static CGWindowListCreateImageFunc s_CGWindowListCreateImage = nullptr;
        static bool s_triedLoad = false;
        
        if (!s_triedLoad) {
            s_triedLoad = true;
            void* cgHandle = dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_LAZY);
            if (cgHandle) {
                s_CGWindowListCreateImage = (CGWindowListCreateImageFunc)dlsym(cgHandle, "CGWindowListCreateImage");
                // Don't dlclose - keep the handle alive
            }
        }
        
        if (!s_CGWindowListCreateImage) {
            // Function not available - return black
            return 0;
        }
        
        // Capture a 1x1 image at the screen coordinates
        CGRect captureRect = CGRectMake((CGFloat)screenX, (CGFloat)screenY, 1.0, 1.0);
        CGImageRef screenshot = s_CGWindowListCreateImage(
            captureRect,
            kCGWindowListOptionOnScreenOnly,
            kCGNullWindowID,
            kCGWindowImageDefault);
        
        if (!screenshot)
            return 0;
        
        // Create a bitmap context to read the pixel
        CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
        uint8_t pixelData[4] = {0, 0, 0, 0};
        CGContextRef context = CGBitmapContextCreate(
            pixelData,
            1, 1,
            8, 4,
            colorSpace,
            (CGBitmapInfo)(kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big));
        CGColorSpaceRelease(colorSpace);
        
        if (!context) {
            CGImageRelease(screenshot);
            return 0;
        }
        
        // Draw the image into our 1x1 context
        CGContextDrawImage(context, CGRectMake(0, 0, 1, 1), screenshot);
        CGContextRelease(context);
        CGImageRelease(screenshot);
        
        // pixelData is now RGBA
        uint8_t r = pixelData[0];
        uint8_t g = pixelData[1];
        uint8_t b = pixelData[2];
        
        return (0xFFu << 24) | (r << 16) | (g << 8) | b;
    }
}

// Get cursor screen position (CG coordinates, top-left origin)
void GE_MacOS_GetCursorScreenPosition(int* outX, int* outY)
{
    @autoreleasepool {
        NSPoint mouseLoc = [NSEvent mouseLocation];
        NSScreen* mainScreen = [NSScreen mainScreen];
        if (mainScreen) {
            CGFloat screenHeight = mainScreen.frame.size.height;
            *outX = (int)mouseLoc.x;
            *outY = (int)(screenHeight - mouseLoc.y);
        } else {
            *outX = 0;
            *outY = 0;
        }
    }
}

// Check if left mouse button is currently down (global state)
bool GE_MacOS_IsLeftMouseButtonDown()
{
    @autoreleasepool {
        NSUInteger buttons = [NSEvent pressedMouseButtons];
        return (buttons & (1 << 0)) != 0;
    }
}

// Check if Option/Alt key is currently held (global state, works during OS window drag)
bool GE_MacOS_IsOptionKeyDown()
{
    @autoreleasepool {
        NSEventModifierFlags flags = [NSEvent modifierFlags];
        return (flags & NSEventModifierFlagOption) != 0;
    }
}

int GE_MacOS_GetModifierKeyMask()
{
    @autoreleasepool {
        const NSEventModifierFlags flags = [NSEvent modifierFlags];
        int mods = 0;
        if ((flags & NSEventModifierFlagShift) != 0)
            mods |= GLFW_MOD_SHIFT;
        if ((flags & NSEventModifierFlagControl) != 0)
            mods |= GLFW_MOD_CONTROL;
        if ((flags & NSEventModifierFlagOption) != 0)
            mods |= GLFW_MOD_ALT;
        if ((flags & NSEventModifierFlagCommand) != 0)
            mods |= GLFW_MOD_SUPER;
        return mods;
    }
}

// Start global mouse click monitoring for eyedropper
// callback is called with (screenX, screenY) in CG coordinates when user clicks anywhere
typedef void (*GE_GlobalClickCallback)(int screenX, int screenY, void* userData);

void GE_MacOS_StartGlobalClickMonitor(GE_GlobalClickCallback callback, void* userData)
{
    @autoreleasepool {
        // Stop any existing monitor
        if (s_globalMouseMonitor) {
            [NSEvent removeMonitor:s_globalMouseMonitor];
            s_globalMouseMonitor = nil;
        }
        if (s_localMouseMonitor) {
            [NSEvent removeMonitor:s_localMouseMonitor];
            s_localMouseMonitor = nil;
        }
        
        if (!callback)
            return;
        
        // Monitor for left mouse down events globally (outside our app)
        s_globalMouseMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown
                                                                      handler:^(NSEvent* event) {
            NSPoint mouseLoc = [NSEvent mouseLocation];
            NSScreen* mainScreen = [NSScreen mainScreen];
            int screenX = (int)mouseLoc.x;
            int screenY = 0;
            if (mainScreen) {
                CGFloat screenHeight = mainScreen.frame.size.height;
                screenY = (int)(screenHeight - mouseLoc.y);
            }
            callback(screenX, screenY, userData);
        }];
        
        // Also monitor local events (inside our app windows)
        s_localMouseMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown
                                                                    handler:^NSEvent*(NSEvent* event) {
            NSPoint mouseLoc = [NSEvent mouseLocation];
            NSScreen* mainScreen = [NSScreen mainScreen];
            int screenX = (int)mouseLoc.x;
            int screenY = 0;
            if (mainScreen) {
                CGFloat screenHeight = mainScreen.frame.size.height;
                screenY = (int)(screenHeight - mouseLoc.y);
            }
            callback(screenX, screenY, userData);
            return nil; // Consume the event when eyedropper is active
        }];
    }
}

void GE_MacOS_StopGlobalClickMonitor()
{
    @autoreleasepool {
        if (s_globalMouseMonitor) {
            [NSEvent removeMonitor:s_globalMouseMonitor];
            s_globalMouseMonitor = nil;
        }
        if (s_localMouseMonitor) {
            [NSEvent removeMonitor:s_localMouseMonitor];
            s_localMouseMonitor = nil;
        }
    }
}

// Global mouse move monitoring for eyedropper live preview
static id s_globalMoveMonitor = nil;
static id s_localMoveMonitor = nil;

typedef void (*GE_GlobalMoveCallback)(int screenX, int screenY, void* userData);

void GE_MacOS_StartGlobalMoveMonitor(GE_GlobalMoveCallback callback, void* userData)
{
    @autoreleasepool {
        // Stop any existing monitor
        if (s_globalMoveMonitor) {
            [NSEvent removeMonitor:s_globalMoveMonitor];
            s_globalMoveMonitor = nil;
        }
        if (s_localMoveMonitor) {
            [NSEvent removeMonitor:s_localMoveMonitor];
            s_localMoveMonitor = nil;
        }
        
        if (!callback)
            return;
        
        // Monitor for mouse moved events globally
        s_globalMoveMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:NSEventMaskMouseMoved
                                                                     handler:^(NSEvent* event) {
            NSPoint mouseLoc = [NSEvent mouseLocation];
            NSScreen* mainScreen = [NSScreen mainScreen];
            int screenX = (int)mouseLoc.x;
            int screenY = 0;
            if (mainScreen) {
                CGFloat screenHeight = mainScreen.frame.size.height;
                screenY = (int)(screenHeight - mouseLoc.y);
            }
            callback(screenX, screenY, userData);
        }];
        
        // Also monitor local events
        s_localMoveMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskMouseMoved
                                                                   handler:^NSEvent*(NSEvent* event) {
            NSPoint mouseLoc = [NSEvent mouseLocation];
            NSScreen* mainScreen = [NSScreen mainScreen];
            int screenX = (int)mouseLoc.x;
            int screenY = 0;
            if (mainScreen) {
                CGFloat screenHeight = mainScreen.frame.size.height;
                screenY = (int)(screenHeight - mouseLoc.y);
            }
            callback(screenX, screenY, userData);
            return event; // Don't consume move events
        }];
    }
}

void GE_MacOS_StopGlobalMoveMonitor()
{
    @autoreleasepool {
        if (s_globalMoveMonitor) {
            [NSEvent removeMonitor:s_globalMoveMonitor];
            s_globalMoveMonitor = nil;
        }
        if (s_localMoveMonitor) {
            [NSEvent removeMonitor:s_localMoveMonitor];
            s_localMoveMonitor = nil;
        }
    }
}

// Store cursor state for eyedropper mode
static bool s_eyedropperCursorActive = false;
static id s_cursorUpdateMonitor = nil;

// Push a crosshair cursor globally (for eyedropper mode)
void GE_MacOS_PushCrosshairCursor()
{
    @autoreleasepool {
        s_eyedropperCursorActive = true;
        
        // Set the crosshair cursor
        [[NSCursor crosshairCursor] set];
        [[NSCursor crosshairCursor] push];
        
        // Monitor mouse movement to keep resetting the cursor
        // (the system may try to change it back)
        if (s_cursorUpdateMonitor) {
            [NSEvent removeMonitor:s_cursorUpdateMonitor];
        }
        s_cursorUpdateMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskMouseMoved
                                                                      handler:^NSEvent*(NSEvent* event) {
            if (s_eyedropperCursorActive) {
                [[NSCursor crosshairCursor] set];
            }
            return event;
        }];
    }
}

// Pop the pushed cursor (restore previous)
void GE_MacOS_PopCursor()
{
    @autoreleasepool {
        s_eyedropperCursorActive = false;
        
        if (s_cursorUpdateMonitor) {
            [NSEvent removeMonitor:s_cursorUpdateMonitor];
            s_cursorUpdateMonitor = nil;
        }
        
        [NSCursor pop];
        [[NSCursor arrowCursor] set];
    }
}

// Set window always on top using NSWindow level
void GE_MacOS_SetWindowAlwaysOnTop(void* nsWindow, bool enable)
{
    @autoreleasepool {
        NSWindow* window = (__bridge NSWindow*)nsWindow;
        if (window) {
            [window setLevel:enable ? NSFloatingWindowLevel : NSNormalWindowLevel];
        }
    }
}

void GE_MacOS_FocusWindow(void* nsWindow)
{
    @autoreleasepool {
        NSWindow* window = (__bridge NSWindow*)nsWindow;
        if (!window)
        {
            return;
        }

        [NSApp activateIgnoringOtherApps:YES];
        [window makeKeyAndOrderFront:nil];
        [window makeMainWindow];
    }
}

void* GE_MacOS_InstallTitleBarRightClickMonitor(void* nsWindowPtr,
                                                void (*callback)(int, int, void*),
                                                void* userData)
{
    @autoreleasepool {
        NSWindow* window = (__bridge NSWindow*)nsWindowPtr;
        if (!window || !callback)
            return nullptr;

        id monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskRightMouseDown
            handler:^NSEvent*(NSEvent* event) {
                if ([event window] != window)
                    return event;

                // Check if click is in the title bar (above the content view)
                NSPoint locInWindow = [event locationInWindow];
                NSRect contentRect = [[window contentView] frame];
                if (locInWindow.y > NSMaxY(contentRect))
                {
                    // Convert to screen coordinates
                    NSPoint screenPoint = [window convertPointToScreen:locInWindow];
                    callback(static_cast<int>(screenPoint.x),
                             static_cast<int>(screenPoint.y),
                             userData);
                }
                return event;
            }];
        return monitor ? (void*)[monitor retain] : nullptr;
    }
}

void GE_MacOS_RemoveTitleBarRightClickMonitor(void* monitor)
{
    if (!monitor)
        return;
    @autoreleasepool {
        id obj = (id)monitor;
        [NSEvent removeMonitor:obj];
        [obj release];
    }
}

} // extern "C"
