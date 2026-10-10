// FrameOrchestrator.cpp
// The frame spine, extracted from RenderServices (A1.4 S1). Owns the per-window
// RenderGraph stream slots, the app-frame epochs + declare sequence, the
// pipeline-asset lifecycle, and the background shader-package cache; sequences
// the stage-G schedulers, pipeline declaration, and MarkOutput across the
// RenderServices-owned subsystems it reaches via m_Services (accessors + the
// friend grant for the frame-local state that stays RS-resident).
#include "Engine/Rendering/FrameOrchestrator.h"
#include "Engine/Rendering/RenderServices.h"
#include "Core/CpuProfiler.h"
#include "Engine/Rendering/IRenderFeature.h"

#include "Core/DebugMetrics.h"
#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialBinder.h"
#include "Engine/Rendering/MaterialCompiler.h"
#include "Engine/Rendering/ShaderGraphMaterial.h"
#include "Engine/Rendering/SceneAccelerationStructureService.h"
#include "Rendering/Geometry/VertexLayoutBuilder.h"
#include "Rendering/Materials/MaterialBuildService.h"
#include "Rendering/Materials/MaterialParamsLayout.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Assets/AssetManager.h"
#include "Assets/BinaryAsset.h"
#include "Assets/MaterialAsset.h"
#include "Assets/ModelAsset.h"
#include "Assets/RenderPipelineAsset.h"
#include "Assets/TextureAsset.h"
#include "Engine/Rendering/EmbeddedImageDecoder.h"
#include "Core/Application.h"
#include "Core/Engine.h"
#include "Engine/Rendering/ShadowMapRenderFeature.h"
#include "Engine/Rendering/ImageBasedLightingFeature.h"
#include "Engine/Rendering/IEnvironmentSource.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "Engine/Rendering/RetargetRenderFeature.h"
#include "Engine/Rendering/Pipeline/Nodes/EngineNodeTypes.h"
#include "Engine/Rendering/Pipeline/PipelineResourceNames.h"
#include "Engine/Rendering/Pipeline/RenderPipeline.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Logger/Logger.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Core/BindlessResourceManager.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/DeviceFormatting.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/CameraDerivation.h"
#include "Rendering/Core/CullingStrategy.h"
#include "Rendering/Core/FrustumCullingStrategy.h"
#include "Rendering/Core/GPUCulling.h"
#include "Rendering/Core/GPUDrawStreamBuilder.h"
#include "Rendering/Core/GPUScene.h"
#include "Rendering/Materials/MaterialHelper.h"
#include "Rendering/Materials/MaterialBuilder.h"
#include "Rendering/Materials/ShaderReflection.h"
#include "Rendering/Common/Utils.h"
#include "Rendering/Passes/SRGBEncodePass.h"
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include "Rendering/Utils/BufferHelpers.h"
#include "Rendering/Utils/TextureUploadHelpers.h"
#include "Rendering/Utils/CubeLutFileParser.h"
#include "Rendering/Utils/CubeLutGpuUpload.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Types/StringId.h"
#include "Mathematics/Matrix4x4.h"
#include "Mathematics/MatrixOps.h"
#include "Rendering/Common/Frustum.h"
#include "Rendering/Common/MatrixUtils.h"

#include "Rendering/Core/ThreadingUtils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "RenderServicesDetail.h"

