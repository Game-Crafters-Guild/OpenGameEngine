#include "Rendering/Materials/ShaderCapabilityDetector.h"

#include <fstream>

namespace GameEngine::Rendering
{

namespace
{

// Capability scans must ignore comments: adapter_vertex.glsl documents the
// `inout VertexOutput` form in a comment while the live wind modifier is the
// simple `vec3 ModifyVertex` form. Scanning the comment would intern the
// output-modifier variant, which the cook never produced.
std::string StripComments(const std::string& source)
{
    std::string out;
    out.reserve(source.size());
    bool inLine = false;
    bool inBlock = false;
    for (size_t i = 0; i < source.size(); ++i)
    {
        if (inLine)
        {
            if (source[i] == '\n')
            {
                inLine = false;
                out.push_back('\n');
            }
            continue;
        }
        if (inBlock)
        {
            if (source[i] == '*' && i + 1 < source.size() && source[i + 1] == '/')
            {
                inBlock = false;
                ++i;
            }
            continue;
        }
        if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '/')
        {
            inLine = true;
            ++i;
            continue;
        }
        if (source[i] == '/' && i + 1 < source.size() && source[i + 1] == '*')
        {
            inBlock = true;
            ++i;
            continue;
        }
        out.push_back(source[i]);
    }
    return out;
}

} // namespace

ShaderCapability ShaderCapabilityDetector::Detect(const std::string& source)
{
    const std::string code = StripComments(source);
    ShaderCapability caps = ShaderCapability::None;

    if (code.find("EvaluateSurface(") != std::string::npos)
        caps = caps | ShaderCapability::Surface;

    // Detect the extended form first (inout VertexOutput) so it takes
    // precedence over the simple form (both contain "ModifyVertex(").
    if (code.find("inout VertexOutput") != std::string::npos
        && code.find("ModifyVertex(") != std::string::npos)
    {
        caps = caps | ShaderCapability::VertexOutputModifier;
    }
    else if (code.find("ModifyVertex(") != std::string::npos)
    {
        caps = caps | ShaderCapability::VertexModifier;
    }

    return caps;
}

ShaderCapability ShaderCapabilityDetector::DetectFromFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
        return ShaderCapability::None;

    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return Detect(content);
}

} // namespace GameEngine::Rendering
