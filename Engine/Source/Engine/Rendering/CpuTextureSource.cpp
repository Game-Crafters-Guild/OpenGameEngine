#include "CpuTextureSources.h"
#include "Rendering/Utils/TextureUploadHelpers.h"
#include <algorithm>
#include <cstring>
#include <new>

namespace GameEngine::Engine::Renderer
{
using namespace Rendering;

struct CpuTextureRevision
{
    uint32_t Width = 0, Height = 0;
    std::vector<std::byte> Pixels;
    std::array<std::byte, 256> Parameters{};
};

struct CpuTextureSourceState
{
    explicit CpuTextureSourceState(CpuTextureSourceDesc desc) : Desc(desc) {}
    // Fixed for the lease's life; readable without the mutex.
    const CpuTextureSourceDesc Desc;
    std::mutex Mutex;
    bool Closed = false;
    std::shared_ptr<const CpuTextureRevision> Pending;
};

namespace
{
constexpr uint32_t kMaxImageAxis = 4096;
constexpr size_t kMaxRevisionBytes = size_t(16) << 20;
constexpr uint32_t kMaxParameterBytes = 256;
constexpr uint32_t kParameterAlignment = 16;

// Tightly packed 8-bit linear formats are the only publishable ones; zero for anything else.
uint32_t PixelBytes(TextureFormat format)
{
    switch (format)
    {
    case TextureFormat::R8_UNORM:
    case TextureFormat::R8G8_UNORM:
    case TextureFormat::RGBA8_UNORM:
        return BytesPerPixel(format);
    default:
        return 0;
    }
}
void Close(const std::weak_ptr<CpuTextureSourceState>& weak)
{
    if (auto source = weak.lock())
    {
        std::lock_guard lock(source->Mutex);
        source->Closed = true;
        source->Pending.reset();
    }
}
} // namespace

CpuTextureSource::CpuTextureSource() = default;
CpuTextureSource::~CpuTextureSource() = default;
CpuTextureSource::CpuTextureSource(CpuTextureSource&&) noexcept = default;
CpuTextureSource& CpuTextureSource::operator=(CpuTextureSource&&) noexcept = default;
CpuTextureSource::CpuTextureSource(std::shared_ptr<CpuTextureSourceState> state)
    : m_State(std::move(state)) {}
void CpuTextureSource::Reset()
{
    m_State.reset();
}
CpuTextureSource::operator bool() const
{
    if (!m_State)
        return false;
    std::lock_guard lock(m_State->Mutex);
    return !m_State->Closed;
}

bool CpuTextureSource::Publish(uint32_t width, uint32_t height,
                               std::span<const std::byte> pixels,
                               std::span<const std::byte> parameters)
{
    if (!m_State || !width || !height || width > kMaxImageAxis || height > kMaxImageAxis)
        return false;
    const size_t bytes = size_t(width) * height * PixelBytes(m_State->Desc.Format);
    if (!bytes || bytes > kMaxRevisionBytes || pixels.size() != bytes ||
        parameters.size() != m_State->Desc.ParameterBytes)
        return false;
    // Copied outside the mutex: the render thread's drain and lookups take it and
    // must never wait behind a publisher's copy.
    std::shared_ptr<CpuTextureRevision> revision;
    try
    {
        revision = std::make_shared<CpuTextureRevision>();
        revision->Width = width;
        revision->Height = height;
        revision->Pixels.assign(pixels.begin(), pixels.end());
        std::copy(parameters.begin(), parameters.end(), revision->Parameters.begin());
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }
    std::lock_guard lock(m_State->Mutex);
    if (m_State->Closed)
        return false;
    m_State->Pending = std::move(revision);
    return true;
}

CpuTextureSources::~CpuTextureSources()
{
    Shutdown(nullptr);
}
void CpuTextureSources::Open()
{
    std::lock_guard lock(m_Mutex);
    m_Open = true;
}
CpuTextureSource CpuTextureSources::Create(const CpuTextureSourceDesc& desc)
{
    if (!desc.Scope.WorldId || !desc.Name || !PixelBytes(desc.Format) ||
        desc.ParameterBytes > kMaxParameterBytes || desc.ParameterBytes % kParameterAlignment)
        return {};
    std::lock_guard lock(m_Mutex);
    if (!m_Open)
        return {};
    for (const auto& entry : m_Entries)
        if (entry.Desc.Scope == desc.Scope && entry.Desc.Name == desc.Name)
            if (auto source = entry.Source.lock())
            {
                std::lock_guard stateLock(source->Mutex);
                if (!source->Closed)
                    return {};
            }
    auto state = std::make_shared<CpuTextureSourceState>(desc);
    m_Entries.push_back({desc, state});
    return CpuTextureSource(std::move(state));
}
void CpuTextureSources::ObserveWorld(CpuTextureScope scope)
{
    std::lock_guard lock(m_Mutex);
    for (auto& entry : m_Entries)
        if (entry.Desc.Scope.WorldId == scope.WorldId)
        {
            entry.Observed = entry.Desc.Scope == scope;
            if (!entry.Observed)
                Close(entry.Source);
        }
}

void CpuTextureSources::Flush(IDevice& device)
{
    std::lock_guard lock(m_Mutex);
    for (auto it = m_Entries.begin(); it != m_Entries.end();)
    {
        auto source = it->Source.lock();
        std::shared_ptr<const CpuTextureRevision> pending;
        bool closed = !source;
        if (source)
        {
            std::lock_guard stateLock(source->Mutex);
            closed = source->Closed;
            pending = source->Pending;
        }
        if (closed)
        {
            // No future lookup can bind this texture. IDevice destruction is
            // fence-deferred, so already submitted frames retain it safely.
            if (it->Texture.IsValid())
                device.DestroyTexture(it->Texture);
            it = m_Entries.erase(it);
            continue;
        }
        if (it->Observed && pending && (pending != it->Uploaded || !it->Texture.IsValid()))
        {
            const bool resize = !it->Texture.IsValid() || !it->Uploaded ||
                                pending->Width != it->Uploaded->Width || pending->Height != it->Uploaded->Height;
            TextureHandle target = it->Texture;
            if (resize)
            {
                TextureDesc desc{};
                desc.width = pending->Width;
                desc.height = pending->Height;
                desc.format = static_cast<uint32_t>(it->Desc.Format);
                desc.usage = static_cast<uint32_t>(TextureUsage::ShaderResource | TextureUsage::TransferDst |
                                                   TextureUsage::TransferSrc);
                desc.persistent = true;
                desc.debugName = "CpuTextureSource";
                target = device.CreateTexture(desc);
            }
            TextureUploadEntry upload{target, pending->Pixels.data(), pending->Width, pending->Height,
                                      1, size_t(pending->Width) * PixelBytes(it->Desc.Format), 0,
                                      resize ? ResourceState::Undefined : ResourceState::ShaderResource};
            bool submitted = false;
            try
            {
                submitted = target.IsValid() && UploadTexturesBatched(&device, &upload, 1, "CpuTextureUpload");
            }
            catch (...)
            {
                // Preparation failures return false inside the helper. A
                // propagated dispatch failure may already have changed pixels;
                // never leave that in-place image paired with old metadata.
                if (!resize)
                    it->Texture = {};
                if (target.IsValid())
                    device.DestroyTexture(target);
                throw;
            }
            if (submitted)
            {
                if (resize && it->Texture.IsValid())
                    device.DestroyTexture(it->Texture);
                it->Texture = target;
                it->Uploaded = std::move(pending);
            }
            else if (resize && target.IsValid())
                device.DestroyTexture(target);
        }
        ++it;
    }
}

CpuTextureBinding CpuTextureSources::Lookup(uint64_t worldId, StringId name) const
{
    std::lock_guard lock(m_Mutex);
    for (const auto& entry : m_Entries)
    {
        if (!entry.Observed || entry.Desc.Scope.WorldId != worldId || entry.Desc.Name != name ||
            !entry.Texture.IsValid() || !entry.Uploaded)
            continue;
        auto source = entry.Source.lock();
        if (!source)
            continue;
        std::lock_guard stateLock(source->Mutex);
        if (source->Closed)
            continue;
        return {entry.Texture, entry.Desc.Format, entry.Uploaded->Width, entry.Uploaded->Height,
                entry.Desc.ParameterBytes, entry.Uploaded->Parameters};
    }
    return {};
}
void CpuTextureSources::Reprovision()
{
    std::lock_guard lock(m_Mutex);
    // The rebuilt device already freed these handles; do not destroy reissued
    // numeric IDs. Retained CPU revisions will upload at the next drain.
    for (auto& entry : m_Entries)
        entry.Texture = {};
}
void CpuTextureSources::Shutdown(IDevice* device)
{
    std::lock_guard lock(m_Mutex);
    m_Open = false;
    for (auto& entry : m_Entries)
    {
        Close(entry.Source);
        if (device && entry.Texture.IsValid())
            device->DestroyTexture(entry.Texture);
    }
    m_Entries.clear();
}
} // namespace GameEngine::Engine::Renderer
