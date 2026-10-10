#pragma once

#include <functional>

namespace GameEngine
{
class GameUIHost;
class UIManager;
namespace Input
{
class InputSystem;
class IRawInputSink;
}
namespace Platform
{
class Window;
}

struct WindowInputRouterConfig
{
    // Every leg of a live play surface, answered together: the game's UI and the
    // game's input are two stages of one chain over one surface, so one answer
    // settles for all of them whether that surface is live. The default (all legs
    // empty) means this window hosts no play surface now.
    struct PlaySurface
    {
        // The play surface's own UI — the HUD. Asked after chrome and before
        // gameplay, so a focused editor field outranks a HUD field and a focused
        // HUD field outranks the game.
        GameUIHost* gameUi = nullptr;
        // Gameplay's raw sink, which consumes exactly what it claims.
        Input::IRawInputSink* gameplaySink = nullptr;
        // Window client pixels to play-surface pixels (Game View viewport local
        // in the editor). Null leaves them unchanged, which is identity for a
        // window whose whole client area is the surface (Player, tests).
        std::function<void(float clientX, float clientY, float& playX, float& playY)> mapGameplayPointer;
        // Window client pixels to the surface UI's own pixels — the extent its
        // layout is sized to, which is not the mapping above: gameplay reads the
        // surface's own space, the HUD is laid out at its composite target.
        //
        // Binding it says this router is the surface UI's pointer source, so the
        // UI's moves, buttons and wheel travel the chain here alongside its keys.
        // It is left unbound where a chrome element owns the surface's rect (the
        // editor's Game View): that element is the only stage that knows whether
        // the cursor is over the surface, so it points the host itself, and a
        // second stream from here would fight it.
        std::function<void(float clientX, float clientY, float& uiX, float& uiY)> mapGameUiPointer;
    };

    Platform::Window* window = nullptr;
    std::function<UIManager*()> getUi;
    std::function<Input::InputSystem*()> getInput;
    std::function<bool(float dx, float dy)> onScrollPre;
    std::function<bool(int key, int action, int mods)> onKeyPre;
    std::function<void(int button, bool pressed)> onMouseButtonPost;
    // A pointer grab held outside this chain. While it answers true the cursor
    // belongs to something that is not this window's UI — a screen-wide sampler
    // that previews the pixel under the cursor wherever it goes and takes the
    // click that commits it — so moves, buttons and the wheel reach no stage and
    // a click meant for a pixel cannot also press the control beneath it. Keys
    // keep travelling: a grab is cancelled with one.
    std::function<bool()> isPointerGrabbed;
    std::function<PlaySurface()> getPlaySurface;
};

class WindowInputRouter
{
  public:
    static void BindBasicHandlers(const WindowInputRouterConfig& config);

