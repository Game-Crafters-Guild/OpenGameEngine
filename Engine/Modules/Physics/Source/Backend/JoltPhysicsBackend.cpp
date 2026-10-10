#include "Physics/Backend/JoltPhysicsBackend.h"

#include "Logger/Logger.h"
#include "Platform/Capabilities.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>

#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/MotionType.h>
#include <Jolt/Physics/EActivation.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>

#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Character/Character.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>

#include <concurrentqueue/moodycamel/concurrentqueue.h>

#include "GenerationalVector/GenerationalVector.h"

namespace GameEngine::Physics
{
namespace
{
// Extra encode-range padding for heightfield shapes, as a fraction of the
// initial max-min sample span, applied symmetrically on both ends. Jolt fixes
// the 16-bit height quantization range at creation, so without headroom the
// first sculpt stroke past the original min/max forces a full shape rebuild.
// Padding proportionally coarsens the quantization step (range/65535), so
// keep it modest. A flat terrain has zero span and therefore zero headroom —
// its first sculpt always takes the rebuild path, which re-pads.
constexpr float32 kHeightFieldRangeHeadroom = 0.25f;

// Broadphase layers (keep minimal for initial integration).
namespace BroadPhaseLayers
{
static constexpr JPH::BroadPhaseLayer Static{0};
static constexpr JPH::BroadPhaseLayer Dynamic{1};
static constexpr uint32 kNum = 2;
} // namespace BroadPhaseLayers

class GE_BroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface
{
public:
    uint32 GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::kNum; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const override
    {
        // Simple mapping: treat layer 0 as static, everything else dynamic for now.
        return (inLayer == 0) ? BroadPhaseLayers::Static : BroadPhaseLayers::Dynamic;
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer inLayer) const override
    {
        switch (inLayer.GetValue())
        {
        case 0:
            return "Static";
        case 1:
            return "Dynamic";
        default:
            return "Unknown";
        }
    }
#endif
};

class GE_ObjectVsBroadPhaseLayerFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
    void SetMasks(const uint32* masks) { m_Masks = masks; }

    bool ShouldCollide(JPH::ObjectLayer inLayer1, JPH::BroadPhaseLayer inLayer2) const override
    {
        if (!m_Masks)
            return true;

        const uint32 l = static_cast<uint32>(inLayer1);
        if (l >= kMaxCollisionLayers)
            return false;

        const uint32 row = m_Masks[l];

        // With our current broadphase mapping:
        // - object layer 0 => BroadPhaseLayers::Static
        // - everything else => BroadPhaseLayers::Dynamic
        if (inLayer2 == BroadPhaseLayers::Static)
        {
            return (row & (1u << static_cast<uint32>(Layers::Static))) != 0u;
        }

        // Dynamic broadphase: collide with any non-static layer.
        const uint32 nonStaticMask = row & ~1u;
        return nonStaticMask != 0u;
    }

private:
    const uint32* m_Masks = nullptr;
};

class GE_ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter
{
public:
    void SetMasks(const uint32* masks) { m_Masks = masks; }

    bool ShouldCollide(JPH::ObjectLayer inObject1, JPH::ObjectLayer inObject2) const override
    {
        if (!m_Masks)
            return true;

        const uint32 a = static_cast<uint32>(inObject1);
        const uint32 b = static_cast<uint32>(inObject2);
        if (a >= kMaxCollisionLayers || b >= kMaxCollisionLayers)
            return false;

        const uint32 bitB = (1u << b);
        const uint32 bitA = (1u << a);
        return ((m_Masks[a] & bitB) != 0u) && ((m_Masks[b] & bitA) != 0u);
    }

private:
    const uint32* m_Masks = nullptr;
};

} // namespace

struct JoltPhysicsBackend::Impl
{
    enum class RawEventType : uint8
    {
        ContactAdded,
        ContactPersisted,
        ContactRemoved,
    };

    struct RawEvent
    {
        RawEventType type = RawEventType::ContactPersisted;
        JPH::SubShapeIDPair pair{};
        JPH::BodyID body1{};
        JPH::BodyID body2{};
        uint64 userData1 = 0;
        uint64 userData2 = 0;
        bool isSensor = false;
        bool body1IsSensor = false;
        bool body2IsSensor = false;
        Vector3 point{0.0f, 0.0f, 0.0f};
        Vector3 normal{0.0f, 1.0f, 0.0f};
    };

    struct CachedPair
    {
        JPH::BodyID body1{};
        JPH::BodyID body2{};
        BodyHandle handle1{};
        BodyHandle handle2{};
        uint64 userData1 = 0;
        uint64 userData2 = 0;
        bool isSensor = false;
        bool body1IsSensor = false;
        bool body2IsSensor = false;
        Vector3 lastPoint{0.0f, 0.0f, 0.0f};
        Vector3 lastNormal{0.0f, 1.0f, 0.0f};
    };

    class GE_ContactListener final : public JPH::ContactListener
    {
    public:
        explicit GE_ContactListener(Impl& impl)
            : m_Impl(impl)
        {
        }

        void OnContactAdded(const JPH::Body& inBody1,
                            const JPH::Body& inBody2,
                            const JPH::ContactManifold& inManifold,
                            JPH::ContactSettings& ioSettings) override
        {
            m_Impl.OnContact(inBody1, inBody2, inManifold, ioSettings, RawEventType::ContactAdded);
        }

        void OnContactPersisted(const JPH::Body& inBody1,
                                const JPH::Body& inBody2,
                                const JPH::ContactManifold& inManifold,
                                JPH::ContactSettings& ioSettings) override
        {
            m_Impl.OnContact(inBody1, inBody2, inManifold, ioSettings, RawEventType::ContactPersisted);
        }

        void OnContactRemoved(const JPH::SubShapeIDPair& inSubShapePair) override
        {
            if (!m_Impl.HasAnyEventCallbacks())
                return;
            RawEvent e{};
            e.type = RawEventType::ContactRemoved;
            e.pair = inSubShapePair;
            e.body1 = inSubShapePair.GetBody1ID();
            e.body2 = inSubShapePair.GetBody2ID();
            m_Impl.rawEvents.enqueue(e);
        }

    private:
        Impl& m_Impl;
    };

    bool initialized = false;

    // Debug contract: in-place shape mutation (UpdateHeightFieldRegion) is
    // only safe while no query runs concurrently (Jolt documents the race).
    // Today's scheduler guarantees it (queries run in earlier waves; waves
    // are sequential); these counters make a future violation trip an assert
    // instead of producing torn reads. Relaxed atomics — negligible cost.
    mutable std::atomic<int32_t> activeQueryCount{0};

    // Jolt global init (Factory/Types)
    bool typesRegistered = false;

    // Runtime allocators/jobs
    std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
    std::unique_ptr<JPH::JobSystem> jobSystem;

    struct CharacterSlot
    {
        CharacterMotor motor = CharacterMotor::Kinematic;
        JPH::Ref<JPH::CharacterVirtual> kinematic;
        JPH::Ref<JPH::Character> dynamicChar;
        float32 maxStepHeight = 0.4f;
        float32 gravityScale = 1.0f;
        float32 radius = 0.4f;
        JPH::ObjectLayer layer = 2;
    };

    static void ReleaseCharacterSlot(CharacterSlot& slot)
    {
        if (slot.dynamicChar)
        {
            slot.dynamicChar->RemoveFromPhysicsSystem();
            slot.dynamicChar = nullptr;
        }
        slot.kinematic = nullptr;
    }

    // Resource tables
    GenerationalVector::GenerationalVector<JPH::ShapeRefC> shapes;
    GenerationalVector::GenerationalVector<JPH::BodyID> bodies;
    GenerationalVector::GenerationalVector<CharacterSlot> characters;

    // Creation-time mapping for heightfield shapes. SetHeights expects heights
    // in the shape's local space (creationOffsetY + creationScaleY * rawSample),
    // so in-place region updates must re-apply the exact mapping the shape was
    // created with. Keyed by ShapeHandle::Value(); erased in DestroyShape.
    struct HeightFieldCreationInfo
    {
        float32 offsetY = 0.0f;
        float32 scaleY = 1.0f;
    };
    std::unordered_map<uint64_t, HeightFieldCreationInfo> heightFieldInfoByShapeHandle;

    // Reverse lookup for query results (BodyID -> BodyHandle::Value()).
    // Note: BodyID.GetIndexAndSequenceNumber() is stable for the life of the body.
    std::unordered_map<uint32_t, uint64_t> bodyHandleByBodyIdKey;

    // Collision filtering
    GE_BroadPhaseLayerInterface broadphaseInterface;
    GE_ObjectVsBroadPhaseLayerFilter objectVsBroadphase;
    GE_ObjectLayerPairFilter objectLayerPairs;
    uint32 layerCollisionMask[kMaxCollisionLayers]{};

    // Simulation
    JPH::PhysicsSystem physicsSystem;

    // Events
    ContactCallback contactCallback{};
    TriggerCallback triggerCallback{};
    moodycamel::ConcurrentQueue<RawEvent> rawEvents;
    std::unordered_map<JPH::SubShapeIDPair, CachedPair> pairCache;

