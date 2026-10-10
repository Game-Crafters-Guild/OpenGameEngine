#pragma once

#include "Rendering/Core/Device.h"
#include "Rendering/Materials/ShaderCompileService.h"

#include <string>

namespace GameEngine::Ocean
{
    /**
     * @brief Resolves an ocean shader program: cooked package first, compiler second.
     *
     * The ocean asked ShaderCompileService directly, which probes a
     * content-addressed cache and, on a miss, compiles the GLSL. That works on
     * a developer desktop and nowhere else — a shipped build has no compiler,
     * so every ocean program failed and the water fell back to Gerstner with no
     * shallows, foam advection or interactive ripples.
     *
     * The build cooks a package per program (see the shaderpkg declarations in
     * Rendering/CMakeLists.txt), named after the request's debugName. Loading
     * that first is what the core pipeline nodes do, and it is the only path
     * available to a runtime without a compiler.
     *
     * @param request  The program, exactly as the compiler path would take it.
     *                 debugName selects the package.
     * @param kind     The shader form the consuming device ingests
     *                 (IDevice::PreferredShaderSource).
     * @param result   Stage bytes and reflection, whichever path supplied it.
     * @param outError Set when both paths fail.
     * @return True when `result` holds a usable program.
     */
    bool LoadOceanShaderProgram(const Rendering::ShaderProgramCompileRequest& request,
                                Rendering::ShaderSourceKind kind,
                                Rendering::ShaderProgramCompileResult& result,
                                std::string* outError);
}
