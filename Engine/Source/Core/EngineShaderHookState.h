#pragma once

#include "Core/Engine.h"
#include "Assets/ShaderProgramAsset.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"

#include <filesystem>
#include <utility>

namespace GameEngine
{
struct EngineCore::ShaderHookState
{
    Rendering::Utils::ShaderFileLoaderFunc Loader = Rendering::Utils::GetShaderFileLoader();
    Rendering::Utils::ShaderPathResolverFunc Resolver = Rendering::Utils::GetShaderPathResolver();
    Rendering::ShaderPackageRebuildActionFunc RebuildAction = Rendering::GetShaderPackageRebuildAction();
    std::filesystem::path CacheRoot = ShaderProgramAsset::GetShaderCacheRoot();

    ~ShaderHookState()
    {
        Rendering::Utils::SetShaderFileLoader(Loader);
        Rendering::Utils::SetShaderPathResolver(Resolver);
        Rendering::SetShaderPackageRebuildAction(RebuildAction);
        ShaderProgramAsset::SetShaderCacheRoot(std::move(CacheRoot));
    }
};
} // namespace GameEngine
