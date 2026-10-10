# Editor-side unit tests. Compiles the units under test directly rather than
# linking Editor.exe, so a test run costs one small object plus Engine.dll.

find_package(GTest QUIET)
if(NOT GTest_FOUND)
    message(STATUS "GTest not found, skipping Editor tests")
    return()
endif()

add_executable(EditorDeviceLossTests
    DeviceLossSurfacerTests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/DeviceLossSurfacer.cpp
)

target_include_directories(EditorDeviceLossTests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../Include
)

target_link_libraries(EditorDeviceLossTests
    PRIVATE
        Engine
        GTest::gtest
        GTest::gtest_main
)

target_compile_features(EditorDeviceLossTests PRIVATE cxx_std_20)

ge_set_output_tests(EditorDeviceLossTests)
ge_stage_runtime_dependencies(EditorDeviceLossTests)

add_test(NAME EditorDeviceLossTests COMMAND EditorDeviceLossTests)

add_executable(EditorLogViewTests
    LogViewTests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/Panels/LogView.cpp
)

target_include_directories(EditorLogViewTests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../Include
)

target_link_libraries(EditorLogViewTests
    PRIVATE
        Engine
        GTest::gtest
        GTest::gtest_main
)

target_compile_features(EditorLogViewTests PRIVATE cxx_std_20)

ge_set_output_tests(EditorLogViewTests)
ge_stage_runtime_dependencies(EditorLogViewTests)

add_test(NAME EditorLogViewTests COMMAND EditorLogViewTests)

add_executable(EditorCaptureOutputDirectoryTests
    CaptureOutputDirectoryTests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/Editor/CaptureOutputDirectory.cpp
)

target_include_directories(EditorCaptureOutputDirectoryTests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../Include
        ${CMAKE_CURRENT_SOURCE_DIR}/../Source
)

target_link_libraries(EditorCaptureOutputDirectoryTests
    PRIVATE
        Engine
        GTest::gtest
        GTest::gtest_main
)

target_compile_features(EditorCaptureOutputDirectoryTests PRIVATE cxx_std_20)

ge_set_output_tests(EditorCaptureOutputDirectoryTests)
ge_stage_runtime_dependencies(EditorCaptureOutputDirectoryTests)

add_test(NAME EditorCaptureOutputDirectoryTests COMMAND EditorCaptureOutputDirectoryTests)

add_executable(EditorScreenshotCaptureWaitTests
    ScreenshotCaptureWaitTests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/DebugServer/ScreenshotCaptureWait.cpp
)

target_include_directories(EditorScreenshotCaptureWaitTests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../Include
        ${CMAKE_CURRENT_SOURCE_DIR}/../Source
)

target_link_libraries(EditorScreenshotCaptureWaitTests
    PRIVATE
        Engine
        GTest::gtest
        GTest::gtest_main
)

target_compile_features(EditorScreenshotCaptureWaitTests PRIVATE cxx_std_20)

ge_set_output_tests(EditorScreenshotCaptureWaitTests)
ge_stage_runtime_dependencies(EditorScreenshotCaptureWaitTests)

add_test(NAME EditorScreenshotCaptureWaitTests COMMAND EditorScreenshotCaptureWaitTests)

add_executable(EditorThumbnailTests
    TextureThumbnailHandlerTests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/Thumbnails/TextureThumbnailHandler.cpp
)

target_include_directories(EditorThumbnailTests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../Include
)

target_link_libraries(EditorThumbnailTests
    PRIVATE
        Engine
        GTest::gtest
        GTest::gtest_main
)

# stb's implementations live inside Engine.dll; the headers are needed here so
# the handler can call stbir_resize_uint8_srgb and the test can read back PNGs.
if(TARGET stb::stb)
    target_link_libraries(EditorThumbnailTests PRIVATE stb::stb)
endif()
target_compile_definitions(EditorThumbnailTests PRIVATE GE_HAVE_STB)

target_compile_features(EditorThumbnailTests PRIVATE cxx_std_20)

ge_set_output_tests(EditorThumbnailTests)
ge_stage_runtime_dependencies(EditorThumbnailTests)

