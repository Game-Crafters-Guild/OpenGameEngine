// Reflected component types: a hand-written draft of the seven components the pages
// touch. The scanner's --emit-dts generates this file from the engine's component
// headers and names every component; the generated file replaces this draft when it lands.
//
// What the generator emits, and this draft with it:
// - Per component `X`: `export interface X { ... }` with one member per reflected field, and
//   `export declare const X: ComponentType<X>`, the value `entity.get / set / add / remove /
//   has` take. A component marked @ge-no-add is typed `ComponentType<X> & NoAdd`, so
//   `entity.add(X)` and `entity.remove(X)` are type errors.
// - Member names are the reflected names in camelCase: a leading capital run is lowercased,
//   except its last capital when a lowercase letter follows (`FovY` -> `fovY`, `MeshNameId` ->
//   `meshNameId`, `MSAASamples` -> `msaaSamples`). The generated JSON carries both the
//   reflected `name` and this `tsName`; the facade maps one to the other on every ABI call.
// - Kinds: Bool -> boolean; 8 to 32-bit integers, Float and Double -> number; 64-bit
//   integers -> bigint; Vec3 -> Vec3; Color -> Color; AssetGuid -> AssetRef; EntityHandle ->
//   Entity | null (null is the unset handle); an enum -> a union of its C++ enumerator names; a float array whose extent is a
//   literal 2, 3, 4, 9 or 16 -> a tuple of that length, any other array -> number[].
// - Left out: fields marked @ge-hidden or [DoNotSerialize], fields of no reflected kind (such
//   as LocalBounds' box, which `entity.bounds` reads instead), and @ge-editor-only components.
// - Units are the engine's: radians for angles unless a field says degrees, meters, lux when
//   a light's intensityUnit is 'Lux'. The hand-written views (`entity.transform`, `scene.sun`,
//   `scene.sky`) use degrees where the engine uses radians.

import type { AssetRef, ComponentType, Entity, NoAdd } from './opengine';

// ---------------------------------------------------------------------------------------
// Transform (Components/Transform.h). @ge-no-add: every entity has one.

export interface Transform {
    /** 4x4 local-to-parent matrix, column-major, left-handed (+X right, +Y up, +Z forward). */
    matrix: [
        number, number, number, number,
        number, number, number, number,
        number, number, number, number,
        number, number, number, number,
    ];
}
export declare const Transform: ComponentType<Transform> & NoAdd;

// ---------------------------------------------------------------------------------------
// Light (Components/Rendering/Light.h). Directional and spot lights shine along the entity's +Z.

export type LightType = 'Directional' | 'Point' | 'Spot' | 'Ambient' | 'Area' | 'Volume';
export type AreaLightShape = 'Rectangle' | 'Disc' | 'Sphere' | 'Cylinder';
export type LightFalloff = 'PhysicalInverseSquare' | 'Linear' | 'SmoothRange' | 'Custom';
export type LightUnit = 'Unitless' | 'Lux' | 'Lumen' | 'Candela';
export type LightShadowTier = 'Inherit' | 'Low' | 'Medium' | 'High';

export interface Light {
    type: LightType;
    /** Linear RGB, 0..1. */
    color: [number, number, number];
    /** In the unit `intensityUnit` names: lux for a directional light, lumen or candela for a punctual one. */
    intensity: number;
    /** Meters (point and spot). */
    range: number;
    /** Radians (spot). */
    innerAngle: number;
    /** Radians (spot). */
    outerAngle: number;
    areaShape: AreaLightShape;
    /** Meters. */
    areaWidth: number;
    /** Meters. */
    areaHeight: number;
    /** Meters. */
    areaRadius: number;
    falloff: LightFalloff;
    decay: number;
    fogContribution: number;
    fogDensityBoost: number;
    fogAnisotropy: number;
    fogOriginFade: number;
    castsLight: boolean;
    castsShadows: boolean;
    /** Directional only. */
    cascadeCount: number;
    shadowResolutionTier: LightShadowTier;
    /** Degrees: the light's disc seen from the receiver (directional only); 0.53 is the sun. */
    shadowAngularDiameter: number;
    useColorTemperature: boolean;
    /** Kelvin; 6500 is neutral. */
    colorTemperature: number;
    intensityUnit: LightUnit;
}
export declare const Light: ComponentType<Light>;

