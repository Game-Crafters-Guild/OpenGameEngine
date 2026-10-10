# GameEngineSDK.cmake
#
# Stages an Engine SDK (headers + static libs + cmake config + Player template)
# alongside a host target (typically the Editor) so the build pipeline can
# compile Player executables without access to the engine source tree.
#
# Usage:
#   include(${CMAKE_SOURCE_DIR}/cmake/GameEngineSDK.cmake)
#   ge_stage_engine_sdk(Editor "$<TARGET_FILE_DIR:Editor>/SDK")

# All engine module targets whose public headers must be included in the SDK.
set(_GE_SDK_MODULE_TARGETS
    Engine
    RenderServices
    GameEngineRendering
    Logger
    Types
    AssetCore
    AssetDatabase
    AssetRuntime
    JobSystem
    ECS
    FileWatcher
    Audio
    Scripting
    Platform
    Memory
    Mathematics
    UI
    Text
    Physics
    PhysicsECS
    Input
    GenerationalVector
    Scheduler
    VCSIntegration
    NativeScripting
)

# Collect source-tree include directories (not vcpkg, not build-tree generated).
function(_ge_sdk_collect_source_include_dirs out_var)
    set(_dirs "")
    foreach(_target IN LISTS _GE_SDK_MODULE_TARGETS)
        if(NOT TARGET ${_target})
            continue()
        endif()
        get_target_property(_inc ${_target} INTERFACE_INCLUDE_DIRECTORIES)
        if(_inc)
            foreach(_d IN LISTS _inc)
                # Unwrap $<BUILD_INTERFACE:path> — modules vary in how they declare their
                # public Include dir, and the bare-path branch below would otherwise skip
                # any module using this genex form (e.g. GenerationalVector).
                if(_d MATCHES "^\\$<BUILD_INTERFACE:(.+)>$")
                    set(_d "${CMAKE_MATCH_1}")
                endif()
                # Skip any remaining generator expressions (e.g. $<INSTALL_INTERFACE:...>)
                string(FIND "${_d}" "$<" _pos)
                if(NOT _pos EQUAL -1)
                    continue()
                endif()
                # Only include source-tree dirs (not vcpkg, not build dirs)
                string(FIND "${_d}" "${CMAKE_SOURCE_DIR}" _src_pos)
                if(_src_pos EQUAL 0 AND IS_DIRECTORY "${_d}")
                    # Only /Include/ dirs (public headers, not Source/)
                    string(FIND "${_d}" "/Include" _inc_pos)
                    if(_inc_pos GREATER -1)
                        list(APPEND _dirs "${_d}")
                    endif()
                endif()
            endforeach()
        endif()
    endforeach()

    # Belt-and-suspenders: stage EVERY engine module's public Include dir. Module
    # CMakeLists vary in how (or whether) they expose include dirs as target properties,
    # so globbing the conventional Engine/Modules/*/Include guarantees the SDK headers are
    # complete for native script compilation on a machine without the engine source tree.
    file(GLOB _module_include_dirs LIST_DIRECTORIES true "${CMAKE_SOURCE_DIR}/Engine/Modules/*/Include")
    foreach(_md IN LISTS _module_include_dirs)
        if(IS_DIRECTORY "${_md}")
            list(APPEND _dirs "${_md}")
        endif()
    endforeach()

    # Also add Engine/Include and ECSModules includes directly
    if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/Engine/Include")
        list(APPEND _dirs "${CMAKE_SOURCE_DIR}/Engine/Include")
    endif()
    if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/Engine/ECSModules/Rendering/Include")
        list(APPEND _dirs "${CMAKE_SOURCE_DIR}/Engine/ECSModules/Rendering/Include")
    endif()
    if(IS_DIRECTORY "${CMAKE_SOURCE_DIR}/Engine/ECSModules/Audio/Include")
        list(APPEND _dirs "${CMAKE_SOURCE_DIR}/Engine/ECSModules/Audio/Include")
    endif()

    list(REMOVE_DUPLICATES _dirs)
    set(${out_var} "${_dirs}" PARENT_SCOPE)
endfunction()

