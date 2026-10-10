# OutputLayout.cmake
# Centralized, consistent output directory layout for runnable targets.
#
# Canonical layout (relative to ${CMAKE_BINARY_DIR}):
#   bin/<Config>/Apps/<AppName>/...
#   bin/<Config>/Tests/...
#   bin/<Config>/Demos/...
#   bin/<Config>/Tools/...
#   bin/<Config>/Benchmarks/...

function(ge_set_target_runtime_output_dir target_name relative_dir)
  if(NOT TARGET ${target_name})
    message(FATAL_ERROR "ge_set_target_runtime_output_dir: target not found: ${target_name}")
  endif()

  # VS/MSBuild multi-config generators don't reliably honor generator expressions
  # in output directory properties, so we set per-config output directories when
  # CMAKE_CONFIGURATION_TYPES is present.
  set(_rel "${relative_dir}")
  string(REGEX REPLACE "^[/\\\\]+" "" _rel "${_rel}")
  string(REGEX REPLACE "[/\\\\]+$" "" _rel "${_rel}")

  # Check if this is a macOS bundle target. get_target_property returns
  # "<target>-NOTFOUND" if the property is not set, so we need to check for TRUE.
  get_target_property(_is_bundle ${target_name} MACOSX_BUNDLE)
  if(NOT _is_bundle)
    set(_is_bundle FALSE)
  endif()
  # A MODULE library's loadable file follows LIBRARY_OUTPUT_DIRECTORY, not the
  # runtime directory, so a module staged for a test must set both to land
  # beside the executable that loads it.
  get_target_property(_target_type ${target_name} TYPE)
  set(_is_module FALSE)
  if(_target_type STREQUAL "MODULE_LIBRARY")
    set(_is_module TRUE)
  endif()

  # The layout-relative directory and the output directory it resolves to (as
  # a generator expression), for staging helpers that share one recipe between
  # every executable landing in the same directory.
  if(CMAKE_CONFIGURATION_TYPES OR CMAKE_BUILD_TYPE)
    set(_layout_out_dir "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/${_rel}")
  else()
    set(_layout_out_dir "${CMAKE_BINARY_DIR}/bin/${_rel}")
  endif()
  set_target_properties(${target_name} PROPERTIES
    GE_RUNTIME_LAYOUT_DIR "${_rel}"
    GE_RUNTIME_LAYOUT_OUTPUT_DIR "${_layout_out_dir}")

  if(CMAKE_CONFIGURATION_TYPES)
    foreach(_cfg IN LISTS CMAKE_CONFIGURATION_TYPES)
      string(TOUPPER "${_cfg}" _cfg_upper)
      set(_out_dir "${CMAKE_BINARY_DIR}/bin/${_cfg}/${_rel}")

      # Always set RUNTIME_OUTPUT_DIRECTORY for consistency.
      set_target_properties(${target_name} PROPERTIES
        "RUNTIME_OUTPUT_DIRECTORY_${_cfg_upper}" "${_out_dir}"
      )
      if(_is_module)
        set_target_properties(${target_name} PROPERTIES
          "LIBRARY_OUTPUT_DIRECTORY_${_cfg_upper}" "${_out_dir}"
        )
      endif()
      # For macOS bundles, ALSO set BUNDLE_OUTPUT_DIRECTORY so the .app lands
      # in the expected location. CMake uses this to place the bundle and
      # generates Info.plist correctly when both are set to the same path.
      if(_is_bundle)
        set_target_properties(${target_name} PROPERTIES
          "BUNDLE_OUTPUT_DIRECTORY_${_cfg_upper}" "${_out_dir}"
        )
      endif()
    endforeach()
  else()
    if(CMAKE_BUILD_TYPE)
      set(_cfg "${CMAKE_BUILD_TYPE}")
    else()
      set(_cfg "")
    endif()

    if(_cfg STREQUAL "")
      set(_out_dir "${CMAKE_BINARY_DIR}/bin/${_rel}")
    else()
      set(_out_dir "${CMAKE_BINARY_DIR}/bin/${_cfg}/${_rel}")
    endif()

    # Always set RUNTIME_OUTPUT_DIRECTORY for consistency.
    set_target_properties(${target_name} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${_out_dir}"
    )
    if(_is_module)
      set_target_properties(${target_name} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${_out_dir}"
      )
    endif()
    # For macOS bundles, ALSO set BUNDLE_OUTPUT_DIRECTORY so the .app lands
    # in the expected location. CMake uses this to place the bundle and
    # generates Info.plist correctly when both are set to the same path.
    if(_is_bundle)
      set_target_properties(${target_name} PROPERTIES
        BUNDLE_OUTPUT_DIRECTORY "${_out_dir}"
      )
    endif()
  endif()
