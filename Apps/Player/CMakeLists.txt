# Apps/Player/CMakeLists.txt
# Standalone Player runtime — lightweight game executable without Editor.

# Web Player (wasm): the same PlayerApplication driven by the browser's rAF
# loop from WebMain.cpp instead of main.cpp's desktop loop. The canvas comes
# from Tools/Web/shell.html and the WebGPU device binds to it. ASYNCIFY is
# required twice over: device bring-up pumps browser promises (adapter/device
# request), and WebDistLoader fetches the export dist synchronously before the
# engine boots.
#
# This target carries NO content. It is the export template: one binary that
# Tools/Web/export_web_player.py copies verbatim into every dist, which is why
# there is nothing here that reads a project directory.
if(EMSCRIPTEN)
    find_package(nlohmann_json CONFIG REQUIRED)
    set(GE_WEB_SHELL ${CMAKE_SOURCE_DIR}/Tools/Web/shell.html)
    add_executable(WebPlayer
        Source/WebMain.cpp
        Source/WebDistLoader.cpp
        Source/WebDistLoader.h
        Source/WebPackReader.cpp
        Source/WebPackReader.h
        Source/PlayerApplication.cpp
        Source/PlayerApplication.h
    )
    target_include_directories(WebPlayer PRIVATE Source)
    target_compile_features(WebPlayer PRIVATE cxx_std_20)
    target_link_libraries(WebPlayer PRIVATE Engine Types nlohmann_json::nlohmann_json)
    set_target_properties(WebPlayer PROPERTIES SUFFIX ".html" LINK_DEPENDS ${GE_WEB_SHELL})
    if(GE_WASM_SINGLE_THREAD)
        set(_ge_webplayer_env "SHELL:-s ENVIRONMENT=web")
    else()
        set(_ge_webplayer_env "SHELL:-s ENVIRONMENT=web,worker")
    endif()
    target_link_options(WebPlayer PRIVATE
        ${_ge_webplayer_env}
        "SHELL:-s ASYNCIFY"
        "SHELL:-s ASYNCIFY_STACK_SIZE=1048576"
        "SHELL:-s INITIAL_MEMORY=512MB"
        "SHELL:--shell-file ${GE_WEB_SHELL}")
    if(NOT GE_WASM_SINGLE_THREAD)
        # Match the root wasm pool. Jolt steps single-threaded on this ABI;
        # extra pthreads each map the whole SharedArrayBuffer.
        target_link_options(WebPlayer PRIVATE "SHELL:-s PTHREAD_POOL_SIZE=8")
    endif()
    return()
endif()

# On macOS, build as a .app bundle. A bare Mach-O GUI executable launched from a
# terminal gets a degraded application context (no proper activation policy), and
# its window self-closes within ~one frame. Wrapping it in a .app with a generated
# Info.plist makes macOS treat it as a real GUI app so the window stays open.
# WIN32 (no console window) on Windows.
include(${CMAKE_SOURCE_DIR}/cmake/DesktopPlayerSources.cmake)
ge_read_desktop_player_sources("${CMAKE_CURRENT_SOURCE_DIR}" _desktop_player_sources)
list(TRANSFORM _desktop_player_sources PREPEND "Source/")
if(APPLE)
    add_executable(Player MACOSX_BUNDLE ${_desktop_player_sources})
else()
    add_executable(Player WIN32 ${_desktop_player_sources})
endif()
ge_add_production_allocation_hook(Player)

target_include_directories(Player PRIVATE Source)
target_compile_features(Player PRIVATE cxx_std_20)
target_compile_definitions(Player PRIVATE
    $<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:_DEBUG>
)

if(PLAYER_MOVIE_RECORDER)
    target_compile_definitions(Player PRIVATE GE_PLAYER_MOVIE_RECORDER=1)
endif()