# Generate GameEngineConfig.cmake at configure time.
function(_ge_sdk_generate_config output_path)
    # Collect PUBLIC compile definitions
    set(_public_defs "")
    foreach(_target IN LISTS _GE_SDK_MODULE_TARGETS)
        if(NOT TARGET ${_target})
            continue()
        endif()
        get_target_property(_defs ${_target} INTERFACE_COMPILE_DEFINITIONS)
        if(_defs)
            foreach(_d IN LISTS _defs)
                string(FIND "${_d}" "$<" _pos)
                if(_pos EQUAL -1)
                    list(APPEND _public_defs "${_d}")
                endif()
            endforeach()
        endif()
    endforeach()
    list(REMOVE_DUPLICATES _public_defs)

    if(WIN32)
        set(_platform_libs "Shlwapi\;Dwmapi\;ole32\;Ws2_32\;Iphlpapi\;Dwrite")
    elseif(APPLE)
        # TODO: Add macOS frameworks (Cocoa, IOKit, CoreFoundation, CoreVideo, QuartzCore)
        set(_platform_libs "")
    else()
        # TODO: Add Linux system libs (pthread, dl, m, rt, X11 or xcb)
        set(_platform_libs "")
    endif()

    # Build the definitions lines
    set(_def_lines "")
    foreach(_def IN LISTS _public_defs)
        string(APPEND _def_lines "target_compile_definitions(GameEngine::Engine INTERFACE ${_def})\n")
    endforeach()

    # The vcpkg installed dir is used below ONLY to auto-detect the triplet. It is
    # deliberately NOT baked into the generated config: that file ships with the SDK,
    # and a build-machine path inside it is dead on every other machine (#370).
    set(_vcpkg_installed_dir "")
    if(DEFINED VCPKG_INSTALLED_DIR)
        set(_vcpkg_installed_dir "${VCPKG_INSTALLED_DIR}")
    elseif(DEFINED _VCPKG_INSTALLED_DIR)
        set(_vcpkg_installed_dir "${_VCPKG_INSTALLED_DIR}")
    endif()

    # Capture the vcpkg triplet used during engine build
    if(DEFINED VCPKG_TARGET_TRIPLET)
        set(_vcpkg_triplet "${VCPKG_TARGET_TRIPLET}")
    elseif(_vcpkg_installed_dir AND IS_DIRECTORY "${_vcpkg_installed_dir}")
        # Detect from installed dir structure (first subdirectory that isn't vcpkg metadata)
        file(GLOB _triplet_candidates "${_vcpkg_installed_dir}/*")
        foreach(_cand IN LISTS _triplet_candidates)
            if(IS_DIRECTORY "${_cand}")
                get_filename_component(_cand_name "${_cand}" NAME)
                if(NOT _cand_name STREQUAL "vcpkg")
                    set(_vcpkg_triplet "${_cand_name}")
                    break()
                endif()
            endif()
        endforeach()
    endif()

    if(NOT DEFINED _vcpkg_triplet)
        set(_vcpkg_triplet "x64-windows")
    endif()

    # nethost import lib: staged into the SDK (see ge_stage_engine_sdk) so the config
    # references it SDK-relative instead of this machine's dotnet packs directory.
    # Only the file NAME is baked (platform-dependent: nethost.lib / libnethost.a).
    set(_nethost_ref "")
    if(DEFINED NETHOST_LIBRARY AND EXISTS "${NETHOST_LIBRARY}")
        get_filename_component(_nethost_name "${NETHOST_LIBRARY}" NAME)
        set(_nethost_ref "\${_GE_SDK_DIR}/lib/nethost/${_nethost_name}")
    endif()

    file(WRITE "${output_path}" "\
# GameEngineConfig.cmake — auto-generated find_package() config for the GameEngine SDK.
# Usage: find_package(GameEngine REQUIRED PATHS <sdk>/cmake NO_DEFAULT_PATH)
cmake_minimum_required(VERSION 3.25)

if(TARGET GameEngine::Engine)
    return()
endif()

# SDK version (from project(GameEngine VERSION ...))
set(GameEngine_VERSION \"${PROJECT_VERSION}\")
set(GameEngine_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(GameEngine_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(GameEngine_VERSION_PATCH ${PROJECT_VERSION_PATCH})

get_filename_component(_GE_SDK_DIR \"\${CMAKE_CURRENT_LIST_DIR}/..\" ABSOLUTE)

# vcpkg triplet used during the engine build
set(_GE_VCPKG_TRIPLET \"${_vcpkg_triplet}\")

# vcpkg dependencies. Engine is a shared library that carries every vcpkg
# dependency it uses, and the vcpkg headers the engine headers include are staged
# at <sdk>/vcpkg-include, so a game's Player configures and links against the SDK
# alone: an editor outside its build tree exports with no vcpkg tree at all.
# A vcpkg installed tree is optional (layout: <dir>/<triplet>/lib and
# <dir>/<triplet>/debug/lib):
#     -DGAMEENGINE_VCPKG_INSTALLED=<vcpkg_installed dir>
# When given, its libraries are linked too, so game C++ compiled into the Player
# can call a vcpkg library the engine does not export; a tree staged into the SDK
# at <sdk>/vcpkg is taken when present. The Editor passes the build tree's (its
# build.vcpkgInstalledDir project setting, or the tree it runs from in a
# development build), which also supplies the other CRT flavor's runtime DLLs for
# a cross-flavor package. No build-machine path is baked into this file.
if(NOT DEFINED GAMEENGINE_VCPKG_INSTALLED AND IS_DIRECTORY \"\${_GE_SDK_DIR}/vcpkg/\${_GE_VCPKG_TRIPLET}/lib\")
    set(GAMEENGINE_VCPKG_INSTALLED \"\${_GE_SDK_DIR}/vcpkg\")
endif()
if(DEFINED GAMEENGINE_VCPKG_INSTALLED)
    if(NOT IS_DIRECTORY \"\${GAMEENGINE_VCPKG_INSTALLED}/\${_GE_VCPKG_TRIPLET}/lib\")
        message(FATAL_ERROR \"GameEngine SDK: GAMEENGINE_VCPKG_INSTALLED='\${GAMEENGINE_VCPKG_INSTALLED}' has no '\${_GE_VCPKG_TRIPLET}/lib' directory — not a vcpkg installed tree matching this SDK's triplet. Point it at the engine build's vcpkg_installed directory, or leave it unset.\")
    endif()
    list(APPEND CMAKE_PREFIX_PATH \"\${GAMEENGINE_VCPKG_INSTALLED}/\${_GE_VCPKG_TRIPLET}\" \"\${GAMEENGINE_VCPKG_INSTALLED}/\${_GE_VCPKG_TRIPLET}/share\")
endif()

# Resolve the engine lib directory for the active build config.
# SDK stores libs in lib/<Config>/ (e.g. lib/Debug/, lib/Release/); every config
# built on the staging machine is staged, so cross-config packaging can resolve
# a CRT-compatible flavor. The requested config comes from GAMEENGINE_BUILD_CONFIG
# (engine-driven Player builds pass it — multi-config generators have no
# CMAKE_BUILD_TYPE at configure time) or CMAKE_BUILD_TYPE. Only 'Debug' uses the
# debug CRT (/MDd, _ITERATOR_DEBUG_LEVEL=2): a fallback may only pick a config of
# the SAME CRT flavor — a mixed-CRT player links cleanly (LNK2038 never crosses a
# DLL import) and then dies at static init on the first cross-module STL access.
set(_GE_LIB_DIR \"\")
set(_GE_REQUESTED_CONFIG \"\")
if(DEFINED GAMEENGINE_BUILD_CONFIG AND GAMEENGINE_BUILD_CONFIG)
    set(_GE_REQUESTED_CONFIG \"\${GAMEENGINE_BUILD_CONFIG}\")
elseif(CMAKE_BUILD_TYPE)
    set(_GE_REQUESTED_CONFIG \"\${CMAKE_BUILD_TYPE}\")
endif()
if(_GE_REQUESTED_CONFIG AND IS_DIRECTORY \"\${_GE_SDK_DIR}/lib/\${_GE_REQUESTED_CONFIG}\")
    set(_GE_LIB_DIR \"\${_GE_SDK_DIR}/lib/\${_GE_REQUESTED_CONFIG}\")
else()
    # Enumerate what IS staged (for fallback and for precise error messages).
    set(_GE_STAGED_CONFIGS \"\")
    foreach(_ge_cfg Release RelWithDebInfo MinSizeRel DebugFast Debug)
        if(IS_DIRECTORY \"\${_GE_SDK_DIR}/lib/\${_ge_cfg}\")
            list(APPEND _GE_STAGED_CONFIGS \"\${_ge_cfg}\")
        endif()
    endforeach()
    if(_GE_REQUESTED_CONFIG AND MSVC)
        # Flavor-aware fallback: pick the first staged config with the SAME CRT
        # flavor as the request. Debug is the only debug-CRT config, so it can
        # only fall back to itself; release-CRT requests prefer the most
        # optimized staged flavor.
        if(_GE_REQUESTED_CONFIG STREQUAL \"Debug\")
            set(_GE_FLAVOR_CANDIDATES Debug)
            set(_GE_FLAVOR_NAME \"debug-CRT (/MDd)\")
        else()
            set(_GE_FLAVOR_CANDIDATES Release RelWithDebInfo MinSizeRel DebugFast)
            set(_GE_FLAVOR_NAME \"release-CRT (/MD)\")
        endif()
        foreach(_ge_cfg IN LISTS _GE_FLAVOR_CANDIDATES)
            if(IS_DIRECTORY \"\${_GE_SDK_DIR}/lib/\${_ge_cfg}\")
                set(_GE_LIB_DIR \"\${_GE_SDK_DIR}/lib/\${_ge_cfg}\")
                break()
            endif()
        endforeach()
        if(_GE_LIB_DIR)
            message(WARNING \"GameEngine SDK: no staged libs for requested config '\${_GE_REQUESTED_CONFIG}' — using same-CRT-flavor '\${_GE_LIB_DIR}'. Build the engine in '\${_GE_REQUESTED_CONFIG}' and rebuild the Editor so its SDK stages lib/\${_GE_REQUESTED_CONFIG}.\")
        else()
            string(JOIN \", \" _GE_STAGED_LIST \${_GE_STAGED_CONFIGS})
            if(NOT _GE_STAGED_LIST)
                set(_GE_STAGED_LIST \"none\")
            endif()
            message(FATAL_ERROR \"GameEngine SDK: no staged engine libs use the \${_GE_FLAVOR_NAME} flavor that config '\${_GE_REQUESTED_CONFIG}' needs (staged configs: \${_GE_STAGED_LIST}) — a mixed-CRT player crashes at static init. Build the engine in a \${_GE_FLAVOR_NAME} config and rebuild the Editor so its SDK stages it, or build the player in a staged config's flavor.\")
        endif()
    else()
        list(GET _GE_STAGED_CONFIGS 0 _GE_LIB_FIRST)
        if(_GE_LIB_FIRST)
            set(_GE_LIB_DIR \"\${_GE_SDK_DIR}/lib/\${_GE_LIB_FIRST}\")
            if(_GE_REQUESTED_CONFIG)
                message(WARNING \"GameEngine SDK: no staged libs for requested config '\${_GE_REQUESTED_CONFIG}' — falling back to '\${_GE_LIB_DIR}'.\")
            else()
                message(WARNING \"GameEngine SDK: no build config requested (multi-config generator without GAMEENGINE_BUILD_CONFIG) — guessing '\${_GE_LIB_DIR}'. Pass -DGAMEENGINE_BUILD_CONFIG=<Config> matching the config you will build, or CRT flavors may mismatch.\")
            endif()
        else()
            message(FATAL_ERROR \"GameEngine SDK: no engine libs are staged under '\${_GE_SDK_DIR}/lib/<Config>/' — the SDK is incomplete. Rebuild the Editor so its post-build SDK staging runs.\")
        endif()
    endif()
endif()

# GE_DEBUG_INSTRUMENTATION — the engine's debug-tripwire switch, taken from the
# configuration whose libs were selected above rather than from this consumer's
# own configuration. Public engine headers declare DATA MEMBERS under it, so a
# program that compiles them with the other value lays out engine classes
# differently from the Engine library it links and reads every member past the
# first difference at the wrong offset. NDEBUG cannot carry this: CMake derives
# it from the CONSUMER's configuration, so a Release game linking a DebugFast
# engine would disagree by construction.
get_filename_component(_GE_SELECTED_CONFIG \"\${_GE_LIB_DIR}\" NAME)
# The configuration this consumer links, for the build pipeline: a packaged game ships that
# configuration's engine runtime (BuildPipeline::kLinkConfigFileName, read after the configure).
file(WRITE \"\${CMAKE_BINARY_DIR}/GameEngineLinkConfig.txt\" \"\${_GE_SELECTED_CONFIG}\")
if(_GE_SELECTED_CONFIG STREQUAL \"Debug\" OR _GE_SELECTED_CONFIG STREQUAL \"DebugFast\")
    set(_GE_DEBUG_INSTRUMENTATION 1)
elseif(_GE_SELECTED_CONFIG STREQUAL \"Release\" OR _GE_SELECTED_CONFIG STREQUAL \"RelWithDebInfo\"
       OR _GE_SELECTED_CONFIG STREQUAL \"MinSizeRel\")
    set(_GE_DEBUG_INSTRUMENTATION 0)
else()
    message(FATAL_ERROR \"GameEngine SDK: '\${_GE_SELECTED_CONFIG}' (from '\${_GE_LIB_DIR}') is not one of the engine's configurations (Debug, DebugFast, Release, RelWithDebInfo, MinSizeRel), so the value of GE_DEBUG_INSTRUMENTATION its libs were built with is unknown — and engine classes lay out differently under the two values. Stage libs built in one of those configurations.\")
endif()

# Explicit ship-lib list. Engine is the ONLY engine-built library a shipped game
# links: it is a shared library and every engine module lives inside it. The SDK
# lib dir also stages editor-only artifacts (EditorSDK import lib for native
# Editor-kind package modules, engine runtime DLLs for cross-flavor packaging) —
# globbing the directory used to link those into every shipped game.
if(WIN32)
    set(_GE_SHIP_LIB_NAMES \"Engine.lib\")
elseif(APPLE)
    set(_GE_SHIP_LIB_NAMES \"libEngine.dylib\")
else()
    set(_GE_SHIP_LIB_NAMES \"libEngine.so\")
endif()
set(_GE_ENGINE_LIBS \"\")
foreach(_ge_ship_name IN LISTS _GE_SHIP_LIB_NAMES)
    if(EXISTS \"\${_GE_LIB_DIR}/\${_ge_ship_name}\")
        list(APPEND _GE_ENGINE_LIBS \"\${_GE_LIB_DIR}/\${_ge_ship_name}\")
    else()
        message(FATAL_ERROR \"GameEngine SDK: required engine library '\${_ge_ship_name}' is not staged in '\${_GE_LIB_DIR}' — the SDK is incomplete. Rebuild the Editor so its post-build SDK staging runs.\")
    endif()
endforeach()

# Create the main imported target.
add_library(GameEngine::Engine INTERFACE IMPORTED)

target_include_directories(GameEngine::Engine INTERFACE \"\${_GE_SDK_DIR}/include\")
target_compile_features(GameEngine::Engine INTERFACE cxx_std_20)

# On MSVC, use /WHOLEARCHIVE for Engine.lib to ensure static auto-registrars
# (scene schemas, component registrations) are included even when no symbol is
# directly referenced. Without this, translation units like BuiltInSceneSchemas.cpp
# are silently dropped and scene loading fails with 'no schema registered'.
if(MSVC)
    foreach(_lib IN LISTS _GE_ENGINE_LIBS)
        get_filename_component(_libname \"\${_lib}\" NAME_WE)
        if(_libname MATCHES \"^Engine(-.*)?$\")
            target_link_options(GameEngine::Engine INTERFACE \"/WHOLEARCHIVE:\${_lib}\")
        else()
            target_link_libraries(GameEngine::Engine INTERFACE \"\${_lib}\")
        endif()
    endforeach()
else()
    target_link_libraries(GameEngine::Engine INTERFACE \${_GE_ENGINE_LIBS})
endif()

# The optional vcpkg tree's libraries (see GAMEENGINE_VCPKG_INSTALLED above): every
# library in it, rather than re-finding individual packages (which requires exact
# name matching and can create broken targets without the vcpkg toolchain).
# Config-specific paths avoid Debug/Release runtime library mismatches.
if(DEFINED GAMEENGINE_VCPKG_INSTALLED)
    file(GLOB _GE_VCPKG_LIBS_RELEASE \"\${GAMEENGINE_VCPKG_INSTALLED}/\${_GE_VCPKG_TRIPLET}/lib/*.lib\" \"\${GAMEENGINE_VCPKG_INSTALLED}/\${_GE_VCPKG_TRIPLET}/lib/*.a\")
    file(GLOB _GE_VCPKG_LIBS_DEBUG \"\${GAMEENGINE_VCPKG_INSTALLED}/\${_GE_VCPKG_TRIPLET}/debug/lib/*.lib\" \"\${GAMEENGINE_VCPKG_INSTALLED}/\${_GE_VCPKG_TRIPLET}/debug/lib/*.a\")
    # Filter out shared library import stubs to avoid duplicate symbol errors
    # (e.g., SPIRV-Tools.lib vs SPIRV-Tools-shared.lib).
    foreach(_lib IN LISTS _GE_VCPKG_LIBS_RELEASE)
        get_filename_component(_name \"\${_lib}\" NAME)
        if(NOT _name MATCHES \"-shared\\\\.(lib|a)$\")
            target_link_libraries(GameEngine::Engine INTERFACE \"\$<\$<NOT:\$<CONFIG:Debug>>:\${_lib}>\")
        endif()
    endforeach()
    foreach(_lib IN LISTS _GE_VCPKG_LIBS_DEBUG)
        get_filename_component(_name \"\${_lib}\" NAME)
        if(NOT _name MATCHES \"-shared\\\\.(lib|a)$\")
            target_link_libraries(GameEngine::Engine INTERFACE \"\$<\$<CONFIG:Debug>:\${_lib}>\")
        endif()
    endforeach()
endif()

# vcpkg headers: the copy staged into the SDK itself; the optional vcpkg tree's
# for an SDK staged without it.
if(IS_DIRECTORY \"\${_GE_SDK_DIR}/vcpkg-include\")
    target_include_directories(GameEngine::Engine INTERFACE \"\${_GE_SDK_DIR}/vcpkg-include\")
elseif(DEFINED GAMEENGINE_VCPKG_INSTALLED)
    target_include_directories(GameEngine::Engine INTERFACE
        \"\${GAMEENGINE_VCPKG_INSTALLED}/\${_GE_VCPKG_TRIPLET}/include\")
endif()

# .NET hosting library (nethost) — staged into the SDK next to the engine libs.
set(_GE_NETHOST_LIB \"${_nethost_ref}\")
if(_GE_NETHOST_LIB AND EXISTS \"\${_GE_NETHOST_LIB}\")
    target_link_libraries(GameEngine::Engine INTERFACE \"\${_GE_NETHOST_LIB}\")
    if(MSVC)
        target_link_libraries(GameEngine::Engine INTERFACE delayimp)
        target_link_options(GameEngine::Engine INTERFACE /DELAYLOAD:nethost.dll)
    endif()
endif()

# Vulkan is always needed (system SDK, not vcpkg)
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    target_link_libraries(GameEngine::Engine INTERFACE Vulkan::Vulkan)
endif()

# Platform system libraries
target_link_libraries(GameEngine::Engine INTERFACE ${_platform_libs})

# Compile definitions from engine build
${_def_lines}
# The engine's debug-tripwire switch, resolved from the staged configuration above.
target_compile_definitions(GameEngine::Engine INTERFACE GE_DEBUG_INSTRUMENTATION=\${_GE_DEBUG_INSTRUMENTATION})
message(STATUS \"GameEngine SDK found at \${_GE_SDK_DIR} (engine libs: \${_GE_SELECTED_CONFIG}, GE_DEBUG_INSTRUMENTATION=\${_GE_DEBUG_INSTRUMENTATION})\")
")
endfunction()

# Main staging function — adds post-build commands to copy headers, libs, and templates.
function(ge_stage_engine_sdk host_target sdk_output_dir)
    if(NOT TARGET ${host_target})
        message(FATAL_ERROR "ge_stage_engine_sdk: target '${host_target}' not found")
    endif()

    # Generate config at configure time
    set(_config_path "${CMAKE_BINARY_DIR}/cmake/GameEngineConfig.cmake")
    _ge_sdk_generate_config("${_config_path}")

    # Collect include directories at configure time
    _ge_sdk_collect_source_include_dirs(_sdk_include_dirs)

    set(_ge_sdk_vcpkg_installed_dir "")
    if(DEFINED VCPKG_INSTALLED_DIR)
        set(_ge_sdk_vcpkg_installed_dir "${VCPKG_INSTALLED_DIR}")
    elseif(DEFINED _VCPKG_INSTALLED_DIR)
        set(_ge_sdk_vcpkg_installed_dir "${_VCPKG_INSTALLED_DIR}")
    endif()

    set(_ge_sdk_vcpkg_triplet "")
    if(DEFINED VCPKG_TARGET_TRIPLET)
        set(_ge_sdk_vcpkg_triplet "${VCPKG_TARGET_TRIPLET}")
    elseif(_ge_sdk_vcpkg_installed_dir AND IS_DIRECTORY "${_ge_sdk_vcpkg_installed_dir}")
        file(GLOB _ge_sdk_triplet_candidates "${_ge_sdk_vcpkg_installed_dir}/*")
        foreach(_cand IN LISTS _ge_sdk_triplet_candidates)
            if(IS_DIRECTORY "${_cand}")
                get_filename_component(_cand_name "${_cand}" NAME)
                if(NOT _cand_name STREQUAL "vcpkg")
                    set(_ge_sdk_vcpkg_triplet "${_cand_name}")
                    break()
                endif()
            endif()
        endforeach()
    endif()

    if(NOT _ge_sdk_vcpkg_installed_dir OR NOT _ge_sdk_vcpkg_triplet)
        message(FATAL_ERROR "ge_stage_engine_sdk: third-party notice staging requires vcpkg installed dir and triplet")
    endif()

    # NativeScripting SDK manifest: lets the editor build hot-reloadable user-script DLLs
    # from the staged SDK (exe-relative) instead of the engine source tree. The user DLL
    # links the Engine import lib directly (minimal/fast — NOT find_package(GameEngine),
    # which whole-archives + statically links every vcpkg dep and would make each rebuild
    # slow). Relative paths resolve against the SDK root; cmake/dotnet are dev-tool absolute
    # paths (a shipped editor re-discovers them — C14 packaging). file(GENERATE) expands
    # $<CONFIG> so each config gets its own manifest next to its Editor.exe.
    # EditorSDK interface (editor hosts only): the import lib native
    # Editor-kind package modules link, plus the staged editor headers. The
    # import lib itself is picked up by the lib/$<CONFIG> glob-stage below;
    # include-editor is staged further down.
    set(_ge_sdk_editor_lines "")
    if(TARGET EditorSDK)
        set(_ge_sdk_editor_lines
"editorimportlib=lib/$<CONFIG>/$<TARGET_LINKER_FILE_NAME:EditorSDK>
editorincludes=include-editor
")
    endif()

    file(GENERATE
        OUTPUT "${sdk_output_dir}/nativescripting/manifest.txt"
        CONTENT
"# AUTO-GENERATED NativeScripting SDK manifest. Relative paths resolve against the SDK root (the parent of this dir).
config=$<CONFIG>
includedir=include
importlib=lib/$<CONFIG>/$<TARGET_LINKER_FILE_NAME:Engine>
sdkentry=nativescripting/UserModuleEntry.cpp
scannerdll=nativescripting/ComponentScanner/ComponentScanner.dll
defs=$<TARGET_PROPERTY:Engine,INTERFACE_COMPILE_DEFINITIONS>
globaldefs=GLM_FORCE_DEPTH_ZERO_TO_ONE;NOMINMAX;WIN32_LEAN_AND_MEAN;GE_DEBUG_INSTRUMENTATION=${GE_DEBUG_INSTRUMENTATION_VALUE}
engineincludes=vcpkg-include;vcpkg-include/harfbuzz;generated-include
cmake=${CMAKE_COMMAND}
dotnet=${DOTNET_EXECUTABLE}
${_ge_sdk_editor_lines}")

    # Create the SDK directories
    add_custom_command(TARGET ${host_target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${sdk_output_dir}/include"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${sdk_output_dir}/lib/$<CONFIG>"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${sdk_output_dir}/cmake"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${sdk_output_dir}/templates/Player"
        COMMENT "Creating Engine SDK directories"
        VERBATIM
    )

    # Copy public headers from each module include directory.
    # Use copy_directory for simplicity (copies all files including non-headers,
    # but the directories only contain headers in practice).
    foreach(_dir IN LISTS _sdk_include_dirs)
        if(IS_DIRECTORY "${_dir}")
            # Compute the module-relative portion for the destination.
            # All headers go flat into sdk/include/ preserving their internal structure.
            add_custom_command(TARGET ${host_target} POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_directory "${_dir}" "${sdk_output_dir}/include"
                VERBATIM
            )
        endif()
    endforeach()

    # Editor extension headers (EditorSDK surface) — staged separately from the
    # engine headers so Runtime-kind module builds never see editor includes.
    if(TARGET EditorSDK)
        add_custom_command(TARGET ${host_target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_SOURCE_DIR}/Apps/Editor/Include" "${sdk_output_dir}/include-editor"
            VERBATIM
        )
    endif()

    # Copy engine link/runtime libraries into config-specific subdirectories —
    # for EVERY config present in the build tree, not just the one being built.
    # The SDK directory itself is per-config (it lives beside that config's
    # Editor.exe), so without cross-config staging a Debug editor's SDK could
    # never hold the release-CRT libs a Release packaged game needs. Staging is
    # an explicit allow-list:
    #   - link interface: Engine (games + native user scripts) and EditorSDK
    #     (native Editor-kind package modules). Editor-only import libs and
    #     test libs never reach the SDK any more.
    #   - engine runtime binaries: Engine + GameEngine.Native shared libraries,
    #     so the build pipeline can ship a CRT flavor other than the running
    #     editor's (vcpkg runtime deps resolve per-flavor from the vcpkg tree).
    # Anything else previously staged into lib/<Config> is pruned so stale
    # editor-only libs can't leak into game link lines through old SDKs.
    set(_lib_copy_script "${CMAKE_BINARY_DIR}/cmake/StageSDKLibs_${host_target}.cmake")
    file(WRITE "${_lib_copy_script}" "\
set(_ship_lib_globs \"Engine.lib\" \"libEngine.dylib\" \"libEngine.so\" \"EditorSDK.lib\" \"libEditorSDK.dylib\" \"libEditorSDK.so\")
set(_ship_runtime_globs \"Engine.dll\" \"GameEngine.Native.dll\" \"libGameEngine.Native.dylib\" \"libGameEngine.Native.so\")

# Every config with built libs in this build tree stages into this SDK.
file(GLOB _cfg_dirs LIST_DIRECTORIES true \"\${LIB_ROOT}/*\")
set(_configs \"\${BUILD_CONFIG}\")
foreach(_cfg_dir IN LISTS _cfg_dirs)
    if(IS_DIRECTORY \"\${_cfg_dir}\")
        get_filename_component(_cfg_name \"\${_cfg_dir}\" NAME)
        list(APPEND _configs \"\${_cfg_name}\")
    endif()
endforeach()
list(REMOVE_DUPLICATES _configs)

foreach(_cfg IN LISTS _configs)
    set(_src_dirs \"\${LIB_ROOT}/\${_cfg}\" \"\${BIN_ROOT}/\${_cfg}\")
    if(_cfg STREQUAL \"\${BUILD_CONFIG}\" AND DEFINED EXTRA_LIB_DIR AND IS_DIRECTORY \"\${EXTRA_LIB_DIR}\")
        list(APPEND _src_dirs \"\${EXTRA_LIB_DIR}\")
    endif()
    set(_active_globs \${_ship_lib_globs})
    # On Windows, packaging stages the running config's runtime DLLs from beside
    # Editor.exe. Keep only the SDK import libs for the running config; runtime
    # DLLs from every other built config remain available for players that link it.
    if(NOT (_cfg STREQUAL \"\${BUILD_CONFIG}\" AND OMIT_CURRENT_RUNTIME_COPY))
        list(APPEND _active_globs \${_ship_runtime_globs})
    endif()
    set(_staged_names \"\")
    foreach(_dir IN LISTS _src_dirs)
        foreach(_glob IN LISTS _active_globs)
            file(GLOB _libs \"\${_dir}/\${_glob}\")
            foreach(_lib IN LISTS _libs)
                get_filename_component(_name \"\${_lib}\" NAME)
                file(MAKE_DIRECTORY \"\${SDK_BASE}/lib/\${_cfg}\")
                # Retried: antivirus briefly locks freshly linked DLLs, and a
                # silently skipped copy would leave a stale engine in the SDK.
                set(_copy_rc 1)
                foreach(_attempt RANGE 2)
                    execute_process(COMMAND \"\${CMAKE_COMMAND}\" -E copy_if_different \"\${_lib}\" \"\${SDK_BASE}/lib/\${_cfg}/\${_name}\"
                                    RESULT_VARIABLE _copy_rc ERROR_QUIET OUTPUT_QUIET)
                    if(_copy_rc EQUAL 0)
                        break()
                    endif()
                    execute_process(COMMAND \"\${CMAKE_COMMAND}\" -E sleep 0.5)
                endforeach()
                if(NOT _copy_rc EQUAL 0)
                    message(FATAL_ERROR \"SDK staging: failed to copy '\${_lib}' into '\${SDK_BASE}/lib/\${_cfg}/' after retries — the SDK would carry a stale/missing '\${_name}'\")
                endif()
                list(APPEND _staged_names \"\${_name}\")
            endforeach()
        endforeach()
    endforeach()
    # A macOS Editor bundle already carries its current-config dylibs in
    # Contents/Frameworks for runtime loading. Keep the SDK paths consumers
    # expect, but make them relative symlinks to those runtime copies instead
    # of embedding the same large binaries twice. Other staged configs remain
    # real files because they can contain different build/ABI variants.
    if(_cfg STREQUAL \"\${BUILD_CONFIG}\" AND DEFINED RUNTIME_LIB_DIR
       AND IS_DIRECTORY \"\${RUNTIME_LIB_DIR}\")
        foreach(_staged_name IN LISTS _staged_names)
            if(_staged_name MATCHES \"\\\\.dylib$\"
               AND EXISTS \"\${RUNTIME_LIB_DIR}/\${_staged_name}\")
                set(_sdk_lib \"\${SDK_BASE}/lib/\${_cfg}/\${_staged_name}\")
                file(RELATIVE_PATH _runtime_lib_relative
                    \"\${SDK_BASE}/lib/\${_cfg}\"
                    \"\${RUNTIME_LIB_DIR}/\${_staged_name}\")
                file(REMOVE \"\${_sdk_lib}\")
                file(CREATE_LINK \"\${_runtime_lib_relative}\" \"\${_sdk_lib}\"
                    SYMBOLIC RESULT _link_result)
                if(NOT _link_result STREQUAL \"0\")
                    message(FATAL_ERROR
                        \"SDK staging: failed to link '\${_sdk_lib}' to the bundled runtime \"
                        \"'\${_runtime_lib_relative}': \${_link_result}\")
                endif()
            endif()
        endforeach()
    endif()
    # Prune files the allow-list no longer stages (old glob-staged libs), and
    # drop the config dir entirely when nothing staged into it — an empty
    # lib/<cfg> would read as a staged-but-incomplete config downstream.
    if(IS_DIRECTORY \"\${SDK_BASE}/lib/\${_cfg}\")
        file(GLOB _staged_files \"\${SDK_BASE}/lib/\${_cfg}/*\")
        foreach(_staged IN LISTS _staged_files)
            get_filename_component(_staged_name \"\${_staged}\" NAME)
            if(NOT _staged_name IN_LIST _staged_names)
                file(REMOVE \"\${_staged}\")
            endif()
        endforeach()
        if(NOT _staged_names)
            file(REMOVE_RECURSE \"\${SDK_BASE}/lib/\${_cfg}\")
        endif()
    endif()
endforeach()
")

    # Stage libs post-build (each build refreshes every staged config's libs).
    # Note: no VERBATIM here — MSVC+MSBuild wraps generator expressions in extra
    # quotes when VERBATIM is set, producing paths like "C:/.../SDK"/lib/Debug.
    set(_stage_sdk_lib_args
        -DLIB_ROOT=${CMAKE_BINARY_DIR}/lib
        -DBIN_ROOT=${CMAKE_BINARY_DIR}/bin
        -DSDK_BASE=${sdk_output_dir}
        -DBUILD_CONFIG=$<CONFIG>
    )
    if(APPLE)
        list(APPEND _stage_sdk_lib_args
            -DEXTRA_LIB_DIR=$<TARGET_BUNDLE_DIR:${host_target}>/Contents/Frameworks
            -DRUNTIME_LIB_DIR=$<TARGET_BUNDLE_DIR:${host_target}>/Contents/Frameworks
        )
    elseif(WIN32)
        list(APPEND _stage_sdk_lib_args -DOMIT_CURRENT_RUNTIME_COPY=ON)
    endif()
    add_custom_command(TARGET ${host_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            ${_stage_sdk_lib_args}
            -P ${_lib_copy_script}
        COMMENT "Staging Engine link + runtime libraries to SDK (all built configs)"
    )

    # Copy GameEngineConfig.cmake
    add_custom_command(TARGET ${host_target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_config_path}"
            "${sdk_output_dir}/cmake/GameEngineConfig.cmake"
        VERBATIM
    )

    # nethost import library — staged so the shipped config references it SDK-relative
    # instead of the build machine's dotnet packs directory.
    if(DEFINED NETHOST_LIBRARY AND EXISTS "${NETHOST_LIBRARY}")
        add_custom_command(TARGET ${host_target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${sdk_output_dir}/lib/nethost"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${NETHOST_LIBRARY}" "${sdk_output_dir}/lib/nethost/"
            COMMENT "Staging nethost import library to SDK"
            VERBATIM
        )
    endif()

    # The desktop template and the desktop target share one source manifest.
    # Web entry points are not desktop sources, even in an SDK previously staged
    # by a version that copied the entire Player source directory.
    include(${CMAKE_SOURCE_DIR}/cmake/DesktopPlayerSources.cmake)
    ge_read_desktop_player_sources("${CMAKE_SOURCE_DIR}/Apps/Player" _desktop_player_sources)
    add_custom_command(TARGET ${host_target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${sdk_output_dir}/templates/Player"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Apps/Player/DesktopSources.txt"
            "${sdk_output_dir}/templates/Player/DesktopSources.txt"
        VERBATIM
    )
    foreach(_source IN LISTS _desktop_player_sources)
        get_filename_component(_parent "${_source}" DIRECTORY)
        add_custom_command(TARGET ${host_target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${sdk_output_dir}/templates/Player/${_parent}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_SOURCE_DIR}/Apps/Player/Source/${_source}"
                "${sdk_output_dir}/templates/Player/${_source}"
            VERBATIM
        )
    endforeach()

    # Copy generated CMakeLists.txt template
    add_custom_command(TARGET ${host_target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Apps/Player/GeneratedCMakeLists.txt.in"
            "${sdk_output_dir}/templates/Player/GeneratedCMakeLists.txt.in"
        VERBATIM
    )

    # Third-party notices used by the build pipeline when producing exported players.
    add_custom_command(TARGET ${host_target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DGE_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DGE_NOTICES_DEST=${sdk_output_dir}/ThirdPartyNotices"
            "-DGE_VCPKG_INSTALLED_DIR=${_ge_sdk_vcpkg_installed_dir}"
            "-DGE_VCPKG_TARGET_TRIPLET=${_ge_sdk_vcpkg_triplet}"
            -P "${CMAKE_SOURCE_DIR}/cmake/StageThirdPartyNotices.cmake"
        COMMENT "Staging third-party notices to SDK"
        VERBATIM
    )

    # Default player application icon (same source as the Editor) for the build pipeline.
    add_custom_command(TARGET ${host_target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Apps/Editor/Icon/AppIcon.png"
            "${sdk_output_dir}/AppIcon.png"
        VERBATIM
    )

    # NativeScripting toolchain: the GameSDK umbrella header (so user code can
    # #include <GameSDK/GameSDK.h>) + the user-module ABI entry source (compiled into
    # every user DLL by the generated build). No genex here → VERBATIM is safe.
    add_custom_command(TARGET ${host_target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/Engine/Modules/NativeScripting/GameSDK/Include"
            "${sdk_output_dir}/include"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${sdk_output_dir}/nativescripting"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/Engine/Modules/NativeScripting/GameSDK/Source/UserModuleEntry.cpp"
            "${sdk_output_dir}/nativescripting/UserModuleEntry.cpp"
        COMMENT "Staging NativeScripting GameSDK header + user-module entry source"
        VERBATIM
    )

    # ComponentScanner (.NET) publish dir — copied whole (dll + .deps.json +
    # .runtimeconfig.json) so the editor can run `dotnet ComponentScanner.dll` from the
    # staged SDK. Built by the Engine build (Editor depends on Engine). $<CONFIG> in the
    # source path → NO VERBATIM (MSBuild double-quotes genexes under VERBATIM).
    add_custom_command(TARGET ${host_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/Managed/ComponentScanner/bin/$<CONFIG>/net10.0
            ${sdk_output_dir}/nativescripting/ComponentScanner
        COMMENT "Staging ComponentScanner to SDK ($<CONFIG>)"
    )

    # Managed scripting runtime + source generator (ship-safety C4): the NativeAOT
    # csproj the BuildPipeline generates for packaged games references these STAGED
    # copies — never the engine repo's Managed/ tree, which a relocated or shipped
    # editor does not have. Copy-if-exists: a scripting-disabled engine build has no
    # managed outputs, and the pipeline reports the absence at package time instead.
    set(_ge_sdk_managed_stage_script "${CMAKE_BINARY_DIR}/cmake/StageSDKManagedFile.cmake")
    file(WRITE "${_ge_sdk_managed_stage_script}" "\
if(EXISTS \"\${SRC}\")
    file(MAKE_DIRECTORY \"\${DST}\")
    execute_process(COMMAND \"\${CMAKE_COMMAND}\" -E copy_if_different \"\${SRC}\" \"\${DST}\")
endif()
")
    add_custom_command(TARGET ${host_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DSRC=${CMAKE_SOURCE_DIR}/Managed/Scripting.Runtime/bin/$<CONFIG>/GameEngine.Scripting.Runtime.dll
            -DDST=${sdk_output_dir}/managed -P ${_ge_sdk_managed_stage_script}
        COMMAND ${CMAKE_COMMAND}
            -DSRC=${CMAKE_SOURCE_DIR}/Managed/Scripting.ABI/bin/$<CONFIG>/GameEngine.Scripting.ABI.dll
            -DDST=${sdk_output_dir}/managed -P ${_ge_sdk_managed_stage_script}
        COMMAND ${CMAKE_COMMAND}
            -DSRC=${CMAKE_SOURCE_DIR}/Managed/ECS.ABI/bin/$<CONFIG>/GameEngine.ECS.ABI.dll
            -DDST=${sdk_output_dir}/managed -P ${_ge_sdk_managed_stage_script}
        COMMAND ${CMAKE_COMMAND}
            -DSRC=${CMAKE_SOURCE_DIR}/Managed/SourceGenerators/EntitySystemGenerator/bin/$<CONFIG>/netstandard2.0/EntitySystemGenerator.dll
            -DDST=${sdk_output_dir}/managed/analyzers -P ${_ge_sdk_managed_stage_script}
        COMMENT "Staging managed scripting assemblies to SDK ($<CONFIG>)"
    )

    # Self-containment: the engine's public headers pull in vcpkg headers (glm, nlohmann,
    # concurrentqueue, …) and a few generated headers. Stage them INTO the SDK so a machine
    # without the engine source tree can still compile native C++ user scripts. The manifest
    # references these SDK-relative (vcpkg-include / generated-include). Engine .a libs are
    # NOT needed — libEngine.dylib is self-contained — so only headers are staged.
    set(_ge_vcpkg_include "")
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET AND
       IS_DIRECTORY "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include")
        set(_ge_vcpkg_include "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include")
    else()
        file(GLOB _ge_vcpkg_inc_candidates "${CMAKE_BINARY_DIR}/vcpkg_installed/*/include")
        foreach(_cand IN LISTS _ge_vcpkg_inc_candidates)
            if(IS_DIRECTORY "${_cand}")
                set(_ge_vcpkg_include "${_cand}")
                break()
            endif()
        endforeach()
    endif()

    if(_ge_vcpkg_include)
        # One-time copy (the vcpkg include tree is large and rarely changes); delete
        # <sdk>/vcpkg-include to force a refresh.
        set(_ge_vcpkg_stage_script "${CMAKE_BINARY_DIR}/cmake/StageSDKVcpkgInclude.cmake")
        file(WRITE "${_ge_vcpkg_stage_script}" "\
if(IS_DIRECTORY \"\${SRC}\")
    if(NOT IS_DIRECTORY \"\${DST}\")
        execute_process(COMMAND \"\${CMAKE_COMMAND}\" -E copy_directory \"\${SRC}\" \"\${DST}\")
    endif()
    # Drop GL/SPIR-V header dirs whose version-like subdirs (e.g. GLES/1.0) make a
    # `codesign --deep` of the enclosing .app fail (\"bundle format unrecognized\").
    # They are not part of the engine's public C++ API.
    foreach(_trap GLES GLSC spirv)
        file(REMOVE_RECURSE \"\${DST}/\${_trap}\")
    endforeach()
endif()
")
        add_custom_command(TARGET ${host_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                -DSRC=${_ge_vcpkg_include}
                -DDST=${sdk_output_dir}/vcpkg-include
                -P ${_ge_vcpkg_stage_script}
            COMMENT "Staging vcpkg headers to SDK (one-time)"
            VERBATIM
        )
    endif()

    # Generated headers (e.g. Jobs/CompileServerVersion.h) referenced by public headers.
    if(IS_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
        add_custom_command(TARGET ${host_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_BINARY_DIR}/generated"
                "${sdk_output_dir}/generated-include"
            COMMENT "Staging generated headers to SDK"
            VERBATIM
        )
    endif()
endfunction()
