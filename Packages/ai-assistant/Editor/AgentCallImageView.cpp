#include "AgentCallImageView.h"

#include "AgentCallFileMenu.h"

#include "Editor/Assets/EditorAssetActions.h"
#include "Input/KeyCodes.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/ResolvedStyle.h"
#include "UI/StyleProperties.h"
#include "UI/UIManager.h"
#include "UI/UIEvents.h"

#include <cmath>
#include <filesystem>
#include <string>

namespace GameEngine
{
namespace
{
// UIEvent::Button for the left mouse button.
constexpr int kLeftButton = 0;

bool SetPx(UIElement& element, const StyleProp<StyleLength>& property, float px)
{
    const auto existing = element.Overrides().Get(property);
    if (existing.has_value() && existing->IsPx() && std::fabs(existing->Value - px) <= 0.5f)
        return false;
    element.Overrides().Set(property, StyleLength::Px(px));
    return true;
}
} // namespace

AgentCallImageView::AgentCallImageView(const std::string& idSuffix)
{
    AddClass("agent-call-image");
    SetId("AgentCallImage:" + idSuffix);
    SetFocusable(true);
    RegisterEventHandler(kEventMouseUp, [this](UIEvent& event) { OnMouseUp(event); });
    RegisterEventHandler(kEventKeyDown, [this](UIEvent& event) { OnKeyDown(event); });
    // Announced while the element is still whole and owned, its removal included, which a
    // destructor is not: RemoveChild clears the owner before the subtree is destroyed.
    RegisterEventHandler(kEventDetachedFromPanel, [this](UIEvent&) { ReleasePicture(); });
    RegisterEventHandler(kEventAttachedToPanel, [this](UIEvent&) { RequestPicture(); });
}

void AgentCallImageView::RequestPicture()
{
    if (!m_Image || Overrides().Get(Style::BackgroundImage).has_value())
        return;
    // The thumbnail service's copy at about the display size (up to 1024 px), downscaled once off
    // the UI thread and kept on disk; the capture itself only when it is no larger.
    if (const auto& show = Editor::GetEditorAssetActions().ShowImage)
        show(*this, std::filesystem::path(std::u8string(m_Image->Path.begin(), m_Image->Path.end())));
}

void AgentCallImageView::ReleasePicture()
{
    // The UI keeps a background image it loaded until its cache's byte budget is exceeded;
    // this one goes as soon as the image leaves the panel.
    const auto source = Overrides().Get(Style::BackgroundImage);
    if (!source)
        return;
    UIManager* ui = GetOwnerManager();
    if (ui && source->Kind == BackgroundImageSource::SourceKind::Path && !source->Value.empty())
        ui->EvictBackgroundTexture(source->Value);
    UI::Layout::DisableBackgroundOverride(*this);
}

void AgentCallImageView::Show(const AgentCallImage& image)
{
    if (m_Image == image)
        return;
    ReleasePicture();
    m_Image = image;
    const std::filesystem::path file = std::filesystem::path(std::u8string(image.Path.begin(), image.Path.end()));
    RequestPicture();
    if (m_Menu)
        RemoveManipulator(m_Menu);
    m_Menu = ContextMenuManipulator::Create(AgentCallFileMenu(file));
    AddManipulator(m_Menu);
    SetTooltip(image.Path + " (" + std::to_string(image.Width) + " x " + std::to_string(image.Height) +
               "): click to open in Asset View, right-click for the file");
    MarkDirty(StyleDirty | LayoutDirty | VisualDirty);
}

bool AgentCallImageView::Fit(float maxWidth, float maxHeight)
{
    // The bounds hold the hairline too: the picture is fitted to what is left and fills the
    // content box, with no bars beside it.
    const auto& border = GetResolvedStyle().Layout.BorderWidth;
    const float frameWidth = border.Left + border.Right;
    const float frameHeight = border.Top + border.Bottom;
    if (!m_Image || maxWidth <= frameWidth || maxHeight <= frameHeight)
        return false;
    const AgentCallImageSize size =
        FitAgentCallImage(m_Image->Width, m_Image->Height, maxWidth - frameWidth, maxHeight - frameHeight);
    const bool widthMoved = SetPx(*this, Style::Width, size.Width + frameWidth);
    const bool heightMoved = SetPx(*this, Style::Height, size.Height + frameHeight);
    if (!widthMoved && !heightMoved)
        return false;
    MarkDirty(StyleDirty | LayoutDirty);
    return true;
}

void AgentCallImageView::OpenInAssetView()
{
    const auto& preview = Editor::GetEditorAssetActions().Preview;
    if (m_Image && preview)
        preview(std::filesystem::path(std::u8string(m_Image->Path.begin(), m_Image->Path.end())));
}

void AgentCallImageView::OnMouseUp(UIEvent& event)
{
    if (event.Button != kLeftButton)
        return;
    event.Stop();
    OpenInAssetView();
}

void AgentCallImageView::OnKeyDown(UIEvent& event)
{
    if (event.Key != Input::kKeyCode_Enter && event.Key != Input::kKeyCode_Space)
        return;
    event.Stop();
    OpenInAssetView();
}
} // namespace GameEngine
