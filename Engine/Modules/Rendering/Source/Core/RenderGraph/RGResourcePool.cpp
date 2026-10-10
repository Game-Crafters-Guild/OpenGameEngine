#include "Rendering/Core/RenderGraph/RGResourcePool.h"

#include "Logger/Logger.h"
#include "Rendering/Core/RenderGraph/RGRealize.h"

#include <algorithm>
#include <cassert>

namespace GameEngine::Rendering::RenderGraph
{

RGResourcePool::~RGResourcePool()
{
    for (auto& [name, e] : m_Tex)
        ReleaseTextureAndViews(e.Handle, e.MipViews);
    for (const auto& [name, e] : m_Buf)
        m_Device->DestroyBuffer(e.Handle);
    for (auto& d : m_DeferredTex)
        ReleaseTextureAndViews(d.Handle, d.MipViews);
    for (const auto& d : m_DeferredBuf)
        m_Device->DestroyBuffer(d.Handle);
}

void RGResourcePool::ReleaseTextureAndViews(TextureHandle handle,
                                            std::vector<TextureViewHandle>& views)
{
    for (TextureViewHandle v : views)
        if (v.IsValid())
            m_Device->DestroyTextureView(v);
    views.clear();
    m_Device->DestroyTexture(handle);
}

TextureViewHandle RGResourcePool::GetOrCreateMipView(TextureHandle texture, uint32_t mip)
{
    if (!texture.IsValid())
        return {};
    const auto keyed = m_TexNameByHandle.find(texture);
    if (keyed == m_TexNameByHandle.end())
        return {};
    const auto it = m_Tex.find(keyed->second);
    if (it == m_Tex.end())
        return {};
    const std::string& name = it->first;
    TexEntry& e = it->second;
    // Only the texture this pool currently holds can have pool-owned views.
    if (e.Handle != texture)
        return {};
    if (mip >= e.Desc.mipLevels)
        return {};
    if (e.MipViews.size() <= mip)
        e.MipViews.resize(mip + 1u);
    if (!e.MipViews[mip].IsValid())
    {
        const std::string debugName = name + ".Mip" + std::to_string(mip);
        // An array image's mip view spans the WHOLE level: a compute pass
        // writing one level of an array pyramid selects its slice through the
        // image2DArray coordinate, so a layer-0 2D view would drop every store
        // past the first slice. NeedsArrayView is the same rule the backend
        // applies to the default view, ForceArrayView included — a single-layer
        // image declared array-shaped must not get a 2D mip view.
        const bool isArray = NeedsArrayView(e.Desc);
        TextureViewDesc vd{};
        vd.viewType = isArray ? TextureViewType::View2DArray : TextureViewType::View2D;
        // A Color view of a depth image is a format mismatch the driver
        // reports as a bad descriptor, not as a wrong picture.
        vd.aspect = IsDepthFormat(static_cast<TextureFormat>(e.Desc.format))
                        ? TextureAspect::Depth
                        : TextureAspect::Color;
        vd.baseMip = mip;
        vd.levelCount = 1u;
        vd.baseLayer = 0u;
        vd.layerCount = isArray ? e.Desc.arrayLayers : 1u;
        vd.debugName = debugName.c_str();
        e.MipViews[mip] = m_Device->CreateTextureView(e.Handle, vd);
    }
    return e.MipViews[mip];
}

TextureHandle RGResourcePool::GetOrCreateTexture(const std::string& name, const TextureDesc& desc,
                                                 uint64_t frameIndex, bool* outNeedsFreshInit)
{
    auto it = m_Tex.find(name);
    if (it != m_Tex.end())
    {
        TexEntry& e = it->second;
        // The importer restates its own desc every frame; the usage this entry
        // has learned it needs beyond that is folded in BEFORE the identity
        // check, so a widened entry matches instead of realloc-thrashing.
        TextureDesc wanted = desc;
        wanted.usage |= e.RequiredUsage;
        if (DescEqual(e.Desc, wanted))
        {
            e.LastUsedFrame = frameIndex;
            // Still-armed entries re-report until an EXECUTED frame clears the
            // arm — a creation frame abandoned before Execute must not silently
            // discharge it.
            if (outNeedsFreshInit)
                *outNeedsFreshInit = e.NeedsFreshInit;
            return e.Handle;
        }
        // Old may be in flight — its views go with it, not before it.
        m_DeferredTex.push_back({e.Handle, frameIndex, std::move(e.MipViews)});
        e.MipViews.clear();
        m_TexNameByHandle.erase(e.Handle);
        e.Handle = m_Device->CreateTexture(wanted);
        m_TexNameByHandle[e.Handle] = name;
        e.Desc = wanted;
        e.Desc.debugName = nullptr; // callers pass stack strings; never store the pointer
        e.Bytes = EstimateBytes(wanted);
        e.LastUsedFrame = frameIndex;
        e.State = ResourceState::Undefined; // fresh allocation
        e.NeedsFreshInit = true;
        if (outNeedsFreshInit)
            *outNeedsFreshInit = true;
        return e.Handle;
    }
    TexEntry e{};
    e.Handle = m_Device->CreateTexture(desc);
    e.Desc = desc;
    e.Desc.debugName = nullptr; // callers pass stack strings; never store the pointer
    e.Bytes = EstimateBytes(desc);
    e.LastUsedFrame = frameIndex;
    e.State = ResourceState::Undefined;
    e.NeedsFreshInit = true;
    const TextureHandle h = e.Handle;
    m_Tex.emplace(name, e);
    m_TexNameByHandle[h] = name;
    if (outNeedsFreshInit)
        *outNeedsFreshInit = true;
    return h;
}

BufferHandle RGResourcePool::GetOrCreateBuffer(const std::string& name, const BufferDesc& desc,
                                               uint64_t frameIndex, bool* outNeedsZeroInit)
{
    auto it = m_Buf.find(name);
    if (it != m_Buf.end())
    {
        BufEntry& e = it->second;
        if (DescEqual(e.Desc, desc))
        {
            e.LastUsedFrame = frameIndex;
            // Still-armed entries re-report until an EXECUTED frame clears the
            // arm (MarkBufferZeroFilled) — a creation frame abandoned before
            // Execute must not silently discharge the zero-init.
            if (outNeedsZeroInit)
                *outNeedsZeroInit = e.NeedsZeroInit;
            return e.Handle;
        }
        m_DeferredBuf.push_back({e.Handle, frameIndex});
        e.Handle = m_Device->CreateBuffer(desc);
        e.Desc = desc;
        e.Desc.debugName = nullptr; // callers pass stack strings; never store the pointer
        e.Bytes = EstimateBytes(desc);
        e.LastUsedFrame = frameIndex;
        e.State = ResourceState::Undefined;
        e.NeedsZeroInit = true;
        if (outNeedsZeroInit)
            *outNeedsZeroInit = true;
        return e.Handle;
    }
    BufEntry e{};
    e.Handle = m_Device->CreateBuffer(desc);
    e.Desc = desc;
    e.Desc.debugName = nullptr; // callers pass stack strings; never store the pointer
    e.Bytes = EstimateBytes(desc);
    e.LastUsedFrame = frameIndex;
    e.State = ResourceState::Undefined;
    e.NeedsZeroInit = true;
    const BufferHandle h = e.Handle;
    m_Buf.emplace(name, e);
    if (outNeedsZeroInit)
        *outNeedsZeroInit = true;
    return h;
}

ResourceState RGResourcePool::GetState(const std::string& name) const
{
    auto t = m_Tex.find(name);
    if (t != m_Tex.end())
        return t->second.State;
    auto b = m_Buf.find(name);
    if (b != m_Buf.end())
        return b->second.State;
    return ResourceState::Undefined;
}

void RGResourcePool::SetState(const std::string& name, ResourceState state)
{
    auto t = m_Tex.find(name);
    if (t != m_Tex.end())
    {
        t->second.State = state;
        return;
    }
    auto b = m_Buf.find(name);
    if (b != m_Buf.end())
    {
        b->second.State = state;
        return;
    }
    // A silent no-op here would quietly desynchronize cross-frame barrier state.
    Logger::Log::Warning("[RenderGraph] RGResourcePool::SetState on unknown resource '{}' (typo or evicted?)",
                         name);
}

void RGResourcePool::RequireTextureUsage(const std::string& name, TextureUsage usage)
{
    // Transfer bits only: everything else is the importer's own declaration,
    // and widening a pooled desc's identity from outside it would be drift in
    // the other direction.
    assert((static_cast<uint32_t>(usage) & ~(static_cast<uint32_t>(TextureUsage::TransferSrc) |
                                             static_cast<uint32_t>(TextureUsage::TransferDst))) == 0 &&
           "RequireTextureUsage takes TransferSrc/TransferDst only");
    auto it = m_Tex.find(name);
    if (it == m_Tex.end())
    {
        // Dropping the requirement silently would leave the next copy of this
        // texture undeclared on the backend, which is exactly the drift the
        // transfer-usage audit exists to catch.
        Logger::Log::Warning(
            "[RenderGraph] RGResourcePool::RequireTextureUsage on unknown resource '{}' (typo or evicted?)",
            name);
        return;
    }
    it->second.RequiredUsage |= static_cast<uint32_t>(usage);
}

uint64_t RGResourcePool::BytesInPool() const
{
    uint64_t total = 0;
    for (const auto& [name, e] : m_Tex)
        total += e.Bytes;
    for (const auto& [name, e] : m_Buf)
        total += e.Bytes;
    return total;
}

void RGResourcePool::TickPoolElements(uint64_t frameIndex, uint64_t maxIdleFrames, uint64_t framesInFlight)
{
    for (auto it = m_Tex.begin(); it != m_Tex.end();)
    {
        if ((frameIndex - it->second.LastUsedFrame) > maxIdleFrames)
        {
            m_TexNameByHandle.erase(it->second.Handle);
            ReleaseTextureAndViews(it->second.Handle, it->second.MipViews);
            it = m_Tex.erase(it);
        }
        else
            ++it;
    }
    for (auto it = m_Buf.begin(); it != m_Buf.end();)
    {
        if ((frameIndex - it->second.LastUsedFrame) > maxIdleFrames)
        {
            m_Device->DestroyBuffer(it->second.Handle);
            it = m_Buf.erase(it);
        }
        else
            ++it;
    }

    auto retiredTex = [&](const DeferredTexture& d) { return frameIndex >= d.RetireFrame + framesInFlight; };
    for (auto& d : m_DeferredTex)
        if (retiredTex(d))
            ReleaseTextureAndViews(d.Handle, d.MipViews);
    m_DeferredTex.erase(std::remove_if(m_DeferredTex.begin(), m_DeferredTex.end(), retiredTex), m_DeferredTex.end());

    auto retiredBuf = [&](const Deferred<BufferHandle>& d) { return frameIndex >= d.RetireFrame + framesInFlight; };
    for (const auto& d : m_DeferredBuf)
        if (retiredBuf(d))
            m_Device->DestroyBuffer(d.Handle);
    m_DeferredBuf.erase(std::remove_if(m_DeferredBuf.begin(), m_DeferredBuf.end(), retiredBuf), m_DeferredBuf.end());
}

} // namespace GameEngine::Rendering::RenderGraph
