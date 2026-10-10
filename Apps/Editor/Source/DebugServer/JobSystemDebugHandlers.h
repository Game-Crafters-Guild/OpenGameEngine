#pragma once

namespace GameEngine
{

class EditorDebugServer;

// The engine job system's live state: what every compute worker and blocking thread runs, each
// lane's backlog, each channel's queue, and the cumulative queue traffic.
void RegisterJobSystemDebugHandlers(EditorDebugServer& server);

} // namespace GameEngine