endfunction()

function(ge_set_output_app target_name app_name)
  ge_set_target_runtime_output_dir(${target_name} "Apps/${app_name}")
endfunction()

# The applications stage first-party libraries (Engine.dll, GameEngine.Native.dll)
# beside themselves, and each application's own POST_BUILD staging runs only when
# that application builds. A build of one application after a library change would
# leave the other's copy from an older link; the two then report different engine
# build identities and the Player refuses game modules the Editor built. So an
# application registers its directory here, and every link of a library listed in
# Engine/CMakeLists.txt refreshes the copies in the registered directories.
# Windows only: there the staged copy is the image the application loads.
#
# The directories are kept on the Engine target, which every application links.
function(ge_register_application_directory target_name)
  if(NOT WIN32 OR NOT TARGET Engine)
    return()
  endif()
  set_property(TARGET Engine APPEND PROPERTY
      GE_APPLICATION_DIRECTORIES "$<TARGET_FILE_DIR:${target_name}>")
endfunction()

# Adds the POST_BUILD step that copies <library_target>'s fresh link over its
# staged copy in every registered application directory
# (cmake/RefreshApplicationCopies.cmake). Call from the directory that creates
# <library_target>.
function(ge_refresh_application_copies library_target)
  if(NOT WIN32 OR NOT TARGET Engine)
    return()
  endif()
  set(_ge_script
      "${CMAKE_BINARY_DIR}/cmake/ge_refresh_application_copies_${library_target}_$<CONFIG>.cmake")
  string(CONCAT _ge_script_content
      "set(GE_LIBRARY_FILE \"$<TARGET_FILE:${library_target}>\")\n"
      "set(GE_APPLICATION_DIRECTORIES \"$<TARGET_GENEX_EVAL:Engine,$<TARGET_PROPERTY:Engine,GE_APPLICATION_DIRECTORIES>>\")\n"
      "include(\"${CMAKE_SOURCE_DIR}/cmake/RefreshApplicationCopies.cmake\")\n"
  )
  file(GENERATE OUTPUT "${_ge_script}" CONTENT "${_ge_script_content}")
  add_custom_command(TARGET ${library_target} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -P "${_ge_script}"
      COMMENT "Refreshing the applications' staged copies of ${library_target}"
      VERBATIM
  )
endfunction()

# Also marks the target as a test executable, which receives the allocation counter's
# hook in every configuration (cmake/AllocationHook.cmake).
function(ge_set_output_tests target_name)
  ge_set_target_runtime_output_dir(${target_name} "Tests")
  set_property(TARGET ${target_name} PROPERTY GE_TEST_EXECUTABLE TRUE)
endfunction()

function(ge_set_output_demos target_name)
  ge_set_target_runtime_output_dir(${target_name} "Demos")
endfunction()

#
# ge_get_staged_content_dir(<target> <out_var>)
#
# The directory a target's staged data (Assets/, ThirdPartyNotices/) goes in:
# the executable's directory, except in a macOS app bundle, whose data is
# staged as resources in Contents/Resources. Contents/MacOS holds code only:
# codesign seals every file there as code, and refuses a directory whose name
# looks like a nested bundle. The runtime half of this rule is
# PathUtils::InstallContentRootFor.
#
function(ge_get_staged_content_dir target_name out_var)
  get_target_property(_ge_is_bundle ${target_name} MACOSX_BUNDLE)
  if(APPLE AND _ge_is_bundle)
    set(${out_var} "$<TARGET_FILE_DIR:${target_name}>/../Resources" PARENT_SCOPE)
  else()
    set(${out_var} "$<TARGET_FILE_DIR:${target_name}>" PARENT_SCOPE)
  endif()
