# Engine library
set(ENGINE_SOURCES
	Source/Core/Application.cpp
    Source/Core/Time.cpp
    Source/Core/CpuProfiler.cpp
    Source/Core/DebugMetrics.cpp
    Source/Core/NvtxRange.cpp
    Source/Core/DeferredActionQueue.cpp
    Source/Core/Engine.cpp
    Source/Core/EngineLoggerBridge.cpp
    Source/Core/WindowInputRouter.cpp
    Source/Core/WindowUiBootstrap.cpp
    Source/Core/PathUtils.cpp
    Source/Core/StandardPaths.cpp
    # Source/Components/ComponentReflection.cpp is generated at build time as
    # ComponentReflection.gen.cpp by the ComponentScanner tool — wired in via
    # target_sources(Engine ...) in the codegen block near the bottom of this file.
    Source/Assets/NavGridAsset.cpp
    Source/Assets/NavMeshAsset.cpp
    Source/Assets/SceneAsset.cpp
    Source/Assets/CoreAssetRegistrations.cpp
    Source/Assets/TextureAsset.cpp
    Source/Assets/TextureParser.cpp
    Source/Assets/PackagedTexturePayload.cpp
    Source/Assets/TextureAlphaDecodeProbe.cpp
    Source/Assets/AlphaCutoffThreshold.cpp
    Source/Assets/TextureCook.cpp
    Source/Assets/TextureCookWorkers.cpp
    Source/Assets/AssetDecodeGate.cpp
    Source/Assets/Textures/BC7TransparentEndpointRepair.cpp
    Source/Assets/Textures/TextureMimeType.cpp
    Source/Assets/AssetDecodeCancellation.cpp
    Source/Assets/SvgRasterizer.cpp
    Source/Assets/AudioAsset.cpp
    Source/Assets/AlphaUVFootprint.cpp
    Source/Assets/RuntimeHumanoidProfile.cpp
    Source/Assets/ModelAsset.cpp
    Source/Assets/ModelExtras.cpp
    Source/Assets/ModelAssetLoadFbx.cpp
    Source/Assets/GltfAccessorUnpack.cpp
    Source/Assets/GltfAllocationBudget.cpp
    Source/Assets/ModelAssetLoadGltf.cpp
    Source/Assets/ModelAssetLoadBlend.cpp
    Source/Assets/SparseMorphTarget.cpp
    Source/Assets/MeshLODGenerator.cpp
    Source/Assets/MeshLODGeometry.cpp
    Source/Assets/MeshLODCache.cpp
    Source/Assets/ImportedMaterialCache.cpp
    Source/Assets/FbxLoaderOptions.cpp
    Source/Assets/LodAssetSettings.cpp
    Source/Assets/ProjectLodReimport.cpp
    Source/Assets/ModelAssetSettings.cpp
    Source/Assets/AuthoredLodImport.cpp
    Source/Assets/HlodClustering.cpp
    Source/Assets/HlodBenefit.cpp
    Source/Assets/HlodMeshMerge.cpp
    Source/Assets/HlodCache.cpp
    Source/Assets/HlodBaker.cpp
    Source/Assets/HlodMemberGather.cpp
    Source/Assets/HlodSelect.cpp
    Source/Assets/HlodReconcile.cpp
    Source/Assets/HlodBakeDriver.cpp

    # Package system (P0: manifest, resolver, mounts)
    Source/Assets/Packages/PackageVersion.cpp
    Source/Assets/Packages/PackageManifest.cpp
    Source/Assets/Packages/ProjectPackagesManifest.cpp
    Source/Assets/Packages/PackageResolver.cpp
    Source/Assets/Packages/PackageGitSource.cpp
    Source/Assets/Packages/PackageNativeCache.cpp
    Source/Assets/Packages/PackageGitFetcher.cpp
    Source/Assets/Packages/PackagesLockFile.cpp
    Source/Assets/Packages/PackageMounts.cpp
    Source/Assets/Packages/PackagePublish.cpp
    Source/Assets/Packages/PackageCodeModules.cpp
    Source/Assets/Packages/PackageCodeDiscovery.cpp
    Source/Assets/Packages/PackageManagementActions.cpp
    Source/Assets/Packages/PackagesIndex.cpp
    Source/Core/CommandLine.cpp

    # Scene/Blueprint IO (INI schema)
    Source/Scene/SceneValue.cpp
    Source/Scene/SceneSchemaRegistry.cpp
    Source/Scene/FieldSerializerRegistry.cpp
    Source/Scene/ReflectionSceneSchema.cpp
    Source/Scene/BuiltInSceneSchemas.cpp
    Source/Components/Rendering/SkySunDrive.cpp
    Source/Components/Rendering/SkySunIlluminance.cpp
    Source/Components/Rendering/SkySunPath.cpp
    Source/Scene/SceneIO.cpp
    Source/Scene/SceneEngineEmbedMaterializer.cpp

    # Derives the hierarchy half of entity enable state.
    Source/ECS/DisabledInHierarchySystem.cpp

    # Job System files moved to JobSystem library
    Source/Jobs/FileChangeTracker.cpp
    Source/Jobs/DependencyGraphAnalyzer.cpp
    Source/Jobs/IncrementalCompilationTask.cpp

    # Rendering hot-reload bridge (Engine-side wiring)
    Source/RenderingHotReloadBridge.cpp

    # Rendering ECS integration sources (engine-owned, ECS-dependent)
	Source/Engine/Rendering/Systems/RenderExtractionSystem.cpp
	Source/Engine/Rendering/Systems/HLODSelectSystem.cpp
    Source/Engine/Rendering/Systems/TransformHierarchySystem.cpp
	Source/Engine/Rendering/Systems/CameraSystem.cpp
	Source/Engine/Rendering/PlanetCameraFraming.cpp
	Source/Engine/Rendering/Systems/ReflectionProbeSystem.cpp
	Source/Engine/Rendering/Systems/GamepadCameraControllerSystem.cpp
	Source/Engine/Rendering/Systems/RenderGraphBuildSystem.cpp
	Source/Engine/Rendering/TimelinePlayback.cpp
	Source/Engine/Rendering/AnimatorScenePlayback.cpp
	Source/Engine/Rendering/AnimatorSourceResolution.cpp
	Source/Engine/Rendering/Systems/TimelinePlaybackSystem.cpp
	Source/Engine/Rendering/Systems/CharacterGraphParamSystem.cpp
	Source/Engine/Rendering/Systems/AnimationEventCollectorSystem.cpp
	Source/Engine/Rendering/Systems/AnimationGraphSystem.cpp
	Source/Engine/Rendering/Systems/AnimationGraphCompileFromModel.cpp
	Source/Engine/Rendering/Systems/AnimationSystem.cpp
	Source/Engine/Rendering/Systems/ValueCurveSystem.cpp
	Source/Engine/Rendering/Systems/HumanoidRetargetSystem.cpp
	Source/Engine/Rendering/Systems/RigidAnimationSystem.cpp
	Source/Engine/Rendering/Systems/SkinningUploadSystem.cpp
	Source/Engine/Rendering/Systems/MorphTargetSystem.cpp
	Source/Engine/Rendering/AnimationSampling.cpp
	Source/Engine/Rendering/AnimationEventDispatch.cpp
	Source/Engine/Rendering/GPUAnimationDataStore.cpp
	Source/Engine/Rendering/AnimationComputePass.cpp
	Source/Engine/Rendering/RetargetGPUDataStore.cpp
	Source/Engine/Rendering/RetargetFullPass.cpp
	Source/Engine/Rendering/RetargetSubmission.cpp
	Source/Engine/Rendering/SkeletonStore.cpp
	Source/Engine/Rendering/ClipStore.cpp
	Source/Assets/AnimationClip.cpp
	Source/Assets/AnimationClipSchema.cpp
	Source/Engine/Rendering/RenderingLoop.cpp
	Source/Engine/Rendering/DeviceLostEcsRecovery.cpp
	Source/Engine/Rendering/MeshReloadBoundsRefresh.cpp
	Source/Engine/Rendering/RenderDeviceContext.cpp
	Source/Engine/Rendering/MaterialCompiler.cpp
	Source/Engine/Rendering/PackageShaderDirs.cpp
	Source/Engine/Rendering/OceanDepthCacheBaker.cpp
	Source/Engine/Rendering/WindVolumeResolver.cpp
	Source/Engine/Rendering/ShaderGraphMaterial.cpp
	Source/Engine/Rendering/MaterialShaderPackageBuilder.cpp
	Source/Engine/Rendering/ProjectMaterialPrewarmService.cpp

	Source/Engine/Rendering/Systems/RegisterRenderingSystems.cpp
    Source/Engine/Rendering/Systems/SkyEnvironmentSystem.cpp
    Source/Engine/Rendering/Systems/AmbientLightSystem.cpp
    Source/Engine/Rendering/Systems/DDGIVolumeSystem.cpp

    # Audio ECS integration sources (engine-owned, ECS-dependent)
    Source/Engine/Audio/Systems/AudioListenerSystem.cpp
    Source/Engine/Audio/Systems/AudioEmitterSystem.cpp
    Source/Engine/Audio/Systems/RegisterAudioSystems.cpp

    # Engine-level UI integration helpers
    Source/Engine/UI/FontResolver.cpp

    # Video ECS integration (platform-neutral system; player backend is platform-conditional below)
    Source/Engine/Video/AsyncVideoRecorder.cpp
    Source/Engine/Video/VideoFrameUploader.cpp
    Source/Engine/Video/VideoTextureSystem.cpp
    Source/Engine/Video/VideoWriterCommon.cpp

    # Engine-owned managed and native scripting ECS bridges
    Source/Scripting/ManagedSystemBridge.cpp
    Source/Scripting/NativePostSimulationSystem.cpp

    # Element types defined outside C++: the native proxy and its orphan policy. The
    # C ABI over it lives in GameEngine.Native; the policy lives here so it is testable
    # without the scripting DLL.
    Source/Scripting/ManagedElementTypes.cpp

    # Who is allowed to own those types: per-load-context ids minted here and validated at
    # every C-ABI entry point. Same reason it lives beside the policy rather than in the
    # ABI DLL — the trust boundary is asserted by tests that never load a CLR.
    Source/Scripting/ManagedTypeOwners.cpp

	# ECS System - Now using ECS library
	# (ECS source files moved to ECS library)
)

set(ENGINE_HEADERS
    Include/Core/Application.h
    Include/Core/DeferredActionQueue.h
    Include/Core/WindowInputRouter.h
    Include/Core/WindowUiBootstrap.h
    Include/Core/StandardPaths.h
    Include/Core/Engine.h

    Include/Assets/AssetManager.h
    Include/Assets/AssetRegistry.h
    Include/Assets/AssetSourceAlias.h
    Include/Assets/AssetDependencyExtractor.h
    Include/Assets/ParserRegistry.h
    Include/Assets/FileWatchingService.h
    Include/Assets/CoreAssetRegistrations.h
    Include/Assets/SceneAsset.h

    Include/Assets/TextureAsset.h
    Include/Assets/AudioAsset.h

    # Video player public headers
    Include/Video/VideoPlayer.h
    Include/Video/VideoTypes.h
    Include/Video/AsyncVideoRecorder.h
    Include/Video/VideoWriter.h
    Include/Components/Video/VideoTextureComponent.h
    Include/Engine/Video/VideoFrameUploader.h
    Include/Engine/Video/VideoTextureSystem.h

    Include/Assets/ModelAsset.h
    Include/Assets/ModelExtras.h
    Include/Assets/ImportedMaterialCache.h
    Include/Assets/MeshLODGenerator.h
    Include/Assets/MeshLODGeometry.h
    Include/Assets/BinaryAsset.h
    Include/Assets/AssetTasks.h
    Include/Assets/AsyncRegistryTasks.h
    Include/Core/CommandLine.h

    Include/Memory/MemoryManager.h
    Include/Platform/Platform.h
    # Core Job System headers moved to JobSystem library
    Include/Jobs/HotReloadTasks.h
    Include/Jobs/FileChangeTracker.h
    Include/Jobs/DependencyGraphAnalyzer.h

    # Rendering authoring components
    Include/Components/Rendering/MeshRenderer.h
    Include/Components/Rendering/LocalBounds.h
    Include/Components/Rendering/Camera.h
    Include/Components/Rendering/Light.h
    Include/Components/Rendering/ReflectionProbe.h
    Include/Components/Rendering/LODGroup.h
    Include/Components/Rendering/RenderLayer.h
    Include/Components/Rendering/PostProcessVolume.h
    Include/Components/Rendering/WindVolume.h
    Include/Components/Rendering/PostProcessEffects/BloomEffect.h
    Include/Components/Rendering/PostProcessEffects/ContrastAdaptiveSharpenEffect.h
    Include/Components/Rendering/PostProcessEffects/CrtEffect.h
    Include/Components/Rendering/PostProcessEffects/VhsEffect.h
    Include/Components/Rendering/PostProcessEffects/ColorFilterEffect.h
    Include/Components/Rendering/PostProcessEffects/ColorGradeEffect.h
    Include/Components/Rendering/PostProcessEffects/CubeLutEffect.h
    Include/Components/Rendering/PostProcessEffects/DebandEffect.h
    Include/Components/Rendering/PostProcessEffects/VignetteEffect.h
    Include/Components/Rendering/PostProcessEffects/ExposureAdjustmentEffect.h
    Include/Components/Rendering/PostProcessEffects/AtmosphericCloudLayer.h
    Include/Components/Rendering/PostProcessEffects/VolumetricClouds.h
    Include/Components/Rendering/PostProcessEffects/VolumetricFogEffect.h
    Include/Engine/Rendering/VolumetricFogTypes.h
    Include/Engine/Rendering/WindVolumeResolver.h
    Include/Components/Rendering/SkinnedMeshRenderer.h
    Include/Components/Rendering/MorphTargetWeights.h
    Include/Components/Rendering/WorldSectorCoord.h
    Include/Components/Animation/SkeletonRef.h
    Include/Components/Animation/AnimatorRef.h
    Include/Components/Animation/Animator.h
    Include/Components/Animation/ValueCurve.h
    Include/Components/Animation/AnimatedNodeRef.h



    Include/ECS/SystemScheduling.h
    Include/ECS/DisabledInHierarchySystem.h

    # Animation authoring components/assets
    Include/Assets/AnimationClip.h
    Include/Assets/Parsers/AnimationAssetParser.h

    Include/Jobs/IHotReloadTransport.h
    Include/Jobs/PipeTransport.h

    Include/Jobs/IncrementalCompilationTask.h

    # Renderer-owned material compiler
    Include/Engine/Rendering/MaterialCompiler.h
    Include/Engine/Rendering/PackageShaderDirs.h
    Include/Engine/Rendering/OceanDepthCacheBaker.h

    # Scene/Blueprint IO (INI schema)
    Include/Scene/SceneValue.h
    Include/Scene/SceneSchemaRegistry.h
    Include/Scene/SceneIO.h

    # Scene authoring/persistence components
    Include/Components/SceneEntityTag.h
    Include/Components/SceneBlueprintInstance.h
    Include/Components/SceneSubsceneInstance.h

	# Rendering hot-reload bridge header
	Include/Engine/RenderingHotReloadBridge.h
	Include/Engine/Rendering/RenderDeviceContext.h
    Include/Engine/Rendering/AnimationSampling.h
    Include/Engine/Rendering/ProjectMaterialPrewarmService.h

    # Engine-level UI integration helpers
    Include/Engine/UI/FontResolver.h

	# ECS System Headers - Now using ECS library
	# (ECS header files moved to ECS library)
	)

# Engine-level RenderServices world rendering services library.
# OBJECT so its symbols land in Engine.dll's export table alongside the rest of
# the engine modules (see GE_ENGINE_MODULES below).
add_library(RenderServices OBJECT
	Source/Engine/Rendering/EmbeddedImageDecoder.cpp
	Source/Engine/Rendering/RenderWorldHooks.cpp
	Source/Engine/Rendering/RenderServices.cpp
	Source/Engine/Rendering/AntiAliasingEnvOverrides.cpp
	Source/Engine/Rendering/CameraAntiAliasing.cpp
	Source/Engine/Rendering/AntiAliasingProjectSettings.cpp
	Source/Engine/Rendering/LodProjectSettings.cpp
	Source/Engine/Rendering/HlodRuntime.cpp
	Source/Engine/Rendering/RenderServicesDetail.cpp
	Source/Engine/Rendering/PointShadowFaceCull.cpp
	Source/Engine/Rendering/PointShadowAtlasPlanner.cpp
	Source/Engine/Rendering/PipelineVariantCache.cpp
	Source/Engine/Rendering/ServedGenerationTable.cpp
	Source/Engine/Rendering/TextureService.cpp
	Source/Engine/Rendering/RenderServicesViewState.cpp
	Source/Engine/Rendering/ViewRegistry.cpp
	Source/Engine/Rendering/ViewTemporalHistory.cpp
	Source/Engine/Rendering/DynamicResolutionController.cpp
	Source/Engine/Rendering/RenderServicesMaterials.cpp
	Source/Engine/Rendering/MaterialSystem.cpp
	Source/Engine/Rendering/RenderServicesFrameGraph.cpp
	Source/Engine/Rendering/FrameOrchestrator.cpp
	Source/Engine/Rendering/RenderServicesGpuDriven.cpp
	Source/Engine/Rendering/LodCrossfadeLiveness.cpp
	Source/Engine/Rendering/RenderServicesDrawStreams.cpp
	Source/Engine/Rendering/RenderServicesDepthShadowPasses.cpp
	Source/Engine/Rendering/SceneAccelerationStructureService.cpp
	Source/Engine/Rendering/RTShadowMaskService.cpp
	Source/Engine/Rendering/ScreenSpaceShadows/ScreenSpaceShadowDispatch.cpp
	Source/Engine/Rendering/ScreenSpaceShadows/ScreenSpaceShadowPasses.cpp
	Source/Engine/Rendering/ScreenSpaceShadows/ScreenSpaceShadowWorldPass.cpp
	Source/Engine/Rendering/DDGIProbeFeature.cpp
	Source/Engine/Rendering/DDGIGlossyResolve.cpp
	Source/Engine/Rendering/DDGIMaterialMapAtlas.cpp
	Source/Engine/Rendering/DDGISceneService.cpp
	Source/Engine/Rendering/DDGIEmitterAreas.cpp
	Source/Engine/Rendering/DDGIUberMaterialBake.cpp
	Source/Engine/Rendering/DDGIFieldRanges.cpp
	Source/Engine/Rendering/DDGICameraIdleGate.cpp
	Source/Engine/Rendering/DDGIEligibleView.cpp
	Source/Engine/Rendering/DDGISolveBudget.cpp
	Source/Engine/Rendering/DDGISkinnedGeometry.cpp
	Source/Engine/Rendering/DepthDrawRecorder.cpp
	Source/Engine/Rendering/RenderServicesWorldPass.cpp
	Source/Engine/Rendering/RenderServicesSortedTransparent.cpp
	Source/Engine/Rendering/WorldDrawBuilder.cpp
	Source/Engine/Rendering/CpuDrawStreamBuilder.cpp
	Source/Engine/Rendering/MaterialBinder.cpp
	Source/Engine/Rendering/MaterialBindingCache.cpp
	Source/Engine/Rendering/MeshGPURegistry.cpp
	Source/Engine/Rendering/MeshGPUResidency.cpp
	Source/Engine/Rendering/MeshPoolGroupPlan.cpp
	Source/Engine/Rendering/BucketPoolAllocator.cpp
	Source/Engine/Rendering/Material.cpp
	Source/Engine/Rendering/MaterialRegistry.cpp
	Source/Engine/Rendering/SceneColorGrab.cpp
	Source/Engine/Rendering/ModelMaterialBridge.cpp
	Source/Engine/Rendering/ShaderCompilationCache.cpp
	Source/Engine/Rendering/CompileConcurrencyGate.cpp
	Source/Engine/Rendering/ViewFinalize.cpp
	Source/Engine/Rendering/ViewReadbackUtils.cpp
	Source/Engine/Rendering/ExposureReadbackFeature.cpp
	Source/Engine/Rendering/SkyRenderFeature.cpp
	Source/Engine/Rendering/LensFlareRenderFeature.cpp
	Source/Engine/Rendering/SunGlareRenderFeature.cpp
	Source/Engine/Rendering/SunScreenProjection.cpp
	Source/Engine/Rendering/CascadeMotionScheduler.cpp
	Source/Engine/Rendering/CascadeShadowCache.cpp
	Source/Engine/Rendering/CascadeCacheLogFormat.cpp
	Source/Engine/Rendering/ShadowMapRenderFeature.cpp
	Source/Engine/Rendering/ShadowMapRenderFeaturePasses.cpp
	Source/Engine/Rendering/ShadowMapRenderFeatureViewFit.cpp
	Source/Engine/Rendering/ShadowReceiverMeasurement.cpp
	Source/Engine/Rendering/ShadowMinMaxPyramid.cpp
	Source/Engine/Rendering/ImageBasedLightingFeature.cpp
	Source/Engine/Rendering/SkyEnvironmentSource.cpp
	Source/Engine/Rendering/SceneReflectionProbeEnvironmentSource.cpp
	Source/Engine/Rendering/SceneProbeRecaptureSchedule.cpp
	Source/Engine/Rendering/RetargetRenderFeature.cpp
	Source/Engine/Rendering/Pipeline/ReflectionsProvider.cpp
	Source/Engine/Rendering/DeformationMotionParams.cpp
	Source/Engine/Rendering/Pipeline/ViewMotionVectors.cpp
	Source/Engine/Rendering/Pipeline/DeformationMotionProducer.cpp
	Source/Engine/Rendering/Pipeline/RenderPipeline.cpp
	Source/Engine/Rendering/Pipeline/DepthUpsamplePass.cpp
	Source/Engine/Rendering/Pipeline/PipelineEnumTables.cpp
	Source/Engine/Rendering/Pipeline/ExpressionEvaluator.cpp
	Source/Engine/Rendering/Pipeline/Nodes/WorldRenderNode.cpp
	Source/Engine/Rendering/Pipeline/Nodes/SortedTransparentNode.cpp
	Source/Engine/Rendering/Pipeline/Nodes/TransmissivePassNode.cpp
	Source/Engine/Rendering/Pipeline/Nodes/DepthPrepassNode.cpp
	Source/Engine/Rendering/Pipeline/Nodes/DepthResolveNode.cpp
	Source/Engine/Rendering/Pipeline/Nodes/AONode.cpp
	Source/Engine/Rendering/Pipeline/Nodes/ScreenSpaceReflectionsNode.cpp
	Source/Engine/Rendering/Pipeline/Nodes/FidelityFXDofNode.cpp
	Source/Engine/Rendering/Pipeline/Nodes/AutoExposureNode.cpp
	Source/Engine/Rendering/Pipeline/Nodes/TemporalAANode.cpp
	Source/Engine/Rendering/Pipeline/Nodes/SmaaNode.cpp
	Source/Engine/Rendering/Pipeline/Nodes/TemporalFxaaNode.cpp
	Source/Engine/Rendering/Pipeline/Nodes/RenderScaleUpscaleNode.cpp
	Source/Engine/Rendering/Pipeline/Nodes/LightUploadNode.cpp
	Source/Engine/Rendering/Pipeline/Nodes/ViewParamsUploadNode.cpp
	Source/Engine/Rendering/Pipeline/Nodes/HeightFogParamsUploadNode.cpp
	Source/Engine/Rendering/CpuTextureSource.cpp
	Source/Engine/Rendering/Pipeline/Nodes/CpuTextureInputNode.cpp
	Source/Engine/Rendering/Pipeline/Nodes/VolumetricCloudsParamsUploadNode.cpp
	Source/Engine/Rendering/Pipeline/Nodes/ColorGradeParamsUploadNode.cpp
	Source/Engine/Rendering/FogSunLightingResolver.cpp
	Source/Engine/Rendering/HeightFogParamsUBO.cpp
	Source/Engine/Rendering/VolumetricCloudsParamsUBO.cpp
	Source/Engine/Rendering/ColorGradeParamsUBO.cpp
	Source/Engine/Rendering/Pipeline/Nodes/FullscreenShaderNode.cpp
	Source/Engine/Rendering/Pipeline/Nodes/ComputeShaderNode.cpp
	Source/Engine/Rendering/Pipeline/Nodes/SkyRenderNode.cpp
	Source/Engine/Rendering/Pipeline/Nodes/LensFlareRenderNode.cpp
	Source/Engine/Rendering/Pipeline/Nodes/SunGlareRenderNode.cpp
	Source/Engine/Rendering/Pipeline/Nodes/EngineNodeTypes.cpp
	Source/Engine/Rendering/Pipeline/Nodes/ShadowMapNode.cpp
	Source/Engine/Rendering/Pipeline/Nodes/ShadowReceiverReduce.cpp
	Source/Engine/Rendering/Pipeline/Nodes/IBLGenNode.cpp
	Source/Engine/Rendering/Pipeline/Nodes/DDGINode.cpp
	Source/Engine/Rendering/Pipeline/Nodes/HZBBuildNode.cpp
	Source/Engine/Rendering/Pipeline/Nodes/OcclusionCullP2Node.cpp
	Source/Engine/Rendering/Pipeline/Nodes/ScatterBNode.cpp
	Source/Engine/Rendering/PostProcessSettings.cpp
	Source/Engine/Rendering/PostProcessEffectRegistry.cpp
	Source/Engine/Rendering/PostProcessEffectDescriptors.cpp
)

set_target_properties(RenderServices PROPERTIES
	CXX_STANDARD 20
	CXX_STANDARD_REQUIRED ON
)

target_compile_features(RenderServices PUBLIC cxx_std_20)

# Unity builds with many translation units can exceed the COFF section limit.
if(MSVC)
    target_compile_options(RenderServices PRIVATE /bigobj)
endif()

