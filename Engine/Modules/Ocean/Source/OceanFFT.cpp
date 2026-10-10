#include "Ocean/OceanFFT.h"
#include "Ocean/OceanShaderProgram.h"
#include "Rendering/Materials/ShaderLayoutShape.h"
#include "OceanShaderDirectory.h"

#include "Logger/Logger.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"
#include "Rendering/Materials/ShaderCompileService.h"
#include "Rendering/Utils/TextureUploadHelpers.h"

#include <array>
#include <cmath>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <vector>

namespace GameEngine::Ocean
{

namespace fs = std::filesystem;
using namespace ::GameEngine::Rendering;

namespace
{

// the reference InitializeButterfly (MIT): radix-2 twiddle table, width = resolution,
// height = log2(resolution). Each texel is (cos, -sin, 0, 1).
void GenerateButterflyTable(uint32 resolution, uint32 passes, std::vector<float>& out)
{
    out.assign(static_cast<size_t>(resolution) * passes * 4u, 0.0f);
    int offset = 1;
    int numIterations = static_cast<int>(resolution) >> 1;
    constexpr float kPi2 = 6.28318530717959f;

    for (int rowIndex = 0; rowIndex < static_cast<int>(passes); ++rowIndex)
    {
        const int rowOffset = rowIndex * static_cast<int>(resolution);
        int start = 0;
        int end = 2 * offset;
        for (int iteration = 0; iteration < numIterations; ++iteration)
        {
            float bigK = 0.0f;
            for (int K = start; K < end; K += 2)
            {
                const float phase = kPi2 * bigK * static_cast<float>(numIterations) / static_cast<float>(resolution);
                const float c = std::cos(phase);
                const float s = std::sin(phase);
                const int i0 = rowOffset + K / 2;
                const int i1 = rowOffset + K / 2 + offset;
                out[i0 * 4 + 0] = c;  out[i0 * 4 + 1] = -s; out[i0 * 4 + 2] = 0.0f; out[i0 * 4 + 3] = 1.0f;
                out[i1 * 4 + 0] = -c; out[i1 * 4 + 1] = s;  out[i1 * 4 + 2] = 0.0f; out[i1 * 4 + 3] = 1.0f;
                bigK += 1.0f;
            }
            start += 4 * offset;
            end = start + 2 * offset;
        }
        numIterations >>= 1;
        offset <<= 1;
    }
}

DescriptorSetLayoutDesc MakeComputeLayout(const char* name, std::initializer_list<uint32> storageImageBindings)
{
    DescriptorSetLayoutDesc layout{};
    layout.debugName = name;

    DescriptorBinding ub{};
    ub.binding = 0;
    ub.type = DescriptorType::UniformBuffer;
    ub.count = 1;
    ub.shaderStages = kShaderStageCompute;
    layout.bindings.push_back(ub);

    for (uint32 binding : storageImageBindings)
    {
        DescriptorBinding si{};
        si.binding = binding;
        si.type = DescriptorType::StorageImage;
        si.count = 1;
        si.shaderStages = kShaderStageCompute;
        layout.bindings.push_back(si);
    }
    return layout;
}

} // anonymous namespace

OceanFFT::~OceanFFT()
{
    if (!m_Device)
        return;
    for (auto t : {m_H0, m_SpecH, m_SpecX, m_SpecZ, m_TmpH, m_TmpX, m_TmpZ, m_Displacement, m_Butterfly})
    {
        if (t.IsValid())
            m_Device->DestroyTexture(t);
    }
    if (m_Sampler.IsValid())
        m_Device->DestroySampler(m_Sampler);
}

bool OceanFFT::Initialize(IDevice* device)
{
    if (m_Ready)
        return true;
    if (!device)
        return false;
    m_Device = device;

    if (!CreateTextures() || !CreateButterflyTable() || !CompilePipelines())
    {
        Logger::Log::Error("OceanFFT: initialization failed");
        return false;
    }

    m_Ready = true;
    Logger::Log::Info("OceanFFT: ready ({}x{} x{} cascades)", kOceanFFTResolution, kOceanFFTResolution,
                      kOceanFFTCascades);
    return true;
}

bool OceanFFT::CreateTextures()
{
    auto makeArray = [&](TextureFormat fmt, const char* name,
                         ResourceState initialState) -> TextureHandle {
        TextureDesc d{};
        d.width = kOceanFFTResolution;
        d.height = kOceanFFTResolution;
        d.arrayLayers = kOceanFFTCascades;
        d.mipLevels = 1;
        d.format = static_cast<uint32>(fmt);
        d.usage = static_cast<uint32>(TextureUsage::UnorderedAccess | TextureUsage::ShaderResource);
        d.flags = TextureCreateFlags::ForceArrayView;
        d.persistent = true;
        d.initialState = initialState;
        d.debugName = name;
        return m_Device->CreateTexture(d);
    };

    // Internals never leave this class; they live in UnorderedAccess and get
    // their first-use init inside RecordDispatch. The displacement is the module
    // output: created at ShaderResource so it RESTS at the state the render
    // graph import claims (same contract as OceanCascadeArray).
    m_H0 = makeArray(TextureFormat::R32G32B32A32_FLOAT, "Ocean_FFT_H0", ResourceState::Undefined);
    m_SpecH = makeArray(TextureFormat::R32G32B32A32_FLOAT, "Ocean_FFT_SpecH", ResourceState::Undefined);
    m_SpecX = makeArray(TextureFormat::R32G32B32A32_FLOAT, "Ocean_FFT_SpecX", ResourceState::Undefined);
    m_SpecZ = makeArray(TextureFormat::R32G32B32A32_FLOAT, "Ocean_FFT_SpecZ", ResourceState::Undefined);
    m_TmpH = makeArray(TextureFormat::R32G32B32A32_FLOAT, "Ocean_FFT_TmpH", ResourceState::Undefined);
    m_TmpX = makeArray(TextureFormat::R32G32B32A32_FLOAT, "Ocean_FFT_TmpX", ResourceState::Undefined);
    m_TmpZ = makeArray(TextureFormat::R32G32B32A32_FLOAT, "Ocean_FFT_TmpZ", ResourceState::Undefined);
    m_Displacement = makeArray(TextureFormat::R16G16B16A16_FLOAT, "Ocean_FFT_Displacement",
                               ResourceState::ShaderResource);

    m_Sampler = m_Device->CreateSampler(SamplerDesc::MaterialLinearRepeat("Ocean_FFT_Sampler"));

    return m_H0.IsValid() && m_SpecH.IsValid() && m_SpecX.IsValid() && m_SpecZ.IsValid() &&
           m_TmpH.IsValid() && m_TmpX.IsValid() && m_TmpZ.IsValid() && m_Displacement.IsValid() &&
           m_Sampler.IsValid();
}

bool OceanFFT::CreateButterflyTable()
{
    std::vector<float> data;
    GenerateButterflyTable(kOceanFFTResolution, kOceanFFTPasses, data);

    TextureDesc d{};
    d.width = kOceanFFTResolution;
    d.height = kOceanFFTPasses;
    d.arrayLayers = 1;
    d.mipLevels = 1;
    d.format = static_cast<uint32>(TextureFormat::R32G32B32A32_FLOAT);
    d.usage = static_cast<uint32>(TextureUsage::UnorderedAccess | TextureUsage::ShaderResource |
                                  TextureUsage::TransferDst);
    d.persistent = true;
    d.debugName = "Ocean_FFT_Butterfly";
    m_Butterfly = m_Device->CreateTexture(d);
    if (!m_Butterfly.IsValid())
        return false;

    UploadTexture2D(m_Device, m_Butterfly, data.data(), kOceanFFTResolution, kOceanFFTPasses,
                    static_cast<size_t>(kOceanFFTResolution) * 4u * sizeof(float),
                    "Ocean_FFT_Butterfly_Upload");
    return true;
}

bool OceanFFT::CompilePipelines()
{
    const fs::path shaderDir = OceanShaderDirectory("ocean_fft_spectrum_init.comp");
    if (shaderDir.empty())
    {
        Logger::Log::Error("OceanFFT: FFT compute shaders not found");
        return false;
    }
    const fs::path includeDir = shaderDir.parent_path(); // Assets/Shaders, for "Ocean/..." includes
    const fs::path cacheRoot = fs::path(".Cache") / "Shaders";

    struct CompiledStage
    {
        std::shared_ptr<const std::vector<uint8_t>> ShaderBytes;
        ShaderMeta Meta;
        explicit operator bool() const { return ShaderBytes != nullptr; }
    };

    auto compile = [&](const char* file, const char* dbg) -> CompiledStage {
        ShaderProgramCompileRequest req{};
        req.debugName = dbg;
        req.baseDirectory = shaderDir;
        req.cacheRoot = cacheRoot;
        req.includeDirs = {includeDir};
        req.stages = {{"cs", file, "main", {}}};

        ShaderProgramCompileResult result{};
        std::string err;
        if (!LoadOceanShaderProgram(req, m_Device->PreferredShaderSource(), result, &err))
        {
            Logger::Log::Error("OceanFFT: compile {} failed: {}", file, err);
            return {};
        }
        auto it = result.stageBytes.find("cs");
        if (it == result.stageBytes.end() || it->second.empty())
        {
            Logger::Log::Error("OceanFFT: no SPIR-V for {}", file);
            return {};
        }
        return CompiledStage{std::make_shared<const std::vector<uint8_t>>(std::move(it->second)),
                             std::move(result.meta)};
    };

    auto initBytes = compile("ocean_fft_spectrum_init.comp", "ocean_fft_init");
    auto updateBytes = compile("ocean_fft_spectrum_update.comp", "ocean_fft_update");
    auto fftHBytes = compile("ocean_fft_horizontal.comp", "ocean_fft_h");
    auto fftVBytes = compile("ocean_fft_vertical.comp", "ocean_fft_v");
    if (!initBytes || !updateBytes || !fftHBytes || !fftVBytes)
        return false;

    m_InitLayout = MakeComputeLayout("OceanFFT_Init_Set0", {1});
    m_UpdateLayout = MakeComputeLayout("OceanFFT_Update_Set0", {1, 2, 3, 4});
    m_FFTHLayout = MakeComputeLayout("OceanFFT_FFTH_Set0", {1, 2, 3, 4, 5, 6, 7});
    m_FFTVLayout = MakeComputeLayout("OceanFFT_FFTV_Set0", {1, 2, 3, 4, 5});

    auto intern = [&](const CompiledStage& stage, DescriptorSetLayoutDesc& layout,
                      const char* dbg) -> ComputePipelineId {
        ComputePipelineDesc cd{};
        cd.ComputeShader = stage.ShaderBytes;
        // The cascades are rgba32f 2D arrays, which a WebGPU layout must state.
        Rendering::ApplyMetaImageShapeToLayout(stage.Meta, layout);
        cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(layout));
        cd.DebugName = dbg;
        return m_Device->InternComputePipeline(cd);
    };

