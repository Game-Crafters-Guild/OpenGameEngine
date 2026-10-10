cmake_minimum_required(VERSION 3.20)

# These host-side checks (the runtime patches, the package build, the examples) run on the
# build host, including desktop builds.
if(BUILD_TESTING)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    find_program(GE_WEB_TEST_NODE NAMES node)
    if(Python3_Interpreter_FOUND AND GE_WEB_TEST_NODE)
        add_test(NAME WebMailboxPatch
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tools/Web/test_patch_check_mailbox.py)
        set_tests_properties(WebMailboxPatch PROPERTIES TIMEOUT 60)
        add_test(NAME WebEnvironmentQuery
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tools/Web/test_web_environment_query.py)
        set_tests_properties(WebEnvironmentQuery PROPERTIES TIMEOUT 30)
        add_test(NAME WebPackageBuild
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tools/Web/test_build_web_package.py)
        set_tests_properties(WebPackageBuild PROPERTIES TIMEOUT 120)
        add_test(NAME WebExamples
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tools/Web/test_web_examples.py)
        set_tests_properties(WebExamples PROPERTIES TIMEOUT 30)
        add_test(NAME WebProgressGate
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tools/Web/test_web_progress_gate.py)
        set_tests_properties(WebProgressGate PROPERTIES TIMEOUT 30)
        add_test(NAME WebSiteSnapshot
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tools/Web/test_commit_site_snapshot.py)
        set_tests_properties(WebSiteSnapshot PROPERTIES TIMEOUT 30)
    endif()
endif()

# Runtime smoke executables link the wasm module graph. Keep the host-side
# patch checks above available to native builds.
if(NOT EMSCRIPTEN)
    return()
endif()

# Headless engine-core smoke for the wasm target (web platform plan, Phase 4).
# Boots Foundation + Platform capabilities + JobSystem + ECS — no renderer, no
# scripting — and prints PASS lines. The phase gate runs the .js output under
# node in both threading configs:
#
#   node build/wasm-debug/bin/EngineCoreSmoke.js                # threaded
#   node build/wasm-debug-singlethread/bin/EngineCoreSmoke.js   # inline JobSystem

add_executable(EngineCoreSmoke EngineCoreSmoke.cpp)
target_link_libraries(EngineCoreSmoke PRIVATE Logger Types AssetCore JobSystem ECS Platform Memory)

if(EMSCRIPTEN)
    # Node-runnable output; EXIT_RUNTIME so the process ends when main returns
    # (with live pthreads the runtime otherwise keeps the event loop alive).
    set_target_properties(EngineCoreSmoke PROPERTIES SUFFIX ".js")
    target_link_options(EngineCoreSmoke PRIVATE "SHELL:-s ENVIRONMENT=node" "SHELL:-s EXIT_RUNTIME=1")
endif()

# Full-graph link gate: every wasm engine module in one executable, booting
# what runs headless (no GPU/canvas — device init is environment-gated).
add_executable(EngineFullSmoke EngineFullSmoke.cpp)
target_link_libraries(EngineFullSmoke PRIVATE
    Logger Types AssetCore JobSystem ECS Platform Memory Mathematics Animation GenerationalVector
    FileSystem FileSystemCore FileWatcher Scheduler NumericExpression
    AssetDatabase AssetRuntime Audio Input
    Physics PhysicsECS Pathfinding
    Text GameEngineRendering Graph UI)

if(EMSCRIPTEN)
    # The production Engine target supplies asset types and their runtime
    # dependencies. Link that graph instead of maintaining a partial copy of
    # its source list in this gate.
    target_link_libraries(EngineFullSmoke PRIVATE Engine)
endif()

if(EMSCRIPTEN)
    set_target_properties(EngineFullSmoke PROPERTIES SUFFIX ".js")
    # ENVIRONMENT includes worker for the pthread pool; node is the smoke
    # runner. Larger initial memory: the full graph's static data plus
    # subsystem boot allocations overflow the default 16 MB before growth.
    target_link_options(EngineFullSmoke PRIVATE
        "SHELL:-s ENVIRONMENT=node,worker"
        "SHELL:-s EXIT_RUNTIME=1"
        "SHELL:-s INITIAL_MEMORY=128MB")
endif()

# The WebLibrary module's exports under node: the page module's own ABI objects with a
# headless boot in place of ge_create, driven by WebLibraryAbiSmoke.mjs:
#
#   node Tests/Wasm/WebLibraryAbiSmoke.mjs --module build/<preset>/bin/WebLibraryAbiSmoke.mjs
if(EMSCRIPTEN AND TARGET WebLibraryAbi)
    add_executable(WebLibraryAbiSmoke WebLibraryAbiSmoke.cpp)
    target_link_libraries(WebLibraryAbiSmoke PRIVATE WebLibraryAbi)
    target_include_directories(WebLibraryAbiSmoke PRIVATE ${CMAKE_SOURCE_DIR}/Apps/WebLibrary/Source)
    set_target_properties(WebLibraryAbiSmoke PROPERTIES SUFFIX ".mjs")
    if(GE_WASM_SINGLE_THREAD)
        set(_ge_abi_smoke_env "SHELL:-s ENVIRONMENT=node")
    else()
        set(_ge_abi_smoke_env "SHELL:-s ENVIRONMENT=node,worker")
    endif()
    target_link_options(WebLibraryAbiSmoke PRIVATE
        ${_ge_abi_smoke_env}
        "SHELL:--no-entry"
        "SHELL:-s INITIAL_MEMORY=256MB"
        # smoke_main_thread_mallocs counts through the wrapper (WebLibraryAbiSmoke.cpp).
        "SHELL:-Wl,--wrap=malloc")
