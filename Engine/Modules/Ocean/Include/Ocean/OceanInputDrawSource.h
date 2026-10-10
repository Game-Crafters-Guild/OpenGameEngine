#pragma once

#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine::Ocean
{

enum class OceanInputFamily : uint32
{
    AnimatedWaves = 0u,
    Height = 1u,
    Foam = 2u,
    DynamicWaves = 3u,
    Flow = 4u,
    Clip = 5u,
    Albedo = 6u,
    Depth = 7u,
    Shadow = 8u,
};

enum class OceanInputBlendMode : uint32
{
    Replace = 0u,
    Additive = 1u,
    Multiply = 2u,
    Minimum = 3u,
    Maximum = 4u,
};

enum class OceanInputGeometry : uint32
{
    Rectangle = 0u,
    EngineMesh = 1u,
    Line = 2u,
    Trail = 3u,
    Particle = 4u,
    SplineBand = 5u,
    CustomNative = 6u,
};

struct OceanInputDrawContext
{
    uint32 CascadeIndex = 0u;
    float32 CascadeOriginX = 0.0f;
    float32 CascadeOriginZ = 0.0f;
    float32 CascadeWorldSize = 0.0f;
    float32 Time = 0.0f;
};

// Renderer-neutral packet produced by an input source. Native render backends can
// attach geometry/material handles through DrawSourceId while CPU evaluation and
// diagnostics use the shared footprint/value fields.
struct OceanInputDrawPacket
{
    OceanInputFamily Family = OceanInputFamily::AnimatedWaves;
    OceanInputBlendMode Blend = OceanInputBlendMode::Replace;
    OceanInputGeometry Geometry = OceanInputGeometry::Rectangle;
    int32 Priority = 0;
    uint32 EntityId = 0u;
    uint64 DrawSourceId = 0u;
    float32 CenterX = 0.0f;
    float32 CenterZ = 0.0f;
    float32 ExtentX = 0.0f;
    float32 ExtentZ = 0.0f;
    float32 Feather = 0.0f;
    float32 Value[4] = {};
};

class IOceanInputDrawSource
{
public:
    virtual ~IOceanInputDrawSource() = default;
    virtual void CollectOceanInputDraws(const OceanInputDrawContext& context,
                                        std::vector<OceanInputDrawPacket>& outPackets) const = 0;
};

class OceanInputDrawRegistry
{
public:
    void Add(const IOceanInputDrawSource* source);
    void Remove(const IOceanInputDrawSource* source);
    void Clear();
    void Collect(const OceanInputDrawContext& context,
                 std::vector<OceanInputDrawPacket>& outPackets) const;

    static void Sort(std::span<OceanInputDrawPacket> packets);
    static float32 ApplyBlend(float32 current, float32 value, float32 weight,
                              OceanInputBlendMode blend);

private:
    std::vector<const IOceanInputDrawSource*> m_Sources;
};

// Lightweight source adapter for custom native producers that already have a
// packet callback and do not need a bespoke class.
using OceanInputCollectFn = void (*)(const OceanInputDrawContext&,
                                     std::vector<OceanInputDrawPacket>&, void* userData);

class OceanNativeInputDrawSource final : public IOceanInputDrawSource
{
public:
    OceanNativeInputDrawSource(OceanInputCollectFn collect, void* userData)
        : m_Collect(collect), m_UserData(userData) {}

    void CollectOceanInputDraws(const OceanInputDrawContext& context,
                                std::vector<OceanInputDrawPacket>& outPackets) const override;

private:
    OceanInputCollectFn m_Collect = nullptr;
    void* m_UserData = nullptr;
};

} // namespace GameEngine::Ocean
