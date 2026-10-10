#include "Scripting/ModelABI.h"

#include "AbiMainThread.h"
#include "AbiTextOut.h"
#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Assets/ModelExtras.h"
#include "Core/Engine.h"
#include "DllEngineBootstrap.h"
#include "Logger/Logger.h"

#include <atomic>
#include <exception>
#include <memory>
#include <string_view>

namespace
{

using GameEngine::ModelObjectKind;

static_assert(GE_ModelObjectKind_Scene == static_cast<uint32_t>(ModelObjectKind::Scene));
static_assert(GE_ModelObjectKind_Node == static_cast<uint32_t>(ModelObjectKind::Node));
static_assert(GE_ModelObjectKind_Mesh == static_cast<uint32_t>(ModelObjectKind::Mesh));
static_assert(GE_ModelObjectKind_Material == static_cast<uint32_t>(ModelObjectKind::Material));
static_assert(GE_ModelObjectKind_Animation == static_cast<uint32_t>(ModelObjectKind::Animation));

// Logs, once per process, that GE_Model_GetExtras was refused off the main thread: a script that calls it
// from a worker every frame would otherwise fill the log with the same line.
void ReportRefusedOffMainThread()
{
    static std::atomic<bool> s_Reported{false};
    if (s_Reported.exchange(true))
        return;
    Logger::Log::Error("GE_Model_GetExtras (ModelApi.GetExtras) was called from a thread other than the engine's "
                       "main thread and was refused, because a hot reload replaces a model's extras on the main "
                       "thread. Read them from a system's update, or read once at spawn and keep what you parsed.");
}

} // namespace

GE_API GE_Result GE_CDECL GE_Model_GetExtras(const uint8_t modelGuidBytes[16],
                                             uint32_t kind,
                                             const char* nameUtf8,
                                             uint32_t nameLength,
                                             char* buffer,
                                             int32_t bufferLen,
                                             int32_t* outLen)
{
    try
    {
        if (outLen)
            *outLen = 0;
        if (!modelGuidBytes || !outLen || (!nameUtf8 && nameLength > 0) || bufferLen < 0 ||
            (!buffer && bufferLen != 0) || kind >= GameEngine::kModelObjectKindCount)
            return GE_Result_InvalidArg;
        if (GameEngine::DllBootstrap::EnsureEngineInitialized() != GE_Result_Ok)
            return GE_Result_NotInitialized;
        if (!GameEngine::ScriptingAbi::OnEngineMainThread())
        {
            ReportRefusedOffMainThread();
            return GE_Result_Fail;
        }
        GameEngine::AssetManager* assetManager = GameEngine::EngineCore::GetInstance().TryGetAssetManager();
        if (!assetManager)
            return GE_Result_NotInitialized;

        using GuidBytes = const GameEngine::uint8[GameEngine::GUID::kSize];
        const GameEngine::GUID modelGuid =
            GameEngine::GUID::FromBytes(*reinterpret_cast<GuidBytes*>(modelGuidBytes));
        const auto model = std::dynamic_pointer_cast<GameEngine::ModelAsset>(assetManager->GetAsset(modelGuid));
        if (!model || !model->IsLoaded())
            return GE_Result_NotFound;

        const std::string_view name(nameUtf8 ? nameUtf8 : "", nameLength);
        return GameEngine::ScriptingAbi::CopyTextOut(
            model->GetExtras().Find(static_cast<ModelObjectKind>(kind), name), buffer, bufferLen, outLen);
    }
    catch (const std::exception& exception)
    {
        Logger::Log::Error("GE_Model_GetExtras (ModelApi.GetExtras) failed: {}", exception.what());
        return GE_Result_Fail;
    }
    catch (...)
    {
        Logger::Log::Error("GE_Model_GetExtras (ModelApi.GetExtras) failed with an unknown exception.");
        return GE_Result_Fail;
    }
}
