#pragma once

// Shader-byte loaders shared by the old-arm overlay path (SceneViewController)
// and the RenderGraph overlay declarations (SceneViewOverlaysRG). Each loader memoizes
// per process; the bytes never change after first load.

#include <cstdint>
#include <vector>

namespace GameEngine
{

namespace Rendering
{
enum class ShaderSourceKind : uint8_t;
}

// `kind` is the form the consuming device ingests
// (IDevice::PreferredShaderSource); the memoized bytes are the ones fed to
// pipeline creation.
bool LoadEditorLinesShaderBytes(Rendering::ShaderSourceKind kind,
                                std::vector<uint8_t>& outVs,
                                std::vector<uint8_t>& outFs);
bool LoadGizmoCompositeShaderBytes(std::vector<uint8_t>& outVs, std::vector<uint8_t>& outFs);
bool LoadSelectionOutlineShaderBytes(std::vector<uint8_t>& outVs, std::vector<uint8_t>& outFs);
bool LoadSelectionMaskShaderBytes(std::vector<uint8_t>& outVs, std::vector<uint8_t>& outFs);
bool LoadSelectionMaskAlphaShaderBytes(std::vector<uint8_t>& outVs, std::vector<uint8_t>& outFs);
bool LoadSelectionMaskSkinnedVertexBytes(std::vector<uint8_t>& outVs);

} // namespace GameEngine