if(APPLE)
    # On macOS, link ONLY Engine (libEngine.dylib), exactly like the Editor.
    # RenderServices and GameEngineRendering are OBJECT libraries spliced into Engine
    # via GE_ENGINE_MODULES, and a dylib exports all symbols — so they already resolve
    # from libEngine.dylib. Re-linking them here splices a SECOND copy of those object
    # files into the Player executable, producing duplicate engine globals/singletons
    # (GLFW classes, the Rendering::Utils shader-resolver, GPUScene import-slot tables).
    # That duplication caused the "no shader path resolver configured" crash (resolver
    # installed on one copy, read from the other) and risked split GPU-scene state.
    target_link_libraries(Player PRIVATE Engine)
else()
    # Engine ONLY — the macOS shape, now correct on Windows too: Engine.dll's
    # auto-generated export table covers the RenderServices/GameEngineRendering
    # symbols (the old "must link statically" justification was stale). The
    # previous explicit static links spliced a SECOND copy of the rendering
    # module into Player.exe, duplicating every file-static (shader-path
    # resolver, loader hooks, handle-manager singletons) — the root cause of
    # the black-screen (46caf4506) and no-geometry (cfd335f42) Player bugs.
    # Do NOT re-add them. (GameEngine.Native.dll still carries its own copy
    # for standalone scripting hosts, so the IDevice virtual probes and
    # module-local loader fallbacks remain warranted defense.)
    target_link_libraries(Player PRIVATE Engine)
endif()

# Scripting native shim (GameEngine.Native.dll)
if(TARGET GameEngine.Native)
    target_link_libraries(Player PRIVATE GameEngine.Native)
endif()

# Output to bin/<Config>/Apps/Player/
ge_set_output_app(Player "Player")

# Stage runtime DLLs (vcpkg, nethost, etc.) next to Player.exe
ge_stage_runtime_dependencies(Player)
ge_register_application_directory(Player)

# First-party packages (Packages/eztree, ...) staged next to Player.exe
# for dev runs; packaged games ship them via the build pipeline instead (the
# macOS template scrub below leaves this copy out of every game bundle).
ge_stage_packages(Player)

# Everything the Player stages apart from code (Assets/, ThirdPartyNotices/) goes
# in its staged content directory: beside the executable, or Contents/Resources
# in the macOS bundle. The bundle is the template every macOS game export copies,
# so this is also where an exported game's content lives.
ge_get_staged_content_dir(Player _GE_PLAYER_CONTENT_DIR)
set(_GE_PLAYER_NOTICES_DEST "${_GE_PLAYER_CONTENT_DIR}/ThirdPartyNotices")

if(DEFINED VCPKG_INSTALLED_DIR)
    set(_GE_VCPKG_INSTALLED_DIR "${VCPKG_INSTALLED_DIR}")
elseif(DEFINED _VCPKG_INSTALLED_DIR)
    set(_GE_VCPKG_INSTALLED_DIR "${_VCPKG_INSTALLED_DIR}")
else()
    message(FATAL_ERROR "Third-party notice staging requires VCPKG_INSTALLED_DIR")
endif()
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    message(FATAL_ERROR "Third-party notice staging requires VCPKG_TARGET_TRIPLET")
endif()

