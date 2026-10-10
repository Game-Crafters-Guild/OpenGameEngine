#pragma once

#include <filesystem>

namespace GameEngine::Editor
{

// When true, double-clicking a material shader graph .glsl opens the Node Graph panel.
// Plain composition/standalone .glsl files still open in the script editor.
bool GetOpenShaderGraphGlslInMaterialGraph();
void SetOpenShaderGraphGlslInMaterialGraph(bool openInMaterialGraph);

bool FileIsShaderGraphGlsl(const std::filesystem::path& path);
bool ShouldOpenGlslInMaterialGraph(const std::filesystem::path& path);

} // namespace GameEngine::Editor
