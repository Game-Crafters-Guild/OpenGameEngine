#pragma once

namespace GameEngine
{

class EditorApplication;
class EditorDebugServer;

void RegisterTerrainDebugHandlers(EditorDebugServer& server, EditorApplication& app);

} // namespace GameEngine