    GE_ContactListener contactListener;

    Impl()
        : contactListener(*this)
    {
        pairCache.reserve(4096);
    }

    bool HasAnyEventCallbacks() const
    {
        return static_cast<bool>(contactCallback) || static_cast<bool>(triggerCallback);
    }

    bool TryMapBodyIdToHandle_MainThread(JPH::BodyID id, BodyHandle& outHandle) const
    {
        const uint32_t key = id.GetIndexAndSequenceNumber();
        auto it = bodyHandleByBodyIdKey.find(key);
        if (it == bodyHandleByBodyIdKey.end())
            return false;
        outHandle = BodyHandle(it->second);
        return true;
    }

    void OnContact(const JPH::Body& inBody1,
                   const JPH::Body& inBody2,
                   const JPH::ContactManifold& inManifold,
                   const JPH::ContactSettings& ioSettings,
                   RawEventType type)
    {
        if (!HasAnyEventCallbacks())
            return;

        RawEvent e{};
        e.type = type;

        // Construct a stable key for this body/sub-shape pair.
        // Jolt callbacks guarantee this identifies the contact (see ContactListener docs).
        const JPH::BodyID id1 = inBody1.GetID();
        const JPH::BodyID id2 = inBody2.GetID();
        e.pair = JPH::SubShapeIDPair(id1, inManifold.mSubShapeID1, id2, inManifold.mSubShapeID2);
        e.body1 = id1;
        e.body2 = id2;
        e.userData1 = static_cast<uint64>(inBody1.GetUserData());
        e.userData2 = static_cast<uint64>(inBody2.GetUserData());
        e.body1IsSensor = inBody1.IsSensor();
        e.body2IsSensor = inBody2.IsSensor();
        e.isSensor = ioSettings.mIsSensor || e.body1IsSensor || e.body2IsSensor;

        if (inManifold.mRelativeContactPointsOn1.size() > 0)
        {
            const JPH::RVec3 p = inManifold.GetWorldSpaceContactPointOn1(0);
            e.point = Vector3(static_cast<float32>(p.GetX()), static_cast<float32>(p.GetY()), static_cast<float32>(p.GetZ()));
        }
        const JPH::Vec3 n = inManifold.mWorldSpaceNormal;
        e.normal = Vector3(static_cast<float32>(n.GetX()), static_cast<float32>(n.GetY()), static_cast<float32>(n.GetZ()));

        rawEvents.enqueue(e);
    }
};

JoltPhysicsBackend::JoltPhysicsBackend() = default;
JoltPhysicsBackend::~JoltPhysicsBackend() { Shutdown(); }

bool JoltPhysicsBackend::Initialize(const PhysicsWorldSettings& settings)
{
    if (m_Impl && m_Impl->initialized)
        return true;

    if (!m_Impl)
        m_Impl = new Impl();

    // Global Jolt init (idempotent for our process)
    JPH::RegisterDefaultAllocator();
    if (JPH::Factory::sInstance == nullptr)
        JPH::Factory::sInstance = new JPH::Factory();

    if (!m_Impl->typesRegistered)
    {
        JPH::RegisterTypes();
        m_Impl->typesRegistered = true;
    }

    const uint32 maxBodies = settings.maxBodies;
    const uint32 maxBodyPairs = settings.maxBodyPairs;
    const uint32 maxContactConstraints = settings.maxContactConstraints;

    // Jolt uses a number of per-body mutexes for multithreaded contention control.
    // 0 lets Jolt choose a reasonable default.
    const uint32 numBodyMutexes = 0;

    m_Impl->physicsSystem.Init(maxBodies,
                               numBodyMutexes,
                               maxBodyPairs,
                               maxContactConstraints,
                               m_Impl->broadphaseInterface,
                               m_Impl->objectVsBroadphase,
                               m_Impl->objectLayerPairs);

    // Event hooks
    m_Impl->physicsSystem.SetContactListener(&m_Impl->contactListener);

    // Collision filtering (layer matrix).
    // Copy into Impl-owned storage so filters can hold a stable pointer.
    for (uint32 i = 0; i < kMaxCollisionLayers; ++i)
        m_Impl->layerCollisionMask[i] = settings.layerCollisionMatrix.mask[i];
    m_Impl->objectLayerPairs.SetMasks(m_Impl->layerCollisionMask);
    m_Impl->objectVsBroadphase.SetMasks(m_Impl->layerCollisionMask);

    m_Impl->physicsSystem.SetGravity(JPH::Vec3(settings.gravity.x, settings.gravity.y, settings.gravity.z));

    // Temp allocator + job system for stepping.
    // IMPORTANT: If this is too small, Jolt will assert/debugbreak on allocation failure.
    const uint32 minTempBytes = 16u * 1024u * 1024u;
    const uint32 tempBytes = std::max(minTempBytes, settings.tempAllocatorBytes);
    m_Impl->tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(tempBytes);

    const int32 wantedThreads = settings.numThreads;
    const uint32 hw = std::max(1u, std::thread::hardware_concurrency());

    // Threading policy:
    // - numThreads < 0: auto => use "hw - 1" worker threads (leave one for the main thread)
    // - numThreads == 0: force single-threaded
    // - numThreads > 0: explicit worker thread count
    uint32 numThreads = 1;
    if (wantedThreads < 0)
    {
        numThreads = (hw > 1u) ? (hw - 1u) : 1u;
    }
    else if (wantedThreads == 0)
    {
        numThreads = 1;
    }
    else
    {
        numThreads = static_cast<uint32>(wantedThreads);
    }

    // Jolt's JobSystemThreadPool is extra threads on top of the engine
    // JobSystem. Use its synchronous backend when the host cannot budget
    // another worker pool.
    if (!Platform::SupportsAuxiliaryThreadPools())
        numThreads = 1;

    if (numThreads <= 1)
    {
        // Avoid thread-pool overhead and potential deadlocks in headless/tests.
        m_Impl->jobSystem = std::make_unique<JPH::JobSystemSingleThreaded>(/*maxJobs*/ 1024);
    }
    else
    {
        // Conservative defaults (lift as needed)
        const uint32 maxJobs = 1024;
        const uint32 maxBarriers = 1024;
        m_Impl->jobSystem = std::make_unique<JPH::JobSystemThreadPool>(maxJobs, maxBarriers, numThreads);
    }

    m_Impl->initialized = true;
    Logger::Log::Info("[Physics] Jolt backend initialized (threads={})", numThreads);
    return true;
}

void JoltPhysicsBackend::Shutdown()
{
    if (!m_Impl)
        return;

    // Characters first: a Dynamic motor is a body the body table does not own.
    if (m_Impl->initialized)
    {
        m_Impl->characters.ForEach([&](GenerationalVector::Handle /*h*/, Impl::CharacterSlot& slot)
                                   {
                                       Impl::ReleaseCharacterSlot(slot);
                                   });
    }
    m_Impl->characters.Clear();

    // Destroy bodies via Jolt first (shapes may be referenced by bodies).
    if (m_Impl->initialized)
    {
        auto& bi = m_Impl->physicsSystem.GetBodyInterface();
        m_Impl->bodies.ForEach([&](GenerationalVector::Handle /*h*/, JPH::BodyID& id)
                               {
                                   bi.RemoveBody(id);
                                   bi.DestroyBody(id);
                               });
    }
    m_Impl->bodyHandleByBodyIdKey.clear();
    m_Impl->bodies.Clear();
    m_Impl->shapes.Clear();
    m_Impl->heightFieldInfoByShapeHandle.clear();

    m_Impl->jobSystem.reset();
    m_Impl->tempAllocator.reset();

    // Note: we intentionally do not delete Factory::sInstance or unregister types
    // here because multiple backends/worlds could exist within the same process.
    // We treat Jolt global init as process-lifetime.
    delete m_Impl;
    m_Impl = nullptr;
}

namespace
{
JPH::RefConst<JPH::Shape> MakeCharacterCapsule(float32 radius, float32 height)
{
    const float r = static_cast<float>(radius);
    const float h = static_cast<float>(height);
    const float cylinderHalf = 0.5f * h - r;
    if (r <= 0.0f || cylinderHalf < 1.0e-4f)
        return {};

    JPH::RefConst<JPH::Shape> capsule = new JPH::CapsuleShape(cylinderHalf, r);
    JPH::RotatedTranslatedShapeSettings offset(
        JPH::Vec3(0.0f, 0.5f * h, 0.0f), JPH::Quat::sIdentity(), capsule);
    JPH::ShapeSettings::ShapeResult result = offset.Create();
    if (result.HasError())
        return {};
    return result.Get();
}

JPH::Quat ToJoltQuat(const Quaternion& rotation)
{
    const auto& qglm = rotation.GetGLM();
    return JPH::Quat(static_cast<float>(qglm.x),
                     static_cast<float>(qglm.y),
                     static_cast<float>(qglm.z),
                     static_cast<float>(qglm.w));
}

JPH::RVec3 ToJoltPos(const Vector3& p)
{
    return JPH::RVec3(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z));
}

