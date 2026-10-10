#pragma once

namespace GameEngine
{

class EditorDebugServer;

// Readback of the sky the renderer draws this frame: every SkySettings input the sky passes read,
// exactly as the sky system handed them over, so two runs or two builds can be compared input by
// input instead of pixel by pixel.
void RegisterSkyDebugHandlers(EditorDebugServer& server);

} // namespace GameEngine
