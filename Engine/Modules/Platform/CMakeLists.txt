cmake_minimum_required(VERSION 3.20)

project(Platform LANGUAGES CXX)

set(PLATFORM_SOURCES
	Source/Capabilities.cpp
	Source/Platform.cpp
)

# Desktop-only implementations: GLFW clipboard and gamepad mapping, OS shell
# integration, curl HTTP. The wasm list below carries a web-shaped counterpart
# for each.
if(NOT EMSCRIPTEN)
	list(APPEND PLATFORM_SOURCES
		Source/Window.cpp
		Source/Display.cpp
		Source/Clipboard.cpp
		Source/Gamepad.cpp
		Source/Shell.cpp
		Source/HttpClient.cpp
	)
endif()

# Path-to-menu-tree construction: pure string work, every platform.
list(APPEND PLATFORM_SOURCES Source/ContextMenuBuilder.cpp)

# The request-header rule both HttpClient backends apply.
list(APPEND PLATFORM_SOURCES Source/HttpHeaderRules.cpp)

# A child process's environment, built from this process's: every platform.
list(APPEND PLATFORM_SOURCES Source/Process.cpp)

# Platform-specific sources
if(EMSCRIPTEN)
	# SystemMetrics reuses the Linux TU's constant defaults (600 ms caret
	# half-period, 400 ms double-click) until CSS-driven metrics arrive with
	# the browser UI slice.
	# Window/Display ride emscripten's JS GLFW (-s USE_GLFW=3): the canvas is
	# the one "monitor" and the window. APIs its 3.3 header declares but the
	# JS library does not implement are kept off the link: glfwGetError is
	# shimmed inside Window.cpp, and the gamepad-mapping API gets its own web
	# TU (Gamepad_Web.cpp) that reports no mapping.
	# Native toolbar and context menu reuse the Linux no-op TUs, which the
	# editor already runs against: the engine draws its own menus, so nothing
	# is lost by having no OS chrome to install them into. Shell, clipboard
	# and WebView have no equivalent at all and get web TUs that report.
	# WebPersistentStorage's OPFS backend lives in WasmFS, so its object is
	# only pulled out of this archive by an executable that calls it — and such
	# an executable must link -sWASMFS, or the link fails naming the backend.
	list(APPEND PLATFORM_SOURCES
		Source/Window.cpp
		Source/Display.cpp
		Source/Theme_Stub.cpp
		Source/HttpClient_Emscripten.cpp
		Source/SystemMetrics_Linux.cpp
		Source/SystemFonts_Web.cpp
		Source/Toolbar_Stub.cpp
		Source/ContextMenu_Stub.cpp
		Source/Clipboard_Web.cpp
		Source/Gamepad_Web.cpp
		Source/Shell_Web.cpp
		Source/WebView_Web.cpp
		Source/WebEnvironment_Web.cpp
		Source/WebPersistentStorage.cpp
	)
elseif (WIN32)
	list(APPEND PLATFORM_SOURCES
		Source/OverlayWindow_Win32.cpp
		Source/Toolbar_Win32.cpp
		Source/ContextMenu_Win32.cpp
		Source/MenuIconBitmap_Win32.cpp
		Source/Theme_Win32.cpp
        Source/SystemFonts_Win32.cpp
        Source/SystemMetrics_Win32.cpp
		Source/WebView_Win32.cpp
	)
elseif(APPLE)
	list(APPEND PLATFORM_SOURCES
		Source/Toolbar_mac.mm
		Source/ContextMenu_mac.mm
		Source/Theme_Stub.cpp
        Source/SystemFonts_mac.mm
        Source/SystemMetrics_mac.mm
        Source/Shell_mac.mm
        Source/ScreenColor_mac.mm
        Source/Display_mac.mm
		Source/WebView_mac.mm
	)
else()
	list(APPEND PLATFORM_SOURCES
		Source/Toolbar_Stub.cpp
		Source/ContextMenu_Stub.cpp
		Source/Theme_Stub.cpp
        Source/SystemFonts_Linux.cpp
        Source/SystemMetrics_Linux.cpp
		Source/WebView_Linux.cpp
	)
endif()