Vector3 FromJoltVec3(JPH::Vec3Arg v)
{
    return Vector3(static_cast<float32>(v.GetX()),
                   static_cast<float32>(v.GetY()),
                   static_cast<float32>(v.GetZ()));
}

} // namespace

void JoltPhysicsBackend::Step(float32 deltaTime, int32 collisionSteps)
{
    if (!m_Impl || !m_Impl->initialized)
        return;
    if (!m_Impl->tempAllocator || !m_Impl->jobSystem)
        return;

    if (collisionSteps < 1)
        collisionSteps = 1;

    m_Impl->physicsSystem.Update(deltaTime,
                                 collisionSteps,
                                 m_Impl->tempAllocator.get(),
                                 m_Impl->jobSystem.get());

    const JPH::Vec3 gravity = m_Impl->physicsSystem.GetGravity();
    m_Impl->characters.ForEach([&](GenerationalVector::Handle /*h*/, Impl::CharacterSlot& slot)
                               {
                                   const JPH::Vec3 scaledGravity = gravity * static_cast<float>(slot.gravityScale);
                                   const auto bpFilter =
                                       m_Impl->physicsSystem.GetDefaultBroadPhaseLayerFilter(slot.layer);
                                   const auto olFilter = m_Impl->physicsSystem.GetDefaultLayerFilter(slot.layer);
                                   const JPH::BodyFilter bodyFilter{};
                                   const JPH::ShapeFilter shapeFilter{};

                                   if (slot.kinematic)
                                   {
                                       JPH::Vec3 vel = slot.kinematic->GetLinearVelocity();
                                       vel += scaledGravity * static_cast<float>(deltaTime);
                                       slot.kinematic->SetLinearVelocity(vel);

                                       JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;
                                       if (slot.maxStepHeight > 0.0f)
                                       {
                                           const float step = static_cast<float>(slot.maxStepHeight);
                                           const float radius = static_cast<float>(slot.radius);
                                           updateSettings.mWalkStairsStepUp = JPH::Vec3(0.0f, step, 0.0f);
                                           updateSettings.mStickToFloorStepDown = JPH::Vec3(0.0f, -step, 0.0f);
                                           // Jolt's defaults (2 cm min forward, 15 cm test) are smaller
                                           // than a typical capsule radius, so a blocked character never
                                           // reaches the ledge top in one stairs attempt.
                                           updateSettings.mWalkStairsMinStepForward = std::max(0.02f, radius);
                                           updateSettings.mWalkStairsStepForwardTest = std::max(0.15f, radius + 0.05f);
                                       }
                                       else
                                       {
                                           updateSettings.mWalkStairsStepUp = JPH::Vec3::sZero();
                                           updateSettings.mStickToFloorStepDown = JPH::Vec3::sZero();
                                       }

                                       slot.kinematic->ExtendedUpdate(static_cast<float>(deltaTime),
                                                                      scaledGravity,
                                                                      updateSettings,
                                                                      bpFilter,
                                                                      olFilter,
                                                                      bodyFilter,
                                                                      shapeFilter,
                                                                      *m_Impl->tempAllocator);
                                   }
                                   else if (slot.dynamicChar)
                                   {
                                       const float separation =
                                           slot.maxStepHeight > 0.0f ? static_cast<float>(slot.maxStepHeight) : 0.1f;
                                       slot.dynamicChar->PostSimulation(separation);
                                   }
                               });
}

void JoltPhysicsBackend::OptimizeBroadphase()
{
    if (!m_Impl || !m_Impl->initialized)
        return;
    m_Impl->physicsSystem.OptimizeBroadPhase();
}

ShapeHandle JoltPhysicsBackend::CreateShape(const ShapeDefinition& definition, const Vector3& bakedScale)
{
    if (!m_Impl || !m_Impl->initialized)
        return {};

    // NOTE: bakedScale is applied here so PhysicsECS can use Transform scale.
    // For shapes that don’t support arbitrary scaling (Plane), bakedScale is ignored.
    auto makeShape = [&](const ShapeDefinition& def, const Vector3& bakedScale) -> std::optional<JPH::ShapeRefC>
    {
        return std::visit(
            [&](auto&& d) -> std::optional<JPH::ShapeRefC>
            {
                using T = std::decay_t<decltype(d)>;
                if constexpr (std::is_same_v<T, SphereShapeDef>)
                {
                    const float32 s = std::max({bakedScale.x, bakedScale.y, bakedScale.z});
                    const float r = static_cast<float>(d.radius * s);
                    if (r <= 0.0f)
                        return std::nullopt;
                    return JPH::ShapeRefC(new JPH::SphereShape(r));
                }
                else if constexpr (std::is_same_v<T, BoxShapeDef>)
                {
                    const Vector3 he(d.halfExtents.x * bakedScale.x, d.halfExtents.y * bakedScale.y, d.halfExtents.z * bakedScale.z);
                    if (he.x < 0.0f || he.y < 0.0f || he.z < 0.0f)
                        return std::nullopt;
                    return JPH::ShapeRefC(new JPH::BoxShape(JPH::Vec3(he.x, he.y, he.z)));
                }
                else if constexpr (std::is_same_v<T, CapsuleShapeDef>)
                {
                    const CapsuleShapeDef scaled = d.Scaled(bakedScale);
                    if (scaled.radius <= 0.0f || scaled.halfHeight <= 0.0f)
                        return std::nullopt;
                    // Jolt capsule is aligned to Y axis by default (cylinder half-height along Y).
                    // If we need X/Z capsules, we can wrap with RotatedTranslatedShape later; for now, Y-only.
                    if (scaled.axis != CapsuleAxis::Y)
                    {
                        Logger::Log::Warning("[Physics] Jolt capsule axis {} not supported yet; using Y axis", (int)scaled.axis);
                    }
                    return JPH::ShapeRefC(new JPH::CapsuleShape(static_cast<float>(scaled.halfHeight), static_cast<float>(scaled.radius)));
                }
                else if constexpr (std::is_same_v<T, PlaneShapeDef>)
                {
                    const Vector3 n = d.normal;
                    const float lenSq = (n.x * n.x) + (n.y * n.y) + (n.z * n.z);
                    if (lenSq <= 0.0f)
                        return std::nullopt;

                    // Normalize for stable distance math.
                    const float len = std::sqrt(lenSq);
                    const float invLen = (len > 0.0f) ? (1.0f / len) : 0.0f;
                    const JPH::Vec3 nn(n.x * invLen, n.y * invLen, n.z * invLen);
                    const float dd = static_cast<float>(d.d) * invLen;

                    // Jolt expects plane constant in the same convention: n . x + c = 0
                    const JPH::Plane p(nn, dd);
                    const float he = (d.halfExtent > 0.0f) ? static_cast<float>(d.halfExtent) : 1000.0f;
                    return JPH::ShapeRefC(new JPH::PlaneShape(p, /*material*/ nullptr, he));
                }
                else if constexpr (std::is_same_v<T, ConvexHullShapeDef>)
                {
                    if (d.points.size() < 4)
                        return std::nullopt;

                    // Jolt has a hard cap; keep it conservative.
                    if (d.points.size() > static_cast<size_t>(JPH::ConvexHullShape::cMaxPointsInHull))
                    {
                        Logger::Log::Warning(
                            "[Physics] Convex hull has {} points (max {}); refusing to create",
                            d.points.size(),
                            (int)JPH::ConvexHullShape::cMaxPointsInHull);
                        return std::nullopt;
                    }

                    JPH::Array<JPH::Vec3> pts;
                    pts.reserve(static_cast<int>(d.points.size()));
                    for (const auto& p : d.points)
                    {
                        pts.push_back(JPH::Vec3(p.x * bakedScale.x, p.y * bakedScale.y, p.z * bakedScale.z));
                    }

                    JPH::ConvexHullShapeSettings settings(pts);
                    JPH::ShapeSettings::ShapeResult res = settings.Create();
                    if (res.HasError())
                    {
                        Logger::Log::Error("[Physics] Failed to create convex hull: {}", res.GetError().c_str());
                        return std::nullopt;
                    }
                    return res.Get();
                }
                else if constexpr (std::is_same_v<T, MeshShapeDef>)
                {
                    if (d.vertices.empty() || d.indices.empty())
                        return std::nullopt;
                    if ((d.indices.size() % 3u) != 0u)
                        return std::nullopt;

                    // Jolt expects CCW triangle winding.
                    JPH::TriangleList tris;
                    tris.reserve(static_cast<int>(d.indices.size() / 3u));

                    for (size_t i = 0; i < d.indices.size(); i += 3u)
                    {
                        const uint32 i0 = d.indices[i + 0];
                        const uint32 i1 = d.indices[i + 1];
                        const uint32 i2 = d.indices[i + 2];
                        if (i0 >= d.vertices.size() || i1 >= d.vertices.size() || i2 >= d.vertices.size())
                            return std::nullopt;

                        const auto& v0 = d.vertices[i0];
                        const auto& v1 = d.vertices[i1];
                        const auto& v2 = d.vertices[i2];

                        const JPH::Vec3 p0(v0.x * bakedScale.x, v0.y * bakedScale.y, v0.z * bakedScale.z);
                        const JPH::Vec3 p1(v1.x * bakedScale.x, v1.y * bakedScale.y, v1.z * bakedScale.z);
                        const JPH::Vec3 p2(v2.x * bakedScale.x, v2.y * bakedScale.y, v2.z * bakedScale.z);
                        tris.push_back(JPH::Triangle(p0, p1, p2));
                    }

                    JPH::MeshShapeSettings settings(tris);
                    JPH::ShapeSettings::ShapeResult res = settings.Create();
                    if (res.HasError())
                    {
                        Logger::Log::Error("[Physics] Failed to create mesh shape: {}", res.GetError().c_str());
                        return std::nullopt;
                    }
                    return res.Get();
                }
                else if constexpr (std::is_same_v<T, CompoundShapeDef>)
                {
                    if (d.children.empty())
                        return std::nullopt;

                    JPH::StaticCompoundShapeSettings settings;
                    settings.mSubShapes.reserve(static_cast<int>(d.children.size()));

                    for (const auto& c : d.children)
                    {
                        const auto baseH = GenerationalVector::Handle(static_cast<uint64_t>(c.shape.Value()));
                        const JPH::ShapeRefC* baseRef = m_Impl->shapes.Get(baseH);
                        if (!baseRef || !baseRef->GetPtr())
                            return std::nullopt;

                        JPH::ShapeRefC childShape = *baseRef;

                        // Optional per-child scaling (non-uniform supported).
                        if (!(c.bakedScale.x == 1.0f && c.bakedScale.y == 1.0f && c.bakedScale.z == 1.0f))
                        {
                            const JPH::Vec3 s(c.bakedScale.x, c.bakedScale.y, c.bakedScale.z);
                            JPH::ScaledShapeSettings ss(childShape.GetPtr(), s);
                            JPH::ShapeSettings::ShapeResult scaledRes = ss.Create();
                            if (scaledRes.HasError())
                            {
                                Logger::Log::Error("[Physics] Failed to scale compound child: {}", scaledRes.GetError().c_str());
                                return std::nullopt;
                            }
                            childShape = scaledRes.Get();
                        }

                        const auto& qglm = c.rotation.GetGLM();
                        const JPH::Quat rot(static_cast<float>(qglm.x), static_cast<float>(qglm.y), static_cast<float>(qglm.z), static_cast<float>(qglm.w));
                        const JPH::Vec3 pos(c.position.x, c.position.y, c.position.z);
                        settings.AddShape(pos, rot, childShape.GetPtr());
                    }

                    JPH::ShapeSettings::ShapeResult res = settings.Create();
                    if (res.HasError())
                    {
                        Logger::Log::Error("[Physics] Failed to create compound shape: {}", res.GetError().c_str());
                        return std::nullopt;
                    }
                    return res.Get();
                }
                else if constexpr (std::is_same_v<T, HeightFieldShapeDef>)
                {
                    if (d.Samples.empty() || d.SampleCount == 0)
                        return std::nullopt;

                    // Jolt HeightFieldShapeSettings expects a flat float array of NxN samples.
                    // The offset and scale map from sample-space to world-space.
                    JPH::HeightFieldShapeSettings settings(
                        d.Samples.data(),
                        JPH::Vec3(d.Offset.x, d.Offset.y, d.Offset.z),
                        JPH::Vec3(d.Scale.x, d.Scale.y, d.Scale.z),
                        d.SampleCount);

                    // Pad the fixed 16-bit encode range so in-place edits
                    // (UpdateHeightFieldRegion) can raise/lower past the
                    // initial min/max without forcing a full rebuild.
                    {
                        float32 rawMin = std::numeric_limits<float32>::max();
                        float32 rawMax = std::numeric_limits<float32>::lowest();
                        for (const float32 s : d.Samples)
                        {
                            if (s == JPH::HeightFieldShapeConstants::cNoCollisionValue)
                                continue;
                            rawMin = std::min(rawMin, s);
                            rawMax = std::max(rawMax, s);
                        }
                        if (rawMin <= rawMax)
                        {
                            const float32 pad = (rawMax - rawMin) * kHeightFieldRangeHeadroom;
                            settings.mMinHeightValue = rawMin - pad;
                            settings.mMaxHeightValue = rawMax + pad;
                        }
                    }

                    JPH::ShapeSettings::ShapeResult res = settings.Create();
                    if (res.HasError())
                    {
                        Logger::Log::Error("[Physics] Failed to create heightfield shape: {}", res.GetError().c_str());
                        return std::nullopt;
                    }
                    auto shape = res.Get();
                    auto bounds = shape->GetLocalBounds();
                    Logger::Log::Info("[Physics] Heightfield shape created: bounds min=[{:.1f},{:.1f},{:.1f}] max=[{:.1f},{:.1f},{:.1f}]",
                        bounds.mMin.GetX(), bounds.mMin.GetY(), bounds.mMin.GetZ(),
                        bounds.mMax.GetX(), bounds.mMax.GetY(), bounds.mMax.GetZ());
                    return shape;
                }
                else
                {
                    return std::nullopt;
                }
            },
            def);
    };

    auto shapeOpt = makeShape(definition, bakedScale);
    if (!shapeOpt.has_value())
        return {};

    const GenerationalVector::Handle h = m_Impl->shapes.Create(std::move(*shapeOpt));

    // Heightfields remember their creation mapping so UpdateHeightFieldRegion
    // can convert raw provider samples into the shape's local height space.
    if (const auto* hfDef = std::get_if<HeightFieldShapeDef>(&definition))
        m_Impl->heightFieldInfoByShapeHandle[h.Value()] = Impl::HeightFieldCreationInfo{hfDef->Offset.y, hfDef->Scale.y};

    return ShapeHandle(h.Value());
}