    m_InitPipe = intern(initBytes, m_InitLayout, "OceanFFT_Init");
    m_UpdatePipe = intern(updateBytes, m_UpdateLayout, "OceanFFT_Update");
    m_FFTHPipe = intern(fftHBytes, m_FFTHLayout, "OceanFFT_FFTH");
    m_FFTVPipe = intern(fftVBytes, m_FFTVLayout, "OceanFFT_FFTV");

    return m_InitPipe.IsValid() && m_UpdatePipe.IsValid() && m_FFTHPipe.IsValid() && m_FFTVPipe.IsValid();
}

void OceanFFT::SetParams(const OceanFFTParamsGPU& params, bool spectrumDirty)
{
    m_Params = params;
    if (spectrumDirty)
        m_SpectrumDirty = true;
}

void OceanFFT::FillParams(OceanFFTParamsGPU& out)
{
    out = m_Params;
}

void OceanFFT::RecordPass(IDevice* device, CommandList* cl, ComputePipelineId pipeline,
                          const DescriptorSetLayoutDesc& layout, BufferHandle paramsBuffer,
                          uint64 paramsOffset, const TextureHandle* images, const uint32* slots,
                          uint32 imageCount, uint32 groupsX, uint32 groupsY, uint32 groupsZ)
{
    PipelineHandle pipe = device->GetOrCreateComputePipeline(pipeline);
    if (!pipe)
        return;

    cl->SetPipeline(pipe);

    DescriptorSetDesc dsDesc{};
    dsDesc.layout = layout;
    dsDesc.transient = true;
    dsDesc.debugName = "OceanFFT_DS";
    DescriptorSetHandle ds = device->CreateDescriptorSet(dsDesc);

    device->UpdateBufferBinding(ds, 0, paramsBuffer, paramsOffset, sizeof(OceanFFTParamsGPU));
    for (uint32 i = 0; i < imageCount; ++i)
        device->UpdateStorageImageBinding(ds, slots[i], images[i]);

    cl->BindDescriptorSet(0, ds, pipe);
    cl->Dispatch(groupsX, groupsY, groupsZ);
}