# Stage engine shader sources (Adapters, Includes, Surfaces) + compiled
# .shaderpkg/.spv for runtime material variant compilation via
# MaterialBuildService / ShaderComposer. Vertex modifiers ship with their
# owning packages (e.g. Packages/eztree), not from the engine tree.
add_custom_command(TARGET Player POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_GE_PLAYER_CONTENT_DIR}/Assets/Shaders/Adapters"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Adapters"
            "${_GE_PLAYER_CONTENT_DIR}/Assets/Shaders/Adapters"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_GE_PLAYER_CONTENT_DIR}/Assets/Shaders/Includes"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Includes"
            "${_GE_PLAYER_CONTENT_DIR}/Assets/Shaders/Includes"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_GE_PLAYER_CONTENT_DIR}/Assets/Shaders/Surfaces"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Surfaces"
            "${_GE_PLAYER_CONTENT_DIR}/Assets/Shaders/Surfaces"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Particles"
            "${_GE_PLAYER_CONTENT_DIR}/Assets/Shaders/Particles"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_GE_PLAYER_CONTENT_DIR}/Assets/Shaders/GPUFogParticles/Surfaces"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/Engine/Modules/GPUFogParticles/Shaders/Surfaces"
            "${_GE_PLAYER_CONTENT_DIR}/Assets/Shaders/GPUFogParticles/Surfaces"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_GE_PLAYER_CONTENT_DIR}/Assets/Shaders/TerrainGrass"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/Engine/Modules/TerrainGrass/Shaders/TerrainGrass"
            "${_GE_PLAYER_CONTENT_DIR}/Assets/Shaders/TerrainGrass"
    # Ocean GLSL sources compile at runtime (shaderc) through the material adapter
    # pipeline + the sim compute drivers, so the whole source dir must ship.
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_GE_PLAYER_CONTENT_DIR}/Assets/Shaders/Ocean"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Ocean"
            "${_GE_PLAYER_CONTENT_DIR}/Assets/Shaders/Ocean"
    # CBT terrain shader sources + the compiled compute kernel, mirroring the
    # Editor's staging (Apps/Editor/CMakeLists.txt). Three runtime consumers
    # resolve this exact directory from the exe:
    #   - CBTRenderFeature::FindCBTShaderDir loads cbt_kernels.comp.spv from it.
    #   - TerrainRenderFeature::EnsureGpuBakePipeline loads terrain_height_bake.comp
    #     from it, which includes cbt_atlas.glsl and terrain_surface_rules.glsl as
    #     siblings.
    #   - The terrain-grass shaders #include cbt_atlas.glsl out of it under both
    #     spellings in use — "CBT/cbt_atlas.glsl" against the Assets/Shaders include
    #     root (terrain_grass_surface.glsl) and the bare "cbt_atlas.glsl" against
    #     the CBT dir itself (terrain_grass_place.comp).
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_GE_PLAYER_CONTENT_DIR}/Assets/Shaders/CBT"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Shaders/CBT/cbt_vertex_modifier.glsl"
            "${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Shaders/CBT/cbt_surface.glsl"
            "${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Shaders/CBT/cbt_layout.glsl"
            "${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Shaders/CBT/cbt_domain.glsl"
            "${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Shaders/CBT/cbt_atlas.glsl"
            "${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Shaders/CBT/cbt_sculpt.glsl"
            "${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Shaders/CBT/cbt_analytic.glsl"
            "${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Shaders/CBT/terrain_height_bake.comp"
            "${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Shaders/CBT/terrain_surface_rules.glsl"
            "${CMAKE_BINARY_DIR}/Shaders/cbt_kernels.comp.spv"
            "${_GE_PLAYER_CONTENT_DIR}/Assets/Shaders/CBT"
    COMMAND ${CMAKE_COMMAND} "-DPLAYER_CONTENT_DIR=${_GE_PLAYER_CONTENT_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/VerifyParticleShaderStaging.cmake"
    COMMENT "Staging engine shaders (Adapters/Includes/Surfaces/Particles/GPUFogParticles/TerrainGrass/Ocean/CBT) for Player"
    VERBATIM
)

if(BUILD_TESTING)
    add_test(NAME PlayerParticleShaderStaging
        COMMAND ${CMAKE_COMMAND} "-DPLAYER_CONTENT_DIR=${_GE_PLAYER_CONTENT_DIR}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/VerifyParticleShaderStaging.cmake")
endif()

# Shader-graph node helpers. ShaderGraphMaterial lives in the shared Engine
# library the Player links, and compiles a graph material at runtime with this
# directory on the include path — it resolves from the exe, so the Player must
# ship it like any other runtime shader source.
ge_stage_shader_graph_nodes(Player)

# Stage the default UI fonts into the Player's staged Assets. Two consumers
# resolve this exact path in a dev run: GameUIHost's default HUD font load
# (without --asset-root the asset root is the install assets root) and
# UIManager's default-atlas resolver (<install assets root>/Fonts). A
# project that ships its own Assets/Fonts overrides these via --asset-root.
# The staged set mirrors the UI test font staging (ge_add_ui_test, Engine/Modules/UI).
add_custom_command(TARGET Player POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Apps/Editor/Assets/Fonts/Roboto-Regular.ttf"
            "${_GE_PLAYER_CONTENT_DIR}/Assets/Fonts/Roboto-Regular.ttf"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Apps/Editor/Assets/Fonts/RobotoMono-Regular.ttf"
            "${_GE_PLAYER_CONTENT_DIR}/Assets/Fonts/RobotoMono-Regular.ttf"
    COMMENT "Staging default UI fonts (Roboto-Regular, RobotoMono-Regular) for Player"
    VERBATIM
)