namespace GameEngine
{
namespace Engine::Renderer
{
using namespace ::GameEngine::Rendering;

FrameOrchestrator::FrameOrchestrator() = default;
FrameOrchestrator::~FrameOrchestrator() = default;

void FrameOrchestrator::Initialize(RenderServices& rs)
{
    // Glue back-pointer: the spine reaches RS-resident frame-local state and the
    // orchestrated subsystems through this (design 0a-A1).
    m_Services = &rs;

    // The data-driven render pipeline system: node registry + compiler. Engine
    // modules add their node types to the same registry from EngineCore's
    // rendering bring-up. Pipeline instances are created lazily per RenderGraph
    // in AcquirePipelineInstanceForGraph (a single RS can drive several graphs).
    m_PipelineNodeRegistry = std::make_unique<Pipeline::RenderPipelineNodeRegistry>();
    Pipeline::Nodes::RegisterEngineNodeTypes(*m_PipelineNodeRegistry);
    m_PipelineCompiler = std::make_unique<Pipeline::RenderPipelineCompiler>();

    if (m_ActiveRenderPipelinePath.empty())
    {
        // Default path under the asset root. Default assets are added later.
        m_ActiveRenderPipelinePath = std::filesystem::path(kEngineStandInPath);
    }

    // Optional override for dev/testing so we can quickly validate pipelines without Editor UI yet.
    // Accepts asset-root-relative paths like "RenderPipelines/ForwardPlus.rendergraph".
    if (const char* env = std::getenv("GE_ACTIVE_RENDER_PIPELINE"))
    {
        if (env[0] != '\0')
        {
            m_ActiveRenderPipelinePath = std::filesystem::path(env);
            Logger::Log::Info("RenderServices: using GE_ACTIVE_RENDER_PIPELINE='{}'", env);
        }
    }
    m_ActiveRenderPipelineHash = 0;
}

void FrameOrchestrator::Shutdown()
{
    // Tear down the data-driven pipeline before releasing the device so any
    // node-owned GPU objects (pipelines/samplers) can be destroyed safely.
    while (!m_RGStreams.empty())
        RemovePipelineInstanceForFrame(m_RGStreams.back().first);
    m_GlobalStagesFrame = nullptr;
    m_ActiveBlueprint.reset();
    m_ActiveRenderPipelineAsset.reset(); // drop the cached asset ref (S3)
    m_StandInAsset.reset();
    m_PipelineCompiler.reset();
    m_PipelineNodeRegistry.reset();
    m_ActiveRenderPipelineHash = 0;
    m_ActiveRenderPipelinePath.clear();
    // m_FrameRG is RS-resident (A1) and reset by RenderServices::Shutdown.
}

void FrameOrchestrator::BeginAppFrame()
{
    // Allow per-view culling to schedule again this frame. Multiple
    // BuildFrameGraph calls per frame (SceneView + GameView controllers)
    // collapse to one dispatch via this guard.
    m_Services->m_CullingScheduledThisFrame = false;
    m_Services->m_FrameRG = {}; // RenderGraph frame-local values die with their frame
    m_Services->m_ViewFrameRG.clear();
    // New app-frame epoch: stage G (skinning/culling/bucketer/union) re-arms,
    // and every stream slot's DeclaredEpoch stamp goes stale (per-stream
    // repeat guards re-open without touching the slots).
    ++m_WorldFrameEpoch;
    // Blackboard frame-pointer ABA hardening: a recycled RGFrame at the same
    // address must not look like "already declared" to FrameResourcesFor.
    for (auto& [framePtr, slot] : m_RGStreams)
        if (slot.Instance)
            slot.Instance->ResetFrameBlackboard();
}

void FrameOrchestrator::SetActiveRenderPipelinePath(const std::filesystem::path& relOrAbsPath)
{
    m_ActiveRenderPipelinePath = relOrAbsPath;
    // Force a recompile on next EnsureActiveRenderPipelineBuilt and clear sticky failure state.
    m_ActiveRenderPipelineHash = 0;
    m_ActiveRenderPipelineSourceHash = 0;
    m_ActiveRenderPipelineIsFallback = false;
    m_ActiveRenderPipelineLastCompileHadErrors = false;
    m_ActiveRenderPipelineRejection.clear();
    m_ActiveRenderPipelineLastFailurePath.clear();
    m_ActiveRenderPipelineLastFailureReason.clear();
    m_RequestedFailureReason.clear();
    m_WaitingForNativeModules = false;
    m_RequestWaitedForScripts = false;
    m_ActiveBlueprintIsPartial = false;
    m_ActiveBlueprintPinned = false; // a path request supersedes a pinned blueprint
    // Event-F6 hygiene (§0a-A10): the path→GUID resolution cache and the cached
    // asset ptr both refer to the OLD path — drop them so the next resolve
    // re-derives, and force the refresh on the next spine call.
    m_ActiveRenderPipelineLastResolvedPath.clear();
    m_ActiveRenderPipelineLastResolvedGuid = GameEngine::GUID{};
    m_ActiveRenderPipelineAsset.reset();
    m_BlueprintRefreshNeeded = true;

    Logger::Log::Info("RenderServices: requested active render pipeline '{}'", m_ActiveRenderPipelinePath.generic_string());
}

PipelineResolveResult FrameOrchestrator::ResolveActiveRenderPipelineNow(const std::filesystem::path& relOrAbsPath)
{
    SetActiveRenderPipelinePath(relOrAbsPath);
    // No stand-in and no failure log: the caller reports a failure, once.
    EnsureActiveRenderPipelineBlueprint(PipelineRefreshMode::Startup);

    // Success is the applied blueprint, not a resident asset: the path switch
    // above zeroed the hash, so a non-zero hash from a requested apply means
    // this refresh applied the requested pipeline.
    if (m_ActiveBlueprintIsRequested && m_ActiveRenderPipelineHash != 0)
    {
        // The refresh this request armed has run; the first frame must not redo it.
        m_BlueprintRefreshNeeded = false;
        return {};
    }
    const std::string pipeline = "render pipeline '" + m_ActiveRenderPipelinePath.generic_string() + "'";
    PipelineResolveResult result{PipelineResolveFailure::NotInMount, pipeline + " is in no asset mount"};
    if (!m_ActiveRenderPipelineRejection.empty())
    {
        // The last error sentence's period is the caller's: it ends the message.
        result = {PipelineResolveFailure::Rejected, pipeline + " was rejected:\n" + m_ActiveRenderPipelineRejection};
        result.Message.pop_back();
    }
    // A registry can still list a file no mount holds (a stale asset database),
    // and its load then fails with "asset returned null": that is a missing file.
    else if (!m_ActiveRenderPipelineLastFailureReason.empty() && RequestedPipelineFileExists())
        result = {PipelineResolveFailure::LoadFailed,
                  pipeline + " failed to load: " + m_ActiveRenderPipelineLastFailureReason};

    // The refresh stays armed and forgets this attempt: a host that renders
    // anyway gets the spine's ordinary refresh at its first frame, which
    // recompiles, logs the failure, applies the stand-in where there is one and
    // publishes the stand-in report.
    m_ActiveRenderPipelineSourceHash = 0;
    m_ActiveRenderPipelineLastFailurePath.clear();
    m_ActiveRenderPipelineLastFailureReason.clear();
    return result;
}

bool FrameOrchestrator::RequestedPipelineFileExists() const
{
    if (!GameEngine::EngineCore::GetInstance().IsInitialized())
        return false;
    const std::filesystem::path resolved =
        GameEngine::EngineCore::GetInstance().GetAssetManager().ResolveAssetPath(m_ActiveRenderPipelinePath);
    std::error_code ec;
    return !resolved.empty() && std::filesystem::is_regular_file(resolved, ec);
}

void FrameOrchestrator::SetActiveRenderPipelineBlueprint(Pipeline::RenderPipelineBlueprint bp)
{
    if (!m_ActiveBlueprint)
        m_ActiveBlueprint = std::make_unique<Pipeline::RenderPipelineBlueprint>();
    *m_ActiveBlueprint = std::move(bp);
    ++m_ActiveBlueprintGen;
    m_ActiveRenderPipelineHash = m_ActiveBlueprint->contentHash;
    m_ActiveRenderPipelineIsFallback = false;
    m_ActiveBlueprintPinned = true;
    m_ActiveBlueprintIsRequested = true;
    m_ActiveBlueprintIsPartial = false;
    m_RequestedFailureReason.clear();
    m_WaitingForNativeModules = false;
    UpdatePipelineStandIn({});
}

void FrameOrchestrator::InstallReloadInvalidator(AssetEventDispatcher& dispatcher)
{
    // The editor inspector saves a .rendergraph via a direct RenderPipelineAsset
    // ::Reload() — eventless — so the spine's cached-asset source-hash poll is
    // the primary detector. This subscriber is the redundant fast path for the
    // file-watcher-driven reload. ReloadedOnly: AssetReloaded is the only event
    // dispatched from the main thread (AssetManager::Update -> CheckForReloads),
    // and blueprint compile is strictly main-thread. The handler only flips the
    // flag; the recompile defers to the next spine call.
    m_PipelineReloadInvalidator = AssetReloadInvalidator(
        dispatcher, AssetType::RenderPipeline,
        [this](const GUID& guid)
        {
            if (guid == m_ActiveRenderPipelineLastResolvedGuid || m_ActiveRenderPipelineIsFallback)
                m_BlueprintRefreshNeeded = true;
        },
        AssetReloadInvalidator::EventSet::ReloadedOnly);
}

void FrameOrchestrator::ResetReloadInvalidator()
{
    m_PipelineReloadInvalidator.Reset();
}

void FrameOrchestrator::RemovePipelineInstanceForFrame(Rendering::RenderGraph::RGFrame* frame)
{
    if (!frame)
        return;
    for (auto it = m_RGStreams.begin(); it != m_RGStreams.end(); ++it)
    {
        if (it->first != frame)
            continue;
        // Pass closures (and their destructor thunks) may live in the same
        // module as the nodes. Retire them while both code and owners are alive.
        frame->DiscardRecordedPasses();
        m_RGStreams.erase(it);
        break;
    }
    // The dying frame must not linger as the stage-G owner or the declare
    // cursor: a future RGFrame at the recycled address would alias it.
    if (m_GlobalStagesFrame == frame)
        m_GlobalStagesFrame = nullptr;
    // Pointer-only reap (A6-i): a dying frame at a soon-to-be-recycled address
    // must be cleared regardless of index, so this compares the address alone
    // and must NOT route through IsFor — adding the index check would let a
    // stale entry survive into a recycled incarnation whose index coincides.
    if (m_Services->m_FrameRG.For.Frame == frame)
        m_Services->m_FrameRG = {};
    for (auto it = m_Services->m_ViewFrameRG.begin(); it != m_Services->m_ViewFrameRG.end();)
    {
        // Same pointer-only reap rationale (A6-i) for the per-view entries.
        if (it->second.For.Frame == frame)
            it = m_Services->m_ViewFrameRG.erase(it);
        else
            ++it;
    }
    // Same aliasing rationale for feature-owned readback pendings: an
    // unstamped pending keyed to this frame must not survive to be falsely
    // stamped by a recycled-address incarnation whose index coincides.
    m_Services->ForEachFeature([&](IRenderFeature& feature)
                               { feature.OnFrameStreamRetiredRG(*frame); });
}

namespace
{
bool IssuesEqual(const std::vector<Pipeline::PipelineIssue>& a,
                 const std::vector<Pipeline::PipelineIssue>& b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].severity != b[i].severity || a[i].message != b[i].message ||
            a[i].nodeId != b[i].nodeId)
            return false;
    return true;
}

// True when the compile failed and every error is a pass type no registered factory provides.
bool OnlyUnknownPassTypes(const Pipeline::RenderPipelineBlueprint& blueprint)
{
    bool anyError = false;
    for (const auto& issue : blueprint.issues)
    {
        if (issue.severity != Pipeline::PipelineIssueSeverity::Error)
            continue;
        if (issue.code != Pipeline::PipelineIssueCode::UnknownPassType)
            return false;
        anyError = true;
    }
    return anyError;
}

// True while a native module may still load and register pipeline node types.
bool NativeModulesPending()
{
    const EngineCore& engine = EngineCore::GetInstance();
    return engine.IsInitialized() && engine.AreNativeModulesPending();
}

// True when a native module that ships with the game failed its latest build.
bool NativeModuleBuildFailed()
{
    const EngineCore& engine = EngineCore::GetInstance();
    return engine.IsInitialized() && engine.HasFailedNativeModuleBuild();
}

// An issue as a sentence for the user: without the compiler's class-name prefix, capitalized,
// ending in a period, and naming its node once (when the message does not already).
std::string IssueSentence(const Pipeline::PipelineIssue& issue)
{
    constexpr std::string_view kCompilerPrefix = "RenderPipelineCompiler: ";
    std::string message = issue.message;
    if (message.rfind(kCompilerPrefix, 0) == 0)
        message.erase(0, kCompilerPrefix.size());
    while (!message.empty() && (message.back() == '.' || message.back() == ' '))
        message.pop_back();
    if (!issue.nodeId.empty() && message.find("'" + issue.nodeId + "'") == std::string::npos)
        message += " (node '" + issue.nodeId + "')";
    if (!message.empty())
        message[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(message[0])));
    return message + ".";
}

// The compile's first error as a sentence for the user; empty when the compile has none.
std::string FirstErrorMessage(const Pipeline::RenderPipelineBlueprint& blueprint)
{
    for (const auto& issue : blueprint.issues)
    {
        if (issue.severity == Pipeline::PipelineIssueSeverity::Error)
            return IssueSentence(issue);
    }
    return {};
}

// The ids of the passes a compile left out with issue `code`, in pipeline order: the script passes
// whose types the project's scripts, still building, may register (PendingScriptPass), or the
// engine passes that wait with them because they read their output (PendingScriptReader).
std::vector<std::string> PassesLeftOut(const Pipeline::RenderPipelineBlueprint& blueprint, Pipeline::PipelineIssueCode code)
{
    std::vector<std::string> passes;
    for (const auto& issue : blueprint.issues)
    {
        if (issue.code == code)
            passes.push_back(issue.nodeId);
    }
    return passes;
}

// The names, quoted and separated by commas.
std::string QuotedList(const std::vector<std::string>& names)
{
    std::string list;
    for (const std::string& name : names)
        list += (list.empty() ? "'" : ", '") + name + "'";
    return list;
}

// True when the pipeline's output comes from a pass left out for the project's scripts.
bool OutputWaitsForScripts(const Pipeline::RenderPipelineBlueprint& blueprint)
{
    return std::any_of(blueprint.issues.begin(), blueprint.issues.end(), [](const Pipeline::PipelineIssue& issue)
                       { return issue.code == Pipeline::PipelineIssueCode::PendingScriptOutput; });
}