endfunction()

# Stage the engine shader-graph node helper tree into a target's staged content
# directory (ge_get_staged_content_dir), at the layout
# ShaderGraph::GetEngineGraphNodesRoot() resolves from the executable
# directory. The resolver has no source-tree fallback — a repo path
# is absent once the build output moves — so a binary that EXERCISES it must
# carry the tree.
#
# "Exercises", not "links": most test executables link the resolver through the
# Engine or Graph libraries without ever reaching it, and staging is applied to
# the ones whose behaviour is measurably sensitive to the tree's absence (armed
# by moving it aside and re-running). Do not re-derive the call list from
# linkage; it would sweep in most of the test tree for nothing.
#
# The staging is a stamped custom target, not a POST_BUILD, because a POST_BUILD
# only runs when the target relinks — that leaves a node-only edit staged stale
# with the build reporting "no work to do".
#
# Deletes are handled by two things together. The glob is written to a list
# file, so adding OR removing a node file rewrites it and re-fires the recipe:
# depending on the globbed inputs alone cannot detect a delete, because the
# stamp is then newer than every input that remains. And the copy is followed
# by the mirror prune (cmake/PruneStagedMirrorOrphans.cmake), because
# copy_directory never deletes: a deleted node file would otherwise survive in
# the staged tree and keep compiling. The prune removes only what this mirror
# staged before, so the shared Assets/ tree next to a test executable — which
# other targets stage their own files into — is never at risk.
#
# One recipe per destination. Every executable in bin/<Config>/Tests resolves
# the same tree, and independent recipes for one directory run concurrently
# under a parallel build: on a cold tree one of them fails mid-copy while
# another is still creating the directory. The first caller for a destination
# defines the staging target; every caller depends on it.
function(ge_stage_shader_graph_nodes target_name)
  if(NOT TARGET ${target_name})
    message(FATAL_ERROR "ge_stage_shader_graph_nodes: target not found: ${target_name}")
  endif()

  # A bundle's executable directory is inside its .app, so a bundle never
  # shares a destination with the other executables of its layout directory.
  get_target_property(_ge_layout_dir ${target_name} GE_RUNTIME_LAYOUT_DIR)
  if(NOT _ge_layout_dir)
    message(FATAL_ERROR
      "ge_stage_shader_graph_nodes: ${target_name} has no runtime layout directory; call ge_set_output_* first")
  endif()
  get_target_property(_ge_is_bundle ${target_name} MACOSX_BUNDLE)
  if(_ge_is_bundle)
    set(_ge_layout_dir "${_ge_layout_dir}/${target_name}")
    ge_get_staged_content_dir(${target_name} _ge_content_dir)
  else()
    get_target_property(_ge_content_dir ${target_name} GE_RUNTIME_LAYOUT_OUTPUT_DIR)
  endif()
  string(MAKE_C_IDENTIFIER "${_ge_layout_dir}" _ge_dest_key)
  set(_ge_stage_target "StageShaderGraphNodes_${_ge_dest_key}")

  if(NOT TARGET ${_ge_stage_target})
    set(_ge_nodes_src "${CMAKE_SOURCE_DIR}/Engine/Modules/Rendering/Shaders/Graph/Nodes")
    file(GLOB_RECURSE _ge_nodes_inputs CONFIGURE_DEPENDS "${_ge_nodes_src}/*")

    set(_ge_nodes_list "${CMAKE_CURRENT_BINARY_DIR}/stage-stamps/graph-nodes-${_ge_dest_key}.list")
    file(CONFIGURE OUTPUT "${_ge_nodes_list}" CONTENT "${_ge_nodes_inputs}" @ONLY)

    set(_ge_nodes_dest "${_ge_content_dir}/Assets/Shaders/Graph/Nodes")
    set(_ge_nodes_stamp
      "${CMAKE_CURRENT_BINARY_DIR}/stage-stamps/graph-nodes-${_ge_dest_key}-$<CONFIG>.stamp")
    set(_ge_prune_script "${CMAKE_SOURCE_DIR}/cmake/PruneStagedMirrorOrphans.cmake")

    add_custom_command(OUTPUT "${_ge_nodes_stamp}"
      COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${_ge_nodes_src}" "${_ge_nodes_dest}"
      COMMAND ${CMAKE_COMMAND}
        "-DSRC=${_ge_nodes_src}" "-DDST=${_ge_nodes_dest}"
        "-DMANIFEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/stage-stamps/$<CONFIG>/mirror-manifests"
        -P "${_ge_prune_script}"
      COMMAND ${CMAKE_COMMAND} -E touch "${_ge_nodes_stamp}"
      DEPENDS ${_ge_nodes_inputs} "${_ge_nodes_list}" "${_ge_prune_script}"
      COMMENT "Staging shader-graph node helpers beside ${_ge_layout_dir}"
      VERBATIM
    )
    add_custom_target(${_ge_stage_target} DEPENDS "${_ge_nodes_stamp}")
    set_target_properties(${_ge_stage_target} PROPERTIES FOLDER "Staging")
  endif()
  add_dependencies(${target_name} ${_ge_stage_target})
