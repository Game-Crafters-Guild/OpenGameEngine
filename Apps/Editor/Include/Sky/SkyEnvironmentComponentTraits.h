#pragma once

namespace GameEngine::Editor
{

// Registers the Sky Environment's EditorComponentTraits. Registration replaces by component type,
// so this is the one place every editor trait of the sky is set; a second registration elsewhere
// would drop the traits set here.
void RegisterSkyEnvironmentComponentTraits();

} // namespace GameEngine::Editor