    // Routing that depends only on the event (and optional window for the
    // client→logical scale), so the handlers stay one-line forwarders and the
    // policy can be exercised without a live window. A null window leaves
    // client pixels unchanged.
    // Stages answer in order — chrome UI, the play surface's own UI, the
    // gameplay sink, then the app InputSystem — and the first that reports it
    // acted stops the press. Each UI stage answers for the control that acted;
    // the gameplay sink answers for the keys it claims (bound in an enabled
    // context, or polled), so a key the game ignores keeps travelling and can
    // still fire editor behaviour mid-play. Only a press or repeat of a key the
    // host's InputSystem reserves (ActionDesc::hostReserved) skips the gameplay
    // sink; a modified key otherwise
    // follows the same claim rules as a bare one. Releases are never consumed
    // and reach every sink, so a consumed press can never leave a key stuck down.
    static void RouteKey(const WindowInputRouterConfig& config, int key, int action, int mods);
    // The character half of a keystroke, walking the same stages and gated on
    // the same consumption answer as RouteKey so text typed into one UI is not
    // also delivered to the next one or to the game. Returns whether a stage
    // turned it into an edit, which is what tells injected text whether it
    // landed anywhere.
    static bool RouteChar(const WindowInputRouterConfig& config, unsigned int codepoint);
    static void RouteMouseMove(const WindowInputRouterConfig& config, Platform::Window* window, float clientX,
                               float clientY);
    // The cursor entering or leaving the window's client area.
    static void RouteCursorEnter(const WindowInputRouterConfig& config, bool entered);
    // Button transitions walk the same stages as keys, with each UI dispatching
    // at the callback: a press a chrome control acted on stops there, one a HUD
    // control acted on stops there, one the gameplay sink claims stops there,
    // and only what none of them took reaches the app InputSystem. Releases are
    // never consumed and reach every stage, so a consumed press can never leave
    // a button stuck down. There is no chord carve-out — a modified click is an
    // ordinary click. onMouseButtonPost is raw physical button state for the
    // host and is never withheld.
    static void RouteMouseButton(const WindowInputRouterConfig& config, int button, bool pressed, int mods);
    // Same stages for the wheel: a scrollable that actually moved consumes it,
    // so it does not also drive camera zoom behind the panel or the game behind
    // the HUD. A wheel has no release half, so there is nothing to
    // always-deliver.
    static void RouteScroll(const WindowInputRouterConfig& config, float dx, float dy, int mods);
    // A gamepad's continuous state — whether a pad is in the slot and where its
    // axes stand — reaches every stage that keeps input state, because an axis
    // is no more consumable than a pointer position.
    static void RouteGamepadState(const WindowInputRouterConfig& config, int gamepadIndex, const float* axes,
                                  int axisCount, bool connected);
    // A gamepad button edge walks the stages a pad can reach: the gameplay sink,
    // which consumes exactly what it claims, and then the app InputSystem, which
    // sees only what the game left. A pad carries no pointer and no modifiers,
    // so there is no hover to hit-test and no chord to carve out, and the UI
    // stages are absent because no UI element answers for a gamepad. Releases
    // are never consumed and reach every stage, so a consumed press cannot leave
    // a button stuck down.
    static void RouteGamepadButton(const WindowInputRouterConfig& config, int gamepadIndex, int button, bool down);
    // The window gaining or losing focus. Every sink that keeps input state
    // hears it — focus is state, like a pointer position — and on loss the UI
    // stages drop the modifiers they adopted from event masks, which the
    // platform never releases. BindBasicHandlers seeds it from the window,
    // because the platform reports focus only when it changes and a window
    // created focused changed before any handler was bound.
    static void RouteFocusChange(const WindowInputRouterConfig& config, bool focused);
    // Re-query the OS cursor and push scaled coordinates into the UI. Call after
    // IPlatformApi content scale changes so hit-testing matches without waiting
    // for the next GLFW cursor callback (same client position would otherwise
    // dedupe in UIManager::OnMouseMove).
    static void RefreshUiMouseCoordinates(Platform::Window* window, UIManager* ui);
    // Convert GLFW client-space cursor coordinates to UI logical pixels and push
    // them into UIManager. Use from custom mouse-move handlers (e.g. standalone
    // dialogs that wrap the basic forwarding with extra logic) so the
    // Additional UI Scale multiplier is honored consistently with the main
    // editor windows.
    static void SendUiMouseFromClientPixels(Platform::Window* window, UIManager* ui, float clientX, float clientY);

    // Convert GLFW client-space cursor coordinates to UI logical pixels
    // (same transform SendUiMouseFromClientPixels applies). outX/outY are
    // unchanged when window or ui is null.
    static void ClientToUiLogical(Platform::Window* window, UIManager* ui, float clientX, float clientY,
                                  float& outX, float& outY);

    // Client pixels to coordinates local to a surface embedded in the UI, given
    // that surface's UI-logical origin: the ClientToUiLogical conversion rebased
    // on the surface. This is what maps a window pointer onto the editor's Game
    // View so runtime input sees play-surface-local pixels; the caller supplies
    // the origin because finding the surface is the host's business, not this
    // class's.
    static void ClientToSurfaceLocal(Platform::Window* window, UIManager* ui, float surfaceLogicalX,
                                     float surfaceLogicalY, float clientX, float clientY, float& outX, float& outY);

    // Scale factor for converting UI logical coordinates (GetLayoutX/Y/Width/Height,
    // returned by Yoga in CSS-logical px) to GLFW client-space coordinates. This
    // is the inverse of the mouse-coord conversion done by SendUiMouseFromClientPixels:
    //   client_pixels = ui_logical_pixels * (logical_scale / native_scale)
    //                 = ui_logical_pixels * Additional_UI_Multiplier
    // Use when sizing or positioning native child views (web view, embedded native
    // controls) or when creating new top-level windows from UI-layout dimensions.
    static float UiLogicalToClientScale(Platform::Window* window, UIManager* ui);
};

} // namespace GameEngine