target_include_directories(RenderServices
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/Include
		${CMAKE_SOURCE_DIR}/Engine/Modules/VolumetricFog/Include
		# SkeletonStore (runtime-create epoch consumed by the skinning
		# visibility gate's spawn-frame guard).
		${CMAKE_CURRENT_SOURCE_DIR}/ECSModules/Rendering/Include
	PRIVATE
		# PostProcessEffectDescriptors.cpp registers HeightFogEffect's
		# descriptor (GetComponentTypeId only). Headers only (no link):
		# HeightFog links into Engine.dll alongside these objects.
		$<TARGET_PROPERTY:HeightFog,INTERFACE_INCLUDE_DIRECTORIES>
		# DDGISceneService builds the software GI lane's scene
		# from SceneBvh's threaded-BVH types. Same header-only pattern: SceneBvh
		# links into Engine.dll alongside these objects (GE_ENGINE_MODULES).
		$<TARGET_PROPERTY:SceneBvh,INTERFACE_INCLUDE_DIRECTORIES>
)

target_link_libraries(RenderServices
	PUBLIC
		# PipelineVariantCache.h exposes Event<const GUID&> in its public API.
		Events
		Logger
		AssetCore
		AssetRuntime
		GameEngineRendering
	PRIVATE
		# HlodRuntime.cpp (and RenderServices.cpp, which owns the HlodRuntime
		# unique_ptr) reach ECS World/Entity/EntityHandle. PRIVATE: compile-time
		# usage requirements only; ECS objects link once into Engine as elsewhere.
		ECS
)
# GetPipelineOutputSpaceRG returns UI::UITextureSpace (#767): compile-time
# include only, the same shim Engine itself uses for UI asset integration;
# UI objects link once into Engine as elsewhere.
target_include_directories(RenderServices PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Modules/UI/Include)

# EnvironmentSwitchEnabled (Platform/Environment.h) reads the producer-arm
# switches: header-only, no link — Platform objects link once into Engine as
# elsewhere.
target_include_directories(RenderServices PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Modules/Platform/Include)