void JoltPhysicsBackend::DestroyShape(ShapeHandle shape)
{
    if (!m_Impl)
        return;
    const auto h = GenerationalVector::Handle(static_cast<uint64_t>(shape.Value()));
    if (!m_Impl->shapes.IsValid(h))
        return;
    m_Impl->heightFieldInfoByShapeHandle.erase(h.Value());
    m_Impl->shapes.Destroy(h);
}
bool JoltPhysicsBackend::IsShapeValid(ShapeHandle shape) const
{
    if (!m_Impl)
        return false;
    return m_Impl->shapes.IsValid(GenerationalVector::Handle(static_cast<uint64_t>(shape.Value())));
}

bool JoltPhysicsBackend::UpdateHeightFieldRegion(BodyHandle body, ShapeHandle shape,
                                                 const float32* allSamples, uint32 gridN,
                                                 uint32 x, uint32 z,
                                                 uint32 sizeX, uint32 sizeZ)
{
    if (!m_Impl || !m_Impl->initialized || !m_Impl->tempAllocator)
        return false;
    if (!allSamples || gridN < 2 || sizeX == 0 || sizeZ == 0)
        return false;

    const auto shapeH = GenerationalVector::Handle(static_cast<uint64_t>(shape.Value()));
    const JPH::ShapeRefC* shapeRef = m_Impl->shapes.Get(shapeH);
    if (!shapeRef || !shapeRef->GetPtr() || (*shapeRef)->GetSubType() != JPH::EShapeSubType::HeightField)
        return false;

    const auto bodyH = GenerationalVector::Handle(static_cast<uint64_t>(body.Value()));
    const JPH::BodyID* bodyId = m_Impl->bodies.Get(bodyH);
    if (!bodyId)
        return false;

    const auto infoIt = m_Impl->heightFieldInfoByShapeHandle.find(shapeH.Value());
    if (infoIt == m_Impl->heightFieldInfoByShapeHandle.end())
        return false;
    const Impl::HeightFieldCreationInfo& info = infoIt->second;
    if (info.scaleY <= 0.0f)
        return false;

    // The backend owns the shape table; ShapeRefC is const only because Jolt
    // bodies share shapes immutably. This is the one sanctioned mutation site
    // (applied at the step-quiescent physics-init window). Jolt documents a
    // race between SetHeights and concurrent queries; the scheduler's waves
    // keep every query caller out of this window — NavigationBuildSystem, the
    // only one, is ordered after the entire physics pipeline via its
    // PhysicsWriteback dependency. The assert enforces that contract against
    // future violators, including any caller that becomes wave-concurrent.
    assert(m_Impl->activeQueryCount.load(std::memory_order_relaxed) == 0 &&
           "UpdateHeightFieldRegion while a physics query is in flight — Jolt SetHeights races queries");
    auto* hf = const_cast<JPH::HeightFieldShape*>(static_cast<const JPH::HeightFieldShape*>(shapeRef->GetPtr()));

    // A region entirely outside the grid means the provider misbehaved;
    // reject so the caller takes the rebuild path instead of silently
    // consuming the edit (advancing lastBuiltVersion with no shape change).
    if (x >= gridN || z >= gridN)
        return false;
    sizeX = std::min(sizeX, gridN - x);
    sizeZ = std::min(sizeZ, gridN - z);

    // SetHeights requires block-aligned regions; the shape's sample count is
    // itself rounded up to a block multiple (cells past gridN are permanent
    // no-collision padding, which the widened write must preserve — Jolt's
    // own constructor pads rounded-up cells with cNoCollisionValue16, so
    // writing cNoCollisionValue there matches creation exactly).
    const uint32 shapeN = hf->GetSampleCount();
    const uint32 blockSize = hf->GetBlockSize();
    // Widen one extra block in the NEGATIVE direction beyond containment:
    // SetHeights re-encodes that border block from decompressed (already
    // quantized) data, which drifts cumulatively over repeated edits along
    // the same boundary — feeding CPU truth for it avoids the drift.
    uint32 x0 = (x / blockSize) * blockSize;
    uint32 z0 = (z / blockSize) * blockSize;
    x0 = x0 >= blockSize ? x0 - blockSize : 0;
    z0 = z0 >= blockSize ? z0 - blockSize : 0;
    const uint32 x1 = std::min(((x + sizeX + blockSize - 1) / blockSize) * blockSize, shapeN);
    const uint32 z1 = std::min(((z + sizeZ + blockSize - 1) / blockSize) * blockSize, shapeN);
    const uint32 spanX = x1 - x0;
    const uint32 spanZ = z1 - z0;

    // Convert raw provider samples into the shape's local height space (the
    // exact mapping used at creation) and pre-check the fixed encode range:
    // SetHeights clamps silently, so exceeding the range must instead reject
    // the in-place path and let the caller rebuild with a re-padded range.
    std::vector<float> localHeights(static_cast<size_t>(spanX) * spanZ);
    float32 localMin = std::numeric_limits<float32>::max();
    float32 localMax = std::numeric_limits<float32>::lowest();
    for (uint32 sz = 0; sz < spanZ; ++sz)
    {
        float* row = localHeights.data() + static_cast<size_t>(sz) * spanX;
        const uint32 gz = z0 + sz;
        for (uint32 sx = 0; sx < spanX; ++sx)
        {
            const uint32 gx = x0 + sx;
            if (gx >= gridN || gz >= gridN)
            {
                row[sx] = JPH::HeightFieldShapeConstants::cNoCollisionValue;
                continue;
            }
            const float32 raw = allSamples[static_cast<size_t>(gz) * gridN + gx];
            if (raw == JPH::HeightFieldShapeConstants::cNoCollisionValue)
            {
                row[sx] = raw;
                continue;
            }
            const float32 local = info.offsetY + info.scaleY * raw;
            row[sx] = local;
            localMin = std::min(localMin, local);
            localMax = std::max(localMax, local);
        }
    }
    if (localMin <= localMax &&
        (localMin < hf->GetMinHeightValue() || localMax > hf->GetMaxHeightValue()))
        return false;

    // Capture the pre-mutation center of mass: NotifyShapeChanged needs it to
    // keep the body's position bookkeeping consistent.
    auto& bi = m_Impl->physicsSystem.GetBodyInterface();
    const JPH::Vec3 prevCOM = bi.GetShape(*bodyId)->GetCenterOfMass();

    hf->SetHeights(x0, z0, spanX, spanZ, localHeights.data(), spanX, *m_Impl->tempAllocator);

    // Mandatory: SetHeights rebuilds the shape's local bounds, but the body's
    // broadphase AABB only refreshes via NotifyShapeChanged. Without it,
    // raised terrain is invisible to queries/collision until a full rebuild.
    bi.NotifyShapeChanged(*bodyId, prevCOM, /*inUpdateMassProperties*/ false, JPH::EActivation::DontActivate);
    return true;
}

