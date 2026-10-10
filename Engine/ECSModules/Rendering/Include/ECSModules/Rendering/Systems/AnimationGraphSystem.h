#pragma once

#include "Animation/AnimationPose.h"
#include "Animation/PoseToSkinMatrices.h"
#include "ECS/Entity.h"
#include "ECS/Systems.h"
#include "Types/Types.h"

#include <unordered_set>
#include <vector>

namespace GameEngine { namespace Engine::Renderer {

class RenderServices;

// Instantiates per-Animator pose-graph players from .animgraph assets,
// applies pending StringId param stamps, Evaluate()s into an AnimationPose,
// optionally extracts root motion into CharacterController / Transform, and
// writes CompactSkinMatrices so SkinningUpload consumes them. Sets
// AnimatorRef.ClipIndex = 0 so AnimationSystem skips the same entities.
class AnimationGraphSystem : public ECS::ISystem
{
public:
    explicit AnimationGraphSystem(RenderServices* rs = nullptr) : m_RenderServices(rs) {}
    const char* GetName() const override { return "AnimationGraph"; }
    void Update(ECS::World& world, float32 deltaTime) override;

private:
    RenderServices* m_RenderServices = nullptr;
    Animation::AnimationPose m_Pose;
    Animation::PoseSampleWorkspace m_Workspace;
    std::vector<ECS::EntityHandle> m_Subtree;
    std::unordered_set<uint32> m_WrittenRuntimes;

    // A skin palette written this update: the runtime, its skeleton and the
    // slot of the pose it was built from. The palette is a function of the
    // skeleton and that pose alone. The pose stands in for the graph's inputs
    // because graph nodes carry state no input record sees: a state machine
    // takes a zero-duration transition whose conditions hold even with a zero
    // delta time, so a paused graph can change pose with unchanged parameters.
    struct PaletteInputs
    {
        uint32 RuntimeId = 0;
        uint32 SkeletonId = 0;
        uint32 PoseSlot = 0;

        bool operator==(const PaletteInputs&) const = default;
    };

    // True when this update's records or the bits of any pose they name differ
    // from the previous update's.
    bool PaletteInputsChanged() const;

    // Palette records of this update, in query order, and of the previous
    // one. Any difference (a pose changed in any bit, a character joined or
    // left) means palette content can differ and is reported to
    // RenderServices; an identical set is not, so a still scene with a graph
    // character at rest lets the depth-derived passes settle. Only the first
    // m_FramePoseCount and m_LastPoseCount poses are live; the rest keep their
    // allocations for the next update.
    std::vector<PaletteInputs> m_FramePaletteInputs;
    std::vector<PaletteInputs> m_LastPaletteInputs;
    std::vector<Animation::AnimationPose> m_FramePoses;
    std::vector<Animation::AnimationPose> m_LastPoses;
    uint32 m_FramePoseCount = 0;
    uint32 m_LastPoseCount = 0;
};

} } // namespace GameEngine::Engine::Renderer
