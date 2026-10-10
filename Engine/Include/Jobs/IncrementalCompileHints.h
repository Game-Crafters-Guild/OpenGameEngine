#pragma once

#include <string>
#include <vector>

namespace GameEngine {
// Production-facing hint APIs for incremental server compile.
// Implemented in HotReloadTasks.cpp; shared with test hooks.
void SetChangedFilesForNextCompile(const std::vector<std::string>& pathsUtf8);
std::vector<std::string> TakeChangedFilesForNextCompile();
}

