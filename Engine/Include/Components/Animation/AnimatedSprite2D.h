#pragma once

#include "AssetCore/GUID.h"
#include "Components/AssetRef.h"
#include "Types/StringUtils.h"
#include "Types/Types.h"

#include <algorithm>
#include <cstring>
#include <string_view>

namespace GameEngine::Components
{

struct AnimatedSprite2D
{
    static constexpr uint32 kAnimationNameCapacity = 64;

    Components::AssetRef<AssetType::SpriteFrames> spriteFramesGuid;
    char animationName[kAnimationNameCapacity] = {};
    float32 timeSeconds = 0.0f;
    float32 speedScale = 1.0f;
    bool playing = true;
    bool loop = true;

    void SetAnimationName(std::string_view value)
    {
        std::memset(animationName, 0, sizeof(animationName));
        const size_t count = std::min(value.size(), sizeof(animationName) - 1);
        if (count > 0)
            std::memcpy(animationName, value.data(), count);
    }

    std::string_view AnimationName() const { return FixedStringView(animationName); }
};

} // namespace GameEngine::Components
