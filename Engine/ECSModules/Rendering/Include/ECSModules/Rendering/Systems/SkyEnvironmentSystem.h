#pragma once

#include "AssetCore/GUID.h"
#include "ECS/Entity.h"
#include "ECS/Systems.h"
#include "Types/Types.h"

namespace GameEngine { namespace Components { struct SkyEnvironment; } }
namespace GameEngine { namespace Rendering { struct SolarFrame; } }

namespace GameEngine { namespace Engine::Renderer {

class RenderServices;

class SkyEnvironmentSystem : public ECS::ISystem {
public:
    explicit SkyEnvironmentSystem(RenderServices* renderServices)
        : m_RenderServices(renderServices)
    {
    }

    const char* GetName() const override { return "SkyEnvironmentSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

private:
    // When TimeOfDayDrivesSunLight is false, the linked directional light is the
    // authoritative sun: this reads its world direction (parent-composition-aware)
    // and adopts it as a manual sun override on the local `comp`, never writing
    // the light. The driving mode (TimeOfDayDrivesSunLight) is handled separately
    // by WriteLinkedSunDirection, which still requires an unparented light. The hour
    // shown for the light's direction is read on the sky's own solar path.
    void SyncSunLight(ECS::World& world, Components::SkyEnvironment& comp,
                      const Rendering::SolarFrame& solarFrame);

    // Follows which light the sky drives and which of its fields, frame to frame
    // (SkySunIlluminance::FieldsSkyDrives). Every field the sky drove last frame and does not drive
    // this frame is handed back, whatever ended it: the colour unchecked, the source switched, the
    // link cleared or moved, the drive or the sky turned off, the sky's component or entity removed
    // or deactivated, an HDRI skybox taking over. A field is handed back only while the light still
    // holds the value the sky wrote; a value put there since (an undo, an edit) is the author's and
    // stays. The colour returns to white; the Intensity to the author's value recorded when the curve
    // started writing it, or to the sky's noon reference when no start was seen.
    //
    // A scene switch clears the world and recycles its entities, so the tracking is dropped whenever
    // the world or its lifecycle reset generation changes: nothing recorded in one scene reaches the
    // next, and the next scene's first frame counts as one with no previous update.
    //
    // It also records that authored value: on the first frame the curve writes the Intensity of the
    // light the sky's link named on the previous update, where it did not write it. With no previous
    // update (a scene that opens with the curve already driving) nothing is recorded, because the
    // Intensity on disk is then a value the sky derived.
    void TrackDrive(ECS::World& world, ECS::EntityHandle candidate, ECS::EntityHandle drivenThisFrame,
                    uint8 fieldsThisFrame);

    void ReleaseEndedFields(ECS::World& world, ECS::EntityHandle light, uint8 endedFields);

    RenderServices* m_RenderServices = nullptr;
    float32 m_AccumulatedSkyTime = 0.0f;
    // Last skybox HDRI GUID we synced GPU texture eviction for (Polyhaven resolution swaps, etc.).
    GUID m_LastSkyboxHdriEvictionGuid{};

    // The light the sky drove last frame, the fields it drove (SkySunIlluminance::kDrives*), and the
    // last colour and Intensity it wrote there.
    ECS::EntityHandle m_DrivenLight{};
    uint8 m_DrivenFields = 0;
    float32 m_DrivenLightColor[3] = {0.0f, 0.0f, 0.0f};
    float32 m_DrivenLightIntensity = 0.0f;
    // The sky's noon reference while the curve drove, in lux: the Intensity a light is handed back
    // when no authored value was recorded.
    float32 m_DrivenNoonReferenceLux = 0.0f;
    // The light the rendered sky's link named on the previous update (drivable, drive on or off), and
    // whether a previous update ran at all.
    ECS::EntityHandle m_PreviousCandidate{};
    bool m_HasPreviousUpdate = false;
    // The author's Intensity, in lux, recorded when the curve started writing `m_AuthoredIntensityLight`.
    ECS::EntityHandle m_AuthoredIntensityLight{};
    float32 m_AuthoredIntensityLux = 0.0f;
    // The world the tracking above belongs to, and its lifecycle reset generation.
    uint64 m_TrackedWorld = ~uint64{0};
    uint64 m_TrackedReset = ~uint64{0};

    // One warning per episode while the sky's sun — the linked light if one is set, otherwise the
    // scene's brightest directional — has no intensity for the physical sky to take its brightness
    // from; cleared again as soon as one does.
    bool m_WarnedNoSceneSun = false;
};

} } // namespace GameEngine::Engine::Renderer
