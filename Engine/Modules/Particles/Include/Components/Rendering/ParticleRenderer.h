#pragma once

#include "Components/AssetRef.h"
#include "Types/Color.h"
#include "Types/Types.h"

#include <type_traits>

namespace GameEngine::Components
{

/// Meshes one particle can draw; a particle with none draws a sprite.
inline constexpr uint32 ParticleMaxMeshes = 4u;

/// The order blended particles draw in. Blending composites back to front, so every order draws the
/// farther first: ViewDepth by each particle's own depth, the others by the depth of the emitter's
/// origin, keeping one emitter's particles together in the order named.
enum class ParticleDrawOrder : uint32
{
    /// In spawn order, the newest on top.
    Spawn = 0,
    /// The youngest first, the oldest on top.
    Lifetime = 1,
    /// The oldest first, the youngest on top.
    ReverseLifetime = 2,
    /// Every particle by its own distance from the camera, across emitters.
    ViewDepth = 3,
};

/// How a particle's quad or mesh is oriented. The values are the shader's alignment codes.
enum class ParticleBillboard : uint32
{
    /// Keeps the emitter's plane.
    WorldOriented = 0,
    /// Faces the camera plane.
    FaceCamera = 1,
    /// Up axis along the velocity in 3D.
    YAlongVelocity = 2,
    /// Faces the camera, up axis along the velocity as seen on screen.
    FaceCameraYAlongVelocity = 3,
    /// Turns toward the camera around the emitter's up axis.
    LockedToEmitterUp = 4,
    /// Faces the camera's position rather than its plane.
    FaceCameraPosition = 5,
    /// Lies flat, facing up.
    Horizontal = 6,
    /// Forward axis along the particle's motion, for meshes.
    ZAlongVelocity = 7,
};

enum class ParticleLightingMode : uint32
{
    /// The authored colour and emission relative to the view's exposure: 1 draws as the display's
    /// white in daylight and at night alike.
    Unlit = 0,
    /// Lit as a uniform volume by the scene's lights and environment; emission is scene light.
    Lit = 1,
    /// Lit from six baked directional response maps, for volumetric smoke and fire.
    SixWay = 2,
};

/// How a pair of six-way response maps packs its six directions.
enum class ParticleSixWayLayout : uint32
{
    /// Map A holds +X/+Y/+Z, map B -X/-Y/-Z.
    SignedAxes = 0,
    /// Map A holds right/top/back/opacity, map B left/bottom/front/emission.
    RightTopBackRgba = 1,
    /// Map A holds top/left/right, map B bottom/back/front.
    TopLeftRightBottomBackFront = 2,
};

/// How an emitter's particles draw: material, texture sheet, lighting, orientation, meshes, trails and
/// distance thinning. Optional: an emitter without one draws white unlit sprites facing the camera.
struct ParticleRenderer
{
    // @ge-tooltip Material asset the particles draw with; its properties and textures apply, the particle shaders stay
    MaterialRef Material{};
    // @ge-tooltip RGBA atlas, cells counted left to right, then top to bottom
    TextureRef Texture{};
    // @ge-tooltip Horizontal cells of the texture sheet
    // @ge-range 1 256
    uint32 Columns = 1u;
    // @ge-tooltip Vertical cells of the texture sheet
    // @ge-range 1 256
    uint32 Rows = 1u;
    // @ge-tooltip Cells the sheet plays; 0 plays every cell
    uint32 FrameCount = 0u;
    // @ge-tooltip Cells per second; 0 plays the sheet once over each particle's life
    // @ge-range 0
    float32 FrameRate = 0.0f;
    // @ge-tooltip First cell, fractional when blending
    // @ge-range 0
    float32 StartFrame = 0.0f;
    // @ge-tooltip Loop the sheet when it plays at a frame rate
    bool Loop = true;
    // @ge-tooltip Crossfade between cells; off shows each cell whole, for hand-drawn frames
    bool BlendFrames = true;

