#pragma once

// ShaderCapabilityDetector: scans GLSL source text for known composition
// function signatures to determine what roles a shader file can fill.
//
// Stateless utility. Called by inspectors and the material build pipeline.

#include <cstdint>
#include <string>

namespace GameEngine::Rendering
{

enum class ShaderCapability : uint32_t
{
    None           = 0,
    Surface              = 1u << 0, // Defines EvaluateSurface(SurfaceInput) -> SurfaceOutput
    VertexModifier       = 1u << 1, // Defines ModifyVertex(vec3, InstanceData) -> vec3
    VertexOutputModifier = 1u << 2, // Defines ModifyVertex(inout VertexOutput, InstanceData)
};

inline ShaderCapability operator|(ShaderCapability a, ShaderCapability b)
{
    return static_cast<ShaderCapability>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline ShaderCapability operator&(ShaderCapability a, ShaderCapability b)
{
    return static_cast<ShaderCapability>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline bool HasCapability(ShaderCapability flags, ShaderCapability test)
{
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(test)) != 0;
}

class ShaderCapabilityDetector
{
  public:
    // Scan GLSL source text and return a bitmask of detected capabilities.
    static ShaderCapability Detect(const std::string& source);

    // Convenience: read a file and detect capabilities.
    static ShaderCapability DetectFromFile(const std::string& path);
};

} // namespace GameEngine::Rendering
