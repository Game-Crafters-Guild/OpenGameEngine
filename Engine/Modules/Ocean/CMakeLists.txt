cmake_minimum_required(VERSION 3.20)

project(Ocean LANGUAGES CXX)

add_library(Ocean OBJECT
    Source/OceanRenderFeature.cpp
    Source/OceanFootprintFrustum.cpp
    Source/OceanSimulationDemand.cpp
    Source/OceanFieldRanges.cpp
    Source/OceanShaderProgram.cpp
    Source/OceanFFT.cpp
    Source/OceanCascadeArray.cpp
    Source/OceanSprayGPU.cpp
    Source/OceanWaterMaterial.cpp
    Source/OceanSplineRaster.cpp
    Source/OceanShadowSim.cpp
    Source/OceanFoamSim.cpp
    Source/OceanCombineSim.cpp
    Source/OceanHeightField.cpp
    Source/OceanGPUQuery.cpp
    Source/OceanCollisionProvider.cpp
    Source/OceanFFTCollisionAsset.cpp
    Source/OceanRayTrace.cpp
    Source/OceanInputDrawSource.cpp
    Source/OceanTime.cpp
    Source/OceanSettingsAsset.cpp
    Source/OceanPresetAsset.cpp
    Source/OceanPresetCache.cpp
    Source/OceanPresetOverrideValidator.cpp
    Source/OceanValidation.cpp
    Source/OceanDepthCacheAsset.cpp
    Source/OceanSeabedDepth.cpp
    Source/OceanSeabedVisibility.cpp
    Source/OceanFlowSim.cpp
    Source/OceanWaveMaskSim.cpp
    Source/OceanDynWaves.cpp
    Source/OceanDynWavesReadback.cpp
    Source/OceanClipSim.cpp
    Source/OceanAlbedoSim.cpp
    Source/OceanCaustics.cpp
    Source/OceanForwardContributor.cpp
    Source/OceanSurfaceMaterial.cpp
    Source/OceanSceneGrab.cpp
    Source/OceanPlanarReflection.cpp
    Source/OceanRasterDepthCapture.cpp
    Source/OceanUnderwater.cpp
    Source/OceanLightShafts.cpp
    Source/OceanRenderNode.cpp
    Source/OceanShaderDirectory.cpp
    Source/Systems/OceanExtractionSystem.cpp
    Source/Systems/OceanBuoyancySystem.cpp
    Source/Systems/RegisterOceanSystems.cpp
)

target_compile_features(Ocean PUBLIC cxx_std_20)

target_include_directories(Ocean
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

target_link_libraries(Ocean
    PUBLIC
        ECS
        ECSComponents
        Types
        Mathematics
        Logger
        GameEngineRendering
    PRIVATE
        # Engine intentionally NOT listed: Engine is SHARED and PRIVATE-links
        # Ocean itself. A direct Ocean -> Engine link would create a
        # SHARED<->STATIC cycle. Engine's symbols (RenderServices, Material,
        # IRenderFeature, etc.) resolve when Engine links Ocean.lib. Headers
        # come in via the generator expression below.
        JobSystem
        AssetCore
        Physics
        PhysicsECS
        Spline
        SplineECS
)

# Engine's public headers are needed by Ocean's implementation. We can't link
# Engine (cycle), so reach into its INTERFACE include directories directly.
target_include_directories(Ocean PRIVATE
    $<TARGET_PROPERTY:Engine,INTERFACE_INCLUDE_DIRECTORIES>
)

if(MSVC)
    target_compile_options(Ocean PRIVATE /W4 /bigobj)
else()
    target_compile_options(Ocean PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
