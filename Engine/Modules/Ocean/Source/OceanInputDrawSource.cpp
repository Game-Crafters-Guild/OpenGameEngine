#include "Ocean/OceanInputDrawSource.h"

#include <algorithm>

namespace GameEngine::Ocean
{

void OceanInputDrawRegistry::Add(const IOceanInputDrawSource* source)
{
    if (!source || std::find(m_Sources.begin(), m_Sources.end(), source) != m_Sources.end())
        return;
    m_Sources.push_back(source);
}

void OceanInputDrawRegistry::Remove(const IOceanInputDrawSource* source)
{
    m_Sources.erase(std::remove(m_Sources.begin(), m_Sources.end(), source), m_Sources.end());
}

void OceanInputDrawRegistry::Clear()
{
    m_Sources.clear();
}

void OceanInputDrawRegistry::Collect(const OceanInputDrawContext& context,
                                     std::vector<OceanInputDrawPacket>& outPackets) const
{
    const size_t first = outPackets.size();
    for (const IOceanInputDrawSource* source : m_Sources)
        if (source)
            source->CollectOceanInputDraws(context, outPackets);
    Sort(std::span<OceanInputDrawPacket>(outPackets).subspan(first));
}

void OceanInputDrawRegistry::Sort(std::span<OceanInputDrawPacket> packets)
{
    std::stable_sort(packets.begin(), packets.end(),
                     [](const OceanInputDrawPacket& a, const OceanInputDrawPacket& b) {
                         if (a.Priority != b.Priority)
                             return a.Priority < b.Priority;
                         return a.EntityId < b.EntityId;
                     });
}

float32 OceanInputDrawRegistry::ApplyBlend(float32 current, float32 value,
                                           float32 weight, OceanInputBlendMode blend)
{
    const float32 w = std::clamp(weight, 0.0f, 1.0f);
    float32 combined = value;
    switch (blend)
    {
    case OceanInputBlendMode::Additive: combined = current + value; break;
    case OceanInputBlendMode::Multiply: combined = current * value; break;
    case OceanInputBlendMode::Minimum: combined = std::min(current, value); break;
    case OceanInputBlendMode::Maximum: combined = std::max(current, value); break;
    case OceanInputBlendMode::Replace: break;
    }
    return current + (combined - current) * w;
}

void OceanNativeInputDrawSource::CollectOceanInputDraws(
    const OceanInputDrawContext& context,
    std::vector<OceanInputDrawPacket>& outPackets) const
{
    if (m_Collect)
        m_Collect(context, outPackets, m_UserData);
}

} // namespace GameEngine::Ocean
