#pragma once

#include "UI/UIStyle.h"

namespace GameEngine::UI
{

/// The UI module's default stylesheet (Engine/Modules/UI/Assets/defaults.css, compiled into the
/// module): the base look of the engine's controls before any document styles them. Parsed once
/// per process and shared by every UIManager, which adds it ahead of all other sheets. Its origin
/// is StyleOrigin::UserAgent, so any theme or document rule wins over it whatever its specificity.
/// It is engine code, not an asset: editing it takes a rebuild, and nothing hot-reloads it. Null
/// only if the sheet does not parse, which logs an Error and asserts in developer builds.
StylesheetHandle GetDefaultStylesheet();

} // namespace GameEngine::UI
