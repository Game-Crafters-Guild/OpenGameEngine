#pragma once

#include "VCSIntegration/VCSFileStatus.h"

#include <string>

namespace GameEngine::Editor
{

// Shared badge presentation for the provider-agnostic VCS status enum —
// every provider's statuses render identically (data over code: a provider
// contributes per-path statuses, never custom badge widgets).
const char* VcsStatusToDisplayString(VCSFileStatus status);
const char* VcsStatusToColor(VCSFileStatus status);

// Global editor preference for the in-place scene diff indicators shared by
// the Hierarchy and Inspector. Missing preferences default to visible.
bool AreVcsSceneDiffIndicatorsVisible();
bool SetVcsSceneDiffIndicatorsVisible(bool visible, std::string* outError = nullptr);

} // namespace GameEngine::Editor