void OceanFFT::RecordDispatch(IDevice* device, CommandList* cl, BufferHandle paramsBuffer,
                              uint64 paramsOffset)
{
    if (!m_Ready || !cl || !device || !paramsBuffer.IsValid())
        return;

    const uint32 layers = kOceanFFTCascades;
    auto uavBarrier = [&](TextureHandle t) {
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(
            t, ResourceState::UnorderedAccess, ResourceState::UnorderedAccess, 0, 1, 0, layers));
    };

    // First-use layout init: bring the INTERNAL storage textures into
    // UnorderedAccess (they never leave it — intra-pass only, invisible to the
    // render graph). The displacement is graph-owned: the pass's declared Write
    // provides its ShaderResource->General entry transition, the first declared
    // reader restores ShaderReadOnly, and MarkOutput covers reader-less frames.
    if (!m_DisplacementWritten)
    {
        for (auto t : {m_H0, m_SpecH, m_SpecX, m_SpecZ, m_TmpH, m_TmpX, m_TmpZ})
            cl->Barrier(ResourceBarrier::CreateTextureBarrier(
                t, ResourceState::Undefined, ResourceState::UnorderedAccess, 0, 1, 0, layers));
        // Butterfly was uploaded as ShaderResource but is read as a storage image.
        cl->Barrier(ResourceBarrier::CreateTextureBarrier(
            m_Butterfly, ResourceState::ShaderResource, ResourceState::UnorderedAccess, 0, 1, 0, 1));
    }

    const uint32 res = kOceanFFTResolution;
    const uint32 g8 = (res + 7u) / 8u;

    // 1. Spectrum init (H0) — only when the spectrum changed.
    if (m_SpectrumDirty)
    {
        const TextureHandle imgs[] = {m_H0};
        const uint32 slots[] = {1};
        RecordPass(device, cl, m_InitPipe, m_InitLayout, paramsBuffer, paramsOffset, imgs, slots, 1,
                   g8, g8, layers);
        uavBarrier(m_H0);
        m_SpectrumDirty = false;
    }

    // 2. Spectrum update (time evolution -> spectral H/X/Z).
    {
        const TextureHandle imgs[] = {m_H0, m_SpecH, m_SpecX, m_SpecZ};
        const uint32 slots[] = {1, 2, 3, 4};
        RecordPass(device, cl, m_UpdatePipe, m_UpdateLayout, paramsBuffer, paramsOffset, imgs, slots,
                   4, g8, g8, layers);
        uavBarrier(m_SpecH);
        uavBarrier(m_SpecX);
        uavBarrier(m_SpecZ);
    }

    // 3. FFT horizontal (rows): one workgroup per row.
    {
        const TextureHandle imgs[] = {m_SpecH, m_SpecX, m_SpecZ, m_Butterfly, m_TmpH, m_TmpX, m_TmpZ};
        const uint32 slots[] = {1, 2, 3, 4, 5, 6, 7};
        RecordPass(device, cl, m_FFTHPipe, m_FFTHLayout, paramsBuffer, paramsOffset, imgs, slots, 7,
                   1, res, layers);
        uavBarrier(m_TmpH);
        uavBarrier(m_TmpX);
        uavBarrier(m_TmpZ);
    }

    // 4. FFT vertical (cols): one workgroup per column; writes the displacement.
    {
        const TextureHandle imgs[] = {m_TmpH, m_TmpX, m_TmpZ, m_Butterfly, m_Displacement};
        const uint32 slots[] = {1, 2, 3, 4, 5};
        RecordPass(device, cl, m_FFTVPipe, m_FFTVLayout, paramsBuffer, paramsOffset, imgs, slots, 5,
                   res, 1, layers);
    }

    m_DisplacementWritten = true;
}

RenderGraph::RGTexture OceanFFT::ImportDisplacementRG(RenderGraph::RGFrame& frame) const
{
    if (!m_Ready || !m_Displacement.IsValid())
        return {};
    return frame.ImportExternalTexture("Ocean_FFT_Displacement", m_Displacement,
                                       ResourceState::ShaderResource,
                                       TextureFormat::R16G16B16A16_FLOAT, 1, kOceanFFTCascades);
}

} // namespace GameEngine::Ocean
