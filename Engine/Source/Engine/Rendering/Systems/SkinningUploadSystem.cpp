#include "ECSModules/Rendering/Systems/SkinningUploadSystem.h"
#include "Engine/Rendering/GPUAnimationDataStore.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/RetargetGPUDataStore.h"
#include "Engine/Rendering/RetargetRenderFeature.h"
#include "Engine/Rendering/SkinPaletteAtlas.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "Rendering/Core/Device.h"

#include "ECS/World.h"
#include "ECS/Query.h"
#include "Components/Animation/SkeletonRef.h"

#include "Engine/Rendering/SkinDiag.h"
#include "Logger/Logger.h"

#include <algorithm>

namespace GameEngine { namespace Engine::Renderer {

void SkinningUploadSystem::Update(ECS::World& world, float32 /*deltaTime*/) {
    RenderServices* rs = m_RenderServices;
    if (!rs)
        return;

    auto* device = rs->GetDevice();
    if (!device) return;

    auto& store = SkeletonStore::Instance();

    // Collect unique runtime IDs referenced by entities in this world.
    // Each runtimeId maps to a per-entity pose buffer allocated by ModelEntityFactory.
    m_UniqueRuntimeIds.clear();
    world.Query<ECS::Read<GameEngine::Components::SkeletonRef>>()
        .Each([&](ECS::EntityHandle, const GameEngine::Components::SkeletonRef& sref) {
            if (sref.runtimeId != 0)
                m_UniqueRuntimeIds.push_back(sref.runtimeId);
        });
    std::sort(m_UniqueRuntimeIds.begin(), m_UniqueRuntimeIds.end());
    m_UniqueRuntimeIds.erase(std::unique(m_UniqueRuntimeIds.begin(), m_UniqueRuntimeIds.end()), m_UniqueRuntimeIds.end());

    auto& gpuStore = rs->GetGPUAnimationDataStore();
    auto& retargetStore = rs->GetRetargetRenderFeature().GetDataStore();

    for (uint32 rtId : m_UniqueRuntimeIds) {
        // Skip runtimes that will be written by the GPU compute skinning pass.
        if (gpuStore.IsGPUHandled(rtId)) continue;
        // Skip runtimes whose atlas slot was reserved by HumanoidRetargetSystem
        // for the GPU retarget dispatch (otherwise CPU and GPU race on the
        // same atlas slot — flicker / stale frames).
        if (retargetStore.IsRetargetGPUHandled(rtId)) continue;

        auto* runtime = store.GetRuntime(rtId);
        if (!runtime) continue;

        const uint32 skelId = store.GetRuntimeSkeletonId(rtId);
        const SkeletonData* sk = store.Get(skelId);
        if (!sk) continue;

        const size_t jointCount = (sk->SkinJointCount > 0) ? (size_t)sk->SkinJointCount : (size_t)sk->BoneCount;
        if (jointCount == 0 || runtime->CompactSkinMatrices.empty()) continue;

        // Upload into the shared palette atlas SSBO.
        auto& atlas = rs->GetSkinPaletteAtlas();
        auto alloc = atlas.Upload(runtime->CompactSkinMatrices.data(), static_cast<uint32>(jointCount));
        if (alloc.valid)
        {
            runtime->SetAtlasPaletteOffset(alloc.OffsetInBones());
            if (SkinDiag::IsEnabled())
            {
                Logger::Log::Debug("[SkinDiag] Upload: rtId={} skelId={} joints={} atlasOffset={}",
                                   rtId, skelId, jointCount, runtime->AtlasPaletteOffsetBones);
            }
        }
        else
        {
            // Retire the offset to the identity block. Leaving the previous
            // value would point this frame's skinning at a slot the atlas has
            // already handed to another runtime — a visibly wrong pose, not a
            // dropped frame. Refusal is what records the demand that grows the
            // ring, so this is a one-cycle bind pose, not a permanent one.
            runtime->SetAtlasPaletteOffset(SkinPaletteAtlas::kIdentityPaletteOffsetBones);

            // Always warn on upload failure — atlas is likely full.
            // Delta-only: suppress repeated warnings for the same runtime.
            static thread_local uint32 s_LastWarnedRtId = 0;
            if (s_LastWarnedRtId != rtId)
            {
                s_LastWarnedRtId = rtId;
                Logger::Log::Warning("SkinPaletteAtlas: upload failed for rtId={} ({} joints) — atlas may be full",
                                     rtId, jointCount);
            }
        }
    }
}

} } // namespace GameEngine::Engine::Renderer

