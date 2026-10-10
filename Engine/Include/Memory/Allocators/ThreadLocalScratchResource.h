#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <vector>

namespace GameEngine::Memory::Allocators
{
// Thread-local scratch allocator intended for short-lived, per-frame/per-task allocations.
//
// Goals:
// - Avoid heap churn for "build" style code (UI layout/style building, graph compilation, etc.)
// - Safe for nested use on the same thread (single underlying arena; no resets while nested)
//
// Usage:
//   Memory::Allocators::ScopedScratchResource scratch(4 * 1024 * 1024);
//   std::pmr::vector<T> tmp(&scratch.Get());
//
// Notes:
// - This is NOT a general-purpose allocator. Memory is reclaimed by calling Release()
//   on the underlying monotonic_buffer_resource at scope begin (outermost scope only).
// - This is per-thread; do not pass the returned resource across threads.
class ScopedScratchResource final
{
  public:
    explicit ScopedScratchResource(std::size_t minBytes = 0)
    {
        Acquire(minBytes);
    }

    ~ScopedScratchResource()
    {
        Release();
    }

    ScopedScratchResource(const ScopedScratchResource&) = delete;
    ScopedScratchResource& operator=(const ScopedScratchResource&) = delete;

    std::pmr::memory_resource& Get() const { return *Tls().resource; }

  private:
    struct TlsState
    {
        std::vector<std::byte> buffer;
        std::unique_ptr<std::pmr::monotonic_buffer_resource> resource;
        std::uint32_t depth = 0;
    };

    static TlsState& Tls()
    {
        static thread_local TlsState s;
        if (!s.resource)
        {
            // Default small buffer; will grow on first Acquire(minBytes).
            s.buffer.resize(256 * 1024);
            s.resource = std::make_unique<std::pmr::monotonic_buffer_resource>(
                s.buffer.data(),
                s.buffer.size(),
                std::pmr::new_delete_resource());
        }
        return s;
    }

    static void Acquire(std::size_t minBytes)
    {
        TlsState& s = Tls();
        const bool outermost = (s.depth == 0);

        // Only the outermost scope is allowed to grow/recreate the backing store.
        // Recreating the resource while nested would invalidate outstanding allocations.
        if (outermost && minBytes > s.buffer.size())
        {
            s.buffer.resize(minBytes);
            s.resource = std::make_unique<std::pmr::monotonic_buffer_resource>(
                s.buffer.data(),
                s.buffer.size(),
                std::pmr::new_delete_resource());
        }

        ++s.depth;

        // Reset only when entering the outermost scope so nested users
        // don't invalidate allocations still in use by their caller.
        if (outermost)
        {
            s.resource->release();
        }
    }

    static void Release()
    {
        TlsState& s = Tls();
        if (s.depth == 0)
            return;
        --s.depth;

        // When the outermost scope ends, immediately free any upstream overflow
        // blocks allocated by the monotonic_buffer_resource during this scope.
        // Without this, overflow blocks are held until the next Acquire() —
        // which may never come if the heavy path stops running (e.g. UI goes
        // idle after our ListView/TreeView guards kick in).
        if (s.depth == 0)
        {
            s.resource->release();
        }
    }
};

} // namespace GameEngine::Memory::Allocators