// Why a request waits, as a sentence: the script passes, and the passes that read their output,
// that join once the project's scripts are built. Nothing needs fixing meanwhile, so it carries no fix.
std::string WaitingReason(const std::vector<std::string>& scriptPasses, const std::vector<std::string>& readers)
{
    const bool oneScriptPass = scriptPasses.size() == 1;
    std::string reason = (oneScriptPass ? "Its script pass " : "Its script passes ") + QuotedList(scriptPasses);
    if (!readers.empty())
        reason += ", and " + QuotedList(readers) + (readers.size() == 1 ? " that reads " : " that read ") +
                  (oneScriptPass ? "its" : "their") + " output,";
    return reason + (scriptPasses.size() + readers.size() == 1 ? " joins" : " join") +
           " once the project's scripts are built.";
}

// An unknown pass type's sentence up to the compiler's fix (": no loaded scripts register it; build
// the scripts or install the package ..."), without it and without the closing period; any other
// sentence without its closing period. The one trim of that fix: the startup error list and
// FrameOrchestrator::ScriptsBuiltWithoutType replace it with their own.
std::string WithoutScriptsFix(const std::string& sentence)
{
    const std::size_t fix = sentence.find(": no loaded scripts register it");
    return fix == std::string::npos ? sentence.substr(0, sentence.size() - 1) : sentence.substr(0, fix);
}

// Every compile error as a sentence (IssueSentence), one per line; empty for an accepted compile.
// An unknown pass type's sentence drops the compiler's fix, which is the editor's (a startup host
// builds no scripts); the caller gives its own.
std::string ErrorSentences(const Pipeline::RenderPipelineBlueprint& blueprint)
{
    std::string text;
    for (const auto& issue : blueprint.issues)
    {
        if (issue.severity != Pipeline::PipelineIssueSeverity::Error)
            continue;
        if (!text.empty())
            text += '\n';
        const bool unknownType = issue.code == Pipeline::PipelineIssueCode::UnknownPassType;
        text += unknownType ? WithoutScriptsFix(IssueSentence(issue)) + "." : IssueSentence(issue);
    }
    return text;
}

// The asset's own load errors as one clause ("JSON parse failed (malformed
// JSON); ..."): the type prefix and the closing period dropped, so callers can
// end the sentence. A generic clause when the asset recorded none.
std::string DescribeLoadErrors(const std::vector<std::string>& errors)
{
    constexpr std::string_view kAssetPrefix = "RenderPipelineAsset: ";
    std::string text;
    for (std::string_view error : errors)
    {
        if (error.starts_with(kAssetPrefix))
            error.remove_prefix(kAssetPrefix.size());
        while (!error.empty() && error.back() == '.')
            error.remove_suffix(1);
        if (error.empty())
            continue;
        if (!text.empty())
            text += "; ";
        text += error;
    }
    return text.empty() ? std::string("it did not parse") : text;
}
} // namespace

void PipelineCompileReport::RecordCompile(const Pipeline::RenderPipelineBlueprint& blueprint,
                                          std::filesystem::path sourcePath, uint64_t sourceHash,
                                          bool fallback, bool awaitsModules)
{
    const bool rejected = blueprint.HasErrors();
    // Idempotent: a transient fallback recompiles the same blueprint every frame;
    // don't advance Generation (the panel's refresh key) unless the outcome moved.
    // A rejection recorded while it awaited modules was not logged, so the same outcome recorded
    // once the wait is over is new.
    if (Generation != 0 && SourcePath == sourcePath && SourceHash == sourceHash &&
        Rejected == rejected && FallbackCompile == fallback && LoadFailureReason.empty() &&
        IssuesEqual(Issues, blueprint.issues) && m_RecordedAwaitingModules == awaitsModules)
        return;
    m_RecordedAwaitingModules = awaitsModules;

    SourcePath = std::move(sourcePath);
    SourceHash = sourceHash;
    Rejected = rejected;
    FallbackCompile = fallback;
    Issues = blueprint.issues;
    LoadFailureReason.clear();
    ++Generation;

    // A pass the compile moved above the depth prepass is logged as well as listed in the panel: a Player has
    // no panel, and the graph should be fixed where it is authored. Once per graph and move.
    if (!Rejected)
    {
        const std::string graph = SourcePath.generic_string();
        std::vector<std::string>& loggedMoves = m_LoggedFeederMoves[graph];
        std::vector<std::string> moves;
        for (const auto& issue : Issues)
        {
            if (issue.code != Pipeline::PipelineIssueCode::DepthPrepassFeederMoved)
                continue;
            if (std::find(loggedMoves.begin(), loggedMoves.end(), issue.message) == loggedMoves.end())
                Logger::Log::Warning("RenderPipeline '{}': {}", graph, issue.message);
            moves.push_back(issue.message);
        }
        loggedMoves = std::move(moves);
    }

    LoggedRejection& logged = FallbackCompile ? m_LoggedFallbackRejection : m_LoggedRequestedRejection;
    if (!Rejected)
    {
        logged = {};
        // A request that compiles ends the episode the fallback stood in for.
        if (!FallbackCompile)
            m_LoggedFallbackRejection = {};
        return;
    }
    if (awaitsModules)
        return;
    if (logged.SourcePath == SourcePath && logged.SourceHash == SourceHash && IssuesEqual(logged.Issues, Issues))
        return;
    logged = {SourcePath, SourceHash, Issues};
    for (const auto& issue : Issues)
    {
        if (issue.severity == Pipeline::PipelineIssueSeverity::Error)
            Logger::Log::Error("RenderPipeline '{}' was rejected{}: {}",
                               SourcePath.generic_string(),
                               FallbackCompile ? " as a fallback" : "",
                               IssueSentence(issue));
    }
}

void PipelineCompileReport::RecordLoadFailure(std::filesystem::path sourcePath, std::string reason)
{
    if (Generation != 0 && SourcePath == sourcePath && LoadFailureReason == reason &&
        !Rejected && !FallbackCompile && Issues.empty())
        return;

    SourcePath = std::move(sourcePath);
    SourceHash = 0;
    // A load failure is not a compile rejection; the banner keys on the reason
    // string, and Issues stay empty (no compile ran to produce PipelineIssues).
    Rejected = false;
    FallbackCompile = false;
    Issues.clear();
    LoadFailureReason = std::move(reason);
    ++Generation;
    m_LoggedRequestedRejection = {};
}

