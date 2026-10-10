#include "SceneView/SceneViewNoticeOverlay.h"

#include "UI/Controls/Label.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace GameEngine::Editor
{
namespace
{
constexpr const char* kHiddenClass = "hidden";
constexpr std::chrono::seconds kNoticeDuration{4};

// The notice raised last. Serial counts the notices raised, so the overlay restarts the banner
// for a notice with the same text as the one before it.
struct RaisedNotice
{
    std::string Text;
    std::chrono::steady_clock::time_point Until{};
    uint64_t Serial = 0;
};

RaisedNotice& Raised()
{
    static RaisedNotice s_Raised;
    return s_Raised;
}
} // namespace

void ShowSceneViewNotice(std::string text)
{
    RaisedNotice& raised = Raised();
    raised.Text = std::move(text);
    raised.Until = std::chrono::steady_clock::now() + kNoticeDuration;
    ++raised.Serial;
}

void SceneViewNoticeOverlay::Present(ViewOverlayView view, UIElement& layer, const ViewOverlayCamera& /*camera*/)
{
    if (view != ViewOverlayView::Scene)
        return;
    auto banner = std::make_unique<Label>();
    banner->AddClass("view-overlay-banner");
    banner->AddClass("scene-view-notice");
    banner->AddClass(kHiddenClass);
    m_Banners.push_back(UIElement::MakeWeakRef(banner.get()));
    layer.AddChild(std::move(banner));
    m_ShownSerial = 0;
    m_Visible = false;
}

void SceneViewNoticeOverlay::Update()
{
    m_Banners.erase(std::remove_if(m_Banners.begin(), m_Banners.end(),
                                   [](const UIElement::WeakRef<Label>& banner) { return banner.Get() == nullptr; }),
                    m_Banners.end());
    const RaisedNotice& raised = Raised();
    const bool visible = raised.Serial != 0 && std::chrono::steady_clock::now() < raised.Until;
    if (visible == m_Visible && raised.Serial == m_ShownSerial)
        return;
    m_Visible = visible;
    m_ShownSerial = raised.Serial;
    for (const UIElement::WeakRef<Label>& weak : m_Banners)
    {
        Label* banner = weak.Get();
        banner->SetText(raised.Text);
        if (visible)
            banner->RemoveClass(kHiddenClass);
        else
            banner->AddClass(kHiddenClass);
    }
}

} // namespace GameEngine::Editor
