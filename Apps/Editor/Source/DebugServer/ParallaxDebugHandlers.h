#pragma once

namespace GameEngine
{

class EditorDebugServer;

// The Parallax steps debug view: shows, in every Scene View, how many height samples the relief
// march of each height-mapped material takes per pixel (`set_parallax_steps_view`).
void RegisterParallaxDebugHandlers(EditorDebugServer& server);

} // namespace GameEngine
