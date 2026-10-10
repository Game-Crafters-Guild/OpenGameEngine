#pragma once

#include "ECS/Entity.h"
#include "Engine/Rendering/DDGIEligibleView.h"
#include "Mathematics/Vector3.h"
#include "ECS/Systems.h"
#include "Types/Types.h"

namespace GameEngine { namespace Engine::Renderer {

class RenderServices;
struct DDGIVolumeDesc;

// Extracts the (0 or 1) active DDGIVolume component each frame and pushes the
// resolved world-space grid snapshot into DDGIProbeFeature — a sibling of
// AmbientLightSystem's shape (same Camera-phase extraction, same "first
// enabled wins, warn on multiple" v1 policy). Component-only authoring: with
// no DDGIVolume present, the pushed desc has Enabled=false and
// DDGIProbeFeature declares nothing that frame.
class DDGIVolumeSystem : public ECS::ISystem {
public:
    explicit DDGIVolumeSystem(RenderServices* renderServices)
        : m_RenderServices(renderServices)
    {
    }

    const char* GetName() const override { return "DDGIVolumeSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

    // Pure extraction: scan the world for the first enabled DDGIVolume
    // (first-wins) and resolve it (component + WorldTransform position) into
    // the GPU-facing snapshot, stamped with world.GetWorldId() for
    // DDGIProbeFeature's NEE light lookup (RenderServices::GetWorldLights).
    // Returns a disabled default when none is present. Split out so it is
    // testable without a device.
    static DDGIVolumeDesc ResolveActiveVolume(ECS::World& world);

private:
    // Re-centres `desc` on the camera when it asks for DDGIVolumeFit::FollowCamera.
    // Kept off ResolveActiveVolume deliberately: that stays a pure, device-free
    // extraction, while following needs the view registry and a centre that
    // persists across frames.
    void ApplyCameraFit(DDGIVolumeDesc& desc);

    RenderServices* m_RenderServices = nullptr;

    // Elects the camera a FollowCamera volume tracks — the most recently
    // moved eligible view, observed rather than host-declared.
    DDGIFollowCameraTracker m_FollowTracker;

    // Centre a following volume is currently pinned at, in world space. Held
    // across frames because re-centring is HYSTERETIC: the grid moves only once
    // the camera leaves an inner margin, not every time it moves. Every move
    // costs a full field clear (DDGIProbeFeature::SetActiveVolume treats a
    // GridMinWS change as geometry churn), so moving on the smallest camera
    // twitch would keep the field permanently un-converged.
    Mathematics::Vector3 m_FollowCentreWS{};
    bool m_HasFollowCentre = false;
};

} } // namespace GameEngine::Engine::Renderer
