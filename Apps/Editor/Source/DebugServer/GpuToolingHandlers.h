#pragma once

namespace GameEngine
{

class EditorDebugServer;
class EditorApplication;

void RegisterGpuToolingHandlers(EditorDebugServer& server, EditorApplication& app);

} // namespace GameEngine