add_library(Platform OBJECT ${PLATFORM_SOURCES})

target_compile_features(Platform PUBLIC cxx_std_20)

# Public headers now live under Engine/Modules/Platform/Include
# Keep Engine/Include for existing headers like Platform/Platform.h
# so includes like "Platform/Platform.h" continue to work.
target_include_directories(Platform
    PUBLIC
        ${CMAKE_SOURCE_DIR}/Engine/Modules/Platform/Include
        ${CMAKE_SOURCE_DIR}/Engine/Include
    PRIVATE
        Source
        # Capabilities.cpp reads the JobSystem's blocking-thread ceiling.
        ${CMAKE_SOURCE_DIR}/Engine/Modules/JobSystem/Include
)

# Dependencies
# Logger is used by Platform.cpp
target_link_libraries(Platform PUBLIC Logger Types)
if(NOT EMSCRIPTEN)
    find_package(glfw3 CONFIG REQUIRED)
    find_package(CURL REQUIRED)
    target_link_libraries(Platform PRIVATE glfw CURL::libcurl)
endif()

 if (WIN32)
	    # Required for AssocQueryStringW and related shell helpers used in
	    # OpenScriptWithProject, and for DwmSetWindowAttribute used by
	    # the theme helpers. dxgi: per-monitor HDR luminance via IDXGIOutput6::GetDesc1.
	    target_link_libraries(Platform PRIVATE Shlwapi Dwmapi dwrite ole32 uuid User32 dxgi)
 endif()

if (APPLE)
    target_link_libraries(Platform PRIVATE "-framework CoreText" "-framework CoreFoundation" "-framework Cocoa" "-framework WebKit")
endif()

