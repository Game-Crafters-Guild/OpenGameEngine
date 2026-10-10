#pragma once

#include "Animation/AnimationEvent.h"
#include "GenerationalVector/GenerationalVector.h"
#include "Types/Types.h"

#include <memory>

namespace GameEngine::Animation
{

// Engine-owned event collectors, one per Animator. Animator holds a POD id into this store (as it does into
// AnimationGraphStore): a collector owns a vector, and a component must stay trivially copyable.
//
// An id is a GenerationalVector handle's value (a 32-bit slot index and a 32-bit generation), so an id copied onto
// another Animator stops reaching anything once its collector is destroyed, and ids stay distinct however many
// collectors are live. Collectors are held by pointer, so the address Get returns stays put while others are created.
//
// The store takes no lock: its users never overlap. Only the primary world runs the animation schedule, where
// AnimationEventCollectorSystem (which creates ids), AnimationGraph, Animation and HumanoidRetarget (which read them)
// run one after another by their declared ordering (RegisterRenderingSystems); the Animator remove hook destroys
// collectors under the world's exclusive structural lock; scripts poll on the main thread only. An Animator in another
// world (a thumbnail's) never gets an id, and looking up or destroying id 0 reads nothing from the store. A new user
// joins that ordering or takes the store back to a lock.
class AnimationEventCollectorStore
{
public:
    static AnimationEventCollectorStore& Instance();

    uint64 Create();
    AnimationEventCollector* Get(uint64 collectorId);
    const AnimationEventCollector* Get(uint64 collectorId) const;

    void Destroy(uint64 collectorId);
    void ClearForTest();

private:
    AnimationEventCollectorStore() = default;

    GenerationalVector::GenerationalVector<std::unique_ptr<AnimationEventCollector>> m_Collectors;
};

} // namespace GameEngine::Animation
