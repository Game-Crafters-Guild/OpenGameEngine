#pragma once

#include "Mathematics/Vector3.h"
#include "Mathematics/Vector4.h"
#include "Types/Types.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace GameEngine::Particles
{

/// One per-particle attribute array of the simulation. The runtime stores each channel as its
/// own contiguous array (structure of arrays) and allocates only the channels the loaded stack
/// declares, so a processor that reads positions and writes velocities touches two arrays.
enum class ParticleChannel : uint8
{
    SpawnIndex,
    Phase,
    Age,
    PreviousAge,
    PhaseAge,
    Lifetime,
    Position,
    PreviousPosition,
    Velocity,
    Size,
    PreviousSize,
    Scale,
    PreviousScale,
    Color,
    PreviousColor,
    Rotation,
    PreviousRotation,
    Spin,
    AnimationSpeed,
    AnimationOffset,
    Custom0,
    Custom1,
    Custom2,
    Custom3,
    LightIntensity,
    LightRange,
    FiredEvents,
    PhaseEntries,
    Contacts,
    Settled,
    Distance,
    PreviousDistance,
    TrailSlot,
    BirthPosition,
    BirthVelocity,
    BirthSize,
    BirthScale,
    BirthColor,
    BirthRotation,
    BirthSpin,
    BirthLifetime,
    BirthAnimationSpeed,
    BirthAnimationOffset,
    BirthCustom0,
    BirthCustom1,
    BirthCustom2,
    BirthCustom3,
    EntryPosition,
    EntryVelocity,
    EntrySize,
    EntryScale,
    EntryColor,
    EntryRotation,
    EntrySpin,
    EntryLifetime,
    EntryAnimationSpeed,
    EntryAnimationOffset,
    EntryCustom0,
    EntryCustom1,
    EntryCustom2,
    EntryCustom3,
    Count
};

inline constexpr uint32 kParticleChannelCount = static_cast<uint32>(ParticleChannel::Count);
static_assert(kParticleChannelCount <= 64, "ParticleChannelMask holds one bit per channel");

/// A set of channels, one bit per ParticleChannel.
using ParticleChannelMask = uint64;

constexpr ParticleChannelMask ChannelBit(ParticleChannel channel)
{
    return ParticleChannelMask{1} << static_cast<uint32>(channel);
}

template <typename... Channels>
constexpr ParticleChannelMask ChannelBits(Channels... channels)
{
    return (ChannelBit(channels) | ... | ParticleChannelMask{0});
}

/// The attributes a Property processor can target. Each has a current channel and a birth and
/// phase-entry snapshot channel the processor may read as its basis. Alpha is the fourth component
/// of Color and shares its channels.
enum class ParticleAttribute : uint8
{
    Position,
    Velocity,
    Size,
    Scale,
    Color,
    Alpha,
    Rotation,
    Spin,
    Lifetime,
    AnimationSpeed,
    AnimationOffset,
    Custom0,
    Custom1,
    Custom2,
    Custom3,
    Count
};

inline constexpr uint32 kParticleAttributeCount = static_cast<uint32>(ParticleAttribute::Count);

/// Which value of an attribute a Property processor combines its result with.
enum class ParticleBasis : uint8
{
    Current,
    Birth,
    Entry
};

/// The channel holding `attribute` for `basis`.
ParticleChannel AttributeChannel(ParticleAttribute attribute, ParticleBasis basis);

/// Number of float components a Property processor writes for an attribute (1, 3 or 4).
uint32 AttributeComponents(ParticleAttribute attribute);

/// Byte size of one element of a channel.
uint32 ChannelElementSize(ParticleChannel channel);

/// The channel's name as the enumerator reads, for diagnostics.
std::string_view ParticleChannelName(ParticleChannel channel);

/// Every channel the runtime always allocates: identity, phase, clocks, the transform and the
/// appearance the renderer reads, and their previous-tick copies for interpolation.
inline constexpr ParticleChannelMask kCoreChannels = ChannelBits(
    ParticleChannel::SpawnIndex, ParticleChannel::Phase, ParticleChannel::Age, ParticleChannel::PreviousAge,
    ParticleChannel::PhaseAge, ParticleChannel::Lifetime, ParticleChannel::Position,
    ParticleChannel::PreviousPosition, ParticleChannel::Velocity, ParticleChannel::Size,
    ParticleChannel::PreviousSize, ParticleChannel::Scale, ParticleChannel::PreviousScale, ParticleChannel::Color,
    ParticleChannel::PreviousColor, ParticleChannel::Rotation, ParticleChannel::PreviousRotation,
    ParticleChannel::Spin, ParticleChannel::AnimationSpeed, ParticleChannel::AnimationOffset,
    ParticleChannel::PhaseEntries, ParticleChannel::FiredEvents);

/// Per-particle attribute arrays, one contiguous array per allocated channel. Storage is sized to
/// the emitter's capacity when the stack is bound and never grows during simulation.
class ParticleChannels
{
  public:
    /// Allocates `mask` (plus the core channels) for `capacity` particles and clears the particles.
    void Configure(ParticleChannelMask mask, uint32 capacity);

    ParticleChannelMask Mask() const { return m_Mask; }
    uint32 Capacity() const { return m_Capacity; }
    uint32 Count() const { return m_Count; }
    bool Has(ParticleChannel channel) const { return (m_Mask & ChannelBit(channel)) != 0; }

    /// Appends `count` particles with every channel zeroed; returns the first new index.
    uint32 Append(uint32 count);
    /// Moves the last particle into `index` and shrinks the count by one.
    void RemoveSwapBack(uint32 index);
    /// Shrinks the particle count, keeping the first `count` particles.
    void Truncate(uint32 count);
    void Clear() { m_Count = 0; }

    /// Copies `count` elements of `source` into `destination` starting at `first`.
    void CopyChannel(ParticleChannel destination, ParticleChannel source, uint32 first, uint32 count);
    /// Copies the elements of `source` at `particles` into `destination`.
    void CopyChannel(ParticleChannel destination, ParticleChannel source, std::span<const uint32> particles);

    /// Names the processor whose code reads the channels next, so a read of a channel no processor
    /// declared says which one did; empty while the runtime itself reads.
    void SetReader(std::string_view processorId) { m_Reader = processorId; }

    /// A channel's storage. Debug builds assert, naming the reader and the channel, when the set holds
    /// particles but no processor of the stack declared the channel: its storage is empty.
    template <typename T>
    std::span<T> Get(ParticleChannel channel)
    {
        CheckDeclared(channel, sizeof(T));
        auto& buffer = m_Buffers[static_cast<uint32>(channel)];
        return {reinterpret_cast<T*>(buffer.data()), m_Capacity};
    }

    template <typename T>
    std::span<const T> Get(ParticleChannel channel) const
    {
        CheckDeclared(channel, sizeof(T));
        const auto& buffer = m_Buffers[static_cast<uint32>(channel)];
        return {reinterpret_cast<const T*>(buffer.data()), m_Capacity};
    }

    std::span<uint32> SpawnIndices() { return Get<uint32>(ParticleChannel::SpawnIndex); }
    std::span<const uint32> SpawnIndices() const { return Get<uint32>(ParticleChannel::SpawnIndex); }
    std::span<uint32> Phases() { return Get<uint32>(ParticleChannel::Phase); }
    std::span<const uint32> Phases() const { return Get<uint32>(ParticleChannel::Phase); }
    std::span<float> Ages() { return Get<float>(ParticleChannel::Age); }
    std::span<const float> Ages() const { return Get<float>(ParticleChannel::Age); }
    std::span<float> PhaseAges() { return Get<float>(ParticleChannel::PhaseAge); }
    std::span<const float> PhaseAges() const { return Get<float>(ParticleChannel::PhaseAge); }
    std::span<float> Lifetimes() { return Get<float>(ParticleChannel::Lifetime); }
    std::span<const float> Lifetimes() const { return Get<float>(ParticleChannel::Lifetime); }
    std::span<Mathematics::Vector3> Positions() { return Get<Mathematics::Vector3>(ParticleChannel::Position); }
    std::span<const Mathematics::Vector3> Positions() const { return Get<Mathematics::Vector3>(ParticleChannel::Position); }
    std::span<Mathematics::Vector3> Velocities() { return Get<Mathematics::Vector3>(ParticleChannel::Velocity); }
    std::span<const Mathematics::Vector3> Velocities() const { return Get<Mathematics::Vector3>(ParticleChannel::Velocity); }
    std::span<float> Sizes() { return Get<float>(ParticleChannel::Size); }
    std::span<const float> Sizes() const { return Get<float>(ParticleChannel::Size); }
    std::span<Mathematics::Vector3> Scales() { return Get<Mathematics::Vector3>(ParticleChannel::Scale); }
    std::span<const Mathematics::Vector3> Scales() const { return Get<Mathematics::Vector3>(ParticleChannel::Scale); }
    std::span<Mathematics::Vector4> Colors() { return Get<Mathematics::Vector4>(ParticleChannel::Color); }
    std::span<const Mathematics::Vector4> Colors() const { return Get<Mathematics::Vector4>(ParticleChannel::Color); }
    std::span<float> Rotations() { return Get<float>(ParticleChannel::Rotation); }
    std::span<const float> Rotations() const { return Get<float>(ParticleChannel::Rotation); }
    std::span<float> Spins() { return Get<float>(ParticleChannel::Spin); }
    std::span<const float> Spins() const { return Get<float>(ParticleChannel::Spin); }

  private:
    void CheckDeclared([[maybe_unused]] ParticleChannel channel, [[maybe_unused]] size_t elementSize) const
    {
#ifndef NDEBUG
        if (m_Capacity > 0 && !Has(channel))
            ReportUndeclared(channel);
        if (elementSize != ChannelElementSize(channel))
            ReportWrongType(channel, elementSize);
#endif
    }
    void ReportUndeclared(ParticleChannel channel) const;
    void ReportWrongType(ParticleChannel channel, size_t elementSize) const;

    std::string_view m_Reader;
    std::array<std::vector<std::byte>, kParticleChannelCount> m_Buffers;
    ParticleChannelMask m_Mask = 0;
    uint32 m_Capacity = 0;
    uint32 m_Count = 0;
};

} // namespace GameEngine::Particles
