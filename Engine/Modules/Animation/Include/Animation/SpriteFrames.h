#pragma once

#include "AssetCore/Asset.h"
#include "AssetCore/GUID.h"
#include "AssetCore/Types.h"

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine
{
namespace Animation
{

struct SpriteFrame
{
    GUID TextureGuid;
    std::string Name;
    float32 DurationSeconds = 0.1f;
    float32 U0 = 0.0f;
    float32 V0 = 0.0f;
    float32 U1 = 1.0f;
    float32 V1 = 1.0f;
};

struct SpriteFrameAnimation
{
    std::string Name;
    bool Loop = true;
    std::vector<SpriteFrame> Frames;
};

class SpriteFrames : public ::GameEngine::Asset
{
public:
    static constexpr int32 kSchemaVersion = 1;

    SpriteFrames(const ::GameEngine::GUID& guid, const std::filesystem::path& path)
        : ::GameEngine::Asset(guid, ::GameEngine::AssetType::SpriteFrames, path) {}

    bool Load() override;
    bool LoadFromData(const Vector<uint8>& data) override;
    void Unload() override;

    bool SaveToData(Vector<uint8>& outData) const;

    const std::vector<SpriteFrameAnimation>& Animations() const { return m_Animations; }
    const SpriteFrameAnimation* FindAnimation(const std::string& name) const;
    const SpriteFrame* Sample(const std::string& animationName, float32 timeSeconds) const;

    void SetAnimationsForTest(std::vector<SpriteFrameAnimation> animations) { m_Animations = std::move(animations); }

private:
    bool ParseJson(const std::string& text);
    std::string SerializeJson() const;

    std::vector<SpriteFrameAnimation> m_Animations;
};

} // namespace Animation
} // namespace GameEngine