# Runtime animation defaults are needed even when the project supplies no profile.
add_custom_target(StagePlayerHumanoidProfile
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_GE_PLAYER_CONTENT_DIR}/Assets/SkeletonProfiles"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Assets/SkeletonProfiles/HumanoidStandard.profile.json"
            "${_GE_PLAYER_CONTENT_DIR}/Assets/SkeletonProfiles/HumanoidStandard.profile.json"
    COMMENT "Staging the runtime humanoid profile for Player"
    VERBATIM)
add_dependencies(Player StagePlayerHumanoidProfile)

# Stage engine-level texture assets into the Player's staged Assets.
# SkyRenderer loads the moon art natively from <install assets root>/Textures/Sky, outside
# the AssetDatabase, so a Player build without this tree renders no moon at all.
# A POST_BUILD copy runs only when the Player relinks: an edited texture stays stale and a
# deleted one is not pruned until then. The dependency-tracked mirror is issue #2473.
add_custom_command(TARGET Player POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_GE_PLAYER_CONTENT_DIR}/Assets/Textures"
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            "${CMAKE_SOURCE_DIR}/Assets/Textures"
            "${_GE_PLAYER_CONTENT_DIR}/Assets/Textures"
    COMMENT "Staging engine texture assets for Player"
    VERBATIM
)

# Stage the engine render pipelines into the Player's staged Assets.
# A Player run from the build tree mounts <install assets root> as 'editor', and the
# pipeline game.config names (ForwardPlus when it names none) resolves from there.
# A packaged game does not take this tree: the export ships only the assigned
# pipeline through AssetCollector::CollectRenderPipeline, and the macOS template
# below drops it.
# A POST_BUILD copy runs only when the Player relinks: an edited .rendergraph stays stale
# and a deleted one is not pruned until then. The dependency-tracked mirror is issue #2473.
add_custom_command(TARGET Player POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_GE_PLAYER_CONTENT_DIR}/Assets/RenderPipelines"
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            "${CMAKE_SOURCE_DIR}/Assets/RenderPipelines"
            "${_GE_PLAYER_CONTENT_DIR}/Assets/RenderPipelines"
    COMMENT "Staging engine render pipelines for Player"
    VERBATIM
)

# Stage third-party notices required by bundled engine modules/assets.
add_custom_command(TARGET Player POST_BUILD
    COMMAND ${CMAKE_COMMAND}
            "-DGE_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DGE_NOTICES_DEST=${_GE_PLAYER_NOTICES_DEST}"
            "-DGE_VCPKG_INSTALLED_DIR=${_GE_VCPKG_INSTALLED_DIR}"
            "-DGE_VCPKG_TARGET_TRIPLET=${VCPKG_TARGET_TRIPLET}"
            -P "${CMAKE_SOURCE_DIR}/cmake/StageThirdPartyNotices.cmake"
    COMMENT "Staging third-party notices for Player"
    VERBATIM
)

# Stage compiled .shaderpkg/.spv files.
# Note: StageShaderPkgs.cmake handles missing SRC_DIR gracefully (empty glob).
# GE_PLAYER_SKIP_SHADERPKG_DEP: cross-build iteration when ShaderReflect cannot run
# under QEMU (e.g. x64 ShaderReflect on arm64 Docker). Shaders must already exist in
# CMAKE_BINARY_DIR/Shaders or be staged from a prior build / Deck package step.
option(GE_PLAYER_SKIP_SHADERPKG_DEP "Skip Player dependency on CompileShaderPkgs" OFF)
if(TARGET CompileShaderPkgs AND NOT GE_PLAYER_SKIP_SHADERPKG_DEP)
    add_dependencies(Player CompileShaderPkgs)
