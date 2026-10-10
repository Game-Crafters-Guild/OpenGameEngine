#pragma once

#include "Mathematics/Vector3.h"
#include "Mathematics/Vector4.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine::Particles
{

/// One recorded point of a trail, in world space.
struct ParticleTrailPoint
{
    Mathematics::Vector3 Position{};
    float Size = 1.0f;
    Mathematics::Vector4 Color{1.0f, 1.0f, 1.0f, 1.0f};
    double Time = 0.0;
    /// Distance along the trail from its first recorded point.
    float Distance = 0.0f;
};

/// Fixed pool of trails, each a ring of points. Sized when a stack is bound; recording, expiry and
/// release are constant time per point, and a trail can outlive its particle while it fades.
class ParticleTrailStore
{
  public:
    static constexpr uint32 kNoTrail = 0;

    void Configure(uint32 trails, uint32 pointsPerTrail);
    void Clear();

    /// Starts a trail for particle `particleId`; returns its handle, or kNoTrail when the pool is full.
    uint32 Acquire(uint32 particleId, float lifetime, bool dieWithParticle);
    /// Appends `point`. When the newest kept point is closer than `spacing` to the one before it,
    /// the newest is replaced instead, so the head follows the particle between kept points.
    void Record(uint32 handle, const ParticleTrailPoint& point, float spacing);
    /// The particle of `handle` died: removes the trail now or lets it fade, as it was acquired.
    void ReleaseOwner(uint32 handle);
    /// Drops points older than their trail's lifetime and frees faded trails without an owner.
    void Expire(double now);

    uint32 PointsPerTrail() const { return m_PointsPerTrail; }
    uint32 SlotCount() const { return static_cast<uint32>(m_Trails.size()); }
    uint32 ActiveCount() const { return m_Active; }
    bool IsActive(uint32 slot) const { return m_Trails[slot].Active; }
    uint32 ParticleId(uint32 slot) const { return m_Trails[slot].ParticleId; }
    uint32 PointCount(uint32 slot) const { return m_Trails[slot].Count; }
    /// Point `index` of trail `slot`, oldest first.
    const ParticleTrailPoint& Point(uint32 slot, uint32 index) const;

  private:
    struct Trail
    {
        uint32 Head = 0;
        uint32 Count = 0;
        uint32 ParticleId = 0;
        float Lifetime = 0.0f;
        bool Active = false;
        bool Owned = false;
        bool DieWithParticle = false;
    };

    void Free(uint32 slot);
    ParticleTrailPoint& At(uint32 slot, uint32 index);

    std::vector<Trail> m_Trails;
    std::vector<ParticleTrailPoint> m_Points;
    std::vector<uint32> m_FreeSlots;
    uint32 m_PointsPerTrail = 0;
    uint32 m_Active = 0;
};

} // namespace GameEngine::Particles