    // @ge-tooltip Unlit, lit by the scene's lights, or lit through six baked response maps
    ParticleLightingMode Lighting = ParticleLightingMode::Unlit;
    // @ge-tooltip First six-way response map; import it as linear data
    TextureRef SixWayMapA{};
    // @ge-tooltip Second six-way response map; import it as linear data
    TextureRef SixWayMapB{};
    // @ge-tooltip How the two response maps pack the six directions
    ParticleSixWayLayout SixWayLayout = ParticleSixWayLayout::SignedAxes;
    // @ge-tooltip Power on the directional response; 1 keeps the maps as baked
    // @ge-range 0
    float32 SixWayContrast = 1.0f;
    // @ge-tooltip Optional RGB emission atlas, played with the texture sheet
    TextureRef EmissionTexture{};
    // @ge-tooltip Color of the particles' own light
    ColorLinear EmissionColor{1.0f, 1.0f, 1.0f, 1.0f};
    // @ge-tooltip Emission multiplier, 0 emits nothing. Unlit: 1 adds the display's white at any exposure. Lit: scene light, 1 is 203 nits
    // @ge-range 0
    float32 EmissionIntensity = 0.0f;

    // @ge-tooltip How each particle's quad or mesh faces; facing the camera's position keeps a particle close to a wide-angle camera from spreading across the view
    ParticleBillboard Billboard = ParticleBillboard::FaceCameraPosition;
    // @ge-tooltip Particles take the emitter's scale on top of their own size
    bool InheritScale = false;
    // @ge-tooltip Extra length along the motion per unit of speed
    // @ge-range 0
    float32 VelocityStretch = 0.0f;
    // @ge-tooltip The order particles draw in; View Depth sorts each particle by its distance from the camera
    ParticleDrawOrder DrawOrder = ParticleDrawOrder::ViewDepth;
    // @ge-tooltip Meshes each particle draws, first submesh of each; none draws a sprite
    ModelRef Meshes[ParticleMaxMeshes]{};
    // @ge-tooltip View layers that draw the particles
    uint32 RenderLayerMask = 1u;
    // @ge-tooltip Distance from the camera in metres at which particles begin to thin out
    // @ge-range 0
    float32 ThinningStart = 0.0f;
    // @ge-tooltip Distance from the camera in metres at which every particle is hidden; 0 turns thinning off
    // @ge-range 0
    float32 ThinningEnd = 0.0f;

    // @ge-tooltip Draw only the trails, not the particles that lay them
    bool TrailOnly = false;
    // @ge-tooltip Trails take the particle's color instead of white
    bool TrailInheritColor = false;
    // @ge-tooltip Trail width follows the particle's size
    bool TrailSizeAffectsWidth = false;
    // @ge-tooltip Fade toward both ends of the trail instead of over its age
    bool TrailFadeOverLength = false;
    // @ge-tooltip Smallest width multiplier, picked once per trail
    // @ge-range 0
    float32 TrailWidthMin = 1.0f;
    // @ge-tooltip Largest width multiplier, picked once per trail
    // @ge-range 0
    float32 TrailWidthMax = 1.0f;
    // @ge-tooltip Opacity at the most opaque point of the trail
    // @ge-range 0 1
    float32 TrailAlphaPeak = 1.0f;
    // @ge-tooltip Repeat the texture by distance travelled instead of stretching it over the trail
    bool TrailTextureTile = false;
    // @ge-tooltip Texture repeats along the trail
    // @ge-range 0
    float32 TrailTextureScaleU = 1.0f;
    // @ge-tooltip Texture repeats across the trail
    // @ge-range 0
    float32 TrailTextureScaleV = 1.0f;

    bool operator==(const ParticleRenderer&) const = default;
};

static_assert(std::is_trivially_copyable_v<ParticleRenderer>, "ParticleRenderer must be trivially copyable for ECS");
static_assert(std::is_standard_layout_v<ParticleRenderer>, "ParticleRenderer must be standard layout for ECS");

} // namespace GameEngine::Components
