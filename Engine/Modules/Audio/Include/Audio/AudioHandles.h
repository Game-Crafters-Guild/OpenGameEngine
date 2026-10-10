#pragma once

#include <cstdint>
#include <type_traits>

namespace GameEngine::Audio
{
namespace Detail
{
    using HandleType = std::uint64_t;
    static constexpr HandleType kInvalidHandle = 0;
    static constexpr std::uint8_t kGenerationBits = 8;
    static constexpr HandleType kGenerationMask = (1ULL << kGenerationBits) - 1;
    static constexpr std::uint8_t kIndexBits = 64 - kGenerationBits;
    static constexpr HandleType kIndexMask = (1ULL << kIndexBits) - 1;
}

struct Handle
{
    Detail::HandleType id = Detail::kInvalidHandle;

    constexpr Handle() = default;
    explicit constexpr Handle(Detail::HandleType raw) : id(raw) {}
    constexpr Handle(std::uint64_t index, std::uint8_t generation)
        : id((static_cast<Detail::HandleType>(generation) << Detail::kIndexBits) | (index & Detail::kIndexMask))
    {
    }

    constexpr std::uint64_t Index() const { return id & Detail::kIndexMask; }
    constexpr std::uint8_t Generation() const
    {
        return static_cast<std::uint8_t>((id >> Detail::kIndexBits) & Detail::kGenerationMask);
    }
    constexpr bool IsValid() const { return id != Detail::kInvalidHandle; }

    constexpr operator Detail::HandleType() const { return id; }
    constexpr bool operator==(const Handle& other) const { return id == other.id; }
    constexpr bool operator!=(const Handle& other) const { return id != other.id; }
    constexpr bool operator<(const Handle& other) const { return id < other.id; }
};

template <typename Tag>
struct StrongHandle
{
    Detail::HandleType id = Detail::kInvalidHandle;

    constexpr StrongHandle() = default;
    explicit constexpr StrongHandle(Detail::HandleType raw) : id(raw) {}
    constexpr StrongHandle(std::uint64_t index, std::uint8_t generation)
        : id((static_cast<Detail::HandleType>(generation) << Detail::kIndexBits) | (index & Detail::kIndexMask))
    {
    }

    constexpr std::uint64_t Index() const { return id & Detail::kIndexMask; }
    constexpr std::uint8_t Generation() const
    {
        return static_cast<std::uint8_t>((id >> Detail::kIndexBits) & Detail::kGenerationMask);
    }
    constexpr bool IsValid() const { return id != Detail::kInvalidHandle; }

    constexpr StrongHandle(Handle h) : id(static_cast<Detail::HandleType>(h)) {}
    constexpr operator Handle() const { return Handle(id); }
    constexpr operator Detail::HandleType() const { return id; }

    constexpr bool operator==(const StrongHandle& other) const { return id == other.id; }
    constexpr bool operator!=(const StrongHandle& other) const { return id != other.id; }
    constexpr bool operator<(const StrongHandle& other) const { return id < other.id; }
};

struct AudioEmitterTag
{
};
struct AudioClipTag
{
};

using AudioEmitterHandle = StrongHandle<AudioEmitterTag>;
using AudioClipHandle = StrongHandle<AudioClipTag>;

static_assert(sizeof(AudioEmitterHandle) == sizeof(Handle), "AudioEmitterHandle must be zero-overhead");
static_assert(std::is_trivially_copyable_v<AudioEmitterHandle>, "AudioEmitterHandle must be trivially copyable");
static_assert(std::is_standard_layout_v<AudioEmitterHandle>, "AudioEmitterHandle must be standard layout");

static_assert(sizeof(AudioClipHandle) == sizeof(Handle), "AudioClipHandle must be zero-overhead");
static_assert(std::is_trivially_copyable_v<AudioClipHandle>, "AudioClipHandle must be trivially copyable");
static_assert(std::is_standard_layout_v<AudioClipHandle>, "AudioClipHandle must be standard layout");

static constexpr AudioEmitterHandle INVALID_AUDIO_EMITTER_HANDLE{};
static constexpr AudioClipHandle INVALID_AUDIO_CLIP_HANDLE{};

} // namespace GameEngine::Audio



