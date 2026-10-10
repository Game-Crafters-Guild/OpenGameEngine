#pragma once

namespace GameEngine
{

class EditorApplication;
class EditorDebugServer;

// The agent's world mark-up methods (markup_list, markup_get, markup_create,
// markup_update, markup_comment, markup_frame, markup_set_visible), over the edit
// world (Markups/MarkupRequests.h holds what each does).
void RegisterMarkupDebugHandlers(EditorDebugServer& server, EditorApplication& app);

} // namespace GameEngine
