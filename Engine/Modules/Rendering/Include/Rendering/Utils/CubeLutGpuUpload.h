#pragma once

#include "Rendering/Core/Device.h"
#include "Rendering/Utils/CubeLutFileParser.h"

#include <string>

namespace GameEngine::Rendering
{

struct CubeLutGpuTextures
{
    TextureHandle Lut3D{};
    TextureHandle Lut1DStrip{};
};

bool CreateCubeLutGpuTextures(IDevice* device,
                              const CubeLutParseResult& parsed,
                              TextureFormat textureFormat,
                              CubeLutGpuTextures& outTextures,
                              std::string* outError = nullptr);

} // namespace GameEngine::Rendering