# Emscripten sets UNIX; keep it out of the Linux desktop-integration probes.
if (UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
    if (CMAKE_CROSSCOMPILING)
        # Do not let CMake pick a host-architecture Fontconfig library while
        # cross-compiling a Linux player from an Apple Silicon Docker host.
        target_compile_definitions(Platform PRIVATE GE_HAVE_FONTCONFIG=0)
    else()
        # Use CMake's built-in FindFontconfig instead of pkg-config to avoid
        # vcpkg debug .pc files injecting debug-suffixed library names
        # (-lfreetyped, -lbz2d, -lpng16d) that don't exist on the system.
        find_package(Fontconfig QUIET)
        if (Fontconfig_FOUND)
            target_compile_definitions(Platform PRIVATE GE_HAVE_FONTCONFIG=1)
            target_link_libraries(Platform PRIVATE Fontconfig::Fontconfig)
        else()
            target_compile_definitions(Platform PRIVATE GE_HAVE_FONTCONFIG=0)
        endif()
    endif()

    # WebKitGTK still needs pkg-config (not affected by vcpkg contamination)
    find_package(PkgConfig QUIET)
    if (PkgConfig_FOUND)
        pkg_check_modules(WEBKIT2GTK QUIET webkit2gtk-4.0)
        if (WEBKIT2GTK_FOUND)
            target_compile_definitions(Platform PRIVATE GE_HAVE_WEBKITGTK=1)
            target_include_directories(Platform PRIVATE ${WEBKIT2GTK_INCLUDE_DIRS})
            target_link_libraries(Platform PRIVATE ${WEBKIT2GTK_LIBRARIES})
        else()
            target_compile_definitions(Platform PRIVATE GE_HAVE_WEBKITGTK=0)
        endif()
    else()
        target_compile_definitions(Platform PRIVATE GE_HAVE_WEBKITGTK=0)
    endif()
    # X11 is needed for direct Xlib calls in Window.cpp (cursor, always-on-top, screen color)
    find_package(X11 QUIET)
    if (X11_FOUND)
        target_link_libraries(Platform PRIVATE X11::X11)
    endif()
endif()

if(MSVC)
    target_compile_options(Platform PRIVATE /W4)
    # Prevent parallel compilation from contending on Platform.pdb (C1041).
    target_compile_options(Platform PRIVATE /FS)
else()
    target_compile_options(Platform PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Unity builds can aggregate Platform sources into a single TU where the
# legacy GameEngine::Platform class (from Engine/Include) and the
# GameEngine::Platform namespace-based APIs (Window/Toolbar) would share the
# same identifier, which MSVC rejects. Disable unity builds for this target
# while keeping them available elsewhere.
if(CMAKE_UNITY_BUILD)
    set_target_properties(Platform PROPERTIES UNITY_BUILD OFF)
endif()

# Tests for Platform module (ContextMenuBuilder, etc.)
if(BUILD_TESTING)
    find_package(GTest REQUIRED)

    add_executable(PlatformContextMenuTests
        Tests/ContextMenuBuilderTests.cpp
        Tests/LinuxDialogPathsTests.cpp
    )

    # Modules are OBJECT libs aggregated into Engine.dll; link Engine to
    # resolve module symbols via the import lib (same pattern as Input /
    # PlatformServices). Linking Platform directly leaves Logger's objects
    # off the link line and the exe fails with unresolved Logger internals.
    target_link_libraries(PlatformContextMenuTests
        PRIVATE
            Engine
            GTest::gtest
            GTest::gtest_main
    )

    # LinuxDialogPaths.h lives in Source/ with the dialog implementation. The
    # Win32 menu-icon decode does too: both are native-shell details exercised
    # here rather than exported for the sake of a test.
    target_include_directories(PlatformContextMenuTests PRIVATE Source)

    if (WIN32)
        target_sources(PlatformContextMenuTests PRIVATE Tests/MenuIconBitmapTests.cpp)

        # One PNG and one SVG of the editor's real artwork, staged at the same
        # relative path the editor mount uses, so the test resolves
        # `editor:Icons/...` exactly as the running menu does.
        add_custom_command(TARGET PlatformContextMenuTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:PlatformContextMenuTests>/Assets/Icons"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/Apps/Editor/Assets/Icons/save.png"
                "${CMAKE_SOURCE_DIR}/Apps/Editor/Assets/Icons/DockLog.svg"
                "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures/menu-icon-orientation.png"
                "$<TARGET_FILE_DIR:PlatformContextMenuTests>/Assets/Icons/"
            COMMENT "Staging menu icon fixtures for PlatformContextMenuTests"
            VERBATIM)
    endif()

    ge_set_output_tests(PlatformContextMenuTests)
    ge_stage_runtime_dependencies(PlatformContextMenuTests)
    add_test(NAME PlatformContextMenuTests COMMAND PlatformContextMenuTests)

    add_executable(PlatformSystemMetricsTests
        Tests/SystemMetricsTests.cpp
    )

    target_link_libraries(PlatformSystemMetricsTests
        PRIVATE
            Engine
            GTest::gtest
            GTest::gtest_main
    )

    ge_set_output_tests(PlatformSystemMetricsTests)
    ge_stage_runtime_dependencies(PlatformSystemMetricsTests)
    add_test(NAME PlatformSystemMetricsTests COMMAND PlatformSystemMetricsTests)

    add_executable(PlatformCapabilitiesTests
        Tests/CapabilitiesTests.cpp
    )

    target_link_libraries(PlatformCapabilitiesTests
        PRIVATE
            Engine
            GTest::gtest
            GTest::gtest_main
    )

    ge_set_output_tests(PlatformCapabilitiesTests)
    ge_stage_runtime_dependencies(PlatformCapabilitiesTests)
    add_test(NAME PlatformCapabilitiesTests COMMAND PlatformCapabilitiesTests)

    # The loopback HTTP server (TestSupport/LoopbackServer.h) every suite that
    # drives HttpClient end to end serves its replies from. INTERFACE = include
    # dir and the socket library only.
    add_library(PlatformTestSupport INTERFACE)
    target_include_directories(PlatformTestSupport INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/TestSupport)
    if (WIN32)
        target_link_libraries(PlatformTestSupport INTERFACE Ws2_32)
    endif()

    # Drives HttpClient against a loopback listener the test itself runs.
    add_executable(PlatformHttpClientTests
        Tests/HttpClientTests.cpp
    )

    target_link_libraries(PlatformHttpClientTests
        PRIVATE
            Engine
            PlatformTestSupport
            GTest::gtest
            GTest::gtest_main
    )

    # HttpHeaderRules.h lives in Source/ beside both backends that apply it.
    target_include_directories(PlatformHttpClientTests PRIVATE Source)

    ge_set_output_tests(PlatformHttpClientTests)
    ge_stage_runtime_dependencies(PlatformHttpClientTests)
    add_test(NAME PlatformHttpClientTests COMMAND PlatformHttpClientTests)

    add_executable(PlatformEnvironmentTests
        Tests/EnvironmentTests.cpp
    )

    target_link_libraries(PlatformEnvironmentTests
        PRIVATE
            Engine
            GTest::gtest
            GTest::gtest_main
    )

    ge_set_output_tests(PlatformEnvironmentTests)
    ge_stage_runtime_dependencies(PlatformEnvironmentTests)
    add_test(NAME PlatformEnvironmentTests COMMAND PlatformEnvironmentTests)

    # Dependency regression for the Win32 patch carried by cmake/ports/glfw3
    # (active-window ownership in modifier-release polling). It links the same
    # GLFW the engine links and nothing else, so it also proves the staged
    # glfw3.dll is the patched build. MSVC only: the harness turns an access
    # violation inside glfwPollEvents into a failing exit status with SEH.
    if(WIN32 AND MSVC)
        add_executable(GlfwWindowOwnershipTests Tests/GlfwWindowOwnershipTests.cpp)
        target_compile_features(GlfwWindowOwnershipTests PRIVATE cxx_std_20)
        target_compile_definitions(GlfwWindowOwnershipTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_link_libraries(GlfwWindowOwnershipTests PRIVATE glfw User32)
        ge_set_output_tests(GlfwWindowOwnershipTests)
        ge_stage_runtime_dependencies(GlfwWindowOwnershipTests)
        foreach(_case local_modifiers ctrl_keyup no_active non_glfw property_sentinel borrowed_property foreign_glfw)
            add_test(NAME GlfwWindowOwnership.${_case}
                COMMAND GlfwWindowOwnershipTests --case ${_case})
            set_tests_properties(GlfwWindowOwnership.${_case} PROPERTIES
                TIMEOUT 30 RUN_SERIAL TRUE LABELS "Platform;GLFW;Win32;Dependency")
        endforeach()

        # Dependency regression for the Win32 button-coordinates patch in
        # cmake/ports/glfw3, on the same terms as the ownership one above.
        add_executable(GlfwButtonCoordinatesTests Tests/GlfwButtonCoordinatesTests.cpp)
        target_compile_features(GlfwButtonCoordinatesTests PRIVATE cxx_std_20)
        target_compile_definitions(GlfwButtonCoordinatesTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_link_libraries(GlfwButtonCoordinatesTests PRIVATE glfw User32)
        ge_set_output_tests(GlfwButtonCoordinatesTests)
        ge_stage_runtime_dependencies(GlfwButtonCoordinatesTests)
        foreach(_case buttons release_point repeated_position relative_modes)
            add_test(NAME GlfwButtonCoordinates.${_case}
                COMMAND GlfwButtonCoordinatesTests --case ${_case})
            set_tests_properties(GlfwButtonCoordinates.${_case} PROPERTIES
                TIMEOUT 30 RUN_SERIAL TRUE LABELS "Platform;GLFW;Win32;Dependency")
        endforeach()

        # The engine half of that contract: Window's button thunk keeps the
        # point GLFW reported for the press instead of re-querying the cursor.
        add_executable(PlatformWindowButtonTests Tests/WindowButtonPositionTests.cpp)
        target_compile_definitions(PlatformWindowButtonTests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
        target_link_libraries(PlatformWindowButtonTests PRIVATE Engine glfw User32 GTest::gtest GTest::gtest_main)
        ge_set_output_tests(PlatformWindowButtonTests)
        ge_stage_runtime_dependencies(PlatformWindowButtonTests)
        add_test(NAME PlatformWindowButtonTests COMMAND PlatformWindowButtonTests)
        set_tests_properties(PlatformWindowButtonTests PROPERTIES RUN_SERIAL TRUE)
    endif()
endif()


message(STATUS "Platform: Configured as static library")