// ---------------------------------------------------------------------------------------
// LocalBounds (Components/Rendering/LocalBounds.h). [DoNotSerialize]: derived from the mesh.
// Its box has no reflected kind; `entity.bounds` reads it.

export interface LocalBounds {
    dynamicObject: boolean;
    castShadows: boolean;
}
export declare const LocalBounds: ComponentType<LocalBounds>;

// ---------------------------------------------------------------------------------------
// MeshRenderer (Components/Rendering/MeshRenderer.h).

export interface MeshRenderer {
    /** Submesh index within the source model. */
    meshId: number;
    /** Submesh selector by source-node name hash; used when meshId is 0. */
    meshNameId: bigint;
    /** Runtime GPU mesh handle; 0 until the renderer binds the model. */
    meshGpuHandleId: bigint;
    materialAssetGuid: AssetRef;
    modelAssetGuid: AssetRef;
    renderLayerMask: number;
    castShadows: boolean;
    receiveShadows: boolean;
    motionVectors: boolean;
}
export declare const MeshRenderer: ComponentType<MeshRenderer>;

// ---------------------------------------------------------------------------------------
// Camera (Components/Rendering/Camera.h). The camera looks along the entity's +Z.

export type ExposureMode = 'Fixed' | 'Manual' | 'Physical' | 'Auto';
export type CameraSensorPreset =
    | 'Custom' | 'Standard8' | 'Super8' | 'Film16' | 'Super16' | 'Film35TwoPerf' | 'Film35ThreePerf'
    | 'Film35Academy' | 'Super35' | 'Film65FivePerf' | 'Imax15Perf' | 'MicroFourThirds' | 'ApsC'
    | 'FullFrame' | 'MediumFormat4433' | 'MediumFormat5440' | 'ArriAlexa35' | 'ArriAlexaLF' | 'ArriAlexa65';

export interface Camera {
    perspective: boolean;
    /** Vertical field of view, degrees. */
    fovY: number;
    /** Vertical extent in meters when `perspective` is false. */
    orthographicSize: number;
    /** Meters. */
    nearZ: number;
    /** Meters. */
    farZ: number;
    cullingMask: number;
    postProcessProfileId: number;
    /** 0 = the engine default, 1 = off, 2, 4 or 8 samples. */
    msaaSamples: number;
    /** 0 = the engine default; otherwise the anti-aliasing mode + 1. */
    antiAliasing: number;
    /** 0 = inherit the engine's render scale; otherwise a fixed scale. */
    renderScale: number;
    postProcessMask: number;
    exposureControl: ExposureMode;
    /** Fixed mode: linear multiplier. */
    exposure: number;
    /** Manual mode: EV100. */
    manualExposureEV: number;
    /** Stops, every mode. */
    exposureCompensation: number;
    /** f-number. */
    aperture: number;
    /** Seconds (Physical mode). */
    shutterTime: number;
    iso: number;
    /** Meters. */
    focusDistance: number;
    focusDebugMode: number;
    focusDebugAlpha: number;
    apertureBladeCount: number;
    apertureRoundness: number;
    /** Degrees. */
    apertureRotation: number;
    anamorphicSqueeze: number;
    sensorPreset: CameraSensorPreset;
    /** Millimeters. */
    sensorHeightMm: number;
    autoExposureMinEv: number;
    autoExposureMaxEv: number;
    autoExposureSpeedUp: number;
    autoExposureSpeedDown: number;
    aspectPreset: number;
    customAspectWidth: number;
    customAspectHeight: number;
    pixelPerfect: boolean;
    pixelPerfectPixelsPerUnit: number;
    pixelPerfectReferenceWidth: number;
    pixelPerfectReferenceHeight: number;
    pixelPerfectPixelSnap: boolean;
    pixelPerfectExpand: boolean;
}
export declare const Camera: ComponentType<Camera>;

