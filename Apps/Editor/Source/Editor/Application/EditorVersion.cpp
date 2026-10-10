#include "Editor/Application/EditorVersion.h"

#include "Core/EngineVersion.h"
#include "Editor/BuildCommit.h"

namespace GameEngine::Editor
{
std::string EditorVersionText()
{
    std::string text = kEngineVersion;
    if (*kBuildCommit != '\0')
    {
        text += " (";
        text += kBuildCommit;
        text += ')';
    }
    return text;
}
} // namespace GameEngine::Editor
