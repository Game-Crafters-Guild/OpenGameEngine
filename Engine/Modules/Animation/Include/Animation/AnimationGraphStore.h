#pragma once

#include "Animation/AnimationGraphPlayer.h"
#include "GenerationalVector/GenerationalVector.h"
#include "Types/Types.h"

#include <memory>
#include <mutex>

namespace GameEngine::Animation
{

// Engine-owned graph instances. Animator holds a POD runtime id into this
// store — never unique_ptr on the ECS component (ClipStore pattern).
// Each Create() is a distinct mutable player; the same .animgraph asset
// must Instantiate once per Animator so SM time and params stay per-entity.
//
// An id is a GenerationalVector handle's value (a 32-bit slot index and a
// 32-bit generation), so Destroy + Create does not alias a stale id copied
// onto another Animator, and ids stay distinct however many players are live.
// Players are held by pointer, so the address Get returns stays put while
// others are created.
class AnimationGraphStore
{
public:
    static AnimationGraphStore& Instance();

    uint64 Create(std::unique_ptr<AnimationGraphPlayer> player);
    AnimationGraphPlayer* Get(uint64 runtimeId);
    const AnimationGraphPlayer* Get(uint64 runtimeId) const;

    void Destroy(uint64 runtimeId);
    void ClearForTest();

private:
    AnimationGraphStore() = default;

    mutable std::mutex m_Mutex;
    GenerationalVector::GenerationalVector<std::unique_ptr<AnimationGraphPlayer>> m_Players;
};

} // namespace GameEngine::Animation
