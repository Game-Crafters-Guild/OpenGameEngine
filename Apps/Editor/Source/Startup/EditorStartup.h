#pragma once

#include "Core/CommandLine.h"
#include "Core/Application.h" // ApplicationConfig
#include "Startup/EditorCommandLine.h"

namespace GameEngine::Editor::Startup
{
GameEngine::ApplicationConfig BuildEditorApplicationConfig(const EditorCommandLineArgs& editorArgs,
                                                           const GameEngine::EngineArgs& engineArgs);
} // namespace GameEngine::Editor::Startup

