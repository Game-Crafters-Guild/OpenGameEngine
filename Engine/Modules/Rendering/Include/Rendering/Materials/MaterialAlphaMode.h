#pragma once

#include <cstdint>
#include <string_view>

namespace GameEngine
{

enum class MaterialAlphaMode : uint32_t
{
    Opaque = 0,
    Mask = 1,
    Blend = 2
};

inline MaterialAlphaMode MaterialAlphaModeFromString(std::string_view s)
{
    if (s == "Blend" || s == "AlphaBlend" || s == "alphaBlend")
        return MaterialAlphaMode::Blend;
    if (s == "Mask" || s == "AlphaTest" || s == "alphaTest")
        return MaterialAlphaMode::Mask;
    return MaterialAlphaMode::Opaque;
}

inline const char* MaterialAlphaModeToString(MaterialAlphaMode mode)
{
    switch (mode)
    {
    case MaterialAlphaMode::Blend: return "Blend";
    case MaterialAlphaMode::Mask: return "Mask";
    default: return "Opaque";
    }
}

} // namespace GameEngine
