#pragma once

#include "Ocean/OceanRecentMaximum.h"
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

// std140 mirror of OceanHeightQueryParams in ocean_height_query.comp.
struct OceanHeightQueryParamsGPU
{
    uint32 Resolution;
    uint32 FFTCascadeCount;
    float OriginX;
    float OriginZ;

    float TexelSize;
    float SeaLevel;
    float Weight;
    float MaxHorizontal;

    float MaxVertical;
    float WaveOriginOffsetX;
    float WaveOriginOffsetZ;
    float Pad2;
};
static_assert(sizeof(OceanHeightQueryParamsGPU) == 48,
              "OceanHeightQueryParamsGPU must be std140 (three vec4 lanes)");

// Bakes a CPU-readable ocean displacement field over a camera-snapped world tile
// and reads it back (a few frames latent) so the buoyancy system floats bodies on
// the FFT surface (Crest's baked-FFT collision provider). A compute pass inverts
// the horizontal FFT displacement
// and writes displacement.xyz plus SeaLevel + height into a host-visible ring
// buffer (GPU UAV-writes, CPU maps the oldest, GPU-complete slot each frame). One
// instance lives on OceanRenderFeature; OceanRenderNode records the dispatch once
// per frame right after the FFT. Degrades to "not ready" (caller stays flat) until
// the first slot lands and for queries outside the tile.
class OceanHeightField
{
public:
    ~OceanHeightField()
    {
        m_Readback.Destroy(m_Device);
    }

    bool Initialize(::GameEngine::Rendering::IDevice* device);
    bool IsReady() const { return m_Ready; }

    // Snap the query tile to the camera (texel-aligned, stable frame-to-frame).
    // Call once per frame at schedule time, before RecordDispatch.
    void BeginFrame(float cameraX, float cameraZ);

    // Stash this frame's FFT inputs + the surface's shape controls (from the ocean
    // params) so the baked height matches OceanShapeDisplacement.
    void SetFrameInputs(::GameEngine::Rendering::TextureHandle fftDisplacement,
                        ::GameEngine::Rendering::SamplerHandle fftSampler, uint32 fftCascadeCount,
                        float seaLevel, float weight, float maxHorizontal, float maxVertical,
                        float waveOriginOffsetX = 0.0f, float waveOriginOffsetZ = 0.0f);

    void OnOriginShift(float32 shiftX, float32 shiftZ, bool invalidate);
    // Drops the CPU copy and the bakes still in flight, so samples report no data
    // until a newer bake reads back.
    void DiscardCpuData();

    // Finalize this frame's GPU params (tile origin snapped by BeginFrame, shape
    // controls from SetFrameInputs). Called at DECLARE time; the caller writes the
    // result into a render-graph upload-ring allocation and hands the
    // {buffer, offset} to RecordDispatch.
    void FillParams(OceanHeightQueryParamsGPU& out) const;

    // Record the bake into the next ring slot, then map the newest token-
    // signaled slot back into the CPU displacement cache. Once per frame,
    // after the FFT. `frame` keys the readback's completion pending (stamped
    // by OnFrameSubmitted); params come from the frame's upload ring
    // (FillParams above) — no per-class buffer ring.
    void RecordDispatch(::GameEngine::Rendering::IDevice* device,
                        ::GameEngine::Rendering::CommandList* cl,
                        const ::GameEngine::Rendering::RenderGraph::RGFrame& frame,
                        ::GameEngine::Rendering::BufferHandle paramsBuffer, uint64 paramsOffset);

    // Post-submit stamp: attaches the submission's fence token to this frame's
    // pending readback (RGReadbackRing completion contract).
    void OnFrameSubmitted(const ::GameEngine::Rendering::RenderGraph::RGFrame& frame,
                          const ::GameEngine::Rendering::IDevice::GpuSyncToken& token)
    {
        m_Readback.OnFrameSubmitted(frame, token);
    }
    void OnFrameStreamRetired(const ::GameEngine::Rendering::RenderGraph::RGFrame& frame)
    {
        m_Readback.OnFrameStreamRetired(frame);
    }

    // Bilinear sample of the last-read displacement field at world XZ. Returns false
    // (leaves outHeight untouched) when the field isn't ready yet or the query is
    // outside the current tile — the caller then keeps its flat/base sample.
    bool Sample(float worldX, float worldZ, float& outHeight) const;
    bool SampleDisplacement(float worldX, float worldZ,
                            float& outDx, float& outDy, float& outDz,
                            float& outHeight) const;

    // Largest |vertical displacement| (meters, after the weight and the clamp) in
    // the read-back tiles of at least the last second: a measurement of the global
    // FFT surface over the 512 m tile around the camera at 2 m spacing. Cascades
    // whose period exceeds the tile are seen over part of it only, so a caller
    // that needs a bound widens this value. False until a slot has been read.
    bool GetRecentMaxVerticalDisplacement(float& outMeters) const;

private:
    static constexpr uint32 kResolution = 256;   // texels per axis
    static constexpr float kTileMeters = 512.0f; // world extent -> 2 m / texel

    ::GameEngine::Rendering::IDevice* m_Device = nullptr;
    bool m_Ready = false;

    ::GameEngine::Rendering::ComputePipelineId m_Pipe{};
    ::GameEngine::Rendering::DescriptorSetLayoutDesc m_Layout;

    // Tile origin stamped when the bake is recorded; the CPU sample window must
    // match the tile the slot was BAKED for, not the current camera tile.
    struct ReadbackMeta
    {
        float OriginX = 0.0f;
        float OriginZ = 0.0f;
    };

    // Host-visible storage ring (GPU UAV-writes, CPU maps the oldest). Sized
    // FramesInFlight+2 so the slot read each frame is guaranteed GPU-complete
    // (pure-latency readback, no fence token — the bake runs every frame so
    // latency is regular).
    ::GameEngine::Rendering::RenderGraph::RGReadbackRing<ReadbackMeta> m_Readback;

    // Per-frame inputs (captured by SetFrameInputs / BeginFrame).
    ::GameEngine::Rendering::TextureHandle m_FFTDisplacement;
    ::GameEngine::Rendering::SamplerHandle m_FFTSampler;
    uint32 m_FFTCascadeCount = 0;
    float m_SeaLevel = 0.0f;
    float m_Weight = 1.0f;
    float m_MaxHorizontal = 15.0f;
    float m_MaxVertical = 10.0f;
    float m_WaveOriginOffsetX = 0.0f;
    float m_WaveOriginOffsetZ = 0.0f;
    float m_OriginX = 0.0f; // this frame's snapped tile origin (min corner)
    float m_OriginZ = 0.0f;
    const float m_TexelSize = kTileMeters / float(kResolution);

    struct DisplacementSample
    {
        float Dx = 0.0f;
        float Dy = 0.0f;
        float Dz = 0.0f;
        float Height = 0.0f;
    };
    static_assert(sizeof(DisplacementSample) == sizeof(float) * 4,
                  "OceanHeightField::DisplacementSample must match a std430 vec4");

    // CPU-side cache of the last successfully-read slot. Written by RecordDispatch
    // (render thread), read by Sample (ECS/physics thread) — guarded by m_Mutex.
    mutable std::mutex m_Mutex;
    std::vector<DisplacementSample> m_CPUDisplacements;
    bool m_HasData = false;
    mutable OceanRecentMaximum m_RecentMaxVertical;
    float m_ReadOriginX = 0.0f;
    float m_ReadOriginZ = 0.0f;
};

} // namespace GameEngine::Ocean
