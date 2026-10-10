#include "SceneView/SceneViewOverlayShared.h"

#include "Logger/Logger.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

namespace GameEngine
{

bool LoadEditorLinesShaderBytes(Rendering::ShaderSourceKind kind,
                                std::vector<uint8_t>& outVs,
                                std::vector<uint8_t>& outFs)
{
    static std::vector<uint8_t> sVs;
    static std::vector<uint8_t> sFs;
    static bool sTried = false;
    static bool sLoggedFailure = false;

    if (!sTried)
    {
        sTried = true;

        GameEngine::Rendering::ShaderPackage pkg{};
        std::string err;
        if (GameEngine::Rendering::LoadShaderPkg("Shaders/editor_lines.shaderpkg", kind, pkg, &err))
        {
            auto itVs = pkg.stageBytes.find("vs");
            auto itFs = pkg.stageBytes.find("fs");
            if (itVs != pkg.stageBytes.end())
                sVs = std::move(itVs->second);
            if (itFs != pkg.stageBytes.end())
                sFs = std::move(itFs->second);
        }
        else
        {
            if (!sLoggedFailure)
            {
                sLoggedFailure = true;
                Logger::Log::Error("SceneView: failed to load shaderpkg 'Shaders/editor_lines.shaderpkg': {}", err);
            }
        }

        // Fallback: try loose SPIR-V (dev convenience; may not be staged in packaged builds).
        if (sVs.empty())
            sVs = GameEngine::Rendering::Utils::LoadShaderFile("Shaders/lines.vert.spv");
        if (sFs.empty())
            sFs = GameEngine::Rendering::Utils::LoadShaderFile("Shaders/lines.frag.spv");
    }

    outVs = sVs;
    outFs = sFs;
    return !outVs.empty() && !outFs.empty();
}

// Loads the fullscreen copy shader used by the Scene View gizmo composite pass.
// The gizmo overlay renders into a dedicated SceneView.GizmoColor target; this
// shader samples it and the composite pipeline alpha-blends the result onto the
// tonemapped SceneView.Color.
bool LoadGizmoCompositeShaderBytes(std::vector<uint8_t>& outVs, std::vector<uint8_t>& outFs)
{
    static std::vector<uint8_t> sVs;
    static std::vector<uint8_t> sFs;
    static bool sTried = false;

    if (!sTried)
    {
        sTried = true;
        sVs = GameEngine::Rendering::Utils::LoadShaderFile("Shaders/fullscreen_noinput.vert.spv");
        sFs = GameEngine::Rendering::Utils::LoadShaderFile("Shaders/copy.frag.spv");
    }

    outVs = sVs;
    outFs = sFs;
    return !outVs.empty() && !outFs.empty();
}

// Loads the fullscreen Sobel selection-outline shader. The fragment shader
// reads a single-channel R8 selection mask, runs a 3x3 edge detector, and
// outputs a constant outline color with alpha proportional to gradient
// magnitude. Caller alpha-blends the result onto SceneView.Color.
bool LoadSelectionOutlineShaderBytes(std::vector<uint8_t>& outVs, std::vector<uint8_t>& outFs)
{
    static std::vector<uint8_t> sVs;
    static std::vector<uint8_t> sFs;
    static bool sTried = false;

    if (!sTried)
    {
        sTried = true;
        sVs = GameEngine::Rendering::Utils::LoadShaderFile("Shaders/fullscreen_noinput.vert.spv");
        sFs = GameEngine::Rendering::Utils::LoadShaderFile("Shaders/selection_outline.frag.spv");
    }

    outVs = sVs;
    outFs = sFs;
    return !outVs.empty() && !outFs.empty();
}

// Loads the position-only selection mask draw shader. The vertex shader
// reads only the position attribute from the standard interleaved core
// vertex buffer; the fragment shader writes 1.0 into the R8 mask. Used by
// the per-entity SelectionMask pass that feeds the SelectionOutline composite.
bool LoadSelectionMaskShaderBytes(std::vector<uint8_t>& outVs, std::vector<uint8_t>& outFs)
{
    static std::vector<uint8_t> sVs;
    static std::vector<uint8_t> sFs;
    static bool sTried = false;

    if (!sTried)
    {
        sTried = true;
        sVs = GameEngine::Rendering::Utils::LoadShaderFile("Shaders/selection_mask.vert.spv");
        sFs = GameEngine::Rendering::Utils::LoadShaderFile("Shaders/selection_mask.frag.spv");
    }

    outVs = sVs;
    outFs = sFs;
    return !outVs.empty() && !outFs.empty();
}

// Alpha-aware selection mask variant used for card foliage. It samples UV0
// albedo alpha before writing the mask so outlines follow the visible cutout.
bool LoadSelectionMaskAlphaShaderBytes(std::vector<uint8_t>& outVs, std::vector<uint8_t>& outFs)
{
    static std::vector<uint8_t> sVs;
    static std::vector<uint8_t> sFs;
    static bool sTried = false;

    if (!sTried)
    {
        sTried = true;
        sVs = GameEngine::Rendering::Utils::LoadShaderFile("Shaders/selection_mask_alpha.vert.spv");
        sFs = GameEngine::Rendering::Utils::LoadShaderFile("Shaders/selection_mask_alpha.frag.spv");
    }

    outVs = sVs;
    outFs = sFs;
    return !outVs.empty() && !outFs.empty();
}

// Vertex shader for the skinned variant of the selection mask pipeline. Reads
// joints/weights and samples the shared bone palette atlas SSBO so animated
// meshes contribute their posed silhouette to the outline mask.
bool LoadSelectionMaskSkinnedVertexBytes(std::vector<uint8_t>& outVs)
{
    static std::vector<uint8_t> sVs;
    static bool sTried = false;

    if (!sTried)
    {
        sTried = true;
        sVs = GameEngine::Rendering::Utils::LoadShaderFile("Shaders/selection_mask_skinned.vert.spv");
    }

    outVs = sVs;
    return !outVs.empty();
}

} // namespace GameEngine
