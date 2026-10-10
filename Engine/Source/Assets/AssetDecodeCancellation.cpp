#include "Assets/AssetDecodeCancellation.h"

namespace GameEngine {

namespace
{
    // Single definition in Engine.dll; every host shares the one Engine module,
    // so a decode's flag is visible to the whole call tree on that thread.
    thread_local std::shared_ptr<std::atomic<bool>> t_DecodeCancelRequested;
} // namespace

ScopedAssetDecodeCancellation::ScopedAssetDecodeCancellation(
    const std::shared_ptr<std::atomic<bool>>& cancelRequested)
    : m_Prev(t_DecodeCancelRequested)
{
    t_DecodeCancelRequested = cancelRequested;
}

ScopedAssetDecodeCancellation::~ScopedAssetDecodeCancellation()
{
    t_DecodeCancelRequested = std::move(m_Prev);
}

const std::shared_ptr<std::atomic<bool>>& CurrentAssetDecodeCancellation()
{
    return t_DecodeCancelRequested;
}

bool IsAssetDecodeCancelled()
{
    return t_DecodeCancelRequested && t_DecodeCancelRequested->load(std::memory_order_acquire);
}

} // namespace GameEngine