endif()

# Browser rendering gate: the engine's WebGPU backend renders a test pattern
# on a real canvas. Serve build/<preset>/bin with Tools/Web/serve.py and open
# RendererBrowserSmoke.html. ASYNCIFY is per-target (device bring-up pumps
# browser promises); the shell provides the #canvas the surface binds to.
if(EMSCRIPTEN)
    set(GE_WEB_SHELL ${CMAKE_SOURCE_DIR}/Tools/Web/shell.html)
    if(NOT GE_WASM_SINGLE_THREAD)
        add_executable(FilePublishBrowserSmoke FilePublishBrowserSmoke.cpp)
        target_link_libraries(FilePublishBrowserSmoke PRIVATE Engine)
        set_target_properties(FilePublishBrowserSmoke PROPERTIES
            SUFFIX ".html" LINK_DEPENDS ${GE_WEB_SHELL})
        target_link_options(FilePublishBrowserSmoke PRIVATE
            "SHELL:-s ENVIRONMENT=web,worker"
            "SHELL:-s EXIT_RUNTIME=1"
            "SHELL:-s WASMFS"
            "SHELL:-s INITIAL_MEMORY=128MB"
            "SHELL:-s PTHREAD_POOL_SIZE=8"
            "SHELL:--shell-file ${GE_WEB_SHELL}")

        add_executable(SvgWorkerBudgetBrowserSmoke SvgWorkerBudgetBrowserSmoke.cpp)
        target_link_libraries(SvgWorkerBudgetBrowserSmoke PRIVATE Engine)
        set_target_properties(SvgWorkerBudgetBrowserSmoke PROPERTIES
            SUFFIX ".html" LINK_DEPENDS ${GE_WEB_SHELL})
        target_link_options(SvgWorkerBudgetBrowserSmoke PRIVATE
            "SHELL:-s ENVIRONMENT=web,worker"
            "SHELL:-s EXIT_RUNTIME=1"
            "SHELL:-s INITIAL_MEMORY=128MB"
            "SHELL:-s PTHREAD_POOL_SIZE=8"
            "SHELL:--shell-file ${GE_WEB_SHELL}")
    endif()

    add_executable(RendererBrowserSmoke RendererBrowserSmoke.cpp)
    target_link_libraries(RendererBrowserSmoke PRIVATE Logger Types Platform GameEngineRendering)
    set_target_properties(RendererBrowserSmoke PROPERTIES SUFFIX ".html" LINK_DEPENDS ${GE_WEB_SHELL})
    if(GE_WASM_SINGLE_THREAD)
        set(_ge_browser_env "SHELL:-s ENVIRONMENT=web")
    else()
        set(_ge_browser_env "SHELL:-s ENVIRONMENT=web,worker")
    endif()
    target_link_options(RendererBrowserSmoke PRIVATE
        ${_ge_browser_env}
        "SHELL:-s ASYNCIFY"
        "SHELL:-s ASYNCIFY_STACK_SIZE=1048576"
        "SHELL:-s INITIAL_MEMORY=128MB"
        "SHELL:--shell-file ${GE_WEB_SHELL}")

    # Persistence gate for the OPFS project mount. Browser-only: OPFS does not
    # exist under node, so this is not one of the node smokes above.
    add_executable(WebStorageBrowserSmoke WebStorageBrowserSmoke.cpp)
    target_link_libraries(WebStorageBrowserSmoke PRIVATE Logger Types Platform)
    set_target_properties(WebStorageBrowserSmoke PROPERTIES
        SUFFIX ".html" LINK_DEPENDS ${GE_WEB_SHELL})
    target_link_options(WebStorageBrowserSmoke PRIVATE
        ${GE_WASM_OPFS_LINK_OPTIONS}
        ${_ge_browser_env}
        "SHELL:--shell-file ${GE_WEB_SHELL}")
endif()

# Exercise cooked terrain WGSL on an actual browser GPU. Native heap32 SPIR-V
# cannot catch WGSL memory-location differences (notably vector component races).
if(EMSCRIPTEN AND TARGET WebEditorShaders)
    set(_cbt_browser_smoke_dir ${CMAKE_BINARY_DIR}/bin/CBTTerrainBrowserSmoke)
    add_custom_target(CBTTerrainBrowserSmoke
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_cbt_browser_smoke_dir}
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/CBTTerrainBrowserSmoke ${_cbt_browser_smoke_dir}
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_BINARY_DIR}/WebEditorShaders/CBT ${_cbt_browser_smoke_dir}/Shaders
        DEPENDS WebEditorShaders
        COMMENT "Staging the production WGSL terrain topology browser regression"
        VERBATIM)
endif()
