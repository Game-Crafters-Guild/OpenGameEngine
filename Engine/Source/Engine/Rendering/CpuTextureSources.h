#pragma once

#include "Engine/Rendering/CpuTextureSource.h"
#include <mutex>
#include <vector>

namespace GameEngine::Engine::Renderer
{
struct CpuTextureRevision;

// Private TextureService storage. No independent singleton or renderer feature.
class CpuTextureSources
{
  public:
    ~CpuTextureSources();
    void Open();
    CpuTextureSource Create(const CpuTextureSourceDesc& desc);
    void ObserveWorld(CpuTextureScope scope);
    void Flush(Rendering::IDevice& device);
    CpuTextureBinding Lookup(uint64_t worldId, StringId name) const;
    void Reprovision();
    void Shutdown(Rendering::IDevice* device);

  private:
    struct Entry
    {
        CpuTextureSourceDesc Desc{};
        std::weak_ptr<CpuTextureSourceState> Source;
        std::shared_ptr<const CpuTextureRevision> Uploaded;
        Rendering::TextureHandle Texture{};
        // A newly created lease is unusable until the world-owning render loop
        // observes its exact reset generation. This also gates stale creation.
        bool Observed = false;
    };
    mutable std::mutex m_Mutex;
    std::vector<Entry> m_Entries;
    bool m_Open = false;
};
} // namespace GameEngine::Engine::Renderer
