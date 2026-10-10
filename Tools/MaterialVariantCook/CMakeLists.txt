# MaterialVariantCook — offline compilation of a project's material shader
# variants into a relocatable shader cache (see MaterialVariantCook.cpp).
# Desktop-only by construction: it exists to produce what a compiler-less
# runtime consumes.
add_executable(MaterialVariantCook
    MaterialVariantCook.cpp
    WebWgslCook.cpp
    ${CMAKE_SOURCE_DIR}/Packages/eztree/Native/EZTreeECS/EZTreeRuntimeMaterials.cpp
)

set_target_properties(MaterialVariantCook PROPERTIES FOLDER "Tools")
target_compile_features(MaterialVariantCook PRIVATE cxx_std_20)
target_include_directories(MaterialVariantCook PRIVATE
    ${CMAKE_SOURCE_DIR}/Packages/eztree/Native)
target_link_libraries(MaterialVariantCook PRIVATE Engine)

# Output layout: bin/<Config>/Tools
ge_set_output_tools(MaterialVariantCook)
ge_stage_runtime_dependencies(MaterialVariantCook)
ge_stage_shader_graph_nodes(MaterialVariantCook)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    add_executable(MaterialVariantCookIntegrationTests MaterialVariantCookIntegrationTests.cpp)
    target_compile_features(MaterialVariantCookIntegrationTests PRIVATE cxx_std_20)
    target_include_directories(MaterialVariantCookIntegrationTests PRIVATE
        ${CMAKE_SOURCE_DIR}/Tests ${CMAKE_SOURCE_DIR}/Engine/Tests)
    # GE_RENDERER_REPO_ROOT is dev-only source anchoring for the web leg: it runs
    # Tools/ShaderCook/shadercook.py in place, because setup.py provisions naga and
    # tint beside that script and a staged copy would find neither.
    target_compile_definitions(MaterialVariantCookIntegrationTests PRIVATE
        GE_MATERIAL_VARIANT_COOK_FILENAME="$<TARGET_FILE_NAME:MaterialVariantCook>"
        GE_RENDERER_REPO_ROOT="${CMAKE_SOURCE_DIR}")
    target_link_libraries(MaterialVariantCookIntegrationTests PRIVATE Engine GTest::gtest_main)
    ge_set_output_tests(MaterialVariantCookIntegrationTests)
    ge_stage_runtime_dependencies(MaterialVariantCookIntegrationTests)
    ge_stage_shader_graph_nodes(MaterialVariantCookIntegrationTests)
    # CollectTargets always adds cbt-terrain; web cooks also add grass and tree materials.
    foreach(shader_root IN ITEMS
        "Engine/Modules/CBTTerrain/Shaders"
        "Engine/Modules/TerrainGrass/Shaders"
        "Packages/eztree/Assets/Shaders")
        file(GLOB_RECURSE material_cook_package_shaders CONFIGURE_DEPENDS
            "${CMAKE_SOURCE_DIR}/${shader_root}/*.glsl")
        foreach(shader IN LISTS material_cook_package_shaders)
            file(RELATIVE_PATH relative_shader "${CMAKE_SOURCE_DIR}/${shader_root}" "${shader}")
            ge_stage_test_asset(MaterialVariantCookIntegrationTests "${shader}"
                "Fixtures/MaterialVariantCook/PackageShaders/${relative_shader}")
        endforeach()
    endforeach()
    add_dependencies(MaterialVariantCookIntegrationTests MaterialVariantCook StageTestAssets)
    add_test(NAME MaterialVariantCookIntegrationTests COMMAND MaterialVariantCookIntegrationTests)
    # Four complete cooks, the web one translating every built-in target through
    # naga and tint: on a loaded host the suite takes about 28 minutes, past
    # ctest's 1500 s default. 3600 s also covers the web cook's 45-minute limit.
    set_tests_properties(MaterialVariantCookIntegrationTests PROPERTIES TIMEOUT 3600)
    add_executable(MaterialCookVariantRequestTests VariantRequestsTests.cpp)
    target_compile_features(MaterialCookVariantRequestTests PRIVATE cxx_std_20)
    target_include_directories(MaterialCookVariantRequestTests PRIVATE
        ${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Include
        ${CMAKE_SOURCE_DIR}/Engine/Modules/Types/Include)
    target_link_libraries(MaterialCookVariantRequestTests PRIVATE GTest::gtest_main)
    ge_set_output_tests(MaterialCookVariantRequestTests)
    ge_stage_runtime_dependencies(MaterialCookVariantRequestTests)
    add_test(NAME MaterialCookVariantRequestTests COMMAND MaterialCookVariantRequestTests)
endif()
