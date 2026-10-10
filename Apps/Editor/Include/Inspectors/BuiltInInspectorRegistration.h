#pragma once

namespace GameEngine
{

// Registers the editor's built-in component and asset inspectors, then lets
// loaded editor plugins contribute theirs. EditorApplication owns startup
// sequencing but does not need per-inspector knowledge.
void RegisterBuiltInInspectors();

} // namespace GameEngine
