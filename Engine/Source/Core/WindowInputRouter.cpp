#include "Core/WindowInputRouter.h"

#include "Engine/GameUI/GameUIHost.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Platform/Window.h"
#include "UI/UIManager.h"

#include <algorithm>

namespace GameEngine
{
namespace
{
// The play surface's UI when this router is its pointer source, which is what
// mapping the window's pointer onto the surface says: the pointer's moves,
// buttons and wheel are then this router's to deliver too. Null for a surface
// embedded in a chrome tree, whose rect only that tree knows.
GameUIHost* SurfaceUiPointedByThisRouter(const WindowInputRouterConfig::PlaySurface& play)
{
    return play.mapGameUiPointer ? play.gameUi : nullptr;
}

// Pointer events stop here while something outside the chain holds the cursor,
// so the grab's own reader is the only one acting on them.
bool PointerIsGrabbedElsewhere(const WindowInputRouterConfig& config)
{
    return config.isPointerGrabbed && config.isPointerGrabbed();
}
} // namespace

void WindowInputRouter::ClientToUiLogical(Platform::Window* window, UIManager* ui, float clientX, float clientY,
                                          float& outX, float& outY)
{
    outX = clientX;
    outY = clientY;
    if (!window || !ui)
        return;
    // UI layout stores rects in CSS logical px. GLFW cursor coords are in
    // window client space, which relates to the physical framebuffer via the
    // OS native content scale. To land in logical:
    //   logical = client * native_scale / IPlatformApi_scale
    //           = client / Additional_UI_Multiplier
    // Both Retina and editor UI-scale are handled by this one ratio.
    float sx = 1.0f;
    float sy = 1.0f;
    window->GetContentScale(sx, sy);
    const float nativeScale = std::max(0.01f, 0.5f * (sx + sy));
    const float logicalScale = std::max(0.01f, ui->GetContentScale());
    const float scale = nativeScale / logicalScale;
    outX = clientX * scale;
    outY = clientY * scale;
}

void WindowInputRouter::ClientToSurfaceLocal(Platform::Window* window, UIManager* ui, float surfaceLogicalX,
                                             float surfaceLogicalY, float clientX, float clientY, float& outX,
                                             float& outY)
{
    float logicalX = clientX;
    float logicalY = clientY;
    ClientToUiLogical(window, ui, clientX, clientY, logicalX, logicalY);
    outX = logicalX - surfaceLogicalX;
    outY = logicalY - surfaceLogicalY;
}

void WindowInputRouter::SendUiMouseFromClientPixels(Platform::Window* window, UIManager* ui, float clientX, float clientY)
{
    if (!window || !ui)
        return;
    float lx = clientX;
    float ly = clientY;
    ClientToUiLogical(window, ui, clientX, clientY, lx, ly);
    ui->OnMouseMove(lx, ly);
}

float WindowInputRouter::UiLogicalToClientScale(Platform::Window* window, UIManager* ui)
{
    if (!window || !ui)
        return 1.0f;
    float sx = 1.0f;
    float sy = 1.0f;
    window->GetContentScale(sx, sy);
    const float nativeScale = std::max(0.01f, 0.5f * (sx + sy));
    const float logicalScale = std::max(0.01f, ui->GetContentScale());
    return logicalScale / nativeScale;
}

void WindowInputRouter::RefreshUiMouseCoordinates(Platform::Window* window, UIManager* ui)
{
    if (!window || !ui)
        return;
    float cx = 0.0f;
    float cy = 0.0f;
    window->GetCursorClientPosition(cx, cy);
    SendUiMouseFromClientPixels(window, ui, cx, cy);
}

void WindowInputRouter::BindBasicHandlers(const WindowInputRouterConfig& config)
{
    if (!config.window)
    {
        return;
    }

    Platform::Window* window = config.window;

    window->SetMouseMoveHandler([window, config](float x, float y)
                                { RouteMouseMove(config, window, x, y); });

    window->SetCursorEnterHandler([config](bool entered) { RouteCursorEnter(config, entered); });

    window->SetFocusHandler([config](bool focused)
                            { RouteFocusChange(config, focused); });

    window->SetScrollHandler([config](float dx, float dy, int mods)
                             { RouteScroll(config, dx, dy, mods); });

    window->SetMouseButtonHandler([config](int button, bool pressed, int mods)
                                  { RouteMouseButton(config, button, pressed, mods); });

    window->SetCharHandler([config](unsigned int codepoint) { RouteChar(config, codepoint); });

    window->SetKeyHandler([config](int key, int action, int mods)
                          { RouteKey(config, key, action, mods); });

    // Focus is reported only when it changes, and a window created focused
    // changed before these handlers existed. Only a focused window has anything
    // to tell: unfocused is what a sink assumes until told otherwise, and a
    // second window bound unfocused must not unfocus a sink it shares with the
    // first.
    if (window->IsFocused())
        RouteFocusChange(config, /*focused=*/true);
}

void WindowInputRouter::RouteMouseMove(const WindowInputRouterConfig& config, Platform::Window* window, float clientX,
                                       float clientY)
{
    if (PointerIsGrabbedElsewhere(config))
        return;

    UIManager* ui = config.getUi ? config.getUi() : nullptr;
    Input::InputSystem* input = config.getInput ? config.getInput() : nullptr;

    if (ui && window)
        SendUiMouseFromClientPixels(window, ui, clientX, clientY);
    if (input)
        input->OnMouseMove({clientX, clientY});
    const WindowInputRouterConfig::PlaySurface play = config.getPlaySurface
                                                          ? config.getPlaySurface()
                                                          : WindowInputRouterConfig::PlaySurface{};
    // A move is never consumed — hover is not a claim — so every stage that
    // tracks a pointer gets one, each in its own pixels.
    if (GameUIHost* surfaceUi = SurfaceUiPointedByThisRouter(play))
    {
        float uiX = clientX;
        float uiY = clientY;
        play.mapGameUiPointer(clientX, clientY, uiX, uiY);
        surfaceUi->OnMouseMove({uiX, uiY});
    }
    if (play.gameplaySink)
    {
        float playX = clientX;
        float playY = clientY;
        if (play.mapGameplayPointer)
            play.mapGameplayPointer(clientX, clientY, playX, playY);
        play.gameplaySink->OnMouseMove({playX, playY});
    }
}

void WindowInputRouter::RouteCursorEnter(const WindowInputRouterConfig& config, bool entered)
{
    UIManager* ui = config.getUi ? config.getUi() : nullptr;
    if (ui)
        ui->OnCursorEnter(entered);
    // Entering carries no position; the move that follows it places the
    // pointer, so only a leave has anything to tell the input sinks.
    if (entered)
        return;
    const WindowInputRouterConfig::PlaySurface play = config.getPlaySurface
                                                          ? config.getPlaySurface()
                                                          : WindowInputRouterConfig::PlaySurface{};
    // Every sink fed this window's moves hears that the pointer is gone, or it
    // keeps reporting it as in the window at the last point inside.
    if (Input::InputSystem* input = config.getInput ? config.getInput() : nullptr)
        input->OnMouseLeave();
    if (play.gameplaySink)
        play.gameplaySink->OnMouseLeave();
    // A surface this router points loses its pointer when the window does:
    // nothing will reach it until the cursor comes back, so a press in flight
    // has to be cancelled rather than left armed on a control the user has
    // walked away from. The host answers for the button state itself, having
    // been told every edge — which is what handing it no pointer means.
    if (GameUIHost* surfaceUi = SurfaceUiPointedByThisRouter(play))
        surfaceUi->UpdatePointer(nullptr);
}

void WindowInputRouter::RouteKey(const WindowInputRouterConfig& config, int key, int action, int mods)
{
    if (config.onKeyPre && config.onKeyPre(key, action, mods))
        return;

    UIManager* ui = config.getUi ? config.getUi() : nullptr;
    Input::InputSystem* input = config.getInput ? config.getInput() : nullptr;
    const WindowInputRouterConfig::PlaySurface play = config.getPlaySurface
                                                          ? config.getPlaySurface()
                                                          : WindowInputRouterConfig::PlaySurface{};

    // Every stage answers whether it acted on the event and the first that did
    // stops the press, so consumption is decided where the behaviour lives rather
    // than by a question this router asks about any stage. Releases are never
    // consumed: a sink that saw the press must see its release, and an unmatched
    // release is a no-op everywhere, so forwarding one costs nothing.
    const bool isRelease = (action == Input::kKeyActionRelease);
    bool consumed = false;

    // Chrome UI first: a viewport or a toggle that does nothing with the key does
    // not consume it, which is what keeps gameplay fed while something holds focus.
    if (ui)
        consumed = ui->OnKey(key, action, mods) && !isRelease;

    // Then the play surface's own UI. Chrome outranks it by being asked first, so
    // a focused editor field keeps its typing; a focused HUD field outranks the
    // game by answering before it. Order is the whole arbitration — no stage
    // declares a priority and none can be asked to yield.
    if (play.gameUi && !consumed)
        consumed = play.gameUi->OnKey(key, action, mods) && !isRelease;

    // Then the game, which consumes exactly what it claims — a key bound to an
    // action of an enabled context, or one it polls. A key it neither binds nor
    // reads keeps travelling and can still fire editor behaviour mid-play.
    if (play.gameplaySink && !consumed)
    {
        const bool hostReserved = !isRelease && input && input->IsHostReservedKey(key, mods);
        if (!hostReserved)
            consumed = play.gameplaySink->OnKey(key, action, mods) && !isRelease;
    }

    if (input && !consumed)
        input->OnKey(key, action, mods);
}

bool WindowInputRouter::RouteChar(const WindowInputRouterConfig& config, unsigned int codepoint)
{
    UIManager* ui = config.getUi ? config.getUi() : nullptr;
    Input::InputSystem* input = config.getInput ? config.getInput() : nullptr;
    const WindowInputRouterConfig::PlaySurface play = config.getPlaySurface
                                                          ? config.getPlaySurface()
                                                          : WindowInputRouterConfig::PlaySurface{};

    // Same stages in the same order as RouteKey: the focused control reports
    // whether it turned this character into an edit, and nothing downstream sees
    // a character that was typed into a UI above it. A key and the character it
    // produces are one keystroke, so withholding only the key would still feed
    // the text to the game.
    bool consumed = false;
    if (ui)
        consumed = ui->OnChar(codepoint);
    if (play.gameUi && !consumed)
        consumed = play.gameUi->OnChar(codepoint);
    if (play.gameplaySink && !consumed)
        play.gameplaySink->OnChar(codepoint);
    if (input && !consumed)
        input->OnChar(codepoint);
    return consumed;
}

void WindowInputRouter::RouteMouseButton(const WindowInputRouterConfig& config, int button, bool pressed, int mods)
{
    if (PointerIsGrabbedElsewhere(config))
        return;

    UIManager* ui = config.getUi ? config.getUi() : nullptr;
    Input::InputSystem* input = config.getInput ? config.getInput() : nullptr;
    const WindowInputRouterConfig::PlaySurface play = config.getPlaySurface
                                                          ? config.getPlaySurface()
                                                          : WindowInputRouterConfig::PlaySurface{};

    // The chain RouteKey walks, for the pointer: each stage answers whether it
    // acted, and the first that did stops the press. Releases are never
    // consumed — a stage that saw the press must see its release, and an
    // unmatched release is a no-op everywhere, so forwarding one costs nothing
    // while withholding one is the stuck-button bug class.
    bool consumed = false;

    // Chrome UI first: a click a panel acted on is that panel's click, and
    // nothing downstream should see it a second time.
    if (ui)
    {
        // The click's mask is live platform state, and it is the only report of
        // a modifier that was already held when this window took focus.
        ui->SyncModifierKeys(mods);
        consumed = ui->OnMouseButton(button, pressed) && pressed;
    }

    // Then the surface's own UI, on the same terms its keys are offered: a HUD
    // button that acted on the press stops it, so the click does not also reach
    // the game behind the HUD. The host dispatches at the position this router
    // last gave it, exactly as chrome dispatches at its own.
    if (GameUIHost* surfaceUi = SurfaceUiPointedByThisRouter(play); surfaceUi && !consumed)
        consumed = surfaceUi->OnMouseButton(button, pressed, mods) && pressed;

    // Then the game, which consumes exactly what it claims. A click it does not
    // claim keeps travelling and can still reach editor behaviour mid-play.
    if (play.gameplaySink && !consumed)
        consumed = play.gameplaySink->OnMouseButton(button, pressed, mods) && pressed;

    if (input && !consumed)
        input->OnMouseButton(button, pressed, mods);

    // Ungated on purpose: this hook mirrors raw physical button state for the
    // host, so withholding a consumed press would leave it believing a held
    // button is up.
    if (config.onMouseButtonPost)
        config.onMouseButtonPost(button, pressed);
}

void WindowInputRouter::RouteScroll(const WindowInputRouterConfig& config, float dx, float dy, int mods)
{
    if (PointerIsGrabbedElsewhere(config))
        return;

    UIManager* ui = config.getUi ? config.getUi() : nullptr;
    if (ui)
    {
        // The platform callback supplies a live query of the OS modifier state,
        // not an event mask, so it outranks the key events this UI tracked: a
        // modifier whose release the window never saw (Cmd+Space to Spotlight,
        // a browser page losing the key-up) would otherwise turn every later
        // plain wheel into the item resize gesture (Ctrl/Cmd + wheel by default).
        ui->ReconcileModifierKeys(mods);
    }

    if (config.onScrollPre && config.onScrollPre(dx, dy))
        return;

    Input::InputSystem* input = config.getInput ? config.getInput() : nullptr;
    const WindowInputRouterConfig::PlaySurface play = config.getPlaySurface
                                                          ? config.getPlaySurface()
                                                          : WindowInputRouterConfig::PlaySurface{};

    // The same chain again. OnScroll answers honestly — a scrollable that
    // actually moved reports handled — and honouring that answer is what stops a
    // wheel over a chrome panel from also driving camera zoom behind it.
    // A wheel has no release half, so there is nothing to always-deliver.
    bool consumed = false;
    if (ui)
        consumed = ui->OnScroll(dx, dy);
    // A HUD list that actually moved takes the tick before the game's zoom does,
    // for the same reason a chrome panel's list does.
    if (GameUIHost* surfaceUi = SurfaceUiPointedByThisRouter(play); surfaceUi && !consumed)
        consumed = surfaceUi->OnScroll({dx, dy});
    if (play.gameplaySink && !consumed)
        consumed = play.gameplaySink->OnMouseScroll({dx, dy}, mods);
    if (input && !consumed)
        input->OnMouseScroll({dx, dy}, mods);
}

void WindowInputRouter::RouteGamepadState(const WindowInputRouterConfig& config, int gamepadIndex, const float* axes,
                                          int axisCount, bool connected)
{
    const WindowInputRouterConfig::PlaySurface play = config.getPlaySurface
                                                          ? config.getPlaySurface()
                                                          : WindowInputRouterConfig::PlaySurface{};

    // Axes are state, not an event: nothing can consume them, so every stage
    // that reads a pad gets its own view — the game and the editor alike.
    if (play.gameplaySink)
        play.gameplaySink->OnGamepadState(gamepadIndex, axes, axisCount, connected);
    Input::InputSystem* input = config.getInput ? config.getInput() : nullptr;
    if (input)
        input->OnGamepadState(gamepadIndex, axes, axisCount, connected);
}

void WindowInputRouter::RouteGamepadButton(const WindowInputRouterConfig& config, int gamepadIndex, int button,
                                           bool down)
{
    const WindowInputRouterConfig::PlaySurface play = config.getPlaySurface
                                                          ? config.getPlaySurface()
                                                          : WindowInputRouterConfig::PlaySurface{};

    // The game answers first, consuming exactly the buttons it claims — bound in
    // an enabled context, or polled — and a button it neither binds nor reads
    // keeps travelling to the editor's own actions.
    bool consumed = false;
    if (play.gameplaySink)
        consumed = play.gameplaySink->OnGamepadButton(gamepadIndex, button, down) && down;

    Input::InputSystem* input = config.getInput ? config.getInput() : nullptr;
    if (input && !consumed)
        input->OnGamepadButton(gamepadIndex, button, down);
}

void WindowInputRouter::RouteFocusChange(const WindowInputRouterConfig& config, bool focused)
{
    const WindowInputRouterConfig::PlaySurface play = config.getPlaySurface
                                                          ? config.getPlaySurface()
                                                          : WindowInputRouterConfig::PlaySurface{};
    if (!focused)
    {
        // The window system synthesizes key and mouse-button releases on focus
        // loss, but only for keys it saw pressed on this window. Held state
        // derived from an event's mods mask has no such release and outlives
        // the focus change.
        UIManager* ui = config.getUi ? config.getUi() : nullptr;
        if (ui)
            ui->ResetModifierKeys();
        if (play.gameUi)
            play.gameUi->ResetKeyboardState();
    }
    // Focus is state, so every sink that keeps one hears it; a sink releases
    // what it holds on loss itself.
    if (Input::InputSystem* input = config.getInput ? config.getInput() : nullptr)
        input->OnWindowFocus(focused);
    if (play.gameplaySink)
        play.gameplaySink->OnWindowFocus(focused);
}

} // namespace GameEngine