void FrameOrchestrator::EnsureActiveRenderPipelineBlueprint(PipelineRefreshMode mode)
{
    // Startup returns its failure to the caller, which reports it once.
    const bool reportFailures = mode == PipelineRefreshMode::Frame;
    // A pinned programmatic blueprint owns the slot — the asset refresh
    // below would replace it every call (its hash never matches a
    // loadable source). SetActiveRenderPipelinePath unpins.
    if (m_ActiveBlueprintPinned)
        return;

    // Attempt to load and compile the pipeline asset. If unavailable or invalid, keep the currently
    // applied pipeline (or, before any has applied, stand the engine's own pipeline in).
    //
    // Important behavior:
    // - We do NOT recompile every frame when the pipeline source is unchanged but invalid.
    // - We avoid logging the same failure every frame.
    // - If a transient fallback is currently applied (the requested asset
    //   wasn't available at the time of last attempt), force a re-attempt of
    //   the requested path so we can take over once the registry indexes it.
    //   A request its compile refused is compiled again only when its source
    //   changes or a node type registers, not on every refresh.
    if (m_ActiveRenderPipelineIsFallback && !m_ActiveRenderPipelineLastCompileHadErrors)
    {
        m_ActiveRenderPipelineSourceHash = 0;
    }

    Pipeline::RenderPipelineBlueprint bp{};
    bool wantSetBlueprint = false;
    uint64_t desiredBlueprintHash = m_ActiveRenderPipelineHash;
    bool loadSuppressed = false;
    bool applyingRequested = false;

    std::filesystem::path path = m_ActiveRenderPipelinePath;
    if (path.empty())
    {
        path = std::filesystem::path(kEngineStandInPath);
    }

    // A default-constructed EngineCore (headless tests, DLL bootstrap) has no
    // AssetManager — skip the lookup entirely; such a spine draws only a
    // blueprint set with SetActiveRenderPipelineBlueprint.
    if (GameEngine::EngineCore::GetInstance().IsInitialized())
    {
        GE_CPU_PROFILE_SCOPE("RenderServices.EnsureActiveRenderPipelineBuilt.AssetLookup");
        try
        {
            using GameEngine::Asset;
            using GameEngine::GUID;
            using GameEngine::SharedPtr;

            auto& am = GameEngine::EngineCore::GetInstance().GetAssetManager();
            auto resolvePipelineGuid = [&am](const std::filesystem::path& pipelinePath) -> GUID
            {
                const std::filesystem::path abs = am.ResolveAssetPath(pipelinePath);
                if (abs.empty())
                    return GUID::Null();

                // ResolveAssetPath can return the project candidate for a
                // missing relative path. Avoid hammering RegisterAsset with a
                // non-existent file while fallback retry is active, but still
                // use ResolveAssetGuid for real files so staged editor assets
                // self-register when the startup scan missed them.
                std::error_code ec;
                if (!std::filesystem::exists(abs, ec) || std::filesystem::is_directory(abs, ec))
                    return am.GetRegistry().GetAssetGUID(abs);

                GUID guid = am.ResolveAssetGuid(pipelinePath);
                if (guid.IsNull())
                    guid = am.GetRegistry().GetAssetGUID(abs);
                return guid;
            };

            // Path resolution + GUID lookup hits the registry hashmap each
            // frame in steady state. Skip when the path string hasn't
            // changed since the last successful resolve.
            GUID guid;
            if (path == m_ActiveRenderPipelineLastResolvedPath && !m_ActiveRenderPipelineLastResolvedGuid.IsNull())
            {
                guid = m_ActiveRenderPipelineLastResolvedGuid;
            }
            else
            {
                guid = resolvePipelineGuid(path);
                if (!guid.IsNull())
                {
                    m_ActiveRenderPipelineLastResolvedPath = path;
                    m_ActiveRenderPipelineLastResolvedGuid = guid;
                }
                else
                {
                    // Failed lookup — clear cache so next attempt re-resolves.
                    m_ActiveRenderPipelineLastResolvedPath.clear();
                    m_ActiveRenderPipelineLastResolvedGuid = GUID{};
                    m_ActiveRenderPipelineAsset.reset(); // resolve failed — drop stale cache
                }
            }
            if (!guid.IsNull())
            {
                // If AssetManager has already suppressed this GUID (unsupported or repeatedly failing),
                // don't hammer it every frame. Log once when the failure state changes.
                String suppressedReason;
                if (am.IsLoadSuppressed(guid, &suppressedReason))
                {
                    if (m_ActiveRenderPipelineLastFailurePath != path ||
                        m_ActiveRenderPipelineLastFailureReason != suppressedReason)
                    {
                        m_ActiveRenderPipelineLastFailurePath = path;
                        m_ActiveRenderPipelineLastFailureReason = suppressedReason;
                        if (reportFailures)
                            Logger::Log::Error(
                                "RenderPipeline '{}' load is suppressed: {}. Keeping current pipeline.",
                                path.string(),
                                suppressedReason);
                    }
                    loadSuppressed = true;
                    m_RequestedFailureReason = "Its last load failed (" + suppressedReason +
                                               "), and it is not loaded again until the file changes; fix the "
                                               "file and save it.";
                    m_WaitingForNativeModules = false;
                }
                if (loadSuppressed)
                {
                    // Intentionally skip any load/compile attempt while suppressed.
                    // AssetManager will clear suppression when the file changes.
                }
                else
                {
                    SharedPtr<Asset> a = am.GetAsset(guid);
                    if (!a)
                    {
                        a = am.LoadAssetAsync(guid).get();
                    }

                    auto rp = std::dynamic_pointer_cast<GameEngine::RenderPipelineAsset>(a);
                    if (rp && rp->IsLoaded())
                    {
                        // Cache the resolved asset so the spine's steady-state
                        // refresh check is a single GetSourceHash() member read on
                        // it — no per-frame GetAsset/cast/path-resolution (§0a-A10).
                        // Reload is in-place, so this same object reflects
                        // hot-reloads; captured in BOTH the unchanged and the
                        // recompile sub-branches (a compile error still leaves the
                        // requested asset resident and pollable).
                        m_ActiveRenderPipelineAsset = rp;
                        // Source hash is precomputed during Load() / Reload() in
                        // RenderPipelineAsset, so this is a member read rather than
                        // a per-frame sweep over the JSON text. Hot-reload runs
                        // through ParseFromText which recomputes the hash, so a
                        // new value here triggers the recompile path.
                        const uint64_t sourceHash = rp->GetSourceHash();
                        if (sourceHash != 0 && sourceHash == m_ActiveRenderPipelineSourceHash)
                        {
                            // Source unchanged since last attempt. If it failed last time, do nothing (keep current pipeline).
                            // If it succeeded last time, do nothing (pipeline already applied).
                        }
                        else
                        {
                            // While the project's scripts build, a pass whose type they may register
                            // waits instead of refusing the graph: the rest draws now. A Startup
                            // refresh does not wait: it refuses the type, so the resolve never
                            // applies a pipeline with passes missing.
                            const bool waitForScripts = mode == PipelineRefreshMode::Frame && NativeModulesPending();
                            bp = m_PipelineCompiler->Compile(*rp, *m_PipelineNodeRegistry,
                                m_Services->GetDevice()->PreferredShaderSource(),
                                waitForScripts ? Pipeline::UnknownPassTypes::PendingScripts
                                               : Pipeline::UnknownPassTypes::Refused);
                            ++m_LastCompileReport.Compiles;

                            // Record the source hash even on failure so we don't try again every frame.
                            m_ActiveRenderPipelineSourceHash = sourceHash;
                            // A graph whose output comes from a waiting pass draws nothing: it is not applied.
                            m_ActiveRenderPipelineLastCompileHadErrors = bp.HasErrors() || OutputWaitsForScripts(bp);
                            m_ActiveRenderPipelineRejection = ErrorSentences(bp);
                            // Retain diagnostics for the editor panel — both outcomes: an
                            // accepted compile keeps its Warnings, a rejected one records
                            // Rejected=true (the guard below leaves the last-good active).
                            m_PendingScriptPasses = PassesLeftOut(bp, Pipeline::PipelineIssueCode::PendingScriptPass);
                            m_WaitingReaders = PassesLeftOut(bp, Pipeline::PipelineIssueCode::PendingScriptReader);
                            // A real error refuses the request now: waiting would hide it and promise a
                            // replacement that cannot come.
                            m_WaitingForNativeModules = !m_PendingScriptPasses.empty() && !bp.HasErrors();
                            if (reportFailures || !m_ActiveRenderPipelineLastCompileHadErrors)
                                m_LastCompileReport.RecordCompile(bp, path, sourceHash, /*fallback=*/false,
                                                                  m_WaitingForNativeModules);
                            m_RequestWaitedForScripts = m_RequestWaitedForScripts || m_WaitingForNativeModules;
                            m_RequestedFailureReason = m_WaitingForNativeModules
                                                           ? WaitingReason(m_PendingScriptPasses, m_WaitingReaders)
                                                           : FirstErrorMessage(bp);
                            if (m_ActiveRenderPipelineLastCompileHadErrors && OnlyUnknownPassTypes(bp) &&
                                NativeModuleBuildFailed())
                                m_RequestedFailureReason = PointAtFailedBuild(m_RequestedFailureReason);
                            else if (m_ActiveRenderPipelineLastCompileHadErrors && OnlyUnknownPassTypes(bp) &&
                                     m_RequestWaitedForScripts && !m_WaitingForNativeModules)
                                m_RequestedFailureReason = ScriptsBuiltWithoutType(m_RequestedFailureReason);

                            if (!m_ActiveRenderPipelineLastCompileHadErrors)
                            {
                                desiredBlueprintHash = bp.contentHash;
                                if (desiredBlueprintHash != m_ActiveRenderPipelineHash)
                                {
                                    wantSetBlueprint = true;
                                    applyingRequested = true;
                                }
                            }
                        }
                    }
                    else if (!a)
                    {
                        // The cached `path → GUID` mapping can go stale on
                        // project switch: the path string is unchanged
                        // ("RenderPipelines/ForwardPlus.rendergraph"), but
                        // the new project's registry has a different GUID
                        // for that path. Bypassing re-resolution loads the
                        // old project's GUID, which the new AssetManager
                        // can't find. Clear the cache so the next frame
                        // re-resolves through the registry — self-heals
                        // without coupling to a project-switch event.
                        m_ActiveRenderPipelineLastResolvedPath.clear();
                        m_ActiveRenderPipelineLastResolvedGuid = GUID{};
                        m_ActiveRenderPipelineSourceHash = 0;
                        m_ActiveRenderPipelineAsset.reset(); // asset gone — drop stale cache

                        // A failed first load returns no asset; the asset manager
                        // keeps why (the decode error, which carries a pipeline's
                        // parse errors) as the GUID's load-suppression reason.
                        String reason;
                        const bool decodeFailed = am.IsLoadSuppressed(guid, &reason) && !reason.empty();
                        if (!decodeFailed)
                            reason = "asset returned null";
                        m_RequestedFailureReason = std::string("The file could not be loaded") +
                                                   (decodeFailed ? " (" + reason + ")" : std::string()) +
                                                   "; check that it is in the project and is a render pipeline "
                                                   "(.rendergraph).";
                        m_WaitingForNativeModules = false;
                        if (m_ActiveRenderPipelineLastFailurePath != path ||
                            m_ActiveRenderPipelineLastFailureReason != reason)
                        {
                            m_ActiveRenderPipelineLastFailurePath = path;
                            m_ActiveRenderPipelineLastFailureReason = reason;
                            // Load failures produce no PipelineIssues — surface the reason
                            // as the panel's load-failure banner (the pipeline stays last-good).
                            m_LastCompileReport.RecordLoadFailure(path, reason);
                            if (reportFailures)
                                Logger::Log::Error("RenderPipeline '{}' failed to load ({}); keeping current pipeline.",
                                                   path.string(), reason);
                        }
                    }
                    else if (rp && !rp->IsLoaded())
                    {
                        // A resident asset whose in-place reload did not parse
                        // (Asset::ReloadFromData unloads, then LoadFromData fails,
                        // leaving it Failed); the asset holds the errors. A first
                        // load that fails returns no asset: the branch above.
                        m_ActiveRenderPipelineAsset.reset(); // parse failed — drop stale cache
                        const String reason = DescribeLoadErrors(rp->GetErrors());
                        m_RequestedFailureReason = "The file could not be parsed (" + reason +
                                                   "); check that it is valid JSON.";
                        m_WaitingForNativeModules = false;
                        if (m_ActiveRenderPipelineLastFailurePath != path ||
                            m_ActiveRenderPipelineLastFailureReason != reason)
                        {
                            m_ActiveRenderPipelineLastFailurePath = path;
                            m_ActiveRenderPipelineLastFailureReason = reason;
                            // Surface the load-failure banner.
                            m_LastCompileReport.RecordLoadFailure(path, reason);
                            if (reportFailures)
                                Logger::Log::Error("RenderPipeline '{}' failed to load ({}); keeping current pipeline.",
                                                   path.string(), reason);
                        }
                    }
                }
            }
        }
        catch (const std::exception& e)
        {
            // For exceptions, log once per (path, message) pair.
            const String reason = e.what();
            m_RequestedFailureReason = "Loading or compiling it failed with an error (" + reason +
                                       "); the console has the details.";
            m_WaitingForNativeModules = false;
            if (m_ActiveRenderPipelineLastFailurePath != path ||
                m_ActiveRenderPipelineLastFailureReason != reason)
            {
                m_ActiveRenderPipelineLastFailurePath = path;
                m_ActiveRenderPipelineLastFailureReason = reason;
                if (reportFailures)
                    Logger::Log::Critical("RenderPipeline '{}' threw during load/compile: {}. Keeping current pipeline.",
                                          path.string(),
                                          e.what());
            }
        }
        catch (...)
        {
            const String reason = "unknown exception";
            m_RequestedFailureReason = "Loading or compiling it failed with an unknown error; the console has the "
                                       "details.";
            m_WaitingForNativeModules = false;
            if (m_ActiveRenderPipelineLastFailurePath != path ||
                m_ActiveRenderPipelineLastFailureReason != reason)
            {
                m_ActiveRenderPipelineLastFailurePath = path;
                m_ActiveRenderPipelineLastFailureReason = reason;
                if (reportFailures)
                    Logger::Log::Critical(
                        "RenderPipeline '{}' threw during load/compile (unknown exception). Keeping current pipeline.",
                        path.string());
            }
        }
    }

    // Until the requested pipeline has applied (or while a stand-in covers it), the engine's own
    // pipeline stands in so the views draw; the requested path is preserved on
    // m_ActiveRenderPipelinePath and re-attempted on later frames through the
    // m_ActiveRenderPipelineIsFallback retry above. A Startup refresh applies nothing but the request.
    // A request drawing without its waiting script passes is not a last good version: when it is
    // refused, the stand-in replaces it too.
    bool applyingFallback = false;
    if (mode == PipelineRefreshMode::Frame && !wantSetBlueprint &&
        (m_ActiveRenderPipelineHash == 0 || m_ActiveRenderPipelineIsFallback ||
         (m_ActiveBlueprintIsPartial && m_ActiveRenderPipelineLastCompileHadErrors)))
    {
        std::string standInFailure;
        const StandInCompile standIn = CompileEngineStandIn(bp, standInFailure);
        if (standIn == StandInCompile::AlreadyApplied)
        {
            applyingFallback = true;
        }
        else if (standIn == StandInCompile::Compiled)
        {
            m_StandInFailureReason.clear();
            desiredBlueprintHash = bp.contentHash;
            applyingFallback = true;
            // Only re-apply if the stand-in would actually change. Otherwise we'd blow away
            // m_WorldPassByView every frame while waiting for the requested pipeline to load,
            // which thrashes thumbnail views and other persistent passes.
            if (desiredBlueprintHash != m_ActiveRenderPipelineHash)
            {
                wantSetBlueprint = true;
                // A refusal already has its error line; this one says what a packaged game does instead.
                if (!m_RequestedFailureReason.empty())
                    Logger::Log::Info("RenderServices: the engine's '{}' stands in for '{}'; a packaged game has no "
                                      "stand-in and draws nothing in its place.",
                                      kEngineStandInPath, path.generic_string());
                // A request that is only not loaded yet gets this.
                else if (m_ActiveRenderPipelineHash == 0)
                {
                    Logger::Log::Warning("RenderServices: requested pipeline '{}' not yet available; the engine's "
                                         "'{}' draws until it loads.",
                                         path.generic_string(), kEngineStandInPath);
                }
            }
        }
        else if (standIn == StandInCompile::NoStandIn)
        {
            // The request's own load or compile failure is the one error line.
            m_StandInFailureReason = std::move(standInFailure);
        }
        else if (standInFailure != m_StandInFailureReason)
        {
            m_StandInFailureReason = std::move(standInFailure);
            Logger::Log::Error("RenderServices: the engine's render pipeline '{}' cannot stand in for '{}', so "
                               "nothing is drawn: {}",
                               kEngineStandInPath, path.generic_string(), m_StandInFailureReason);
        }
    }

    if (wantSetBlueprint)
    {
        // Log once per successful apply. This is a high-signal breadcrumb when debugging whether
        // the Editor is actually using the selected pipeline.
        if (!bp.pipelineName.empty())
        {
            Logger::Log::Info("RenderServices: applying pipeline '{}' (passes={}, outputs={})",
                              bp.pipelineName,
                              static_cast<uint32_t>(bp.passes.size()),
                              static_cast<uint32_t>(bp.outputs.size()));
        }
        // Store as the active blueprint and bump the generation. Each per-graph instance
        // applies it lazily on its next build (see the apply step before EnsureBuilt),
        // so a freshly-created instance for a new graph still picks up the current pipeline.
        if (!m_ActiveBlueprint)
            m_ActiveBlueprint = std::make_unique<Pipeline::RenderPipelineBlueprint>();
        *m_ActiveBlueprint = std::move(bp);
        ++m_ActiveBlueprintGen;
        m_ActiveRenderPipelineHash = desiredBlueprintHash;
        m_ActiveRenderPipelineIsFallback = applyingFallback;
        m_ActiveBlueprintIsRequested = applyingRequested;
        m_ActiveBlueprintIsPartial = applyingRequested && m_WaitingForNativeModules;

        // The active blueprint just swapped: drain in-flight PSO prewarm before
        // the old nodes are destroyed by SetBlueprint/EnsureBuilt (workers hold
        // PipelineDesc copies whose debugName pointers reference the old nodes'
        // string storage) and drop stale per-material variant entries. Both are
        // material-cache concerns, so they live behind the facade now (A1.3).
        m_Services->Materials().OnActiveRenderPipelineChanged();
    }

    // A Startup failure publishes no report: nothing stands in, its caller reports the failure, and a
    // host that renders anyway publishes it from the first frame's refresh.
    if (reportFailures || applyingRequested)
        UpdatePipelineStandIn(path);
}

