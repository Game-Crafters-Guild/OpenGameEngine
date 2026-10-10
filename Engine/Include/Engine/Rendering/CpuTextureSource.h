#pragma once

#include "Rendering/Core/Device.h"
#include "Types/Types.h"
#include <array>
#include <cstddef>
#include <memory>
#include <span>

namespace GameEngine::Engine::Renderer
{
struct CpuTextureScope
{
    uint64_t WorldId = 0;
    uint64_t ResetGeneration = 0;
    bool operator==(const CpuTextureScope&) const = default;
};

struct CpuTextureSourceDesc
{
    CpuTextureScope Scope{};
    StringId Name = 0;
    Rendering::TextureFormat Format = Rendering::TextureFormat::R8G8_UNORM;
    // Fixed opaque metadata accompanying each image. Zero, or a multiple of 16
    // up to 256 bytes. The producer and its shader own the layout.
    uint32_t ParameterBytes = 0;
};

class CpuTextureSources;
struct CpuTextureSourceState;

// A single publisher's CPU-only lease. Engine code owns every retained byte and
// control block; no producer callback or module-owned deleter is retained.
// Calls on this object (including move/reset) require caller synchronization.
class CpuTextureSource
{
  public:
    CpuTextureSource();
    ~CpuTextureSource();
    CpuTextureSource(CpuTextureSource&&) noexcept;
    CpuTextureSource& operator=(CpuTextureSource&&) noexcept;
    CpuTextureSource(const CpuTextureSource&) = delete;
    CpuTextureSource& operator=(const CpuTextureSource&) = delete;

    explicit operator bool() const;
    void Reset();

    // Copies one complete image+metadata revision. Latest publication wins;
    // storage retains at most one pending and one uploaded revision. Images are
    // tightly packed R8/R8G8/RGBA8_UNORM, <=4096 per axis and <=16MiB. Invalid
    // data/allocation failure/closed lease returns false and preserves last-good.
    // Success accepts CPU data, not GPU readiness. A render-thread drain submits
    // it later; multiple views then share that revision without more uploads.
    [[nodiscard]] bool Publish(uint32_t width, uint32_t height,
                               std::span<const std::byte> pixels,
                               std::span<const std::byte> parameters = {});

  private:
    friend class CpuTextureSources;
    explicit CpuTextureSource(std::shared_ptr<CpuTextureSourceState> state);
    std::shared_ptr<CpuTextureSourceState> m_State;
};

// Render-thread snapshot, copied from one successfully uploaded revision.
// The texture is borrowed until the next upload drain; never cache it across
// frames. A missing binding must not publish either half of the pair.
struct CpuTextureBinding
{
    Rendering::TextureHandle Texture{};
    Rendering::TextureFormat Format = Rendering::TextureFormat::Unknown;
    uint32_t Width = 0, Height = 0, ParameterBytes = 0;
    std::array<std::byte, 256> Parameters{};
};
} // namespace GameEngine::Engine::Renderer