void JoltPhysicsBackend::ActivateBodiesInAABB(const AABB& worldBox)
{
    if (!m_Impl || !m_Impl->initialized)
        return;

    const JPH::AABox box(JPH::Vec3(worldBox.min.x, worldBox.min.y, worldBox.min.z),
                         JPH::Vec3(worldBox.max.x, worldBox.max.y, worldBox.max.z));
    // Statics can't be activated — only sweep the dynamic broadphase layer.
    m_Impl->physicsSystem.GetBodyInterface().ActivateBodiesInAABox(
        box,
        JPH::SpecifiedBroadPhaseLayerFilter(BroadPhaseLayers::Dynamic),
        JPH::ObjectLayerFilter{});
}

BodyHandle JoltPhysicsBackend::CreateBody(const BodySettings& settings)
{
    if (!m_Impl || !m_Impl->initialized)
        return {};

    // Resolve shape
    const auto shapeH = GenerationalVector::Handle(static_cast<uint64_t>(settings.shape.Value()));
    const JPH::ShapeRefC* shapeRef = m_Impl->shapes.Get(shapeH);
    if (!shapeRef || !shapeRef->GetPtr())
        return {};

    JPH::EMotionType mt = JPH::EMotionType::Dynamic;
    switch (settings.motionType)
    {
    case MotionType::Static:
        mt = JPH::EMotionType::Static;
        break;
    case MotionType::Kinematic:
        mt = JPH::EMotionType::Kinematic;
        break;
    case MotionType::Dynamic:
    default:
        mt = JPH::EMotionType::Dynamic;
        break;
    }

    // Quaternion is stored as (w,x,y,z) in GLM; Jolt is (x,y,z,w)
    const auto& qglm = settings.rotation.GetGLM();
    const JPH::Quat rot(static_cast<float>(qglm.x), static_cast<float>(qglm.y), static_cast<float>(qglm.z), static_cast<float>(qglm.w));
    const JPH::RVec3 pos(static_cast<float>(settings.position.x), static_cast<float>(settings.position.y), static_cast<float>(settings.position.z));

    const JPH::ObjectLayer layer = static_cast<JPH::ObjectLayer>(settings.layer);

    JPH::ShapeRefC bodyShape = *shapeRef;
    const JPH::Vec3 comOffset(settings.centerOfMassOffset.x, settings.centerOfMassOffset.y, settings.centerOfMassOffset.z);
    if (!comOffset.IsNearZero())
    {
        JPH::OffsetCenterOfMassShapeSettings offsetSettings(comOffset, bodyShape.GetPtr());
        JPH::ShapeSettings::ShapeResult offsetResult = offsetSettings.Create();
        if (offsetResult.HasError())
        {
            Logger::Log::Error("[Physics] Failed to offset center of mass: {}", offsetResult.GetError().c_str());
            return {};
        }
        bodyShape = offsetResult.Get();
    }

    JPH::BodyCreationSettings bcs(bodyShape.GetPtr(), pos, rot, mt, layer);
    bcs.mIsSensor = settings.isSensor;
    bcs.mAllowSleeping = settings.allowSleep;
    bcs.mLinearDamping = static_cast<float>(settings.linearDamping);
    bcs.mAngularDamping = static_cast<float>(settings.angularDamping);
    bcs.mGravityFactor = static_cast<float>(settings.gravityScale);
    bcs.mFriction = static_cast<float>(std::max(0.0f, settings.friction));
    bcs.mRestitution = static_cast<float>(std::clamp(settings.restitution, 0.0f, 1.0f));
    bcs.mUserData = static_cast<uint64_t>(settings.userData);
    bcs.mLinearVelocity = JPH::Vec3(settings.linearVelocity.x, settings.linearVelocity.y, settings.linearVelocity.z);
    bcs.mAngularVelocity = JPH::Vec3(settings.angularVelocity.x, settings.angularVelocity.y, settings.angularVelocity.z);
    bcs.mMotionQuality = settings.continuousCollision ? JPH::EMotionQuality::LinearCast : JPH::EMotionQuality::Discrete;

    // Mass override (dynamic only). If mass <= 0, let Jolt compute from shape.
    if (mt == JPH::EMotionType::Dynamic && settings.mass > 0.0f)
    {
        bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        bcs.mMassPropertiesOverride.mMass = static_cast<float>(settings.mass);
    }

    auto& bi = m_Impl->physicsSystem.GetBodyInterface();
    JPH::Body* body = bi.CreateBody(bcs);
    if (!body)
        return {};

    const JPH::BodyID id = body->GetID();
    const GenerationalVector::Handle h = m_Impl->bodies.Create(id);

    // Store reverse mapping for query results (BodyID -> handle)
    const uint32_t key = id.GetIndexAndSequenceNumber();
    m_Impl->bodyHandleByBodyIdKey[key] = h.Value();

    const JPH::EActivation act =
        (settings.startAwake && mt != JPH::EMotionType::Static) ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;
    bi.AddBody(id, act);

    return BodyHandle(h.Value());
}

void JoltPhysicsBackend::DestroyBody(BodyHandle body)
{
    if (!m_Impl || !m_Impl->initialized)
        return;

    const auto h = GenerationalVector::Handle(static_cast<uint64_t>(body.Value()));
    JPH::BodyID* id = m_Impl->bodies.Get(h);
    if (!id)
        return;

    auto& bi = m_Impl->physicsSystem.GetBodyInterface();
    bi.RemoveBody(*id);
    bi.DestroyBody(*id);

    m_Impl->bodyHandleByBodyIdKey.erase(id->GetIndexAndSequenceNumber());
    m_Impl->bodies.Destroy(h);
}

bool JoltPhysicsBackend::IsBodyValid(BodyHandle body) const
{
    if (!m_Impl)
        return false;
    return m_Impl->bodies.IsValid(GenerationalVector::Handle(static_cast<uint64_t>(body.Value())));
}

