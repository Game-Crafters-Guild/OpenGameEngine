#pragma once

namespace GameEngine::Editor
{
class MarkupEditorBridge;

// The Markup component's inspector section: the status, the free tags, who made and last
// changed it, the description and the thread with an Add comment field. Edits are made
// for the user, one undo step each; in play mode the section is read-only under the
// bridge's play-mode notice. Selecting a mark-up marks its updates seen. Also registers
// the MarkupVolume section (its shape, refused in play mode as every mark-up edit is) and
// both components' section titles, "Mark-up" and "Mark-up Volume".
void RegisterMarkupInspector(MarkupEditorBridge& bridge);

} // namespace GameEngine::Editor
