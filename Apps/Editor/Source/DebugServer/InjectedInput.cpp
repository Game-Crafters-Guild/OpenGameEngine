#include "DebugServer/InjectedInput.h"

#include "Core/WindowInputRouter.h"
#include "Platform/Window.h"
#include "UI/UIManager.h"

#include <cmath>

namespace GameEngine::Editor
{
namespace
{
// Injection names the UIManager it is aimed at. A window's bound getUi returns
// null while a UI replay owns that manager — correct for OS events, which would
// contaminate the deterministic stream, and wrong for a tool's deliberate click,
// which would simply be dropped. Pin the UI sink to the caller's manager and
// leave every other sink on the window's own policy.
WindowInputRouterConfig ConfigTargeting(const WindowInputRouterConfig& config, UIManager* ui)
{
    WindowInputRouterConfig injected = config;
    injected.getUi = [ui]() { return ui; };
    return injected;
}
} // namespace

bool IsInjectablePointerPosition(float logicalX, float logicalY)
{
    return std::isfinite(logicalX) && std::isfinite(logicalY);
}

void InjectMouseMove(const WindowInputRouterConfig& config, Platform::Window* window, UIManager* ui, float logicalX,
                     float logicalY)
{
    if (!ui)
        return;

    if (!IsInjectablePointerPosition(logicalX, logicalY))
        return;

    // BindBasicHandlers is what populates the sinks; an unbound config routes
    // nowhere, so the direct feed is the only one that delivers the event.
    if (!config.getUi)
    {
        ui->OnMouseMove(logicalX, logicalY);
        return;
    }

    // Null window leaves the scale unknown, so logical == client.
    const float scale = WindowInputRouter::UiLogicalToClientScale(window, ui);
    if (!window)
    {
        // RouteMouseMove needs the window to convert client pixels back for the
        // UI, and skips that sink without one. Deliver the UI leg here; the
        // editor and runtime legs still go through the router.
        ui->OnMouseMove(logicalX, logicalY);
    }
    WindowInputRouter::RouteMouseMove(ConfigTargeting(config, ui), window, logicalX * scale, logicalY * scale);
}

void InjectMouseButton(const WindowInputRouterConfig& config, UIManager* ui, int button, bool pressed)
{
    if (!ui)
        return;

    if (!config.getUi)
    {
        ui->OnMouseButton(button, pressed);
        return;
    }

    // RouteMouseButton reconciles held modifiers against the platform mask the
    // event carries. A synthesized click carries no such mask, and passing zero
    // would clear modifiers an earlier send_key established. Re-asserting the
    // mask the UI already holds is idempotent: a modifier counts as held when
    // its key is physically down or the mask says so, so deriving from the
    // current mask reproduces the current state.
    WindowInputRouter::RouteMouseButton(ConfigTargeting(config, ui), button, pressed, ui->GetModifierKeys());
}

void InjectKey(const WindowInputRouterConfig& config, UIManager* ui, int key, int action, int mods)
{
    if (!ui)
        return;

    if (!config.getUi)
    {
        ui->OnKey(key, action, mods);
        return;
    }

    WindowInputRouter::RouteKey(ConfigTargeting(config, ui), key, action, mods);
}

bool InjectChar(const WindowInputRouterConfig& config, UIManager* ui, unsigned int codepoint)
{
    if (!ui)
        return false;

    if (!config.getUi)
        return ui->OnChar(codepoint);

    return WindowInputRouter::RouteChar(ConfigTargeting(config, ui), codepoint);
}

} // namespace GameEngine::Editor
