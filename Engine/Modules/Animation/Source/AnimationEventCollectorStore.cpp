#include "Animation/AnimationEventCollectorStore.h"

namespace GameEngine::Animation
{

AnimationEventCollectorStore& AnimationEventCollectorStore::Instance()
{
    static AnimationEventCollectorStore store;
    return store;
}

uint64 AnimationEventCollectorStore::Create()
{
    return m_Collectors.Create(std::make_unique<AnimationEventCollector>()).Value();
}

AnimationEventCollector* AnimationEventCollectorStore::Get(uint64 collectorId)
{
    std::unique_ptr<AnimationEventCollector>* collector = m_Collectors.Get(GenerationalVector::Handle(collectorId));
    return collector ? collector->get() : nullptr;
}

const AnimationEventCollector* AnimationEventCollectorStore::Get(uint64 collectorId) const
{
    const std::unique_ptr<AnimationEventCollector>* collector =
        m_Collectors.Get(GenerationalVector::Handle(collectorId));
    return collector ? collector->get() : nullptr;
}

void AnimationEventCollectorStore::Destroy(uint64 collectorId)
{
    const GenerationalVector::Handle handle(collectorId);
    if (m_Collectors.IsValid(handle))
        m_Collectors.Destroy(handle);
}

void AnimationEventCollectorStore::ClearForTest()
{
    m_Collectors.Clear();
}

} // namespace GameEngine::Animation
