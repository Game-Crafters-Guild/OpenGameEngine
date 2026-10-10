#include "ColorPicker/ColorPickerPresenter.h"

#include "Assets/AssetRegistry.h"
#include "ColorPicker/ColorPickerScope.h"
#include "ColorPicker/ColorPickerWindow.h"
#include "Core/Engine.h"
#include "Logger/Logger.h"
#include "Mathematics/Vector2.h"
#include "Panels/ColorPicker.h"
#include "Panels/ColorPickerPopup.h"
#include "Platform/Capabilities.h"
#include "UI/UIManager.h"

#include <filesystem>
#include <utility>

namespace GameEngine
{
namespace Editor
{

void ColorPickerPresenter::Initialize(Dependencies deps)
{
    m_Deps = std::move(deps);
}

OpenColorPickerWindowFn ColorPickerPresenter::AsOpenCallback()
{
    return [this](uint32_t initialArgb, float initialIntensity, ColorPickerCallbacks callbacks)
    { Open(initialArgb, initialIntensity, std::move(callbacks)); };
}

void ColorPickerPresenter::Open(uint32_t initialArgb, float initialIntensity, ColorPickerCallbacks callbacks)
{
    EditorWindowContext* mainWindow = m_Deps.GetMainWindow ? m_Deps.GetMainWindow() : nullptr;
    if (!mainWindow)
    {
        Logger::Log::Error("ColorPickerPresenter: no main window to present the color picker from");
        return;
    }

    ColorPickerScope* scope = callbacks.scope;
    if (scope)
        callbacks = scope->Bind(std::move(callbacks));

    if (Platform::SupportsMultipleWindows())
    {
        const uint64_t windowId = OpenNativeWindow(*mainWindow, initialArgb, initialIntensity, std::move(callbacks));
        if (scope && windowId != 0)
            scope->AddCloseAction([windowId]() { ColorPickerWindow::Close(windowId); });
        return;
    }

    std::weak_ptr<ModalSession> session = OpenModal(*mainWindow, initialArgb, initialIntensity, std::move(callbacks));
    if (scope)
    {
        // This presenter owns the session, so a live session means a live presenter.
        scope->AddCloseAction(
            [this, session = std::move(session)]()
            {
                if (const std::shared_ptr<ModalSession> open = session.lock())
                    CloseModal(*open);
            });
    }
}

uint64_t ColorPickerPresenter::OpenNativeWindow(EditorWindowContext& mainWindow, uint32_t initialArgb,
                                                float initialIntensity, ColorPickerCallbacks callbacks)
{
    ColorPickerWindow::Params params;
    params.assetManager = &EngineCore::GetInstance().GetAssetManager();
    params.ownerWindow = mainWindow.window.get();
    params.styleGuid = params.assetManager->ResolveAssetGuid(std::filesystem::path("UI") / "theme.css",
                                                             kAssetSourceAliasEditor);
    params.isUiReplayActive = m_Deps.IsUiReplayActive;

    ColorPickerWindow::Callbacks cb;
    cb.onApply = std::move(callbacks.onApply);
    cb.onCancel = std::move(callbacks.onCancel);
    cb.onValueChanging = std::move(callbacks.onValueChanging);
    auto winCtx = ColorPickerWindow::CreateWindowContext(initialArgb, initialIntensity, std::move(cb), params);
    if (!winCtx)
        return 0;
    const uint64_t windowId = winCtx->windowId;
    m_Deps.QueueNativeToolWindow(std::move(winCtx));
    return windowId;
}

ColorPickerPopup* ColorPickerPresenter::EnsureModal(EditorWindowContext& mainWindow)
{
    if (ColorPickerPopup* modal = m_Modal.Get())
        return modal;
    UIElement* root = mainWindow.ui ? mainWindow.ui->GetRootElement() : nullptr;
    if (!root)
        return nullptr;

    auto popup = std::make_unique<ColorPickerPopup>();
    // Hosted on the window root rather than inside a panel, so it needs the
    // overlay layer to outrank dropdowns instead of relying on a z-index that
    // hit-testing compares flat across stacking contexts.
    popup->SetOverlayLayer(OverlayLayer::Modal);
    ColorPickerPopup* modal = popup.get();
    m_Modal = UIElement::MakeWeakRef(modal);
    root->AddChild(std::move(popup));
    return modal;
}

std::weak_ptr<ColorPickerPresenter::ModalSession> ColorPickerPresenter::OpenModal(EditorWindowContext& mainWindow,
                                                                                  uint32_t initialArgb,
                                                                                  float initialIntensity,
                                                                                  ColorPickerCallbacks callbacks)
{
    UIElement* root = mainWindow.ui ? mainWindow.ui->GetRootElement() : nullptr;
    if (!root)
    {
        Logger::Log::Error("ColorPickerPresenter: no main-window UI root to host the color picker modal");
        return {};
    }

    // Replacing the session ends the previous open: its pending show and its
    // scope's close action both find it expired.
    m_ModalSession = std::make_shared<ModalSession>();
    std::weak_ptr<ModalSession> session = m_ModalSession;

    // Sampled here rather than inside the deferred action: this runs during the
    // swatch's click dispatch, the same moment ColorPickerWindow samples the
    // cursor for its own placement. By the time the action drains, the pointer
    // has moved.
    const Mathematics::Vector2 cursor = mainWindow.ui->GetMousePosition();

    // Attaching the modal appends to the root's child list, and the swatch
    // click that reaches here is an event dispatch — the same iterator
    // invalidation RemoveChild defers for. Present on the next safe point.
    root->PostSafeAction(
        [this, session, initialArgb, initialIntensity, cursor, callbacks = std::move(callbacks)]() mutable
        {
            const std::shared_ptr<ModalSession> open = session.lock();
            if (!open || open->closed)
                return;
            // The main window is re-resolved here: the cached modal is owned by
            // its UI root, so the window has to be checked before the cache is
            // trusted, not after.
            EditorWindowContext* hostWindow = m_Deps.GetMainWindow();
            ColorPickerPopup* modal = hostWindow ? EnsureModal(*hostWindow) : nullptr;
            if (!modal)
                return;

            // Rebound before Show(): the picker's change handler survives a
            // previous Show, so a stale binding would deliver this open's first
            // value to the property the last open was editing.
            if (callbacks.onValueChanging)
            {
                modal->SetOnValueChanging([fn = std::move(callbacks.onValueChanging)](const ColorPickerValue& v)
                                          { fn(ColorPickerValueToArgb(v), v.intensity); });
            }
            else
            {
                modal->SetOnValueChanging({});
            }
            modal->SetOnCancel(std::move(callbacks.onCancel));
            modal->SetOnApply([fn = std::move(callbacks.onApply)](const ColorPickerValue& v)
                              {
                                  if (fn)
                                      fn(ColorPickerValueToArgb(v), v.intensity);
                              });

            ColorPickerValue initial = ColorPickerValueFromArgb(initialArgb);
            initial.intensity = initialIntensity;
            modal->ShowAt(initial, cursor.x, cursor.y);
        });
    return session;
}

void ColorPickerPresenter::CloseModal(ModalSession& session)
{
    session.closed = true;
    ColorPickerPopup* modal = m_Modal.Get();
    if (modal && modal->IsVisible())
        modal->Hide();
}

} // namespace Editor
} // namespace GameEngine