endif()
set(_GE_PLAYER_SHADER_STAGE_SCRIPT "${CMAKE_BINARY_DIR}/cmake/StagePlayerShaders.cmake")
file(WRITE "${_GE_PLAYER_SHADER_STAGE_SCRIPT}" "\
set(SRC_DIR \"${CMAKE_BINARY_DIR}/Shaders\")
set(DST_DIR \"\${PLAYER_SHADER_DST}\")
include(\"${CMAKE_SOURCE_DIR}/cmake/StageShaderPkgs.cmake\")
")
add_custom_command(TARGET Player POST_BUILD
    COMMAND ${CMAKE_COMMAND}
            -DPLAYER_SHADER_DST=${_GE_PLAYER_CONTENT_DIR}/Assets/Shaders
            -P "${_GE_PLAYER_SHADER_STAGE_SCRIPT}"
    COMMENT "Staging compiled shaders (.shaderpkg/.spv) for Player"
    VERBATIM
)

# Stage managed assemblies for scripting (CoreBridge, HotReload runtimeconfig, ABIs and
# Scripting.Runtime, the GameSystem runner ManagedSystemBridge loads — no Editor.Managed)
if(ENABLE_SCRIPTING)
    if(NOT APPLE)
        add_custom_command(TARGET Player POST_BUILD
            # CoreBridge (stable bridge, needed for CLR hosting)
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_SOURCE_DIR}/Managed/CoreBridge/bin/$<CONFIG>/net10.0"
                    "$<TARGET_FILE_DIR:Player>"
            # HotReload (provides the .runtimeconfig.json needed for CLR initialization;
            # the Player doesn't use hot-reload but needs the config to bootstrap the runtime)
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_SOURCE_DIR}/Managed/HotReload/bin/$<CONFIG>/net10.0"
                    "$<TARGET_FILE_DIR:Player>"
            # Scripting ABI surfaces (no editor-specific assemblies)
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_SOURCE_DIR}/Managed/Scripting.ABI/bin/$<CONFIG>"
                    "$<TARGET_FILE_DIR:Player>"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_SOURCE_DIR}/Managed/ECS.ABI/bin/$<CONFIG>"
                    "$<TARGET_FILE_DIR:Player>"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_SOURCE_DIR}/Managed/Input.ABI/bin/$<CONFIG>"
                    "$<TARGET_FILE_DIR:Player>"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_SOURCE_DIR}/Managed/Physics.ABI/bin/$<CONFIG>"
                    "$<TARGET_FILE_DIR:Player>"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_SOURCE_DIR}/Managed/Platform.ABI/bin/$<CONFIG>"
                    "$<TARGET_FILE_DIR:Player>"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_SOURCE_DIR}/Managed/Scripting.Runtime/bin/$<CONFIG>"
                    "$<TARGET_FILE_DIR:Player>"
            COMMENT "Staging managed assemblies (CoreBridge/ABI/Scripting.Runtime) next to Player.exe"
            VERBATIM
        )
    else()
        # macOS: stage the managed assemblies as bundle *resources* (Contents/Resources/
        # Managed), not in Contents/MacOS. Mirrors the Editor: keeping Contents/MacOS
        # code-only avoids codesign treating *.deps.json / *.runtimeconfig.json / managed
        # *.dll as unsigned subcomponents. The CoreCLR host resolves
        # GameEngine.HotReload.runtimeconfig.json from here (same PathResolver as the
        # Editor), which is what the standalone Player was previously missing. Runtime-only
        # set: CoreBridge + HotReload + ABI surfaces + Scripting.Runtime (no Editor.Managed /
        # CompileServerHost). An exported game does not inherit it: the export stages its own.
        ge_stage_managed_assemblies_macos(Player
            "$<TARGET_FILE_DIR:Player>/../Resources/Managed"
            SOURCE_DIRS
                # CoreBridge (runtimeconfig/deps used for CoreCLR initialization)
                "${CMAKE_SOURCE_DIR}/Managed/CoreBridge/bin/$<CONFIG>/net10.0"
                # HotReload (provides the .runtimeconfig.json needed to bootstrap the runtime)
                "${CMAKE_SOURCE_DIR}/Managed/HotReload/bin/$<CONFIG>/net10.0"
                # Scripting ABI surfaces (no editor-specific assemblies)
                "${CMAKE_SOURCE_DIR}/Managed/Scripting.ABI/bin/$<CONFIG>"
                "${CMAKE_SOURCE_DIR}/Managed/ECS.ABI/bin/$<CONFIG>"
                "${CMAKE_SOURCE_DIR}/Managed/Input.ABI/bin/$<CONFIG>"
                "${CMAKE_SOURCE_DIR}/Managed/Physics.ABI/bin/$<CONFIG>"
                "${CMAKE_SOURCE_DIR}/Managed/Platform.ABI/bin/$<CONFIG>"
                # GameSystem runner (ManagedSystemBridge resolves it beside CoreBridge)
                "${CMAKE_SOURCE_DIR}/Managed/Scripting.Runtime/bin/$<CONFIG>"
        )
    endif()
