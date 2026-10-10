#pragma once

namespace GameEngine
{

class EditorDebugServer;

// assistant_bind {token} and assistant_authorize {tool, arguments}: the AI Assistant's MCP
// server binds each connection to its conversation, and asks before a tool that acts
// outside the editor (a file write, a process). The decision is the gate's that claims the
// method (DebugRequestGate::ClaimedMethods), taken in its Before: a request that reaches
// the handler was admitted. With no claiming gate (the AI Assistant package is not loaded)
// both refuse, so the server fails closed.
void RegisterAssistantSessionHandlers(EditorDebugServer& server);

} // namespace GameEngine
