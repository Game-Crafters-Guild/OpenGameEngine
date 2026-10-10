#pragma once

// RenderGraph (render-graph v2) per-frame bump allocator.
//
// The whole frame graph — passes, resources, accesses, barriers, the submission
// plan — is allocated from one RGArena and freed wholesale at frame end via
// Reset(). This is the core of the immediate-mode design: no per-object heap
// frees, no retained diff-tracking. Blocks are chained (never reallocated), so a
// pointer handed out by Alloc/New stays valid for the whole frame even if the
// arena grows. Reset() keeps the blocks (capacity is retained) and only rewinds,
// so after warmup a steady-state frame performs zero heap allocations — which is
// what keeps Debug builds (where _ITERATOR_DEBUG_LEVEL=2 amplifies allocation
// cost) cheap.
//
// Discipline:
//  - Alloc<T>() / AllocArray<T>() are for trivially-destructible types only
//    (enforced by static_assert). They are pure bump allocations.
//  - New<T>() is for types that own resources (e.g. an RGPass holding
//    std::function execute lambdas). It records a destructor that Reset() runs
//    LIFO before rewinding, mirroring UE FRDGAllocator's tracked-object path.

#include <cstddef>
#include <cstdint>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace GameEngine::Rendering::RenderGraph
{

class RGArena
{
  public:
    static constexpr size_t kDefaultBlockBytes = 64 * 1024;
    static constexpr size_t kDefaultAlign = alignof(std::max_align_t);

    explicit RGArena(size_t firstBlockBytes = kDefaultBlockBytes)
        : m_DefaultBlockBytes(firstBlockBytes ? firstBlockBytes : kDefaultBlockBytes)
    {
        m_Blocks.reserve(4);
        m_Dtors.reserve(64);
        AddBlock(m_DefaultBlockBytes);
    }

    ~RGArena()
    {
        RunDestructors();
        for (Block& b : m_Blocks)
            ::operator delete(b.Data, std::align_val_t{kBlockAlign});
    }

    RGArena(const RGArena&) = delete;
    RGArena& operator=(const RGArena&) = delete;
    RGArena(RGArena&&) = delete;
    RGArena& operator=(RGArena&&) = delete;

    // Raw aligned bump allocation. Pointer is stable until Reset(). Alignment is
    // applied to the ABSOLUTE address (blocks are only kBlockAlign-aligned, so
    // aligning the offset alone would not satisfy over-aligned requests).
    void* AllocRaw(size_t size, size_t align = kDefaultAlign)
    {
        if (size == 0)
            size = 1;
        Block* blk = &m_Blocks[m_Current];
        uintptr_t base = reinterpret_cast<uintptr_t>(blk->Data);
        uintptr_t aligned = AlignUp(base + blk->Used, align);
        size_t newUsed = static_cast<size_t>(aligned - base) + size;
        if (newUsed > blk->Capacity)
        {
            blk = AcquireBlockFor(size, align);
            base = reinterpret_cast<uintptr_t>(blk->Data);
            aligned = AlignUp(base + blk->Used, align);
            newUsed = static_cast<size_t>(aligned - base) + size;
        }
        void* p = reinterpret_cast<void*>(aligned);
        blk->Used = newUsed;
        m_BytesUsed += size;
        return p;
    }

    // Single trivially-destructible object.
    template <class T, class... Args>
    T* Alloc(Args&&... args)
    {
        static_assert(std::is_trivially_destructible_v<T>,
                      "RGArena::Alloc is for trivially-destructible types; use New<T>() for types with destructors");
        void* p = AllocRaw(sizeof(T), alignof(T));
        return ::new (p) T(std::forward<Args>(args)...);
    }

    // Array of trivially-destructible, default-initialized elements.
    template <class T>
    std::span<T> AllocArray(size_t count)
    {
        static_assert(std::is_trivially_destructible_v<T>,
                      "RGArena::AllocArray is for trivially-destructible types");
        if (count == 0)
            return std::span<T>{};
        void* p = AllocRaw(sizeof(T) * count, alignof(T));
        T* arr = static_cast<T*>(p);
        for (size_t i = 0; i < count; ++i)
            ::new (&arr[i]) T();
        return std::span<T>(arr, count);
    }

    // Copy a range into an arena-backed span (trivially-destructible elements).
    template <class T>
    std::span<T> CopyArray(const T* src, size_t count)
    {
        std::span<T> dst = AllocArray<T>(count);
        for (size_t i = 0; i < count; ++i)
            dst[i] = src[i];
        return dst;
    }

    // Object that needs destruction (e.g. holds std::function). Its destructor is
    // recorded and run by Reset()/~RGArena() before the storage is rewound.
    template <class T, class... Args>
    T* New(Args&&... args)
    {
        void* p = AllocRaw(sizeof(T), alignof(T));
        T* obj = ::new (p) T(std::forward<Args>(args)...);
        if constexpr (!std::is_trivially_destructible_v<T>)
            m_Dtors.push_back(DtorEntry{obj, &DestroyOne<T>});
        return obj;
    }

    // Rewind the arena for a new frame: run recorded destructors LIFO, then reset
    // every block's used-offset to 0 (capacity retained — no heap free).
    void Reset()
    {
        RunDestructors();
        for (Block& b : m_Blocks)
            b.Used = 0;
        m_Current = 0;
        m_BytesUsed = 0;
    }

    size_t BytesUsed() const { return m_BytesUsed; }

    size_t BytesReserved() const
    {
        size_t total = 0;
        for (const Block& b : m_Blocks)
            total += b.Capacity;
        return total;
    }

    size_t BlockCount() const { return m_Blocks.size(); }
    size_t PendingDestructors() const { return m_Dtors.size(); }

  private:
    static constexpr size_t kBlockAlign = alignof(std::max_align_t);

    struct Block
    {
        uint8_t* Data = nullptr;
        size_t Capacity = 0;
        size_t Used = 0;
    };

    struct DtorEntry
    {
        void* Obj;
        void (*Fn)(void*);
    };

    static size_t AlignUp(size_t v, size_t a)
    {
        return (v + (a - 1)) & ~(a - 1);
    }

    template <class T>
    static void DestroyOne(void* p)
    {
        static_cast<T*>(p)->~T();
    }

    void AddBlock(size_t bytes)
    {
        Block b;
        b.Capacity = bytes;
        b.Used = 0;
        b.Data = static_cast<uint8_t*>(::operator new(bytes, std::align_val_t{kBlockAlign}));
        m_Blocks.push_back(b);
    }

    // Find the next existing block that fits (absolute aligned address + size), or
    // allocate one. A fresh block is sized to size + align so worst-case base
    // misalignment still fits. Used by Reset()-reuse: subsequent frames re-bump
    // through retained blocks.
    Block* AcquireBlockFor(size_t size, size_t align)
    {
        for (size_t i = m_Current + 1; i < m_Blocks.size(); ++i)
        {
            Block& b = m_Blocks[i];
            uintptr_t base = reinterpret_cast<uintptr_t>(b.Data);
            uintptr_t aligned = AlignUp(base + b.Used, align);
            if (static_cast<size_t>(aligned - base) + size <= b.Capacity)
            {
                m_Current = i;
                return &m_Blocks[i];
            }
        }
        size_t need = size + align;
        size_t blockBytes = need > m_DefaultBlockBytes ? need : m_DefaultBlockBytes;
        AddBlock(blockBytes);
        m_Current = m_Blocks.size() - 1;
        return &m_Blocks[m_Current];
    }

    void RunDestructors()
    {
        for (size_t i = m_Dtors.size(); i-- > 0;)
            m_Dtors[i].Fn(m_Dtors[i].Obj);
        m_Dtors.clear();
    }

    std::vector<Block> m_Blocks;
    std::vector<DtorEntry> m_Dtors;
    size_t m_Current = 0;
    size_t m_DefaultBlockBytes;
    size_t m_BytesUsed = 0;
};

} // namespace GameEngine::Rendering::RenderGraph