endif()

# The staging above copies out of Managed/*/bin/, so those dotnet builds must
# complete first. HotReloadAssembly is included because both staging branches
# copy Managed/HotReload/bin/... and it is a custom target with no implicit
# ordering against Player, so without this the POST_BUILD copy can race the
# dotnet build under parallel builds.
#
# add_dependencies only orders the build; the POST_BUILD staging re-runs only
# when the Player binary relinks. Listing the stamps as LINK_DEPENDS forces that
# relink on a managed-only change. Ninja and Makefile generators only; the
# Visual Studio generator ignores the property.
if(ENABLE_SCRIPTING)
    set(_ge_player_managed_deps
        CoreBridgeAssembly HotReloadAssembly ScriptingAbiAssembly
        EcsAbiAssembly InputAbiAssembly PhysicsAbiAssembly PlatformAbiAssembly
        ScriptingRuntimeAssembly)
    add_dependencies(Player ${_ge_player_managed_deps})

    set(_ge_player_managed_stamps "")
    foreach(_dep IN LISTS _ge_player_managed_deps)
        list(APPEND _ge_player_managed_stamps "${_GE_MANAGED_STAMP_DIR}/${_dep}.$<CONFIG>.stamp")
    endforeach()
    set_property(TARGET Player APPEND PROPERTY LINK_DEPENDS ${_ge_player_managed_stamps})
endif()

set_target_properties(Player PROPERTIES
    FOLDER "Apps"
)

