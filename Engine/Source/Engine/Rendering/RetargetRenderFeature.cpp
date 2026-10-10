#include "Engine/Rendering/RetargetRenderFeature.h"

#include "Core/Application.h"
#include "Engine/Rendering/RetargetFullPass.h"
#include "Logger/Logger.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/Device.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>

namespace GameEngine
{
namespace Engine::Renderer
{

namespace
{
// 0 = unread, 1 = GPU disabled, 2 = GPU enabled. Lookup is intentionally
// process-scoped: changing the env var mid-process needs a restart. Mirrors
// the existing IsRetargetCPUOnly() pattern in RetargetSubmission.cpp.
std::atomic<uint8_t> g_GpuEnabledState{0};

// The build stages retarget kernels to <install assets root>/Shaders for the Editor, the
// Player and RetargetGPUParityTests alike, so that is the only correct candidate. Relative
// spellings would resolve against the process working directory, which the engine pins to
// the open project root — never a directory containing engine shaders.
std::vector<uint8_t> LoadRetargetSpv(const char* name)
{
    const std::filesystem::path installRoot = PathUtils::GetInstallAssetsRoot();
    if (installRoot.empty())
        return {};

    return ::GameEngine::Rendering::Utils::ReadFile((installRoot / "Shaders" / name).string());
}

} // namespace

bool RetargetRenderFeature::ReadEnvFlag()
{
    // Phase 3c: GPU retarget is now the default. Defaults to ENABLED unless
    // GE_RETARGET_CPU=1 (or r.RetargetCPU=1) forces the legacy CPU path
    // (the CPU runtime path itself is gated separately in HumanoidRetargetSystem).
    uint8_t state = g_GpuEnabledState.load(std::memory_order_relaxed);
    if (state != 0)
        return state == 2;

    bool cpuOnly = false;
#if defined(_WIN32)
    char* val = nullptr;
    size_t len = 0;
    if (_dupenv_s(&val, &len, "GE_RETARGET_CPU") == 0 && val)
    {
        cpuOnly = (len > 1 && val[0] == '1');
        free(val);
    }
    if (!cpuOnly && _dupenv_s(&val, &len, "r.RetargetCPU") == 0 && val)
    {
        cpuOnly = (len > 1 && val[0] == '1');
        free(val);
    }
#else
    if (const char* v = std::getenv("GE_RETARGET_CPU"))
        cpuOnly = (v[0] == '1');
    if (!cpuOnly)
    {
        if (const char* v = std::getenv("r.RetargetCPU"))
            cpuOnly = (v[0] == '1');
    }
#endif
    const bool enabled = !cpuOnly;
    g_GpuEnabledState.store(enabled ? 2u : 1u, std::memory_order_relaxed);
    return enabled;
}

RetargetRenderFeature::~RetargetRenderFeature()
{
    if (m_Initialized)
        m_DataStore.Shutdown();
    // m_Pass dtor runs here. RG holds raw lambda captures of m_Pass.get();
    // any pass that fires after this destructor would UAF. Acceptable because
    // feature destruction happens at engine teardown, after the RG is torn
    // down too.
}

bool RetargetRenderFeature::Initialize(::GameEngine::Rendering::IDevice* device)
{
    if (m_Initialized)
        return true;
    if (!device)
        return false;
    if (m_InitializeAttempted.exchange(true))
        return false;

    m_Device = device;

    if (!m_DataStore.Initialize(device))
    {
        Logger::Log::Warning("[RetargetRenderFeature] RetargetGPUDataStore::Initialize failed; GPU path unavailable.");
        return false;
    }

    // Load the fused retarget compute SPIR-V. Loaded once at engine init.
    m_FullSpv = LoadRetargetSpv("retarget_full.comp.spv");
    if (m_FullSpv.empty())
    {
        Logger::Log::Warning("[RetargetRenderFeature] retarget_full.comp.spv not found; GPU path disabled.");
        m_DataStore.Shutdown();
        return false;
    }

    // Persistent pass instance. RG's CreatePass version-token short-circuit
    // keeps the existing lambda capture across frames; the pointer must stay
    // stable for the lifetime of the feature or executes will UAF.
    m_Pass = std::make_unique<RetargetFullPass>();
    m_Pass->SetComputeShader(m_FullSpv);
    m_Pass->SetDataStore(&m_DataStore);

    m_Initialized = true;
    m_GpuEnabled  = ReadEnvFlag();
    Logger::Log::Info("[RetargetRenderFeature] Initialized. GPU dispatch={} "
                      "(set GE_RETARGET_CPU=1 to force the legacy CPU path).",
                      m_GpuEnabled ? "ENABLED (default)" : "disabled (CPU forced)");
    return true;
}

void RetargetRenderFeature::BeginFrame(uint32_t frameIndex)
{
    if (!m_Initialized) return;
    m_FrameIndex = frameIndex;
    m_DataStore.BeginFrame(frameIndex);
}

void RetargetRenderFeature::EndFrame()
{
    // Reserved.
}

void RetargetRenderFeature::BuildPasses(::GameEngine::Rendering::RenderGraph::RGFrame& frame,
                                        ::GameEngine::Rendering::RenderGraph::RGBuffer skinPaletteAtlas,
                                        float deltaTime)
{
    (void)deltaTime;
    if (!m_Initialized || !m_GpuEnabled || !m_Pass) return;
    if (m_DataStore.GetActiveCharacterCount() == 0) return;
    if (m_DataStore.IsDispatchScheduledThisFrame()) return;
    if (!skinPaletteAtlas.IsValid()) return;

    // Same RGBuffer value the skinning pass writes - one resource id, so RenderGraph
    // orders the two atlas writers; no rebind dance, no phase pinning.
    m_Pass->Declare(frame, skinPaletteAtlas);
    m_DataStore.MarkDispatchScheduledThisFrame();
}

} // namespace Engine::Renderer
} // namespace GameEngine