add_test(NAME EditorThumbnailTests COMMAND EditorThumbnailTests)

# Global search palette: lexical searches and the code index run as JobSystem jobs.
add_executable(EditorUniversalSearchTests
    UniversalSearchProviderTests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/UI/UniversalSearchProvider.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/Assets/AssetSearchProvider.cpp
)

target_include_directories(EditorUniversalSearchTests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../Include
)

target_link_libraries(EditorUniversalSearchTests
    PRIVATE
        Engine
        GTest::gtest
        GTest::gtest_main
)

target_compile_features(EditorUniversalSearchTests PRIVATE cxx_std_20)

ge_set_output_tests(EditorUniversalSearchTests)
ge_stage_runtime_dependencies(EditorUniversalSearchTests)

add_test(NAME EditorUniversalSearchTests COMMAND EditorUniversalSearchTests)

# Routing-level companion: exercises ThumbnailService itself, which needs the
# handlers it dispatches through compiled alongside it.
add_executable(EditorThumbnailRoutingTests
    ThumbnailServiceRoutingTests.cpp
    ThumbnailResultCacheTests.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/Thumbnails/ThumbnailService.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/Thumbnails/FolderBakeReport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/Thumbnails/TextureThumbnailHandler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/Thumbnails/ModelThumbnailHandler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/Thumbnails/ModelThumbnailBakeQueue.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/Thumbnails/ModelThumbnailBakedImages.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/Thumbnails/ModelThumbnailCacheFiles.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/Thumbnails/ModelThumbnailImports.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/Thumbnails/ModelThumbnailPngPersistence.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/Thumbnails/ModelThumbnailPreviewScene.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/Thumbnails/ModelThumbnailRenderLanes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/Thumbnails/ShaderGraphThumbnailMaterial.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../Source/Editor/Assets/ShaderGlslOpen.cpp
)

target_include_directories(EditorThumbnailRoutingTests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../Include
        ${CMAKE_CURRENT_SOURCE_DIR}/../Source
)

target_link_libraries(EditorThumbnailRoutingTests
    PRIVATE
        Engine
        EditorSDK
        GTest::gtest
        GTest::gtest_main
)

if(TARGET stb::stb)
    target_link_libraries(EditorThumbnailRoutingTests PRIVATE stb::stb)
endif()
target_compile_definitions(EditorThumbnailRoutingTests PRIVATE GE_HAVE_STB)
target_compile_features(EditorThumbnailRoutingTests PRIVATE cxx_std_20)

ge_set_output_tests(EditorThumbnailRoutingTests)
ge_stage_runtime_dependencies(EditorThumbnailRoutingTests)

add_test(NAME EditorThumbnailRoutingTests COMMAND EditorThumbnailRoutingTests)

