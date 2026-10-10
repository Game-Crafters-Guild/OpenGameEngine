#pragma once

namespace GameEngine
{

class EditorDebugServer;

// Ground-truth queries for placement verification: what height the composed
// terrain actually carries at a world XZ, and how far each placed spline piece
// sits above or below it.
void RegisterGroundQueryDebugHandlers(EditorDebugServer& server);

} // namespace GameEngine
