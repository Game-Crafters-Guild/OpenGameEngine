cmake_minimum_required(VERSION 3.20)

project(CBTTerrainECS LANGUAGES CXX)

# Engine-integration layer for the CBT terrain renderer: the render feature (owns
# the GPU CBT instance + material), the per-view pipeline node, the ECS bridge
# system, and the terrain provisioning helpers. Kept separate from the low-level CBTTerrain
# module (which links only GameEngineRendering and stays headless-testable) because
# these pieces need Engine's RenderServices/Material/DrawCommand — exactly the
# Terrain (headless) vs TerrainECS (Engine-integrated) split.
add_library(CBTTerrainECS OBJECT
    Source/CBTRenderFeature.cpp
    Source/CBTTerrainMaterial.cpp
    Source/CBTRenderNode.cpp
    Source/TerrainShadowBake.cpp
    Source/CBTTreeSeed.cpp
    Source/CBTValidationReadback.cpp
    Source/TerrainProvisioning.cpp
    Source/Systems/CBTUpdateSystem.cpp
    Source/Systems/RegisterCBTSystems.cpp
)

target_compile_features(CBTTerrainECS PUBLIC cxx_std_20)

target_include_directories(CBTTerrainECS
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

target_link_libraries(CBTTerrainECS
    PUBLIC
        CBTTerrain
        ECS
        ECSComponents
        Types
        Mathematics
        Logger
        GameEngineRendering
        AssetCore
    PRIVATE
        # Engine intentionally NOT listed: Engine is SHARED and PRIVATE-links this
        # module, so a direct link would be a SHARED<->STATIC cycle. Engine's
        # symbols resolve when Engine.dll links CBTTerrainECS.lib; headers come in
        # via the generator expression below (the TerrainECS pattern).
        JobSystem
)

# Engine's public headers (RenderServices.h, Material.h, DrawCommand.h,
# Core/Application.h) plus TerrainECS's public headers (TerrainRenderFeature.h — C4
# consumes the same heightmap/world-size the CDLOD path renders) and Ocean's
# (OceanRenderFeature.h — the water plane Classify keeps the seabed coarse under is the
# sea level the ocean surface draws at) are needed by the implementation. Headers only
# (no link): all three modules link into Engine.dll, so their symbols resolve there (the
# established Engine pattern above).
target_include_directories(CBTTerrainECS PRIVATE
    $<TARGET_PROPERTY:Engine,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:TerrainECS,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:Ocean,INTERFACE_INCLUDE_DIRECTORIES>
)

# Inherit Engine's compile definitions (GE_ENABLE_CPU_PROFILING) without a link
# dependency, so Terrain.SphereDab and friends record and ScopedCpuProfile has a
# single consistent definition across TUs.
target_compile_definitions(CBTTerrainECS PRIVATE
    $<TARGET_PROPERTY:Engine,INTERFACE_COMPILE_DEFINITIONS>
)