std::string FrameOrchestrator::ScriptsBuiltWithoutType(const std::string& reason)
{
    return WithoutScriptsFix(reason) + ": the project's scripts built without registering it; check the script "
                                       "that should register it, install the package that provides it, or remove "
                                       "the node.";
}

std::string FrameOrchestrator::PointAtFailedBuild(const std::string& reason)
{
    const std::size_t fix = reason.find("; build the scripts or install the package");
    const std::string head = fix == std::string::npos ? reason.substr(0, reason.size() - 1) : reason.substr(0, fix);
    return head + ", and a script failed to build; fix its errors in the Script Errors tab.";
}

FrameOrchestrator::StandInCompile FrameOrchestrator::CompileEngineStandIn(Pipeline::RenderPipelineBlueprint& out,
                                                                          std::string& failure)
{
    // The stand-in applied already, from an unchanged source: nothing to resolve or compile.
    if (m_ActiveRenderPipelineIsFallback && m_StandInAsset && m_StandInAsset->GetSourceHash() == m_StandInSourceHash)
        return StandInCompile::AlreadyApplied;
    if (!GameEngine::EngineCore::GetInstance().IsInitialized())
    {
        failure = "The engine has no asset manager to load its pipeline from.";
        return StandInCompile::Failed;
    }
    try
    {
        auto& am = GameEngine::EngineCore::GetInstance().GetAssetManager();
        const std::filesystem::path relative(kEngineStandInPath);
        // Only the 'editor' mount holds the stand-in, so a project file of the same name never
        // stands in. A packaged game has no such mount and no stand-in.
        if (am.GetSourceRoot(kAssetSourceAliasEditor).empty())
        {
            failure = "A packaged game has no stand-in pipeline.";
            return StandInCompile::NoStandIn;
        }
        const std::filesystem::path absolute = am.ResolveAssetPath(relative, kAssetSourceAliasEditor);
        std::error_code ec;
        if (absolute.empty() || !std::filesystem::is_regular_file(absolute, ec))
        {
            failure = "The engine's pipeline file is missing (" +
                      (absolute.empty() ? relative.generic_string() : absolute.generic_string()) + ").";
            return StandInCompile::Failed;
        }
        GameEngine::GUID guid = am.ResolveAssetGuid(relative, kAssetSourceAliasEditor);
        if (guid.IsNull())
            guid = am.GetRegistry().GetAssetGUID(absolute);
        if (guid.IsNull())
        {
            failure = "The asset registry has no entry for the engine's pipeline (" + absolute.generic_string() + ").";
            return StandInCompile::Failed;
        }
        if (guid == m_ActiveRenderPipelineLastResolvedGuid)
        {
            failure = "The engine's pipeline is the requested pipeline itself.";
            return StandInCompile::Failed;
        }
        SharedPtr<Asset> asset = am.GetAsset(guid);
        if (!asset)
            asset = am.LoadAssetAsync(guid).get();
        auto pipeline = std::dynamic_pointer_cast<GameEngine::RenderPipelineAsset>(asset);
        if (!pipeline || !pipeline->IsLoaded())
        {
            failure = "The engine's pipeline file could not be loaded or parsed (" + absolute.generic_string() + ").";
            return StandInCompile::Failed;
        }
        m_StandInAsset = pipeline;
        m_StandInSourceHash = pipeline->GetSourceHash();
        out = m_PipelineCompiler->Compile(*pipeline, *m_PipelineNodeRegistry,
                                          m_Services->GetDevice()->PreferredShaderSource());
        ++m_LastCompileReport.Compiles;
        if (out.HasErrors())
        {
            // A refused stand-in is reported and logged like a refused request (FallbackCompile marks
            // it); one that compiles leaves the report on the request's own diagnostic.
            m_LastCompileReport.RecordCompile(out, relative, pipeline->GetSourceHash(), /*fallback=*/true,
                                              /*awaitsModules=*/false);
            failure = "The engine's pipeline was refused: " + FirstErrorMessage(out);
            return StandInCompile::Failed;
        }
        return StandInCompile::Compiled;
    }
    catch (const std::exception& e)
    {
        failure = std::string("Loading or compiling the engine's pipeline failed with an error (") + e.what() + ").";
    }
    catch (...)
    {
        failure = "Loading or compiling the engine's pipeline failed with an unknown error.";
    }
    return StandInCompile::Failed;
}

