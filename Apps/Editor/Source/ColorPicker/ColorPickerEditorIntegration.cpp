#include "ColorPicker/ColorPickerEditorIntegration.h"

#include "ColorPicker/ColorPickerContext.h"
#include "Platform/Window.h"

#include <unordered_map>
#include <utility>

namespace GameEngine
{
namespace ColorPickerEditorIntegration
{
namespace
{
// Per-window storage keyed by stable WindowId.
static std::unordered_map<uint64_t, std::unique_ptr<ColorPickerContext>> s_Ctx;
}

ColorPickerContext* GetOrCreateContext(uint64_t windowId)
{
    if (windowId == 0)
        return nullptr;
    auto& slot = s_Ctx[windowId];
    if (!slot)
        slot = std::make_unique<ColorPickerContext>();
    return slot.get();
}

ColorPickerContext* GetContext(uint64_t windowId)
{
    if (windowId == 0)
        return nullptr;
    auto it = s_Ctx.find(windowId);
    return (it != s_Ctx.end()) ? it->second.get() : nullptr;
}

void Cleanup(uint64_t windowId)
{
    if (windowId == 0)
        return;

    auto it = s_Ctx.find(windowId);
    if (it == s_Ctx.end())
        return;

    ColorPickerContext* cpCtx = it->second.get();
    if (cpCtx)
    {
        // Defensive teardown for global eyedropper state: this prevents stale global
        // callbacks from outliving the window context during close/shutdown.
        if (cpCtx->eyedropperActive || cpCtx->eyedropperMonitorsArmed)
        {
            cpCtx->eyedropperActive = false;
            cpCtx->eyedropperMonitorsArmed = false;
            Platform::Window::StopGlobalMoveMonitor();
            Platform::Window::StopGlobalClickMonitor();
            Platform::Window::PopGlobalCursor();
        }

        cpCtx->ownerUi = nullptr;
        cpCtx->window = nullptr;
        cpCtx->onApply = nullptr;
        cpCtx->onCancel = nullptr;
        cpCtx->onChange = nullptr;
    }

    s_Ctx.erase(it);
}

ColorPickerContext* GetOrCreateContext(EditorApplication::EditorWindowContext* ctx)
{
    return ctx ? GetOrCreateContext(ctx->windowId) : nullptr;
}

ColorPickerContext* GetContext(EditorApplication::EditorWindowContext* ctx)
{
    return ctx ? GetContext(ctx->windowId) : nullptr;
}

void Cleanup(EditorApplication::EditorWindowContext* ctx)
{
    if (ctx)
        Cleanup(ctx->windowId);
}
} // namespace ColorPickerEditorIntegration
} // namespace GameEngine