endfunction()

function(ge_set_output_tools target_name)
  ge_set_target_runtime_output_dir(${target_name} "Tools")
endfunction()

function(ge_set_output_benchmarks target_name)
  ge_set_target_runtime_output_dir(${target_name} "Benchmarks")
endfunction()

#
# Runtime dependency staging
#
# Copies only the runtime DLL dependencies of a built executable into its output
# folder, so targets can run from their isolated output directories.
#
function(ge_stage_runtime_dependencies target_name)
  if(NOT TARGET ${target_name})
    message(FATAL_ERROR "ge_stage_runtime_dependencies: target not found: ${target_name}")
  endif()

  # Only stage runtime dependencies for runnable targets.
  get_target_property(_ge_target_type ${target_name} TYPE)
  if(NOT (_ge_target_type STREQUAL "EXECUTABLE" OR _ge_target_type STREQUAL "MODULE_LIBRARY"))
    return()
  endif()

  # All platforms use the same scan-based staging: `file(GET_RUNTIME_DEPENDENCIES)`
  # reads the binary's import table and locates each needed library in the search
  # dirs. This catches every transitive vcpkg DLL — including those exposed via
  # find_package modules that don't create imported SHARED targets (e.g. FindFFMPEG,
  # parts of FindVulkan in older SDKs) which CMake's `TARGET_RUNTIME_DLLS` would
  # otherwise miss.
  #
  # vcpkg's own applocal.ps1 post-build step is gated off project-wide via
  # `VCPKG_APPLOCAL_DEPS=OFF` in the top-level CMakeLists.txt, so this is the
  # single source of truth for runtime DLL staging.

  # Prepare search directories for dependency resolution.
  #
  # Single-config generators (Ninja/Make) on macOS/Linux place first-party
  # SHARED libraries in the non-per-config CMAKE_LIBRARY_OUTPUT_DIRECTORY
  # (${CMAKE_BINARY_DIR}/lib) even under a Debug build, while executables still
  # land in bin/$<CONFIG>. Without the non-config bin/lib entries below,
  # file(GET_RUNTIME_DEPENDENCIES) can't locate libEngine.dylib /
  # libGameEngine.Native.dylib (scripting builds) — so they, and their
  # transitive Vulkan loader, never get staged into the .app bundle and the
  # app aborts at launch with "Library not loaded: @rpath/libEngine.dylib".
  # First-party dylibs have unique names, so these dirs can't reintroduce the
  # debug/release variant ambiguity the vcpkg search order below guards against.
  # Fresh build-output dirs come before the staged app dir so NEW dependencies
  # resolve to fresh binaries. Note this ordering alone does NOT protect the
  # scan from stale ALREADY-STAGED copies: on Windows the PE scanner probes a
  # scanned module's own directory before this list, so StageRuntimeDependencies
  # refreshes staged libraries from these dirs before scanning (see the
  # pre-refresh pass there for the full failure mode).
  set(_ge_search_dirs
      "${CMAKE_BINARY_DIR}/bin/$<CONFIG>"
      "${CMAKE_BINARY_DIR}/lib/$<CONFIG>"
      "${CMAKE_BINARY_DIR}/bin"
      "${CMAKE_BINARY_DIR}/lib"
      "$<TARGET_FILE_DIR:${target_name}>"
  )

  if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    # vcpkg ships per-config variants for most ports: debug-built libs under
    # <triplet>/debug/{bin,lib} (linked against /MDd on Windows, with debug
    # symbols on Linux/macOS) and release-built ones under <triplet>/{bin,lib}.
    # Ports the overlay triplets build release-only (cmake/triplets) install no
    # debug/ files at all; in Debug their DLLs intentionally resolve through
    # the release dirs listed after the debug ones.
    # `file(GET_RUNTIME_DEPENDENCIES)` resolves each dependency by NAME against
    # the DIRECTORIES list in order — for ports where Debug and Release variants
    # share a filename (e.g. utf8proc.dll, lexbor.dll), the search order is the
    # only signal that picks the right variant.
    #
    # Putting debug/{bin,lib} first under Debug guarantees the staged DLL set
    # matches Engine.dll's CRT linkage. The classic failure when this is wrong is
    # `RtlValidateHeap` aborting on free: a release-CRT dep returns a buffer
    # allocated from ucrtbase.dll's heap and the engine frees it via ucrtbased's
    # heap — the trap class the keep rules in cmake/triplets/x64-windows.cmake
    # exist for (ktx's WriteToMemory buffer is the live example).
    #
    # The $<$<CONFIG:Debug>:...> guards collapse to empty in any non-Debug
    # config — DebugFast, Release, RelWithDebInfo, MinSizeRel, Distribution all
    # use /MD via CMAKE_MSVC_RUNTIME_LIBRARY — so those builds resolve every
    # dep through the release bin/lib dirs. StageRuntimeDependencies.cmake
    # filters the resulting empty entries from the search list.
    list(APPEND _ge_search_dirs
        "$<$<CONFIG:Debug>:${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin>"
        "$<$<CONFIG:Debug>:${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib>"
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin"
        # On macOS/Linux, vcpkg dylibs often live under lib/ (not bin/).
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib"
    )
  endif()

  # If CoreCLR hosting is enabled, include the directory containing the detected nethost library.
  # This helps resolve @rpath/libnethost.* when staging bundles.
  if(DEFINED NETHOST_LIBRARY AND EXISTS "${NETHOST_LIBRARY}")
    get_filename_component(_ge_nethost_dir "${NETHOST_LIBRARY}" DIRECTORY)
    if(_ge_nethost_dir)
      list(APPEND _ge_search_dirs "${_ge_nethost_dir}")
    endif()
  endif()

  # Generate a per-config script so generator expressions are resolved at build time.
  set(_ge_stage_dir "${CMAKE_BINARY_DIR}/cmake")
  file(MAKE_DIRECTORY "${_ge_stage_dir}")

  # Default destination is next to the executable/module.
  set(_ge_dest_dir "$<TARGET_FILE_DIR:${target_name}>")
  # For macOS bundles, stage dylibs into Contents/Frameworks (standard app bundle layout).
  if(APPLE)
    get_target_property(_ge_is_bundle ${target_name} MACOSX_BUNDLE)
    if(_ge_is_bundle)
      set(_ge_dest_dir "$<TARGET_FILE_DIR:${target_name}>/../Frameworks")
    endif()
  endif()

  set(_ge_script "${_ge_stage_dir}/ge_stage_runtime_deps_${target_name}_$<CONFIG>.cmake")
  string(CONCAT _ge_script_content
        "set(GE_RUNTIME_DEPS_EXECUTABLE \"$<TARGET_FILE:${target_name}>\")\n"
        "set(GE_RUNTIME_DEPS_DEST_DIR \"${_ge_dest_dir}\")\n"
        "set(GE_RUNTIME_DEPS_SEARCH_DIRS \"${_ge_search_dirs}\")\n"
        "set(GE_RUNTIME_DEPS_VERBOSE OFF)\n"
        "include(\"${CMAKE_SOURCE_DIR}/cmake/StageRuntimeDependencies.cmake\")\n"
  )
  file(GENERATE
      OUTPUT "${_ge_script}"
      CONTENT "${_ge_script_content}"
  )

  add_custom_command(TARGET ${target_name} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -P "${_ge_script}"
      COMMENT "Staging runtime dependencies for ${target_name}"
      VERBATIM
  )

  # Centralized extras (cross-platform safe; helpers no-op when not applicable).
  if(COMMAND ge_add_dotnet_hosting_runtime)
    ge_add_dotnet_hosting_runtime(${target_name})
  endif()
  if(COMMAND ge_add_asan_runtime)
    ge_add_asan_runtime(${target_name})
  endif()
