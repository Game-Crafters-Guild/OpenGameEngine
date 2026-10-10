#include "Editor/Assets/ShaderGlslOpen.h"

#include "Editor/Settings/ScriptEditorSettings.h"

#include "Rendering/ShaderGraph/SgTagParser.h"

#include <fstream>

namespace GameEngine::Editor
{

bool GetOpenShaderGraphGlslInMaterialGraph()
{
    return ScriptEditorSettings::Get().GetOpenShaderGraphGlslInMaterialGraph();
}

void SetOpenShaderGraphGlslInMaterialGraph(bool openInMaterialGraph)
{
    ScriptEditorSettings::Get().SetOpenShaderGraphGlslInMaterialGraph(openInMaterialGraph);
}

bool FileIsShaderGraphGlsl(const std::filesystem::path& path)
{
    if (path.empty())
        return false;

    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;

    std::string head;
    head.resize(8192);
    in.read(head.data(), static_cast<std::streamsize>(head.size()));
    head.resize(static_cast<size_t>(in.gcount()));
    return ShaderGraph::IsShaderGraphSource(head);
}

bool ShouldOpenGlslInMaterialGraph(const std::filesystem::path& path)
{
    if (!GetOpenShaderGraphGlslInMaterialGraph())
        return false;
    return FileIsShaderGraphGlsl(path);
}

} // namespace GameEngine::Editor