// ---------------------------------------------------------------------------------------
// SkyEnvironment (Components/Rendering/SkyEnvironment.h).
//
// DRAFT SUBSET: 15 of the 82 reflected fields, the ones that place the sun and set the sky's
// overall look. The generated file carries all of them.

export type SkyMode = 'Physical' | 'Gradient';
export type SkySunPathKind = 'Earth' | 'Custom';
export type SkySunIlluminanceSource = 'Light' | 'Curve';

export interface SkyEnvironment {
    mode: SkyMode;
    /** Local solar time in hours; 12 is solar noon. */
    timeOfDayHours: number;
    /** Degrees north (positive) or south of the equator, -90..90. */
    latitude: number;
    /** 1 = 1 January, 172 = 21 June, 355 = 21 December. */
    dayOfYear: number;
    /** Degrees clockwise from above, 0 = +Z, 90 = +X: where north points. */
    northHeading: number;
    sunPath: SkySunPathKind;
    /** When true, timeOfDayHours advances every frame. */
    animateTimeOfDay: boolean;
    /** Wall-clock seconds per 24 hours when animateTimeOfDay is on. */
    timeOfDayCycleSeconds: number;
    /** Stops. */
    skyExposureTrim: number;
    iblIntensity: number;
    /** The directional light the sky drives, or null for none. */
    sunLight: Entity | null;
    timeOfDayDrivesSunLight: boolean;
    driveSunColor: boolean;
    sunIlluminanceSource: SkySunIlluminanceSource;
    showSun: boolean;
}
export declare const SkyEnvironment: ComponentType<SkyEnvironment>;

// ---------------------------------------------------------------------------------------
// DDGIVolume (Components/Rendering/DDGIVolume.h): dynamic diffuse global illumination. A box of
// probes, sized and placed by the entity's transform (a unit cube scaled by Transform.scale),
// that traces how light bounces between surfaces and the sky and lights everything inside it.
//
// DRAFT SUBSET: 12 of the 28 reflected fields, the ones a page sets. The generated file
// carries all of them.

export type DDGIVolumeFit = 'Manual' | 'FollowCamera';
export type DDGIProbePlacement = 'Grid' | 'Adaptive';
export type DDGICascadeMode = 'SingleGrid' | 'Cascaded2';
export type DDGIDebugView = 'None' | 'Coverage' | 'ProbeState' | 'CascadeWeight' | 'NearestProbeIrradiance' |
    'ProbeStateFine' | 'NearestProbeIrradianceFine' | 'FineProbeCoord' | 'FineProbeSlot';

export interface DDGIVolume {
    /** Manual: the transform places and sizes the grid. FollowCamera: re-centred on the camera, snapped to whole probe cells; followRange and followHeightFraction size it, the transform's scale does not. */
    fit: DDGIVolumeFit;
    /** Meters the grid reaches from the camera when fit is 'FollowCamera'. */
    followRange: number;
    /** Vertical reach of a 'FollowCamera' grid, as a fraction of followRange. */
    followHeightFraction: number;
    /** 'Adaptive' moves probes out of geometry and switches off the ones that stay buried. */
    probePlacement: DDGIProbePlacement;
    /** Probes along the grid's longest axis, 2..32; cost grows roughly with its cube. */
    probesLongAxis: number;
    /** Rays per probe per update, 32..256. */
    raysPerProbe: number;
    /** Multiplier on the indirect light the volume adds. */
    intensity: number;
    /** 0: one bounce; about 1: full multi-bounce. */
    bounceIntensity: number;
    /** How much sky light probe rays that hit nothing pick up; 1 is the sky's own brightness. */
    skyIntensity: number;
    /** 'Cascaded2' adds a finer grid over the middle of the volume at about twice the cost. */
    cascades: DDGICascadeMode;
    /** Reflections from the probes on metals and smooth surfaces. */
    enableGlossy: boolean;
    /** Draws what the probes resolved instead of lit shading; diagnostic only. */
    debugView: DDGIDebugView;
}
export declare const DDGIVolume: ComponentType<DDGIVolume>;
