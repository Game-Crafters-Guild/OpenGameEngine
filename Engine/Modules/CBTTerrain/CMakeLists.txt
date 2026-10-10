cmake_minimum_required(VERSION 3.20)

project(CBTTerrain LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------
add_library(CBTTerrain OBJECT
    Source/CBTKernelSet.cpp
    Source/CBTResources.cpp
    Source/CBTInstance.cpp
    Source/CBTUpdateNode.cpp
    Source/CBTUpdateRestGate.cpp
    Source/CBTActivityReadback.cpp
    Source/CBTTreeAudit.cpp
    Source/TerrainShadowGrid.cpp
    Source/SphereSculptLayer.cpp
    Source/SphereSculptSerialization.cpp
)

target_compile_features(CBTTerrain PUBLIC cxx_std_20)

target_include_directories(CBTTerrain
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

target_link_libraries(CBTTerrain
    PUBLIC
        GameEngineRendering
        Logger
        Types
        Mathematics
    PRIVATE
        AssetCore
)

# ---------------------------------------------------------------------------
# Shader compilation: cbt_kernels.comp -> ${CMAKE_BINARY_DIR}/Shaders/*.spv
# ---------------------------------------------------------------------------
# int64 storage + spec constants target Vulkan 1.3 / SPIR-V 1.6 (matches the
# engine's device). spirv-val runs explicitly to catch the findMSB(u64) trap
# (plan §9) before the SPIR-V ever reaches a pipeline.
find_package(Vulkan QUIET)
set(_CBT_GLSLC_HINTS "${Vulkan_INCLUDE_DIRS}/../bin")
if(VCPKG_DIR AND VCPKG_ARCH)
    list(APPEND _CBT_GLSLC_HINTS
        "${VCPKG_DIR}/installed/${VCPKG_ARCH}/tools"
        "${VCPKG_DIR}/installed/${VCPKG_ARCH}/tools/shaderc")
endif()
find_program(CBT_GLSLC_EXECUTABLE glslc HINTS ${_CBT_GLSLC_HINTS})
find_program(CBT_SPIRV_VAL_EXECUTABLE spirv-val HINTS ${_CBT_GLSLC_HINTS})

# The sources sit under Shaders/CBT so the repo layout MIRRORS the staged layout
# (Assets/Shaders/CBT): material documents name their surface as 'CBT/cbt_surface.glsl',
# and every resolver — runtime and offline cook alike — joins a root with that whole
# path. A flat Shaders/ resolved at runtime only because staging reshaped it.
set(CBT_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/CBT)
set(CBT_SHADER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/Shaders)
file(MAKE_DIRECTORY ${CBT_SHADER_OUTPUT_DIR})

if(CBT_GLSLC_EXECUTABLE)
    set(CBT_KERNELS_SRC ${CBT_SHADER_DIR}/cbt_kernels.comp)
    set(CBT_KERNELS_SPV ${CBT_SHADER_OUTPUT_DIR}/cbt_kernels.comp.spv)
    set(CBT_KERNELS_INCLUDES ${CBT_SHADER_DIR}/cbt_layout.glsl ${CBT_SHADER_DIR}/cbt_domain.glsl
        ${CBT_SHADER_DIR}/cbt_atlas.glsl ${CBT_SHADER_DIR}/cbt_sculpt.glsl
        ${CBT_SHADER_DIR}/cbt_analytic.glsl ${CBT_SHADER_DIR}/cbt_deep_decode.glsl
        ${CBT_SHADER_DIR}/cbt_page.glsl)

    set(_CBT_COMPILE_COMMANDS
        COMMAND ${CBT_GLSLC_EXECUTABLE} --target-env=vulkan1.3 --target-spv=spv1.6
                -I ${CBT_SHADER_DIR} ${CBT_KERNELS_SRC} -o ${CBT_KERNELS_SPV})
    if(CBT_SPIRV_VAL_EXECUTABLE)
        list(APPEND _CBT_COMPILE_COMMANDS
            COMMAND ${CBT_SPIRV_VAL_EXECUTABLE} --target-env vulkan1.3 ${CBT_KERNELS_SPV})
        message(STATUS "CBTTerrain: spirv-val enabled (${CBT_SPIRV_VAL_EXECUTABLE})")
    else()
        message(STATUS "CBTTerrain: spirv-val not found; relying on glslc validation")
    endif()

    add_custom_command(
        OUTPUT ${CBT_KERNELS_SPV}
        ${_CBT_COMPILE_COMMANDS}
        DEPENDS ${CBT_KERNELS_SRC} ${CBT_KERNELS_INCLUDES}
        COMMENT "Compiling + validating CBT compute kernels: cbt_kernels.comp"
        VERBATIM
    )
    # The narrow-heap arm (GE_CBT_HEAP32) as a desktop Vulkan blob, spec-constant kernel
    # select intact. Nothing ships it: it exists so the golden decode oracles can run the
    # SAME arm the web cook compiles to WGSL — before this, that arm never met any test.
    set(CBT_KERNELS_HEAP32_SPV ${CBT_SHADER_OUTPUT_DIR}/cbt_kernels_heap32.comp.spv)
    set(_CBT_COMPILE_HEAP32_COMMANDS
        COMMAND ${CBT_GLSLC_EXECUTABLE} --target-env=vulkan1.3 --target-spv=spv1.6
                -DGE_CBT_HEAP32 -I ${CBT_SHADER_DIR} ${CBT_KERNELS_SRC}
                -o ${CBT_KERNELS_HEAP32_SPV})
    if(CBT_SPIRV_VAL_EXECUTABLE)
        list(APPEND _CBT_COMPILE_HEAP32_COMMANDS
            COMMAND ${CBT_SPIRV_VAL_EXECUTABLE} --target-env vulkan1.3 ${CBT_KERNELS_HEAP32_SPV})
    endif()
    add_custom_command(
        OUTPUT ${CBT_KERNELS_HEAP32_SPV}
        ${_CBT_COMPILE_HEAP32_COMMANDS}
        DEPENDS ${CBT_KERNELS_SRC} ${CBT_KERNELS_INCLUDES}
        COMMENT "Compiling + validating CBT compute kernels (heap32 arm): cbt_kernels.comp"
        VERBATIM
    )
    add_custom_target(CBTCompileShaders ALL DEPENDS ${CBT_KERNELS_SPV} ${CBT_KERNELS_HEAP32_SPV})
    add_dependencies(CBTTerrain CBTCompileShaders)
    # The CBT terrain declare tests stage these kernels next to their executable (Engine/CMakeLists.txt).
    if(TARGET RenderPipelineDeclareTests)
        add_dependencies(RenderPipelineDeclareTests CBTCompileShaders)
    endif()
else()
    message(WARNING "CBTTerrain: glslc not found - CBT kernels will not be compiled")
endif()

# ---------------------------------------------------------------------------
# Tests (real Vulkan device)
# ---------------------------------------------------------------------------
if(BUILD_TESTING)
    find_package(GTest CONFIG QUIET)
    if(NOT GTest_FOUND AND TARGET GTest::gtest)
        set(GTest_FOUND TRUE)
    endif()
    if(GTest_FOUND OR TARGET GTest::gtest)
        # Marked blocks of the shipped terrain shaders are EXTRACTED and compiled into this suite as
        # C++ (Tests/GlslShim.h), so the tests execute the shipped shader code rather than a copy of
        # it that can drift. A missing or reordered marker is a hard error in the extraction step:
        # extracting nothing would leave the suite green with no shader in it.
        # Extracted from the SHARED albedo include rather than from cbt_surface.glsl: the ground
        # surface and the grass surface both include it, so this is the one copy either of them
        # executes — the same reason the blend selection below is extracted from its include.
        set(TERRAIN_MATERIAL_ALBEDO_GLSL
            ${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Includes/terrain_material_albedo.glsl)
        set(CBT_HEX_LATTICE_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/CBTHexLatticeExtracted.h)
        add_custom_command(
            OUTPUT ${CBT_HEX_LATTICE_HEADER}
            COMMAND ${CMAKE_COMMAND} -DSRC=${TERRAIN_MATERIAL_ALBEDO_GLSL}
                                     -DOUT=${CBT_HEX_LATTICE_HEADER}
                                     "-DBEGIN_MARKER=// GE_SHARED_HEX_LATTICE_BEGIN"
                                     "-DEND_MARKER=// GE_SHARED_HEX_LATTICE_END"
                                     -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ExtractShaderBlock.cmake
            DEPENDS ${TERRAIN_MATERIAL_ALBEDO_GLSL}
                    ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ExtractShaderBlock.cmake
            COMMENT "Extracting the shared hex lattice from terrain_material_albedo.glsl"
            VERBATIM)

        # The per-fragment material selection: the weight floor, the duplicate-role fold and the
        # top-K rank + renormalize. Pure weight arithmetic with no samplers, so it runs on the host.
        #
        # Extracted from the SHARED include rather than from cbt_surface.glsl: the ground surface and
        # the grass surface both include it, so this is the one copy either of them executes.
        set(TERRAIN_BLEND_RESOLVE_GLSL
            ${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Includes/terrain_blend_resolve.glsl)
        set(CBT_BLEND_SELECTION_HEADER
            ${CMAKE_CURRENT_BINARY_DIR}/generated/CBTBlendSelectionExtracted.h)
        add_custom_command(
            OUTPUT ${CBT_BLEND_SELECTION_HEADER}
            COMMAND ${CMAKE_COMMAND} -DSRC=${TERRAIN_BLEND_RESOLVE_GLSL}
                                     -DOUT=${CBT_BLEND_SELECTION_HEADER}
                                     "-DBEGIN_MARKER=// GE_SHARED_BLEND_SELECTION_BEGIN"
                                     "-DEND_MARKER=// GE_SHARED_BLEND_SELECTION_END"
                                     -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ExtractShaderBlock.cmake
            DEPENDS ${TERRAIN_BLEND_RESOLVE_GLSL}
                    ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ExtractShaderBlock.cmake
            COMMENT "Extracting the blend selection from terrain_blend_resolve.glsl"
            VERBATIM)

        # The tangent-normal decode and the per-projection strength application — the two halves of
        # the terrain normal SIGN, which nothing but a runtime A/B could check before this. The
        # texture tap deliberately stays outside both blocks, which is what leaves them pure enough
        # to run on the host.
        set(SURFACE_IO_GLSL
            ${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Includes/surface_io.glsl)
        set(CBT_TANGENT_DECODE_HEADER
            ${CMAKE_CURRENT_BINARY_DIR}/generated/CBTTangentNormalDecodeExtracted.h)
        add_custom_command(
            OUTPUT ${CBT_TANGENT_DECODE_HEADER}
            COMMAND ${CMAKE_COMMAND} -DSRC=${SURFACE_IO_GLSL}
                                     -DOUT=${CBT_TANGENT_DECODE_HEADER}
                                     "-DBEGIN_MARKER=// GE_SHARED_DECODE_TANGENT_NORMAL_BEGIN"
                                     "-DEND_MARKER=// GE_SHARED_DECODE_TANGENT_NORMAL_END"
                                     -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ExtractShaderBlock.cmake
            DEPENDS ${SURFACE_IO_GLSL}
                    ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ExtractShaderBlock.cmake
            COMMENT "Extracting the tangent normal decode from surface_io.glsl"
            VERBATIM)

        set(CBT_PROJ_NORMAL_HEADER
            ${CMAKE_CURRENT_BINARY_DIR}/generated/CBTProjNormalFromSampleExtracted.h)
        add_custom_command(
            OUTPUT ${CBT_PROJ_NORMAL_HEADER}
            COMMAND ${CMAKE_COMMAND} -DSRC=${CBT_SHADER_DIR}/cbt_surface.glsl
                                     -DOUT=${CBT_PROJ_NORMAL_HEADER}
                                     "-DBEGIN_MARKER=// GE_SHARED_PROJ_NORMAL_FROM_SAMPLE_BEGIN"
                                     "-DEND_MARKER=// GE_SHARED_PROJ_NORMAL_FROM_SAMPLE_END"
                                     -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ExtractShaderBlock.cmake
            DEPENDS ${CBT_SHADER_DIR}/cbt_surface.glsl
                    ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ExtractShaderBlock.cmake
            COMMENT "Extracting the projection normal decode from cbt_surface.glsl"
            VERBATIM)

        # The terrain's sun-space clearance map: the shared block the bake kernel and both shading
        # callers compile (the scan step, the per-sample caster, the receiver's lookup).
        set(TERRAIN_SHADOW_GLSL
            ${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Includes/terrain_shadow.glsl)
        set(CBT_TERRAIN_SHADOW_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/TerrainShadowExtracted.h)
        add_custom_command(
            OUTPUT ${CBT_TERRAIN_SHADOW_HEADER}
            COMMAND ${CMAKE_COMMAND} -DSRC=${TERRAIN_SHADOW_GLSL}
                                     -DOUT=${CBT_TERRAIN_SHADOW_HEADER}
                                     "-DBEGIN_MARKER=// GE_SHARED_TERRAIN_SHADOW_BEGIN"
                                     "-DEND_MARKER=// GE_SHARED_TERRAIN_SHADOW_END"
                                     -P ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ExtractShaderBlock.cmake
            DEPENDS ${TERRAIN_SHADOW_GLSL}
                    ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ExtractShaderBlock.cmake
            COMMENT "Extracting the terrain shadow block from terrain_shadow.glsl"
            VERBATIM)

        add_executable(CBTTerrainTests
            ${CBT_TERRAIN_SHADOW_HEADER}
            ${CBT_HEX_LATTICE_HEADER}
            ${CBT_BLEND_SELECTION_HEADER}
            ${CBT_TANGENT_DECODE_HEADER}
            ${CBT_PROJ_NORMAL_HEADER}
            Tests/CBTMaterialBlendTests.cpp
            Tests/CBTProjNormalSignTests.cpp
            Tests/CBTLayoutTests.cpp
            Tests/CBTNearBiasTests.cpp
            Tests/CBTScreenTargetTests.cpp
            Tests/CBTResourceTests.cpp
            Tests/CBTKernelSetTests.cpp
            Tests/CBTKernelWideVectorTests.cpp
            Tests/CBTInstanceTests.cpp
            Tests/CBTUpdateNodeTests.cpp
            Tests/CBTUpdateRestGateTests.cpp
            Tests/CBTScreenSpaceTests.cpp
            Tests/CBTHexTilingTests.cpp
            Tests/CBTDecodeGoldenTests.cpp
            Tests/CBTDeepDecodeTests.cpp
            Tests/CBTSphereRootsTests.cpp
            Tests/CBTSphereDomainTests.cpp
            Tests/CBTScaleAuditTests.cpp
            Tests/CBTDemandAuditTests.cpp
            Tests/CBTSphereFaceMapTests.cpp
            Tests/SphereSculptLayerTests.cpp
            Tests/SphereSculptSerializationTests.cpp
            Tests/SphereSculptShapeAccuracyTests.cpp
            Tests/SphereSculptEscalationTests.cpp
            Tests/SphereSculptUndoTests.cpp
            Tests/CBTPlanetShadingTests.cpp
            Tests/CBTSphereSculptReEvalTests.cpp
            Tests/CBTEditDrivenRetessTests.cpp
            Tests/CBTFarFieldDrainProbeTests.cpp
            Tests/CBTFarFieldReseedTests.cpp
            Tests/CBTLookBackPriorityTests.cpp
            Tests/CBTPlanarDemandGateTests.cpp
            Tests/CBTBringUpDrainTests.cpp
            Tests/CBTPoolHealthTests.cpp
            Tests/CBTPlanarScaleAuditTests.cpp
            Tests/CBTNearFieldMergeTests.cpp
            Tests/CBTParentMergeMetricTests.cpp
            Tests/CBTWatertightnessTests.cpp
            Tests/CBTContentAwareSplitTests.cpp
            Tests/TerrainShadowMapTests.cpp
        )
        set_target_properties(CBTTerrainTests PROPERTIES FOLDER "CBTTerrain/Tests")
        if(MSVC)
            # Silence C6326 (/analyze) "constant compared with constant" noise from
            # GTest EXPECT_EQ on literal expectations (same as RenderGraphTests).
            target_compile_options(CBTTerrainTests PRIVATE /wd6326)
        endif()
        target_link_libraries(CBTTerrainTests PRIVATE
            CBTTerrain
            GameEngineRendering
            # HeightfieldData: the CPU height field the rendered corners are held to.
            Terrain
            FileSystemObjects
            AssetCore
            RenderGraphTestSupport
            GTest::gtest
            GTest::gtest_main
        )
        # OBJECT->OBJECT object files are not spliced transitively on the VS
        # generator: splice Logger in directly (the Rendering tests do the same)
        # and provide the stb_image impl that SkyRenderer pulls into
        # GameEngineRendering but does not export.
        target_sources(CBTTerrainTests PRIVATE $<TARGET_OBJECTS:Logger>)
        target_sources(CBTTerrainTests PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/stb_image_impl.cpp)
        target_compile_definitions(CBTTerrainTests PRIVATE GE_HAVE_STB)
        if(TARGET stb::stb)
            target_link_libraries(CBTTerrainTests PRIVATE stb::stb)
        endif()
        target_compile_definitions(CBTTerrainTests PRIVATE
            CBT_SHADER_OUTPUT_DIR="${CBT_SHADER_OUTPUT_DIR}"
            CBT_SHADER_SOURCE_DIR="${CBT_SHADER_DIR}"
            # TerrainParamsEntry is re-declared in four shaders against one std430
            # buffer; the grass three live in another module, so the drift lock needs
            # to read their source too.
            TERRAIN_GRASS_SHADER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/Engine/Modules/TerrainGrass/Shaders/TerrainGrass"
            # Scanned to rediscover that set, so a mirror added in any module reds the
            # completeness lock instead of going unguarded.
            GE_ENGINE_MODULES_DIR="${CMAKE_SOURCE_DIR}/Engine/Modules"
            # The shipped blend width and the resolve it parameterizes both live here, shared by
            # the ground and grass surfaces — so the width pin reads this rather than a surface.
            RENDERING_SHADER_INCLUDES_DIR="${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Includes")
        target_include_directories(CBTTerrainTests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_CURRENT_BINARY_DIR}/generated
            # TerrainGPUParams — the C++ twin those GLSL mirrors describe.
            ${CMAKE_SOURCE_DIR}/Engine/Modules/Terrain/Include)
        ge_set_output_tests(CBTTerrainTests)
        ge_stage_runtime_dependencies(CBTTerrainTests)
        if(TARGET CBTCompileShaders)
            add_dependencies(CBTTerrainTests CBTCompileShaders)
        endif()
        add_test(NAME CBTTerrainTests COMMAND CBTTerrainTests)
    endif()
endif()
