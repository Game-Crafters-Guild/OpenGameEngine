#pragma once

#include <string>

namespace GameEngine::Editor
{
    /// The status-bar build tag: config name plus the running executable's
    /// modification time ("Debug Build - Aug 15 17:14"). The timestamp comes
    /// from a runtime stat of the executable — the truth about the binary in
    /// memory, unlike __DATE__/__TIME__ which only move when one translation
    /// unit recompiles. Falls back to the bare config tag on stat failure.
    std::string BuildConfigStatusText();
} // namespace GameEngine::Editor
