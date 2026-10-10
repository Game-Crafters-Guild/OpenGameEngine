#pragma once

#include <atomic>
#include <memory>

namespace GameEngine {

/**
 * @brief Thread-scoped access to the cancellation flag of the asset decode
 * running on this thread.
 *
 * Decodes reach long-running work (the texture import cook's BCn encode)
 * through the generic ProcessAssetData seam, which is shared by every asset
 * type and carries no per-load state. Rather than widening that seam with a
 * parameter no other asset type reads, the two decode entry points publish
 * their existing cancel flag for the duration of the decode — the same idiom
 * AssetManager::ScopedThreadAssetManager already uses, at the same call sites,
 * for the same reason.
 *
 * The flag is the one the decode was handed, so it serves every cancel path
 * that owns one (shutdown, eject/unload, reload supersede) without the callee
 * knowing which triggered it. Work that can abandon itself mid-flight should
 * take the flag once and poll the atomic; a decode running outside either
 * entry point (synchronous load, tooling) sees null and is simply not
 * cancellable.
 */
class ScopedAssetDecodeCancellation
{
  public:
    explicit ScopedAssetDecodeCancellation(const std::shared_ptr<std::atomic<bool>>& cancelRequested);
    ~ScopedAssetDecodeCancellation();

    ScopedAssetDecodeCancellation(const ScopedAssetDecodeCancellation&) = delete;
    ScopedAssetDecodeCancellation& operator=(const ScopedAssetDecodeCancellation&) = delete;

  private:
    std::shared_ptr<std::atomic<bool>> m_Prev;
};

/**
 * @brief The cancel flag of the decode running on this thread, or null when
 * called outside a decode context. Fetch once and poll the atomic — the
 * lookup is thread-local, the poll is lock-free.
 */
const std::shared_ptr<std::atomic<bool>>& CurrentAssetDecodeCancellation();

/**
 * @brief Convenience poll of CurrentAssetDecodeCancellation(): true only when a
 * flag is published AND set. False outside a decode context.
 */
bool IsAssetDecodeCancelled();

} // namespace GameEngine