Transform JoltPhysicsBackend::GetBodyTransform(BodyHandle body) const
{
    Transform out{};
    if (!m_Impl || !m_Impl->initialized)
        return out;

    const auto h = GenerationalVector::Handle(static_cast<uint64_t>(body.Value()));
    const JPH::BodyID* id = m_Impl->bodies.Get(h);
    if (!id)
        return out;

    const auto& bi = m_Impl->physicsSystem.GetBodyInterface();
    const JPH::RVec3 p = bi.GetPosition(*id);
    const JPH::Quat q = bi.GetRotation(*id);

    out.position = Vector3(static_cast<float32>(p.GetX()), static_cast<float32>(p.GetY()), static_cast<float32>(p.GetZ()));
    out.rotation = Quaternion(static_cast<float32>(q.GetW()), static_cast<float32>(q.GetX()), static_cast<float32>(q.GetY()), static_cast<float32>(q.GetZ()));
    return out;
}

void JoltPhysicsBackend::SetBodyTransform(BodyHandle body, const Transform& transform, ActivationMode activation)
{
    if (!m_Impl || !m_Impl->initialized)
        return;

    const auto h = GenerationalVector::Handle(static_cast<uint64_t>(body.Value()));
    const JPH::BodyID* id = m_Impl->bodies.Get(h);
    if (!id)
        return;

    const auto& qglm = transform.rotation.GetGLM();
    const JPH::Quat rot(static_cast<float>(qglm.x), static_cast<float>(qglm.y), static_cast<float>(qglm.z), static_cast<float>(qglm.w));
    const JPH::RVec3 pos(static_cast<float>(transform.position.x), static_cast<float>(transform.position.y), static_cast<float>(transform.position.z));

    auto& bi = m_Impl->physicsSystem.GetBodyInterface();
    const JPH::EActivation act = (activation == ActivationMode::Activate) ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;
    bi.SetPositionAndRotation(*id, pos, rot, act);
}

Vector3 JoltPhysicsBackend::GetLinearVelocity(BodyHandle body) const
{
    if (!m_Impl || !m_Impl->initialized)
        return {};
    const auto h = GenerationalVector::Handle(static_cast<uint64_t>(body.Value()));
    const JPH::BodyID* id = m_Impl->bodies.Get(h);
    if (!id)
        return {};

    const auto& bi = m_Impl->physicsSystem.GetBodyInterface();
    const JPH::Vec3 v = bi.GetLinearVelocity(*id);
    return Vector3(static_cast<float32>(v.GetX()), static_cast<float32>(v.GetY()), static_cast<float32>(v.GetZ()));
}

void JoltPhysicsBackend::SetLinearVelocity(BodyHandle body, const Vector3& velocity)
{
    if (!m_Impl || !m_Impl->initialized)
        return;
    const auto h = GenerationalVector::Handle(static_cast<uint64_t>(body.Value()));
    const JPH::BodyID* id = m_Impl->bodies.Get(h);
    if (!id)
        return;

    auto& bi = m_Impl->physicsSystem.GetBodyInterface();
    bi.SetLinearVelocity(*id, JPH::Vec3(velocity.x, velocity.y, velocity.z));
}

Vector3 JoltPhysicsBackend::GetAngularVelocity(BodyHandle body) const
{
    if (!m_Impl || !m_Impl->initialized)
        return {};
    const auto h = GenerationalVector::Handle(static_cast<uint64_t>(body.Value()));
    const JPH::BodyID* id = m_Impl->bodies.Get(h);
    if (!id)
        return {};
    const auto& bi = m_Impl->physicsSystem.GetBodyInterface();
    const JPH::Vec3 v = bi.GetAngularVelocity(*id);
    return Vector3(static_cast<float32>(v.GetX()), static_cast<float32>(v.GetY()), static_cast<float32>(v.GetZ()));
}

void JoltPhysicsBackend::SetAngularVelocity(BodyHandle body, const Vector3& velocity)
{
    if (!m_Impl || !m_Impl->initialized)
        return;
    const auto h = GenerationalVector::Handle(static_cast<uint64_t>(body.Value()));
    const JPH::BodyID* id = m_Impl->bodies.Get(h);
    if (!id)
        return;
    auto& bi = m_Impl->physicsSystem.GetBodyInterface();
    bi.SetAngularVelocity(*id, JPH::Vec3(velocity.x, velocity.y, velocity.z));
}

Vector3 JoltPhysicsBackend::GetCenterOfMassPosition(BodyHandle body) const
{
    if (!m_Impl || !m_Impl->initialized)
        return {};
    const auto h = GenerationalVector::Handle(static_cast<uint64_t>(body.Value()));
    const JPH::BodyID* id = m_Impl->bodies.Get(h);
    if (!id)
        return {};
    const auto& bi = m_Impl->physicsSystem.GetBodyInterface();
    const JPH::RVec3 p = bi.GetCenterOfMassPosition(*id);
    return Vector3(static_cast<float32>(p.GetX()), static_cast<float32>(p.GetY()), static_cast<float32>(p.GetZ()));
}

void JoltPhysicsBackend::AddForce(BodyHandle body, const Vector3& force)
{
    if (!m_Impl || !m_Impl->initialized)
        return;
    const auto h = GenerationalVector::Handle(static_cast<uint64_t>(body.Value()));
    const JPH::BodyID* id = m_Impl->bodies.Get(h);
    if (!id)
        return;
    auto& bi = m_Impl->physicsSystem.GetBodyInterface();
    bi.AddForce(*id, JPH::Vec3(force.x, force.y, force.z), JPH::EActivation::Activate);
}

void JoltPhysicsBackend::AddForceAtPosition(BodyHandle body, const Vector3& force, const Vector3& worldPosition)
{
    if (!m_Impl || !m_Impl->initialized)
        return;
    const auto h = GenerationalVector::Handle(static_cast<uint64_t>(body.Value()));
    const JPH::BodyID* id = m_Impl->bodies.Get(h);
    if (!id)
        return;
    auto& bi = m_Impl->physicsSystem.GetBodyInterface();
    bi.AddForce(*id, JPH::Vec3(force.x, force.y, force.z),
                JPH::RVec3(worldPosition.x, worldPosition.y, worldPosition.z),
                JPH::EActivation::Activate);
}

void JoltPhysicsBackend::AddTorque(BodyHandle body, const Vector3& torque)
{
    if (!m_Impl || !m_Impl->initialized)
        return;
    const auto h = GenerationalVector::Handle(static_cast<uint64_t>(body.Value()));
    const JPH::BodyID* id = m_Impl->bodies.Get(h);
    if (!id)
        return;
    auto& bi = m_Impl->physicsSystem.GetBodyInterface();
    bi.AddTorque(*id, JPH::Vec3(torque.x, torque.y, torque.z), JPH::EActivation::Activate);
}

void JoltPhysicsBackend::AddImpulse(BodyHandle body, const Vector3& impulse)
{
    if (!m_Impl || !m_Impl->initialized)
        return;
    const auto h = GenerationalVector::Handle(static_cast<uint64_t>(body.Value()));
    const JPH::BodyID* id = m_Impl->bodies.Get(h);
    if (!id)
        return;
    auto& bi = m_Impl->physicsSystem.GetBodyInterface();
    bi.AddImpulse(*id, JPH::Vec3(impulse.x, impulse.y, impulse.z));
}

void JoltPhysicsBackend::AddImpulseAtPosition(BodyHandle body, const Vector3& impulse, const Vector3& worldPosition)
{
    if (!m_Impl || !m_Impl->initialized)
        return;
    const auto h = GenerationalVector::Handle(static_cast<uint64_t>(body.Value()));
    const JPH::BodyID* id = m_Impl->bodies.Get(h);
    if (!id)
        return;
    auto& bi = m_Impl->physicsSystem.GetBodyInterface();
    bi.AddImpulse(*id, JPH::Vec3(impulse.x, impulse.y, impulse.z),
                  JPH::RVec3(worldPosition.x, worldPosition.y, worldPosition.z));
}

namespace
{
// Scope guard for the query/mutation debug contract (see Impl::activeQueryCount).
struct ActiveQueryScope
{
    std::atomic<int32_t>& Count;
    explicit ActiveQueryScope(std::atomic<int32_t>& count) : Count(count)
    {
        Count.fetch_add(1, std::memory_order_relaxed);
    }
    ~ActiveQueryScope() { Count.fetch_sub(1, std::memory_order_relaxed); }
};
} // namespace

