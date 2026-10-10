cmake_minimum_required(VERSION 3.20)

project(TerrainGrass LANGUAGES CXX)

add_library(TerrainGrass OBJECT
    Include/TerrainGrass/GrassPlacementElisionInputs.h
    Include/TerrainGrass/GrassPlacementModel.h
    Include/TerrainGrass/TerrainGrassClamps.h
    Include/TerrainGrass/TerrainGrassPlacementStats.h
    Include/TerrainGrass/TerrainGrassRenderFeature.h
    Include/TerrainGrass/TerrainGrassRenderNode.h
    Include/TerrainGrass/RegisterTerrainGrass.h
    Include/TerrainGrass/Scene/TerrainGrassSceneSchemas.h
    Source/GrassPlacementElisionInputs.cpp
    Source/TerrainGrassComponents.cpp
    Source/TerrainGrassPlacementStats.cpp
    Source/TerrainGrassRenderFeature.cpp
    Source/TerrainGrassRenderNode.cpp
    Source/RegisterTerrainGrass.cpp
    Source/Scene/TerrainGrassSceneSchemas.cpp
)

target_compile_features(TerrainGrass PUBLIC cxx_std_20)

target_include_directories(TerrainGrass
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

target_link_libraries(TerrainGrass
    PUBLIC
        TerrainECS
        GameEngineRendering
        Logger
    PRIVATE
        Terrain
)

target_include_directories(TerrainGrass PRIVATE
    $<TARGET_PROPERTY:Engine,INTERFACE_INCLUDE_DIRECTORIES>
)

if(MSVC)
    target_compile_options(TerrainGrass PRIVATE /W4 /bigobj)
else()
    target_compile_options(TerrainGrass PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(BUILD_TESTING)
    # This module is added BEFORE the Rendering module and the root test block, so no
    # find_package(GTest) has run yet in any enclosing scope: without its own find here the
    # guard below is silently false and the suite never generates (the CBTTerrain test block
    # does the same normalize for the same reason).
    find_package(GTest CONFIG QUIET)
    if(NOT GTest_FOUND AND TARGET GTest::gtest)
        set(GTest_FOUND TRUE)
    endif()
    if(GTest_FOUND OR TARGET GTest::gtest)
        # Compiles TerrainGrassPlacementStats.cpp DIRECTLY rather than linking the
        # TerrainGrass object library: the stats class deliberately depends on
        # nothing above Rendering, so the suite needs a device and not the render
        # feature's whole TerrainECS/RenderServices closure.
        # The ground splat SOURCE SELECTION is lifted verbatim out of the shipped shared include and
        # compiled into this suite as C++ (CBTTerrain/Tests/GlslShim.h), so the tests execute the
        # shader rather than a copy of it that can drift. A missing or reordered marker is a hard
        # error in the extraction step: extracting nothing would leave the suite green with no
        # shader in it. The extractor and the shim live in the CBTTerrain test tree, where this
        # pattern started; both are generic and this is their second consumer.
        set(GRASS_ATLAS_SPLAT_GLSL
            ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/TerrainGrass/grass_atlas_splat.glsl)
        set(GRASS_GLSL_SHIM_DIR ${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Tests)
        set(GRASS_SPLAT_SOURCE_HEADER
            ${CMAKE_CURRENT_BINARY_DIR}/generated/GrassAtlasSplatSourceExtracted.h)
        add_custom_command(
            OUTPUT ${GRASS_SPLAT_SOURCE_HEADER}
            COMMAND ${CMAKE_COMMAND} -DSRC=${GRASS_ATLAS_SPLAT_GLSL}
                                     -DOUT=${GRASS_SPLAT_SOURCE_HEADER}
                                     "-DBEGIN_MARKER=// GE_SHARED_GRASS_SPLAT_SOURCE_BEGIN"
                                     "-DEND_MARKER=// GE_SHARED_GRASS_SPLAT_SOURCE_END"
                                     -P ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            DEPENDS ${GRASS_ATLAS_SPLAT_GLSL}
                    ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            COMMENT "Extracting the ground splat source selection from grass_atlas_splat.glsl"
            VERBATIM)

        # Same seam for the wind seed's noise-domain offset, so the suite EXECUTES the shipped
        # expression rather than restating it: a restatement could not tell whether seed 0 is still
        # the identity that the "unseeded content is unchanged" claim rests on.
        set(GRASS_VERTEX_MODIFIER_GLSL
            ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/TerrainGrass/terrain_grass_vertex_modifier.glsl)
        set(GRASS_WIND_SEED_HEADER
            ${CMAKE_CURRENT_BINARY_DIR}/generated/GrassWindSeedOffsetExtracted.h)
        add_custom_command(
            OUTPUT ${GRASS_WIND_SEED_HEADER}
            COMMAND ${CMAKE_COMMAND} -DSRC=${GRASS_VERTEX_MODIFIER_GLSL}
                                     -DOUT=${GRASS_WIND_SEED_HEADER}
                                     "-DBEGIN_MARKER=// GE_SHARED_WIND_SEED_OFFSET_BEGIN"
                                     "-DEND_MARKER=// GE_SHARED_WIND_SEED_OFFSET_END"
                                     -P ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            DEPENDS ${GRASS_VERTEX_MODIFIER_GLSL}
                    ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            COMMENT "Extracting the wind seed offset from terrain_grass_vertex_modifier.glsl"
            VERBATIM)

        # Same seam again for the scatter-gain deficit, so the BOUND that keeps that term from
        # handing back more brightness than the settle cost is pinned by the shipped expression.
        # GrassScatterBoundTests executes this; deleting the min() cap in the shader reds it.
        set(GRASS_SURFACE_GLSL
            ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/TerrainGrass/terrain_grass_surface.glsl)
        set(GRASS_SCATTER_DEFICIT_HEADER
            ${CMAKE_CURRENT_BINARY_DIR}/generated/GrassScatterDeficitExtracted.h)
        add_custom_command(
            OUTPUT ${GRASS_SCATTER_DEFICIT_HEADER}
            COMMAND ${CMAKE_COMMAND} -DSRC=${GRASS_SURFACE_GLSL}
                                     -DOUT=${GRASS_SCATTER_DEFICIT_HEADER}
                                     "-DBEGIN_MARKER=// GE_SHARED_GRASS_SCATTER_DEFICIT_BEGIN"
                                     "-DEND_MARKER=// GE_SHARED_GRASS_SCATTER_DEFICIT_END"
                                     -P ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            DEPENDS ${GRASS_SURFACE_GLSL}
                    ${GRASS_GLSL_SHIM_DIR}/ExtractShaderBlock.cmake
            COMMENT "Extracting the scatter-gain deficit from terrain_grass_surface.glsl"
            VERBATIM)

        add_executable(TerrainGrassStatsTests
            ${GRASS_SPLAT_SOURCE_HEADER}
            ${GRASS_WIND_SEED_HEADER}
            ${GRASS_SCATTER_DEFICIT_HEADER}
            Tests/TerrainGrassPlacementStatsTests.cpp
            Tests/GrassPlacementModelTests.cpp
            Tests/GrassPlacementElisionTests.cpp
            Source/GrassPlacementElisionInputs.cpp
            Tests/GrassAtlasSplatResolveTests.cpp
            Tests/GrassDrawModeTests.cpp
            Tests/GrassWindSeedTests.cpp
            Tests/GrassScatterBoundTests.cpp
            Source/TerrainGrassPlacementStats.cpp
        )
        set_target_properties(TerrainGrassStatsTests PROPERTIES FOLDER "TerrainGrass/Tests")
        target_link_libraries(TerrainGrassStatsTests PRIVATE
            GameEngineRendering
            FileSystemObjects
            GTest::gtest
            GTest::gtest_main
        )
        # OBJECT->OBJECT object files are not spliced transitively on the VS
        # generator: splice Logger in directly (the CBT and Rendering suites do the
        # same) and provide the stb_image impl GameEngineRendering pulls in but
        # does not export.
        target_sources(TerrainGrassStatsTests PRIVATE $<TARGET_OBJECTS:Logger>)
        target_sources(TerrainGrassStatsTests PRIVATE
            ${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/stb_image_impl.cpp)
        target_compile_definitions(TerrainGrassStatsTests PRIVATE GE_HAVE_STB)
        # GrassShaderSitePurity reads the shipped grass GLSL and fails on any seed path
        # that draws from the per-frame instance slot instead of the blade's site.
        target_compile_definitions(TerrainGrassStatsTests PRIVATE
            TERRAIN_GRASS_SHADER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Shaders/TerrainGrass")
        if(TARGET stb::stb)
            target_link_libraries(TerrainGrassStatsTests PRIVATE stb::stb)
        endif()
        target_include_directories(TerrainGrassStatsTests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Include
            # GlslShim.h, the extracted-block header, and Terrain/TerrainTypes.h — the last so the
            # shader's mirror of kTerrainFlagAtlasBacked is asserted against the C++ constant
            # rather than against a literal.
            ${GRASS_GLSL_SHIM_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}/generated
            ${CMAKE_SOURCE_DIR}/Engine/Modules/Terrain/Include)
        ge_set_output_tests(TerrainGrassStatsTests)
        ge_stage_runtime_dependencies(TerrainGrassStatsTests)
        add_test(NAME TerrainGrassStatsTests COMMAND TerrainGrassStatsTests)
    endif()
endif()
