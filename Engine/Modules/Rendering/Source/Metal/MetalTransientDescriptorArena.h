#pragma once

#include <Metal/Metal.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

// The memory that the argument tables of one frame slot's transient descriptor
// sets live in. Tables are carved in order out of a few large shared pages, so
// creating a transient set costs no Metal allocation once the pages have grown
// to what a frame uses; Reset makes the whole arena free again when the frame
// slot comes round (MetalDevice::BeginFrame, after the slot's command buffers
// completed), which is the lifetime DescriptorSetDesc::transient promises.
// Pages are kept for the device's life, so the arena settles at the largest
// frame it has served.
//
// The pages are untracked: the CPU writes a table before the command buffer
// that reads it is committed and the GPU only reads it, so Metal's hazard
// tracking would only serialise unrelated encoders that bind the same page.
//
// Not thread-safe: MetalDevice calls it under its resource mutex.
class MetalTransientDescriptorArena
{
  public:
    struct Allocation
    {
        MTL::Buffer* Buffer = nullptr; // nullptr when Metal could not allocate a page
        size_t Offset = 0;
        uint8_t* Data = nullptr;  // CPU view of [Offset, Offset + bytes)
        uint64_t GpuAddress = 0;  // GPU address of Offset
    };

    MetalTransientDescriptorArena() = default;
    ~MetalTransientDescriptorArena();

    MetalTransientDescriptorArena(const MetalTransientDescriptorArena&) = delete;
    MetalTransientDescriptorArena& operator=(const MetalTransientDescriptorArena&) = delete;

    // `bytes` of zeroed memory at an offset that is a multiple of
    // kOffsetAlignment. Takes a new page only when no page left in this frame
    // has room.
    Allocation Allocate(MTL::Device& device, size_t bytes);
    // Frees every allocation. The GPU must no longer read any of them.
    void Reset();
    // Releases the pages; the arena may be used again afterwards.
    void Release();

    // A descriptor set's table is bound with setVertexBuffer / setBuffer at its
    // offset, and SPIRV-Cross declares argument buffers in the constant address
    // space, whose offsets macOS requires to be multiples of 256 bytes.
    static constexpr size_t kOffsetAlignment = 256;
    // 1024 tables of a set with up to 20 resource slots (256 bytes each).
    static constexpr size_t kPageBytes = 256 * 1024;

  private:
    struct Page
    {
        MTL::Buffer* Buffer = nullptr; // owned (+1)
        uint8_t* Data = nullptr;
        uint64_t GpuAddress = 0;
        size_t Capacity = 0;
        size_t Used = 0;
    };

    std::vector<Page> m_Pages;
    size_t m_CurrentPage = 0;
};

} // namespace Rendering
} // namespace GameEngine