bool JoltPhysicsBackend::RayCast(const RayCastQuery& query, RayCastResult& outResult) const
{
    outResult = {};
    if (!m_Impl || !m_Impl->initialized)
        return false;
    ActiveQueryScope queryScope(m_Impl->activeQueryCount);

    const Vector3 dir = query.ray.direction;
    const float32 lenSq = dir.x * dir.x + dir.y * dir.y + dir.z * dir.z;
    if (lenSq <= 0.0f)
        return false;

    const float32 invLen = 1.0f / std::sqrt(lenSq);
    const Vector3 nd(dir.x * invLen, dir.y * invLen, dir.z * invLen);

    const JPH::RVec3 origin(static_cast<float>(query.ray.origin.x), static_cast<float>(query.ray.origin.y), static_cast<float>(query.ray.origin.z));
    const JPH::Vec3 direction(nd.x * query.maxDistance, nd.y * query.maxDistance, nd.z * query.maxDistance);

    const JPH::RRayCast ray(origin, direction);
    JPH::RayCastResult hit;

    const auto& npq = m_Impl->physicsSystem.GetNarrowPhaseQuery();
    if (!npq.CastRay(ray, hit))
        return false;

    // Map body id back to our handle
    const uint32_t key = hit.mBodyID.GetIndexAndSequenceNumber();
    auto it = m_Impl->bodyHandleByBodyIdKey.find(key);
    if (it != m_Impl->bodyHandleByBodyIdKey.end())
    {
        outResult.body = BodyHandle(it->second);
    }

    const JPH::RVec3 p = ray.GetPointOnRay(hit.mFraction);
    outResult.hitPoint = Vector3(static_cast<float32>(p.GetX()), static_cast<float32>(p.GetY()), static_cast<float32>(p.GetZ()));
    outResult.distance = query.maxDistance * static_cast<float32>(hit.mFraction);

    // Fetch normal + user data
    {
        JPH::BodyLockRead lock(m_Impl->physicsSystem.GetBodyLockInterface(), hit.mBodyID);
        if (lock.Succeeded())
        {
            const JPH::Body& b = lock.GetBody();
            const JPH::Vec3 n = b.GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, p);
            outResult.hitNormal = Vector3(static_cast<float32>(n.GetX()), static_cast<float32>(n.GetY()), static_cast<float32>(n.GetZ()));
            outResult.userData = static_cast<uint64>(b.GetUserData());
        }
    }

    return true;
}

bool JoltPhysicsBackend::RayCastAll(const RayCastQuery& query, std::vector<RayCastResult>& outResults, uint32 maxResults) const
{
    outResults.clear();
    outResults.reserve(std::min<uint32>(maxResults, 64u));

    RayCastResult first{};
    if (!RayCast(query, first))
        return false;
    outResults.push_back(first);
    return true;
}

bool JoltPhysicsBackend::BoxOverlap(const BoxOverlapQuery& query, std::vector<OverlapResult>& outResults, uint32 maxResults) const
{
    outResults.clear();
    if (!m_Impl || !m_Impl->initialized || maxResults == 0)
        return false;

    const auto finite = [](const Vector3& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    };
    const auto& q = query.rotation.GetGLM();
    const double lengthSq = double(q.x) * q.x + double(q.y) * q.y + double(q.z) * q.z + double(q.w) * q.w;
    if (!finite(query.center) || !finite(query.halfExtents) ||
        query.halfExtents.x <= 0.0f || query.halfExtents.y <= 0.0f || query.halfExtents.z <= 0.0f ||
        !std::isfinite(lengthSq) || lengthSq <= 0.0f)
        return false;

    ActiveQueryScope queryScope(m_Impl->activeQueryCount);
    class QueryLayerFilter final : public JPH::ObjectLayerFilter
    {
    public:
        explicit QueryLayerFilter(const CollisionFilter& filter) : m_Filter(filter) {}
        bool ShouldCollide(JPH::ObjectLayer layer) const override { return m_Filter.PassesLayer(layer); }
    private:
        const CollisionFilter& m_Filter;
    } layerFilter(query.filter);

    class QueryBodyFilter final : public JPH::BodyFilter
    {
    public:
        explicit QueryBodyFilter(bool ignoreSensors) : m_IgnoreSensors(ignoreSensors) {}
        bool ShouldCollideLocked(const JPH::Body& body) const override
        {
            return !m_IgnoreSensors || !body.IsSensor();
        }
    private:
        bool m_IgnoreSensors;
    } bodyFilter(query.filter.ignoreSensors);

    class OverlapCollector final : public JPH::CollideShapeCollector
    {
    public:
        struct Hit { JPH::BodyID body; uint64 userData; };
        std::vector<Hit> hits;
        explicit OverlapCollector(uint32 max) : m_MaxCount(max) {}

        void OnBody(const JPH::Body& body) override
        {
            // NarrowPhaseQuery holds the body read lock here; AddHit runs after
            // releasing it, with the transformed shape kept alive by Jolt.
            m_UserData = static_cast<uint64>(body.GetUserData());
        }

        void AddHit(const JPH::CollideShapeResult& hit) override
        {
            if (ShouldEarlyOut() || std::any_of(hits.begin(), hits.end(), [&](const Hit& old)
                { return old.body == hit.mBodyID2; }))
                return;
            // A mesh/compound may report several contacts for one body. Count
            // native bodies, including those without an engine BodyHandle.
            hits.push_back({hit.mBodyID2, m_UserData});
            if (hits.size() >= m_MaxCount)
                ForceEarlyOut();
        }
    private:
        uint32 m_MaxCount;
        uint64 m_UserData = 0;
    };

    // Broadphase bounds are only candidates: a heightfield's remote peak must
    // not obstruct empty space above a low part of that same body.
    const JPH::BoxShape box(JPH::Vec3(query.halfExtents.x, query.halfExtents.y, query.halfExtents.z), 0.0f);
    const double inverseLength = 1.0 / std::sqrt(lengthSq);
    const JPH::Quat rotation(static_cast<float>(q.x * inverseLength), static_cast<float>(q.y * inverseLength),
                             static_cast<float>(q.z * inverseLength), static_cast<float>(q.w * inverseLength));
    const JPH::RVec3 center(query.center.x, query.center.y, query.center.z);
    JPH::CollideShapeSettings settings;
    // Overlap has no travel direction; both sides of a mesh can obstruct it.
    settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
    OverlapCollector collector(maxResults);
    m_Impl->physicsSystem.GetNarrowPhaseQuery().CollideShape(
        &box, JPH::Vec3::sReplicate(1.0f), JPH::RMat44::sRotationTranslation(rotation, center),
        settings, center, collector, {}, layerFilter, bodyFilter);

    outResults.reserve(collector.hits.size());
    for (const auto& hit : collector.hits)
    {
        OverlapResult result;

        const uint32_t key = hit.body.GetIndexAndSequenceNumber();
        auto it = m_Impl->bodyHandleByBodyIdKey.find(key);
        if (it != m_Impl->bodyHandleByBodyIdKey.end())
            result.body = BodyHandle(it->second);

        result.userData = hit.userData;
        outResults.push_back(result);
    }

    return !outResults.empty();
}

void JoltPhysicsBackend::SetContactCallback(ContactCallback callback)
{
    if (!m_Impl)
        return;
    m_Impl->contactCallback = std::move(callback);
}

void JoltPhysicsBackend::SetTriggerCallback(TriggerCallback callback)
{
    if (!m_Impl)
        return;
    m_Impl->triggerCallback = std::move(callback);
}

void JoltPhysicsBackend::ProcessEvents()
{
    if (!m_Impl || !m_Impl->initialized)
        return;

    // Drain raw events captured from worker threads.
    // We do all caching and callback dispatching on the main thread here.
    Impl::RawEvent re{};
    while (m_Impl->rawEvents.try_dequeue(re))
    {
        switch (re.type)
        {
        case Impl::RawEventType::ContactAdded:
        case Impl::RawEventType::ContactPersisted:
        {
            // Update cache for removals (and to provide stable payloads).
            Impl::CachedPair c{};
            c.body1 = re.body1;
            c.body2 = re.body2;
            (void)m_Impl->TryMapBodyIdToHandle_MainThread(re.body1, c.handle1);
            (void)m_Impl->TryMapBodyIdToHandle_MainThread(re.body2, c.handle2);
            c.userData1 = re.userData1;
            c.userData2 = re.userData2;
            c.isSensor = re.isSensor;
            c.body1IsSensor = re.body1IsSensor;
            c.body2IsSensor = re.body2IsSensor;
            c.lastPoint = re.point;
            c.lastNormal = re.normal;
            m_Impl->pairCache[re.pair] = c;

            if (re.isSensor)
            {
                // Trigger enter/stay. For now we emit only enter on ContactAdded.
                if (!m_Impl->triggerCallback)
                    break;
                if (re.type != Impl::RawEventType::ContactAdded)
                    break;

                const bool body1Trigger = re.body1IsSensor; // if both are sensors, treat body1 as trigger
                TriggerEvent e{};
                e.trigger = body1Trigger ? c.handle1 : c.handle2;
                e.other = body1Trigger ? c.handle2 : c.handle1;
                e.triggerUserData = body1Trigger ? c.userData1 : c.userData2;
                e.otherUserData = body1Trigger ? c.userData2 : c.userData1;
                e.isEntering = true;
                m_Impl->triggerCallback(e);
            }
            else
            {
                if (!m_Impl->contactCallback)
                    break;

                ContactEvent e{};
                e.bodyA = c.handle1;
                e.bodyB = c.handle2;
                e.userDataA = c.userData1;
                e.userDataB = c.userData2;
                e.state = (re.type == Impl::RawEventType::ContactAdded) ? ContactState::Begin : ContactState::Persist;
                e.point = re.point;
                e.normal = re.normal;
                e.impulse = 0.0f;
                m_Impl->contactCallback(e);
            }
            break;
        }

        case Impl::RawEventType::ContactRemoved:
        {
            auto it = m_Impl->pairCache.find(re.pair);
            if (it == m_Impl->pairCache.end())
                break;
            const Impl::CachedPair c = it->second;
            m_Impl->pairCache.erase(it);

            if (c.isSensor)
            {
                if (!m_Impl->triggerCallback)
                    break;
                const bool body1Trigger = c.body1IsSensor;
                TriggerEvent e{};
                e.trigger = body1Trigger ? c.handle1 : c.handle2;
                e.other = body1Trigger ? c.handle2 : c.handle1;
                e.triggerUserData = body1Trigger ? c.userData1 : c.userData2;
                e.otherUserData = body1Trigger ? c.userData2 : c.userData1;
                e.isEntering = false;
                m_Impl->triggerCallback(e);
            }
            else
            {
                if (!m_Impl->contactCallback)
                    break;
                ContactEvent e{};
                e.bodyA = c.handle1;
                e.bodyB = c.handle2;
                e.userDataA = c.userData1;
                e.userDataB = c.userData2;
                e.state = ContactState::End;
                e.point = c.lastPoint;
                e.normal = c.lastNormal;
                e.impulse = 0.0f;
                m_Impl->contactCallback(e);
            }
            break;
        }
        }
    }
}