void FrameOrchestrator::UpdatePipelineStandIn(const std::filesystem::path& requestedPath)
{
    PipelineStandIn next{};
    if (m_WaitingForNativeModules || (!m_ActiveBlueprintIsRequested && !m_RequestedFailureReason.empty()))
    {
        next.RequestedPath = requestedPath;
        next.WaitingForModules = m_WaitingForNativeModules;
        if (m_WaitingForNativeModules)
        {
            next.PendingPasses = m_PendingScriptPasses;
            next.WaitingReaders = m_WaitingReaders;
            next.LastVersionDraws = m_ActiveBlueprintIsRequested && !m_ActiveBlueprintIsPartial;
        }
        next.Reason = m_RequestedFailureReason;
        // A wait over the request's last compiled version names neither: that version draws on.
        if (m_ActiveBlueprint && m_ActiveRenderPipelineIsFallback)
            next.StandInName = m_ActiveBlueprint->pipelineName;
        else if (!m_ActiveBlueprintIsRequested)
            next.StandInFailure = m_StandInFailureReason;
    }
    if (next.RequestedPath == m_PipelineStandIn.RequestedPath && next.StandInName == m_PipelineStandIn.StandInName &&
        next.Reason == m_PipelineStandIn.Reason && next.StandInFailure == m_PipelineStandIn.StandInFailure &&
        next.WaitingForModules == m_PipelineStandIn.WaitingForModules &&
        next.PendingPasses == m_PipelineStandIn.PendingPasses && next.WaitingReaders == m_PipelineStandIn.WaitingReaders &&
        next.LastVersionDraws == m_PipelineStandIn.LastVersionDraws)
        return;

    // A refusal's one error line is the failed load's or compile's own; a wait gets one info line.
    next.Generation = m_PipelineStandIn.Generation + 1;
    if (next.WaitingForModules)
    {
        Logger::Log::Info("RenderPipeline '{}' waits for the project's scripts to build: {}",
                          next.RequestedPath.generic_string(), next.Reason);
    }
    m_PipelineStandIn = std::move(next);
}

FrameOrchestrator::RGFrameStreamSlot* FrameOrchestrator::FindOrCreateRGStream(
    Rendering::RenderGraph::RGFrame& frame)
{
    for (auto& [framePtr, slot] : m_RGStreams)
        if (framePtr == &frame)
            return &slot;
    if (m_RGStreams.size() >= kMaxRGFrameStreams)
    {
        // Evict a slot that did NOT declare this app frame: a live slot's
        // pipeline instance may still be captured by a declared-but-
        // unexecuted frame's record callbacks (destroying it is a UAF at
        // Execute), and its outputs would silently die for this frame.
        auto victim = m_RGStreams.begin();
        for (auto it = m_RGStreams.begin(); it != m_RGStreams.end(); ++it)
        {
            if (it->second.DeclaredEpoch != m_WorldFrameEpoch)
            {
                victim = it;
                break;
            }
        }
        if (victim->second.DeclaredEpoch == m_WorldFrameEpoch)
            Logger::Log::Error(
                "RenderServices: evicting a SAME-FRAME RenderGraph stream slot ({} live streams in one "
                "app frame exceeds kMaxRGFrameStreams) — the evicted window's output dies this "
                "frame; its declared work is discarded before its pipeline instance is destroyed.",
                m_RGStreams.size());
        // Evicted inactive streams may still retain last-frame DLL callbacks.
        // Use the same retirement boundary as explicit close and module reload.
        RemovePipelineInstanceForFrame(victim->first);
    }
    m_RGStreams.emplace_back(&frame, RGFrameStreamSlot{});
    return &m_RGStreams.back().second;
}

void FrameOrchestrator::PublishRenderTimelineStats(double bucketerScheduleMs)
{
    if (!m_Services)
        return;

    RenderServices::RenderTimelineStats& stats = m_Services->m_RenderTimelineStats;
    stats.BucketerScheduleMs = bucketerScheduleMs;

    m_TimelineWindow[m_TimelineWindowHead] =
        RenderTimelineSample{stats.SortMs, stats.BucketerScheduleMs};
    m_TimelineWindowHead = (m_TimelineWindowHead + 1) % kRenderTimelineWindow;
    if (m_TimelineWindowCount < kRenderTimelineWindow)
        ++m_TimelineWindowCount;

    double sortSum = 0.0;
    double bucketerSum = 0.0;
    for (std::size_t i = 0; i < m_TimelineWindowCount; ++i)
    {
        sortSum += m_TimelineWindow[i].SortMs;
        bucketerSum += m_TimelineWindow[i].BucketerScheduleMs;
    }
    const double invCount =
        m_TimelineWindowCount ? 1.0 / static_cast<double>(m_TimelineWindowCount) : 0.0;
    stats.SortMsMean = sortSum * invCount;
    stats.BucketerScheduleMsMean = bucketerSum * invCount;
}

