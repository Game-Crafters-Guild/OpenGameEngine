#pragma once

#include <cstdint>

namespace GameEngine
{
class ScrollView;
class UIElement;
}

namespace GameEngine::Editor
{

// Shared row styling for VCS provider settings tabs, so package-provided tabs
// match the built-in ones. (These are the former SettingsPanel file-statics
// the VCS tab bodies depended on.)
void VcsSettingsApplyMinWidth(UIElement* el, float px);
void VcsSettingsApplyMaxWidth(UIElement* el, float px);
void VcsSettingsApplyValueContainerStyle(UIElement* el, float minWidthPx = 150.0f);
void VcsSettingsApplyStatusDotStyle(UIElement* el, uint32_t argb);

// "Repository Status" section header with the Refresh button (pokes the active
// integration's status cache).
void VcsSettingsAddRepositoryStatusHeader(ScrollView* contentBody, const char* logPrefix);

} // namespace GameEngine::Editor
