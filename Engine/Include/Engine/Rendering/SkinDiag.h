#pragma once

#include <cstdlib>

namespace GameEngine::Engine::Renderer::SkinDiag {

// Returns true when the GE_RENDER_DIAG_SKINNING environment variable is set.
// Used by AnimationSystem, SkinningUploadSystem, and RenderExtractionSystem
// to trace the per-entity skinning pipeline.
inline bool IsEnabled()
{
    static bool s_Enabled = []() {
        const char* env = std::getenv("GE_RENDER_DIAG_SKINNING");
        return env && !(env[0] == '0' && env[1] == '\0');
    }();
    return s_Enabled;
}

} // namespace GameEngine::Engine::Renderer::SkinDiag
