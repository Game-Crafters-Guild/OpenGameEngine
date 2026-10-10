#include "Editor/Vcs/VcsSettingsUi.h"

#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "Logger/Logger.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/ScrollView.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"
#include "VCSIntegration/IVCSIntegration.h"

#include <memory>

namespace GameEngine::Editor
{

void VcsSettingsApplyMinWidth(UIElement* el, float px)
{
    if (!el)
        return;
    el->Overrides().Set(Style::MinWidth, StyleLength::Px(px));
}

void VcsSettingsApplyMaxWidth(UIElement* el, float px)
{
    if (!el)
        return;
    el->Overrides().Set(Style::MaxWidth, StyleLength::Px(px));
}

void VcsSettingsApplyValueContainerStyle(UIElement* el, float minWidthPx)
{
    if (!el)
        return;
    el->Overrides()
        .Set(Style::MinWidth, StyleLength::Px(minWidthPx))
        .Set(Style::Display, DisplayMode::Flex)
        .Set(Style::AlignItems, AlignItems::Center)
        .Set(Style::Gap, StyleLength::Px(8.0f));
}

void VcsSettingsApplyStatusDotStyle(UIElement* el, uint32_t argb)
{
    if (!el)
        return;
    el->Overrides()
        .Set(Style::Width, StyleLength::Px(10.0f))
        .Set(Style::Height, StyleLength::Px(10.0f))
        .Set(Style::BorderRadius, CornerRadiiTLTRBRBL{5.0f, 5.0f, 5.0f, 5.0f})
        .Set(Style::BackgroundColor, (uint32_t)argb)
        .Set(Style::Display, DisplayMode::Flex);
}

void VcsSettingsAddRepositoryStatusHeader(ScrollView* contentBody, const char* logPrefix)
{
    if (!contentBody)
        return;

    auto header = std::make_unique<UIElement>();
    header->AddClass("settings-section-header");
    header->AddClass("settings-vcs-status-header");

    auto title = std::make_unique<Label>();
    title->SetText("Repository Status");
    title->AddClass("settings-vcs-status-title");
    header->AddChild(std::move(title));

    auto button = std::make_unique<Button>();
    button->SetText("Refresh");
    button->AddClass("small");
    button->AddClass("settings-button");
    button->AddClass("settings-vcs-refresh-button");
    button->RegisterEventHandler(kEventButtonClick, [logPrefix](UIEvent&) {
        if (IVCSIntegration* vcs = EditorVcsProviderRegistry::Get().ActiveIntegration())
        {
            vcs->RefreshStatus();
            Logger::Log::Info("{}: Status cache cleared, will refresh on next cycle", logPrefix);
        }
    });
    header->AddChild(std::move(button));

    contentBody->AddContent(std::move(header));
}

} // namespace GameEngine::Editor
