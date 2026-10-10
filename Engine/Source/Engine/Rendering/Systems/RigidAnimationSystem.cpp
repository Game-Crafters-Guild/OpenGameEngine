#include "ECSModules/Rendering/Systems/RigidAnimationSystem.h"

#include "Assets/AnimationClip.h"
#include "Components/Animation/AnimatedNodeRef.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Transform.h"
#include "ECS/Query.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "ECSModules/Rendering/SkeletonStore.h"
#include "Engine/Rendering/AnimationSampling.h"

#include <algorithm>
#include <cstring>

namespace GameEngine
{
namespace Engine::Renderer
{

using namespace GameEngine::Components;

void RigidAnimationSystem::Update(ECS::World& world, [[maybe_unused]] float32 deltaTime)
{
    auto query = world.Query<ECS::Write<AnimatorRef>,
                             ECS::Read<SkeletonRef>,
                             ECS::Read<AnimatedNodeRef>,
                             ECS::Write<Transform>>();
    query.Each([&](ECS::EntityHandle, AnimatorRef& animator, const SkeletonRef& skeletonRef, const AnimatedNodeRef& nodeRef, Transform& transform)
    {
        if (animator.ClipIndex == 0 || skeletonRef.skeletonId == 0)
            return;

        auto& clipStore = ClipStore::Instance();
        auto& skeletonStore = SkeletonStore::Instance();
        SharedPtr<AnimationClip> clip = clipStore.Get(animator.ClipIndex);
        const SkeletonData* skeleton = skeletonStore.Get(skeletonRef.skeletonId);
        if (!clip || !skeleton || nodeRef.nodeIndex >= skeleton->BoneCount)
            return;

        // Time advancement is handled by AnimationSystem (which runs before us
        // and matches all entities with AnimatorRef+SkeletonRef). We only read
        // the current Time to sample the node transform. Defensive clamping in
        // case time exceeds duration.
        {
            const float duration = clip->GetDuration();
            if (duration > 0.0f)
            {
                if (animator.IsLooping())
                {
                    animator.Time = static_cast<float>(WrapClipTime(animator.Time, 0.0f, duration));
                }
                else if (animator.Time > duration)
                {
                    animator.Time = duration;
                }
            }
        }

        SampleAnimationPose(*skeleton, clip.get(), animator.Time, &m_NodeWorldMatrices, nullptr, m_Workspace);
        const size_t matrixOffset = static_cast<size_t>(nodeRef.nodeIndex) * 16u;
        if (matrixOffset + 16u > m_NodeWorldMatrices.size())
            return;

        std::memcpy(transform.matrix, m_NodeWorldMatrices.data() + matrixOffset, sizeof(float32) * 16u);
    });
}

} // namespace Engine::Renderer
} // namespace GameEngine
