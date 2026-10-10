#include "Inspectors/BuiltInInspectorRegistration.h"

#include "Editor/Registries/EditorPluginRegistry.h"
#include "Inspectors/AmbientLightInspector.h"
#include "Inspectors/AmbientOcclusionEffectInspector.h"
#include "Inspectors/AnimatorInspector.h"
#include "Inspectors/AtmosphericCloudLayerInspector.h"
#include "Inspectors/VolumetricCloudsInspector.h"
#include "Inspectors/AudioEmitterInspector.h"
#include "Inspectors/AudioInspector.h"
#include "Inspectors/BloomEffectInspector.h"
#include "Inspectors/CameraInspector.h"
#include "Inspectors/ChromaticAberrationEffectInspector.h"
#include "Inspectors/ColorFilterEffectInspector.h"
#include "Inspectors/ColorGradeEffectInspector.h"
#include "Inspectors/ContrastAdaptiveSharpenEffectInspector.h"
#include "Inspectors/CrtEffectInspector.h"
#include "Inspectors/CubeLutEffectInspector.h"
#include "Inspectors/DebandEffectInspector.h"
#include "Inspectors/DepthOfFieldEffectInspector.h"
#include "Inspectors/ExposureAdjustmentEffectInspector.h"
#include "Inspectors/FastBlurEffectInspector.h"
#include "Inspectors/FilmSimulationEffectInspector.h"
#include "Inspectors/HeatDistortionEffectInspector.h"
#include "Inspectors/HeightFogEffectInspector.h"
#include "Inspectors/HumanoidRigInspector.h"
#include "Inspectors/LensFlareSourceInspector.h"
#include "Inspectors/LightInspector.h"
#include "Inspectors/MaterialInspector.h"
#include "Inspectors/MeasureInspector.h"
#include "Inspectors/MeshRendererInspector.h"
#include "Inspectors/ModelInspector.h"
#include "Inspectors/NativeSourceInspector.h"
#include "Inspectors/OceanDepthCacheSourceInspector.h"
#include "Inspectors/OceanWaveSpectrumInspector.h"
#include "Inspectors/Particles/ParticleInspector.h"
#include "Inspectors/PhysicsBodyInspector.h"
#include "Inspectors/CharacterControllerInspector.h"
#include "Inspectors/PhysicsColliderInspector.h"
#include "Inspectors/PostProcessVolumeInspector.h"
#include "Inspectors/RenderPipelineInspector.h"
#include "Inspectors/TerrainMaterialLibraryInspector.h"
#include "Inspectors/RetargetMapInspector.h"
#include "Inspectors/ScreenSpaceReflectionsEffectInspector.h"
#include "Inspectors/ScriptInspector.h"
#include "Inspectors/ShaderInspector.h"
#include "Inspectors/ShadowSettingsEffectInspector.h"
#include "Inspectors/SkeletonProfileInspector.h"
#include "Inspectors/SkinnedMeshRendererInspector.h"
#include "Inspectors/SkyEnvironmentInspector.h"
#include "Inspectors/SkyboxInspector.h"
#include "Inspectors/SplineFenceInspector.h"
#include "Inspectors/SplineFenceSpanPieceInspector.h"
#include "Inspectors/SplineInspector.h"
#include "Inspectors/SplinePlacementInspector.h"
#include "Inspectors/TextureInspector.h"
#include "Inspectors/TransformInspector.h"
#include "Inspectors/ValueCurveInspector.h"
#include "Inspectors/VhsEffectInspector.h"
#include "Inspectors/VignetteEffectInspector.h"
#include "Inspectors/VolumetricFogEffectInspector.h"
#include "Inspectors/WindVolumeInspector.h"
#include "Terrain/TerrainHeightmapInspector.h"
#include "Terrain/TerrainInspector.h"
#include "Terrain/TerrainModifierInspectors.h"

namespace GameEngine
{

void RegisterBuiltInInspectors()
{
    RegisterTransformInspector();
    RegisterMeshRendererInspector();
    RegisterSkinnedMeshRendererInspector();
    RegisterAnimatorInspector();
    RegisterValueCurveInspector();
    RegisterAudioEmitterInspector();
    RegisterCameraInspector();
    RegisterLightInspector();
    RegisterLensFlareSourceInspector();
    RegisterMeasureInspector();
    RegisterSkyEnvironmentInspector();
    RegisterAmbientLightInspector();
    RegisterSkyboxInspector();
    RegisterOceanDepthCacheSourceInspector();
    RegisterOceanWaveSpectrumInspector();
    RegisterParticleInspector();
    RegisterPostProcessVolumeInspector();
    RegisterWindVolumeInspector();
    RegisterBloomEffectInspector();
    RegisterChromaticAberrationEffectInspector();
    RegisterDepthOfFieldEffectInspector();
    RegisterFilmSimulationEffectInspector();
    RegisterAmbientOcclusionEffectInspector();
    RegisterScreenSpaceReflectionsEffectInspector();
    RegisterColorGradeEffectInspector();
    RegisterContrastAdaptiveSharpenEffectInspector();
    RegisterExposureAdjustmentEffectInspector();
    RegisterCrtEffectInspector();
    RegisterFastBlurEffectInspector();
    RegisterHeatDistortionEffectInspector();
    RegisterVhsEffectInspector();
    RegisterHeightFogEffectInspector();
    RegisterColorFilterEffectInspector();
    RegisterCubeLutEffectInspector();
    RegisterDebandEffectInspector();
    RegisterVignetteEffectInspector();
    RegisterShadowSettingsEffectInspector();
    RegisterAtmosphericCloudLayerInspector();
    RegisterVolumetricCloudsInspector();
    RegisterVolumetricFogEffectInspector();
    RegisterPhysicsBodyInspector();
    RegisterCharacterControllerInspector();
    RegisterPhysicsColliderInspectors();
    RegisterTerrainInspector();
    RegisterTerrainModifierInspectors();
    RegisterSplineInspectors();
    RegisterSplinePlacementInspector();
    RegisterSplineFenceInspector();
    RegisterSplineFenceSpanPieceInspector();

    RegisterMaterialInspector();
    RegisterModelInspector();
    RegisterTextureInspector();
    RegisterAudioInspector();
    RegisterScriptInspector();
    RegisterNativeSourceInspector();
    RegisterShaderInspector();
    RegisterRenderPipelineInspector();
    RegisterTerrainMaterialLibraryInspector();
    RegisterTerrainHeightmapInspector();
    RegisterSkeletonProfileInspector();
    RegisterHumanoidRigInspector();
    RegisterRetargetMapInspector();

    Editor::EditorPluginRegistry::Get().RegisterInspectors();
}

} // namespace GameEngine
