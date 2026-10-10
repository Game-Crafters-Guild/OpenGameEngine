#pragma once

#include <type_traits>
#ifdef ECS_ADAPTER_DIAGNOSTICS
#include <atomic>
#endif

namespace GameEngine {
namespace ECS {
namespace Detail {

#ifdef ECS_ADAPTER_DIAGNOSTICS
    struct ChunkAdapterDiag {
        static inline std::atomic<uint64_t> CallsWithCount{0};
        static inline std::atomic<uint64_t> CallsWithoutCount{0};
        static inline void Reset() {
            CallsWithCount.store(0, std::memory_order_relaxed);
            CallsWithoutCount.store(0, std::memory_order_relaxed);
        }
    };
#endif

    template<typename F>
    struct ChunkCallAdapter {
        F func;
        template<typename... Ptrs>
        void operator()(Ptrs... ptrs, std::size_t count) {
            using FnT = F;
            if constexpr (std::is_invocable_v<FnT, Ptrs..., std::size_t>) {
#ifdef ECS_ADAPTER_DIAGNOSTICS
                ChunkAdapterDiag::CallsWithCount.fetch_add(1, std::memory_order_relaxed);
#endif
                func(ptrs..., count);
            } else if constexpr (std::is_invocable_v<FnT, Ptrs...>) {
#ifdef ECS_ADAPTER_DIAGNOSTICS
                ChunkAdapterDiag::CallsWithoutCount.fetch_add(1, std::memory_order_relaxed);
#endif
                (void)count;
                func(ptrs...);
            } else {
                static_assert(!std::is_same_v<FnT, FnT>, "Unsupported functor signature for chunk iteration; expected (T*..., size_t) or (T*...)");
            }
        }
    };
}
}
}