CharacterHandle JoltPhysicsBackend::CreateCharacter(const CharacterSettings& settings)
{
    if (!m_Impl || !m_Impl->initialized)
        return {};

    JPH::RefConst<JPH::Shape> shape = MakeCharacterCapsule(settings.radius, settings.height);
    if (!shape)
    {
        Logger::Log::Error("[Physics] Character capsule invalid (radius={}, height={})",
                           settings.radius,
                           settings.height);
        return {};
    }

    Impl::CharacterSlot slot;
    slot.motor = settings.motor;
    slot.maxStepHeight = settings.maxStepHeight;
    slot.gravityScale = settings.gravityScale;
    slot.radius = settings.radius;
    slot.layer = static_cast<JPH::ObjectLayer>(settings.layer);

    const JPH::RVec3 pos = ToJoltPos(settings.position);
    const JPH::Quat rot = ToJoltQuat(settings.rotation);
    const float maxSlope = JPH::DegreesToRadians(static_cast<float>(settings.maxSlopeAngleDegrees));

    if (settings.motor == CharacterMotor::Dynamic)
    {
        JPH::CharacterSettings cs;
        cs.mShape = shape;
        cs.mUp = JPH::Vec3::sAxisY();
        cs.mMaxSlopeAngle = maxSlope;
        cs.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -static_cast<float>(settings.radius));
        cs.mLayer = slot.layer;
        cs.mMass = static_cast<float>(settings.mass);
        cs.mGravityFactor = static_cast<float>(settings.gravityScale);
        slot.dynamicChar = new JPH::Character(&cs, pos, rot, settings.userData, &m_Impl->physicsSystem);
        slot.dynamicChar->AddToPhysicsSystem(JPH::EActivation::Activate);
    }
    else
    {
        JPH::CharacterVirtualSettings vs;
        vs.mShape = shape;
        vs.mUp = JPH::Vec3::sAxisY();
        vs.mMaxSlopeAngle = maxSlope;
        vs.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -static_cast<float>(settings.radius));
        vs.mMass = static_cast<float>(settings.mass);
        vs.mCharacterPadding = static_cast<float>(settings.skinWidth);
        slot.kinematic =
            new JPH::CharacterVirtual(&vs, pos, rot, settings.userData, &m_Impl->physicsSystem);
    }

    const GenerationalVector::Handle h = m_Impl->characters.Create(std::move(slot));
    return CharacterHandle(h.Value());
}

void JoltPhysicsBackend::DestroyCharacter(CharacterHandle character)
{
    if (!m_Impl || !m_Impl->initialized)
        return;

    const auto h = GenerationalVector::Handle(static_cast<uint64_t>(character.Value()));
    Impl::CharacterSlot* slot = m_Impl->characters.Get(h);
    if (!slot)
        return;
    Impl::ReleaseCharacterSlot(*slot);
    m_Impl->characters.Destroy(h);
}

bool JoltPhysicsBackend::IsCharacterValid(CharacterHandle character) const
{
    if (!m_Impl)
        return false;
    return m_Impl->characters.IsValid(GenerationalVector::Handle(static_cast<uint64_t>(character.Value())));
}

Transform JoltPhysicsBackend::GetCharacterTransform(CharacterHandle character) const
{
    Transform out{};
    if (!m_Impl || !m_Impl->initialized)
        return out;

    const auto h = GenerationalVector::Handle(static_cast<uint64_t>(character.Value()));
    const Impl::CharacterSlot* slot = m_Impl->characters.Get(h);
    if (!slot)
        return out;

    JPH::RVec3 p;
    JPH::Quat q;
    if (slot->kinematic)
    {
        p = slot->kinematic->GetPosition();
        q = slot->kinematic->GetRotation();
    }
    else if (slot->dynamicChar)
    {
        p = slot->dynamicChar->GetPosition();
        q = slot->dynamicChar->GetRotation();
    }
    else
        return out;

    out.position = Vector3(static_cast<float32>(p.GetX()),
                           static_cast<float32>(p.GetY()),
                           static_cast<float32>(p.GetZ()));
    out.rotation = Quaternion(static_cast<float32>(q.GetW()),
                              static_cast<float32>(q.GetX()),
                              static_cast<float32>(q.GetY()),
                              static_cast<float32>(q.GetZ()));
    return out;
}

void JoltPhysicsBackend::SetCharacterPosition(CharacterHandle character, const Vector3& position)
{
    if (!m_Impl || !m_Impl->initialized)
        return;

    const auto h = GenerationalVector::Handle(static_cast<uint64_t>(character.Value()));
    Impl::CharacterSlot* slot = m_Impl->characters.Get(h);
    if (!slot)
        return;

    const JPH::RVec3 pos = ToJoltPos(position);
    if (slot->kinematic)
        slot->kinematic->SetPosition(pos);
    else if (slot->dynamicChar)
        slot->dynamicChar->SetPosition(pos, JPH::EActivation::Activate);
}

Vector3 JoltPhysicsBackend::GetCharacterLinearVelocity(CharacterHandle character) const
{
    if (!m_Impl || !m_Impl->initialized)
        return {};

    const auto h = GenerationalVector::Handle(static_cast<uint64_t>(character.Value()));
    const Impl::CharacterSlot* slot = m_Impl->characters.Get(h);
    if (!slot)
        return {};

    if (slot->kinematic)
        return FromJoltVec3(slot->kinematic->GetLinearVelocity());
    if (slot->dynamicChar)
        return FromJoltVec3(slot->dynamicChar->GetLinearVelocity());
    return {};
}

void JoltPhysicsBackend::SetCharacterLinearVelocity(CharacterHandle character, const Vector3& velocity)
{
    if (!m_Impl || !m_Impl->initialized)
        return;

    const auto h = GenerationalVector::Handle(static_cast<uint64_t>(character.Value()));
    Impl::CharacterSlot* slot = m_Impl->characters.Get(h);
    if (!slot)
        return;

    const JPH::Vec3 v(velocity.x, velocity.y, velocity.z);
    if (slot->kinematic)
        slot->kinematic->SetLinearVelocity(v);
    else if (slot->dynamicChar)
        slot->dynamicChar->SetLinearVelocity(v);
}

bool JoltPhysicsBackend::IsCharacterGrounded(CharacterHandle character) const
{
    if (!m_Impl || !m_Impl->initialized)
        return false;

    const auto h = GenerationalVector::Handle(static_cast<uint64_t>(character.Value()));
    const Impl::CharacterSlot* slot = m_Impl->characters.Get(h);
    if (!slot)
        return false;

    const JPH::CharacterBase* base = nullptr;
    if (slot->kinematic)
        base = slot->kinematic.GetPtr();
    else if (slot->dynamicChar)
        base = slot->dynamicChar.GetPtr();
    if (!base)
        return false;
    return base->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
}

Vector3 JoltPhysicsBackend::GetCharacterGroundNormal(CharacterHandle character) const
{
    if (!m_Impl || !m_Impl->initialized)
        return Vector3(0.0f, 1.0f, 0.0f);

    const auto h = GenerationalVector::Handle(static_cast<uint64_t>(character.Value()));
    const Impl::CharacterSlot* slot = m_Impl->characters.Get(h);
    if (!slot)
        return Vector3(0.0f, 1.0f, 0.0f);

    const JPH::CharacterBase* base = nullptr;
    if (slot->kinematic)
        base = slot->kinematic.GetPtr();
    else if (slot->dynamicChar)
        base = slot->dynamicChar.GetPtr();
    if (!base)
        return Vector3(0.0f, 1.0f, 0.0f);
    return FromJoltVec3(base->GetGroundNormal());
}

} // namespace GameEngine::Physics