endfunction()


#
# Single-file test asset staging
#
# Stage one source file at relative_dest under a target's runtime layout
# directory (bin/<Config>/Tests for ge_set_output_tests targets) — the
# exe-anchored root a test reads staged content from
# (PathUtils::GetInstallAssetsRoot() is <exe>/Assets for a test executable).
# Every executable in one layout directory shares that tree, so several targets
# may name the same destination: keyed by layout directory + relative_dest, the
# copy is made once and each declaring target depends on it — building any one
# of them stages the file, and two targets disagreeing about a destination's
# source is a configure error rather than last-writer-wins. A target in another
# layout directory gets its own copy beside its own executable.
#
# The destination file itself is the command's OUTPUT (legal because the layout
# output directory carries only $<CONFIG>), so a deleted or stale copy is
# restored by the next build and an edit to the asset alone re-stages it; a
# POST_BUILD only runs when its target relinks, and a stamp file outlives a
# deleted copy.
#
function(ge_stage_test_asset target_name source relative_dest)
  if(NOT TARGET ${target_name})
    message(FATAL_ERROR "ge_stage_test_asset: target not found: ${target_name}")
  endif()
  if(NOT EXISTS "${source}")
    message(FATAL_ERROR "ge_stage_test_asset: source not found: ${source}")
  endif()
  get_target_property(_ge_layout_dir ${target_name} GE_RUNTIME_LAYOUT_DIR)
  if(NOT _ge_layout_dir)
    message(FATAL_ERROR
      "ge_stage_test_asset: ${target_name} has no runtime layout directory; call ge_set_output_* first")
  endif()
  get_target_property(_ge_is_bundle ${target_name} MACOSX_BUNDLE)
  if(_ge_is_bundle)
    message(FATAL_ERROR
      "ge_stage_test_asset: ${target_name} is an app bundle; stage its resources with the bundle's own rules")
  endif()
  get_target_property(_ge_layout_out_dir ${target_name} GE_RUNTIME_LAYOUT_OUTPUT_DIR)

  string(MAKE_C_IDENTIFIER "${_ge_layout_dir}/${relative_dest}" _ge_dest_key)
  set(_ge_stage_target "StageTestAsset_${_ge_dest_key}")

  if(TARGET ${_ge_stage_target})
    get_target_property(_ge_recorded_source ${_ge_stage_target} GE_STAGE_SOURCE)
    if(NOT _ge_recorded_source STREQUAL source)
      message(FATAL_ERROR
        "ge_stage_test_asset: ${_ge_layout_dir}/${relative_dest} is already staged from ${_ge_recorded_source}; "
        "${target_name} asked for ${source}")
    endif()
    add_dependencies(${target_name} ${_ge_stage_target})
    return()
  endif()

  set(_ge_dest "${_ge_layout_out_dir}/${relative_dest}")
  get_filename_component(_ge_rel_dir "${relative_dest}" DIRECTORY)
  add_custom_command(OUTPUT "${_ge_dest}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_ge_layout_out_dir}/${_ge_rel_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy "${source}" "${_ge_dest}"
    DEPENDS "${source}"
    COMMENT "Staging test asset ${_ge_layout_dir}/${relative_dest}"
    VERBATIM
  )
  add_custom_target(${_ge_stage_target} DEPENDS "${_ge_dest}")
  set_target_properties(${_ge_stage_target} PROPERTIES
    FOLDER "Staging"
    GE_STAGE_SOURCE "${source}")
  add_dependencies(${target_name} ${_ge_stage_target})
endfunction()
