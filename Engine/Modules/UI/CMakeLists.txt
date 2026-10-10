cmake_minimum_required(VERSION 3.20)
project(UI LANGUAGES CXX)

# Headers and sources
file(GLOB_RECURSE UI_PUBLIC_HEADERS ${CMAKE_CURRENT_SOURCE_DIR}/Include/UI/*.h)
set(UI_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UIManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/DefaultStylesheet.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UIManager_Update.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UIManager_Input.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UIManager_Assets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UIElement.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/ModuleOwnedHandlers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/ITextMeasurable.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/GlyphRunEmitter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UIHotReload.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UIStyle.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UITargetSpace.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UITextureSpace.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/ClassBackgroundImageQuery.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/StyleOverrides.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/StyleInterpolation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/StyleApplier.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/TransitionEngine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/ResolvedStyle.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UIManager_StyleResolve.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UIManager_StyleAnalysis.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UIManager_HoverAndEvents.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UIManager_AttachEvents.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UiDispatcher.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UiContext.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UiPostTarget.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UiPostHandle.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UiCoalescedPost.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UIManagerRef.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/ModifierKeyState.cpp

    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Assets/UILayoutAsset.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Assets/UIStyleAsset.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Assets/CssImports.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Parsers/XMLParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Parsers/CSSParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Parsers/CSSValueParsers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Parsers/CSSVarResolver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Parsers/CSSStylesheetParser.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Layout/YogaLayout.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/Label.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/BaseField.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/TextField.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/FloatField.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/TextArea.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/ScrollView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/Scrollbar.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/DockspaceElement.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/DockOverlay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Layout/Docking.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Layout/DockLayoutController.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Layout/DockingHitTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/Mount.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/Button.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Registration/ElementRegistration.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/ControlRegistration.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/TreeView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/GridView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/VirtualWindowCore.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/ListView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/AxisModel.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/AxisHeaderBar.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/TableView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/EnableDot.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/Toggle.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/Checkbox.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/Dropdown.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/PopoverArrow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/Slider.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/Foldout.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/AccordionItem.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/Accordion.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/SearchDialog.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/SearchFieldWithFilter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/FloatingPanel.cpp

    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/DockTab.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Controls/DockTabBar.cpp

    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Interaction/Manipulator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Interaction/PointerManipulator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Interaction/ContextMenuManipulator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Interaction/DismissablePopup.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Interaction/FocusIsInside.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Interaction/ItemResizeGesture.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Interaction/DragDropManager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Interaction/DragDropOverlay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Interaction/DragDropGhostRendererRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/Interaction/TooltipOverlay.cpp

    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UICompatDrawRuns.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UITextureRegistry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UIManager_RenderPrep.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UIManager_RenderRG.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UIManager_PrimitiveGen.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/NineSliceLayout.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/TextSegmentation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UIManager_Layout.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UIManager_CascadeProbe.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UIManager_Fonts.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/UIManager_Debug.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Source/SlotAllocator.cpp
)

# The default stylesheet is compiled into the module (Source/DefaultStylesheet.cpp includes the
# generated header), so every executable and every exported game carries it with no staged file.
set(UI_DEFAULT_STYLESHEET_CSS ${CMAKE_CURRENT_SOURCE_DIR}/Assets/defaults.css)
set(UI_DEFAULT_STYLESHEET_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/DefaultStylesheetText.h)
add_custom_command(
    OUTPUT ${UI_DEFAULT_STYLESHEET_HEADER}
    COMMAND ${CMAKE_COMMAND} -DSRC=${UI_DEFAULT_STYLESHEET_CSS}
                             -DOUT=${UI_DEFAULT_STYLESHEET_HEADER}
                             -DSYMBOL=kDefaultStylesheetText
                             -P ${CMAKE_SOURCE_DIR}/cmake/EmbedTextFile.cmake
    DEPENDS ${UI_DEFAULT_STYLESHEET_CSS} ${CMAKE_SOURCE_DIR}/cmake/EmbedTextFile.cmake
    COMMENT "Embedding the UI default stylesheet"
    VERBATIM)
list(APPEND UI_SOURCES ${UI_DEFAULT_STYLESHEET_HEADER})

add_library(UI OBJECT ${UI_PUBLIC_HEADERS} ${UI_SOURCES})
target_include_directories(UI PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)

# Public includes
target_include_directories(UI PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Include)
# Access Engine headers for AssetManager integration (UIStyleAsset, UILayoutAsset, UIManager)
# This is a PRIVATE include since only UI .cpp files need it
target_include_directories(UI PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Include)
# AssetManager.h includes FileWatcher/FileWatcher.h. Headers only: the
# FileWatcher objects reach the binary through the Engine module splice.
target_include_directories(UI PRIVATE $<TARGET_PROPERTY:FileWatcher,INTERFACE_INCLUDE_DIRECTORIES>)

# libktx: UI background/preview uploads re-decode block-compressed KTX2
# containers (material path transcodes them to BC7, which is not per-pixel
# addressable for the RGBA8 UI upload). PUBLIC so Engine, which splices this
# OBJECT library's objects in, inherits the link requirement.
find_package(Ktx CONFIG QUIET)
if(TARGET KTX::ktx)
    target_link_libraries(UI PUBLIC KTX::ktx)
    target_compile_definitions(UI PRIVATE GE_HAVE_KTX)
endif()

# Third-party dependencies via vcpkg (optional for now; linked when present)
find_package(pugixml CONFIG QUIET)
if(pugixml_FOUND)
    target_link_libraries(UI PUBLIC pugixml::pugixml)
    target_compile_definitions(UI PUBLIC GE_HAVE_PUGIXML=1)
else()
    target_compile_definitions(UI PUBLIC GE_HAVE_PUGIXML=0)
endif()

# Prefer CMake package config if provided by vcpkg; fall back to manual discovery in manifest mode
find_package(lexbor CONFIG QUIET)
if(lexbor_FOUND)
    if(TARGET lexbor::lexbor)
        target_link_libraries(UI PUBLIC lexbor::lexbor)
        target_compile_definitions(UI PUBLIC GE_HAVE_LEXBOR=1)
    elseif(TARGET lexbor::lexbor_static)
        target_link_libraries(UI PUBLIC lexbor::lexbor_static)
        target_compile_definitions(UI PUBLIC GE_HAVE_LEXBOR=1)
    else()
        message(FATAL_ERROR "lexbor package found, but no supported imported target was exported.")
    endif()
else()
    # Manual discovery: vcpkg's official port for lexbor may not export CMake config yet.
    # Look for headers and library in common vcpkg manifest locations.
    set(_LEXBOR_HINT_INCLUDE
        "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/include"
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include"
        "${CMAKE_SOURCE_DIR}/dependencies/vcpkg/installed/${VCPKG_TARGET_TRIPLET}/include"
    )
    set(_LEXBOR_HINT_LIB
        "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/lib"
        "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/debug/lib"
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib"
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib"
        "${CMAKE_SOURCE_DIR}/dependencies/vcpkg/installed/${VCPKG_TARGET_TRIPLET}/lib"
        "${CMAKE_SOURCE_DIR}/dependencies/vcpkg/installed/${VCPKG_TARGET_TRIPLET}/debug/lib"
    )

    find_path(LEXBOR_INCLUDE_DIR NAMES lexbor/css/parser.h HINTS ${_LEXBOR_HINT_INCLUDE})
    # vcpkg on Linux installs as lexbor_static, on Windows as lexbor
    find_library(LEXBOR_LIBRARY NAMES lexbor lexbor_static liblexbor_static HINTS ${_LEXBOR_HINT_LIB})

    if(LEXBOR_INCLUDE_DIR AND LEXBOR_LIBRARY)
        add_library(lexbor::lexbor UNKNOWN IMPORTED)
        set_target_properties(lexbor::lexbor PROPERTIES IMPORTED_LOCATION "${LEXBOR_LIBRARY}")
        target_include_directories(lexbor::lexbor INTERFACE "${LEXBOR_INCLUDE_DIR}")
        target_link_libraries(lexbor::lexbor INTERFACE)
        target_link_libraries(UI PUBLIC lexbor::lexbor)
        target_compile_definitions(UI PUBLIC GE_HAVE_LEXBOR=1)
        message(STATUS "lexbor located via manual discovery: ${LEXBOR_LIBRARY}")
    else()
        message(FATAL_ERROR "lexbor not found. Ensure it is installed via vcpkg for ${VCPKG_TARGET_TRIPLET} and that CMAKE_TOOLCHAIN_FILE points to vcpkg.cmake.")
    endif()
endif()

find_package(yoga CONFIG QUIET)
# Only link Yoga if a known imported target exists; avoid raw 'yoga' to prevent link errors
if(yoga_FOUND)
    if(TARGET yoga::yogacore)
        target_link_libraries(UI PUBLIC yoga::yogacore)
        target_compile_definitions(UI PUBLIC GE_HAVE_YOGA=1)
    elseif(TARGET yoga::yoga)
        target_link_libraries(UI PUBLIC yoga::yoga)
        target_compile_definitions(UI PUBLIC GE_HAVE_YOGA=1)
    elseif(TARGET yogacore)
        target_link_libraries(UI PUBLIC yogacore)
        target_compile_definitions(UI PUBLIC GE_HAVE_YOGA=1)
    elseif(TARGET YOGA::YOGA)
        target_link_libraries(UI PUBLIC YOGA::YOGA)
        target_compile_definitions(UI PUBLIC GE_HAVE_YOGA=1)
    else()
        message(WARNING "Yoga package found but no known CMake target exported; skipping link.")
        target_compile_definitions(UI PUBLIC GE_HAVE_YOGA=0)
    endif()
else()
    target_compile_definitions(UI PUBLIC GE_HAVE_YOGA=0)
endif()

# Link to engine modules
# Platform is here for one interface: INativeContextMenu (Platform/ContextMenu.h), which
# ContextMenuManipulator builds into through the host-installed factory. Platform::Window
# stays opaque throughout the UI module.
target_link_libraries(UI PUBLIC Logger AssetCore GameEngineRendering Text Types JobSystem Scheduler Input NumericExpression Platform)


# Manually inherit compile definitions from Text module to ensure font generation is enabled
get_target_property(TEXT_COMPILE_DEFS Text INTERFACE_COMPILE_DEFINITIONS)
if(TEXT_COMPILE_DEFS)
    target_compile_definitions(UI PUBLIC ${TEXT_COMPILE_DEFS})
endif()

# UIManager_*.cpp include Core/CpuProfiler.h, whose ScopedCpuProfile layout is
# gated on GE_ENABLE_CPU_PROFILING and defaults to 1 when undefined. UI cannot
# link Engine (cycle) but is spliced into Engine.dll, so without replaying
# Engine's INTERFACE definitions a -DENABLE_CPU_PROFILING=OFF build would put
# two different ScopedCpuProfile layouts inside one binary. Same pattern as
# TerrainECS.
target_compile_definitions(UI PRIVATE
    $<TARGET_PROPERTY:Engine,INTERFACE_COMPILE_DEFINITIONS>
)


set_target_properties(UI PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    FOLDER "UI"
)

if(MSVC)
    target_compile_options(UI PRIVATE /bigobj)
endif()

# Example: windowed demo (uses GLFW if available)
option(UI_BUILD_EXAMPLES "Build UI module examples" ON)
if(UI_BUILD_EXAMPLES AND BUILD_EXAMPLES)
    find_package(glfw3 QUIET)

    add_executable(UIWindowDemo ${CMAKE_CURRENT_SOURCE_DIR}/Examples/UIWindowDemo.cpp)
    target_link_libraries(UIWindowDemo PRIVATE UI Engine GameEngineRendering FileWatcher)
    if(glfw3_FOUND)
        target_link_libraries(UIWindowDemo PRIVATE glfw)
        target_compile_definitions(UIWindowDemo PRIVATE HAS_GLFW=1)
    endif()
    set_target_properties(UIWindowDemo PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "UI/Examples"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/Examples"
    )

    # The demo loads Assets/UI/<file> relative to its working directory, the exe folder.
    add_custom_command(TARGET UIWindowDemo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/Examples/Resources/UI"
                "$<TARGET_FILE_DIR:UIWindowDemo>/Assets/UI"
        COMMENT "Staging UIWindowDemo layout and stylesheets"
        VERBATIM)

    if(MSVC)
        set_target_properties(UIWindowDemo PROPERTIES
            VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:UIWindowDemo>"
        )
    endif()

    # SearchDialog visual demo
    add_executable(UISearchDialogDemo
        ${CMAKE_CURRENT_SOURCE_DIR}/Examples/UISearchDialogDemo.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UITestsHotReloadStubs.cpp
    )
    target_link_libraries(UISearchDialogDemo PRIVATE UI AssetRuntime Engine GameEngineRendering)
    if(glfw3_FOUND)
        target_link_libraries(UISearchDialogDemo PRIVATE glfw)
        target_compile_definitions(UISearchDialogDemo PRIVATE HAS_GLFW=1)
    endif()
    set_target_properties(UISearchDialogDemo PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "UI/Examples"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/Examples"
    )

    # Copy CSS assets needed by the demo into its working directory
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Assets/UI/theme)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/Assets/UI/controls)
    configure_file(
        "${CMAKE_SOURCE_DIR}/Apps/Editor/Assets/UI/theme/tokens.css"
        "${CMAKE_CURRENT_BINARY_DIR}/Assets/UI/theme/tokens.css" COPYONLY)
    configure_file(
        "${CMAKE_SOURCE_DIR}/Apps/Editor/Assets/UI/controls/SearchDialog.css"
        "${CMAKE_CURRENT_BINARY_DIR}/Assets/UI/controls/SearchDialog.css" COPYONLY)

    # Stage nethost.dll so the delay-loaded Engine dependency resolves at runtime.
    if(WIN32)
        ge_add_dotnet_hosting_runtime(UISearchDialogDemo)
    endif()

    if(MSVC)
        set_target_properties(UISearchDialogDemo PROPERTIES
            VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        )
        target_compile_options(UISearchDialogDemo PRIVATE /wd4273)
    endif()
endif()


# Unit tests for UI module
if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    # One member of the UI test family. Every member shares the headless-device
    # shader resolver (UITestShaderSetup.cpp), the process precondition that runs
    # first (#842), the .NET hosting runtime and the shipped Roboto faces.
    #
    # The faces sit next to the executables at the relative path UIManager's font
    # resolver reads (<exe dir>/Assets/Fonts), staged once for the shared test
    # directory through ge_stage_test_asset. Without them the resolver falls
    # through to a system face — Segoe UI on Windows — and every test that lays
    # out text would measure whatever font the host happens to have instead of the
    # one its expected numbers were derived from. RobotoMono rides along because
    # UIFontLoadingTests needs a SECOND, visibly different face to tell "the
    # default atlas" from "an arbitrary loaded one"; unstaged, that test resolved
    # nothing and skipped.
    #
    # Engine ONLY — UI / AssetCore / AssetRuntime / GameEngineRendering are
    # OBJECT modules already spliced into Engine.dll; linking them here as
    # well would give the exe a second copy of every module's file-scope state
    # (UIManager singletons, texture registries — issue #356, same disease as
    # #340). Module headers propagate via Engine's re-exported INTERFACE, and
    # Engine.dll exports the GE_* hot-reload dispatcher, so the local
    # UITestsHotReloadStubs.cpp compile is not needed either. GameEngine.Native
    # imports the same Engine.dll, so linking it adds an import, not a second
    # copy of any engine module.
    #
    # Include roots: UI/Source for UIManager_Internal.h (surgical-op tests inspect
    # NodeRec scratch vectors), Engine/Source for GameUI/SessionEvent.h (the
    # session-gated event registration, deliberately not public C++ API), and
    # Rendering/Tests for ScopedEnvVar, the RAII guard the device-loss pins use.
    #
    # Unity build stays off: the tests carry per-file helpers with duplicate names
    # (MakeHeadlessDevice, SendMouse) that only compile one translation unit at a
    # time.
    function(ge_add_ui_test target_name)
        add_executable(${target_name}
            # First so the process-level precondition reports before any test whose
            # cost a heap-diagnostic flag would misattribute (#842). Every member is
            # its own process, so every member carries it.
            ${CMAKE_CURRENT_SOURCE_DIR}/Tests/HeapDiagnosticsPreconditionTests.cpp
            ${ARGN}
            ${CMAKE_CURRENT_SOURCE_DIR}/Tests/SharedHeadlessDevice.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UITestShaderSetup.cpp)
        if(WIN32)
            # nethost.dll/hostfxr.dll next to the exe, so delay-loaded hosting calls
            # from Engine/Scripting can succeed in this test binary.
            ge_add_dotnet_hosting_runtime(${target_name})
        endif()
        target_link_libraries(${target_name} PRIVATE
            Engine GameEngine.Native RenderGraphTestSupport GTest::gtest GTest::gtest_main)
        # Layout, parser and cascade tests drive the YG* C API directly (LayoutYogaTests,
        # SubtreeDirtyTests, CSSKeywordParsingTests among them). yogacore is a
        # third-party static lib linked PRIVATE into Engine.dll, and the generated
        # exports.def walks only the engine OBJECT modules, so YG* is not exported —
        # the exe needs its own copy. This is not the #340 shape: the YG accessors
        # operate on caller-provided node structs and duplicate no engine-module state.
        if(TARGET yoga::yogacore)
            target_link_libraries(${target_name} PRIVATE yoga::yogacore)
        elseif(TARGET yoga::yoga)
            target_link_libraries(${target_name} PRIVATE yoga::yoga)
        elseif(TARGET yogacore)
            target_link_libraries(${target_name} PRIVATE yogacore)
        elseif(TARGET YOGA::YOGA)
            target_link_libraries(${target_name} PRIVATE YOGA::YOGA)
        endif()
        target_compile_definitions(${target_name} PRIVATE UI_TEST_BUILD_DIR="${CMAKE_BINARY_DIR}")
        target_include_directories(${target_name} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/Source
            ${CMAKE_SOURCE_DIR}/Engine/Source
            ${CMAKE_CURRENT_SOURCE_DIR}/../Rendering/Tests)
        add_dependencies(${target_name} CompileShaders)
        set_target_properties(${target_name} PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            FOLDER "UI/Tests"
            UNITY_BUILD OFF)
        ge_set_output_tests(${target_name})
        ge_stage_runtime_dependencies(${target_name})
        ge_stage_test_asset(${target_name}
            "${CMAKE_SOURCE_DIR}/Apps/Editor/Assets/Fonts/Roboto-Regular.ttf"
            "Assets/Fonts/Roboto-Regular.ttf")
        ge_stage_test_asset(${target_name}
            "${CMAKE_SOURCE_DIR}/Apps/Editor/Assets/Fonts/RobotoMono-Regular.ttf"
            "Assets/Fonts/RobotoMono-Regular.ttf")
        if(MSVC)
            target_compile_options(${target_name} PRIVATE /wd6326 /wd6011)
        endif()
        # Use the actual target file path so CTest can find the binary even when
        # RUNTIME_OUTPUT_DIRECTORY is customized.
        add_test(NAME ${target_name} COMMAND $<TARGET_FILE:${target_name}>)
    endfunction()

    # Text and layout: text geometry and input hit-testing, segmentation,
    # baselines, the flex and box-model solve against Chrome, and the paint
    # primitives (borders, backgrounds, clips, text coverage) the layout emits.
    ge_add_ui_test(UITextLayoutTests
        # Opt-in benchmarks (DISABLED_ by convention) and the primitive-gen audit's
        # refutation pass. Compiled with the suite so they cannot rot unnoticed;
        # run the benchmarks with --gtest_also_run_disabled_tests.
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UIEmitPerfTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/AdversarialAuditRepros.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UICompatDrawRunsTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/PercentageUnitsFontGapTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/PercentageUnitsMarginRadiusTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/PercentageUnitsLengthTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ShadowGlowPrimitiveTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TextEffectsTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/BackgroundOriginPaddingBoxTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/NineSliceLayoutTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/LayoutYogaTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/YogaRedesignPreconditionTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/SubtreeDirtyTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TextFieldOverflowClipTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TextOverflowEllipsisTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TextInputGeometryTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TextSegmentationTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TextSegmentationPerfTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TextGeometryChromeParityTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TextPenPlacementParityTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/LetterSpacingTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/MultilineTextLayoutTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/OverflowWrapTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/IsolatedUIFixtureTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/BaselineSnapTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TextBaselineAlignmentTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/BaselineChromeParityProbe.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/LineBoxDevicePixelParityTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ViewportRectSnapTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TextAreaContentScaleTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TextAreaMetricsTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TextAreaScrollCullingTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/CssColorToPrimitiveTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TextSelectionPaintOrderTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ZeroRectDirtyGateTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ZeroSizeSubtreeTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/PercentBorderRadiusTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/BorderBoxModelTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/FlexShorthandLayoutTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/BorderPaintPrimitiveTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/BorderEdgeSnapTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/BorderBoxModelHostileTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/MarginAutoTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/FlexBoxModelInFlexTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/FlexDefaultsParityTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/FlexPrimitiveSpotCheckTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/FlexGrowShrinkBasisParityTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/FlexJustifyAlignParityTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/FlexWrapGapChromeParityTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/DrainClipCoherenceTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ClipRadiiPhysicalTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ShapedWidthCacheTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UIFontLoadingTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TextCoverageCorrectionTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TextMaskGammaGoldenTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/SlugEdgeProfileTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UIEncodedBlendParityTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/HudFlipPreviewProbe.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/BorderBackgroundCompositeTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/OverflowClipPaddingBoxTests.cpp)

    # Styling: the CSS and XML parsers, the cascade and its caches, restyle and
    # invalidation, transitions, and every test that reads the shipped editor
    # stylesheets or layouts.
    ge_add_ui_test(UIStyleTests
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UITransitionCostTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/CSSParserTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/CssImportsTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/CSSMathFunctionTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ClassBackgroundImageQueryTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/BorderSideShorthandTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/CSSKeywordParsingTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/XMLParserTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/StyleUtilTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/StyleOverrideCustomColorTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/FloatingPanelTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UIManagerPseudoInvalidationTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/StructuralPseudoRestyleTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/RuleCacheValidatorTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/CascadeShareParityTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/LevelWaveCascadeProbeTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UIManagerRetainedModeRegressionTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/FocusRingTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ShippedFocusRingTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ShippedFocusRingCompositeTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/FieldCaptionOverflowTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/StyleOverrideSubtreePropagationTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ScrollViewDisplayCascadeTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ShippedMarkupTextAreaFocusTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TextSelectionColorTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ShippedMarkupUnseenDotTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ShippedSearchPlaceholderTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/StyleApplierTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UIHoverTransitionTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/MathInterpolationTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/CSSTransitionParserTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/StyleInterpolationTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TransitionEngineTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/SearchDialogFocusTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/DefaultStylesheetTests.cpp)
    # Stage the shipped editor stylesheets next to the test exe, at the same
    # relative path the editor reads them from under its own exe. ShippedFocusRingTests
    # asserts against these bytes rather than inline CSS, which is the only way a
    # test can see a rule duplicated into a second sheet — the defect that made
    # the :focus -> :focus-visible migration a no-op for generic buttons.
    # Every shipped sheet at every depth: a control or panel that keeps its sheet
    # in its own folder (controls/CurveField/CurveField.css) is swept too.
    set(GE_UITESTS_SHIPPED_UI_ROOT "${CMAKE_SOURCE_DIR}/Apps/Editor/Assets/UI")
    file(GLOB_RECURSE GE_UITESTS_SHIPPED_CSS RELATIVE "${GE_UITESTS_SHIPPED_UI_ROOT}"
        "${GE_UITESTS_SHIPPED_UI_ROOT}/*.css")
    set(GE_UITESTS_SHIPPED_CSS_COMMANDS)
    foreach(GE_UITESTS_SHIPPED_SHEET IN LISTS GE_UITESTS_SHIPPED_CSS)
        get_filename_component(GE_UITESTS_SHIPPED_SHEET_DIR "${GE_UITESTS_SHIPPED_SHEET}" DIRECTORY)
        list(APPEND GE_UITESTS_SHIPPED_CSS_COMMANDS
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:UIStyleTests>/Assets/UI/${GE_UITESTS_SHIPPED_SHEET_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${GE_UITESTS_SHIPPED_UI_ROOT}/${GE_UITESTS_SHIPPED_SHEET}"
                "$<TARGET_FILE_DIR:UIStyleTests>/Assets/UI/${GE_UITESTS_SHIPPED_SHEET}")
    endforeach()
    add_custom_command(TARGET UIStyleTests POST_BUILD
        ${GE_UITESTS_SHIPPED_CSS_COMMANDS}
        COMMENT "Staging shipped editor stylesheets for UIStyleTests"
        VERBATIM)

    # layout.uxml alongside them: ClassBackgroundImageQueryTests reads the panel
    # inventory out of it and requires every panel's icon class to resolve
    # against the staged stylesheets, so a panel added without a rule fails
    # here rather than showing the launcher's generic mark in the editor.
    add_custom_command(TARGET UIStyleTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Apps/Editor/Assets/UI/layout.uxml"
            "$<TARGET_FILE_DIR:UIStyleTests>/Assets/UI/layout.uxml"
        COMMENT "Staging shipped layout.uxml for UIStyleTests"
        VERBATIM)

    # FloatingPanel instantiates its chrome from this layout through the asset
    # manager; FloatingPanelTests mount the staged Assets/ tree as the editor
    # source, so the file has to be there next to the control's stylesheet.
    add_custom_command(TARGET UIStyleTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Apps/Editor/Assets/UI/controls/FloatingPanel.uxml"
            "$<TARGET_FILE_DIR:UIStyleTests>/Assets/UI/controls/FloatingPanel.uxml"
        COMMENT "Staging FloatingPanel.uxml for UIStyleTests"
        VERBATIM)

    # Controls: buttons, toggles, dropdowns, fields and text inputs, sliders,
    # list, grid, tree and table views, popups, docking and focus hand-off.
    ge_add_ui_test(UIControlTests
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/DockingTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/DockPanelDefaultTabIconTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/DockingHitTestTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ButtonTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ContextMenuManipulatorTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ToggleCheckboxTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/DropdownTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/DisabledControlInputTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/DismissablePopupTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/PopupPlacementTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TooltipPlacementTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UIScrollVirtualizationTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/VirtualWindowCoreTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TextFieldXmlTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TextFieldPointerTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/FieldCaptionTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/FloatFieldDragToChangeTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TextInputSelectionTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ReleaseFocusHandoffTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TextInputCursorTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TextAreaTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TreeViewTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TableViewTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/SliderTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ListViewTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/GridViewTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/SearchDialogTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/CaretBlinkTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/CaretForceVisibleDeadlineTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ItemResizeGestureTests.cpp)

    # The UI core the rest sits on: event dispatch and handler tables, element
    # registration and lifecycle, attach/detach, UIManager ordering and input
    # routing, the game UI host and its element ABI, hot reload, the render-graph
    # arm, target and texture spaces, and the texture registry.
    # GameEngine.Native carries the GE_UIElement_* exports, and this is the only
    # suite that has BOTH a real mounted UI tree (HudFixture, on a published
    # gameplay host) and can call them, which is what the element ABI's TYPE
    # rules need: they are cross-DLL dynamic_casts to class TEMPLATE
    # instantiations (Field<float>, Field<bool>), and nothing else exercises one.
    # AbiNativeSmokeTest links the exports but has no host, so every call there
    # stops at not-found before a type is ever examined.
    ge_add_ui_test(UITests
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ModuleOwnedHandlerRevocationTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ElementFactoryOwnershipTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ElementTypeOwnershipHazardTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ElementRegistrationTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UiDispatcherTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/PostActionThreadingTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UIManagerTeardownTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UIElementEventDispatchTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/HandlerTableCompactionTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/DispatchWalkStabilityTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ButtonClickEventTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UIEventHandlerBitsTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UIElementWeakRefTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/AttachDetachEventTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/FocusReleaseOnRemovalTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ControlValueEventTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UIManagerOrderTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UIMouseButtonDispatchTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UIManagerRenderRGTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UITargetSpaceTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UITextureSpaceTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/GameUIHostTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/GameUIHostKeyboardTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/GameUIElementHandleTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UIIdleGateTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ManagedElementOrphanTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/ManagedTypeLifecycleTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/TagIdentityTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UIHotReloadTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UIStyleAssetReloadTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/OverlayLayerOrderTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UITextureRegistryTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UITextureRegistryDeviceRebuildTests.cpp)

    # --- Dispatch allocation contract. Its own target because it counts allocations in a
    #     process-wide window, which must not take in the UI test executables' shared tests.
    #     Same isolation rule as Tests/Editor/ToolbarDragDropAllocationTests.
    #
    # The allocation contracts measured in Engine are registered only where Engine's
    # allocations are counted. On Windows every image has its own operator new, and
    # Engine.dll carries the allocation hook only where GE_DEBUG_INSTRUMENTATION is 1, so a
    # Windows Release run would read a false zero: those configurations are left out
    # rather than run as a vacuous pass. On macOS and Linux the replacements coalesce
    # process-wide, so every configuration measures.
    if(WIN32)
        set(_ge_engine_allocation_test_configurations CONFIGURATIONS Debug DebugFast)
    else()
        set(_ge_engine_allocation_test_configurations "")
    endif()
    add_executable(UIDispatchAllocationTests
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UIDispatchAllocationTests.cpp
    )
    target_link_libraries(UIDispatchAllocationTests PRIVATE Engine GTest::gtest GTest::gtest_main)
    set_target_properties(UIDispatchAllocationTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "UI/Tests"
        VCPKG_APPLOCAL_DEPS OFF
    )
    ge_set_output_tests(UIDispatchAllocationTests)
    ge_stage_runtime_dependencies(UIDispatchAllocationTests)
    add_test(NAME UIDispatchAllocationTests ${_ge_engine_allocation_test_configurations}
        COMMAND $<TARGET_FILE:UIDispatchAllocationTests>)

    # --- Mouse dispatch allocation contract. Its own target for the same
    #     isolation reason, and separate from UIDispatchAllocationTests because
    #     it counts this thread only: OnMouseButton lives in Engine.dll and the
    #     job system's workers must not be charged to a click.
    add_executable(UIMouseDispatchAllocationTests
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/UIMouseDispatchAllocationTests.cpp
    )
    target_link_libraries(UIMouseDispatchAllocationTests PRIVATE Engine GTest::gtest GTest::gtest_main)
    target_include_directories(UIMouseDispatchAllocationTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests
        ${CMAKE_CURRENT_SOURCE_DIR}/../Rendering/Tests)
    add_dependencies(UIMouseDispatchAllocationTests CompileShaders)
    set_target_properties(UIMouseDispatchAllocationTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "UI/Tests"
        VCPKG_APPLOCAL_DEPS OFF
    )
    ge_set_output_tests(UIMouseDispatchAllocationTests)
    ge_stage_runtime_dependencies(UIMouseDispatchAllocationTests)
    add_test(NAME UIMouseDispatchAllocationTests ${_ge_engine_allocation_test_configurations}
        COMMAND $<TARGET_FILE:UIMouseDispatchAllocationTests>)

    # --- Stage 0 of the event-driven UI rewrite: standalone slot-allocator
    #     correctness + microbench. Intentionally has no dependency on the UI
    #     library, Engine, AssetRuntime, or rendering — the allocator is a
    #     self-contained class so the gating bench can iterate quickly.
    add_executable(UISlotAllocatorBench
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/SlotAllocator.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/SlotAllocatorTests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/Tests/SlotAllocatorBench.cpp
    )
    target_include_directories(UISlotAllocatorBench PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Include
    )
    target_link_libraries(UISlotAllocatorBench PRIVATE
        GTest::gtest GTest::gtest_main
    )
    set_target_properties(UISlotAllocatorBench PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "UI/Tests"
        UNITY_BUILD OFF
    )
    ge_set_output_tests(UISlotAllocatorBench)
    ge_stage_runtime_dependencies(UISlotAllocatorBench)
    add_test(NAME UISlotAllocatorBench COMMAND $<TARGET_FILE:UISlotAllocatorBench>)
endif()
