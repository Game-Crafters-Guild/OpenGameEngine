#include "Panels/GameViewPanel.h"

#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "AssetCore/AssetTypes.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "EditorPanelIds.h"
#include "Logger/Logger.h"
#include "UI/Assets/UILayoutAsset.h"
#include "UI/Assets/UIStyleAsset.h"
#include "UI/Controls/Label.h"
#include "UI/UIElement.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/UiDispatcher.h"
#include "UI/ViewOverlayHost.h"
#include "UI/StyleProperties.h"

#include <algorithm>

namespace GameEngine
{

GameViewPanel::GameViewPanel()
    : DockPanel("Game View")
{
    // A .uxml hot reload can destroy and re-create the viewport and the overlay label
    // under this panel; re-resolve them from the reconciled subtree.
    RegisterEventHandler(kEventLayoutReconciled,
                         [this](UIEvent&)
                         {
                             RefreshElementPointers();
                             SetNoCameraOverlayVisible(m_NoCameraOverlayVisible);
                         });
}

GameViewPanel::~GameViewPanel()
{
    // The pending asset callbacks capture this panel; withdraw them before it goes
    // away (destroying an AssetLoadHandle does not).
    if (m_LayoutLoadHandle)
        m_LayoutLoadHandle->Cancel();
    if (m_PanelStyleLoadHandle)
        m_PanelStyleLoadHandle->Cancel();

    ClearViewportPointerHandlers();
    CancelViewportPointer();
}

void GameViewPanel::ClearViewportPointerHandlers()
{
    if (auto* viewport = m_PointerViewport.Get())
        for (const auto token : m_PointerTokens)
            viewport->UnregisterEventHandler(token);
    m_PointerTokens = {};
    m_PointerViewport = {};
}

void GameViewPanel::SetPointerCallback(PointerCallback callback)
{
    CancelViewportPointer();
    m_PointerSink = callback ? std::make_shared<PointerSink>(PointerSink{std::move(callback)}) : nullptr;
}

void GameViewPanel::ClearPointerCallback()
{
    if (m_PointerSink)
    {
        ++m_PointerSink->Generation;
        m_PointerSink->Callback = {};
    }
    m_PointerSink.reset();
}

void GameViewPanel::CancelViewportPointer()
{
    m_PointerDown = false;
    if (m_PointerSink)
    {
        ++m_PointerSink->Generation;
        ForwardViewportPointer(false, 0.0f, 0.0f, false, 0);
    }
}

void GameViewPanel::ForwardViewportPointer(bool over, float x, float y, bool down, int mods)
{
    if (!m_PointerSink)
        return;
    if (auto* owner = GetOwnerManager())
        m_PointerDispatchOwner = UIManagerRef(owner);
    // A detached source can still terminate its delivered gesture on its last
    // live manager.
    UIManager* dispatchOwner = m_PointerDispatchOwner.Get();
    if (!dispatchOwner)
        return;
    auto* dispatcher = dispatchOwner->GetDispatcher();
    if (!dispatcher)
        return;
    const auto sink = m_PointerSink;
    const uint64_t generation = sink->Generation;
    // A second UIManager must not dispatch inside the editor's active bubble:
    // its scratch chain is shared. The existing dispatcher drains after that
    // bubble, before OnMouseButton returns; no render boundary samples the edges.
    // Capture values and the owned sink only, never the panel/viewport/controller.
    dispatcher->Post([sink, generation, over, x, y, down, mods] {
        if (sink->Generation == generation && sink->Callback)
            sink->Callback(over, x, y, down, mods);
    });
}

void GameViewPanel::PrepareForFirstMount(UIManager* ui)
{
    // Nothing left to drive once the layout attempt is settled — applied or terminally
    // failed — and the stylesheet is on.
    if (!ui || ((m_BindApplied || m_BindFailed) && m_StyleApplied))
        return;

    m_PreMountManager = UIManagerRef(ui);

    // Inactive dock tabs are not mounted and therefore do not receive
    // OnPostLayout. Bind their cached assets during the hidden render warmup so
    // activating the tab does not expose an empty panel for its first frame.
    LoadBindAttachLayoutAndStyle(ui);
    RefreshElementPointers();
    SetNoCameraOverlayVisible(m_NoCameraOverlayVisible);
}

void GameViewPanel::OnDockTabActivationArmed(float contentWidth, float contentHeight)
{
    if (m_ActivationArmedCallback)
        m_ActivationArmedCallback(contentWidth, contentHeight);
}

void GameViewPanel::OnMountVisibilityChanged(bool isVisible)
{
    // Dock activation assigns the owner manager before this notification. Bind
    // the cached layout during the mount swap so the first visible frame already
    // contains the viewport; OnPostLayout remains the async-load fallback.
    // Goes straight to the load so only the queued action releases the latch on
    // a detached panel — this entry never queued one.
    if (!isVisible)
    {
        CancelViewportPointer();
        return;
    }
    if (UIManager* ui = GetOwnerManager())
        LoadBindAttachLayoutAndStyle(ui);
}

void GameViewPanel::OnPostLayout()
{
    if (m_BindApplied || m_BindPending || m_BindFailed)
        return;
    if (!GetOwnerManager())
        return;

    m_BindPending = true;
    // A dropped action never runs, so it would never release the latch: release it here
    // instead and let the next laid-out frame try again.
    if (!this->PostAction([this]()
                          { this->BindFromAssetsDeferred(); }))
        m_BindPending = false;
}

void GameViewPanel::BindFromAssetsDeferred()
{
    UIManager* ui = GetOwnerManager();
    if (!ui)
    {
        // Detached between scheduling and draining: transient, so release the
        // latch and let the next laid-out frame schedule a fresh attempt.
        m_BindPending = false;
        return;
    }

    // m_BindPending stays set: LoadBindAttachLayoutAndStyle hands it to the async
    // load, whose completion is what resolves it — and is also where the element
    // pointers and the overlay state get applied, since the UXML children exist
    // only then.
    LoadBindAttachLayoutAndStyle(ui);
}

void GameViewPanel::MarkBindFailed(std::string_view reason)
{
    m_BindPending = false;
    m_BindFailed = true;
    Logger::Log::Error(
        "GameViewPanel: layout bind failed ({}). The panel stays unbound; "
        "check that UI/panels/GameViewPanel.uxml is staged under the editor asset mount.",
        reason);
}

void GameViewPanel::LoadBindAttachLayoutAndStyle(UIManager* ui)
{
    auto& am = EngineCore::GetInstance().GetAssetManager();
    const std::filesystem::path layoutAssetPath = std::filesystem::path("UI") / "panels" / "GameViewPanel.uxml";
    const std::filesystem::path styleAssetPath = std::filesystem::path("UI") / "panels" / "GameViewPanel.css";

    const GUID layoutGuid = am.ResolveAssetGuid(layoutAssetPath, GameEngine::kAssetSourceAliasEditor);
    const GUID styleGuid = am.ResolveAssetGuid(styleAssetPath, GameEngine::kAssetSourceAliasEditor);

    // Entered from three places and once per frame from the warmup, so the arm below is
    // itself the once-guard: it runs while no attempt has started, and every exit it takes
    // either applies the bind, fails it terminally, or hands m_BindPending to the load.
    if (!m_BindApplied && !m_BindFailed && !m_LayoutLoadHandle)
    {
        if (layoutGuid.IsNull())
        {
            MarkBindFailed("the layout guid did not resolve from the editor asset source");
        }
        else if (auto layoutAsset = am.GetAsset(layoutGuid);
                 layoutAsset && layoutAsset->GetType() == AssetType::UILayout)
        {
            // Already cached — bind now so an activating tab never shows an empty frame.
            if (!ui->BindLayoutToSubtreeChildrenFromAsset(
                    this, *static_cast<UILayoutAsset*>(layoutAsset.get())))
            {
                MarkBindFailed("binding the layout into the panel subtree was rejected");
            }
            else
            {
                m_BindApplied = true;
                m_BindPending = false;
                RefreshElementPointers();
                SetNoCameraOverlayVisible(m_NoCameraOverlayVisible);
            }
        }
        else
        {
            // The load is the rest of this attempt; its completion resolves the latch.
            m_BindPending = true;
            m_LayoutLoadHandle = std::make_unique<AssetLoadHandle>(am.LoadAsset(
                layoutGuid,
                         [this, post = GetPostHandle(), layoutGuid](Result<SharedPtr<Asset>, AssetError> r)
                         {
                             // The load completes on a worker; every latch mutation below
                             // runs on the UI thread through PostAction.
                             const bool loaded = r.IsOk() && r.Value() &&
                                                 r.Value()->GetType() == AssetType::UILayout;
                             post.Post([this, layoutGuid, loaded]()
                                              {
                                                  if (!loaded)
                                                      return MarkBindFailed("the layout asset did not load");
                                                  UIManager* ui2 = GetOwnerManager();
                                                  if (!ui2)
                                                      ui2 = m_PreMountManager.Get();
                                                  if (!ui2 || m_BindApplied)
                                                  {
                                                      m_BindPending = false;
                                                      return;
                                                  }
                                                  auto& am2 = EngineCore::GetInstance().GetAssetManager();
                                                  auto a2 = am2.GetAsset(layoutGuid);
                                                  if (!a2 || a2->GetType() != AssetType::UILayout)
                                                      return MarkBindFailed("the loaded layout asset is not a UILayout");
                                                  if (!ui2->BindLayoutToSubtreeChildrenFromAsset(
                                                          this, *static_cast<UILayoutAsset*>(a2.get())))
                                                      return MarkBindFailed("binding the layout into the panel subtree was rejected");

                                                  m_BindApplied = true;
                                                  m_BindPending = false;
                                                  // Now that the layout is actually bound, refresh pointers and
                                                  // apply any deferred visibility state (e.g. camera already exists).
                                                  RefreshElementPointers();
                                                  SetNoCameraOverlayVisible(m_NoCameraOverlayVisible);
                                              });
                         }, AssetLoadPriority::High));
        }
    }

    // The stylesheet is independent of the layout: a panel that fails to bind its .uxml
    // still wants its own styles on the root. m_PanelStyleLoadHandle is this arm's
    // once-guard; a style that never arrives leaves the panel usable, not wedged.
    if (!styleGuid.IsNull() && !m_StyleApplied)
    {
        auto styleAsset = am.GetAsset(styleGuid);
        if (styleAsset && styleAsset->GetType() == AssetType::UIStyle)
        {
            m_StyleApplied = ui->AttachStyleToSubtreeFromAsset(
                this, *static_cast<UIStyleAsset*>(styleAsset.get()));
        }
        else if (!m_PanelStyleLoadHandle)
        {
            m_PanelStyleLoadHandle = std::make_unique<AssetLoadHandle>(am.LoadAsset(
                styleGuid,
                         [this, post = GetPostHandle(), styleGuid](Result<SharedPtr<Asset>, AssetError> r)
                         {
                             if (!r.IsOk() || !r.Value() || r.Value()->GetType() != AssetType::UIStyle)
                                 return;
                             post.Post([this, styleGuid]()
                                              {
                                                  UIManager* ui3 = GetOwnerManager();
                                                  if (!ui3)
                                                      ui3 = m_PreMountManager.Get();
                                                  if (!ui3)
                                                      return;
                                                  auto& am3 = EngineCore::GetInstance().GetAssetManager();
                                                  auto a3 = am3.GetAsset(styleGuid);
                                                  if (a3 && a3->GetType() == AssetType::UIStyle)
                                                  {
                                                      m_StyleApplied = ui3->AttachStyleToSubtreeFromAsset(
                                                          this, *static_cast<UIStyleAsset*>(a3.get()));
                                                  }
                                              });
                         }, AssetLoadPriority::High));
        }
    }
}

void GameViewPanel::RefreshElementPointers()
{
    UIElement* const previous = m_PointerViewport.Get();
    UIElement* const viewport = FindById(EditorPanelIds::GameViewViewport);
    if (viewport != previous)
    {
        // A hot reload that recreates the viewport element needs fresh handlers.
        ClearViewportPointerHandlers();
        CancelViewportPointer();
    }
    m_NoCameraLabel = {};
    if (!viewport)
        return;
    // Hosts a world render target sized from its own rect — the rect has to
    // land on whole device pixels for the `cover` fit to stay 1:1
    // (UIElement::SetSnapRectToDevicePixels).
    viewport->SetSnapRectToDevicePixels(true);
    m_NoCameraLabel = MakeWeakRef(dynamic_cast<Label*>(viewport->FindById(EditorPanelIds::GameViewNoCameraLabel)));
    RegisterViewportPointerHandlers(*viewport);
    Editor::ViewOverlayHost::Get().PublishViewport(Editor::ViewOverlayView::Game, *viewport, {});
}

void GameViewPanel::RegisterViewportPointerHandlers(UIElement& viewport)
{
    if (m_PointerViewport.Get())
        return;
    UIElement* const element = &viewport; // outlives the handlers it owns
    m_PointerViewport = UIElement::MakeWeakRef(element);
    // UIEvent x/y are window-global UI coords. Store the pointer NORMALIZED to the
    // viewport rect (0..1); the controller scales by the game render extent. This
    // is robust to DPI and to the render target not being 1:1 with the viewport
    // element (the HUD is laid out in render-target pixels, not viewport pixels).
    auto forward = [this, element](const UIEvent& e) {
        const float vpW = std::max(1.0f, element->GetLayoutWidth());
        const float vpH = std::max(1.0f, element->GetLayoutHeight());
        const float x = (e.X - element->GetLayoutX()) / vpW;
        const float y = (e.Y - element->GetLayoutY()) / vpH;
        if (!(x >= 0.0f && x < 1.0f && y >= 0.0f && y < 1.0f))
            CancelViewportPointer();
        else
            ForwardViewportPointer(true, x, y, m_PointerDown, e.Mods);
    };
    try
    {
        m_PointerTokens[0] = element->RegisterEventHandler(kEventMouseMove, [forward](UIEvent& e) { forward(e); });
        m_PointerTokens[1] = element->RegisterEventHandler(kEventMouseEnter, [forward](UIEvent& e) { forward(e); });
        m_PointerTokens[2] = element->RegisterEventHandler(kEventMouseDown, [this, forward](UIEvent& e) {
            if (e.Button == 0)
            {
                m_PointerDown = true;
                forward(e);
            }
        });
        m_PointerTokens[3] = element->RegisterEventHandler(kEventMouseUp, [this, forward](UIEvent& e) {
            if (e.Button == 0)
            {
                m_PointerDown = false;
                forward(e);
            }
        });
        m_PointerTokens[4] = element->RegisterEventHandler(kEventMouseLeave, [this](UIEvent&) {
            CancelViewportPointer();
        });
        m_PointerTokens[5] = element->RegisterEventHandler(kEventMouseCancel, [this](UIEvent&) {
            CancelViewportPointer();
        });
    }
    catch (...)
    {
        ClearViewportPointerHandlers();
        throw;
    }
}

void GameViewPanel::SetNoCameraOverlayVisible(bool visible)
{
    // Persist requested visibility even if the UI isn't bound yet.
    m_NoCameraOverlayVisible = visible;
    Label* const label = m_NoCameraLabel.Get();
    if (!label)
        return;

    if (visible)
    {
        label->Overrides()
            .Set(Style::Opacity, 1.0f)
            .Set(Style::PointerEvents, false);
        if (label->GetText().empty())
        {
            label->SetText("No camera in scene (add an ECS Camera component).");
        }
    }
    else
    {
        label->Overrides()
            .Set(Style::Opacity, 0.0f)
            .Set(Style::PointerEvents, false);
    }
}

} // namespace GameEngine
