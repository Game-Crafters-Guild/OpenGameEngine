#pragma once

#include <string>

namespace GameEngine::Editor
{
    /// The version the editor shows in its title bar and info dialog: the engine version from
    /// the root VERSION file and, when the build could read it, the short commit it was built
    /// from ("2026.10.0-alpha.4 (963d147)").
    std::string EditorVersionText();
} // namespace GameEngine::Editor