void FrameOrchestrator::BuildFrameGraph(Rendering::RenderGraph::RGFrame& frame,
                                     const FrameGraphBuildParamsRG& params)
{
    GE_CPU_PROFILE_SCOPE("RenderServices.BuildFrameGraphRG");

    // Per-stream repeat guard (slice 8a). The stamps are (epoch, frameIndex):
    // pointer identity alone would no-op a frame re-begun mid-app-frame
    // (passive RenderSingle), leaving its consumers holding dead ids from
    // the previous incarnation. A genuine repeat call with the same
    // incarnation is a total no-op (the F1 contract: ONE call carries the
    // frame's complete view set).
    RGFrameStreamSlot* slot = FindOrCreateRGStream(frame);
    if (slot->DeclaredEpoch == m_WorldFrameEpoch &&
        slot->DeclaredFrameIndex == frame.FrameIndex())
        return;
    slot->DeclaredEpoch = m_WorldFrameEpoch;
    slot->DeclaredFrameIndex = frame.FrameIndex();
    // Mint this spine call's declare sequence: it identifies THIS declare in
    // OnFrameSubmittedRG's gate, so a frame that skipped its declare (or a
    // second window's declare) never routes a submit stamp to the wrong
    // stream's feature readback pendings.
    m_RGDeclareSeq = ++m_RGDeclareSeqCounter;
    slot->DeclareSeq = m_RGDeclareSeq;

    // ── Stage G: app-frame-global GPU-driven work, declared ONCE per app
    // frame into the first frame stream that gets here (the "owner frame").
    // Skinning palettes, culling visibility, draw-stream slots are one set
    // of physicals per app frame — a second declare would double the GPU
    // work and (for culling) wipe the CPU-side visibility-range table both
    // windows' buckets consume. ──
    const bool globalStagesThisCall = (m_GlobalStagesEpoch != m_WorldFrameEpoch);
    if (globalStagesThisCall)
    {
        m_GlobalStagesEpoch = m_WorldFrameEpoch;
        m_GlobalStagesFrame = &frame;

        // A2 STEP-0: zero this app frame's sort bracket up front. BuildWorldBatchKeys
        // overwrites it when it actually runs, so a frame that skips the build
        // correctly reports zero sort cost instead of ringing a stale value.
        m_Services->m_RenderTimelineStats.SortMs = 0.0;
        m_Services->m_RenderTimelineStats.SortViewCount = 0;

        // 0. CPU draw-list build + GPUScene upload — same guards as the old
        //    spine above, shared semantics.
        if (params.BuildWorldDrawListsIfReady)
        {
            const uint64_t frameIdx = m_Services->GetGPUScene() ? static_cast<uint64_t>(m_Services->GetGPUScene()->GetFrameIndex()) : 0ull;
            const bool extractionBeganThisFrame = (m_Services->m_WorldBeginFrameIndex == frameIdx);
            const bool hasSubmissions = (m_Services->m_WorldSubmissionCountThisFrame.load(std::memory_order_relaxed) > 0u);
            const bool alreadyBuilt = (m_Services->m_WorldDrawListsBuiltFrameIndex == frameIdx);
            if (!alreadyBuilt && extractionBeganThisFrame && hasSubmissions)
                m_Services->BuildWorldBatchKeys();
        }
        if (m_Services->GetGPUScene() && m_Services->GetGPUScene()->IsDirty())
            m_Services->GetGPUScene()->FlushGPUBuffers();

        // 1-3. GPU-driven prologue in the CONTRACT order (GpuDrivenFrameRG):
        //      skinning → culling → bucketer. NOT the old spine's order —
        //      there the bucketer ran after the pipeline; in RenderGraph recording
        //      order gives edges their direction, and the bucketer must
        //      precede every DrawStreamOrdering reader or the edge derives
        //      WAR (frame N−1 slot buffers).
        m_Services->ScheduleGpuSkinningAndRetarget(frame);
        m_Services->ScheduleViewCullingDispatches(frame, params.DeltaTime);
        // A2 STEP-0 bucketer bracket: the call-site wrap captures every internal
        // early-return path in one steady_clock pair. Publish the render-timeline
        // window here — stage G is the once-per-app-frame owner boundary.
        const auto bucketerBegin = std::chrono::steady_clock::now();
        m_Services->ScheduleWorldBucketerDispatches(frame);
        PublishRenderTimelineStats(
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - bucketerBegin)
                .count());
        // Shared ray-query AS pool: its one per-frame step, ahead of every
        // consumer (the RT shadow mask below, DDGI in the pipeline declare),
        // whether or not any consumer is active this frame.
        if (m_Services->m_SceneAS)
            m_Services->m_SceneAS->BeginFrame();
        // RT shadow-mask lane (DirectionalShadowMode::RayTraced, default
        // Cascades — no-op on the shipping path). Owner spine, once per app
        // frame, after the GPUScene flush above so the instance mirror matches
        // this frame, and after extraction so the worlds' resolved shadow
        // modes are this frame's.
        m_Services->ScheduleRTShadowMask(frame);

        slot->Values = m_Services->m_FrameRG;
    }
    else
    {
        // ── Non-owner stream: re-import the global products' PHYSICALS into
        // THIS frame (frame-local ids never cross frames). Ordering: same
        // physical graphics queue, owner submitted first in the editor's
        // per-window loop, and each re-imported physical's first touch gets
        // the BottomOfPipe-sourced import sync; the reverse order (a passive
        // float declaring before window 0) is stale-by-one but hazard-free —
        // both directions are covered by per-frame first-touch deps. MEMORY
        // visibility of the owner's compute writes (skin palettes, slot
        // buffers) rides the bucketer's end-of-pass CreateStreamReadyBarrier
        // — a GLOBAL barrier whose dst scope covers indirect+vertex reads in
        // every later same-queue submission. Any app frame where a non-owner
        // can draw world content also schedules the owner's bucketer (bucket
        // registration iterates m_Views, not the caller's ViewTargets), so
        // the barrier is always in place when it matters. ──
        GpuDrivenFrameRG values{};
        values.For.Stamp(frame);
        if (const auto atlasBuf = m_Services->GetSkinPaletteAtlas().GetBuffer(); atlasBuf.IsValid())
            values.SkinPaletteAtlas = frame.ImportExternalBuffer("SkinPaletteAtlas", atlasBuf);
        if (m_Services->GetGPUScene())
        {
            const GPUScene::GPUSceneFrameRG sceneRG = m_Services->GetGPUScene()->ImportFrameResources(frame);
            values.SceneInstances = sceneRG.Instances;
            values.SceneMeshes = sceneRG.Meshes;
        }
        if (m_Services->GetGPUCullingPipeline() &&
            m_Services->GetGPUCullingPipeline()->LastPublishArm() ==
                Rendering::GPUCullingPipeline::PublishArm::RenderGraph)
            values.Visibility = m_Services->GetGPUCullingPipeline()->ImportVisibility(frame);
        // Ordering proxy: only meaningful when the owner actually scheduled
        // the bucketer this app frame (its sentinel writes are what the edge
        // orders against).
        bool ownerScheduledBucketer = false;
        if (m_GlobalStagesFrame)
            for (const auto& [framePtr, ownerSlot] : m_RGStreams)
                if (framePtr == m_GlobalStagesFrame)
                {
                    ownerScheduledBucketer = ownerSlot.Values.DrawStreamOrdering.IsValid();
                    break;
                }
        if (m_Services->GetDrawStreamBuilder() && ownerScheduledBucketer)
        {
            if (const auto sentinel = m_Services->GetDrawStreamBuilder()->GetSentinelBuffer(); sentinel.IsValid())
                values.DrawStreamOrdering =
                    frame.ImportExternalBuffer("DrawStream.Ordering", sentinel);
        }
        // RuntimeVisible deliberately stays empty for non-owners (8a):
        // ImportRuntimeVisible mutates the owner-frame value the aggregate's
        // dedup re-import depends on, and the field is write-only outside
        // skinning (Phase6 gate default OFF).
        slot->Values = values;
        // The declare-scope cursor the per-view pass arms read (world/
        // prepass/cascade/area + overlays, all guarded Frame == &frame).
        // m_ViewFrameRG is NOT cleared here: entries self-heal per (view,
        // frame, frameIndex) in ViewFrameRGFor and every reader dual-compares
        // — a wholesale clear would kill the owner's live same-epoch entries
        // (shadow array / MSM / area imports its views still reference).
        m_Services->m_FrameRG = values;
    }

    // 4. Pipeline declaration (per-view passes in blueprint order), on THIS
    //    stream's own instance + blackboard.
    if (m_PipelineCompiler && m_PipelineNodeRegistry)
    {
        // Cheap steady-state check: ONE member read on the cached asset (the
        // inspector's direct Reload() is eventless — this poll is the primary
        // detector; the AssetReloaded invalidator below is a redundant fast
        // path). Fallback state keeps the full per-frame retry: first-time GUID
        // resolution has no event.
        if (m_ActiveRenderPipelineAsset &&
            m_ActiveRenderPipelineAsset->GetSourceHash() != m_ActiveRenderPipelineSourceHash)
            m_BlueprintRefreshNeeded = true;
        if (m_WaitingForNativeModules && !NativeModulesPending())
        {
            // The modules are in: the request compiles once more, and a pass type still unknown
            // is a refusal now.
            m_ActiveRenderPipelineSourceHash = 0;
            m_BlueprintRefreshNeeded = true;
        }
        if (m_BlueprintRefreshNeeded || m_ActiveRenderPipelineIsFallback)
        {
            EnsureActiveRenderPipelineBlueprint(PipelineRefreshMode::Frame);
            m_BlueprintRefreshNeeded = false;
        }
        if (m_ActiveBlueprint)
        {
            if (!slot->Instance)
                slot->Instance = std::make_unique<Pipeline::RenderPipelineInstance>(
                    *m_Services, *m_PipelineNodeRegistry);
            if (slot->AppliedBlueprintGen != m_ActiveBlueprintGen)
            {
                slot->Instance->SetBlueprint(*m_ActiveBlueprint);
                slot->AppliedBlueprintGen = m_ActiveBlueprintGen;
            }
            const auto& views = m_Services->Views().GetViews();
            slot->Instance->Declare(frame, params.ViewTargets,
                                    std::span<const Rendering::ViewDesc>(views.data(), views.size()),
                                    params.DeltaTime);
        }
    }

    // 5-6. Visibility epilogue, OWNER FRAME ONLY (the union/aggregate
    //      declarations consume the owner's frame-local visibility value;
    //      GPUCulling's m_FrameRGOwner bails are defense-in-depth behind
    //      this gate). Kept at the spine tail so the owner's declaration
    //      order is exactly the pre-8a order: union after every per-view
    //      culling pass (RAW on the visibility slices); aggregate's Write
    //      AFTER skinning's declared Read (the WAR pin).
    if (globalStagesThisCall && m_Services->GetGPUCullingPipeline() && m_Services->GetGPUScene())
    {
        m_Services->GetGPUCullingPipeline()->ScheduleVisibilityUnion(frame, m_Services->GetGPUScene());
        m_Services->GetGPUCullingPipeline()->ScheduleRuntimeVisibilityAggregate(frame, m_Services->GetGPUScene());
    }

    // 7. Output policy: mark each active view's FinalColor as an external
    //    sink at ShaderReadOnly — the layout both consumers want (editor UI
    //    samples it; the Player's encode/blit samples it). The backbuffer is
    //    the frame driver's business (ImportBackbuffer auto-marks, and the
    //    device owns its PRESENT_SRC transition — a sampling export layout
    //    must never be stamped on it).
    //
    //    Captures copy it too (viewport screenshots, scene thumbnails, the
    //    Player's --screenshot), so it requires TransferSrc. Which chain
    //    resource FinalColor IS on a given frame is a fact of that frame's
    //    declaration — skipped stages thread their input through and the
    //    identity-elided FinalCopy republishes View.Resolve onto the last LDR
    //    target — so only this resolved handle, never the blueprint, can
    //    carry the requirement. Stated every frame: the pool applies it from
    //    the next materialization, ahead of any capture request.
    if (slot->Instance)
    {
        for (const auto& vt : params.ViewTargets)
        {
            const Rendering::ViewDesc* view = m_Services->Views().FindViewDesc(vt.View);
            if (!view || view->ActiveRenderLayerMask() == 0)
                continue;
            if (auto out = slot->Instance->GetOutputRG(vt.View, "FinalColor");
                out.IsValid() && !frame.IsBackbuffer(out))
            {
                frame.MarkOutput(out, Rendering::RenderGraph::RGImageLayout::ShaderReadOnly);
                frame.RequireTransferUsage(out, Rendering::TextureUsage::TransferSrc);
            }
        }
    }
}

