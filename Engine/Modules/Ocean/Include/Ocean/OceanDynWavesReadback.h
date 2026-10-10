#pragma once

#include "Ocean/OceanTypes.h"

#include "Rendering/Core/Device.h" // DescriptorSetLayoutDesc
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Core/RenderGraph/RGReadbackRing.h"
#include "Types/Types.h"

#include <cstdint>
#include <mutex>
#include <vector>

namespace GameEngine::Rendering
{
class IDevice;
class CommandList;
} // namespace GameEngine::Rendering

namespace GameEngine::Ocean
{

// std140 mirror of OceanDynWavesQueryParams in ocean_dynwaves_query.comp.
struct OceanDynWavesQueryParamsGPU
{
    uint32 Resolution;
    uint32 DynLodCount;
    float OriginX;
    float OriginZ;

    float TexelSize;
    float Pad0;
    float Pad1;
    float Pad2;

    OceanCascadeLayoutGPU Cascade;
};
static_assert(sizeof(OceanDynWavesQueryParamsGPU) == 32 + sizeof(OceanCascadeLayoutGPU),
              "OceanDynWavesQueryParamsGPU must be std140 (two scalar lanes + cascade)");

// CPU-readable readback of the dynamic-wave height cascade. The surface samples
// OceanDynWaves directly on the GPU; this companion pass samples the same snapped
// cascade into a small host-visible tile a few frames late so gameplay queries can
// include interactive ripples without synchronizing the render thread.
class OceanDynWavesReadback
{
public:
    ~OceanDynWavesReadback()
    {
        m_Ring.Destroy(m_Device);
    }

    bool Initialize(::GameEngine::Rendering::IDevice* device);
    bool IsReady() const { return m_Ready; }

    void BeginFrame(float cameraX, float cameraZ);
    void SetFrameInputs(::GameEngine::Rendering::TextureHandle dynWaves,
                        ::GameEngine::Rendering::SamplerHandle dynSampler,
                        const OceanCascadeLayoutGPU& layout);

    // Finalize this frame's GPU params (tile origin and cascade layout were
    // fixed by BeginFrame/SetFrameInputs). Called at DECLARE time; the caller
    // writes the result into a render-graph upload-ring allocation and hands
    // the {buffer, offset} to RecordDispatch.
    void FillParams(OceanDynWavesQueryParamsGPU& out);

    // Records the query dispatch and maps the newest token-signaled slot into
    // the CPU tile. `frame` keys the readback's completion pending (stamped by
    // OnFrameSubmitted); params come from the frame's upload ring (FillParams
    // above) — no per-class buffer ring. Any dispatch cadence is valid: the
    // dyn-waves quiescence gap needs no reset.
    void RecordDispatch(::GameEngine::Rendering::IDevice* device,
                        ::GameEngine::Rendering::CommandList* cl,
                        const ::GameEngine::Rendering::RenderGraph::RGFrame& frame,
                        ::GameEngine::Rendering::BufferHandle paramsBuffer, uint64 paramsOffset);

    // Post-submit stamp: attaches the submission's fence token to this frame's
    // pending readback (RGReadbackRing completion contract).
    void OnFrameSubmitted(const ::GameEngine::Rendering::RenderGraph::RGFrame& frame,
                          const ::GameEngine::Rendering::IDevice::GpuSyncToken& token)
    {
        m_Ring.OnFrameSubmitted(frame, token);
    }
    void OnFrameStreamRetired(const ::GameEngine::Rendering::RenderGraph::RGFrame& frame)
    {
        m_Ring.OnFrameStreamRetired(frame);
    }

    bool Sample(float worldX, float worldZ, float& outHeight) const;
    void OnOriginShift(float32 shiftX, float32 shiftZ, bool invalidate);

private:
    static constexpr uint32 kResolution = 256;
    static constexpr float kTileMeters = 512.0f;

    ::GameEngine::Rendering::IDevice* m_Device = nullptr;
    bool m_Ready = false;

    ::GameEngine::Rendering::ComputePipelineId m_Pipe{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_Layout;

    // Tile origin stamped when the dispatch writes a slot; the CPU sample
    // window must match the tile the data was QUERIED for, not the current one.
    struct ReadbackMeta
    {
        float OriginX = 0.0f;
        float OriginZ = 0.0f;
    };
    ::GameEngine::Rendering::RenderGraph::RGReadbackRing<ReadbackMeta> m_Ring;

    ::GameEngine::Rendering::TextureHandle m_DynWaves;
    ::GameEngine::Rendering::SamplerHandle m_DynSampler;
    OceanCascadeLayoutGPU m_CascadeLayout{};
    float m_OriginX = 0.0f;
    float m_OriginZ = 0.0f;
    const float m_TexelSize = kTileMeters / float(kResolution);

    mutable std::mutex m_Mutex;
    std::vector<float> m_CPUHeights;
    bool m_HasData = false;
    float m_ReadOriginX = 0.0f;
    float m_ReadOriginZ = 0.0f;
};

} // namespace GameEngine::Ocean
