#pragma once

#include <cstdlib>

// Centralized renderer debug env-var checks, evaluated once via Meyers singleton.
// Lightweight header — no engine dependencies. Safe to include from any TU.
namespace GameEngine::Rendering
{

struct RenderingDebugFlags
{
    bool traceRendering = false;
    bool srgbDiag = false;

    static inline const RenderingDebugFlags& Get()
    {
        static const RenderingDebugFlags instance = [] {
            RenderingDebugFlags f;
            auto enabled = [](const char* name) {
                const char* v = std::getenv(name);
                return v && *v && *v != '0';
            };
            f.traceRendering = enabled("GE_DEBUG_TRACE_RENDERING");
            f.srgbDiag = enabled("GE_DEBUG_SRGB_ENCODE");
            return f;
        }();
        return instance;
    }
};

} // namespace GameEngine::Rendering
