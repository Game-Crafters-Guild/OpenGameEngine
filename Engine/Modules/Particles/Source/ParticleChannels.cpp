#include "Particles/ParticleChannels.h"

#include "Logger/Logger.h"

#include <array>
#include <cassert>

namespace GameEngine::Particles
{
namespace
{
struct AttributeChannels
{
    ParticleChannel Current;
    ParticleChannel Birth;
    ParticleChannel Entry;
    uint32 Components;
};

constexpr AttributeChannels kAttributes[kParticleAttributeCount] = {
    {ParticleChannel::Position, ParticleChannel::BirthPosition, ParticleChannel::EntryPosition, 3},
    {ParticleChannel::Velocity, ParticleChannel::BirthVelocity, ParticleChannel::EntryVelocity, 3},
    {ParticleChannel::Size, ParticleChannel::BirthSize, ParticleChannel::EntrySize, 1},
    {ParticleChannel::Scale, ParticleChannel::BirthScale, ParticleChannel::EntryScale, 3},
    {ParticleChannel::Color, ParticleChannel::BirthColor, ParticleChannel::EntryColor, 4},
    {ParticleChannel::Color, ParticleChannel::BirthColor, ParticleChannel::EntryColor, 1},
    {ParticleChannel::Rotation, ParticleChannel::BirthRotation, ParticleChannel::EntryRotation, 1},
    {ParticleChannel::Spin, ParticleChannel::BirthSpin, ParticleChannel::EntrySpin, 1},
    {ParticleChannel::Lifetime, ParticleChannel::BirthLifetime, ParticleChannel::EntryLifetime, 1},
    {ParticleChannel::AnimationSpeed, ParticleChannel::BirthAnimationSpeed, ParticleChannel::EntryAnimationSpeed, 1},
    {ParticleChannel::AnimationOffset, ParticleChannel::BirthAnimationOffset, ParticleChannel::EntryAnimationOffset, 1},
    {ParticleChannel::Custom0, ParticleChannel::BirthCustom0, ParticleChannel::EntryCustom0, 1},
    {ParticleChannel::Custom1, ParticleChannel::BirthCustom1, ParticleChannel::EntryCustom1, 1},
    {ParticleChannel::Custom2, ParticleChannel::BirthCustom2, ParticleChannel::EntryCustom2, 1},
    {ParticleChannel::Custom3, ParticleChannel::BirthCustom3, ParticleChannel::EntryCustom3, 1},
};

constexpr uint32 kBirthFirst = static_cast<uint32>(ParticleChannel::BirthPosition);
constexpr uint32 kSnapshotAttributes = 14;
static_assert(static_cast<uint32>(ParticleChannel::EntryCustom3) - kBirthFirst + 1 == 2 * kSnapshotAttributes);

// Element components of each snapshot channel, in channel order from BirthPosition.
constexpr uint32 kSnapshotComponents[kSnapshotAttributes] = {3, 3, 1, 3, 4, 1, 1, 1, 1, 1, 1, 1, 1, 1};
} // namespace

ParticleChannel AttributeChannel(ParticleAttribute attribute, ParticleBasis basis)
{
    const uint32 index = static_cast<uint32>(attribute);
    assert(index < kParticleAttributeCount);
    switch (basis)
    {
    case ParticleBasis::Birth:
        return kAttributes[index].Birth;
    case ParticleBasis::Entry:
        return kAttributes[index].Entry;
    case ParticleBasis::Current:
    default:
        return kAttributes[index].Current;
    }
}

uint32 AttributeComponents(ParticleAttribute attribute)
{
    const uint32 index = static_cast<uint32>(attribute);
    assert(index < kParticleAttributeCount);
    return kAttributes[index].Components;
}

std::string_view ParticleChannelName(ParticleChannel channel)
{
    static constexpr std::array<std::string_view, kParticleChannelCount> kNames = {
    "SpawnIndex",
    "Phase",
    "Age",
    "PreviousAge",
    "PhaseAge",
    "Lifetime",
    "Position",
    "PreviousPosition",
    "Velocity",
    "Size",
    "PreviousSize",
    "Scale",
    "PreviousScale",
    "Color",
    "PreviousColor",
    "Rotation",
    "PreviousRotation",
    "Spin",
    "AnimationSpeed",
    "AnimationOffset",
    "Custom0",
    "Custom1",
    "Custom2",
    "Custom3",
    "LightIntensity",
    "LightRange",
    "FiredEvents",
    "PhaseEntries",
    "Contacts",
    "Settled",
    "Distance",
    "PreviousDistance",
    "TrailSlot",
    "BirthPosition",
    "BirthVelocity",
    "BirthSize",
    "BirthScale",
    "BirthColor",
    "BirthRotation",
    "BirthSpin",
    "BirthLifetime",
    "BirthAnimationSpeed",
    "BirthAnimationOffset",
    "BirthCustom0",
    "BirthCustom1",
    "BirthCustom2",
    "BirthCustom3",
    "EntryPosition",
    "EntryVelocity",
    "EntrySize",
    "EntryScale",
    "EntryColor",
    "EntryRotation",
    "EntrySpin",
    "EntryLifetime",
    "EntryAnimationSpeed",
    "EntryAnimationOffset",
    "EntryCustom0",
    "EntryCustom1",
    "EntryCustom2",
    "EntryCustom3"};
    const uint32 index = static_cast<uint32>(channel);
    return index < kNames.size() ? kNames[index] : std::string_view("Unknown");
}

void ParticleChannels::ReportUndeclared(ParticleChannel channel) const
{
    Logger::Log::Error("Particle processor '{}' reads the {} channel, which no processor of its stack declared, so it "
                       "has no storage: add the channel to the processor's Reads or Writes, or to what its "
                       "ParameterChannels returns.",
                       m_Reader.empty() ? std::string_view("(the runtime)") : m_Reader, ParticleChannelName(channel));
    assert(false && "A particle processor read a channel no processor of its stack declared");
}

void ParticleChannels::ReportWrongType(ParticleChannel channel, size_t elementSize) const
{
    Logger::Log::Error("Particle processor '{}' reads the {} channel as {}-byte elements, and the channel holds {}-byte "
                       "elements: read it as the type the channel stores.",
                       m_Reader.empty() ? std::string_view("(the runtime)") : m_Reader, ParticleChannelName(channel),
                       elementSize, ChannelElementSize(channel));
    assert(false && "A particle processor read a channel as the wrong element type");
}

uint32 ChannelElementSize(ParticleChannel channel)
{
    const uint32 index = static_cast<uint32>(channel);
    if (index >= kBirthFirst)
        return kSnapshotComponents[(index - kBirthFirst) % kSnapshotAttributes] * static_cast<uint32>(sizeof(float));
    switch (channel)
    {
    case ParticleChannel::Position:
    case ParticleChannel::PreviousPosition:
    case ParticleChannel::Velocity:
    case ParticleChannel::Scale:
    case ParticleChannel::PreviousScale:
        return static_cast<uint32>(sizeof(Mathematics::Vector3));
    case ParticleChannel::Color:
    case ParticleChannel::PreviousColor:
        return static_cast<uint32>(sizeof(Mathematics::Vector4));
    case ParticleChannel::FiredEvents:
    case ParticleChannel::Contacts:
    case ParticleChannel::Distance:
    case ParticleChannel::PreviousDistance:
        return 8;
    default:
        return 4;
    }
}

void ParticleChannels::Configure(ParticleChannelMask mask, uint32 capacity)
{
    m_Mask = mask | kCoreChannels;
    m_Capacity = capacity;
    m_Count = 0;
    for (uint32 channel = 0; channel < kParticleChannelCount; ++channel)
    {
        auto& buffer = m_Buffers[channel];
        if ((m_Mask & (ParticleChannelMask{1} << channel)) == 0)
        {
            buffer.clear();
            buffer.shrink_to_fit();
            continue;
        }
        buffer.assign(static_cast<size_t>(capacity) * ChannelElementSize(static_cast<ParticleChannel>(channel)),
                      std::byte{0});
    }
}

uint32 ParticleChannels::Append(uint32 count)
{
    assert(m_Count + count <= m_Capacity);
    const uint32 first = m_Count;
    for (uint32 channel = 0; channel < kParticleChannelCount; ++channel)
    {
        if ((m_Mask & (ParticleChannelMask{1} << channel)) == 0)
            continue;
        const size_t element = ChannelElementSize(static_cast<ParticleChannel>(channel));
        std::memset(m_Buffers[channel].data() + first * element, 0, count * element);
    }
    m_Count += count;
    return first;
}

void ParticleChannels::RemoveSwapBack(uint32 index)
{
    assert(index < m_Count);
    const uint32 last = m_Count - 1;
    if (index != last)
    {
        for (uint32 channel = 0; channel < kParticleChannelCount; ++channel)
        {
            if ((m_Mask & (ParticleChannelMask{1} << channel)) == 0)
                continue;
            const size_t element = ChannelElementSize(static_cast<ParticleChannel>(channel));
            auto* data = m_Buffers[channel].data();
            std::memcpy(data + index * element, data + last * element, element);
        }
    }
    m_Count = last;
}

void ParticleChannels::Truncate(uint32 count)
{
    if (count < m_Count)
        m_Count = count;
}

void ParticleChannels::CopyChannel(ParticleChannel destination, ParticleChannel source, uint32 first, uint32 count)
{
    assert(Has(destination) && Has(source));
    assert(ChannelElementSize(destination) == ChannelElementSize(source));
    const size_t element = ChannelElementSize(source);
    std::memcpy(m_Buffers[static_cast<uint32>(destination)].data() + first * element,
                m_Buffers[static_cast<uint32>(source)].data() + first * element, count * element);
}

void ParticleChannels::CopyChannel(ParticleChannel destination, ParticleChannel source,
                                   std::span<const uint32> particles)
{
    assert(Has(destination) && Has(source));
    assert(ChannelElementSize(destination) == ChannelElementSize(source));
    const size_t element = ChannelElementSize(source);
    auto* to = m_Buffers[static_cast<uint32>(destination)].data();
    const auto* from = m_Buffers[static_cast<uint32>(source)].data();
    for (const uint32 index : particles)
        std::memcpy(to + index * element, from + index * element, element);
}

} // namespace GameEngine::Particles
