#pragma once

#include "UI/Controls/DockPanel.h"
#include "UI/UIManagerRef.h"

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

namespace GameEngine
{
struct AssetLoadHandle;
class UIElement;
class UIManager;
class Label;

// Game View panel:
// - displays engine("game_main") (provided by GameViewController)
// - shows a simple overlay message when no ECS camera exists
class GameViewPanel : public DockPanel
{
  public:
    std::string_view DeclaredTabIconClass() const override { return "dock-game-view-icon"; }

    GameViewPanel();
    ~GameViewPanel() override;

    UIElement* GetViewportElement() const { return m_PointerViewport.Get(); }
    bool IsNoCameraOverlayVisible() const { return m_NoCameraOverlayVisible; }

    void SetNoCameraOverlayVisible(bool visible);

    // Forward each viewport input transition immediately, normalized to its rect.
    // The callback resolves the current window/controller, rather than retaining
    // a controller across tear-off/redock. false ends the surface's gesture.
    using PointerCallback = std::function<void(bool over, float x, float y, bool down, int mods)>;
    void SetPointerCallback(PointerCallback callback);
    // Final owner teardown: revoke queued callbacks without invoking the sink.
    // The owner cancels surviving hosts before calling this.
    void ClearPointerCallback();
    friend struct GameViewPanelTestAccess;

    void OnPostLayout() override;
    void OnMountVisibilityChanged(bool isVisible) override;
    void OnDockTabActivationArmed(float contentWidth, float contentHeight) override;
    void PrepareForFirstMount(UIManager* ui);

    void SetActivationArmedCallback(std::function<void(float, float)> callback)
    {
        m_ActivationArmedCallback = std::move(callback);
    }

  private:
    void BindFromAssetsDeferred();
    void LoadBindAttachLayoutAndStyle(UIManager* ui);
    void MarkBindFailed(std::string_view reason);
    void RefreshElementPointers();
    void RegisterViewportPointerHandlers(UIElement& viewport);
    void CancelViewportPointer();
    void ClearViewportPointerHandlers();
    void ForwardViewportPointer(bool over, float x, float y, bool down, int mods);

  private:
    // Deferred layout bind. Three entry points reach it — the pre-mount warmup, the dock
    // mount swap, and the layout pass — so m_BindPending covers the whole attempt (the
    // queued action AND the async asset load it starts) and is cleared only by an outcome:
    // applied, failed, or "panel detached, try again". m_BindFailed is terminal, because
    // retrying an unresolvable editor-mount asset per frame never resolves it.
    bool m_BindApplied = false;
    bool m_StyleApplied = false;
    bool m_BindPending = false;
    bool m_BindFailed = false;

    // Resolved from the layout on bind and on every kEventLayoutReconciled; reads
    // null once a reconcile has destroyed the label.
    WeakRef<Label> m_NoCameraLabel;

    // Desired overlay visibility even before the UXML has been bound (when m_NoCameraLabel is null).
    bool m_NoCameraOverlayVisible = true;

    // Moves preserve the delivered primary state; leaving the surface cancels it.
    bool m_PointerDown = false;
    struct PointerSink
    {
        PointerCallback Callback;
        uint64_t Generation = 0;
    };
    std::shared_ptr<PointerSink> m_PointerSink;
    UIManagerRef m_PointerDispatchOwner; // UI thread only
    // The viewport the handlers below are registered on; non-null exactly while
    // they are live, so it is also the "already registered" test.
    WeakRef<UIElement> m_PointerViewport;
    std::array<EventHandlerToken, 6> m_PointerTokens{};

    std::unique_ptr<AssetLoadHandle> m_LayoutLoadHandle;
    std::unique_ptr<AssetLoadHandle> m_PanelStyleLoadHandle;
    // The manager PrepareForFirstMount last bound through; the load completions fall back
    // to it while the panel is unmounted. Panels are app-owned and move between windows,
    // so the window that owns this manager can close while the panel lives on.
    UIManagerRef m_PreMountManager;
    std::function<void(float, float)> m_ActivationArmedCallback;
};

} // namespace GameEngine