void FrameOrchestrator::OnFrameSubmittedRG(Rendering::RenderGraph::RGFrame& frame,
                                        const Rendering::IDevice::GpuSyncToken& token)
{
    // Stamp with the SUBMITTED frame's declare sequence, never the latest
    // minted one — a second stream's declare between this frame's declare
    // and its submit must not redirect the stamp. The epoch/frameIndex gate
    // refuses a STALE seq: a frame that skipped its declare this app frame
    // (old-arm excursion, HDR-switch abort) must not stamp last declare's
    // token-less pendings with a token from a submission that never ran the
    // dispatches.
    bool found = false;
    for (const auto& [framePtr, slot] : m_RGStreams)
        if (framePtr == &frame)
        {
            found = (slot.DeclareSeq != 0 && slot.DeclaredEpoch == m_WorldFrameEpoch &&
                     slot.DeclaredFrameIndex == frame.FrameIndex());
            break;
        }
    if (!found)
        return; // stream never declared this incarnation (or evicted) — nothing to stamp
    // Generic feature fan-out: readback rings stamp their pendings with the
    // submission token (RGReadbackRing completion contract). CSM's SDSM
    // readback rides this hook too — no feature-specific stamping remains.
    m_Services->ForEachFeature([&](IRenderFeature& feature)
                               { feature.OnFrameSubmittedRG(frame, token); });
}

Pipeline::RenderPipelineInstance* FrameOrchestrator::PipelineInstanceForFrame(
    const Rendering::RenderGraph::RGFrame& frame) const
{
    for (const auto& [framePtr, slot] : m_RGStreams)
        if (framePtr == &frame)
            return slot.Instance.get();
    return nullptr;
}

void FrameOrchestrator::OnDeviceRebuilt()
{
    // A device rebuild leaves each node's lazily-cached device handles (samplers,
    // interned pipeline state, node-owned GPU buffers) dangling. Flag every live
    // stream's instance so its next Declare tears down the stale nodes (dtors
    // no-op on the already-freed handles) and rebuilds fresh ones.
    for (auto& [framePtr, slot] : m_RGStreams)
    {
        (void)framePtr;
        if (slot.Instance)
            slot.Instance->OnDeviceRebuilt();
    }
}

Pipeline::RenderPipelineNodeRegistry* FrameOrchestrator::GetPipelineNodeRegistry()
{
    return m_PipelineNodeRegistry.get();
}

const Pipeline::RenderPipelineNodeRegistry* FrameOrchestrator::GetPipelineNodeRegistry() const
{
    return m_PipelineNodeRegistry.get();
}

bool FrameOrchestrator::RegisterPipelineNodeType(const std::string& type,
                                               std::function<std::unique_ptr<Pipeline::IRenderPipelineNode>()> factory,
                                               bool perView)
{
    if (!m_PipelineNodeRegistry)
        return false;
    const bool registered = m_PipelineNodeRegistry->Register(type, std::move(factory), perView);
    if (registered && !m_ActiveBlueprintPinned &&
        (m_ActiveRenderPipelineLastCompileHadErrors || m_ActiveBlueprintIsPartial))
    {
        // A rejected asset, or one drawing without its waiting script passes, may
        // reference a type registered by a later module load. Its unchanged source
        // hash does not capture that compiler input.
        // Coalesce successful registrations into one retry on the next build;
        // another rejection remains cached until a source or registry change.
        m_ActiveRenderPipelineSourceHash = 0;
        m_BlueprintRefreshNeeded = true;
    }
    return registered;
}

void FrameOrchestrator::ReconcileModuleNodeRegistrations(std::string_view moduleId,
                                                         uint64_t currentGeneration)
{
    if (!m_PipelineNodeRegistry)
        return;
    // Ownership check BEFORE the retire pass: a module whose newest load
    // dropped ALL of its node types still has live instances holding old-code
    // nodes, and those must go down with the registrations.
    const bool ownsNodeTypes = m_PipelineNodeRegistry->HasModuleNodes(moduleId);
    m_PipelineNodeRegistry->RetireSupersededModuleNodes(moduleId, currentGeneration);
    if (!ownsNodeTypes)
        return;

    // Tear down live pipeline instances through the stream-eviction path (it
    // reaps the blackboard values, feature pendings, and stage-G ownership the
    // dying instance's frame may hold). The slots re-create on the next
    // BuildFrameGraph and re-instantiate nodes from the reconciled registry —
    // which also picks up types a FIRST load just made resolvable.
    std::vector<Rendering::RenderGraph::RGFrame*> liveFrames;
    liveFrames.reserve(m_RGStreams.size());
    for (const auto& [framePtr, slot] : m_RGStreams)
        if (slot.Instance)
            liveFrames.push_back(framePtr);
    for (Rendering::RenderGraph::RGFrame* frame : liveFrames)
        RemovePipelineInstanceForFrame(frame);
    if (!liveFrames.empty())
        Logger::Log::Info("[RenderPipeline] Module '{}' reload reconcile: {} live pipeline instance(s) "
                          "torn down for rebuild from the newest factories",
                          moduleId, liveFrames.size());
}

std::size_t FrameOrchestrator::PurgeModulePipelineNodes(std::string_view moduleId, uint64_t generation)
{
    return m_PipelineNodeRegistry ? m_PipelineNodeRegistry->PurgeModuleNodes(moduleId, generation) : 0;
}

std::size_t FrameOrchestrator::CountSupersededModulePipelineNodes(std::string_view moduleId,
                                                                  uint64_t currentGeneration) const
{
    return m_PipelineNodeRegistry
               ? m_PipelineNodeRegistry->CountSupersededModuleNodes(moduleId, currentGeneration)
               : 0;
}

void FrameOrchestrator::PreLoadShaderPackage(const std::string& name, Rendering::ShaderSourceKind kind)
{
    Rendering::ShaderPackage pkg{};
    std::string err;
    if (Rendering::LoadShaderPkg(name, kind, pkg, &err))
    {
        std::lock_guard<std::mutex> lock(m_ShaderPkgCacheMutex);
        m_ShaderPkgCache.emplace(name, std::move(pkg));
    }
}

bool FrameOrchestrator::TryConsumePreLoadedShaderPackage(const std::string& name, Rendering::ShaderPackage& out)
{
    std::lock_guard<std::mutex> lock(m_ShaderPkgCacheMutex);
    auto it = m_ShaderPkgCache.find(name);
    if (it == m_ShaderPkgCache.end())
        return false;
    out = std::move(it->second);
    m_ShaderPkgCache.erase(it);
    return true;
}

} // namespace Engine::Renderer
} // namespace GameEngine