# ---------------------------------------------------------------------------
# macOS .app bundle: properties + post-build finalization.
# Mirrors the Editor's bundle pipeline (Apps/Editor/CMakeLists.txt) but for the
# Player target, reusing the same shared cmake scripts. Required so the Player
# runs as a real GUI app (window stays open) and so its bundled dylibs + MoltenVK
# resolve via @rpath/../Frameworks.
# ---------------------------------------------------------------------------
if(APPLE)
    set_target_properties(Player PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "Open Engine Player"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.gameengine.player"
        MACOSX_BUNDLE_BUNDLE_VERSION "${GE_ENGINE_VERSION_NUMERIC}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${GE_ENGINE_VERSION_NUMERIC}"
        MACOSX_BUNDLE_INFO_STRING "Open Engine Player ${GE_ENGINE_VERSION}"
        # Load bundled dylibs from Contents/Frameworks (staged by
        # ge_stage_runtime_dependencies). BUILD_WITH_INSTALL_RPATH keeps dev
        # builds run from the build tree consistent with the staged layout.
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path/../Frameworks"
        INSTALL_RPATH_USE_LINK_PATH FALSE
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.gameengine.player"
        # Re-sign explicitly as the final post-build step (see CodesignMacAppBundle
        # below); disable Xcode's built-in CodeSign so it doesn't run before our
        # staging/fixup mutates the bundle.
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
        XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
    )

    # Stage MoltenVK (Vulkan-on-Metal) into the bundle at build time so launches
    # succeed even when VULKAN_SDK wasn't exported during configure. Uses the same
    # shared override cache vars as the Editor (empty = auto-detect SDK/Homebrew).
    option(GE_STAGE_MOLTENVK_IN_PLAYER_BUNDLE "Stage MoltenVK into Player.app" ON)
    set(GE_MOLTENVK_SOURCE_LIB "" CACHE FILEPATH "Explicit libMoltenVK.dylib to stage into app bundles before auto-detected SDK copies")
    set(GE_MOLTENVK_SOURCE_ICD "" CACHE FILEPATH "Explicit MoltenVK_icd.json to stage into app bundles before auto-detected SDK copies")
    if(GE_STAGE_MOLTENVK_IN_PLAYER_BUNDLE)
        add_custom_command(TARGET Player POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                    -DGE_APP_EXE=$<TARGET_FILE:Player>
                    -DGE_REQUIRE_MOLTENVK=ON
                    -DGE_MOLTENVK_VERBOSE=OFF
                    -DGE_MOLTENVK_SOURCE_LIB=${GE_MOLTENVK_SOURCE_LIB}
                    -DGE_MOLTENVK_SOURCE_ICD=${GE_MOLTENVK_SOURCE_ICD}
                    -P "${CMAKE_SOURCE_DIR}/cmake/StageMoltenVkAppBundle.cmake"
            COMMENT "Staging MoltenVK into Player.app bundle (macOS)"
            VERBATIM
        )
    endif()

    # Ensure the MoltenVK ICD JSON is present in the bundle.
    add_custom_command(TARGET Player POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DOUTPUT="$<TARGET_FILE_DIR:Player>/../Resources/vulkan/icd.d/MoltenVK_icd.json"
                -DINPUT=${GE_MOLTENVK_SOURCE_ICD}
                -P "${CMAKE_SOURCE_DIR}/cmake/WriteMoltenVkIcdJson.cmake"
        COMMENT "Ensuring MoltenVK_icd.json exists in Player.app bundle (macOS)"
        VERBATIM
    )

    # GLFW may dlopen libvulkan.1.dylib / libvulkan.dylib by name; ensure those
    # aliases exist in Contents/Frameworks.
    add_custom_command(TARGET Player POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DGE_APP_EXE=$<TARGET_FILE:Player>
                -P "${CMAKE_SOURCE_DIR}/cmake/FixupVulkanLoaderNames.cmake"
        COMMENT "Fixing Vulkan loader dylib aliases in Player.app bundle (macOS)"
        VERBATIM
    )

    # Fix dylib install names / rpaths so the bundled exe resolves its
    # dependencies from Contents/Frameworks, then validate, then codesign as the
    # final mutation (Apple Silicon requires a valid — even ad-hoc — signature
    # after install_name_tool edits).
    # Running the Player from inside the bundle writes a shader cache to
    # Contents/MacOS/.Cache (the Player's workspace root is the exe dir). On the
    # next build, codesign rejects that stray cache subcomponent ("bundle format
    # unrecognized"). Remove it before fixup/codesign so build-after-run is robust.
    add_custom_command(TARGET Player POST_BUILD
        COMMAND /bin/sh -c "chmod -R u+w \"$<TARGET_FILE_DIR:Player>/.Cache\" 2>/dev/null; rm -rf \"$<TARGET_FILE_DIR:Player>/.Cache\" || true"
        COMMENT "Removing stale runtime .Cache from Player.app before codesign (macOS)"
        VERBATIM
    )

    option(GE_FIXUP_MAC_PLAYER_BUNDLE "Fix up Player.app dylib install names/rpaths after build" ON)
    if(GE_FIXUP_MAC_PLAYER_BUNDLE)
        add_custom_command(TARGET Player POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                    -DGE_APP_EXE=$<TARGET_FILE:Player>
                    -DGE_FIXUP_VERBOSE=OFF
                    -P "${CMAKE_SOURCE_DIR}/cmake/FixupMacAppBundle.cmake"
            COMMENT "Fixing up Player.app dylib install names/rpaths (macOS)"
            VERBATIM
        )
    endif()
    # NOTE: ValidateMacAppBundle is intentionally NOT run for the Player. Its data
    # layout is the Editor's (Contents/Resources/Assets), but the validator also
    # requires the Editor's managed assembly set (GameEngine.Editor.Scripting.ABI),
    # which the Player's runtime-only set leaves out, so it would false-fail on a
    # valid Player bundle.

    option(GE_CODESIGN_MAC_PLAYER_BUNDLE "Re-sign Player.app after post-build staging" ON)
    set(GE_MAC_CODESIGN_IDENTITY "" CACHE STRING "macOS codesign identity for app bundles; empty uses ad-hoc")
    if(GE_CODESIGN_MAC_PLAYER_BUNDLE)
        add_custom_command(TARGET Player POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                    -DGE_APP_BUNDLE=$<TARGET_BUNDLE_DIR:Player>
                    -DGE_CODESIGN_IDENTITY=${GE_MAC_CODESIGN_IDENTITY}
                    -P "${CMAKE_SOURCE_DIR}/cmake/CodesignMacAppBundle.cmake"
            COMMENT "Re-signing Player.app after post-build staging (macOS)"
            VERBATIM
        )
    endif()

    # Stage this finalized Player.app as the prebuilt TEMPLATE that the Editor's
    # Build pipeline copies and injects user content into (Unity-style packaging:
    # no per-game player recompile). It ships inside the Editor's SDK so a shipped
    # Editor can produce game bundles on a machine without the engine source tree.
    # ditto preserves the bundle's symlinks (libvulkan aliases) and structure;
    # the scrub drops any stale per-project content so the template is generic,
    # the dev-layout render pipelines (an export ships only the assigned one), and
    # the development copy of the engine packages (an export stages the packages a
    # game uses under Contents/Resources/Packages).
    # Built after codesign so the template is a complete, finalized bundle.
    # Editor depends on Player so building the Editor refreshes the template.
    if(TARGET Editor)
        add_dependencies(Editor Player)

        # StageEditorBundleResources signs the existing Editor bundle independently
        # of the Editor link step. Ensure it cannot race this Player POST_BUILD copy,
        # which replaces SDK/templates/Player.app inside that same bundle.
        if(TARGET StageEditorBundleResources)
            add_dependencies(StageEditorBundleResources Player)
        endif()

        set(_ge_player_template "$<TARGET_FILE_DIR:Editor>/SDK/templates/Player.app")
        # The scrub removes files the copied seal lists, so the signed template is
        # re-signed after it with the Player's own signing step.
        set(_ge_player_template_resign "")
        if(GE_CODESIGN_MAC_PLAYER_BUNDLE)
            set(_ge_player_template_resign
                COMMAND ${CMAKE_COMMAND}
                        -DGE_APP_BUNDLE=${_ge_player_template}
                        -DGE_CODESIGN_IDENTITY=${GE_MAC_CODESIGN_IDENTITY}
                        -P "${CMAKE_SOURCE_DIR}/cmake/CodesignMacAppBundle.cmake")
        endif()
        add_custom_command(TARGET Player POST_BUILD
            COMMAND /bin/sh -c "set -e; tpl=\"${_ge_player_template}\"; rm -rf \"$tpl\"; mkdir -p \"$(dirname \"$tpl\")\"; /usr/bin/ditto \"$<TARGET_BUNDLE_DIR:Player>\" \"$tpl\"; rm -rf \"$tpl/Contents/MacOS/.Cache\" \"$tpl/Contents/MacOS/ScriptAssemblies\" \"$tpl/Contents/MacOS/tools\" \"$tpl/Contents/MacOS/AssetDatabase.assetdb\" \"$tpl/Contents/MacOS/Packages\" \"$tpl/Contents/Resources/Assets/RenderPipelines\""
            ${_ge_player_template_resign}
            COMMENT "Staging prebuilt Player.app template into Editor SDK (macOS)"
            VERBATIM
        )

        if(BUILD_TESTING)
            add_test(NAME PlayerMacTemplateLayout
                COMMAND ${CMAKE_COMMAND}
                        "-DPLAYER_APP=$<TARGET_BUNDLE_DIR:Player>"
                        "-DPLAYER_TEMPLATE_APP=${_ge_player_template}"
                        "-DPLAYER_STAGES_MANAGED=${ENABLE_SCRIPTING}"
                        -P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/VerifyMacPlayerTemplateLayout.cmake")
        endif()
    endif()
endif()
