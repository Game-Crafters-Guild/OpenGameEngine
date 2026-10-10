#include "Ocean/OceanShaderProgram.h"

#include "Logger/Logger.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <utility>

namespace GameEngine::Ocean
{

bool LoadOceanShaderProgram(const Rendering::ShaderProgramCompileRequest& request,
                            Rendering::ShaderSourceKind kind,
                            Rendering::ShaderProgramCompileResult& result,
                            std::string* outError)
{
    // The cooked package, named after the program. This is the only path a
    // runtime without a compiler has, so it is tried first everywhere rather
    // than gated on the platform: a desktop build reads the same package the
    // web build does, which is what keeps the two honest about the same bytes.
    if (!request.debugName.empty())
    {
        const std::string path = "Shaders/" + request.debugName + ".shaderpkg";
        Rendering::ShaderPackage pkg{};
        std::string loadErr;
        if (Rendering::LoadShaderPkg(path, kind, pkg, &loadErr))
        {
            result.meta = std::move(pkg.meta);
            result.stageBytes = std::move(pkg.stageBytes);
            result.cacheInfoJson = std::move(pkg.cacheInfoJson);
            return true;
        }
    }

    // No package: a developer desktop still compiles from source, which is what
    // makes editing an ocean shader a rebuild-free loop.
    return Rendering::ShaderCompileService::CompileProgramToCache(request, kind, result, outError);
}

} // namespace GameEngine::Ocean
