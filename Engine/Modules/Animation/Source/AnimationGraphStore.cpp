#include "Animation/AnimationGraphStore.h"

#include "Animation/AnimationGraphPlayer.h"

namespace GameEngine::Animation
{

AnimationGraphStore& AnimationGraphStore::Instance()
{
    static AnimationGraphStore store;
    return store;
}

uint64 AnimationGraphStore::Create(std::unique_ptr<AnimationGraphPlayer> player)
{
    if (!player)
        return 0;
    std::lock_guard lock(m_Mutex);
    return m_Players.Create(std::move(player)).Value();
}

AnimationGraphPlayer* AnimationGraphStore::Get(uint64 runtimeId)
{
    std::lock_guard lock(m_Mutex);
    std::unique_ptr<AnimationGraphPlayer>* player = m_Players.Get(GenerationalVector::Handle(runtimeId));
    return player ? player->get() : nullptr;
}

const AnimationGraphPlayer* AnimationGraphStore::Get(uint64 runtimeId) const
{
    std::lock_guard lock(m_Mutex);
    const std::unique_ptr<AnimationGraphPlayer>* player = m_Players.Get(GenerationalVector::Handle(runtimeId));
    return player ? player->get() : nullptr;
}

void AnimationGraphStore::Destroy(uint64 runtimeId)
{
    const GenerationalVector::Handle handle(runtimeId);
    std::lock_guard lock(m_Mutex);
    if (m_Players.IsValid(handle))
        m_Players.Destroy(handle);
}

void AnimationGraphStore::ClearForTest()
{
    std::lock_guard lock(m_Mutex);
    m_Players.Clear();
}

} // namespace GameEngine::Animation