# Execute the production scheduling, publication, slot-release, import-release, idle and
# folder-bake queue methods with deterministic hosts.
# This standalone target needs no full Editor or GPU to catch a whole-sweep return.
set(THUMBNAIL_CACHE_TEST_HEADERS)
# Each phase is extracted from the file that owns it: <phase>:<file under Source/Thumbnails>.
foreach(entry IN ITEMS Schedule:ModelThumbnailPngPersistence.cpp Persist:ModelThumbnailPngPersistence.cpp
                       Release:ModelThumbnailBakedImages.cpp Imports:ModelThumbnailImports.cpp
                       Idle:ModelThumbnailHandler.cpp Bakes:ModelThumbnailBakeQueue.cpp)
    string(REPLACE ":" ";" entry_parts "${entry}")
    list(GET entry_parts 0 phase)
    list(GET entry_parts 1 phase_source)
    string(TOUPPER "${phase}" phase_upper)
    set(header "${CMAKE_CURRENT_BINARY_DIR}/generated/ThumbnailCache${phase}Extracted.h")
    add_custom_command(OUTPUT "${header}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND ${CMAKE_COMMAND}
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/../Source/Thumbnails/${phase_source}
            -DOUT=${header}
            "-DBEGIN_MARKER=// GE_THUMBNAIL_CACHE_${phase_upper}_BEGIN"
            "-DEND_MARKER=// GE_THUMBNAIL_CACHE_${phase_upper}_END"
            -P ${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Tests/ExtractShaderBlock.cmake
        DEPENDS ../Source/Thumbnails/${phase_source}
            ${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Tests/ExtractShaderBlock.cmake
        VERBATIM)
    list(APPEND THUMBNAIL_CACHE_TEST_HEADERS "${header}")
endforeach()
add_executable(EditorThumbnailPersistenceTests ThumbnailPersistenceTests.cpp
    ${THUMBNAIL_CACHE_TEST_HEADERS}
    ${CMAKE_SOURCE_DIR}/Engine/Modules/AssetCore/Source/GUID.cpp)
target_include_directories(EditorThumbnailPersistenceTests PRIVATE
    ../Include ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${CMAKE_SOURCE_DIR}/Engine/Modules/AssetCore/Include)
target_link_libraries(EditorThumbnailPersistenceTests PRIVATE GTest::gtest_main)
if(TARGET stb::stb)
    target_link_libraries(EditorThumbnailPersistenceTests PRIVATE stb::stb)
endif()
target_compile_features(EditorThumbnailPersistenceTests PRIVATE cxx_std_20)
if(MSVC)
    target_compile_definitions(EditorThumbnailPersistenceTests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
ge_set_output_tests(EditorThumbnailPersistenceTests)
add_test(NAME EditorThumbnailPersistenceTests COMMAND EditorThumbnailPersistenceTests)

# Pure path-key tests: no renderer, editor process, or repository needed.
add_executable(VCSPathKeyTests VCSPathKeyTests.cpp)
target_include_directories(VCSPathKeyTests PRIVATE
    ${CMAKE_SOURCE_DIR}/Engine/Modules/VCSIntegration/Include)
target_link_libraries(VCSPathKeyTests PRIVATE GTest::gtest_main)
target_compile_features(VCSPathKeyTests PRIVATE cxx_std_20)
ge_set_output_tests(VCSPathKeyTests)
add_test(NAME VCSPathKeyTests COMMAND VCSPathKeyTests)

# The AI Assistant package's agent interface, its providers and its settings page,
# compiled from the package tree: no editor process or network needed. The Claude API
# provider streams recorded replies from PlatformTestSupport's loopback server; the CLI
# session providers run FakeAgentCli, which replays the redacted recordings in
# AgentCliFixtures/ (both staged beside the suite).
find_package(nlohmann_json CONFIG REQUIRED)
add_executable(AgentProviderTests
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Tests/AgentProviderTests.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Tests/SseParserTests.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Tests/ClaudeApiProviderTests.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Tests/CliSessionProviderTests.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Tests/AiAssistantSettingsTests.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Tests/AgentSessionStateTests.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Tests/CliSessionFilesTests.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Tests/AssistantAttachmentTests.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AssistantAttachment.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AssistantMode.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AssistantTools.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AgentSessionState.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AiAssistantLoginStatusRow.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AiAssistantSettings.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/ModelDisplayName.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/Providers/SseParser.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/Providers/ClaudeApiStream.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/Providers/ClaudeApiProvider.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/Providers/CliSessionProvider.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/Providers/ClaudeSessionProvider.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/Providers/CodexSessionProvider.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/Providers/CliSessionFiles.cpp)
target_include_directories(AgentProviderTests PRIVATE
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor)
target_link_libraries(AgentProviderTests PRIVATE
    Engine EditorSDK PlatformTestSupport nlohmann_json::nlohmann_json GTest::gtest_main)
target_compile_features(AgentProviderTests PRIVATE cxx_std_20)
ge_set_output_tests(AgentProviderTests)
ge_stage_runtime_dependencies(AgentProviderTests)
add_test(NAME AgentProviderTests COMMAND AgentProviderTests)

add_executable(FakeAgentCli ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Tests/FakeAgentCli.cpp)
set_target_properties(FakeAgentCli PROPERTIES
    FOLDER "Tests"
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    VCPKG_APPLOCAL_DEPS OFF)
ge_set_output_tests(FakeAgentCli)
add_dependencies(AgentProviderTests FakeAgentCli)
add_custom_command(TARGET AgentProviderTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Tests/Fixtures
        $<TARGET_FILE_DIR:AgentProviderTests>/AgentCliFixtures)

# The AI Assistant panel's conversation half, without a UI: the controller's turn
# thread, serial queue, Stop and coalescing mailbox over scripted providers, the
# conversation model and the row model with its call rows, their inline images and asset
# tiles; the assistant's gate over the
# tool classes, its asking, its ledger and its undo steps; continued sessions over the
# Claude session connection running FakeAgentCli, and the Session control's rows.
add_executable(AiAssistantPanelTests
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Tests/AiAssistantPanelTests.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Tests/AssistantGateTests.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AssistantGate.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AssistantActionLedger.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AssistantTurnUndo.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AgentCallRows.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AgentCallPieces.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AgentCallMedia.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AgentCallResourceResolver.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AgentCallResourceTile.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AgentCallImageView.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AgentCallFileMenu.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AgentCallRow.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AgentCallPieceView.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AssistantAttachment.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AssistantMode.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AssistantTools.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AgentConversationController.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AgentSessionSearchProvider.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AgentModelPopover.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/ModeControlFit.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AgentConversationRowModel.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AgentConversationRow.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AgentReplyActions.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/Conversation.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AgentStatusMessages.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/TurnMailbox.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AgentSessionState.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AiAssistantLoginStatusRow.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/AiAssistantSettings.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/ModelDisplayName.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/Providers/SseParser.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/Providers/ClaudeApiStream.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/Providers/ClaudeApiProvider.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/Providers/CliSessionProvider.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/Providers/ClaudeSessionProvider.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/Providers/CodexSessionProvider.cpp
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor/Providers/CliSessionFiles.cpp)
target_include_directories(AiAssistantPanelTests PRIVATE
    ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Editor
    # UIRgTestHarness.h: a headless device for a real UIManager.
    ${CMAKE_SOURCE_DIR}/Engine/Modules/UI/Tests)
target_link_libraries(AiAssistantPanelTests PRIVATE
    Engine EditorSDK nlohmann_json::nlohmann_json GTest::gtest_main)
target_compile_features(AiAssistantPanelTests PRIVATE cxx_std_20)
ge_set_output_tests(AiAssistantPanelTests)
ge_stage_runtime_dependencies(AiAssistantPanelTests)
add_dependencies(AiAssistantPanelTests FakeAgentCli)
add_custom_command(TARGET AiAssistantPanelTests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:AiAssistantPanelTests>/AiAssistantPanelFixtures
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Tests/Fixtures/claude-dead-session.stdout
        $<TARGET_FILE_DIR:AiAssistantPanelTests>/AiAssistantPanelFixtures/claude-dead-session.stdout
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:AiAssistantPanelTests>/AiAssistantPanelFixtures/ai-assistant/Editor/UI/panels
        $<TARGET_FILE_DIR:AiAssistantPanelTests>/AiAssistantPanelFixtures/ai-assistant/Editor/UI/controls
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Assets/Editor/UI/panels/PromptPanel.css
        $<TARGET_FILE_DIR:AiAssistantPanelTests>/AiAssistantPanelFixtures/ai-assistant/Editor/UI/panels/PromptPanel.css
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Assets/Editor/UI/controls/AgentConversationRow.uxml
        $<TARGET_FILE_DIR:AiAssistantPanelTests>/AiAssistantPanelFixtures/ai-assistant/Editor/UI/controls/AgentConversationRow.uxml
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/Packages/ai-assistant/Assets/Editor/UI/controls/AgentModelPopover.uxml
        $<TARGET_FILE_DIR:AiAssistantPanelTests>/AiAssistantPanelFixtures/ai-assistant/Editor/UI/controls/AgentModelPopover.uxml)
add_test(NAME AiAssistantPanelTests COMMAND AiAssistantPanelTests)
