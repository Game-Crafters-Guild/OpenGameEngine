#pragma once

namespace Logger
{
class RingBufferSink;
}

namespace GameEngine
{

class EditorDebugServer;
class EditorApplication;
struct SceneViewCameraPose;

// Register all MCP debug handlers on the given server. Must be called before
// EditorDebugServer::Start(). The RingBufferSink pointer is used for log
// retrieval; may be null if unavailable. RenderDoc is resolved lazily through
// the EditorApplication reference when trigger_capture is invoked.
void RegisterDebugHandlers(EditorDebugServer& server,
                           EditorApplication& app,
                           Logger::RingBufferSink* ringBufferSink);

// Moves the main window's Scene View camera to `pose` and tells its panel the new
// angles. False when there is no Scene View.
bool ApplyMainSceneViewPose(EditorApplication& app, const SceneViewCameraPose& pose);

} // namespace GameEngine
