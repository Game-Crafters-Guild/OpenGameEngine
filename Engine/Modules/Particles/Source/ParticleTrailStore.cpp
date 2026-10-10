#include "Particles/ParticleTrailStore.h"

#include <cassert>

namespace GameEngine::Particles
{
namespace
{
float PointDistance(const ParticleTrailPoint& a, const ParticleTrailPoint& b)
{
    return (b.Position - a.Position).Length();
}
} // namespace

void ParticleTrailStore::Configure(uint32 trails, uint32 pointsPerTrail)
{
    m_PointsPerTrail = pointsPerTrail;
    m_Trails.assign(trails, Trail{});
    m_Points.assign(static_cast<size_t>(trails) * pointsPerTrail, ParticleTrailPoint{});
    Clear();
}

void ParticleTrailStore::Clear()
{
    m_FreeSlots.clear();
    m_FreeSlots.reserve(m_Trails.size());
    // Free slots are taken from the back, so fill them in reverse to hand out slot 0 first.
    for (uint32 slot = static_cast<uint32>(m_Trails.size()); slot-- > 0;)
    {
        m_Trails[slot] = Trail{};
        m_FreeSlots.push_back(slot);
    }
    m_Active = 0;
}

uint32 ParticleTrailStore::Acquire(uint32 particleId, float lifetime, bool dieWithParticle)
{
    if (m_FreeSlots.empty() || m_PointsPerTrail < 2)
        return kNoTrail;
    const uint32 slot = m_FreeSlots.back();
    m_FreeSlots.pop_back();
    auto& trail = m_Trails[slot];
    trail = Trail{};
    trail.ParticleId = particleId;
    trail.Lifetime = lifetime;
    trail.Active = true;
    trail.Owned = true;
    trail.DieWithParticle = dieWithParticle;
    ++m_Active;
    return slot + 1;
}

ParticleTrailPoint& ParticleTrailStore::At(uint32 slot, uint32 index)
{
    const auto& trail = m_Trails[slot];
    return m_Points[static_cast<size_t>(slot) * m_PointsPerTrail + (trail.Head + index) % m_PointsPerTrail];
}

const ParticleTrailPoint& ParticleTrailStore::Point(uint32 slot, uint32 index) const
{
    assert(index < m_Trails[slot].Count);
    const auto& trail = m_Trails[slot];
    return m_Points[static_cast<size_t>(slot) * m_PointsPerTrail + (trail.Head + index) % m_PointsPerTrail];
}

void ParticleTrailStore::Record(uint32 handle, const ParticleTrailPoint& point, float spacing)
{
    if (handle == kNoTrail)
        return;
    const uint32 slot = handle - 1;
    auto& trail = m_Trails[slot];
    ParticleTrailPoint next = point;
    if (trail.Count > 1 && PointDistance(At(slot, trail.Count - 2), At(slot, trail.Count - 1)) < spacing)
        --trail.Count; // the moving head has not travelled far enough to become a kept point
    if (trail.Count > 0)
    {
        const auto& previous = At(slot, trail.Count - 1);
        next.Distance = previous.Distance + PointDistance(previous, next);
    }
    if (trail.Count == m_PointsPerTrail)
    {
        trail.Head = (trail.Head + 1) % m_PointsPerTrail;
        --trail.Count;
    }
    At(slot, trail.Count) = next;
    ++trail.Count;
}

void ParticleTrailStore::ReleaseOwner(uint32 handle)
{
    if (handle == kNoTrail)
        return;
    const uint32 slot = handle - 1;
    auto& trail = m_Trails[slot];
    if (!trail.Active)
        return;
    if (trail.DieWithParticle)
        Free(slot);
    else
        trail.Owned = false;
}

void ParticleTrailStore::Expire(double now)
{
    for (uint32 slot = 0; slot < m_Trails.size(); ++slot)
    {
        auto& trail = m_Trails[slot];
        if (!trail.Active)
            continue;
        const double cutoff = now - trail.Lifetime;
        while (trail.Count > 0 && At(slot, 0).Time < cutoff)
        {
            trail.Head = (trail.Head + 1) % m_PointsPerTrail;
            --trail.Count;
        }
        if (trail.Count == 0 && !trail.Owned)
            Free(slot);
    }
}

void ParticleTrailStore::Free(uint32 slot)
{
    m_Trails[slot] = Trail{};
    m_FreeSlots.push_back(slot);
    --m_Active;
}

} // namespace GameEngine::Particles