# Vendored Bend screen-space shadow planner (header-only, Apache 2.0).
# See ThirdParty/BendScreenSpaceShadows/UPSTREAM.md for the upstream pin.
target_include_directories(RenderServices PRIVATE ${CMAKE_SOURCE_DIR}/ThirdParty)
# The upstream header assigns an int literal to a float member, which MSVC
# reports as C4244 and warnings-as-errors turns into a build failure. The
# header is retained unmodified, so the including TU tolerates its warnings.
set_source_files_properties(Source/Engine/Rendering/ScreenSpaceShadows/ScreenSpaceShadowDispatch.cpp
    PROPERTIES COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/WX->")

# Engine.dll references five C-ABI hot-reload entry points (GE_PreloadAssemblyContext,
# GE_SwapPreloadedContext, GE_CleanupOldContext, GE_ClearCompilerCache, GE_GetCompilerStats)
# from IncrementalCompilationTask / AssemblyPrepTask / MainThreadSwapTask. Their real
# implementations live in GameEngine.Native.dll, which depends on Engine.dll — a SHARED↔SHARED
# cycle that the linker can't resolve at Engine.dll's link step.
#
# Phase 1 (this commit): link the no-op stubs into Engine.dll so it self-resolves. This means
# C# script hot-reload silently does nothing at runtime (the stubs return GE_Result_Ok).
#
# Phase 1 follow-up: replace the direct GE_* calls with a runtime function-pointer table that
# GameEngine.Native.dll registers with Engine.dll on init. Restores hot-reload in production
# without re-creating the link cycle.
list(APPEND ENGINE_SOURCES Source/HotReloadNativeStubs.cpp)

# Engine is now a SHARED library (Phase 1 of the native-C++ scripting plan).
# Each consumer (Editor, Player, GameEngine.Native, future user DLLs) links
# against the import lib and resolves symbols against the single canonical
# Engine.dll/dylib/so at runtime — no more per-DLL static re-linking with
# singleton-injection scaffolding.
#
# Export policy during the staged annotation rollout:
#   - Windows: WINDOWS_EXPORT_ALL_SYMBOLS=ON so unannotated public surface
#     still exports (annotations are accepted but not yet gating).
#   - POSIX:   default (permissive) visibility kept until the final Phase 1
#     commit; CXX_VISIBILITY_PRESET=hidden is NOT set yet.
# The final Phase 1 commit flips both to "curated exports only" via GE_API.
# One static binary on wasm: Emscripten dynamic linking (MAIN_MODULE/
# SIDE_MODULE) is rejected for size/DCE cost, and a single static Engine copy
# satisfies the single-shared-copy invariants by construction.
if(GE_WASM)
    add_library(Engine STATIC ${ENGINE_SOURCES} ${ENGINE_HEADERS})
else()
    add_library(Engine SHARED ${ENGINE_SOURCES} ${ENGINE_HEADERS})
    target_compile_definitions(Engine PRIVATE GAMEENGINE_BUILD_DLL)
    ge_add_production_allocation_hook(Engine)
endif()
# DO NOT enable WINDOWS_EXPORT_ALL_SYMBOLS — see the GenerateEngineExportsDef
# block further down for why we generate Engine.dll's .def file ourselves.

# Model-to-entity factory, render setup utilities, and primitive generator (depends on ECS + RenderServices).
target_sources(Engine PRIVATE
    Source/Engine/Rendering/Systems/LensFlareExtractionSystem.cpp
    Source/Engine/Rendering/ModelEntityFactory.cpp
    Source/Engine/Rendering/MeshNameRegistry.cpp
    Source/Engine/Rendering/ModelRenderSetup.cpp
    Source/Engine/Rendering/SceneBuildPump.cpp
    Source/Engine/Rendering/SceneResolveService.cpp
    Source/Engine/Rendering/PrimitiveGenerator.cpp
    Source/Engine/Rendering/MeshReprovisionSource.cpp
)

# ---------------------------------------------------------------------------
# Built-in component reflection codegen.
#
# The ComponentScanner C# tool (Managed/ComponentScanner) scans the C++ headers
# under Include/Components and emits one GE_REGISTER_COMPONENT(...) /
# GE_REGISTER_COMPONENT_NO_ADD(...) line per reflected component struct into
# ComponentReflection.gen.cpp, which compiles into Engine in place of the old
# hand-written Source/Components/ComponentReflection.cpp.
#
# This runs at build time: any change to a component header re-runs the scan
# (the headers are in the custom command's DEPENDS via a CONFIGURE_DEPENDS glob),
# and any change to the scanner's own sources rebuilds it first.
#
# This subdirectory is processed before the root CMakeLists resolves
# DOTNET_EXECUTABLE / defines ge_add_managed_assembly, so we locate dotnet here
# (find_program caches the result, so the root block's later lookup is a no-op)
# and drive the scanner build + generation with plain custom commands.
# ---------------------------------------------------------------------------
if(NOT DOTNET_EXECUTABLE OR DOTNET_EXECUTABLE STREQUAL "DOTNET_EXECUTABLE-NOTFOUND")
    set(_ge_cs_dotnet_hints "")
    if(DEFINED ENV{DOTNET_ROOT} AND NOT "$ENV{DOTNET_ROOT}" STREQUAL "")
        list(APPEND _ge_cs_dotnet_hints "$ENV{DOTNET_ROOT}")
    endif()
    list(APPEND _ge_cs_dotnet_hints
        "/usr/local/share/dotnet" "/usr/share/dotnet" "/opt/homebrew/share/dotnet"
        "/usr/local/bin" "/opt/homebrew/bin")
    if(WIN32)
        list(APPEND _ge_cs_dotnet_hints "C:/Program Files/dotnet" "C:/Program Files (x86)/dotnet")
    endif()
    find_program(DOTNET_EXECUTABLE
        NAMES dotnet dotnet.exe
        HINTS ${_ge_cs_dotnet_hints}
        PATH_SUFFIXES "" "bin")
endif()

if(NOT DOTNET_EXECUTABLE OR DOTNET_EXECUTABLE STREQUAL "DOTNET_EXECUTABLE-NOTFOUND")
    message(FATAL_ERROR
        "ComponentScanner requires 'dotnet' to generate ComponentReflection.gen.cpp, "
        "but it was not found. Install the .NET 10 SDK, or set DOTNET_ROOT / DOTNET_EXECUTABLE.")
endif()

set(_GE_COMPONENT_SCANNER_CSPROJ "${CMAKE_SOURCE_DIR}/Managed/ComponentScanner/ComponentScanner.csproj")
set(_GE_COMPONENT_SCANNER_DLL "${CMAKE_SOURCE_DIR}/Managed/ComponentScanner/bin/$<CONFIG>/net10.0/ComponentScanner.dll")
set(_GE_COMPONENT_GEN_DIR "${CMAKE_BINARY_DIR}/generated")
set(_GE_COMPONENT_GEN_CPP "${_GE_COMPONENT_GEN_DIR}/ComponentReflection.gen.cpp")
# The web library's component types, from the same scan: build products, never committed.
set(_GE_COMPONENT_GEN_DTS "${_GE_COMPONENT_GEN_DIR}/components.d.ts")
set(_GE_COMPONENT_GEN_JSON "${_GE_COMPONENT_GEN_DIR}/components.json")
set(_GE_COMPONENT_SCANNER_STAMP "${CMAKE_BINARY_DIR}/_managed_stamps/ComponentScanner.$<CONFIG>.stamp")
file(MAKE_DIRECTORY "${_GE_COMPONENT_GEN_DIR}")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/_managed_stamps")

# The scanner's own sources, re-globbed before each build (outside its obj/
# and bin/ outputs) so an added source rebuilds the scanner.
include("${CMAKE_SOURCE_DIR}/cmake/ManagedProjectSources.cmake")
ge_managed_project_sources(_ge_component_scanner_sources
    "${CMAKE_SOURCE_DIR}/Managed/ComponentScanner")

# Component headers to scan: the core Engine/Include/Components plus each engine
# module that ships its own Include/Components tree (e.g. HeightFog, Particles).
# Each such dir becomes a separate scanner --in root; include paths are computed
# per-root, and structs seen from more than one root are de-duplicated by name.
set(_ge_component_scan_roots "${CMAKE_CURRENT_SOURCE_DIR}/Include/Components")
file(GLOB _ge_module_component_roots LIST_DIRECTORIES true
    "${CMAKE_CURRENT_SOURCE_DIR}/Modules/*/Include/Components")
foreach(_root ${_ge_module_component_roots})
    if(IS_DIRECTORY "${_root}")
        list(APPEND _ge_component_scan_roots "${_root}")
    endif()
endforeach()

set(_ge_scan_in_args "")
foreach(_root ${_ge_component_scan_roots})
    list(APPEND _ge_scan_in_args --in "${_root}")
endforeach()

# Re-glob the scanned headers (core + module) at build time so adding either
# re-triggers the scan without re-running configure.
file(GLOB_RECURSE _ge_component_headers CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/Include/Components/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/Modules/*/Include/Components/*.h")

# Build the scanner (stamped — a no-op MSBuild visit when nothing changed).
add_custom_command(
    OUTPUT "${_GE_COMPONENT_SCANNER_STAMP}"
    # --unset=Platform: vcvars shells export Platform=x64, which would reroute the
    # output to bin/x64/<Config>/ away from _GE_COMPONENT_SCANNER_DLL below.
    COMMAND ${CMAKE_COMMAND} -E env --unset=TargetName --unset=TARGETNAME
            --unset=Platform --unset=PLATFORM
            "${DOTNET_EXECUTABLE}" build "${_GE_COMPONENT_SCANNER_CSPROJ}"
            --configuration $<CONFIG>
            -nologo -v:minimal /p:UseSharedCompilation=true
    COMMAND ${CMAKE_COMMAND} -E touch "${_GE_COMPONENT_SCANNER_STAMP}"
    DEPENDS ${_ge_component_scanner_sources}
    COMMENT "Building ComponentScanner (component-reflection codegen tool)"
    VERBATIM)

# Run the scanner over the component headers to (re)generate the reflection TU and,
# from the same parse, the web library's components.d.ts and components.json.
add_custom_command(
    OUTPUT "${_GE_COMPONENT_GEN_CPP}" "${_GE_COMPONENT_GEN_DTS}" "${_GE_COMPONENT_GEN_JSON}"
    COMMAND "${DOTNET_EXECUTABLE}" "${_GE_COMPONENT_SCANNER_DLL}"
            ${_ge_scan_in_args}
            --out "${_GE_COMPONENT_GEN_CPP}"
            --emit-dts "${_GE_COMPONENT_GEN_DTS}"
            --emit-json "${_GE_COMPONENT_GEN_JSON}"
    DEPENDS ${_ge_component_headers} "${_GE_COMPONENT_SCANNER_STAMP}"
    COMMENT "Generating ComponentReflection.gen.cpp from component headers"
    VERBATIM)

# The generated TU lives in the binary dir; mark it generated.
set_source_files_properties("${_GE_COMPONENT_GEN_CPP}" PROPERTIES
    GENERATED TRUE)

# The reflection TUs (generated + hand-written) contain nothing but file-scope
# registrar statics — no symbol any other TU references. As members of a STATIC
# Engine archive (the wasm build) the linker silently drops them and every
# reflected component loses its field table at runtime; the SHARED desktop
# Engine includes every member, which is what kept the defect latent. An OBJECT
# library's objects reach every consumer's final link unconditionally, on both
# link shapes. Each file is its own TU (UNITY_BUILD OFF) so GE_REFLECT explicit
# specializations / per-line auto-register statics stay isolated.
#
# LensFlareSourceReflection / CubeLutEffectReflection / UIDocumentReflection:
# hand-registered (AssetRef<> fields the scanner can't parse; UIDocument also
# carries the hand-written UIRenderMode enum-name table).
add_library(EngineComponentReflection OBJECT
    "${_GE_COMPONENT_GEN_CPP}"
    Source/Components/LensFlareSourceReflection.cpp
    Source/Components/CubeLutEffectReflection.cpp
    Source/Components/UIDocumentReflection.cpp)
set_target_properties(EngineComponentReflection PROPERTIES UNITY_BUILD OFF)
target_include_directories(EngineComponentReflection PRIVATE
    $<TARGET_PROPERTY:Engine,INCLUDE_DIRECTORIES>)
target_compile_definitions(EngineComponentReflection PRIVATE
    $<TARGET_PROPERTY:Engine,COMPILE_DEFINITIONS>)
# The same usage requirements Engine's own TUs compile against (module
# component headers arrive through these interfaces).
target_link_libraries(EngineComponentReflection PRIVATE Input Scene)
# Linkage shape is load-bearing. STATIC Engine (wasm): the objects must reach
# the final executable's link line as EXPLICIT object files — immune to
# archive-member elision. Neither PRIVATE nor PUBLIC linking of the object
# library does that (verified against the generated ninja graph: the objects
# neither enter libEngine.a nor propagate through the interface); the
# $<TARGET_OBJECTS> genex in the PUBLIC link interface does. SHARED Engine
# (desktop): PRIVATE embeds the objects in the dylib once; the genex there
# would instead hand a copy to every consumer, double-running the registrars.
get_target_property(_ge_engine_lib_type Engine TYPE)
if(_ge_engine_lib_type STREQUAL "STATIC_LIBRARY")
    target_link_libraries(Engine PUBLIC $<TARGET_OBJECTS:EngineComponentReflection>)
    add_dependencies(Engine EngineComponentReflection)
else()
    target_link_libraries(Engine PRIVATE EngineComponentReflection)
endif()

target_sources(Engine PRIVATE Source/Components/CoreComponentInstantiations.cpp)
target_sources(Engine PRIVATE Source/Components/DDGIComponentInstantiations.cpp)
target_sources(Engine PRIVATE Source/Components/AnimationComponentInstantiations.cpp)
target_sources(Engine PRIVATE Source/Components/SplineWallDefaults.cpp)

# Game UI host: drives a game UIManager from UIDocument entities (editor + Player).
target_sources(Engine PRIVATE
    Source/Engine/GameUI/GameUIHost.cpp
    Source/Engine/GameUI/GameplayUI.cpp
    Source/Engine/GameUI/UIScaleProjectSettings.cpp)

# Runtime host: the window, render device, pipeline, scene and render-graph frame
# a runtime (the Player) runs a game in.
target_sources(Engine PRIVATE
    Source/Engine/Hosting/HudPointer.cpp
    Source/Engine/Hosting/RuntimeHost.cpp)

# Build system: GameConfig, Camera utility, BuildPipeline, AssetCollector (Player support)
target_sources(Engine PRIVATE
    Source/Engine/Build/GameConfig.cpp
    Source/Engine/Build/LinuxRuntimeTemplate.cpp
    Source/Engine/Build/AssetCollector.cpp
    Source/Engine/Build/AppIconGenerator.cpp
    Source/Engine/Build/BuildHost.cpp
    Source/Engine/Build/BuildPipeline.cpp
    Source/Engine/Build/TerrainPageExport.cpp
    Source/Engine/Build/TexturePackageCook.cpp
    Source/Engine/Build/StagedAssetCook.cpp
    Source/Engine/Build/SceneExportStrip.cpp
    Source/Engine/Build/BuildPlatforms.cpp
    Source/Engine/Build/WebExportRunner.cpp
    Source/Engine/Build/MacBundleAssembler.cpp
    Source/Engine/Build/NativeAotProject.cpp
    Source/Engine/Build/NativeScriptStaging.cpp
    Source/Engine/Build/OutputPromotion.cpp
    Source/Engine/Build/PackageBuildStaging.cpp
    Source/Engine/Build/PackagedGameLayout.cpp
    Source/Engine/Build/PlayerSourcePreparation.cpp
    Source/Engine/Build/RuntimeDependencyStaging.cpp
    Source/Engine/Build/CancellableShellProcess.cpp
    Source/Engine/Build/DirectoryCopy.cpp
    Source/Engine/Build/DotnetHost.cpp
    Source/Engine/Rendering/Camera.cpp
    Source/Engine/Rendering/CameraAspectRatio.cpp
)

# Video player + writer backends: AVFoundation on macOS, FFmpeg on Windows/Linux.
if(APPLE)
    target_sources(Engine PRIVATE
        Source/Engine/Video/VideoPlayer.mm
        Source/Engine/Video/VideoWriter.mm
    )
    # ObjC++ cannot share a C++-only precompiled header or be concatenated into
    # a generated C++ unity translation unit.
    set_source_files_properties(
        Source/Engine/Video/VideoPlayer.mm
        Source/Engine/Video/VideoWriter.mm
        PROPERTIES
            SKIP_PRECOMPILE_HEADERS ON
            SKIP_UNITY_BUILD_INCLUSION ON)
    target_link_libraries(Engine PRIVATE
        "-framework AVFoundation"
        "-framework CoreVideo"
        "-framework CoreMedia"
        "-framework Foundation"
        "-framework QuartzCore"
        "-framework VideoToolbox"
    )
else()
    find_package(FFMPEG COMPONENTS AVCODEC AVFORMAT AVUTIL SWSCALE QUIET)
    if(FFMPEG_FOUND)
        target_sources(Engine PRIVATE
            Source/Engine/Video/VideoPlayerFFmpeg.cpp
            Source/Engine/Video/VideoWriterFFmpeg.cpp
        )
        target_include_directories(Engine PRIVATE ${FFMPEG_INCLUDE_DIRS})
        target_link_libraries(Engine PRIVATE ${FFMPEG_LIBRARIES})
        target_compile_definitions(Engine PRIVATE GE_HAVE_FFMPEG=1)
    else()
        message(STATUS "FFmpeg not found — video playback and recording disabled (null backends)")
        target_sources(Engine PRIVATE
            Source/Engine/Video/VideoPlayerNull.cpp
            Source/Engine/Video/VideoWriterNull.cpp
        )
    endif()
endif()

# Propagate scripting enable flag to C++ as GE_ENABLE_SCRIPTING (1 or 0)
target_compile_definitions(Engine PUBLIC GE_ENABLE_SCRIPTING=$<BOOL:${ENABLE_SCRIPTING}>)

# Profiling / diagnostic monitoring switches. Each defaults ON for dev and
# release; pass -D<flag>=OFF for shipping/user builds to strip the matching
# subsystem to no-op stubs so scopes, metric pushes, and GPU timestamp queries
# compile away. The umbrella ENABLE_PROFILING flag, when explicitly set to OFF,
# disables all three at once (individual overrides still win if set).
option(ENABLE_PROFILING "Master switch for CPU/GPU profiling and monitors" ON)
option(ENABLE_CPU_PROFILING "Enable CpuProfiler and scope timing" ${ENABLE_PROFILING})
option(ENABLE_GPU_PROFILING "Enable RenderGraph per-pass GPU timings" ${ENABLE_PROFILING})
option(ENABLE_METRICS "Enable DebugMetrics named monitor collection" ${ENABLE_PROFILING})
target_compile_definitions(Engine PUBLIC GE_ENABLE_CPU_PROFILING=$<BOOL:${ENABLE_CPU_PROFILING}>)
target_compile_definitions(Engine PUBLIC GE_ENABLE_GPU_PROFILING=$<BOOL:${ENABLE_GPU_PROFILING}>)
target_compile_definitions(Engine PUBLIC GE_ENABLE_METRICS=$<BOOL:${ENABLE_METRICS}>)

# NVTX ranges for external profilers (Nsight Systems). A second, independent
# instrument from ENABLE_CPU_PROFILING above: every GE_CPU_PROFILE_SCOPE also
# opens an NVTX range, emitted whether or not the in-engine profiler is armed.
# Runtime gate is the GE_NVTX environment variable (truthy unless "0").
#
# Headers come from the nvtx3-upstream overlay port and are header-only — NVTX
# resolves its injection library at runtime, so there is nothing to link.
#
# GE_ENABLE_NVTX is PRIVATE, unlike the three PUBLIC flags above. Five OBJECT
# modules compile Core/CpuProfiler.h without being able to link Engine —
# MeshPicking, UI, Scene, TerrainECS, CBTTerrainECS — and each replays Engine's
# INTERFACE compile definitions to keep ScopedCpuProfile ODR-consistent, so
# anything added to that INTERFACE set becomes part of their ABI contract too.
# GE_ENABLE_NVTX has no business there: only NvtxRange.cpp reads it, and
# Core/NvtxRange.h is deliberately gate-free, so ScopedNvtxRange — and the
# ScopedCpuProfile that embeds it — has one layout in every TU regardless.
#
# The include dir is PRIVATE for the same "only NvtxRange.cpp needs it" reason,
# not as a barrier: find_path usually resolves to the shared vcpkg include root
# that every target already has. The header is kept out of reach by never
# appearing in a public header, not by include-path scoping.
find_path(NVTX3_INCLUDE_DIR NAMES nvtx3/nvToolsExt.h)
option(ENABLE_NVTX "NVTX ranges for external profilers (Nsight Systems)" ${ENABLE_PROFILING})
set(GE_NVTX_ACTIVE OFF)
if(ENABLE_NVTX)
    if(NOT NVTX3_INCLUDE_DIR)
        message(STATUS "ENABLE_NVTX is ON but nvtx3/nvToolsExt.h was not found "
                       "(is nvtx3-upstream installed?) — NVTX ranges compile out.")
    elseif(APPLE)
        message(STATUS "ENABLE_NVTX is ON but Nsight Systems has no macOS target "
                       "support — NVTX ranges compile out.")
    else()
        set(GE_NVTX_ACTIVE ON)
        target_include_directories(Engine PRIVATE ${NVTX3_INCLUDE_DIR})
    endif()
endif()
target_compile_definitions(Engine PRIVATE GE_ENABLE_NVTX=$<BOOL:${GE_NVTX_ACTIVE}>)
message(STATUS "NVTX ranges: ${GE_NVTX_ACTIVE} (runtime gate: GE_NVTX)")

# Generated headers: Core/EngineVersion.h, written from the root VERSION file by the root
# CMakeLists.txt, and Jobs/CompileServerVersion.h, written below.
target_include_directories(Engine PUBLIC "${CMAKE_BINARY_DIR}/generated")

# GE_ENGINE_BUILD_CONFIG: the configuration this Engine library is built in ("DebugFast",
# "Release", ...), from the generator's per-config $<CONFIG>. The build pipeline compares it with
# the configuration a packaged player links, to know whether the running engine is that player's
# runtime.
target_compile_definitions(Engine PRIVATE GE_ENGINE_BUILD_CONFIG="$<CONFIG>")

# CompileServer version (single source of truth)
set(COMPILE_SERVER_VERSION_TXT "${CMAKE_SOURCE_DIR}/Managed/CompileServerHost/CompileServerVersion.txt")
if(EXISTS "${COMPILE_SERVER_VERSION_TXT}")
    # file(READ) alone is not a configure dependency: a version bump without a
    # manual reconfigure ships a client expecting the OLD version against a
    # server reporting the new one — the handshake then recycle-loops the
    # server and hot-reload compiles silently die.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${COMPILE_SERVER_VERSION_TXT}")
    file(READ "${COMPILE_SERVER_VERSION_TXT}" COMPILE_SERVER_VERSION_RAW)
    string(STRIP "${COMPILE_SERVER_VERSION_RAW}" COMPILE_SERVER_VERSION)
    set(_GEN_DIR "${CMAKE_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${_GEN_DIR}/Jobs")
    configure_file("${CMAKE_SOURCE_DIR}/Engine/Include/Jobs/CompileServerVersion.h.in" "${_GEN_DIR}/Jobs/CompileServerVersion.h" @ONLY)
else()
    message(WARNING "CompileServer version file not found: ${COMPILE_SERVER_VERSION_TXT}")
endif()

# Enable C++20 features
# Examples toggle

target_compile_features(Engine PUBLIC cxx_std_20)

# Unity builds with many schemas can exceed the COFF section limit.
if(MSVC)
    target_compile_options(Engine PRIVATE /bigobj)
endif()

# Scripting ABI is linked statically from Engine; disable dllimport in headers for dependents
# Precompiled headers for Engine (standard headers only)
if (COMMAND target_precompile_headers)
    target_precompile_headers(Engine PRIVATE
        <vector> <string> <memory> <unordered_map> <unordered_set>
        <optional> <algorithm> <span> <cstdint> <cstring>
    )
endif()

# so tests and other static consumers see GE_API as empty instead of dllimport
# This avoids __imp_ unresolved externals when not using a DLL
 target_compile_definitions(Engine PRIVATE GE_SCRIPTING_STATIC)

 target_link_libraries(Engine
 	PUBLIC
 		# PipelineVariantCache.h (reached through RenderServices.h) exposes
 		# Event<const GUID&>, so the header-only Events include travels with
 		# Engine's INTERFACE dirs — modules that pull those instead of linking
 		# Engine (TerrainECS) get it too.
 		Events
 		Input
 		Scene
 )

# Optional SVG rasterization via ThorVG (vcpkg).
# The vcpkg thorvg port only provides pkg-config (no CMake config targets).
# ThorVG 1.x renamed the pkg-config module and install paths to thorvg-1.
# Try pkg-config first; fall back to manual header/library search on platforms
# where pkg-config is typically absent (Windows).
find_package(PkgConfig QUIET)
set(_GE_THORVG_FOUND FALSE)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(GE_THORVG1 QUIET IMPORTED_TARGET thorvg-1)
    if(GE_THORVG1_FOUND)
        target_link_libraries(Engine PRIVATE PkgConfig::GE_THORVG1)
        set(_GE_THORVG_FOUND TRUE)
    endif()

    if(NOT _GE_THORVG_FOUND)
        pkg_check_modules(GE_THORVG_LEGACY QUIET IMPORTED_TARGET thorvg)
    endif()
    if(GE_THORVG_LEGACY_FOUND)
        target_link_libraries(Engine PRIVATE PkgConfig::GE_THORVG_LEGACY)
        set(_GE_THORVG_FOUND TRUE)
    endif()
endif()
if(NOT _GE_THORVG_FOUND)
    find_path(_THORVG_INCLUDE_DIR NAMES "thorvg.h" PATH_SUFFIXES thorvg-1)
    find_library(_THORVG_LIBRARY NAMES thorvg-1 thorvg)
    if(_THORVG_INCLUDE_DIR AND _THORVG_LIBRARY)
        target_include_directories(Engine PRIVATE ${_THORVG_INCLUDE_DIR})
        target_link_libraries(Engine PRIVATE ${_THORVG_LIBRARY})
        set(_GE_THORVG_FOUND TRUE)
    endif()
endif()
if(_GE_THORVG_FOUND)
    target_compile_definitions(Engine PRIVATE GE_HAVE_THORVG=1)
endif()

# FBX support is provided by the vendored ufbx library at
# Engine/Source/ThirdParty/ufbx/ — wired in unconditionally below. Other
# importers (cgltf, tinyobj, stb) are still discovered via vcpkg config
# packages. glTF loading is cgltf (header-only, find_path detection below).
find_package(tinyobjloader CONFIG QUIET)
find_package(cgltf CONFIG QUIET)
find_package(stb CONFIG QUIET)
# meshoptimizer: automatic LOD generation (meshopt_simplify) for imported models.
find_package(meshoptimizer CONFIG QUIET)

# ECS SIMD support detection with ARM64 compatibility
if(MSVC)
    # Detect architecture for SIMD support based on *target* architecture.
    # For MSVC/Visual Studio, prefer CMAKE_GENERATOR_PLATFORM so an ARM64 host
    # building x64 binaries does not accidentally enable NEON.
    if(CMAKE_GENERATOR_PLATFORM MATCHES "ARM64|arm64")
        # ARM64 NEON support
        target_compile_definitions(Engine PRIVATE ECS_SIMD_ENABLED=1 ECS_SIMD_ARM64=1)
        message(STATUS "Enabling ARM64 NEON SIMD support for Engine (MSVC, generator ARM64)")
    elseif(NOT CMAKE_GENERATOR_PLATFORM AND CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        # Fallback for single-arch generators (e.g., Ninja with MSVC) on ARM64
        target_compile_definitions(Engine PRIVATE ECS_SIMD_ENABLED=1 ECS_SIMD_ARM64=1)
        message(STATUS "Enabling ARM64 NEON SIMD support for Engine (MSVC, system processor ARM64)")
    else()
        # x64 AVX2 support
        target_compile_options(Engine PRIVATE /arch:AVX2)
        target_compile_definitions(Engine PRIVATE ECS_SIMD_ENABLED=1 ECS_SIMD_X64=1)
        message(STATUS "Enabling x64 AVX2 SIMD support for Engine (MSVC, assuming x64 target)")
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # Check architecture for SIMD support
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        # ARM64 NEON support
        target_compile_definitions(Engine PRIVATE ECS_SIMD_ENABLED=1 ECS_SIMD_ARM64=1)
        message(STATUS "Enabling ARM64 NEON SIMD support for Engine (GCC/Clang)")
    else()
        # Check for AVX2 support on x64
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag("-mavx2" COMPILER_SUPPORTS_AVX2)
        if(COMPILER_SUPPORTS_AVX2)
            target_compile_options(Engine PRIVATE -mavx2)
            target_compile_definitions(Engine PRIVATE ECS_SIMD_ENABLED=1 ECS_SIMD_X64=1)
            message(STATUS "Enabling x64 AVX2 SIMD support for Engine (GCC/Clang)")
        endif()
    endif()
endif()

# EngineInterop (legacy P/Invoke shim) removed – replaced by typed C ABI in Engine


# Find and link dependencies
find_package(concurrentqueue CONFIG REQUIRED)

# Link libraries
find_package(nlohmann_json CONFIG REQUIRED)
target_link_libraries(Engine PRIVATE nlohmann_json::nlohmann_json)

# RenderServices also uses JSON for RenderPipeline compilation.
target_link_libraries(RenderServices PRIVATE nlohmann_json::nlohmann_json)

# stb_image for EmbeddedImageDecoder (embedded texture decoding).
if(TARGET stb::stb)
    target_link_libraries(RenderServices PRIVATE stb::stb)
elseif(STB_INCLUDE_DIRS)
    target_include_directories(RenderServices PRIVATE ${STB_INCLUDE_DIRS})
endif()

# GLM usage requirements propagate to Engine consumers via Scene -> Mathematics.
# Do not link glm::glm directly on Engine here — that duplicates libglm on
# consumer link lines (Scene already PUBLIC-links Mathematics, which INTERFACE-
# links glm).
find_package(glm CONFIG QUIET)
if (TARGET glm::glm)
	target_compile_definitions(Engine PRIVATE GE_HAVE_GLM)
endif()

# Engine SHARED aggregates every module's compiled code into a single Engine.dll image.
# Modules are OBJECT libraries (see Engine/Modules/*/CMakeLists.txt), so their .obj
# files are spliced directly into Engine.dll at link time. WINDOWS_EXPORT_ALL_SYMBOLS
# walks those .obj files alongside Engine's own, emitting an export entry for every
# non-local definition — so module-public symbols land in Engine.dll's import lib and
# consumers (Editor.exe, Player.exe, GameEngine.Native.dll, user DLLs) can resolve
# them across the DLL boundary without re-linking module archives.
#
# Modules MUST be PRIVATE here. If they were PUBLIC, each consumer would also splice
# the OBJECT libs' .obj files into its own binary via INTERFACE_LINK_LIBRARIES,
# producing a second copy of every file-scope static (Utils::g_shaderPathResolver,
# UIManager singletons, etc.). Engine::Initialize would set Engine.dll's copy and
# consumer-side dispatch would see null. PRIVATE keeps the single-copy invariant.
#
# Header propagation: each module's INTERFACE_INCLUDE_DIRECTORIES and
# INTERFACE_COMPILE_DEFINITIONS are re-exported via Engine's INTERFACE so consumers
# can still `#include <Rendering/...>`, `<UI/...>`, `<ECS/...>` without naming the
# module on their link line.
# Every OBJECT module that should land inside Engine.dll. The PRIVATE link below
# splices each module's .obj files into Engine's SHARED link, and CMake's
# __create_def walks them for export when WINDOWS_EXPORT_ALL_SYMBOLS=ON.
#
# Editor-only modules (MeshPicking; VCSIntegration, which lives in
# EditorSDK.dll) are NOT listed here — Engine.dll doesn't use them, and
# packaged games must ship no VCS code (every VCS provider is an
# Packages/*-vcs Editor-kind package module). Header-only INTERFACE
# libs (Mathematics, Types) are also omitted since they have no .obj files
# to splice.
#
# Graph WAS editor-only before commit c38828aee added MaterialCompiler /
# CoreAssetRegistrations using it. It now lives inside Engine.dll like the
# other ECS-ish modules; Editor.exe drops its direct Graph link to avoid
# duplicate-state across the DLL boundary.
set(GE_ENGINE_MODULES
    Logger AssetCore AssetDatabase JobSystem ECS FileWatcher AssetRuntime Audio Scripting NativeScripting
    Platform FileSystem FileSystemCore Memory GameEngineRendering RenderServices VolumetricFog HeightFog GPUFogParticles Particles ParticlesAuthoring ParticlesAssets ParticlesSystems ParticlesRendering Animation NumericExpression UI Physics PhysicsECS
    Pathfinding PathfindingECS Terrain PageStreaming TerrainECS TerrainGrass CBTTerrain CBTTerrainECS Ocean Spline SplineGeometry SplineLayout SplineECS MarkupECS Graph
    GenerationalVector Scheduler Scene SceneBvh Text Input PluginAPI
    PlatformServices PlatformProvider${GE_PLATFORM_BACKEND})
target_link_libraries(Engine
    PRIVATE ${GE_ENGINE_MODULES} ${PLATFORM_LIBS} concurrentqueue::concurrentqueue
)
foreach(_ge_mod IN LISTS GE_ENGINE_MODULES)
    # Splice each OBJECT module's .obj files into Engine via TARGET_OBJECTS so
    # the linker sees them at link time. (NOTE: CMake's __create_def for
    # WINDOWS_EXPORT_ALL_SYMBOLS still does NOT walk these on the VS generator —
    # that's why we run GenerateEngineExportsDef.cmake separately below.)
    target_sources(Engine PRIVATE $<TARGET_OBJECTS:${_ge_mod}>)
    target_include_directories(Engine INTERFACE
        $<TARGET_PROPERTY:${_ge_mod},INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions(Engine INTERFACE
        $<TARGET_PROPERTY:${_ge_mod},INTERFACE_COMPILE_DEFINITIONS>)
endforeach()

# Engine.dll's exports.def is generated by GenerateEngineExportsDef.cmake at
# PRE_LINK time. It invokes the same internal `cmake -E __create_def` helper
# that WINDOWS_EXPORT_ALL_SYMBOLS would, but with a complete object-file list
# that also includes every OBJECT module's .obj files (CMake's auto-mechanism
# misses them on the VS generator). Use CMake's TARGET_OBJECTS generator
# expressions for module objects instead of globbing module build directories:
# Visual Studio can leave stale .obj files behind after source moves/removals,
# and stale objects must not leak into Engine.dll's export table.
if(MSVC)
    set(_engine_def_file "$<TARGET_PROPERTY:Engine,BINARY_DIR>/Engine.dir/$<CONFIG>/exports.def")
    set(_engine_module_objlist_file "${CMAKE_CURRENT_BINARY_DIR}/Engine.dir/$<CONFIG>/engine-module-objects.txt")
    # Engine's own objects ride the same TARGET_OBJECTS list as the modules: the
    # ENGINE_OBJ_DIR glob inside the script only matches the VS generator's
    # Engine.dir/<Config>/ object layout, so Ninja builds would otherwise export
    # module symbols but none of Engine's own (the script dedupes the overlap).
    set(_engine_module_object_genex "$<TARGET_OBJECTS:Engine>")
    foreach(_ge_mod IN LISTS GE_ENGINE_MODULES)
        list(APPEND _engine_module_object_genex "$<TARGET_OBJECTS:${_ge_mod}>")
    endforeach()
    file(GENERATE
        OUTPUT "${_engine_module_objlist_file}"
        CONTENT "$<JOIN:${_engine_module_object_genex},\n>\n"
    )

    add_custom_command(TARGET Engine PRE_LINK
        COMMAND ${CMAKE_COMMAND}
            "-DENGINE_OBJ_DIR=$<TARGET_PROPERTY:Engine,BINARY_DIR>/Engine.dir/$<CONFIG>"
            "-DENGINE_PROJECT_FILE=${CMAKE_CURRENT_BINARY_DIR}/Engine.vcxproj"
            "-DMODULE_OBJECTS_FILE=${_engine_module_objlist_file}"
            "-DOUTPUT_DEF=${_engine_def_file}"
            -P ${CMAKE_SOURCE_DIR}/cmake/GenerateEngineExportsDef.cmake
        COMMENT "Generating Engine.dll exports.def (Engine + OBJECT modules)"
        VERBATIM
    )
    # Hand the linker our .def explicitly. Without WINDOWS_EXPORT_ALL_SYMBOLS,
    # CMake won't add /DEF on its own.
    target_link_options(Engine PRIVATE "/DEF:${_engine_def_file}")
endif()

# Vendored ufbx (single-file FBX importer). Compiled in on every platform.
target_sources(Engine PRIVATE Source/ThirdParty/ufbx/ufbx.c)
target_include_directories(Engine PRIVATE Source/ThirdParty/ufbx)
target_compile_definitions(Engine PRIVATE GE_HAVE_UFBX)
set_source_files_properties(Source/ThirdParty/ufbx/ufbx.c PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON
    SKIP_UNITY_BUILD_INCLUSION ON)

# Vendored fbtBlend (header-only Blender .blend reader, Zlib license).
# See Engine/Source/ThirdParty/fbtBlend/UPDATE-NOTES.md for upstream pin.
target_sources(Engine PRIVATE Source/ThirdParty/fbtBlend/fbtBlend.cpp)
target_include_directories(Engine PRIVATE Source/ThirdParty/fbtBlend)
target_compile_definitions(Engine PRIVATE GE_HAVE_FBTBLEND=1)
set_source_files_properties(Source/ThirdParty/fbtBlend/fbtBlend.cpp PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON
    SKIP_UNITY_BUILD_INCLUSION ON
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/WX->")

# ModelAssetLoadBlend.cpp transitively includes fbtBlend.h which is large +
# emits MSVC narrowing warnings even with #pragma warning push/disable at the
# include boundary (some warnings escape the pragma scope). Mirror the codec
# shim's flags so the TU stays out of the unity build and tolerates upstream
# warnings.
set_source_files_properties(Source/Assets/ModelAssetLoadBlend.cpp PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON
    SKIP_UNITY_BUILD_INCLUSION ON
    COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/WX->")

# Optional zlib for fbtBlend (Blender < 3.0 PM_COMPRESSED .blend files).
find_package(ZLIB QUIET)
if(TARGET ZLIB::ZLIB)
    target_link_libraries(Engine PRIVATE ZLIB::ZLIB)
    target_compile_definitions(Engine PRIVATE GE_FBTBLEND_HAVE_ZLIB=1)
    set_source_files_properties(Source/ThirdParty/fbtBlend/fbtBlend.cpp PROPERTIES
        COMPILE_DEFINITIONS "GE_FBTBLEND_HAVE_ZLIB=1")
else()
    message(STATUS "[fbtBlend] zlib not found; pre-3.0 compressed .blend files will be unsupported.")
endif()

# Optional zstd for fbtBlend (Blender >= 3.0 zstd-compressed .blend files).
find_package(zstd CONFIG QUIET)
if(TARGET zstd::libzstd_static OR TARGET zstd::libzstd_shared OR TARGET zstd::libzstd)
    if(TARGET zstd::libzstd_static)
        target_link_libraries(Engine PRIVATE zstd::libzstd_static)
    elseif(TARGET zstd::libzstd_shared)
        target_link_libraries(Engine PRIVATE zstd::libzstd_shared)
    else()
        target_link_libraries(Engine PRIVATE zstd::libzstd)
    endif()
    target_compile_definitions(Engine PRIVATE GE_FBTBLEND_HAVE_ZSTD=1)
    get_source_file_property(_fbt_existing_defs Source/ThirdParty/fbtBlend/fbtBlend.cpp COMPILE_DEFINITIONS)
    if(_fbt_existing_defs STREQUAL "NOTFOUND")
        set(_fbt_existing_defs "")
    endif()
    list(APPEND _fbt_existing_defs "GE_FBTBLEND_HAVE_ZSTD=1")
    set_source_files_properties(Source/ThirdParty/fbtBlend/fbtBlend.cpp PROPERTIES
        COMPILE_DEFINITIONS "${_fbt_existing_defs}")
else()
    message(STATUS "[fbtBlend] zstd not found via CONFIG; .blend files saved by Blender >= 3.0 (compressed) will be unsupported.")
endif()

# Optionally link asset importers if present
if(TARGET tinyobjloader::tinyobjloader)
    target_link_libraries(Engine PRIVATE tinyobjloader::tinyobjloader)
    target_compile_definitions(Engine PRIVATE GE_HAVE_TINYOBJ)
endif()
# cgltf is header-only via vcpkg (no config target): include path detection
find_path(CGLTF_INCLUDE_DIRS "cgltf.h")
if(CGLTF_INCLUDE_DIRS)
    target_include_directories(Engine PRIVATE ${CGLTF_INCLUDE_DIRS})
    target_compile_definitions(Engine PRIVATE GE_HAVE_CGLTF)
    target_sources(Engine PRIVATE Source/ThirdParty/cgltf_impl.cpp)
endif()
# meshoptimizer: mesh simplification for automatic LOD generation.
if(TARGET meshoptimizer::meshoptimizer)
    target_link_libraries(Engine PRIVATE meshoptimizer::meshoptimizer)
    target_compile_definitions(Engine PRIVATE GE_HAVE_MESHOPTIMIZER)

    # Fold the pinned meshopt version into the cooked-LOD cache config hash so a
    # version bump (which changes produced indices for an unchanged config) is a
    # cache miss instead of a silent stale cook (design v0.2 §3.2, A3/F7). Read
    # the committed port manifest at configure time; a version string ("1.1.1")
    # is what the cook + Player key must agree on. Fall back to whatever
    # find_package reported, then to a sentinel, so the build never hardcodes it.
    set(GE_MESHOPT_VERSION_STR "${meshoptimizer_VERSION}")
    set(_ge_meshopt_port "${CMAKE_SOURCE_DIR}/dependencies/vcpkg/ports/meshoptimizer/vcpkg.json")
    if(EXISTS "${_ge_meshopt_port}")
        # Re-run configure when the port version bumps, so the baked key follows
        # the pin (F2). Residual risk: if meshopt is ever resolved from a
        # different overlay/registry than this port, this file is not the source
        # of truth and the key can lag — revisit if the meshopt provenance moves.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_ge_meshopt_port}")
        file(READ "${_ge_meshopt_port}" _ge_meshopt_manifest)
        string(JSON _ge_meshopt_ver ERROR_VARIABLE _ge_meshopt_err
               GET "${_ge_meshopt_manifest}" "version")
        if(NOT _ge_meshopt_err AND _ge_meshopt_ver)
            set(GE_MESHOPT_VERSION_STR "${_ge_meshopt_ver}")
        endif()
    endif()
    if(NOT GE_MESHOPT_VERSION_STR)
        set(GE_MESHOPT_VERSION_STR "unpinned")
    endif()
    target_compile_definitions(Engine PRIVATE
        GE_MESHOPT_VERSION="meshopt-${GE_MESHOPT_VERSION_STR}")
    message(STATUS "Cooked-LOD cache meshopt version key: meshopt-${GE_MESHOPT_VERSION_STR}")
endif()

# stb_image (header-only via vcpkg config target or include path)
# Try modern imported target first
if(TARGET stb::stb)
    target_link_libraries(Engine PRIVATE stb::stb)
    target_compile_definitions(Engine PRIVATE GE_HAVE_STB)
    target_sources(Engine PRIVATE Source/ThirdParty/stb_image_impl.cpp)
endif()

# Fallback: detect headers directly if no CMake target is exported
find_path(STB_INCLUDE_DIRS "stb_image.h")
if(STB_INCLUDE_DIRS)
    target_include_directories(Engine PRIVATE ${STB_INCLUDE_DIRS})
    target_compile_definitions(Engine PRIVATE GE_HAVE_STB)
    target_sources(Engine PRIVATE Source/ThirdParty/stb_image_impl.cpp)
endif()

# Fold the stb_image_resize2 version into the texture-cook cache config hash: a
# version bump changes mip-chain bytes for an unchanged recipe, which must be a
# cache miss, not a silently stale artifact (mirrors GE_MESHOPT_VERSION above).
# stbir exposes no version macro, so read the release tag from the header's
# first line at configure time; CONFIGURE_DEPENDS re-runs configure when a
# vcpkg upgrade rewrites the header.
find_path(GE_STBIR_INCLUDE_DIR "stb_image_resize2.h")
set(GE_STBIR_VERSION_STR "unpinned")
if(GE_STBIR_INCLUDE_DIR)
    set(_ge_stbir_header "${GE_STBIR_INCLUDE_DIR}/stb_image_resize2.h")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_ge_stbir_header}")
    file(STRINGS "${_ge_stbir_header}" _ge_stbir_first_line LIMIT_COUNT 1)
    if(_ge_stbir_first_line MATCHES "stb_image_resize2 - v([0-9]+\\.[0-9]+)")
        set(GE_STBIR_VERSION_STR "${CMAKE_MATCH_1}")
    endif()
endif()
target_compile_definitions(Engine PRIVATE GE_STBIR_VERSION="stbir-${GE_STBIR_VERSION_STR}")
message(STATUS "Texture-cook cache stbir version key: stbir-${GE_STBIR_VERSION_STR}")

    # libktx (KTX/KTX2 texture loading via vcpkg). The vcpkg package exports
    # KTX::ktx (capitalized) — the previous lowercase ktx::ktx check never
    # matched, silently compiling ALL KTX2 loading out of the engine.
    # PRIVATE: no public header includes <ktx.h>, and libktx carries
    # process-global encoder/transcoder state. Propagating a static archive of
    # it (every non-Windows triplet) would give each consumer executable its own
    # partial copy alongside this dylib's. GE_HAVE_KTX stays PUBLIC, so the one
    # target whose own sources call libktx — the Engine test umbrella below —
    # links KTX::ktx directly.
    find_package(Ktx CONFIG QUIET)
    if(TARGET KTX::ktx)
        target_link_libraries(Engine PRIVATE KTX::ktx)
        target_compile_definitions(Engine PUBLIC GE_HAVE_KTX)
    else()
        message(WARNING "libktx not found: KTX2 texture loading will be unavailable")
    endif()

    # VkFormat constants for the KTX2 loader/cook (header-only usage).
    find_path(GE_ENGINE_VULKAN_INCLUDE_DIRS "vulkan/vulkan_core.h")
    if(GE_ENGINE_VULKAN_INCLUDE_DIRS)
        target_include_directories(Engine PRIVATE ${GE_ENGINE_VULKAN_INCLUDE_DIRS})
    endif()

    # DirectXTex (vcpkg on Windows, Linux and macOS; the macOS ports are the
    # overlays in cmake/macos-ports): BCn block compression for the texture
    # import cook and the package cook. Optional — without it the cook still
    # bakes mip chains but resolves every format to Uncompressed, so a host
    # without it ships RGBA8. Macs sample BC (MTLDevice
    # supportsBCTextureCompression is YES on Apple silicon), so a macOS host
    # cooks the same BC artifacts as the other desktop hosts.
    find_package(directxtex CONFIG QUIET)
    if(TARGET Microsoft::DirectXTex)
        target_link_libraries(Engine PRIVATE Microsoft::DirectXTex)
        target_compile_definitions(Engine PRIVATE GE_HAVE_DIRECTXTEX)
    else()
        message(STATUS "DirectXTex not found: texture cook BCn encoding unavailable")
    endif()

    # Include directories
    target_include_directories(Engine PUBLIC Include)
    target_include_directories(Engine PUBLIC ${CMAKE_SOURCE_DIR}/Engine/Modules/VolumetricFog/Include)
    target_include_directories(Engine PUBLIC ${CMAKE_SOURCE_DIR}/Engine/Modules/HeightFog/Include)
    # Particles ships its own Include/Components scanned into ComponentReflection.gen.cpp.
    target_include_directories(Engine PUBLIC ${CMAKE_SOURCE_DIR}/Engine/Modules/Particles/Include)
    # Ocean ships its own Include/Components scanned into ComponentReflection.gen.cpp.
    target_include_directories(Engine PUBLIC ${CMAKE_SOURCE_DIR}/Engine/Modules/Ocean/Include)
    target_include_directories(Engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/ECSModules/Rendering/Include)
    target_include_directories(Engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/ECSModules/Audio/Include)

    # Private includes for Engine implementation details (including UI asset integration shims)
    target_include_directories(Engine PRIVATE Source)
    target_include_directories(Engine PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Modules/UI/Include)

# Platform-specific linking for .NET hosting (nethost)
if(NETHOST_LIBRARY)
	target_include_directories(Engine PRIVATE ${NETHOST_INCLUDE_DIR})
	target_include_directories(RenderServices PRIVATE ${NETHOST_INCLUDE_DIR})
	# nethost is consumed by Scripting (STATIC). Since Engine PUBLIC-links
	# Scripting today, Scripting's TUs leak into consumers (Editor.exe,
	# Player.exe) until the SHARED-aggregator refactor flips Engine's
	# PUBLIC module links to PRIVATE. Until then, propagate nethost via
	# INTERFACE so the consumer link line can satisfy the dllimport calls
	# CoreCLRHost makes against get_hostfxr_path / hostfxr_initialize_*.
	target_link_libraries(Engine PUBLIC ${NETHOST_LIBRARY})
	add_definitions(-DNETHOST_AVAILABLE)
	if(WIN32 AND MSVC)
	    # Delay-load nethost.dll is only relevant for final binaries that actually host .NET
	    # Do not propagate this to dependents to avoid warnings in test executables that don't import nethost
	    target_link_options(Engine PRIVATE "/DELAYLOAD:nethost.dll")
	    target_link_libraries(Engine PRIVATE delayimp)
	endif()
	message(STATUS "Engine: Linking with .NET hosting library")
elseif(WIN32)
	message(STATUS "Engine: .NET hosting library not available on Windows, using stubs")
else()
	message(STATUS "Engine: .NET hosting library not available on this platform, using stubs")
endif()


# Native ABI DLL: GameEngine.Native
# Only build the native scripting shim when scripting support is enabled.
if(ENABLE_SCRIPTING)
  add_library(GameEngine.Native SHARED
      Source/Scripting/ScriptingABI.cpp
      Source/Scripting/TimelineABI.cpp
      Source/Scripting/AnimatorABI.cpp
      Source/Scripting/ModelABI.cpp
      Source/Scripting/ECSABI.cpp
      Source/Scripting/GameUIABI.cpp
      Source/Scripting/UIElementABI.cpp
      Source/Scripting/UIElementValueABI.cpp
      Source/Scripting/UIElementTypeABI.cpp
      Source/Scripting/PhysicsABI.cpp
      Source/Scripting/PlatformServicesABI.cpp
  )

  # Export symbols from this target
  target_compile_definitions(GameEngine.Native PRIVATE GE_SCRIPTING_BUILD)
  ge_add_production_allocation_hook(GameEngine.Native)

  # Phase 1: Engine is now SHARED. GameEngine.Native links against Engine's
  # import lib (Windows) / soname (POSIX) like any other dynamic consumer.
  # The previous --whole-archive / -force_load dance was needed to pull
  # Engine's static .o files into a SHARED GameEngine.Native; with Engine
  # itself SHARED, those flags would error or be no-ops.
  target_link_libraries(GameEngine.Native PRIVATE Engine)

	  # When mimalloc is enabled, ensure the native ABI DLL participates in the same
	  # allocator/override setup as the host Editor executable to avoid cross-module
	  # allocator mismatches.
	  if(ENABLE_MIMALLOC)
	    find_package(mimalloc CONFIG REQUIRED)

			    # Only enable mimalloc when the *shared library* is available; otherwise fall back
			    # to the system allocator to avoid cross-module heap mismatches.
			    if(TARGET mimalloc)
			      set(_GE_MIMALLOC_IS_SHARED FALSE)
			      get_target_property(_ge_mimalloc_type mimalloc TYPE)
			      if(_ge_mimalloc_type STREQUAL "SHARED_LIBRARY" OR _ge_mimalloc_type STREQUAL "MODULE_LIBRARY")
			        set(_GE_MIMALLOC_IS_SHARED TRUE)
			      else()
			        get_target_property(_ge_mimalloc_is_imported mimalloc IMPORTED)
			        if(_ge_mimalloc_is_imported)
			          foreach(_ge_prop IN ITEMS
			              IMPORTED_LOCATION
			              IMPORTED_LOCATION_DEBUG
			              IMPORTED_LOCATION_RELEASE
			              IMPORTED_LOCATION_RELWITHDEBINFO
			              IMPORTED_LOCATION_MINSIZEREL
			          )
			            get_target_property(_ge_loc mimalloc ${_ge_prop})
			            if(_ge_loc AND NOT _ge_loc MATCHES "NOTFOUND")
			              get_filename_component(_ge_ext "${_ge_loc}" EXT)
			              string(TOLOWER "${_ge_ext}" _ge_ext_lower)
			              if(_ge_ext_lower STREQUAL ".dll" OR _ge_ext_lower STREQUAL ".so" OR _ge_ext_lower STREQUAL ".dylib")
			                set(_GE_MIMALLOC_IS_SHARED TRUE)
			                break()
			              endif()
			            endif()
			          endforeach()
			        endif()
			      endif()

			      if(_GE_MIMALLOC_IS_SHARED)
			        set(_GE_MIMALLOC_OVERRIDE_LIB "")
			        foreach(_candidate IN ITEMS mimalloc-override mimalloc-override-shared)
			          if(TARGET ${_candidate})
			            set(_GE_MIMALLOC_OVERRIDE_LIB ${_candidate})
			            break()
			          endif()
			        endforeach()

			        # Link order matters for some mimalloc override/redirect variants on Windows.
			        # Ensure the allocator library is linked before the override shim.
			        target_link_libraries(GameEngine.Native PRIVATE mimalloc)
			        if(_GE_MIMALLOC_OVERRIDE_LIB)
			          target_link_libraries(GameEngine.Native PRIVATE ${_GE_MIMALLOC_OVERRIDE_LIB})
			        else()
			          target_sources(GameEngine.Native PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Source/Memory/MimallocOverrides.cpp)
			        endif()
			      else()
			        message(STATUS "GameEngine.Native: mimalloc target is not a shared library; disabling mimalloc (falling back to system allocator)")
			      endif()
			    else()
			      message(STATUS "GameEngine.Native: mimalloc target not available; disabling mimalloc (falling back to system allocator)")
			    endif()
	  endif()

	  # Ensure GameEngine.Native sees NETHOST headers/libs if available
	  if(NETHOST_LIBRARY)
	    target_include_directories(GameEngine.Native PRIVATE ${NETHOST_INCLUDE_DIR})
	    target_link_libraries(GameEngine.Native PRIVATE ${NETHOST_LIBRARY})
	    if(WIN32 AND MSVC)
	      target_link_options(GameEngine.Native PRIVATE "/DELAYLOAD:nethost.dll")
	      target_link_libraries(GameEngine.Native PRIVATE delayimp)
	    endif()
	    # Managed interop tests resolve GameEngine.Native.dll straight from this
	    # output dir (GE_NATIVE_DIR / PATH in their ctest environment). The delay-loaded
	    # nethost.dll (+ hostfxr.dll) must sit beside it, or CLR bring-up SEH-fails into
	    # Inconclusive in tests-only builds that never stage an Editor/Player.
	    if(COMMAND ge_add_dotnet_hosting_runtime)
	      ge_add_dotnet_hosting_runtime(GameEngine.Native)
	    endif()
	  endif()

  # Include (public API) plus Source, which carries GameUI/SessionEvent.h — the
  # session-gated event registration this shim is the only production consumer of,
  # kept off the public C++ surface deliberately.
  target_include_directories(GameEngine.Native PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/Include
      ${CMAKE_CURRENT_SOURCE_DIR}/Source)

  # Output directories and name.
  # WINDOWS_EXPORT_ALL_SYMBOLS ensures __declspec(dllexport) symbols from the
  # ABI .cpp files are actually exported from the DLL, even when Engine.lib
  # (compiled with GE_SCRIPTING_STATIC) is also linked in. Without this,
  # MSVC's linker may resolve ABI symbols from Engine.lib (no export
  # decoration) instead of the DLL's .obj (dllexport), silently dropping them
  # from the export table.
  set_target_properties(GameEngine.Native PROPERTIES
      OUTPUT_NAME "GameEngine.Native"
      WINDOWS_EXPORT_ALL_SYMBOLS ON
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
      RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_BINARY_DIR}/bin/Debug
      RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin/Release
  )
endif()

# The first-party libraries the applications stage beside themselves. Each link
# refreshes every application's staged copy (ge_refresh_application_copies in
# cmake/OutputLayout.cmake), so the Editor and the Player of one configuration
# always load the same builds.
foreach(_ge_application_library Engine GameEngine.Native)
    if(TARGET ${_ge_application_library})
        ge_refresh_application_copies(${_ge_application_library})
    endif()
endforeach()


# Engine tests: Hot-reload bridge smoke test

# Engine owns the hot-reload native stubs. Scripting is an object library that is
# linked into Engine, so adding the same stub source to Scripting would duplicate
# the exported GE_* symbols when GameEngine.Native is not part of the build.
if(NOT TARGET GameEngine.Native)
    message(STATUS "Engine: GameEngine.Native not present -> using Engine-owned HotReload native stubs")
endif()

# Only discover GTest for test configures: every test block below gates on
# GTest_FOUND / GTest::gtest, so this single find is what keeps them (and
# their ~dozens of exes) out of BUILD_TESTING=OFF builds.
if(BUILD_TESTING)
    find_package(GTest CONFIG QUIET)
endif()
if(BUILD_TESTING AND (GTest_FOUND OR TARGET GTest::gtest))
    # Link the runtime API through Engine, with no editor include directories or
    # implementation TUs. This catches missing DLL exports as well as API drift.
    add_executable(EngineSplineLayoutTests Tests/SplineLayoutRuntimeTests.cpp)
    target_link_libraries(EngineSplineLayoutTests PRIVATE Engine GTest::gtest GTest::gtest_main)
    target_compile_features(EngineSplineLayoutTests PRIVATE cxx_std_20)
    set_target_properties(EngineSplineLayoutTests PROPERTIES FOLDER "Engine/Tests")
    ge_set_output_tests(EngineSplineLayoutTests)
    ge_stage_runtime_dependencies(EngineSplineLayoutTests)
    add_test(NAME EngineSplineLayoutTests COMMAND $<TARGET_FILE:EngineSplineLayoutTests>)
endif()
if(GTest_FOUND OR TARGET GTest::gtest)
    add_executable(EngineHotReloadBridgeSmokeTests Tests/RenderingHotReloadBridgeSmokeTests.cpp)
    target_link_libraries(EngineHotReloadBridgeSmokeTests PRIVATE Engine GTest::gtest GTest::gtest_main)
    if(TARGET GameEngine.Native)
        target_link_libraries(EngineHotReloadBridgeSmokeTests PRIVATE GameEngine.Native)
    endif()
    set_target_properties(EngineHotReloadBridgeSmokeTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Engine/Tests"
    )
    ge_set_output_tests(EngineHotReloadBridgeSmokeTests)
    ge_stage_runtime_dependencies(EngineHotReloadBridgeSmokeTests)
    add_test(NAME EngineHotReloadBridgeSmokeTests COMMAND $<TARGET_FILE:EngineHotReloadBridgeSmokeTests>)
endif()

if(GTest_FOUND OR TARGET GTest::gtest)
    add_executable(OceanDepthCacheBakerTests Tests/OceanDepthCacheBakerTests.cpp
        ${CMAKE_SOURCE_DIR}/Tests/EngineShutdownTestEnvironment.cpp)
    target_link_libraries(OceanDepthCacheBakerTests PRIVATE Engine GTest::gtest)
    set_target_properties(OceanDepthCacheBakerTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Engine/Tests"
    )
    ge_set_output_tests(OceanDepthCacheBakerTests)
    ge_stage_runtime_dependencies(OceanDepthCacheBakerTests)
    add_test(NAME OceanDepthCacheBakerTests COMMAND $<TARGET_FILE:OceanDepthCacheBakerTests>)
    set_tests_properties(OceanDepthCacheBakerTests PROPERTIES LABELS "Ocean;Terrain;Integration")
endif()

if(GTest_FOUND OR TARGET GTest::gtest)
    # Raw/cooked upload parity needs a disposable EngineCore asset registry.
    # Keep its lifecycle separate from the EngineCore-free render-services test executables.
    add_executable(TextureAlphaCoverageGpuTests Tests/TextureAlphaCoverageGpuTests.cpp
        ${CMAKE_SOURCE_DIR}/Tests/EngineShutdownTestEnvironment.cpp)
    target_link_libraries(TextureAlphaCoverageGpuTests PRIVATE Engine GTest::gtest GTest::gtest_main)
    set_target_properties(TextureAlphaCoverageGpuTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Engine/Tests"
    )
    ge_set_output_tests(TextureAlphaCoverageGpuTests)
    ge_stage_runtime_dependencies(TextureAlphaCoverageGpuTests)
    add_test(NAME TextureAlphaCoverageGpuTests COMMAND $<TARGET_FILE:TextureAlphaCoverageGpuTests>)
    set_tests_properties(TextureAlphaCoverageGpuTests PROPERTIES LABELS "Texture;GPU;Integration")
endif()

if(GTest_FOUND OR TARGET GTest::gtest)
    # The bind-time cook usage tag is written through the EngineCore asset registry, so the
    # tests that drive it own an EngineCore lifecycle, kept out of the EngineCore-free
    # render-services test executables.
    add_executable(TextureCookUsageTagTests Tests/TextureCookUsageTagTests.cpp
        ${CMAKE_SOURCE_DIR}/Tests/EngineShutdownTestEnvironment.cpp)
    target_link_libraries(TextureCookUsageTagTests PRIVATE Engine GTest::gtest GTest::gtest_main)
    set_target_properties(TextureCookUsageTagTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Engine/Tests"
    )
    ge_set_output_tests(TextureCookUsageTagTests)
    ge_stage_runtime_dependencies(TextureCookUsageTagTests)
    add_test(NAME TextureCookUsageTagTests COMMAND $<TARGET_FILE:TextureCookUsageTagTests>)
    set_tests_properties(TextureCookUsageTagTests PROPERTIES LABELS "Texture;Integration")
endif()

if(GTest_FOUND OR TARGET GTest::gtest)
    add_executable(SkyRendererCameraStateTests Tests/SkyRendererCameraStateTests.cpp)
    target_link_libraries(SkyRendererCameraStateTests PRIVATE Engine GTest::gtest GTest::gtest_main)
    set_target_properties(SkyRendererCameraStateTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Engine/Tests"
    )
    ge_set_output_tests(SkyRendererCameraStateTests)
    ge_stage_runtime_dependencies(SkyRendererCameraStateTests)
    add_test(NAME SkyRendererCameraStateTests COMMAND $<TARGET_FILE:SkyRendererCameraStateTests>)
endif()

# Engine tests: Hot-reload bridge selective invalidation test
if(GTest_FOUND OR TARGET GTest::gtest)
    add_executable(EngineHotReloadBridgeSelectiveInvalidationTests Tests/RenderingHotReloadBridgeSelectiveInvalidationTests.cpp)
    target_link_libraries(EngineHotReloadBridgeSelectiveInvalidationTests PRIVATE Engine GTest::gtest GTest::gtest_main)
    target_include_directories(EngineHotReloadBridgeSelectiveInvalidationTests PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Tests)
    target_compile_definitions(EngineHotReloadBridgeSelectiveInvalidationTests PRIVATE
        RENDERING_SOURCE_DIR="${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering"
        RENDERING_SHADER_OUTPUT_DIR="${CMAKE_BINARY_DIR}/Shaders")
    if(TARGET GameEngine.Native)
        target_link_libraries(EngineHotReloadBridgeSelectiveInvalidationTests PRIVATE GameEngine.Native)
    endif()
    set_target_properties(EngineHotReloadBridgeSelectiveInvalidationTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Engine/Tests"
    )
    ge_set_output_tests(EngineHotReloadBridgeSelectiveInvalidationTests)
    ge_stage_runtime_dependencies(EngineHotReloadBridgeSelectiveInvalidationTests)
    add_test(NAME EngineHotReloadBridgeSelectiveInvalidationTests COMMAND $<TARGET_FILE:EngineHotReloadBridgeSelectiveInvalidationTests>)
endif()
# Engine tests: Application Tick/RunPlatformLoop driver contract
if(GTest_FOUND OR TARGET GTest::gtest)
    add_executable(EngineApplicationTickTests
        Tests/ApplicationTickTests.cpp
        )
    target_link_libraries(EngineApplicationTickTests PRIVATE Engine GTest::gtest GTest::gtest_main)
    set_target_properties(EngineApplicationTickTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Engine/Tests"
    )
    ge_set_output_tests(EngineApplicationTickTests)
    ge_stage_runtime_dependencies(EngineApplicationTickTests)
    add_test(NAME EngineApplicationTickTests COMMAND $<TARGET_FILE:EngineApplicationTickTests>)
endif()

# Engine tests: shaderpkg WGSL chunk round-trip
if(GTest_FOUND OR TARGET GTest::gtest)
    add_executable(ShaderPackageWgslTests
        Tests/ShaderPackageWgslTests.cpp
        )
    target_link_libraries(ShaderPackageWgslTests PRIVATE Engine GTest::gtest GTest::gtest_main)
    set_target_properties(ShaderPackageWgslTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Engine/Tests"
    )
    ge_set_output_tests(ShaderPackageWgslTests)
    ge_stage_runtime_dependencies(ShaderPackageWgslTests)
    add_test(NAME ShaderPackageWgslTests COMMAND $<TARGET_FILE:ShaderPackageWgslTests>)
endif()

# Engine tests: CompileServerUtil pipe-name determinism
if(GTest_FOUND OR TARGET GTest::gtest)
    add_executable(OceanQueryTests
        Tests/OceanQueryTests.cpp
        Tests/OceanPresetOverrideTests.cpp
        Tests/OceanScheduleTests.cpp
        )
    target_link_libraries(OceanQueryTests PRIVATE Engine GTest::gtest GTest::gtest_main)
    set_target_properties(OceanQueryTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Engine/Tests"
    )
    ge_set_output_tests(OceanQueryTests)
    ge_stage_runtime_dependencies(OceanQueryTests)
    add_test(NAME OceanQueryTests COMMAND $<TARGET_FILE:OceanQueryTests>)
endif()

# Packaged content crosses the public export pipeline and the Player manifest mount.
if(GTest_FOUND OR TARGET GTest::gtest)
    add_executable(PackagedAssetManifestTests Tests/PackagedAssetManifestTests.cpp)
    target_link_libraries(PackagedAssetManifestTests PRIVATE Engine GTest::gtest_main)
    target_include_directories(PackagedAssetManifestTests PRIVATE ${CMAKE_SOURCE_DIR}/Tests)
    target_compile_features(PackagedAssetManifestTests PRIVATE cxx_std_20)
    ge_set_output_tests(PackagedAssetManifestTests)
    ge_stage_runtime_dependencies(PackagedAssetManifestTests)
    add_test(NAME PackagedAssetManifestTests COMMAND PackagedAssetManifestTests)
endif()

# Engine tests: automatic LOD generation (meshoptimizer)
if(GTest_FOUND OR TARGET GTest::gtest)
    add_executable(MeshLODGeneratorTests
        Tests/MeshLODGeneratorTests.cpp
        Tests/MeshLODGeometryTests.cpp
        Tests/LodAssetSettingsTests.cpp
        Tests/MeshLODCacheTests.cpp
        Tests/PackagedModelLodTests.cpp
        Tests/AuthoredLODColorTests.cpp
        )
    target_link_libraries(MeshLODGeneratorTests PRIVATE Engine GTest::gtest GTest::gtest_main)
    target_include_directories(MeshLODGeneratorTests PRIVATE ${CMAKE_SOURCE_DIR}/Tests) # TestTempDir.h
    set_target_properties(MeshLODGeneratorTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Engine/Tests"
    )
    ge_set_output_tests(MeshLODGeneratorTests)
    ge_stage_runtime_dependencies(MeshLODGeneratorTests)
    add_test(NAME MeshLODGeneratorTests COMMAND $<TARGET_FILE:MeshLODGeneratorTests>)

    # Engine tests: HLOD offline pipeline (clustering, benefit heuristic, .gehlod).
    add_executable(EngineHlodBakeTests
        Tests/HlodClusteringTests.cpp
        Tests/HlodBenefitTests.cpp
        Tests/HlodMeshMergeTests.cpp
        Tests/HlodCacheTests.cpp
        Tests/HlodBakerTests.cpp
        Tests/HlodMemberGatherTests.cpp
        Tests/HlodSelectTests.cpp
        Tests/HlodReconcileTests.cpp
        Tests/HlodBakeDriverTests.cpp
        )
    # GameEngine.Native supplies the ECS component handler symbols the member-gather
    # test instantiates (World + MeshRenderer/WorldTransform/... e.Set paths); the
    # other HLOD suites are pure and need only Engine.
    target_link_libraries(EngineHlodBakeTests PRIVATE Engine GameEngine.Native GTest::gtest GTest::gtest_main)
    set_target_properties(EngineHlodBakeTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Engine/Tests"
    )
    ge_set_output_tests(EngineHlodBakeTests)
    ge_stage_runtime_dependencies(EngineHlodBakeTests)
    add_test(NAME EngineHlodBakeTests COMMAND $<TARGET_FILE:EngineHlodBakeTests>)

    # Focused GPU coverage for the complete GTAO shader chain and history policy.
    add_executable(GtaoTests
        Tests/GtaoQualityTests.cpp
        Tests/GtaoPackingContractTests.cpp
        Tests/GtaoComputeTests.cpp
        Tests/EngineTestShaderSetup.cpp)
    target_link_libraries(GtaoTests PRIVATE Engine GTest::gtest GTest::gtest_main)
    target_compile_definitions(GtaoTests PRIVATE ENGINE_TEST_BUILD_DIR="${CMAKE_BINARY_DIR}" GE_RENDERER_REPO_ROOT="${PROJECT_SOURCE_DIR}")
    set_target_properties(GtaoTests PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON FOLDER "Engine/Tests")
    ge_set_output_tests(GtaoTests)
    ge_stage_runtime_dependencies(GtaoTests)
    # Loads "Shaders/<kernel>.shaderpkg"; the edge resolves at generate time.
    add_dependencies(GtaoTests CompileShaderPkgs)
    add_test(NAME GtaoTests COMMAND $<TARGET_FILE:GtaoTests>)

    add_executable(EngineCompileServerUtilTests
        Tests/CompileServerUtilTests.cpp
        )
    target_link_libraries(EngineCompileServerUtilTests PRIVATE Engine GTest::gtest GTest::gtest_main)
    set_target_properties(EngineCompileServerUtilTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Engine/Tests"
    )
    ge_set_output_tests(EngineCompileServerUtilTests)
    ge_stage_runtime_dependencies(EngineCompileServerUtilTests)
    # Multi-config name baked in so the test resolves the exact staged host path.
    target_compile_definitions(EngineCompileServerUtilTests PRIVATE GE_TEST_CONFIG_NAME="$<CONFIG>")
    if(WIN32 AND ENABLE_SCRIPTING)
        # The spoof-version suites launch GameEngine.CompileServerHost.dll
        # themselves. Stage it beside the exe in the same layout the Editor
        # stages (and CompileServerClient::FindHostPath probes) so the test
        # resolves it exe-anchored; reaching from the CWD into the source
        # tree's Managed/ breaks the moment the build output is relocated.
        # CompileServerHost is defined later at the root scope (ENABLE_SCRIPTING
        # block); add_dependencies resolves it at generate time.
        add_dependencies(EngineCompileServerUtilTests CompileServerHost)
        # The host build's stop script is staged too: a test runs it against
        # hosts launched from the staged copy.
        add_custom_command(TARGET EngineCompileServerUtilTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
                    "${CMAKE_SOURCE_DIR}/Managed/CompileServerHost/bin/$<CONFIG>/net10.0"
                    "$<TARGET_FILE_DIR:EngineCompileServerUtilTests>/Managed/CompileServerHost/bin/$<CONFIG>/net10.0"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_SOURCE_DIR}/Managed/CompileServerHost/StopHostRunningFromOutput.ps1"
                    "$<TARGET_FILE_DIR:EngineCompileServerUtilTests>/StopHostRunningFromOutput.ps1"
            COMMENT "Staging CompileServerHost beside EngineCompileServerUtilTests")
    endif()
    add_test(NAME EngineCompileServerUtilTests COMMAND $<TARGET_FILE:EngineCompileServerUtilTests>)
endif()

# Engine tests: HotReloadPipeline compile-failure reporting, stage lifetime and
# cancellation, on every platform. The suite drives the pipeline with fake
# compilers and the mock loader and swapper, and the swap stage against a
# headless engine with the CLR disabled, so it needs neither GameEngine.Native
# nor the managed test projects. It links
# Engine only: JobSystem (issue #340) and Scripting (issue #356) are already
# spliced into Engine.dll.
if(ENABLE_SCRIPTING AND (GTest_FOUND OR TARGET GTest::gtest))
    add_executable(EngineHotReloadPipelineTests
        Tests/HotReloadPipelineCompileFailureTests.cpp
        Tests/HotReloadPipelineJoinTests.cpp
        Tests/MainThreadSwapCancelTests.cpp)
    target_link_libraries(EngineHotReloadPipelineTests PRIVATE Engine GTest::gtest GTest::gtest_main)
    # Jobs/MockHotReloadDependencies.h and TestTempDir.h.
    target_include_directories(EngineHotReloadPipelineTests PRIVATE ${CMAKE_SOURCE_DIR}/Tests)
    set_target_properties(EngineHotReloadPipelineTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Engine/Tests"
    )
    ge_set_output_tests(EngineHotReloadPipelineTests)
    ge_stage_runtime_dependencies(EngineHotReloadPipelineTests)
    add_test(NAME EngineHotReloadPipelineTests COMMAND $<TARGET_FILE:EngineHotReloadPipelineTests>)
    set_tests_properties(EngineHotReloadPipelineTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "Scripting;HotReload")
endif()

		# Engine tests: minimal compile-server smoke test
		if(GTest_FOUND OR TARGET GTest::gtest)
		    add_executable(EngineCompileServerSmokeTests Tests/CompileServerSmokeTests.cpp)
			    # Tests expect to be able to include Engine public headers such as
			    # Jobs/CompileServerClient.h via the Engine include root.
			    target_include_directories(EngineCompileServerSmokeTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Include)
			    # When GameEngine.Native is available (scripting enabled), link the
			    # full scripting/compile-server stack so the smoke test exercises
			    # the real pipeline on all platforms.  Otherwise fall back to
			    # Engine-only so the test can still build and will skip at runtime
			    # if the compile server is not available.
			    #
			    # Scripting is deliberately NOT linked directly: it is an OBJECT
			    # module already spliced into Engine.dll, and linking it here again
			    # would give the exe a second copy of every module static (issue
			    # #356, same disease as #340). Headers and symbols resolve through
			    # Engine's INTERFACE + export table.
				    if(TARGET GameEngine.Native)
				        target_link_libraries(EngineCompileServerSmokeTests PRIVATE
				            Engine
				            GameEngine.Native
				            GTest::gtest
				            GTest::gtest_main)
				    else()
				        target_link_libraries(EngineCompileServerSmokeTests PRIVATE
				            Engine
				            GTest::gtest
				            GTest::gtest_main)
				    endif()
		    set_target_properties(EngineCompileServerSmokeTests PROPERTIES
		        CXX_STANDARD 20
		        CXX_STANDARD_REQUIRED ON
		        FOLDER "Engine/Tests"
		    )
		    ge_set_output_tests(EngineCompileServerSmokeTests)
		    ge_stage_runtime_dependencies(EngineCompileServerSmokeTests)
		    add_test(NAME EngineCompileServerSmokeTests COMMAND $<TARGET_FILE:EngineCompileServerSmokeTests>)
		endif()

# Engine tests: ScriptManager hot reload with async hot reload disabled, on every
# platform. The suite runs ScriptManager with the CLR disabled, a local pool and
# the CompileServer transport seam, so it needs neither GameEngine.Native nor
# the managed test projects. It links Engine only: JobSystem (issue #340) and
# Scripting (issue #356) are already spliced into Engine.dll.
if(ENABLE_SCRIPTING AND (GTest_FOUND OR TARGET GTest::gtest))
    add_executable(EngineScriptManagerSyncHotReloadTests Tests/ScriptManagerSyncHotReloadTests.cpp)
    target_link_libraries(EngineScriptManagerSyncHotReloadTests PRIVATE Engine GTest::gtest GTest::gtest_main)
    # WatchedFileEvents.h and Jobs/ScopedHotReloadTestHooks.h.
    target_include_directories(EngineScriptManagerSyncHotReloadTests PRIVATE ${CMAKE_SOURCE_DIR}/Tests)
    set_target_properties(EngineScriptManagerSyncHotReloadTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Engine/Tests"
    )
    ge_set_output_tests(EngineScriptManagerSyncHotReloadTests)
    ge_stage_runtime_dependencies(EngineScriptManagerSyncHotReloadTests)
    add_test(NAME EngineScriptManagerSyncHotReloadTests COMMAND $<TARGET_FILE:EngineScriptManagerSyncHotReloadTests>)
    set_tests_properties(EngineScriptManagerSyncHotReloadTests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "Scripting;HotReload")
endif()

			# Engine tests: the render services, one executable per responsibility, so a
			# gate runs the executable that owns the code it touched instead of all of them.
			if(GTest_FOUND OR TARGET GTest::gtest)
            # One member of the render-services test family. Every member shares the
            # isolation tripwire main on Windows (gtest_main elsewhere), the shader path
            # resolver that reads the build directory (EngineTestShaderSetup.cpp) and the
            # shader packages it resolves. GE_RENDERER_REPO_ROOT is dev-only source
            # anchoring for the shader-contract tests (never shipped; staged copies would
            # go stale on shader-only edits). What a member needs beyond that is added
            # after its call. OPT_IN builds the executable without registering it with
            # CTest: no gate runs it, and its tests run by name.
            function(ge_add_render_services_test target_name)
                cmake_parse_arguments(PARSE_ARGV 1 GE_RENDER_TEST "OPT_IN" "" "")
                add_executable(${target_name} ${GE_RENDER_TEST_UNPARSED_ARGUMENTS} Tests/EngineTestShaderSetup.cpp)
                if(WIN32)
                    target_sources(${target_name} PRIVATE Tests/TestIsolationTripwireMain.cpp)
                    target_link_libraries(${target_name} PRIVATE Engine GameEngine.Native GTest::gtest)
                else()
                    target_link_libraries(${target_name} PRIVATE Engine GameEngine.Native GTest::gtest GTest::gtest_main)
                endif()
                target_compile_definitions(${target_name} PRIVATE
                    ENGINE_TEST_BUILD_DIR="${CMAKE_BINARY_DIR}"
                    GE_RENDERER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
                set_target_properties(${target_name} PROPERTIES
                    CXX_STANDARD 20
                    CXX_STANDARD_REQUIRED ON
                    FOLDER "Engine/Tests")
                ge_set_output_tests(${target_name})
                ge_stage_runtime_dependencies(${target_name})
                # Loads "Shaders/<name>.shaderpkg"; the edge resolves at generate time.
                add_dependencies(${target_name} CompileShaderPkgs)
                if(NOT GE_RENDER_TEST_OPT_IN)
                    add_test(NAME ${target_name} COMMAND $<TARGET_FILE:${target_name}>)
                endif()
            endfunction()

            # Materials: registration, binding, pipeline compilation and its caches,
            # warm-up, the variant cook, shader-graph surfaces and reflection.
            ge_add_render_services_test(EngineMaterialTests
                Tests/GeneratedSurfaceLocationTests.cpp
                Tests/ShaderGraphNodesRootTests.cpp
                Tests/ShaderReflectionDiffTests.cpp
                Tests/ModelMaterialBridgeTests.cpp
                Tests/MaterialBinderTests.cpp
                Tests/RenderingRecordCacheThreadedTests.cpp
                Tests/MaterialTests.cpp
                Tests/MaterialVariantCookTests.cpp
                Tests/MaterialBlendTests.cpp
                Tests/MaterialRegistryTests.cpp
                Tests/ParallaxShadingCompositionTests.cpp
                Tests/ProjectMaterialPrewarmServiceTests.cpp
                Tests/ServedGenerationTableTests.cpp
                Tests/MaterialBridgeIntegrationTests.cpp
                Tests/MaterialAssetParsingTests.cpp
                Tests/ShaderCompilationCacheTests.cpp
                Tests/CompileConcurrencyGateTests.cpp
                # MaterialVariantCookTests exercises the offline web cook itself,
                # not a re-implementation of it: the same TU the cook tool links.
                ${CMAKE_SOURCE_DIR}/Tools/MaterialVariantCook/WebWgslCook.cpp
                ${CMAKE_SOURCE_DIR}/Packages/eztree/Native/EZTreeECS/EZTreeRuntimeMaterials.cpp)
            target_include_directories(EngineMaterialTests PRIVATE
                ${CMAKE_SOURCE_DIR}/Tools/MaterialVariantCook
                ${CMAKE_SOURCE_DIR}/Tests # TestTempDir.h
                ${CMAKE_SOURCE_DIR}/Packages/eztree/Native)
            ge_stage_shader_graph_nodes(EngineMaterialTests)

            # Textures: the texture service and bindless indexing, alpha probes, the
            # import and package cooks, KTX2 loading, SVG rasterization, video textures.
            ge_add_render_services_test(EngineTextureTests
                Tests/AwaitedEmissiveTextureTests.cpp
                Tests/EmbeddedTextureReloadTests.cpp
                Tests/AlphaUVFootprintTests.cpp
                Tests/BC7TransparentEndpointRepairTests.cpp
                Tests/TextureAlphaProbeTests.cpp
                Tests/TextureAlphaDecodeProbeTests.cpp
                Tests/TextureMimeTypeTests.cpp
                Tests/TextureServiceClassicIndexingTests.cpp
                Tests/SamplerPresetResolutionTests.cpp
                Tests/TextureServiceSamplerTests.cpp
                Tests/TextureGPUCacheTests.cpp
                Tests/VideoTextureSystemTests.cpp
                Tests/VideoPlayerTests.cpp
                Tests/VideoFrameUploaderTests.cpp
                Tests/TextureAssetKtx2Tests.cpp
                Tests/TextureCookTests.cpp
                Tests/TextureCookWorkersTests.cpp
                Tests/TexturePackageCookTests.cpp
                Tests/TextureAlphaCoverageTests.cpp
                Tests/SvgRasterizerTests.cpp
                Tests/RenderServicesBindlessTests.cpp)
            target_include_directories(EngineTextureTests PRIVATE ${CMAKE_SOURCE_DIR}/Tests) # TestTempDir.h
            # SamplerPresetResolutionTests reads the GE_TS_* sampler indices out of the shader
            # include and compares them with the SamplerPreset enum the bindless array follows.
            target_compile_definitions(EngineTextureTests PRIVATE
                GE_BINDLESS_TEXTURES_GLSL="${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Includes/bindless_textures.glsl")
            # KTX2 fixture encoding (TextureAssetKtx2Tests, TextureAlphaDecodeProbeTests)
            # calls libktx's Basis encoder. Engine links KTX::ktx PRIVATE, so this target
            # links it itself whatever the triplet does.
            #
            # A static libktx additionally needs WHOLE_ARCHIVE: the Engine dylib never
            # references the encoder, so a plain link pulls basis_encode.o into this
            # executable while basisu::basis_compressor stays in the dylib, splitting
            # basisu's one-shot init flag across two images — the encoder then refuses
            # every compress with "basisu_encoder_init() MUST be called".
            if(TARGET KTX::ktx)
                get_target_property(_ge_ktx_type KTX::ktx TYPE)
                if(NOT _ge_ktx_type STREQUAL "STATIC_LIBRARY")
                    # Shared libktx: one copy already, but Engine links it PRIVATE, so this
                    # target's own encoder calls need the import lib directly.
                    target_link_libraries(EngineTextureTests PRIVATE KTX::ktx)
                elseif(CMAKE_CXX_LINK_LIBRARY_USING_WHOLE_ARCHIVE_SUPPORTED)
                    target_link_libraries(EngineTextureTests PRIVATE
                        "$<LINK_LIBRARY:WHOLE_ARCHIVE,KTX::ktx>")
                else()
                    target_link_libraries(EngineTextureTests PRIVATE KTX::ktx)
                    message(WARNING "No WHOLE_ARCHIVE linker feature for this toolchain: "
                                    "the KTX2 encoder fixtures in EngineTextureTests will fail.")
                endif()
                unset(_ge_ktx_type)
            endif()

            # World drawing: ECS extraction into the GPU scene, mesh registration and
            # residency, LOD and HLOD selection, HZB culling, draw-stream building and
            # scatter, sorted transparency, skinning palettes and the render origin.
            ge_add_render_services_test(EngineWorldDrawTests
                Tests/RenderServicesWorldDrawBuilderTests.cpp
                Tests/RenderServicesWorldMaterialTests.cpp
                Tests/MaskVertexAlphaDepthTests.cpp
                Tests/DisabledInHierarchyTests.cpp
                Tests/ExtractionFastPathTests.cpp
                Tests/HlodEvictionStickTests.cpp
                Tests/HlodRuntimeReconcileTests.cpp
                Tests/ExtractionStampAuditTests.cpp
                Tests/MorphTargetDeepPathTests.cpp
                Tests/ViewRegistryTests.cpp
                Tests/MeshGPURegistryTests.cpp
                Tests/MeshGPUResidencyTests.cpp
                Tests/MeshGPUResidencyGateTests.cpp
                Tests/LodCrossfadeLivenessTests.cpp
                Tests/MeshPoolGroupPlanTests.cpp
                Tests/BucketPoolAllocatorTests.cpp
                Tests/GPUMeshTableTests.cpp
                Tests/LodViewBudgetTests.cpp
                Tests/BatchRegistryTests.cpp
                Tests/GPUSceneRemoveInstancesTests.cpp
                Tests/GPUSceneUpdateInstancesTests.cpp
                Tests/GPUScenePrevVisibleResetTests.cpp
                Tests/GPUSceneBufferCapacityTests.cpp
                Tests/BatchScatterComputeTests.cpp
                Tests/PropCoverageFloorTests.cpp
                Tests/HzbBuildComputeTests.cpp
                Tests/HzbSpdParityTests.cpp
                Tests/HzbSpdMinMaxParityTests.cpp
                Tests/HzbCullingComputeTests.cpp
                Tests/GPUDrawStreamBuilderTests.cpp
                Tests/DrawStreamLookupKeyTests.cpp
                Tests/PerFrameWritePoolTests.cpp
                Tests/ModelRenderSetupTests.cpp
                Tests/SceneBuildPumpTests.cpp
                Tests/SceneResolveFrameTests.cpp
                Tests/SceneResolveServiceTests.cpp
                Tests/SortedTransparentCullTests.cpp
                Tests/SortedTransparentRunTests.cpp
                Tests/GPUSortTests.cpp
                Tests/SortedTransparentDeciderTests.cpp
                Tests/SkinPaletteAtlasTests.cpp
                Tests/SkinPaletteRefusalTests.cpp
                Tests/SkeletonStoreRuntimeTests.cpp
                Tests/SkeletonRuntimeOwnershipTests.cpp
                Tests/ModelEntityFactoryHelperNodeTests.cpp
                Tests/ModelEntityFactoryPlacementTests.cpp
                Tests/PrimitiveGeneratorTests.cpp
                Tests/MeshReloadBoundsRefreshTests.cpp
                Tests/RenderOriginTests.cpp
                Tests/RenderOriginDecodeComputeTests.cpp
                Tests/LodCrossfadeShaderContractTests.cpp)

            # Opt-in render checks no gate runs: the Slang lane's parity gate, which needs an
            # external slangc (GE_SLANGC), and the extraction fast path's manual
            # microbenchmark. Build EngineRenderOptInTests and run a test by name.
            ge_add_render_services_test(EngineRenderOptInTests OPT_IN
                Tests/HzbSlangParityTests.cpp
                Tests/ExtractionFastPathMicrobenchmark.cpp)

            # Shadows: point and cascade planning, caster invalidation, the shadow cache,
            # sampling and filtering, and the ray-traced shadow mask.
            ge_add_render_services_test(EngineShadowTests
                Tests/PointShadowFaceCullTests.cpp
                Tests/PointShadowAtlasPlannerTests.cpp
                Tests/PointShadowLodSelectionTests.cpp
                Tests/CascadeFitStabilityTests.cpp
                Tests/CascadeReceiverFitTests.cpp
                Tests/CascadeMotionSchedulerTests.cpp
                Tests/CascadeShadowCacheTests.cpp
                Tests/SkinnedShadowInvalidationTests.cpp
                Tests/VertexModifierContentInvalidationTests.cpp
                Tests/ShadowSamplingFitTests.cpp
                Tests/ShadowSamplingShaderContractTests.cpp
                Tests/ShadowPcssDecisionTests.cpp
                Tests/ShadowMinMaxPyramidTests.cpp
                Tests/ShadowFilterComputeTests.cpp
                Tests/RTShadowMaskShaderContractTests.cpp
                Tests/RTShadowDenoiseShaderContractTests.cpp
                Tests/ShadowDitherBasisTests.cpp
                Tests/ShadowDistanceFitTests.cpp
                Tests/ShadowReceiverReduceComputeTests.cpp
                Tests/WorldTexelCascadeTests.cpp)
            # Rendering's CompileShaders owns compilation of test shader sources.
            # Stage its output beside the executable, independent of the build tree layout.
            add_custom_target(ShadowFilterTestShader
                COMMAND ${CMAKE_COMMAND} -E make_directory
                    "$<TARGET_FILE_DIR:EngineShadowTests>/TestData/ShadowFilter"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_BINARY_DIR}/Shaders/shadow_filter_test.comp.spv"
                    "$<TARGET_FILE_DIR:EngineShadowTests>/TestData/ShadowFilter/shadow_filter_test.spv"
                VERBATIM)
            add_dependencies(ShadowFilterTestShader CompileShaders)
            add_dependencies(EngineShadowTests ShadowFilterTestShader)

            # Post-processing: exposure, the post-process volume blend, bloom, lens flare,
            # sun glare, temporal anti-aliasing jitter, GTAO and screen-space reflections.
            ge_add_render_services_test(EnginePostProcessTests
                Tests/TemporalAAJitterTests.cpp
                Tests/SssrRefineBudgetTests.cpp
                Tests/SssrHistoryPolicyTests.cpp
                Tests/GtaoPackingContractTests.cpp
                Tests/SssrCompositeWeightTests.cpp
                Tests/SssrBlueNoiseFrameMaskTests.cpp
                Tests/SssrPrefilterTileDispatchTests.cpp
                Tests/SssrPrefilterFootprintTests.cpp
                Tests/SunGlareRadianceTests.cpp
                Tests/SssrDispatchArgsBoundsTests.cpp
                Tests/ExposureTests.cpp
                Tests/AutoExposureComputeTests.cpp
                Tests/PostProcessBlendCoverageTests.cpp
                Tests/BloomExposureUnitsContractTests.cpp
                Tests/LensFlareImporterTests.cpp)

            # The render services themselves and the lighting they carry:
            # the feature registry, device recovery, views and cameras, project settings,
            # frame clocks, lights, reflection probes and IBL, DDGI, sky and sun, ocean.
            ge_add_render_services_test(EngineRenderServicesTests
                Tests/RenderServicesFeatureRegistryConcurrencyTests.cpp
                Tests/LodProjectSettingsTests.cpp
                Tests/AntiAliasingProjectSettingsTests.cpp
                Tests/UIScaleProjectSettingsTests.cpp
                Tests/RenderServicesDeviceRecoveryTests.cpp
                Tests/ViewLightBufferWriterTests.cpp
                Tests/DeviceLostEcsRecoveryTests.cpp
                Tests/CameraProjectionTests.cpp
                Tests/HalfFloatTests.cpp
                Tests/RecomputeElisionGateTests.cpp
                Tests/DDGISceneServiceTests.cpp
                Tests/DDGIUberMaterialBakeTests.cpp
                Tests/DDGIGlossyAtlasLayoutTests.cpp
                Tests/DDGIReflectionParallaxTests.cpp
                Tests/DDGIMaterialMapContractTests.cpp
                Tests/DDGICameraIdleGateTests.cpp
                Tests/DDGISolveBudgetTests.cpp
                Tests/DDGIProbeWindowTests.cpp
                Tests/DDGIDepthNormalComputeTests.cpp
                Tests/DDGIBlendComputeTests.cpp
                Tests/DDGIParallaxComputeTests.cpp
                Tests/DDGIMomentPairComputeTests.cpp
                Tests/DDGIClassifyComputeTests.cpp
                Tests/DDGIDistanceBoundComputeTests.cpp
                Tests/DDGIRelocationComputeTests.cpp
                Tests/DDGIResolveBlurComputeTests.cpp
                Tests/OceanCascadeLayoutTests.cpp
                Tests/OceanSeabedDepthShaderTests.cpp
                Tests/OceanShallowClarityShaderTests.cpp
                Tests/OceanSurfaceComposeTests.cpp
                Tests/OceanTemporalHistoryTests.cpp
                Tests/OceanWaterInViewTests.cpp
                Tests/OceanShaderPackageTests.cpp
                Tests/OceanSplineRasterTests.cpp
                Tests/LightPhotometryTests.cpp
                Tests/AmbientLightSystemTests.cpp
                Tests/ReflectionProbeSystemTests.cpp
                Tests/SceneReflectionProbeRecaptureTests.cpp
                Tests/IblShaderContractTests.cpp
                Tests/GlassTransmissionShaderContractTests.cpp
                Tests/SkyGroundBounceContractTests.cpp
                Tests/SkySunColorTests.cpp
                Tests/SkySunAnchorTests.cpp
                Tests/SkySunIlluminanceTests.cpp
                Tests/SkySunPathTests.cpp
                Tests/SkySunDriveTests.cpp
                Tests/SkyMoonlightTests.cpp
                Tests/SkyGroundFloorParityTests.cpp
                Tests/SkyDiffuseConvolveChromaticityTests.cpp
                Tests/SkyTransmittanceLutParityTests.cpp
                Tests/ParticleCoverageTests.cpp
                Tests/ParticleNearFadeTests.cpp
                Tests/SunDiscRadianceTests.cpp
                Tests/SunDiscSizeTests.cpp
                Tests/IblEnvDataRingTests.cpp
                Tests/LightContributionSortTests.cpp
                Tests/AnimationClockPersistenceTests.cpp
                Tests/ReadbackSourceSpaceTests.cpp
                Tests/ShaderAnimationClockContractTests.cpp)
            # OceanCascadeLayoutTest initializes the camera-snapped ocean sims, which
            # compile their GLSL at runtime from <install assets root>/Shaders/Ocean
            # (OceanShaderDirectory) with "Includes/..." alongside; stage both next to
            # the test exe so the sims initialize instead of declining.
            add_custom_command(TARGET EngineRenderServicesTests POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
                        "${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Ocean"
                        "$<TARGET_FILE_DIR:EngineRenderServicesTests>/Assets/Shaders/Ocean"
                COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
                        "${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Includes"
                        "$<TARGET_FILE_DIR:EngineRenderServicesTests>/Assets/Shaders/Includes"
                COMMENT "Staging ocean sim shaders for EngineRenderServicesTests")
            set(_ge_sss_test_spv "${CMAKE_CURRENT_BINARY_DIR}/Tests/Shaders/screen_space_shadow_test.spv")
            add_custom_command(OUTPUT "${_ge_sss_test_spv}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/Tests/Shaders"
                COMMAND "${GLSLC_EXECUTABLE}" --target-env=vulkan1.2 -DGE_SSS_TEST_COVERAGE=1
                    -I "${CMAKE_CURRENT_SOURCE_DIR}/Modules/Rendering/Shaders"
                    "${CMAKE_CURRENT_SOURCE_DIR}/Modules/Rendering/Shaders/ScreenSpaceShadows/screen_space_shadow.comp"
                    -o "${_ge_sss_test_spv}"
                DEPENDS Modules/Rendering/Shaders/ScreenSpaceShadows/screen_space_shadow.comp VERBATIM)
            set(_ge_sss_prepare_test_spv "${CMAKE_CURRENT_BINARY_DIR}/Tests/Shaders/screen_space_shadow_prepare_test.spv")
            add_custom_command(OUTPUT "${_ge_sss_prepare_test_spv}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/Tests/Shaders"
                COMMAND "${GLSLC_EXECUTABLE}" --target-env=vulkan1.2
                    -I "${CMAKE_CURRENT_SOURCE_DIR}/Modules/Rendering/Shaders"
                    "${CMAKE_CURRENT_SOURCE_DIR}/Modules/Rendering/Shaders/ScreenSpaceShadows/screen_space_shadow_prepare.comp"
                    -o "${_ge_sss_prepare_test_spv}"
                DEPENDS Modules/Rendering/Shaders/ScreenSpaceShadows/screen_space_shadow_prepare.comp VERBATIM)
            set(_ge_sss_resolve_test_spv "${CMAKE_CURRENT_BINARY_DIR}/Tests/Shaders/screen_space_shadow_resolve_test.spv")
            add_custom_command(OUTPUT "${_ge_sss_resolve_test_spv}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/Tests/Shaders"
                COMMAND "${GLSLC_EXECUTABLE}" --target-env=vulkan1.2
                    -I "${CMAKE_CURRENT_SOURCE_DIR}/Modules/Rendering/Shaders"
                    "${CMAKE_CURRENT_SOURCE_DIR}/Modules/Rendering/Shaders/ScreenSpaceShadows/screen_space_shadow_resolve.comp"
                    -o "${_ge_sss_resolve_test_spv}"
                DEPENDS Modules/Rendering/Shaders/ScreenSpaceShadows/screen_space_shadow_resolve.comp VERBATIM)
            set(_ge_sss_match_test_spv "${CMAKE_CURRENT_BINARY_DIR}/Tests/Shaders/screen_space_shadow_match_test.spv")
            add_custom_command(OUTPUT "${_ge_sss_match_test_spv}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/Tests/Shaders"
                COMMAND "${GLSLC_EXECUTABLE}" --target-env=vulkan1.2
                    -I "${CMAKE_CURRENT_SOURCE_DIR}/Modules/Rendering/Shaders"
                    "${CMAKE_CURRENT_SOURCE_DIR}/Modules/Rendering/Shaders/ScreenSpaceShadows/screen_space_shadow_match.comp"
                    -o "${_ge_sss_match_test_spv}"
                DEPENDS Modules/Rendering/Shaders/ScreenSpaceShadows/screen_space_shadow_match.comp
                    Modules/Rendering/Shaders/Includes/screen_position.glsl VERBATIM)
            set(_ge_shadow_plane_test_spv "${CMAKE_CURRENT_BINARY_DIR}/Tests/Shaders/shadow_receiver_plane_test.spv")
            add_custom_command(OUTPUT "${_ge_shadow_plane_test_spv}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/Tests/Shaders"
                COMMAND "${GLSLC_EXECUTABLE}" --target-env=vulkan1.2
                    "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Shaders/ShadowReceiverPlane.comp"
                    -o "${_ge_shadow_plane_test_spv}"
                DEPENDS Tests/Shaders/ShadowReceiverPlane.comp
                    Modules/Rendering/Shaders/Includes/shadow_receiver_plane.glsl VERBATIM)
            add_custom_target(ScreenSpaceShadowTestShader DEPENDS
                "${_ge_sss_test_spv}" "${_ge_sss_prepare_test_spv}" "${_ge_sss_resolve_test_spv}"
                "${_ge_sss_match_test_spv}" "${_ge_shadow_plane_test_spv}")
            add_executable(EngineScreenSpaceShadowTests
                Tests/ScreenSpaceShadowComputeTests.cpp
                Tests/ScreenSpaceShadowPassesTests.cpp
                Tests/EngineTestShaderSetup.cpp)
            target_link_libraries(EngineScreenSpaceShadowTests PRIVATE Engine GTest::gtest GTest::gtest_main)
            target_compile_definitions(EngineScreenSpaceShadowTests PRIVATE
                ENGINE_TEST_BUILD_DIR="${CMAKE_BINARY_DIR}")
            set_target_properties(EngineScreenSpaceShadowTests PROPERTIES
                CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON FOLDER "Engine/Tests")
            add_dependencies(EngineScreenSpaceShadowTests ScreenSpaceShadowTestShader CompileShaderPkgs)
            ge_set_output_tests(EngineScreenSpaceShadowTests)
            ge_stage_runtime_dependencies(EngineScreenSpaceShadowTests)
            add_test(NAME EngineScreenSpaceShadowTests COMMAND $<TARGET_FILE:EngineScreenSpaceShadowTests>)

			endif()

			if(GTest_FOUND OR TARGET GTest::gtest)
			    # Keep allocation injection isolated. Compile the real retirement
			    # owners here so the scoped allocation faults reach their STL
			    # allocations through this executable's hook in every configuration,
			    # including Windows Release, where Engine.dll carries no hook.
			    add_executable(MeshRetirementTests
			        Tests/MeshRetirementTests.cpp
			        Source/Engine/Rendering/MeshGPURegistry.cpp
			        Source/Engine/Rendering/MeshGPUResidency.cpp
			        Source/Engine/Rendering/BucketPoolAllocator.cpp
			        Modules/Rendering/Source/Core/GPUScene.cpp)
			    target_link_libraries(MeshRetirementTests PRIVATE Engine GTest::gtest GTest::gtest_main)
			    if(MSVC)
			        target_compile_options(MeshRetirementTests PRIVATE /bigobj)
			    endif()
			    set_target_properties(MeshRetirementTests PROPERTIES
			        CXX_STANDARD 20
			        CXX_STANDARD_REQUIRED ON
			        FOLDER "Engine/Tests")
			    ge_set_output_tests(MeshRetirementTests)
			    ge_stage_runtime_dependencies(MeshRetirementTests)
			    add_test(NAME MeshRetirementTests COMMAND $<TARGET_FILE:MeshRetirementTests>)
			endif()

			if(GTest_FOUND OR TARGET GTest::gtest)
			    # Small cross-platform post-process contract suite. Keep this separate
			    # from EnginePostProcessTests so shader/control tests do not depend on
			    # the render-services executables' platform-specific tripwire main.
			    add_executable(BloomRenderingTests Tests/BloomRenderingTests.cpp)
			    target_link_libraries(BloomRenderingTests PRIVATE Engine GTest::gtest GTest::gtest_main)
			    target_compile_definitions(BloomRenderingTests PRIVATE
			        RENDERING_SHADER_OUTPUT_DIR="${CMAKE_BINARY_DIR}/Shaders")
			    add_dependencies(BloomRenderingTests CompileShaders)
			    set_target_properties(BloomRenderingTests PROPERTIES
			        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON FOLDER "Engine/Tests")
			    ge_set_output_tests(BloomRenderingTests)
			    ge_stage_runtime_dependencies(BloomRenderingTests)
			    add_test(NAME BloomRenderingTests COMMAND $<TARGET_FILE:BloomRenderingTests>)

			    add_executable(PostProcessDofControlTests
			            Tests/PostProcessBlendCoverageTests.cpp)
			    target_link_libraries(PostProcessDofControlTests PRIVATE Engine GTest::gtest GTest::gtest_main)
			    target_compile_definitions(PostProcessDofControlTests PRIVATE
			        GE_RENDERER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
			    set_target_properties(PostProcessDofControlTests PROPERTIES
			        CXX_STANDARD 20
			        CXX_STANDARD_REQUIRED ON
			        FOLDER "Engine/Tests"
			    )
			    ge_set_output_tests(PostProcessDofControlTests)
			    ge_stage_runtime_dependencies(PostProcessDofControlTests)
			    add_test(NAME PostProcessDofControlTests COMMAND $<TARGET_FILE:PostProcessDofControlTests>)
			endif()

			if(GTest_FOUND OR TARGET GTest::gtest)
			    # Input routing policy: needs Engine (WindowInputRouter) plus the UI
			    # module's headless-device harness to build a real UIManager.
			    add_executable(WindowInputRoutingTests Tests/WindowInputRoutingTests.cpp)
			    # GLFW directly as well: the focus-at-bind row runs GLFW's null
			    # platform through the same glfw3 DLL Engine loads.
			    find_package(glfw3 CONFIG REQUIRED)
			    target_link_libraries(WindowInputRoutingTests PRIVATE Engine glfw GTest::gtest GTest::gtest_main)
			    target_include_directories(WindowInputRoutingTests PRIVATE
			        ${CMAKE_SOURCE_DIR}/Engine/Modules/UI/Tests)
			    set_target_properties(WindowInputRoutingTests PROPERTIES
			        CXX_STANDARD 20
			        CXX_STANDARD_REQUIRED ON
			        FOLDER "Engine/Tests"
			    )
			    ge_set_output_tests(WindowInputRoutingTests)
			    ge_stage_runtime_dependencies(WindowInputRoutingTests)
			    add_test(NAME WindowInputRoutingTests COMMAND $<TARGET_FILE:WindowInputRoutingTests>)
			endif()

			# ENABLE_CPU_PROFILING gates the target, not just its assertions: with
			# the switch OFF the header compiles the stub, whose IsEnabled() and
			# IsMainThread() return false and whose CopyFrameSamples() clears its
			# out-parameter, so every assertion here fails by construction. A suite
			# that is red whenever a supported configuration is built is a false
			# instrument, so it is not built in that configuration at all.
			if((GTest_FOUND OR TARGET GTest::gtest) AND ENABLE_CPU_PROFILING)
			    # CpuProfiler singleton reach across the Engine.dll boundary. Links
			    # Engine and nothing else on purpose: this executable is its own
			    # linked image, so a scope recorded by Engine.dll code (the probe is
			    # SceneTlas::TraverseRay, whose objects are spliced into Engine.dll)
			    # is only observable through Get() here if both sides resolve one
			    # instance. Linking the Scene OBJECT lib directly would put the probe
			    # scope in THIS image and the suite would pass either way.
			    add_executable(CpuProfilerModuleBoundaryTests Tests/CpuProfilerModuleBoundaryTests.cpp)
			    target_link_libraries(CpuProfilerModuleBoundaryTests PRIVATE Engine GTest::gtest GTest::gtest_main)
			    set_target_properties(CpuProfilerModuleBoundaryTests PROPERTIES
			        CXX_STANDARD 20
			        CXX_STANDARD_REQUIRED ON
			        FOLDER "Engine/Tests"
			    )
			    ge_set_output_tests(CpuProfilerModuleBoundaryTests)
			    ge_stage_runtime_dependencies(CpuProfilerModuleBoundaryTests)
			    add_test(NAME CpuProfilerModuleBoundaryTests COMMAND $<TARGET_FILE:CpuProfilerModuleBoundaryTests>)
			endif()

			if(GTest_FOUND OR TARGET GTest::gtest)
			    # Host-vs-Engine.dll layout agreement. The two probe translation units
			    # compile the same public headers under the two NDEBUG states a host and
			    # the engine can be built in — the Editor's DebugFast has NDEBUG undefined,
			    # a packaged game's Release has it defined — so a public header that moves
			    # a member between them fails here instead of in a shipped game. Links
			    # Engine and nothing else, like the other boundary suites: the sizes it
			    # compares must be the ones a host's own compile produces. The suite also
			    # sweeps every public header in the source tree for guards on macros a
			    # host's configuration decides, which is why it is told where the tree is.
			    add_executable(EngineHostAbiLayoutTests
			        Tests/EngineHostAbiLayoutTests.cpp
			        Tests/EngineHostAbiLayoutProbeWithNDebug.cpp
			        Tests/EngineHostAbiLayoutProbeWithoutNDebug.cpp)
			    target_link_libraries(EngineHostAbiLayoutTests PRIVATE Engine GTest::gtest GTest::gtest_main)
			    target_compile_definitions(EngineHostAbiLayoutTests PRIVATE GE_ENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
			    set_target_properties(EngineHostAbiLayoutTests PROPERTIES
			        CXX_STANDARD 20
			        CXX_STANDARD_REQUIRED ON
			        UNITY_BUILD OFF
			        FOLDER "Engine/Tests"
			    )
			    ge_set_output_tests(EngineHostAbiLayoutTests)
			    ge_stage_runtime_dependencies(EngineHostAbiLayoutTests)
			    add_test(NAME EngineHostAbiLayoutTests COMMAND $<TARGET_FILE:EngineHostAbiLayoutTests>)
			endif()

			if(GTest_FOUND OR TARGET GTest::gtest)
			    # One registrar per engine component: a headless EngineCore::Initialize
			    # must re-register nothing, and every ComponentRegistry entry must be
			    # spelled by the canonical type name its type id hashes from. Links
			    # Engine because the sweep runs over what Engine.dll's static
			    # initializers registered; a suite without Engine would sweep an empty
			    # registry and pass having examined nothing.
			    add_executable(EngineComponentRegistrationTests Tests/EngineComponentRegistrationTests.cpp
			        ${CMAKE_SOURCE_DIR}/Tests/EngineShutdownTestEnvironment.cpp)
			    target_link_libraries(EngineComponentRegistrationTests PRIVATE Engine GTest::gtest GTest::gtest_main)
			    target_include_directories(EngineComponentRegistrationTests PRIVATE ${CMAKE_SOURCE_DIR}/Tests) # TestTempDir.h
			    set_target_properties(EngineComponentRegistrationTests PROPERTIES
			        CXX_STANDARD 20
			        CXX_STANDARD_REQUIRED ON
			        FOLDER "Engine/Tests"
			    )
			    ge_set_output_tests(EngineComponentRegistrationTests)
			    ge_stage_runtime_dependencies(EngineComponentRegistrationTests)
			    add_test(NAME EngineComponentRegistrationTests COMMAND $<TARGET_FILE:EngineComponentRegistrationTests>)
			endif()

			if(GTest_FOUND OR TARGET GTest::gtest)
			    # Which EngineCore GetInstance() returns once an owned engine is gone.
			    # Its own binary: the test controls when the process's default
			    # instance is created, which any earlier test in a shared binary would
			    # do first.
			    add_executable(EngineCoreInstanceTests Tests/EngineCoreInstanceTests.cpp)
			    target_link_libraries(EngineCoreInstanceTests PRIVATE Engine GTest::gtest GTest::gtest_main)
			    set_target_properties(EngineCoreInstanceTests PROPERTIES
			        CXX_STANDARD 20
			        CXX_STANDARD_REQUIRED ON
			        FOLDER "Engine/Tests"
			    )
			    ge_set_output_tests(EngineCoreInstanceTests)
			    ge_stage_runtime_dependencies(EngineCoreInstanceTests)
			    add_test(NAME EngineCoreInstanceTests COMMAND $<TARGET_FILE:EngineCoreInstanceTests>)
			endif()

			if(GTest_FOUND OR TARGET GTest::gtest)
			    # Its own binary on purpose: BindlessResourceManager reports its capacity once
			    # per process, so a test of that has to be the process's first manager build.
			    # Every umbrella suite builds one through TextureService before its own tests
			    # start.
			    add_executable(BindlessResourceManagerDiagnosticsTests Tests/BindlessResourceManagerDiagnosticsTests.cpp)
			    target_link_libraries(BindlessResourceManagerDiagnosticsTests PRIVATE Engine GTest::gtest GTest::gtest_main)
			    set_target_properties(BindlessResourceManagerDiagnosticsTests PROPERTIES
			        CXX_STANDARD 20
			        CXX_STANDARD_REQUIRED ON
			        FOLDER "Engine/Tests"
			    )
			    ge_set_output_tests(BindlessResourceManagerDiagnosticsTests)
			    ge_stage_runtime_dependencies(BindlessResourceManagerDiagnosticsTests)
			    add_test(NAME BindlessResourceManagerDiagnosticsTests COMMAND $<TARGET_FILE:BindlessResourceManagerDiagnosticsTests>)
			endif()

			if(GTest_FOUND OR TARGET GTest::gtest)
			    # What the shared log-capture guard hands back to the process. Its own binary so
			    # the starting state is a fresh one rather than whatever ran before it.
			    add_executable(EngineLogCaptureTests Tests/EngineLogCaptureTests.cpp)
			    target_link_libraries(EngineLogCaptureTests PRIVATE Engine GTest::gtest GTest::gtest_main)
			    set_target_properties(EngineLogCaptureTests PROPERTIES
			        CXX_STANDARD 20
			        CXX_STANDARD_REQUIRED ON
			        FOLDER "Engine/Tests"
			    )
			    ge_set_output_tests(EngineLogCaptureTests)
			    ge_stage_runtime_dependencies(EngineLogCaptureTests)
			    add_test(NAME EngineLogCaptureTests COMMAND $<TARGET_FILE:EngineLogCaptureTests>)
			endif()

			if(GTest_FOUND OR TARGET GTest::gtest)
			    add_executable(EngineAssetLoadStarvationTests Tests/AssetLoadPoolStarvationTests.cpp)
			    target_link_libraries(EngineAssetLoadStarvationTests PRIVATE Engine GTest::gtest GTest::gtest_main)
			    set_target_properties(EngineAssetLoadStarvationTests PROPERTIES
			        CXX_STANDARD 20
			        CXX_STANDARD_REQUIRED ON
			        FOLDER "Engine/Tests"
			    )
			    ge_set_output_tests(EngineAssetLoadStarvationTests)
			    ge_stage_runtime_dependencies(EngineAssetLoadStarvationTests)
			    add_test(NAME EngineAssetLoadStarvationTests COMMAND $<TARGET_FILE:EngineAssetLoadStarvationTests>)
			endif()

			if(GTest_FOUND OR TARGET GTest::gtest)
			    add_executable(EngineSourceRebindAsyncTests Tests/SourceRebindAsyncTests.cpp)
			    target_link_libraries(EngineSourceRebindAsyncTests PRIVATE Engine GTest::gtest GTest::gtest_main)
			    set_target_properties(EngineSourceRebindAsyncTests PROPERTIES
			        CXX_STANDARD 20
			        CXX_STANDARD_REQUIRED ON
			        FOLDER "Engine/Tests"
			    )
			    ge_set_output_tests(EngineSourceRebindAsyncTests)
			    ge_stage_runtime_dependencies(EngineSourceRebindAsyncTests)
			    add_test(NAME EngineSourceRebindAsyncTests COMMAND $<TARGET_FILE:EngineSourceRebindAsyncTests>)
			endif()

			if(GTest_FOUND OR TARGET GTest::gtest)
			    add_executable(EngineExpectedAssetWriteTests Tests/ExpectedAssetWriteTests.cpp)
			    target_link_libraries(EngineExpectedAssetWriteTests PRIVATE Engine GTest::gtest GTest::gtest_main)
			    target_include_directories(EngineExpectedAssetWriteTests PRIVATE ${CMAKE_SOURCE_DIR}/Tests) # WatchedFileEvents.h
			    set_target_properties(EngineExpectedAssetWriteTests PROPERTIES
			        CXX_STANDARD 20
			        CXX_STANDARD_REQUIRED ON
			        FOLDER "Engine/Tests"
			    )
			    ge_set_output_tests(EngineExpectedAssetWriteTests)
			    ge_stage_runtime_dependencies(EngineExpectedAssetWriteTests)
			    add_test(NAME EngineExpectedAssetWriteTests COMMAND $<TARGET_FILE:EngineExpectedAssetWriteTests>)
			endif()

			if(GTest_FOUND OR TARGET GTest::gtest)
			    add_executable(EngineExpectedWriteLedgerTests Tests/ExpectedWriteLedgerTests.cpp)
			    target_link_libraries(EngineExpectedWriteLedgerTests PRIVATE Engine GTest::gtest GTest::gtest_main)
			    set_target_properties(EngineExpectedWriteLedgerTests PROPERTIES
			        CXX_STANDARD 20
			        CXX_STANDARD_REQUIRED ON
			        FOLDER "Engine/Tests"
			    )
			    ge_set_output_tests(EngineExpectedWriteLedgerTests)
			    ge_stage_runtime_dependencies(EngineExpectedWriteLedgerTests)
			    add_test(NAME EngineExpectedWriteLedgerTests COMMAND $<TARGET_FILE:EngineExpectedWriteLedgerTests>)
			endif()

			if(GTest_FOUND OR TARGET GTest::gtest)
			    add_executable(EngineAssetRenamePathTests Tests/AssetRenamePathTests.cpp)
			    target_link_libraries(EngineAssetRenamePathTests PRIVATE Engine GTest::gtest GTest::gtest_main)
			    set_target_properties(EngineAssetRenamePathTests PROPERTIES
			        CXX_STANDARD 20
			        CXX_STANDARD_REQUIRED ON
			        FOLDER "Engine/Tests"
			    )
			    ge_set_output_tests(EngineAssetRenamePathTests)
			    ge_stage_runtime_dependencies(EngineAssetRenamePathTests)
			    add_test(NAME EngineAssetRenamePathTests COMMAND $<TARGET_FILE:EngineAssetRenamePathTests>)
			endif()

			if(GTest_FOUND OR TARGET GTest::gtest)
			    # Compiles the Packages/eztree native module sources directly —
			    # the same TU set the package's runtime DLL builds from — so generator
			    # determinism and the plugin's late-registration wave placement stay
			    # covered without the engine containing any EZTree runtime code.
			    set(_GE_EZTREE_PKG_NATIVE "${CMAKE_SOURCE_DIR}/Packages/eztree/Native")
			    add_executable(EZTreeNativeTests Tests/EZTreeNativeTests.cpp
			        "${_GE_EZTREE_PKG_NATIVE}/EZTree/EZTreeGenerator.cpp"
			        "${_GE_EZTREE_PKG_NATIVE}/EZTree/EZTreeOptions.cpp"
			        "${_GE_EZTREE_PKG_NATIVE}/EZTree/EZTreePresets.cpp"
			        "${_GE_EZTREE_PKG_NATIVE}/EZTree/EZTreeRng.cpp"
			        "${_GE_EZTREE_PKG_NATIVE}/EZTreeECS/EZTreePlugin.cpp"
			        "${_GE_EZTREE_PKG_NATIVE}/EZTreeECS/EZTreeRuntimeMaterials.cpp"
			        "${_GE_EZTREE_PKG_NATIVE}/EZTreeECS/EZTreeService.cpp"
			        "${_GE_EZTREE_PKG_NATIVE}/EZTreeECS/EZTreeSceneSchema.cpp"
			        "${_GE_EZTREE_PKG_NATIVE}/EZTreeECS/Systems/EZTreeExtractionSystem.cpp"
			        "${_GE_EZTREE_PKG_NATIVE}/EZTreeECS/Systems/RegisterEZTreeSystems.cpp"
			    )
			    target_include_directories(EZTreeNativeTests PRIVATE "${_GE_EZTREE_PKG_NATIVE}")
			    target_link_libraries(EZTreeNativeTests PRIVATE Engine nlohmann_json::nlohmann_json GTest::gtest GTest::gtest_main)
			    set_target_properties(EZTreeNativeTests PROPERTIES
			        CXX_STANDARD 20
			        CXX_STANDARD_REQUIRED ON
			        FOLDER "Engine/Tests"
			    )
			    ge_set_output_tests(EZTreeNativeTests)
			    ge_stage_runtime_dependencies(EZTreeNativeTests)
			    # LoadPreset resolves exe-anchored (Assets/EZTree/Presets next to the
			    # running binary); stage the vcpkg preset JSONs beside the test exe.
			    add_custom_command(TARGET EZTreeNativeTests POST_BUILD
			        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
			                "${GE_EZTREE_PRESETS_SOURCE_DIR}"
			                "$<TARGET_FILE_DIR:EZTreeNativeTests>/Assets/EZTree/Presets"
			        COMMENT "Staging EZ-Tree presets next to EZTreeNativeTests")
			    # The package's committed manifest is this suite's fixture for the
			    # texture-classification test. TestData/, like every other package
			    # fixture, so the test reads a staged copy and never the source tree.
			    add_custom_command(TARGET EZTreeNativeTests POST_BUILD
			        COMMAND ${CMAKE_COMMAND} -E make_directory
			                "$<TARGET_FILE_DIR:EZTreeNativeTests>/TestData/Packages/eztree/Assets"
			        COMMAND ${CMAKE_COMMAND} -E copy_if_different
			                "${CMAKE_SOURCE_DIR}/Packages/eztree/Assets/.assetmanifest"
			                "$<TARGET_FILE_DIR:EZTreeNativeTests>/TestData/Packages/eztree/Assets/.assetmanifest"
			        COMMENT "Staging the eztree package manifest next to EZTreeNativeTests")
			    add_test(NAME EZTreeNativeTests COMMAND $<TARGET_FILE:EZTreeNativeTests>)
			endif()

			# Engine tests: slice-4 RenderGraph pipeline Declare spine + node declaration.
			if(GTest_FOUND OR TARGET GTest::gtest)
			    # The fixture gates Vulkan code on Rendering's propagated
			    # RENDERING_HAS_VULKAN, after backend discovery later in root order.
			    add_executable(CpuTextureSourceTests Tests/CpuTextureSourceTests.cpp)
			    target_include_directories(CpuTextureSourceTests PRIVATE
			        "${CMAKE_CURRENT_SOURCE_DIR}/Source/Engine/Rendering"
			        "${CMAKE_CURRENT_SOURCE_DIR}/Modules/Rendering")
			    target_link_libraries(CpuTextureSourceTests PRIVATE Engine GTest::gtest GTest::gtest_main)
			    set_target_properties(CpuTextureSourceTests PROPERTIES
			        CXX_STANDARD 20
			        CXX_STANDARD_REQUIRED ON
			        FOLDER "Engine/Tests"
			    )
			    ge_set_output_tests(CpuTextureSourceTests)
			    ge_stage_runtime_dependencies(CpuTextureSourceTests)
			    add_test(NAME CpuTextureSourceTests COMMAND $<TARGET_FILE:CpuTextureSourceTests>)
			    add_executable(RenderPipelineRegistrationTests Tests/RenderPipelineRegistrationTests.cpp
			        ${CMAKE_SOURCE_DIR}/Tests/EngineShutdownTestEnvironment.cpp)
			    target_link_libraries(RenderPipelineRegistrationTests PRIVATE Engine GTest::gtest GTest::gtest_main)
			    target_compile_definitions(RenderPipelineRegistrationTests PRIVATE ENGINE_TEST_BUILD_DIR="${CMAKE_BINARY_DIR}")
			    set_target_properties(RenderPipelineRegistrationTests PROPERTIES
			        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON FOLDER "Engine/Tests")
			    ge_set_output_tests(RenderPipelineRegistrationTests)
			    ge_stage_runtime_dependencies(RenderPipelineRegistrationTests)
			    # Loads "Shaders/<name>.shaderpkg"; the edge resolves at generate time.
			    add_dependencies(RenderPipelineRegistrationTests CompileShaderPkgs)
			    add_test(NAME RenderPipelineRegistrationTests COMMAND $<TARGET_FILE:RenderPipelineRegistrationTests>)
			    set_tests_properties(RenderPipelineRegistrationTests PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

			    add_executable(PixelPerfectCameraTests Tests/PixelPerfectCameraTests.cpp)
			    target_link_libraries(PixelPerfectCameraTests PRIVATE Engine GTest::gtest GTest::gtest_main)
			    set_target_properties(PixelPerfectCameraTests PROPERTIES
			        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON FOLDER "Engine/Tests")
			    ge_set_output_tests(PixelPerfectCameraTests)
			    ge_stage_runtime_dependencies(PixelPerfectCameraTests)
			    add_test(NAME PixelPerfectCameraTests COMMAND $<TARGET_FILE:PixelPerfectCameraTests>)
			    set_tests_properties(PixelPerfectCameraTests PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

			    add_executable(RenderPipelineDeclareTests
			        Tests/RenderPipelineDeclareTests.cpp
			        # Which depth the auto-exposure meter reads, and the ocean step's post-ocean resolve.
			        Tests/MeteringDepthDeclareTests.cpp
			        # The motion payload's reconstruction lanes, rendered: same
			        # target because it needs the same staged-shader resolver and
			        # the same Engine-level producer builder.
			        Tests/DeformationMotionPayloadTests.cpp
			        # The compatibility profile's instance-list fetch, rendered from the
			        # shipped instance_io.glsl: same staged-shader resolver.
			        Tests/CompatInstanceListFetchTests.cpp
			        # The producer's decisions: arm selection, the discontinuity
			        # policy, lane membership, and the segment keyword choice.
			        Tests/DeformationMotionProducerTests.cpp
			        Tests/ViewTemporalHistoryTests.cpp
			        Tests/TransmissiveDepthAttachmentTests.cpp
			        Tests/DynamicResolutionControllerTests.cpp
			        Tests/RenderServicesDrsModeTests.cpp
			        # Shader-path resolver for the spine-driven device tests (world/
			        # shadow pass setup resolves staged .spv against the build dir) —
			        # same shared setup the world/material test target uses.
			        Tests/EngineTestShaderSetup.cpp
			        ${CMAKE_SOURCE_DIR}/Tests/EngineShutdownTestEnvironment.cpp
			        )
			    target_link_libraries(RenderPipelineDeclareTests PRIVATE Engine RenderGraphTestSupport GTest::gtest GTest::gtest_main)
                if(MSVC)
                    target_compile_options(RenderPipelineDeclareTests PRIVATE /bigobj)
                endif()
			    target_compile_definitions(RenderPipelineDeclareTests PRIVATE
			        ENGINE_TEST_BUILD_DIR="${CMAKE_BINARY_DIR}"
			        # dev-only source anchoring for the SSR shader-contract rows
			        GE_RENDERER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
			    set_target_properties(RenderPipelineDeclareTests PROPERTIES
			        CXX_STANDARD 20
			        CXX_STANDARD_REQUIRED ON
			        FOLDER "Engine/Tests"
			    )
			    ge_set_output_tests(RenderPipelineDeclareTests)
			    ge_stage_runtime_dependencies(RenderPipelineDeclareTests)
			    # The grass-placement device oracles compile terrain_grass_place.comp at runtime;
			    # it #includes Includes/bindless_textures.glsl (IncludeRoot), cbt_atlas.glsl (CBT
			    # dir) and grass_placement.glsl (its own dir). Stage all three next to the test exe so EnsureComputePipeline's exe-relative
			    # candidate resolves — tests run from the build dir, not the repo root, so without this
			    # the shader isn't found, the compute never dispatches, and the oracles read placed=0.
			    add_custom_command(TARGET RenderPipelineDeclareTests POST_BUILD
			        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
			                "${CMAKE_SOURCE_DIR}/Engine/Modules/TerrainGrass/Shaders/TerrainGrass"
			                "$<TARGET_FILE_DIR:RenderPipelineDeclareTests>/Assets/Shaders/TerrainGrass"
			        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
			                "${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Includes"
			                "$<TARGET_FILE_DIR:RenderPipelineDeclareTests>/Assets/Shaders/Includes"
			        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
			                "${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Shaders/CBT"
			                "$<TARGET_FILE_DIR:RenderPipelineDeclareTests>/Assets/Shaders/CBT"
			        COMMENT "Staging grass compact + atlas shaders for RenderPipelineDeclareTests device oracles"
			        VERBATIM)
			    # The CBT terrain declare tests bring up the CBT feature, which loads its compiled kernels
			    # from Assets/Shaders/CBT next to the executable. CBTCompileShaders is declared after this
			    # directory, and only when glslc is found, so the copy is decided at generate time and
			    # Engine/Modules/CBTTerrain adds the build edge.
			    add_custom_command(TARGET RenderPipelineDeclareTests POST_BUILD
			        COMMAND "$<IF:$<TARGET_EXISTS:CBTCompileShaders>,${CMAKE_COMMAND};-E;copy_if_different;${CMAKE_BINARY_DIR}/Shaders/cbt_kernels.comp.spv;${CMAKE_BINARY_DIR}/Shaders/cbt_kernels_heap32.comp.spv;$<TARGET_FILE_DIR:RenderPipelineDeclareTests>/Assets/Shaders/CBT,${CMAKE_COMMAND};-E;true>"
			        COMMAND_EXPAND_LISTS
			        COMMENT "Staging the CBT kernels for RenderPipelineDeclareTests"
			        VERBATIM)
			    # Several declare tests load "Shaders/<name>.shaderpkg". CompileShaderPkgs
			    # is an ALL target, so a full build hides a missing edge; building this
			    # test alone does not. The edge resolves at generate time.
			    add_dependencies(RenderPipelineDeclareTests CompileShaderPkgs)
			    add_test(NAME RenderPipelineDeclareTests COMMAND $<TARGET_FILE:RenderPipelineDeclareTests>)

                if(NOT EMSCRIPTEN)
                    # A real unloadable node tests both its captured state and
                    # the module-instantiated arena destructor/execute thunks.
                    add_library(RenderGraphCallbackProbe MODULE Tests/RenderGraphCallbackProbeModule.cpp)
                    target_link_libraries(RenderGraphCallbackProbe PRIVATE Engine)
                    set_target_properties(RenderGraphCallbackProbe PROPERTIES
                        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON FOLDER "Engine/Tests")
                    ge_set_output_tests(RenderGraphCallbackProbe)

                    add_executable(RenderPipelineCallbackLifetimeTests Tests/RenderPipelineCallbackLifetimeTests.cpp)
                    target_link_libraries(RenderPipelineCallbackLifetimeTests PRIVATE Engine GTest::gtest GTest::gtest_main)
                    target_compile_definitions(RenderPipelineCallbackLifetimeTests PRIVATE
                        CALLBACK_PROBE_MODULE_NAME="$<TARGET_FILE_NAME:RenderGraphCallbackProbe>")
                    set_target_properties(RenderPipelineCallbackLifetimeTests PROPERTIES
                        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON FOLDER "Engine/Tests")
                    add_dependencies(RenderPipelineCallbackLifetimeTests RenderGraphCallbackProbe)
                    ge_set_output_tests(RenderPipelineCallbackLifetimeTests)
                    ge_stage_runtime_dependencies(RenderPipelineCallbackLifetimeTests)
                    add_test(NAME RenderPipelineCallbackLifetimeTests COMMAND $<TARGET_FILE:RenderPipelineCallbackLifetimeTests>)
                endif()

                # Headless shader correctness gates, independent of the editor
                # benchmark scripts and of pipeline-declaration fixture state.
                foreach(aa_test IN ITEMS TaaResolveHistoryTests SsaaResolveTests)
                    add_executable(${aa_test} Tests/${aa_test}.cpp)
                    target_link_libraries(${aa_test} PRIVATE Engine RenderGraphTestSupport GTest::gtest GTest::gtest_main)
                    set_target_properties(${aa_test} PROPERTIES
                        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON FOLDER "Engine/Tests")
                    ge_set_output_tests(${aa_test})
                    ge_stage_runtime_dependencies(${aa_test})
                    # Loads "Shaders/<name>.shaderpkg"; the edge resolves at generate time.
                    add_dependencies(${aa_test} CompileShaderPkgs)
                    add_test(NAME ${aa_test} COMMAND $<TARGET_FILE:${aa_test}>)
                    set_tests_properties(${aa_test} PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
                endforeach()
			endif()

			# Engine tests: the headless view fixture — a real device + RenderServices
			# spine + world view rendered without a window, handing back destination
			# bytes for the view-output-chain byte gates. Its own executable because
			# it pins process-wide dither/deband environment latches: sharing a process
			# with a suite that reads them first would silently change what it measures.
			if(GTest_FOUND OR TARGET GTest::gtest)
			    add_executable(HeadlessViewFixtureTests
			        Tests/HeadlessViewFixture.cpp
			        Tests/HeadlessViewFixtureTests.cpp
			        # I4 transport byte equality. Same target: it needs the same
			        # process-wide dither/deband latches pinned before any test body
			        # runs, and it shares the fixture TU.
			        Tests/HeadlessViewTransportTests.cpp
			        # I11 replication exemption, composed over that same transport
			        # stage — same target, same reason.
			        Tests/HeadlessViewReplicationTests.cpp
			        # Chain shapes B and C: the real UIManager composite over the
			        # finalized destination, the synthetic S1 overlay — same target,
			        # same latches, same fixture TU.
			        Tests/HeadlessViewCompositeTests.cpp
			        # I11's declare-time half: the terminal encode's extent invariant,
			        # over the real encode pass — same target, same latches, and it
			        # needs the fixture TU for its production-chain silence arm.
			        Tests/TransportExtentInvariantTests.cpp
			        Tests/EngineTestShaderSetup.cpp
			        )
			    target_link_libraries(HeadlessViewFixtureTests PRIVATE Engine RenderGraphTestSupport GTest::gtest GTest::gtest_main)
			    target_compile_definitions(HeadlessViewFixtureTests PRIVATE
			        ENGINE_TEST_BUILD_DIR="${CMAKE_BINARY_DIR}")
			    set_target_properties(HeadlessViewFixtureTests PROPERTIES
			        CXX_STANDARD 20
			        CXX_STANDARD_REQUIRED ON
			        FOLDER "Engine/Tests"
			    )
			    ge_set_output_tests(HeadlessViewFixtureTests)
			    ge_stage_runtime_dependencies(HeadlessViewFixtureTests)
			    # The finalize loads a .shaderpkg, which only CompileShaderPkgs produces;
			    # it is an ALL target, so a full build hides a missing edge. The edge
			    # resolves at generate time. The ui_sdf .spv set the HUD composite loads
			    # comes from CompileShaders, which Engine already depends on through
			    # GameEngineRendering.
			    add_dependencies(HeadlessViewFixtureTests CompileShaderPkgs)
			    add_test(NAME HeadlessViewFixtureTests COMMAND $<TARGET_FILE:HeadlessViewFixtureTests>)
			    # The finalize loads "Shaders/encode_srgb.shaderpkg" CWD-relative from
			    # the build root, where CompileShaderPkgs emits it.
			    set_tests_properties(HeadlessViewFixtureTests PROPERTIES
			        WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
			endif()

			# Engine tests: S2.3 validating RenderPipelineCompiler + the 24-blueprint fleet gate.
			if(GTest_FOUND OR TARGET GTest::gtest)
			    add_executable(RenderPipelineCompilerTests
			        Tests/RenderPipelineCompilerTests.cpp
			        Tests/EngineShaderPathTests.cpp
			        # C12 module-reload attribution of the node-factory registry.
			        Tests/RenderPipelineNodeModuleReloadTests.cpp
			        )
			    target_link_libraries(RenderPipelineCompilerTests PRIVATE Engine GTest::gtest GTest::gtest_main)
			    set_target_properties(RenderPipelineCompilerTests PROPERTIES
			        CXX_STANDARD 20
			        CXX_STANDARD_REQUIRED ON
			        FOLDER "Engine/Tests"
			    )
			    # Dev-only: the fleet gate globs the repo's .rendergraph source trees.
			    # This test is never shipped, so anchoring to the source dir is fine.
			    target_compile_definitions(RenderPipelineCompilerTests PRIVATE
			        GE_RENDERER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
			    ge_set_output_tests(RenderPipelineCompilerTests)
			    ge_stage_runtime_dependencies(RenderPipelineCompilerTests)
			    # Loads "Shaders/<name>.shaderpkg"; the edge resolves at generate time.
			    add_dependencies(RenderPipelineCompilerTests CompileShaderPkgs)
			    add_test(NAME RenderPipelineCompilerTests COMMAND $<TARGET_FILE:RenderPipelineCompilerTests>)
			    set_tests_properties(RenderPipelineCompilerTests PROPERTIES
			        WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
			endif()

			# Engine tests: the runtime host. One frame through a real window and device pins the
			# hook and readback contract; the HUD pointer rows are pure arithmetic.
			if(GTest_FOUND OR TARGET GTest::gtest)
			    add_executable(RuntimeHostTests
			        Tests/RuntimeHostTests.cpp
			        Tests/HudPointerTests.cpp
			        ${CMAKE_SOURCE_DIR}/Tests/EngineShutdownTestEnvironment.cpp
			        )
			    target_link_libraries(RuntimeHostTests PRIVATE Engine GTest::gtest GTest::gtest_main)
			    target_compile_definitions(RuntimeHostTests PRIVATE ENGINE_TEST_BUILD_DIR="${CMAKE_BINARY_DIR}")
			    set_target_properties(RuntimeHostTests PROPERTIES
			        CXX_STANDARD 20
			        CXX_STANDARD_REQUIRED ON
			        FOLDER "Engine/Tests"
			    )
			    ge_set_output_tests(RuntimeHostTests)
			    ge_stage_runtime_dependencies(RuntimeHostTests)
			    # The frame's finalize and terminal encode load "Shaders/<name>.shaderpkg" from the
			    # build root; the edge resolves at generate time.
			    add_dependencies(RuntimeHostTests CompileShaderPkgs)
			    add_test(NAME RuntimeHostTests COMMAND $<TARGET_FILE:RuntimeHostTests>)
			    set_tests_properties(RuntimeHostTests PROPERTIES
			        WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
			endif()

			# Engine tests: slice-3 RenderGraph pass arms (world/prepass/shadow on RGFrame).
			# Own executable, so its signal stays separate from the render-services
			# test executables.
			if(GTest_FOUND OR TARGET GTest::gtest)
			    add_executable(RenderServicesRGPassTests
			        Tests/RenderServicesRGPassTests.cpp
			        # Shader-path resolver for the pass-arm device tests (the world/
			        # shadow arms load the shared depth .spv staged in the build dir) —
			        # same shared setup the world/material and declare-spine targets use.
			        Tests/EngineTestShaderSetup.cpp
			        )
			    target_link_libraries(RenderServicesRGPassTests PRIVATE Engine GTest::gtest GTest::gtest_main)
			    target_compile_definitions(RenderServicesRGPassTests PRIVATE
			        ENGINE_TEST_BUILD_DIR="${CMAKE_BINARY_DIR}")
			    set_target_properties(RenderServicesRGPassTests PROPERTIES
			        CXX_STANDARD 20
			        CXX_STANDARD_REQUIRED ON
			        FOLDER "Engine/Tests"
			    )
			    ge_set_output_tests(RenderServicesRGPassTests)
			    ge_stage_runtime_dependencies(RenderServicesRGPassTests)
			    # The DDGI glossy resolve arm loads "Shaders/ddgi_glossy_resolve*.shaderpkg";
			    # the edge resolves at generate time.
			    add_dependencies(RenderServicesRGPassTests CompileShaderPkgs)
			    add_test(NAME RenderServicesRGPassTests COMMAND $<TARGET_FILE:RenderServicesRGPassTests>)
			endif()


	# Engine tests: Terrain GPU integration tests (requires Vulkan device)
	if(GTest_FOUND OR TARGET GTest::gtest)
		    add_executable(TerrainRenderFeatureTests
		        ${CMAKE_SOURCE_DIR}/Engine/Modules/Terrain/Tests/TerrainRenderFeatureTests.cpp
		        ${CMAKE_SOURCE_DIR}/Engine/Modules/Terrain/Tests/TiledTerrainTests.cpp
		        ${CMAKE_SOURCE_DIR}/Engine/Modules/Terrain/Tests/TerrainGrassRegionGpuTests.cpp
		        )
		    target_link_libraries(TerrainRenderFeatureTests PRIVATE
		        Engine
		        GTest::gtest GTest::gtest_main)
        target_include_directories(TerrainRenderFeatureTests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Tests
            ${CMAKE_SOURCE_DIR}/Engine/Modules/TerrainECS/Source
            ${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering
            ${Vulkan_INCLUDE_DIRS})
        target_compile_definitions(TerrainRenderFeatureTests PRIVATE
            GRASS_REGION_MODULES_DIR="${CMAKE_SOURCE_DIR}/Engine/Modules")
		    set_target_properties(TerrainRenderFeatureTests PROPERTIES
		        CXX_STANDARD 20
		        CXX_STANDARD_REQUIRED ON
		        FOLDER "Engine/Tests/Terrain"
		    )
		    ge_set_output_tests(TerrainRenderFeatureTests)
		    ge_stage_runtime_dependencies(TerrainRenderFeatureTests)
		    add_test(NAME TerrainRenderFeatureTests COMMAND $<TARGET_FILE:TerrainRenderFeatureTests>)
	endif()

	# Engine tests: GPU compute skinning validation (requires Vulkan device)
	if(GTest_FOUND OR TARGET GTest::gtest)
	    add_executable(GPUAnimationComputeTests
	        Tests/GPUAnimationComputeTests.cpp
	        )
	    target_link_libraries(GPUAnimationComputeTests PRIVATE
	        Engine GameEngine.Native
	        GTest::gtest GTest::gtest_main)
	    set_target_properties(GPUAnimationComputeTests PROPERTIES
	        CXX_STANDARD 20
	        CXX_STANDARD_REQUIRED ON
	        FOLDER "Engine/Tests"
	    )
	    ge_set_output_tests(GPUAnimationComputeTests)
	    ge_stage_runtime_dependencies(GPUAnimationComputeTests)
	    add_test(NAME GPUAnimationComputeTests COMMAND $<TARGET_FILE:GPUAnimationComputeTests>)
	    # Same suite on the classic descriptor-set path. Validation on the default
	    # descriptor-buffer path misses some descriptor errors the classic path
	    # reports, e.g. a storage descriptor on a buffer without STORAGE usage.
	    add_test(NAME GPUAnimationComputeTests_ClassicDescriptorSets
	        COMMAND $<TARGET_FILE:GPUAnimationComputeTests>)
	    set_tests_properties(GPUAnimationComputeTests_ClassicDescriptorSets PROPERTIES
	        ENVIRONMENT "GE_VK_USE_DESCRIPTOR_BUFFER=0")
	endif()

	# Engine tests: Phase 25.4 GPU retarget parity — dispatch retarget_full.comp
	# and compare the SkinPaletteAtlas mat4s against the CPU reference. Skips
	# at runtime when no Vulkan device is available.
	if(GTest_FOUND OR TARGET GTest::gtest)
	    add_executable(RetargetGPUParityTests
	        Tests/RetargetGPUParityTests.cpp
	        )
	    # These retarget/animation suites link Engine ONLY (plus the
	    # GameEngine.Native ABI DLL). Animation is an OBJECT module spliced into
	    # Engine.dll; linking it directly as well would put a second copy of the
	    # module's file-scope state in the exe (issue #356, same disease as
	    # #340). Animation headers propagate via Engine's re-exported INTERFACE.
	    target_link_libraries(RetargetGPUParityTests PRIVATE
	        Engine GameEngine.Native
	        GTest::gtest GTest::gtest_main)
	    target_compile_definitions(RetargetGPUParityTests PRIVATE
	        GE_BINARY_DIR="${CMAKE_BINARY_DIR}"
	        RENDERING_SOURCE_DIR="${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering")
	    set_target_properties(RetargetGPUParityTests PROPERTIES
	        CXX_STANDARD 20
	        CXX_STANDARD_REQUIRED ON
	        FOLDER "Engine/Tests"
	    )
	    ge_set_output_tests(RetargetGPUParityTests)
	    ge_stage_runtime_dependencies(RetargetGPUParityTests)
	    add_dependencies(RetargetGPUParityTests CompileShaders)
	    # Stage the fused retarget shader next to the test exe so the
	    # cwd-relative shader lookup hits it without a system install.
	    add_custom_command(TARGET RetargetGPUParityTests POST_BUILD
	        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:RetargetGPUParityTests>/Assets/Shaders
	        COMMAND ${CMAKE_COMMAND} -E copy_if_different
	            ${CMAKE_BINARY_DIR}/Shaders/retarget_full.comp.spv
	            $<TARGET_FILE_DIR:RetargetGPUParityTests>/Assets/Shaders/retarget_full.comp.spv
	        COMMENT "Staging retarget_full.comp.spv for RetargetGPUParityTests"
	        VERBATIM
	    )
	    add_test(NAME RetargetGPUParityTests COMMAND $<TARGET_FILE:RetargetGPUParityTests>
	             WORKING_DIRECTORY $<TARGET_FILE_DIR:RetargetGPUParityTests>)
	endif()


	# Engine tests: Phase 6 ECS bridge — HumanoidRetargetSystem end-to-end,
	# auto-bootstrap, layered-blend integration, hot-reload race, same-rig
	# coexistence. Runs in pure CPU mode (no GPU device required).
	if(GTest_FOUND OR TARGET GTest::gtest)
	    add_executable(HumanoidRetargetSystemTests
	        Tests/HumanoidRetargetSystemTests.cpp
	        Tests/HumanoidRetargetSourceTests.cpp
	        ${CMAKE_SOURCE_DIR}/Tests/EngineShutdownTestEnvironment.cpp
	        )
	    target_link_libraries(HumanoidRetargetSystemTests PRIVATE
	        Engine GameEngine.Native
	        GTest::gtest GTest::gtest_main)
	    set_target_properties(HumanoidRetargetSystemTests PROPERTIES
	        CXX_STANDARD 20
	        CXX_STANDARD_REQUIRED ON
	        FOLDER "Engine/Tests"
	    )
	    ge_set_output_tests(HumanoidRetargetSystemTests)
	    ge_stage_runtime_dependencies(HumanoidRetargetSystemTests)
	    add_test(NAME HumanoidRetargetSystemTests COMMAND $<TARGET_FILE:HumanoidRetargetSystemTests>)

	    # Engine tests: Phase 8 crowd benchmark + perf gate. Drives 100 + 500
	    # retargeted characters through the CPU fallback and gates against a
	    # baseline JSON in Engine/Tests/Baselines/Retarget/. CPU-only by
	    # design; GPU integration adds GPU-time columns when wired.
	    add_executable(HumanoidRetargetCrowdBench
	        Tests/HumanoidRetargetCrowdBench.cpp
	        Tests/RetargetSafetyTests.cpp
	        )
	    target_link_libraries(HumanoidRetargetCrowdBench PRIVATE
	        Engine GameEngine.Native
	        GTest::gtest GTest::gtest_main)
	    # Dev-only: the bench WRITES committed baselines back into the source
	    # tree (Engine/Tests/Baselines/Retarget). Self-declaring name; the
	    # bench falls back to <cwd>/Baselines when built without a source tree.
	    target_compile_definitions(HumanoidRetargetCrowdBench PRIVATE
	        GE_DEV_SOURCE_TREE_DIR="${CMAKE_SOURCE_DIR}")
	    set_target_properties(HumanoidRetargetCrowdBench PROPERTIES
	        CXX_STANDARD 20
	        CXX_STANDARD_REQUIRED ON
	        FOLDER "Engine/Tests"
	    )
	    ge_set_output_tests(HumanoidRetargetCrowdBench)
	    ge_stage_runtime_dependencies(HumanoidRetargetCrowdBench)
	    add_test(NAME HumanoidRetargetCrowdBench COMMAND $<TARGET_FILE:HumanoidRetargetCrowdBench>)
	    set_tests_properties(HumanoidRetargetCrowdBench PROPERTIES LABELS "Animation;Bench")

	    # Engine tests: Phase 7b clip-frame bake helper (HumanoidRigEdit).
	    # Lives at Engine/ scope (not Animation module) because BakeRetarget-
	    # PoseFromClipFrame depends on AnimationClip's Asset I/O, which is
	    # implemented in Engine/Source/Assets/AnimationClip.cpp.
	    add_executable(HumanoidRigEditClipBakeTests
	        Tests/HumanoidRigEditClipBakeTests.cpp
	        )
	    target_link_libraries(HumanoidRigEditClipBakeTests PRIVATE
	        Engine GameEngine.Native
	        GTest::gtest GTest::gtest_main)
	    set_target_properties(HumanoidRigEditClipBakeTests PROPERTIES
	        CXX_STANDARD 20
	        CXX_STANDARD_REQUIRED ON
	        FOLDER "Engine/Tests"
	    )
	    ge_set_output_tests(HumanoidRigEditClipBakeTests)
	    ge_stage_runtime_dependencies(HumanoidRigEditClipBakeTests)
	    add_test(NAME HumanoidRigEditClipBakeTests COMMAND $<TARGET_FILE:HumanoidRigEditClipBakeTests>)
	endif()

	# Engine tests: Phase 9 end-to-end smoke test. Single integration test
	# that drives 30 frames through HumanoidRetargetSystem's CPU fallback
	# path on a synthetic 4-bone humanoid spine + walking clip; asserts no
	# NaNs, non-identity skin matrices, no crashes. The proof that the
	# whole retarget stack works end-to-end (different from the per-stage
	# unit tests in Animation/Tests/).
	if(GTest_FOUND OR TARGET GTest::gtest)
	    add_executable(HumanoidRetargetEndToEndSmokeTest
	        Tests/HumanoidRetargetEndToEndSmokeTest.cpp
	        )
	    target_link_libraries(HumanoidRetargetEndToEndSmokeTest PRIVATE
	        Engine GameEngine.Native
	        GTest::gtest GTest::gtest_main)
	    set_target_properties(HumanoidRetargetEndToEndSmokeTest PROPERTIES
	        CXX_STANDARD 20
	        CXX_STANDARD_REQUIRED ON
	        FOLDER "Engine/Tests"
	    )
	    ge_set_output_tests(HumanoidRetargetEndToEndSmokeTest)
	    ge_stage_runtime_dependencies(HumanoidRetargetEndToEndSmokeTest)
	    add_test(NAME HumanoidRetargetEndToEndSmokeTest COMMAND $<TARGET_FILE:HumanoidRetargetEndToEndSmokeTest>)
	endif()

	# Engine tests: Phase B1 .blend file smoke parse via vendored fbtBlend.
	# Skip-gates if Tests/BlendSamples/ is not extracted.
	# Compiles the vendored fbtBlend.cpp shim directly so this test target does
	# not depend on the full Engine link chain (lets Phase B1 land standalone,
	# decoupled from concurrent retarget-pipeline work in RenderServices).
	if(GTest_FOUND OR TARGET GTest::gtest)
	    add_executable(BlendSmokeParseTests
	        Tests/BlendSmokeParseTests.cpp
	        Source/ThirdParty/fbtBlend/fbtBlend.cpp)
	    target_link_libraries(BlendSmokeParseTests PRIVATE
	        GTest::gtest GTest::gtest_main)
	    target_include_directories(BlendSmokeParseTests PRIVATE
	        ${CMAKE_CURRENT_SOURCE_DIR}/Source/ThirdParty/fbtBlend)
	    target_compile_definitions(BlendSmokeParseTests PRIVATE
	        GE_HAVE_FBTBLEND=1)
	    if(TARGET ZLIB::ZLIB)
	        target_link_libraries(BlendSmokeParseTests PRIVATE ZLIB::ZLIB)
	        target_compile_definitions(BlendSmokeParseTests PRIVATE GE_FBTBLEND_HAVE_ZLIB=1)
	    endif()
	    if(TARGET zstd::libzstd_static)
	        target_link_libraries(BlendSmokeParseTests PRIVATE zstd::libzstd_static)
	        target_compile_definitions(BlendSmokeParseTests PRIVATE GE_FBTBLEND_HAVE_ZSTD=1)
	    elseif(TARGET zstd::libzstd_shared)
	        target_link_libraries(BlendSmokeParseTests PRIVATE zstd::libzstd_shared)
	        target_compile_definitions(BlendSmokeParseTests PRIVATE GE_FBTBLEND_HAVE_ZSTD=1)
	    elseif(TARGET zstd::libzstd)
	        target_link_libraries(BlendSmokeParseTests PRIVATE zstd::libzstd)
	        target_compile_definitions(BlendSmokeParseTests PRIVATE GE_FBTBLEND_HAVE_ZSTD=1)
	    endif()
	    set_source_files_properties(Source/ThirdParty/fbtBlend/fbtBlend.cpp
	        TARGET_DIRECTORY BlendSmokeParseTests
	        PROPERTIES
	            SKIP_PRECOMPILE_HEADERS ON
	            SKIP_UNITY_BUILD_INCLUSION ON
	            COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/WX->")
	    set_source_files_properties(Tests/BlendSmokeParseTests.cpp
	        TARGET_DIRECTORY BlendSmokeParseTests
	        PROPERTIES
	            SKIP_PRECOMPILE_HEADERS ON
	            SKIP_UNITY_BUILD_INCLUSION ON
	            COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC>:/WX->")
	    set_target_properties(BlendSmokeParseTests PROPERTIES
	        CXX_STANDARD 20
	        CXX_STANDARD_REQUIRED ON
	        FOLDER "Engine/Tests"
	    )
	    ge_set_output_tests(BlendSmokeParseTests)
	    ge_stage_runtime_dependencies(BlendSmokeParseTests)
	    add_test(NAME BlendSmokeParseTests COMMAND $<TARGET_FILE:BlendSmokeParseTests>)
	endif()

	# Engine tests: Phase B2 .blend model importer (mesh + skeleton extraction).
	# Skip-gates when Tests/BlendSamples/ is not extracted.
	if(GTest_FOUND OR TARGET GTest::gtest)
	    add_executable(BlendModelLoadTests
	        Tests/BlendModelLoadTests.cpp
	        )
	    target_link_libraries(BlendModelLoadTests PRIVATE
	        Engine GameEngine.Native
	        GTest::gtest GTest::gtest_main)
	    set_target_properties(BlendModelLoadTests PROPERTIES
	        CXX_STANDARD 20
	        CXX_STANDARD_REQUIRED ON
	        FOLDER "Engine/Tests"
	    )
	    ge_set_output_tests(BlendModelLoadTests)
	    ge_stage_runtime_dependencies(BlendModelLoadTests)
	    add_test(NAME BlendModelLoadTests COMMAND $<TARGET_FILE:BlendModelLoadTests>)
	endif()

	# Engine tests: Phase B3 .blend Action -> AnimationClip extraction.
	# Skip-gates when Tests/BlendSamples/ is not extracted.
	if(GTest_FOUND OR TARGET GTest::gtest)
	    add_executable(BlendActionExtractTests
	        Tests/BlendActionExtractTests.cpp
	        )
	    target_link_libraries(BlendActionExtractTests PRIVATE
	        Engine GameEngine.Native
	        GTest::gtest GTest::gtest_main)
	    set_target_properties(BlendActionExtractTests PROPERTIES
	        CXX_STANDARD 20
	        CXX_STANDARD_REQUIRED ON
	        FOLDER "Engine/Tests"
	    )
	    ge_set_output_tests(BlendActionExtractTests)
	    ge_stage_runtime_dependencies(BlendActionExtractTests)
	    add_test(NAME BlendActionExtractTests COMMAND $<TARGET_FILE:BlendActionExtractTests>)
	endif()

	# Engine tests: ModelAsset::PostLoad humanoid auto-bootstrap on real
	# store-licensed test fixtures. Skip-gated when Assets/Models/FBXTest/ isn't present.
	if(GTest_FOUND OR TARGET GTest::gtest)
	    add_executable(RuntimeHumanoidProfileTests Tests/RuntimeHumanoidProfileTests.cpp
	        ${CMAKE_SOURCE_DIR}/Tests/EngineShutdownTestEnvironment.cpp)
	    target_link_libraries(RuntimeHumanoidProfileTests PRIVATE Engine GTest::gtest GTest::gtest_main)
	    target_compile_definitions(RuntimeHumanoidProfileTests PRIVATE
	        RUNTIME_PROFILE_FIXTURE="${CMAKE_SOURCE_DIR}/Assets/SkeletonProfiles/HumanoidStandard.profile.json")
	    set_target_properties(RuntimeHumanoidProfileTests PROPERTIES
	        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON FOLDER "Engine/Tests")
	    ge_set_output_tests(RuntimeHumanoidProfileTests)
	    ge_stage_runtime_dependencies(RuntimeHumanoidProfileTests)
	    add_test(NAME RuntimeHumanoidProfileTests COMMAND $<TARGET_FILE:RuntimeHumanoidProfileTests>)
	    set_tests_properties(RuntimeHumanoidProfileTests PROPERTIES LABELS "Animation;Integration")

	    add_executable(ModelAssetHumanoidPostLoadTests
	        Tests/ModelAssetHumanoidPostLoadTests.cpp
	        )
	    target_link_libraries(ModelAssetHumanoidPostLoadTests PRIVATE
	        Engine GameEngine.Native
	        GTest::gtest GTest::gtest_main)
	    set_target_properties(ModelAssetHumanoidPostLoadTests PROPERTIES
	        CXX_STANDARD 20
	        CXX_STANDARD_REQUIRED ON
	        FOLDER "Engine/Tests"
	    )
	    ge_set_output_tests(ModelAssetHumanoidPostLoadTests)
	    ge_stage_runtime_dependencies(ModelAssetHumanoidPostLoadTests)
	    add_test(NAME ModelAssetHumanoidPostLoadTests COMMAND $<TARGET_FILE:ModelAssetHumanoidPostLoadTests>)

	    add_executable(FbxAxisConversionTests
	        Tests/FbxAxisConversionTests.cpp
	        Tests/FbxEmissionTests.cpp
	        Tests/FbxMeshInstanceTests.cpp
	        )
	    target_link_libraries(FbxAxisConversionTests PRIVATE
	        Engine
	        GTest::gtest GTest::gtest_main)
	    target_include_directories(FbxAxisConversionTests PRIVATE
	        ${CMAKE_SOURCE_DIR}/Engine/Source/Assets
	        ${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/ufbx
	        ${CMAKE_SOURCE_DIR}/Engine/Tests)
	    target_compile_definitions(FbxAxisConversionTests PRIVATE GE_HAVE_UFBX)
	    set_target_properties(FbxAxisConversionTests PROPERTIES
	        CXX_STANDARD 20
	        CXX_STANDARD_REQUIRED ON
	        FOLDER "Engine/Tests"
	    )
	    ge_set_output_tests(FbxAxisConversionTests)
	    ge_stage_runtime_dependencies(FbxAxisConversionTests)
	    add_test(NAME FbxAxisConversionTests COMMAND $<TARGET_FILE:FbxAxisConversionTests>)

	    # glTF import refusals (cgltf_validate, extensionsRequired, short external buffers) and the
	    # animation channel paths the glTF clip path keeps; every document is built in the test.
	    add_executable(GltfImportTests
	        Tests/GltfImportTests.cpp
	        )
	    target_link_libraries(GltfImportTests PRIVATE
	        Engine
	        GTest::gtest GTest::gtest_main)
	    target_include_directories(GltfImportTests PRIVATE
	        ${CMAKE_SOURCE_DIR}/Engine/Source/Assets
	        ${CGLTF_INCLUDE_DIRS})
	    set_target_properties(GltfImportTests PROPERTIES
	        CXX_STANDARD 20
	        CXX_STANDARD_REQUIRED ON
	        FOLDER "Engine/Tests"
	    )
	    ge_set_output_tests(GltfImportTests)
	    ge_stage_runtime_dependencies(GltfImportTests)
	    add_test(NAME GltfImportTests COMMAND $<TARGET_FILE:GltfImportTests>)
	endif()

	# Engine tests: Phase 13 real-asset E2E retargeting on BusinessMale +
	# A_Walk_F_Masc. Skip-gates when the licensed test fixture isn't present.
	# Discriminates "math is correct on real cross-rig data" vs broken /
	# T-pose output. The synthetic suites can't surface this signal because
	# they share rest poses across source + target.
	if(GTest_FOUND OR TARGET GTest::gtest)
	    add_executable(HumanoidRetargetRealAssetTests
	        Tests/HumanoidRetargetRealAssetTests.cpp
	        )
	    target_link_libraries(HumanoidRetargetRealAssetTests PRIVATE
	        Engine GameEngine.Native
	        GTest::gtest GTest::gtest_main)
	    set_target_properties(HumanoidRetargetRealAssetTests PROPERTIES
	        CXX_STANDARD 20
	        CXX_STANDARD_REQUIRED ON
	        FOLDER "Engine/Tests"
	    )
	    ge_set_output_tests(HumanoidRetargetRealAssetTests)
	    ge_stage_runtime_dependencies(HumanoidRetargetRealAssetTests)
	    add_test(NAME HumanoidRetargetRealAssetTests COMMAND $<TARGET_FILE:HumanoidRetargetRealAssetTests>)
	    set_tests_properties(HumanoidRetargetRealAssetTests PROPERTIES LABELS "Animation;Integration")
	endif()

	# Engine tests: Phase B4 cross-format retargeting smoke test. Loads
	# BusinessMale.fbx as target + Ellie's ellie_animation.blend as source,
	# auto-imports humanoid sidecars on both, builds a RetargetMap, and
	# asserts non-T-pose output across 30 frames of CPU retarget evaluation.
	# Skip-gated when blend-samples/ellie/... or BusinessMale.fbx isn't
	# present.
	if(GTest_FOUND OR TARGET GTest::gtest)
	    add_executable(BlenderToFbxRetargetTest
	        Tests/BlenderToFbxRetargetTest.cpp
	        )
	    target_link_libraries(BlenderToFbxRetargetTest PRIVATE
	        Engine GameEngine.Native
	        GTest::gtest GTest::gtest_main)
	    set_target_properties(BlenderToFbxRetargetTest PROPERTIES
	        CXX_STANDARD 20
	        CXX_STANDARD_REQUIRED ON
	        FOLDER "Engine/Tests"
	    )
	    ge_set_output_tests(BlenderToFbxRetargetTest)
	    ge_stage_runtime_dependencies(BlenderToFbxRetargetTest)
	    add_test(NAME BlenderToFbxRetargetTest COMMAND $<TARGET_FILE:BlenderToFbxRetargetTest>)
	    set_tests_properties(BlenderToFbxRetargetTest PROPERTIES LABELS "Animation;Integration;Blend")
	endif()

	# Durable model skeleton sources, group remapping and runtime ownership.
	if(GTest_FOUND OR TARGET GTest::gtest)
	    add_executable(ModelSkeletonPersistenceTests Tests/ModelSkeletonPersistenceTests.cpp)
	    target_link_libraries(ModelSkeletonPersistenceTests PRIVATE Engine GTest::gtest GTest::gtest_main)
	    target_compile_features(ModelSkeletonPersistenceTests PRIVATE cxx_std_20)
	    ge_set_output_tests(ModelSkeletonPersistenceTests)
	    ge_stage_runtime_dependencies(ModelSkeletonPersistenceTests)
	    add_test(NAME ModelSkeletonPersistenceTests COMMAND $<TARGET_FILE:ModelSkeletonPersistenceTests>)
	endif()

	# Engine tests: Phase 15 — editor skinned-mesh spawn-flow contract.
	# Verifies ModelEntityFactory::CreateFromModel produces Transform +
	# SkinnedMeshRenderer + SkeletonRef + AnimatorRef on every skinned
	# submesh entity (the components animation requires) AND that clip
	# assignment is non-destructive. Skip-gated when the licensed test fixtures
	# or Vulkan device aren't present.
	if(GTest_FOUND OR TARGET GTest::gtest)
	    add_executable(EditorSkinnedSpawnTests
	        Tests/EditorSkinnedSpawnTests.cpp
	        )
	    target_link_libraries(EditorSkinnedSpawnTests PRIVATE
	        Engine GameEngine.Native
	        GTest::gtest GTest::gtest_main)
	    target_include_directories(EditorSkinnedSpawnTests PRIVATE
	        ${CMAKE_CURRENT_SOURCE_DIR}/Tests)
	    set_target_properties(EditorSkinnedSpawnTests PROPERTIES
	        CXX_STANDARD 20
	        CXX_STANDARD_REQUIRED ON
	        FOLDER "Engine/Tests"
	    )
	    ge_set_output_tests(EditorSkinnedSpawnTests)
	    ge_stage_runtime_dependencies(EditorSkinnedSpawnTests)
	    add_test(NAME EditorSkinnedSpawnTests COMMAND $<TARGET_FILE:EditorSkinnedSpawnTests>)
	    set_tests_properties(EditorSkinnedSpawnTests PROPERTIES LABELS "Animation;Integration")
	endif()

	# Engine tests: CoreCLR hosting smoke test (cross-platform)
	if(GTest_FOUND OR TARGET GTest::gtest)
	    add_executable(EngineCoreClrSmokeTests
	        Tests/CoreClrHostingSmokeTests.cpp
	    )
		    target_link_libraries(EngineCoreClrSmokeTests PRIVATE Engine GameEngine.Native GTest::gtest GTest::gtest_main)
		    # Ensure the smoke test can see the real .NET hosting headers when available.
		    if(NETHOST_LIBRARY)
		        target_include_directories(EngineCoreClrSmokeTests PRIVATE ${NETHOST_INCLUDE_DIR})
		    endif()
	    set_target_properties(EngineCoreClrSmokeTests PROPERTIES
	        CXX_STANDARD 20
	        CXX_STANDARD_REQUIRED ON
	        FOLDER "Engine/Tests"
	    )
	    ge_set_output_tests(EngineCoreClrSmokeTests)
	    ge_stage_runtime_dependencies(EngineCoreClrSmokeTests)

	    # Stage managed CoreBridge/HotReload outputs next to the test executable so
	    # PathResolver can find runtimeconfig + assemblies when running from the build tree.
	    if(ENABLE_SCRIPTING)
	        add_custom_command(TARGET EngineCoreClrSmokeTests POST_BUILD
	            COMMAND ${CMAKE_COMMAND} -E copy_directory
	                    "${CMAKE_SOURCE_DIR}/Managed/CoreBridge/bin/$<CONFIG>/net10.0"
	                    "$<TARGET_FILE_DIR:EngineCoreClrSmokeTests>"
	            COMMAND ${CMAKE_COMMAND} -E copy_directory
	                    "${CMAKE_SOURCE_DIR}/Managed/HotReload/bin/$<CONFIG>/net10.0"
	                    "$<TARGET_FILE_DIR:EngineCoreClrSmokeTests>"
	            COMMENT "Staging managed CoreBridge/HotReload next to EngineCoreClrSmokeTests"
	        )
	    endif()
	    add_test(NAME EngineCoreClrSmokeTests COMMAND $<TARGET_FILE:EngineCoreClrSmokeTests>)
	endif()

	# Engine tests: terrain grass wind volumes. The sources live with the module that owns the
	# shader but build here, because the parity test builds an ECS world and calls the engine's
	# WindVolumeResolver — the closure TerrainGrassStatsTests deliberately does without. Both
	# suites compile blocks lifted verbatim out of the shipped GLSL, so a shader that drifts from
	# the CPU resolver reds them instead of passing against a copy.
	if(GTest_FOUND OR TARGET GTest::gtest)
	    set(GRASS_WIND_TEST_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/GrassWindVolumesExtracted.h")
	    add_custom_command(OUTPUT "${GRASS_WIND_TEST_HEADER}"
	        COMMAND ${CMAKE_COMMAND}
	            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/Modules/TerrainGrass/Shaders/TerrainGrass/grass_wind_volumes.glsl
	            -DOUT=${GRASS_WIND_TEST_HEADER}
	            "-DBEGIN_MARKER=// GE_SHARED_GRASS_WIND_VOLUMES_BEGIN"
	            "-DEND_MARKER=// GE_SHARED_GRASS_WIND_VOLUMES_END"
	            -P ${CMAKE_CURRENT_SOURCE_DIR}/Modules/CBTTerrain/Tests/ExtractShaderBlock.cmake
	        DEPENDS Modules/TerrainGrass/Shaders/TerrainGrass/grass_wind_volumes.glsl
	            Modules/CBTTerrain/Tests/ExtractShaderBlock.cmake
	        VERBATIM)
	    set(GRASS_WIND_APPLY_TEST_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/GrassWindApplyExtracted.h")
	    add_custom_command(OUTPUT "${GRASS_WIND_APPLY_TEST_HEADER}"
	        COMMAND ${CMAKE_COMMAND}
	            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/Modules/TerrainGrass/Shaders/TerrainGrass/grass_wind_volumes.glsl
	            -DOUT=${GRASS_WIND_APPLY_TEST_HEADER}
	            "-DBEGIN_MARKER=// GE_SHARED_GRASS_WIND_APPLY_BEGIN"
	            "-DEND_MARKER=// GE_SHARED_GRASS_WIND_APPLY_END"
	            -P ${CMAKE_CURRENT_SOURCE_DIR}/Modules/CBTTerrain/Tests/ExtractShaderBlock.cmake
	        DEPENDS Modules/TerrainGrass/Shaders/TerrainGrass/grass_wind_volumes.glsl
	            Modules/CBTTerrain/Tests/ExtractShaderBlock.cmake
	        VERBATIM)
	    add_executable(GrassWindVolumeTests
	        Modules/TerrainGrass/Tests/GrassWindVolumeTests.cpp
	        Modules/TerrainGrass/Tests/GrassWindApplyTests.cpp
	        "${GRASS_WIND_TEST_HEADER}" "${GRASS_WIND_APPLY_TEST_HEADER}")
	    target_link_libraries(GrassWindVolumeTests PRIVATE Engine GTest::gtest GTest::gtest_main)
	    target_include_directories(GrassWindVolumeTests PRIVATE
	        Modules/CBTTerrain/Tests "${CMAKE_CURRENT_BINARY_DIR}/generated")
	    set_target_properties(GrassWindVolumeTests PROPERTIES CXX_STANDARD 20 FOLDER "Engine/Tests")
	    ge_set_output_tests(GrassWindVolumeTests)
	    ge_stage_runtime_dependencies(GrassWindVolumeTests)
	    add_test(NAME GrassWindVolumeTests COMMAND GrassWindVolumeTests)
	endif()

    if((GTest_FOUND OR TARGET GTest::gtest) AND NOT EMSCRIPTEN)
        add_executable(PackageNativeCacheTests Tests/PackageNativeCacheTests.cpp)
        target_include_directories(PackageNativeCacheTests PRIVATE "${CMAKE_SOURCE_DIR}/Tests")
        target_link_libraries(PackageNativeCacheTests PRIVATE Engine GTest::gtest GTest::gtest_main)
        ge_set_output_tests(PackageNativeCacheTests)
        ge_stage_runtime_dependencies(PackageNativeCacheTests)
        add_test(NAME PackageNativeCacheTests COMMAND PackageNativeCacheTests)
        set_tests_properties(PackageNativeCacheTests PROPERTIES TIMEOUT 60)
    endif()

	# Engine tests: Portable particle authoring contracts.
	if(GTest_FOUND OR TARGET GTest::gtest)
	    add_executable(ParticleCompatibilityTests
	        Tests/ParticleCompatibilityTests.cpp
            Tests/ParticleSimulationSystemTests.cpp
            Tests/ParticleStackAssetTests.cpp
	        )
	    target_link_libraries(ParticleCompatibilityTests PRIVATE Engine GTest::gtest GTest::gtest_main)
	    set_target_properties(ParticleCompatibilityTests PROPERTIES
	        CXX_STANDARD 20
	        CXX_STANDARD_REQUIRED ON
	        FOLDER "Engine/Tests"
	    )
	    ge_set_output_tests(ParticleCompatibilityTests)
	    ge_stage_runtime_dependencies(ParticleCompatibilityTests)
	    add_test(NAME ParticleCompatibilityTests COMMAND $<TARGET_FILE:ParticleCompatibilityTests>)

        # The particle module links no engine: the processor registry's module stamp is the one ECS
        # source it needs beyond the logger, compiled in.
        add_executable(ParticleRuntimeTests Tests/ParticleRuntimeTests.cpp Modules/ECS/Source/ModuleRegistration.cpp)
        target_include_directories(ParticleRuntimeTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Include)
        target_link_libraries(ParticleRuntimeTests PRIVATE Particles ParticlesAuthoring AssetCore Logger GTest::gtest GTest::gtest_main)
        ge_set_output_tests(ParticleRuntimeTests)
        ge_stage_runtime_dependencies(ParticleRuntimeTests)
        add_test(NAME ParticleRuntimeTests COMMAND $<TARGET_FILE:ParticleRuntimeTests>)

	endif()

	# Engine tests: Scene file load/save (schema round-trips, hierarchy authoring order), plus the
	# field-bound passes those schemas apply and the reflected read shape those bounds are read
	# back through. Both extra suites need the REAL component reflection, which only exists once
	# Engine is linked and its generated reflection TU has run its static initializers — in a
	# suite without Engine they would sweep an empty registry and pass vacuously. The clamp suite
	# additionally lives with the module that owns the pass but builds here, because constructing
	# a real component pulls the AssetCore closure TerrainGrassStatsTests deliberately does without.
	if(GTest_FOUND OR TARGET GTest::gtest)
	    add_executable(EngineSceneIOTests
	        Tests/SceneIOTests.cpp
	        Tests/EmbeddedAnimationPersistenceTests.cpp
	        Tests/SceneIntegerTests.cpp
	        Tests/SceneStructArrayTests.cpp
	        Tests/SceneSchemaOwnershipTests.cpp
	        Tests/SceneExportStripTests.cpp
	        Tests/SplineSceneSerializationTests.cpp
	        Tests/ReflectedComponentJsonTests.cpp
	        Modules/TerrainGrass/Tests/TerrainGrassClampsTests.cpp)
	    target_link_libraries(EngineSceneIOTests PRIVATE Engine GTest::gtest GTest::gtest_main)
	    # TerrainGrassClamps.SceneSchemaCarriesNoInlineClampCall reads the scene schema and fails on
	    # any field bound written inline there instead of in the shared clamp pass.
	    target_compile_definitions(EngineSceneIOTests PRIVATE
	        TERRAIN_GRASS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Modules/TerrainGrass/Source")
	    if(TARGET GameEngine.Native)
	        target_link_libraries(EngineSceneIOTests PRIVATE GameEngine.Native)
	    endif()
	    set_target_properties(EngineSceneIOTests PROPERTIES
	        CXX_STANDARD 20
	        CXX_STANDARD_REQUIRED ON
	        FOLDER "Engine/Tests"
	    )
	    ge_set_output_tests(EngineSceneIOTests)
	    ge_stage_runtime_dependencies(EngineSceneIOTests)
	    add_test(NAME EngineSceneIOTests COMMAND $<TARGET_FILE:EngineSceneIOTests>)
	endif()

	if(GTest_FOUND OR TARGET GTest::gtest)
	    add_executable(ValueCurveEvaluatorTests
	        Tests/ValueCurveEvaluatorTests.cpp
	        )
	    target_link_libraries(ValueCurveEvaluatorTests PRIVATE Engine GTest::gtest GTest::gtest_main)
	    set_target_properties(ValueCurveEvaluatorTests PROPERTIES
	        CXX_STANDARD 20
	        CXX_STANDARD_REQUIRED ON
	        FOLDER "Engine/Tests"
	    )
	    ge_set_output_tests(ValueCurveEvaluatorTests)
	    ge_stage_runtime_dependencies(ValueCurveEvaluatorTests)
	    add_test(NAME ValueCurveEvaluatorTests COMMAND $<TARGET_FILE:ValueCurveEvaluatorTests>)

	    add_executable(AsyncVideoRecorderTests
	        Tests/AsyncVideoRecorderTests.cpp
	    )
	    target_link_libraries(AsyncVideoRecorderTests PRIVATE Engine GTest::gtest GTest::gtest_main)
	    set_target_properties(AsyncVideoRecorderTests PROPERTIES
	        CXX_STANDARD 20
	        CXX_STANDARD_REQUIRED ON
	        FOLDER "Engine/Tests"
	    )
	    ge_set_output_tests(AsyncVideoRecorderTests)
	    ge_stage_runtime_dependencies(AsyncVideoRecorderTests)
	    add_test(NAME AsyncVideoRecorderTests COMMAND $<TARGET_FILE:AsyncVideoRecorderTests>)

	    # Closed-loop writer tests inspect the container with ffmpeg directly,
	    # so they link it in addition to the Engine (whose linkage is PRIVATE).
	    if(FFMPEG_FOUND)
	        add_executable(VideoWriterFFmpegTests
	            Tests/VideoWriterFFmpegTests.cpp
	        )
	        target_link_libraries(VideoWriterFFmpegTests PRIVATE Engine GTest::gtest GTest::gtest_main ${FFMPEG_LIBRARIES})
	        target_include_directories(VideoWriterFFmpegTests PRIVATE ${FFMPEG_INCLUDE_DIRS})
	        set_target_properties(VideoWriterFFmpegTests PROPERTIES
	            CXX_STANDARD 20
	            CXX_STANDARD_REQUIRED ON
	            FOLDER "Engine/Tests"
	        )
	        ge_set_output_tests(VideoWriterFFmpegTests)
	        ge_stage_runtime_dependencies(VideoWriterFFmpegTests)
	        add_test(NAME VideoWriterFFmpegTests COMMAND $<TARGET_FILE:VideoWriterFFmpegTests>)
	    endif()
	endif()

if(BUILD_TESTING AND TARGET GTest::gtest_main)
    add_executable(PackagedTextureExportTests Tests/PackagedTextureExportTests.cpp)
    target_link_libraries(PackagedTextureExportTests PRIVATE Engine GTest::gtest_main)
    target_include_directories(PackagedTextureExportTests PRIVATE ${CMAKE_SOURCE_DIR}/Tests)
    target_compile_features(PackagedTextureExportTests PRIVATE cxx_std_20)
    ge_set_output_tests(PackagedTextureExportTests)
    ge_stage_runtime_dependencies(PackagedTextureExportTests)
    add_test(NAME PackagedTextureExportTests COMMAND PackagedTextureExportTests)
endif()

if(BUILD_TESTING AND TARGET GTest::gtest_main)
    add_executable(EngineShaderHookLifetimeTests Tests/EngineShaderHookLifetimeTests.cpp)
    target_link_libraries(EngineShaderHookLifetimeTests PRIVATE Engine GTest::gtest_main)
    target_include_directories(EngineShaderHookLifetimeTests PRIVATE ${CMAKE_SOURCE_DIR}/Tests)
    target_compile_features(EngineShaderHookLifetimeTests PRIVATE cxx_std_20)
    ge_set_output_tests(EngineShaderHookLifetimeTests)
    ge_stage_runtime_dependencies(EngineShaderHookLifetimeTests)
    add_test(NAME EngineShaderHookLifetimeTests COMMAND EngineShaderHookLifetimeTests)
endif()
