# EditorSDK phase 1 tests — the editor extension ABI native Editor-kind
# package modules link against. Only exists when the Editor (and with it the
# EditorSDK target) is being built.

if(NOT TARGET EditorSDK)
    return()
endif()

find_package(GTest REQUIRED)

# ---------------------------------------------------------------------------
# Registry semantics: replace-forward hot-reload, late-plugin inspector
# replay, traits/pick-provider registration, selection-mask dispatch, and the
# phase-2 panel registry (descriptor round-trip, consumer replay, opener).
# EditorVcsFirstDetectGateTests exercises the editor's VCS service gate
# (first-detect deferral until package providers finish loading), so the
# service cpp compiles into this target directly.
# ---------------------------------------------------------------------------
add_executable(EditorSdkRegistryTests
    EditorSdkRegistryTests.cpp
    EditorSdkPanelRegistryTests.cpp
    EditorSdkMenuRegistryTests.cpp
    EditorSdkSceneCommandsTests.cpp
    EditorSdkSettingsRegistryTests.cpp
    # Link the shared SDK registry, as the Build panel and package modules do.
    BuildExporterRegistryTests.cpp
    DebugRequestGateRegistryTests.cpp
    DirectionalShadowSettingsPageTests.cpp
    ${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/Settings/DirectionalShadowSettingsPage.cpp
    # Project LOD settings: the editor-side WRITE into ProjectSettings.json.
    # Engine owns the shape and the read side (Engine/Tests). The writer lives in
    # the Editor target, so the cpp compiles in here directly, like the VCS
    # service below.
    LodProjectSettingsTests.cpp
    ${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/Settings/LodProjectSettingsWriter.cpp
    # The LOD page's registration shape -- the rows a user actually sees. Same
    # arrangement: the registrar lives in the Editor target, so its cpp compiles
    # in here directly.
    LodSettingsPageTests.cpp
    ${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/Settings/LodSettingsPage.cpp
    # Game UI scale policy: the editor-side write and the Game UI Scaling page,
    # which SettingsPageOpenIsNotAnEditTests also seeds. Same arrangement.
    UIScaleProjectSettingsWriterTests.cpp
    ${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/Settings/UIScaleProjectSettingsWriter.cpp
    ${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/Settings/GameUIScaleSettingsPage.cpp
    # Node Graph page: same registration-shape gate. GraphCanvas/GraphPanel
    # statics are stubbed; GraphNodeRegistry + NodeColorSettings compile in so
    # the type-color rows exist without the canvas TU.
    GraphSettingsPageTests.cpp
    GraphSettingsPageTestStubs.cpp
    ${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/Settings/GraphSettingsPage.cpp
    ${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Graph/GraphNodeRegistry.cpp
    ${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Graph/AnimationGraphNodeTypes.cpp
    # RegistersGameLogicAndMaterialTypeColors asserts the MATERIAL type colors,
    # so these have to be really registered rather than stubbed — and
    # GraphNodeRegistry.cpp does not link without this TU either.
    ${CMAKE_SOURCE_DIR}/Apps/Editor/Source/ShaderGraph/MaterialGraphNodeTypes.cpp
    ${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Graph/NodeColorSettings.cpp
    # Opening a settings page must not write the project file. Complements
    # LodSettingsPageTests, which asserts the descriptor's shape without
    # invoking its closures: this one drives those closures the way the row
    # builders do, against a temp workspace root.
    SettingsPageOpenIsNotAnEditTests.cpp
    # The project-switch re-apply for every renderer knob the project settings
    # file owns (it composes Rendering::LodProjectSettings from Engine), same
    # arrangement.
    RenderProjectSettingsTests.cpp
    ${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Editor/Settings/RenderProjectSettings.cpp
    # The guard that keeps an unchanged HDR re-apply (project open, settings
    # page edit) from recreating the swapchain. A pure predicate over
    # HdrOutputState, so it compiles in here without a device.
    HdrOutputMatchTests.cpp
    ${CMAKE_SOURCE_DIR}/Apps/Editor/Source/Display/HdrOutputMatch.cpp
    EditorSdkVcsRegistryTests.cpp
    # Material inspector row rules (visibility / control selection / order), and what
    # assigning an emissive map does to the rest of the document.
    EditorSdkMaterialRowsTests.cpp
    EditorSdkEmissiveMapAssignmentTests.cpp
    EditorSdkMaterialUndoSnapshotTests.cpp
    # C12 editor-kind unload refusal: module-stamped registry pins.
    EditorSdkModulePinTests.cpp
    EditorVcsFirstDetectGateTests.cpp
    # Status-subscription lifetime: outliving the service, and surviving the
    # Shutdown() that every project switch runs.
    EditorVcsSubscriptionLifetimeTests.cpp
    ${CMAKE_SOURCE_DIR}/Apps/Editor/Source/VersionControl/EditorVersionControlService.cpp)
set_target_properties(EditorSdkRegistryTests PROPERTIES UNITY_BUILD OFF)
target_include_directories(EditorSdkRegistryTests PRIVATE
    ${CMAKE_SOURCE_DIR}/Apps/Editor/Include
    # LodSettingsPage.h is a Source-local header (only the Editor target and this
    # test include it), unlike LodProjectSettings.h which is public in Include/.
    ${CMAKE_SOURCE_DIR}/Apps/Editor/Source)
target_link_libraries(EditorSdkRegistryTests PRIVATE
    Engine EditorSDK GTest::gtest GTest::gtest_main)

ge_set_output_tests(EditorSdkRegistryTests)
ge_stage_runtime_dependencies(EditorSdkRegistryTests)
# Compiles the editor's MaterialGraphNodeTypes.cpp, whose material palette comes
# from reflecting the engine node library off disk.
ge_stage_shader_graph_nodes(EditorSdkRegistryTests)

add_test(NAME EditorSdkRegistryTests COMMAND $<TARGET_FILE:EditorSdkRegistryTests>)
set_tests_properties(EditorSdkRegistryTests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# ---------------------------------------------------------------------------
# End-to-end: build a REAL Editor-kind module DLL with cmake + MSVC against
# the Engine + EditorSDK import libs, load it through NativeScriptManager, and
# assert its registrations land in the shared registries (incl. the
# late-plugin inspector replay). Heavyweight — longer ctest timeout, own
# scratch build dir (mirrors UserModuleBuildLoadTest).
# ---------------------------------------------------------------------------
add_executable(EditorModuleBuildLoadTest EditorModuleBuildLoadTest.cpp)
set_target_properties(EditorModuleBuildLoadTest PROPERTIES UNITY_BUILD OFF)
target_link_libraries(EditorModuleBuildLoadTest PRIVATE
    Engine EditorSDK Types GTest::gtest GTest::gtest_main)
target_include_directories(EditorModuleBuildLoadTest PRIVATE
    ${CMAKE_SOURCE_DIR}/Engine/Include)

ge_set_output_tests(EditorModuleBuildLoadTest)
ge_stage_runtime_dependencies(EditorModuleBuildLoadTest)

# The engine + editor build interface, via generator expressions — the same
# shape the c10 test uses, extended with the EditorSDK import lib and the
# editor headers an Editor-kind module compiles against. The vcpkg include
# dir mirrors the staged SDK's engineincludes (editor headers pull nlohmann).
set(_ge_editorsdk_vcpkg_include "")
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET AND
   IS_DIRECTORY "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include")
    set(_ge_editorsdk_vcpkg_include ";${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include")
endif()
file(GENERATE
    OUTPUT "$<TARGET_FILE_DIR:EditorModuleBuildLoadTest>/editorsdk_config.txt"
    CONTENT
"cmake=${CMAKE_COMMAND}
importlib=$<TARGET_LINKER_FILE:Engine>
editorimportlib=$<TARGET_LINKER_FILE:EditorSDK>
config=$<CONFIG>
sdkentry=${CMAKE_SOURCE_DIR}/Engine/Modules/NativeScripting/GameSDK/Source/UserModuleEntry.cpp
sdkinc=${CMAKE_SOURCE_DIR}/Engine/Modules/NativeScripting/GameSDK/Include
workdir=${CMAKE_BINARY_DIR}/Tests/EditorSDK/editorsdk_work
includes=$<TARGET_PROPERTY:Engine,INTERFACE_INCLUDE_DIRECTORIES>
editorincludes=$<TARGET_PROPERTY:EditorSDK,INTERFACE_INCLUDE_DIRECTORIES>${_ge_editorsdk_vcpkg_include}
defs=$<TARGET_PROPERTY:Engine,INTERFACE_COMPILE_DEFINITIONS>
globaldefs=GLM_FORCE_DEPTH_ZERO_TO_ONE;NOMINMAX;WIN32_LEAN_AND_MEAN;GE_DEBUG_INSTRUMENTATION=${GE_DEBUG_INSTRUMENTATION_VALUE}
")

# The real customer: the eztree package's Editor/ module builds and loads in
# the same test (acceptance proof for the EZTreeEditor migration). Tests never
# reach into the source tree at runtime — stage a package copy beside the exe.
# Sources + manifest only: the module build never reads Assets/, and the
# texture payload would otherwise duplicate per config.
add_custom_command(TARGET EditorModuleBuildLoadTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            "${CMAKE_SOURCE_DIR}/Packages/eztree/Editor"
            "$<TARGET_FILE_DIR:EditorModuleBuildLoadTest>/TestData/Packages/eztree/Editor"
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            "${CMAKE_SOURCE_DIR}/Packages/eztree/Native"
            "$<TARGET_FILE_DIR:EditorModuleBuildLoadTest>/TestData/Packages/eztree/Native"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Packages/eztree/package.json"
            "$<TARGET_FILE_DIR:EditorModuleBuildLoadTest>/TestData/Packages/eztree/package.json"
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            "${CMAKE_SOURCE_DIR}/Packages/git-vcs/Editor"
            "$<TARGET_FILE_DIR:EditorModuleBuildLoadTest>/TestData/Packages/git-vcs/Editor"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Packages/git-vcs/package.json"
            "$<TARGET_FILE_DIR:EditorModuleBuildLoadTest>/TestData/Packages/git-vcs/package.json"
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            "${CMAKE_SOURCE_DIR}/Packages/lore-vcs/Editor"
            "$<TARGET_FILE_DIR:EditorModuleBuildLoadTest>/TestData/Packages/lore-vcs/Editor"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Packages/lore-vcs/package.json"
            "$<TARGET_FILE_DIR:EditorModuleBuildLoadTest>/TestData/Packages/lore-vcs/package.json"
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            "${CMAKE_SOURCE_DIR}/Packages/svn-vcs/Editor"
            "$<TARGET_FILE_DIR:EditorModuleBuildLoadTest>/TestData/Packages/svn-vcs/Editor"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Packages/svn-vcs/package.json"
            "$<TARGET_FILE_DIR:EditorModuleBuildLoadTest>/TestData/Packages/svn-vcs/package.json"
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            "${CMAKE_SOURCE_DIR}/Packages/diversion-vcs/Editor"
            "$<TARGET_FILE_DIR:EditorModuleBuildLoadTest>/TestData/Packages/diversion-vcs/Editor"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Packages/diversion-vcs/package.json"
            "$<TARGET_FILE_DIR:EditorModuleBuildLoadTest>/TestData/Packages/diversion-vcs/package.json"
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            "${CMAKE_SOURCE_DIR}/Packages/unity-import/Editor"
            "$<TARGET_FILE_DIR:EditorModuleBuildLoadTest>/TestData/Packages/unity-import/Editor"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Packages/unity-import/package.json"
            "$<TARGET_FILE_DIR:EditorModuleBuildLoadTest>/TestData/Packages/unity-import/package.json"
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            "${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor"
            "$<TARGET_FILE_DIR:EditorModuleBuildLoadTest>/TestData/Packages/ai-assistant/Editor"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Packages/ai-assistant/package.json"
            "$<TARGET_FILE_DIR:EditorModuleBuildLoadTest>/TestData/Packages/ai-assistant/package.json"
    COMMENT "Staging engine package sources for EditorModuleBuildLoadTest")

add_test(NAME EditorModuleBuildLoadTest COMMAND $<TARGET_FILE:EditorModuleBuildLoadTest>)
set_tests_properties(EditorModuleBuildLoadTest PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    TIMEOUT 900)
