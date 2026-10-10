# Modern GPU-Driven Rendering Library
cmake_minimum_required(VERSION 3.20)
project(GameEngineRendering LANGUAGES CXX)

# C++20 requirement for modern features
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build options
option(RENDERING_BUILD_VULKAN "Build Vulkan backend" ON)
# Temporarily disable DirectX 12 backend by default until it is fixed
option(RENDERING_BUILD_DIRECTX12 "Build DirectX 12 backend" OFF)
# Enforce DX12 disabled in this subproject to avoid accidental enablement from parent cache
set(RENDERING_BUILD_DIRECTX12 OFF CACHE BOOL "Build DirectX 12 backend" FORCE)
# Native Metal backend (macOS only). GraphicsAPI::Auto resolves to Metal on
# macOS (DeviceFactory); GE_GFX_API=vulkan is the explicit MoltenVK pick.
option(RENDERING_BUILD_METAL "Build Metal backend (experimental)" ON)
# WebGPU backend: wgpu-native on desktop (development/debugging of the web
# renderer), emdawnwebgpu in the browser. Opt in at runtime via
# GE_GFX_API=webgpu; Vulkan stays the default everywhere but wasm.
option(RENDERING_BUILD_WEBGPU "Build WebGPU backend (experimental)" ON)
if(EMSCRIPTEN)
    set(RENDERING_BUILD_WEBGPU ON CACHE BOOL "Build WebGPU backend (experimental)" FORCE)
    set(RENDERING_BUILD_VULKAN OFF CACHE BOOL "Build Vulkan backend" FORCE)
endif()
option(RENDERING_BUILD_TESTS "Build unit tests" ON)

# Optional GPU culling examples (visual/sample apps). The automated GPU culling test
# suite is controlled by standard BUILD_TESTING / RENDERING_BUILD_TESTS options.
option(RENDERING_ENABLE_GPU_CULLING "Build GPU culling examples" OFF)

# Platform detection. Emscripten first: it reports UNIX and would otherwise
# classify as Linux.
if(EMSCRIPTEN)
    set(RENDERING_PLATFORM_WEB TRUE)
elseif(WIN32)
    set(RENDERING_PLATFORM_WINDOWS TRUE)
elseif(UNIX AND NOT APPLE)
    set(RENDERING_PLATFORM_LINUX TRUE)
elseif(APPLE)
    set(RENDERING_PLATFORM_MACOS TRUE)
endif()

# ----------------------------------------------------------------------------
# macOS Vulkan SDK auto-detection (MoltenVK)
# ----------------------------------------------------------------------------
#
# On macOS, vcpkg can provide Vulkan headers + the cross-platform loader, but it
# does not provide MoltenVK. Many developers install the official Vulkan SDK,
# which typically lives under:
#   ~/VulkanSDK/<version>/macOS
# but may not have VULKAN_SDK exported in the shell that runs CMake.
#
# To reduce setup friction, if VULKAN_SDK is unset we try to auto-detect the
# latest installed SDK and set ENV{VULKAN_SDK} for this configure run.
if(RENDERING_PLATFORM_MACOS AND NOT DEFINED ENV{VULKAN_SDK})
    set(_GE_VULKAN_SDK_CANDIDATE "")
    set(_GE_VULKAN_SDK_BEST_VERSION "")

    # Common install roots for the LunarG Vulkan SDK on macOS.
    set(_GE_VULKAN_SDK_ROOTS
        "$ENV{HOME}/VulkanSDK"
        "/Users/Shared/VulkanSDK"
        "/opt/VulkanSDK"
        "/Applications/VulkanSDK")

    foreach(_root IN LISTS _GE_VULKAN_SDK_ROOTS)
        if(NOT _root OR _root STREQUAL "")
            continue()
        endif()
        if(NOT EXISTS "${_root}")
            continue()
        endif()

        # Look for versioned directories (e.g. 1.4.304.1) and prefer the newest.
        file(GLOB _sdk_entries LIST_DIRECTORIES true "${_root}/*")
        foreach(_entry IN LISTS _sdk_entries)
            if(NOT IS_DIRECTORY "${_entry}")
                continue()
            endif()

            get_filename_component(_ver "${_entry}" NAME)
            if(NOT _ver MATCHES "^[0-9]+(\\.[0-9]+)*$")
                continue()
            endif()

            set(_candidate "${_entry}/macOS")
            if(NOT IS_DIRECTORY "${_candidate}")
                continue()
            endif()

            # Basic sanity checks: headers + loader library exist under the candidate.
            if(NOT (EXISTS "${_candidate}/include/vulkan/vulkan.h" OR EXISTS "${_candidate}/Include/vulkan/vulkan.h"))
                continue()
            endif()
            if(NOT (EXISTS "${_candidate}/lib/libvulkan.dylib" OR EXISTS "${_candidate}/Lib/libvulkan.dylib"))
                continue()
            endif()

            if(_GE_VULKAN_SDK_BEST_VERSION STREQUAL "" OR _ver VERSION_GREATER _GE_VULKAN_SDK_BEST_VERSION)
                set(_GE_VULKAN_SDK_BEST_VERSION "${_ver}")
                set(_GE_VULKAN_SDK_CANDIDATE "${_candidate}")
            endif()
        endforeach()
    endforeach()

    if(_GE_VULKAN_SDK_CANDIDATE AND EXISTS "${_GE_VULKAN_SDK_CANDIDATE}")
        set(ENV{VULKAN_SDK} "${_GE_VULKAN_SDK_CANDIDATE}")
        message(STATUS "VULKAN_SDK was not set; auto-detected Vulkan SDK at: $ENV{VULKAN_SDK}")
    endif()
endif()

# Find required packages
# Try to find Vulkan via vcpkg (CONFIG) or system SDK. The vcpkg manifest
# declares the `vulkan` port so in the default flow this resolves to the
# vcpkg-provided headers/loader rather than a hard-coded local SDK path.
# Validation layers come from the system Vulkan SDK install (registered with
# the Vulkan loader via OS config), not from vcpkg.
find_package(Vulkan QUIET)

# On macOS, if CMake did not find Vulkan but the official Vulkan SDK
# (MoltenVK) is installed and VULKAN_SDK is set, fall back to using that
# SDK's headers and loader. This helps developers who are not using the
# vcpkg toolchain and rely solely on the standalone SDK.
if(RENDERING_PLATFORM_MACOS AND NOT Vulkan_FOUND AND DEFINED ENV{VULKAN_SDK})
    set(Vulkan_FOUND TRUE)
    if(EXISTS "$ENV{VULKAN_SDK}/include")
        set(Vulkan_INCLUDE_DIRS "$ENV{VULKAN_SDK}/include")
    else()
        set(Vulkan_INCLUDE_DIRS "$ENV{VULKAN_SDK}/Include")
    endif()
    if(EXISTS "$ENV{VULKAN_SDK}/lib/libvulkan.dylib")
        set(Vulkan_LIBRARIES "$ENV{VULKAN_SDK}/lib/libvulkan.dylib")
    else()
        set(Vulkan_LIBRARIES "$ENV{VULKAN_SDK}/Lib/libvulkan.dylib")
    endif()
    message(STATUS "Vulkan (macOS): using SDK at $ENV{VULKAN_SDK}")
endif()
# Option to enable SPIR-V reflection; default ON for dev/editor builds
option(RENDERING_ENABLE_SPIRV_REFLECTION "Enable SPIR-V reflection to generate ShaderMeta at build or runtime" ON)

# Force-enable reflection in this subproject for dev unless overridden by parent.
# wasm: spirv-reflect is not in the wasm triplet (shader authoring is a
# desktop/editor concern; the browser runtime consumes prebuilt WGSL/SPIR-V).
if(EMSCRIPTEN)
    set(RENDERING_ENABLE_SPIRV_REFLECTION OFF CACHE BOOL "Enable SPIR-V reflection" FORCE)
else()
    set(RENDERING_ENABLE_SPIRV_REFLECTION ON CACHE BOOL "Enable SPIR-V reflection" FORCE)
endif()

if(Vulkan_FOUND AND RENDERING_BUILD_VULKAN)
    set(RENDERING_HAS_VULKAN TRUE)
    message(STATUS "Vulkan found: ${Vulkan_INCLUDE_DIRS}")
else()
    set(RENDERING_HAS_VULKAN FALSE)
    message(WARNING "Vulkan not found - Vulkan backend disabled")
endif()

# DirectX 12 (Windows only)
if(RENDERING_PLATFORM_WINDOWS AND RENDERING_BUILD_DIRECTX12)
    set(RENDERING_HAS_DIRECTX12 TRUE)
    message(STATUS "DirectX 12 support enabled (Windows)")
else()
    set(RENDERING_HAS_DIRECTX12 FALSE)
endif()

# Metal (macOS only)
if(RENDERING_PLATFORM_MACOS AND RENDERING_BUILD_METAL)
    set(RENDERING_HAS_METAL TRUE)
    message(STATUS "Metal backend enabled (macOS)")
else()
    set(RENDERING_HAS_METAL FALSE)
endif()

# WebGPU. Desktop builds bind the wgpu-native vcpkg port; the browser build gets
# its implementation from Emscripten's emdawnwebgpu port instead, so no library
# lookup happens there.
if(RENDERING_BUILD_WEBGPU AND EMSCRIPTEN)
    # webgpu.h and the JS implementation come from emscripten's emdawnwebgpu
    # port (--use-port on the target below); nothing to find here.
    set(RENDERING_HAS_WEBGPU TRUE CACHE INTERNAL "WebGPU backend compiled in")
    message(STATUS "WebGPU backend enabled (emdawnwebgpu port)")
elseif(RENDERING_BUILD_WEBGPU)
    find_path(WGPU_NATIVE_INCLUDE_DIR NAMES webgpu/webgpu.h)
    find_library(WGPU_NATIVE_LIBRARY NAMES wgpu_native)
    if(WGPU_NATIVE_INCLUDE_DIR AND WGPU_NATIVE_LIBRARY)
        set(RENDERING_HAS_WEBGPU TRUE CACHE INTERNAL "WebGPU backend compiled in")
        message(STATUS "WebGPU backend enabled (wgpu-native: ${WGPU_NATIVE_LIBRARY})")
    else()
        set(RENDERING_HAS_WEBGPU FALSE CACHE INTERNAL "WebGPU backend compiled in")
        message(WARNING "RENDERING_BUILD_WEBGPU is ON but wgpu-native was not found; WebGPU backend disabled")
    endif()
else()
    set(RENDERING_HAS_WEBGPU FALSE CACHE INTERNAL "WebGPU backend compiled in")
endif()

# External dependencies - automatically fetch if not found
include(FetchContent)

# GenerationalVector is already included by the root CMakeLists.txt
# We just need to link to the existing target

# GLFW via vcpkg (required on desktop). On wasm the GLFW API is emscripten's
# JS implementation: headers ship in the sysroot, -s USE_GLFW=3 links it.
if(NOT EMSCRIPTEN)
    find_package(glfw3 CONFIG REQUIRED)
    message(STATUS "GLFW found via find_package (glfw3 CONFIG)")

    find_path(RENDERDOC_APP_API_INCLUDE_DIR NAMES renderdoc_app.h REQUIRED)
endif()

# VMA (Vulkan Memory Allocator) via vcpkg
if(RENDERING_HAS_VULKAN)
    find_package(VulkanMemoryAllocator CONFIG REQUIRED)
    set(VMA_FOUND TRUE)
    # Propagate include dirs for any direct includes in examples/tests
    get_target_property(VMA_INCLUDE_DIRS GPUOpen::VulkanMemoryAllocator INTERFACE_INCLUDE_DIRECTORIES)
    message(STATUS "VMA found via find_package (VulkanMemoryAllocator CONFIG)")
endif()

# ============================================================================
# SHADER COMPILATION AND MANAGEMENT
# ============================================================================

# Find glslc (Vulkan shader compiler)
# Prefer vcpkg-provided shaderc tools when available, but still work with a
# system Vulkan SDK install.
set(_GLSLC_HINT_DIRS ${Vulkan_INCLUDE_DIRS}/../bin)
if(VCPKG_DIR AND VCPKG_ARCH)
    list(APPEND _GLSLC_HINT_DIRS
        "${VCPKG_DIR}/installed/${VCPKG_ARCH}/tools"
        "${VCPKG_DIR}/installed/${VCPKG_ARCH}/tools/shaderc")
endif()
if(EMSCRIPTEN)
    # SPIR-V authoring (glslc + ShaderReflect packaging) is a desktop/editor
    # concern; the wasm runtime consumes prebuilt shader packages, and the
    # WebGPU WGSL pipeline arrives with the browser-renderer slice.
    set(GLSLC_EXECUTABLE "")
else()
    find_program(GLSLC_EXECUTABLE glslc HINTS ${_GLSLC_HINT_DIRS})
endif()

if(GLSLC_EXECUTABLE)
    message(STATUS "✅ Found glslc: ${GLSLC_EXECUTABLE}")
elseif(NOT EMSCRIPTEN)
    message(WARNING "⚠️ glslc not found - shader compilation will be skipped; install Vulkan SDK or ensure vcpkg shaderc is available")
endif()

# Function to copy shaders to build directory
function(copy_shaders_to_build)
    # Create shaders directory in build
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/Shaders)

    # Copy all shader files to build directory for runtime access
    file(GLOB_RECURSE SHADER_FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.spv"
        "${CMAKE_CURRENT_SOURCE_DIR}/Shaders/*.cso"
    )

    foreach(SHADER_FILE ${SHADER_FILES})
        get_filename_component(SHADER_NAME ${SHADER_FILE} NAME)
        configure_file(${SHADER_FILE} ${CMAKE_BINARY_DIR}/Shaders/${SHADER_NAME} COPYONLY)
    endforeach()

    list(LENGTH SHADER_FILES SHADER_COUNT)
    message(STATUS "✅ Copied ${SHADER_COUNT} shader files to build directory")
endfunction()

# Shader compilation: we accumulate every .spv output into a single list and
# attach them to ONE custom target ("CompileShaders") at the end. Previously
# each shader had its own add_custom_target(${SHADER}_target ...), which
# spawned 100+ MSBuild "projects" — each costing ~150-300 ms of scheduler
# overhead on every build, regardless of whether anything changed.
set(GE_ALL_SHADER_SPV_OUTPUTS "")

# Compile shaders if glslc is available
if(GLSLC_EXECUTABLE)
    # Define shader source and output directories
    set(SHADER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Shaders)
    set(GE_SHADER_SOURCE_DIRS ${SHADER_SOURCE_DIR})
    # First-party post-effect packages own their shader sources. They compile
    # into the common staged Shaders directory so runtime package removal is a
    # manifest/build choice, while the renderer keeps one bytecode resolver.
    list(APPEND GE_SHADER_SOURCE_DIRS
        "${CMAKE_SOURCE_DIR}/Packages/fidelityfx-dof/Assets/Shaders"
        "${CMAKE_SOURCE_DIR}/Packages/film-simulation/Assets/Shaders"
        "${CMAKE_SOURCE_DIR}/Packages/chromatic-aberration/Assets/Shaders")
    # Probe shaders that only test executables load. Compiled into the same
    # staged Shaders directory, but never into a build that ships without tests.
    if(BUILD_TESTING AND EXISTS "${CMAKE_SOURCE_DIR}/Engine/Tests/Shaders")
        list(APPEND GE_SHADER_SOURCE_DIRS "${CMAKE_SOURCE_DIR}/Engine/Tests/Shaders")
    endif()
    file(GLOB GE_SHADER_COMMON_INCLUDES "${SHADER_SOURCE_DIR}/Includes/*.glsl")
    set(GE_VOLUMETRIC_FOG_SHADER_DIR "${CMAKE_SOURCE_DIR}/Engine/Modules/VolumetricFog/Shaders")
    if(EXISTS "${GE_VOLUMETRIC_FOG_SHADER_DIR}")
        list(APPEND GE_SHADER_SOURCE_DIRS "${GE_VOLUMETRIC_FOG_SHADER_DIR}")
    endif()
    set(GE_HEIGHT_FOG_SHADER_DIR "${CMAKE_SOURCE_DIR}/Engine/Modules/HeightFog/Shaders")
    if(EXISTS "${GE_HEIGHT_FOG_SHADER_DIR}")
        list(APPEND GE_SHADER_SOURCE_DIRS "${GE_HEIGHT_FOG_SHADER_DIR}")
    endif()
    # Ocean lives in a SUBdirectory of the shader root, and the auto-glob below
    # is not recursive, so none of it was ever compiled: the module fell back to
    # compiling GLSL at runtime, which no shipping runtime can do — the web
    # build reported six ocean compile failures at every boot. The includes are
    # root-relative ("Ocean/ocean_fft_common.glsl"), which -I${SHADER_SOURCE_DIR}
    # on the compile rules already satisfies.
    set(GE_OCEAN_SHADER_DIR "${SHADER_SOURCE_DIR}/Ocean")
    if(EXISTS "${GE_OCEAN_SHADER_DIR}")
        list(APPEND GE_SHADER_SOURCE_DIRS "${GE_OCEAN_SHADER_DIR}")
    endif()
    # Ambient occlusion owns a subdirectory of the shader root for the same reason
    # the ocean does, and the auto-glob below is not recursive.
    set(GE_AMBIENT_OCCLUSION_SHADER_DIR "${SHADER_SOURCE_DIR}/AmbientOcclusion")
    if(EXISTS "${GE_AMBIENT_OCCLUSION_SHADER_DIR}")
        list(APPEND GE_SHADER_SOURCE_DIRS "${GE_AMBIENT_OCCLUSION_SHADER_DIR}")
        file(GLOB GE_AMBIENT_OCCLUSION_INCLUDES "${GE_AMBIENT_OCCLUSION_SHADER_DIR}/*.glsl")
        list(APPEND GE_SHADER_COMMON_INCLUDES ${GE_AMBIENT_OCCLUSION_INCLUDES})
    endif()
    # Screen-space reflections own a subdirectory of the shader root for the same
    # reason ambient occlusion does, and the auto-glob below is not recursive.
    set(GE_SCREEN_SPACE_REFLECTIONS_SHADER_DIR "${SHADER_SOURCE_DIR}/ScreenSpaceReflections")
    if(EXISTS "${GE_SCREEN_SPACE_REFLECTIONS_SHADER_DIR}")
        list(APPEND GE_SHADER_SOURCE_DIRS "${GE_SCREEN_SPACE_REFLECTIONS_SHADER_DIR}")
        file(GLOB GE_SCREEN_SPACE_REFLECTIONS_INCLUDES "${GE_SCREEN_SPACE_REFLECTIONS_SHADER_DIR}/*.glsl")
        list(APPEND GE_SHADER_COMMON_INCLUDES ${GE_SCREEN_SPACE_REFLECTIONS_INCLUDES})
    endif()
    # Screen-space shadows own a subdirectory of the shader root for the same
    # reason ambient occlusion does, and the auto-glob below is not recursive.
    set(GE_SCREEN_SPACE_SHADOWS_SHADER_DIR "${SHADER_SOURCE_DIR}/ScreenSpaceShadows")
    if(EXISTS "${GE_SCREEN_SPACE_SHADOWS_SHADER_DIR}")
        list(APPEND GE_SHADER_SOURCE_DIRS "${GE_SCREEN_SPACE_SHADOWS_SHADER_DIR}")
        file(GLOB GE_SCREEN_SPACE_SHADOWS_INCLUDES "${GE_SCREEN_SPACE_SHADOWS_SHADER_DIR}/*.glsl")
        list(APPEND GE_SHADER_COMMON_INCLUDES ${GE_SCREEN_SPACE_SHADOWS_INCLUDES})
    endif()
    set(GE_VOLUMETRIC_CLOUDS_SHADER_DIR "${CMAKE_SOURCE_DIR}/Engine/Modules/VolumetricClouds/Shaders")
    if(EXISTS "${GE_VOLUMETRIC_CLOUDS_SHADER_DIR}")
        list(APPEND GE_SHADER_SOURCE_DIRS "${GE_VOLUMETRIC_CLOUDS_SHADER_DIR}")
    endif()
    set(SHADER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/Shaders)
    file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

    # List of shaders to compile (GLSL -> SPIR-V)
    set(VERTEX_SHADERS
        triangle.vert
        cube.vert
        vertex_input_triangle.vert
        fullscreen_noinput.vert
        conflict_vs.vert
        world_debug.vert
        world_debug_forwardplus.vert
        ubo_nudge.vert
    )

    set(FRAGMENT_SHADERS
        triangle.frag
        cube.frag
        vertex_input_triangle.frag
        sample_texture.frag
        solidcolor.frag
        letterbox_overlay.frag
        ubo_sample.frag
        conflict_fs.frag
        pbr_forward_shadowdebug.frag
        show_shadowmap.frag
        world_debug.frag
        world_debug_forwardplus.frag
        world_depthonly.frag
        forwardplus_lighting.frag
    )
    # Add PBR and tonemap shaders to compile lists
    list(APPEND VERTEX_SHADERS pbr_forward.vert)
    list(APPEND FRAGMENT_SHADERS pbr_forward.frag tonemap.frag copy.frag evsm_write.frag evsm_blur_h.frag evsm_blur_v.frag
                                msm_write.frag msm_blur_h.frag msm_blur_v.frag
                                bloom_threshold.frag bloom_downsample.frag bloom_combine.frag cas.frag post_fx_ldr_stack.frag selection_outline.frag selection_mask.frag selection_mask_alpha.frag)
    list(APPEND VERTEX_SHADERS selection_mask.vert selection_mask_alpha.vert)
    # Lens flare (instanced additive screen-space quads)
    list(APPEND VERTEX_SHADERS lens_flare.vert)
    list(APPEND FRAGMENT_SHADERS lens_flare.frag)

    # Sun glare (analytic additive veiling halo)
    list(APPEND VERTEX_SHADERS sun_glare.vert)
    list(APPEND VERTEX_SHADERS sun_glare_ms.vert)
    list(APPEND FRAGMENT_SHADERS sun_glare.frag)

    # Skeleton overlay shaders
    list(APPEND VERTEX_SHADERS lines.vert)
    list(APPEND FRAGMENT_SHADERS lines.frag)
    # The Scene View's world mark-up glow (desktop only: never in the web cook)
    list(APPEND VERTEX_SHADERS markup_glow.vert)
    list(APPEND VERTEX_SHADERS markup_glow_mesh.vert)
    list(APPEND FRAGMENT_SHADERS markup_glow.frag)

    set(COMPUTE_SHADERS
        frustum_culling.comp
        visibility_compaction.comp
        visibility_union.comp
        runtime_visibility_aggregate.comp
        draw_command_scatter.comp
        compact_visible_transforms.comp
        minimal_test.comp
        pc_compute.comp
        compute_write_image.comp
        spec_constants.comp
        brdf_lut.comp
        ubo_copy.comp
        msm_cascade.comp
        runtime_array_stride.comp
    )

    # One rule shape for every shader this module compiles, so the flag policy is
    # stated once. The include path in particular is not per-rule: a rule without
    # it still builds anything whose includes resolve next to the source and
    # fails only on a root-relative one, so an omission stays invisible until a
    # shader gains an include or moves directory.
    #
    # glslc writes the include set it actually opened to a depfile, which is the
    # only exact record of it: the DEPENDS globs cover Includes/ and UI/, and
    # every shader that includes across a module directory - the ocean cascade
    # and FFT headers, the fog and cloud commons - falls outside them.
    function(ge_compile_shader)
        cmake_parse_arguments(ARG "VULKAN13" "OUTPUT;SOURCE;COMMENT" "DEFINES;DEPENDS" ${ARGN})
        set(_ge_flags -I${SHADER_SOURCE_DIR})
        if(ARG_VULKAN13)
            # Subgroup arithmetic and ray query need SPIR-V >= 1.3/1.4.
            list(APPEND _ge_flags --target-env=vulkan1.3 --target-spv=spv1.6)
        endif()
        add_custom_command(
            OUTPUT ${ARG_OUTPUT}
            COMMAND ${GLSLC_EXECUTABLE} ${_ge_flags} ${ARG_DEFINES} ${ARG_SOURCE}
                    -o ${ARG_OUTPUT} -MD -MF ${ARG_OUTPUT}.d
            DEPENDS ${ARG_SOURCE} ${ARG_DEPENDS}
            DEPFILE ${ARG_OUTPUT}.d
            COMMENT "${ARG_COMMENT}"
            VERBATIM
        )
        set(GE_ALL_SHADER_SPV_OUTPUTS ${GE_ALL_SHADER_SPV_OUTPUTS} ${ARG_OUTPUT} PARENT_SCOPE)
    endfunction()

    # UI SDF shaders. One table of (output stem, stage, defines) rather than a
    # block per variant: the profile axis alone would otherwise double six
    # copies. Include dependencies are listed explicitly so an edit to any
    # UI/*.glsl recompiles every variant.
    #
    #   (none)   the Vulkan/Metal default
    #   subpixel dual-source blending: per-channel text coverage in a second
    #            fragment output, and wider glyph dilation for the outer taps
    #   encoded  UITargetSpace::EncodedSrgb — paint adapters pass raw sRGB bytes
    #            so the ROP interpolates encoded values. Fragment-only: the
    #            define reshapes no vertex-stage code
    #   compat   GE_COMPAT_PROFILE — fixed texture slots, hoisted derivatives and
    #            a per-run draw base, for the WebGPU-class target. Selected at
    #            load time by UIManager from the resolved RendererProfile
    file(GLOB UI_SDF_INCLUDES "${SHADER_SOURCE_DIR}/UI/*.glsl")
    set(UI_SDF_VARIANTS
        "ui_sdf|"
        "ui_sdf_subpixel|-DUI_SUBPIXEL_DUAL_SRC"
        "ui_sdf_compat|-DGE_COMPAT_PROFILE"
        "ui_sdf_subpixel_compat|-DUI_SUBPIXEL_DUAL_SRC,-DGE_COMPAT_PROFILE"
    )
    set(UI_SDF_FRAG_ONLY_VARIANTS
        "ui_sdf_encoded|-DUI_BLEND_SPACE_ENCODED"
        "ui_sdf_subpixel_encoded|-DUI_SUBPIXEL_DUAL_SRC,-DUI_BLEND_SPACE_ENCODED"
        "ui_sdf_encoded_compat|-DUI_BLEND_SPACE_ENCODED,-DGE_COMPAT_PROFILE"
        "ui_sdf_subpixel_encoded_compat|-DUI_SUBPIXEL_DUAL_SRC,-DUI_BLEND_SPACE_ENCODED,-DGE_COMPAT_PROFILE"
    )

    # _defines arrives comma-separated so it survives being one field of a
    # semicolon-delimited CMake list entry.
    function(ge_add_ui_sdf_variant _stem _stage _defines)
        string(REPLACE "," ";" _defineList "${_defines}")
        set(SHADER_SOURCE ${SHADER_SOURCE_DIR}/ui_sdf.${_stage})
        if(NOT EXISTS "${SHADER_SOURCE}")
            return()
        endif()
        set(SHADER_OUTPUT ${SHADER_OUTPUT_DIR}/${_stem}.${_stage}.spv)
        ge_compile_shader(
            OUTPUT ${SHADER_OUTPUT} SOURCE ${SHADER_SOURCE}
            DEFINES ${_defineList}
            DEPENDS ${UI_SDF_INCLUDES}
            COMMENT "Compiling UI shader: ${_stem}.${_stage}")
        set(GE_ALL_SHADER_SPV_OUTPUTS ${GE_ALL_SHADER_SPV_OUTPUTS} PARENT_SCOPE)
    endfunction()

    foreach(_ui_sdf_variant ${UI_SDF_VARIANTS})
        string(REPLACE "|" ";" _parts "${_ui_sdf_variant}")
        list(GET _parts 0 _stem)
        list(LENGTH _parts _partCount)
        set(_defines "")
        if(_partCount GREATER 1)
            list(GET _parts 1 _defines)
        endif()
        foreach(_ui_sdf_stage vert frag)
            ge_add_ui_sdf_variant("${_stem}" "${_ui_sdf_stage}" "${_defines}")
        endforeach()
    endforeach()

    foreach(_ui_sdf_variant ${UI_SDF_FRAG_ONLY_VARIANTS})
        string(REPLACE "|" ";" _parts "${_ui_sdf_variant}")
        list(GET _parts 0 _stem)
        list(GET _parts 1 _defines)
        ge_add_ui_sdf_variant("${_stem}" "frag" "${_defines}")
    endforeach()

        # Auto-include any additional shaders found in known shader roots (so new files compile without manual edits)
        foreach(_shader_dir IN LISTS GE_SHADER_SOURCE_DIRS)
            file(GLOB AUTO_VERT_SHADERS   "${_shader_dir}/*.vert")
            file(GLOB AUTO_FRAG_SHADERS   "${_shader_dir}/*.frag")
            file(GLOB AUTO_COMP_SHADERS   "${_shader_dir}/*.comp")
            foreach(FPATH ${AUTO_VERT_SHADERS})
                get_filename_component(SNAME ${FPATH} NAME)
                list(APPEND VERTEX_SHADERS ${SNAME})
            endforeach()
            foreach(FPATH ${AUTO_FRAG_SHADERS})
                get_filename_component(SNAME ${FPATH} NAME)
                list(APPEND FRAGMENT_SHADERS ${SNAME})
            endforeach()
            foreach(FPATH ${AUTO_COMP_SHADERS})
                get_filename_component(SNAME ${FPATH} NAME)
                list(APPEND COMPUTE_SHADERS ${SNAME})
            endforeach()
        endforeach()
        list(REMOVE_DUPLICATES VERTEX_SHADERS)
        list(REMOVE_DUPLICATES FRAGMENT_SHADERS)
        list(REMOVE_DUPLICATES COMPUTE_SHADERS)
        # Avoid duplicate output rules for shaders compiled via dedicated custom commands.
        list(REMOVE_ITEM FRAGMENT_SHADERS mesh_triangle.frag)
        list(REMOVE_ITEM VERTEX_SHADERS ui_sdf.vert)
        list(REMOVE_ITEM FRAGMENT_SHADERS ui_sdf.frag)
        # draw_command_scatter.comp uses subgroup arithmetic (P0 shadow-arc
        # per-slice triangle reduction), which needs SPIR-V >= 1.3; the generic
        # compute rule below targets glslc's vulkan1.0 default. Compile it with a
        # dedicated vulkan1.3/spv1.6 command instead.
        list(REMOVE_ITEM COMPUTE_SHADERS draw_command_scatter.comp)
        # rt_shadow_mask.comp uses GL_EXT_ray_query (needs SPIR-V >= 1.4); same
        # dedicated vulkan1.3/spv1.6 treatment as the scatter shader.
        list(REMOVE_ITEM COMPUTE_SHADERS rt_shadow_mask.comp)
        # ddgi_trace_hw.comp and ddgi_classify.comp use GL_EXT_ray_query too —
        # same reason, same treatment. The other DDGI kernels (blend/upload/
        # clear) are plain compute and stay in the generic COMPUTE_SHADERS glob.
        list(REMOVE_ITEM COMPUTE_SHADERS ddgi_trace_hw.comp)
        list(REMOVE_ITEM COMPUTE_SHADERS ddgi_classify.comp)

    # Compile vertex / fragment / compute shaders. Each produces an .spv file
    # which is accumulated into GE_ALL_SHADER_SPV_OUTPUTS; the single
    # CompileShaders target gates them all.
    foreach(SHADER ${VERTEX_SHADERS})
        set(SHADER_SOURCE "")
        foreach(_shader_dir IN LISTS GE_SHADER_SOURCE_DIRS)
            if(EXISTS "${_shader_dir}/${SHADER}")
                set(SHADER_SOURCE "${_shader_dir}/${SHADER}")
                break()
            endif()
        endforeach()
        set(SHADER_OUTPUT ${SHADER_OUTPUT_DIR}/${SHADER}.spv)
        if(EXISTS "${SHADER_SOURCE}")
            ge_compile_shader(
                OUTPUT ${SHADER_OUTPUT} SOURCE ${SHADER_SOURCE}
                DEPENDS ${GE_SHADER_COMMON_INCLUDES}
                COMMENT "Compiling vertex shader: ${SHADER}")
        endif()
    endforeach()

    foreach(SHADER ${FRAGMENT_SHADERS})
        set(SHADER_SOURCE "")
        foreach(_shader_dir IN LISTS GE_SHADER_SOURCE_DIRS)
            if(EXISTS "${_shader_dir}/${SHADER}")
                set(SHADER_SOURCE "${_shader_dir}/${SHADER}")
                break()
            endif()
        endforeach()
        set(SHADER_OUTPUT ${SHADER_OUTPUT_DIR}/${SHADER}.spv)
        if(EXISTS "${SHADER_SOURCE}")
            ge_compile_shader(
                OUTPUT ${SHADER_OUTPUT} SOURCE ${SHADER_SOURCE}
                DEPENDS ${GE_SHADER_COMMON_INCLUDES}
                COMMENT "Compiling fragment shader: ${SHADER}")
        endif()
    endforeach()

    foreach(SHADER ${COMPUTE_SHADERS})
        set(SHADER_SOURCE "")
        foreach(_shader_dir IN LISTS GE_SHADER_SOURCE_DIRS)
            if(EXISTS "${_shader_dir}/${SHADER}")
                set(SHADER_SOURCE "${_shader_dir}/${SHADER}")
                break()
            endif()
        endforeach()
        set(SHADER_OUTPUT ${SHADER_OUTPUT_DIR}/${SHADER}.spv)
        if(EXISTS "${SHADER_SOURCE}")
            ge_compile_shader(
                OUTPUT ${SHADER_OUTPUT} SOURCE ${SHADER_SOURCE}
                DEPENDS ${GE_SHADER_COMMON_INCLUDES}
                COMMENT "Compiling compute shader: ${SHADER}")
        endif()
    endforeach()

    # The skinning kernel's GE_COMPAT_PROFILE arm as a desktop Vulkan blob. Nothing ships or loads
    # it: it exists so GPUAnimationComputeTests runs the same arm the web cook translates to WGSL
    # (the constant-bound, masked level loop), which no desktop profile executes. It sits under
    # Shaders/Tests because bundle staging copies only the top level of Shaders/.
    file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR}/Tests)
    ge_compile_shader(
        OUTPUT ${SHADER_OUTPUT_DIR}/Tests/animation_skinning_compat.comp.spv
        SOURCE ${SHADER_SOURCE_DIR}/animation_skinning.comp
        DEFINES -DGE_COMPAT_PROFILE
        DEPENDS ${GE_SHADER_COMMON_INCLUDES}
        COMMENT "Compiling compute shader: animation_skinning.comp (compat profile arm)")
    # The SDSM receiver reduce's GE_COMPAT_PROFILE arm (its depth flush), on the same
    # terms, for ShadowReceiverReduceComputeTests.
    ge_compile_shader(
        OUTPUT ${SHADER_OUTPUT_DIR}/Tests/depth_reduce_compat.comp.spv
        SOURCE ${SHADER_SOURCE_DIR}/depth_reduce.comp
        DEFINES -DGE_COMPAT_PROFILE
        DEPENDS ${GE_SHADER_COMMON_INCLUDES}
        COMMENT "Compiling compute shader: depth_reduce.comp (compat profile arm)")

    # draw_command_scatter.comp: dedicated vulkan1.3/spv1.6 command (subgroup
    # arithmetic for the P0 shadow-arc per-slice triangle reduction). Output path
    # matches the generic rule's ${name}.comp.spv so _ge_add_shaderpkg_compute's
    # dependency is unchanged.
    set(SCATTER_COMP_SRC "")
    foreach(_shader_dir IN LISTS GE_SHADER_SOURCE_DIRS)
        if(EXISTS "${_shader_dir}/draw_command_scatter.comp")
            set(SCATTER_COMP_SRC "${_shader_dir}/draw_command_scatter.comp")
            break()
        endif()
    endforeach()
    if(SCATTER_COMP_SRC)
        set(SCATTER_COMP_SPV ${SHADER_OUTPUT_DIR}/draw_command_scatter.comp.spv)
        ge_compile_shader(VULKAN13
            OUTPUT ${SCATTER_COMP_SPV} SOURCE ${SCATTER_COMP_SRC}
            DEPENDS ${GE_SHADER_COMMON_INCLUDES}
            COMMENT "Compiling compute shader (vulkan1.3): draw_command_scatter.comp")

        # Coalesced scatter-hot variant (-DSCATTER_COMPACT): same source, binds
        # the 32 B mirror at binding 1. Same vulkan1.3/spv1.6 flags (subgroup
        # arithmetic). Output name feeds _ge_add_shaderpkg_compute below.
        set(SCATTER_COMPACT_COMP_SPV ${SHADER_OUTPUT_DIR}/draw_command_scatter_compact.comp.spv)
        ge_compile_shader(VULKAN13
            OUTPUT ${SCATTER_COMPACT_COMP_SPV} SOURCE ${SCATTER_COMP_SRC}
            DEFINES -DSCATTER_COMPACT
            DEPENDS ${GE_SHADER_COMMON_INCLUDES}
            COMMENT "Compiling compute shader (vulkan1.3): draw_command_scatter.comp [SCATTER_COMPACT]")

        # Instrumentation variants (-DSCATTER_STATS): identical scatter, plus the
        # per-slice drawnTriangles subgroup reduction + atomic. Default-OFF
        # (above) is the shipping path; these are selected only when
        # GE_SCATTER_STATS=1 (the bench shadow lanes read the triangle counts).
        # Both fetch axes get a stats twin so GE_SCATTER_COMPACT is honoured
        # independently of GE_SCATTER_STATS (the fetch-fallback keeps the stats
        # setting). Four packages total: {full-fat,compact} × {stats-off,on}.
        set(SCATTER_STATS_COMP_SPV ${SHADER_OUTPUT_DIR}/draw_command_scatter_stats.comp.spv)
        ge_compile_shader(VULKAN13
            OUTPUT ${SCATTER_STATS_COMP_SPV} SOURCE ${SCATTER_COMP_SRC}
            DEFINES -DSCATTER_STATS
            DEPENDS ${GE_SHADER_COMMON_INCLUDES}
            COMMENT "Compiling compute shader (vulkan1.3): draw_command_scatter.comp [SCATTER_STATS]")

        set(SCATTER_COMPACT_STATS_COMP_SPV ${SHADER_OUTPUT_DIR}/draw_command_scatter_compact_stats.comp.spv)
        ge_compile_shader(VULKAN13
            OUTPUT ${SCATTER_COMPACT_STATS_COMP_SPV} SOURCE ${SCATTER_COMP_SRC}
            DEFINES -DSCATTER_COMPACT -DSCATTER_STATS
            DEPENDS ${GE_SHADER_COMMON_INCLUDES}
            COMMENT "Compiling compute shader (vulkan1.3): draw_command_scatter.comp [SCATTER_COMPACT SCATTER_STATS]")
    endif()

    # rt_shadow_mask.comp: dedicated vulkan1.3/spv1.6 command (GL_EXT_ray_query
    # needs SPIR-V >= 1.4). Output path matches the generic rule's
    # ${name}.comp.spv so _ge_add_shaderpkg_compute's dependency is unchanged.
    set(RT_SHADOW_MASK_COMP_SRC "")
    foreach(_shader_dir IN LISTS GE_SHADER_SOURCE_DIRS)
        if(EXISTS "${_shader_dir}/rt_shadow_mask.comp")
            set(RT_SHADOW_MASK_COMP_SRC "${_shader_dir}/rt_shadow_mask.comp")
            break()
        endif()
    endforeach()
    if(RT_SHADOW_MASK_COMP_SRC)
        set(RT_SHADOW_MASK_COMP_SPV ${SHADER_OUTPUT_DIR}/rt_shadow_mask.comp.spv)
        ge_compile_shader(VULKAN13
            OUTPUT ${RT_SHADOW_MASK_COMP_SPV} SOURCE ${RT_SHADOW_MASK_COMP_SRC}
            DEPENDS ${GE_SHADER_COMMON_INCLUDES}
            COMMENT "Compiling compute shader (vulkan1.3): rt_shadow_mask.comp")
    endif()

    # ddgi_trace_hw.comp: dedicated vulkan1.3/spv1.6 command (GL_EXT_ray_query
    # needs SPIR-V >= 1.4) — same treatment as rt_shadow_mask.comp above.
    set(DDGI_TRACE_HW_COMP_SRC "")
    foreach(_shader_dir IN LISTS GE_SHADER_SOURCE_DIRS)
        if(EXISTS "${_shader_dir}/ddgi_trace_hw.comp")
            set(DDGI_TRACE_HW_COMP_SRC "${_shader_dir}/ddgi_trace_hw.comp")
            break()
        endif()
    endforeach()
    if(DDGI_TRACE_HW_COMP_SRC)
        set(DDGI_TRACE_HW_COMP_SPV ${SHADER_OUTPUT_DIR}/ddgi_trace_hw.comp.spv)
        ge_compile_shader(VULKAN13
            OUTPUT ${DDGI_TRACE_HW_COMP_SPV} SOURCE ${DDGI_TRACE_HW_COMP_SRC}
            DEPENDS ${GE_SHADER_COMMON_INCLUDES}
            COMMENT "Compiling compute shader (vulkan1.3): ddgi_trace_hw.comp")
    endif()

    # ddgi_classify.comp: same dedicated vulkan1.3/spv1.6 treatment (also
    # GL_EXT_ray_query, no buffer_reference/int64 — a smaller descriptor set
    # than ddgi_trace_hw.comp but the same SPIR-V version requirement).
    set(DDGI_CLASSIFY_COMP_SRC "")
    foreach(_shader_dir IN LISTS GE_SHADER_SOURCE_DIRS)
        if(EXISTS "${_shader_dir}/ddgi_classify.comp")
            set(DDGI_CLASSIFY_COMP_SRC "${_shader_dir}/ddgi_classify.comp")
            break()
        endif()
    endforeach()
    if(DDGI_CLASSIFY_COMP_SRC)
        set(DDGI_CLASSIFY_COMP_SPV ${SHADER_OUTPUT_DIR}/ddgi_classify.comp.spv)
        ge_compile_shader(VULKAN13
            OUTPUT ${DDGI_CLASSIFY_COMP_SPV} SOURCE ${DDGI_CLASSIFY_COMP_SRC}
            DEPENDS ${GE_SHADER_COMMON_INCLUDES}
            COMMENT "Compiling compute shader (vulkan1.3): ddgi_classify.comp")
    endif()

    message(STATUS "✅ Configured automatic GLSL to SPIR-V compilation")

else()
    message(STATUS "ℹ️ Using pre-compiled SPIR-V shaders (glslc not available)")
endif()

# Create the single CompileShaders target now that all outputs are known.
# Marked ALL so it builds as part of the default solution build; depending on
# GE_ALL_SHADER_SPV_OUTPUTS makes MSBuild check timestamps once per output
# rather than spinning up one project per shader.
add_custom_target(CompileShaders ALL
    DEPENDS ${GE_ALL_SHADER_SPV_OUTPUTS}
    COMMENT "Compiling all shaders to SPIR-V"
)

# Published for the staging stamps (StageEditorShaders and friends): they must
# re-run on raw .spv-only changes too, not just .shaderpkg repacks — a shader
# edit whose package didn't change used to leave the staged Assets/Shaders
# .spv stale (the classic stale-SPIR-V trap).
set_property(GLOBAL PROPERTY GE_SHADER_SPV_OUTPUTS "${GE_ALL_SHADER_SPV_OUTPUTS}")

# Core library sources
set(RENDERING_CORE_SOURCES
	    # Core interfaces
	    Source/Core/Device.cpp
	    Source/Core/RendererProfile.cpp
	    Source/Core/CommandList.cpp
	    # The compat-profile flag the resolved RendererProfile sets. Core rather
	    # than part of the Materials block below: ShaderVariantKey reads it in
	    # every configuration, including builds without nlohmann.
	    Source/Materials/ShaderProfileDefines.cpp
	    # Immediate-mode render graph (the RenderGraph rewrite; dirs rename to
	    # RenderGraph in this slice-9 commit)
	    Source/Core/RenderGraph/RGGraph.cpp
	    Source/Core/RenderGraph/RGFrame.cpp
	    Source/Core/RenderGraph/RGFrameStamp.cpp
	    Source/Core/RenderGraph/RGRecord.cpp
	    Source/Core/RenderGraph/RGSchedule.cpp
	    Source/Core/RenderGraph/RGBarriers.cpp
	    Source/Core/RenderGraph/RGSubmission.cpp
	    Source/Core/RenderGraph/RGTransientPool.cpp
	    Source/Core/RenderGraph/RGResourcePool.cpp
	    Source/Core/RenderGraph/RGUploadRing.cpp
	    Source/Core/RenderGraph/RGFullscreen.cpp
	    Source/Core/RenderGraph/RGWindowFrame.cpp
	    Source/Core/PipelineTypes.cpp
	    Source/Core/PipelineCache.cpp
	    Source/Core/PipelineBuildTable.cpp
	    Source/Core/PipelineDescTranslator.cpp
	    Source/Core/ResourceManager.cpp
	    Source/Core/BindlessResourceManager.cpp
	    Source/Core/SpecializationConstants.cpp
	    # Note: BufferManager, TextureManager, PipelineManager, SamplerManager are header-only
	    # and compiled inline with VulkanDevice.cpp which provides the Vulkan context
		    Source/Core/GPUScene.cpp
		    Source/Core/GPUCulling.cpp
		    Source/Core/PrevVisibleResetTracker.cpp
		    Source/Core/GPUDrawStreamBuilder.cpp
		    Source/Core/RecomputeElision.cpp
		    Source/Core/FrustumCullingStrategy.cpp
		    Source/Core/HzbCullingStrategy.cpp
		    Source/CameraDerivation.cpp

	    # Common utilities
	    Source/Common/Utils.cpp
		    Source/Common/Math.cpp
		    Source/Common/Frustum.cpp
		    Source/Common/CubeLutFileParser.cpp
		    Source/Common/CubeLutGpuUpload.cpp

	    ${CMAKE_CURRENT_SOURCE_DIR}/Include/Rendering/Core/DescriptorWriter.h

	    # ShaderDesc sidecar utilities (added later with target_sources below)

    # Modular pass implementations
    Source/Core/HotReloadIntegration.cpp
    Source/Core/DebugCommands.cpp

	    Source/Passes/SRGBEncodePass.cpp
    Source/Passes/TemporalAAOptions.cpp
    Source/Passes/TemporalDither.cpp
    Source/Passes/TonemapPass.cpp
    Source/Passes/PixelPerfectUpscalePass.cpp

    # Sky
    Source/Sky/AtmosphereTransmittance.cpp
    Source/Sky/SkyRenderer.cpp
    Source/Sky/SolarPath.cpp
)

# Platform-specific sources
set(RENDERING_VULKAN_SOURCES
    Source/Vulkan/TextureUsagePolicy.cpp
    Source/Vulkan/ValidationStats.cpp
    Source/Vulkan/VulkanDevice.cpp
    Source/Vulkan/VulkanAccelerationStructures.cpp
    Source/Vulkan/DeviceHealthState.cpp
    Source/Vulkan/VulkanGpuCheckpoints.cpp
    Source/Vulkan/VulkanDeviceFault.cpp
    Source/Vulkan/VulkanToolingInfo.cpp
    Source/Vulkan/VulkanCommandList.cpp
    Source/Vulkan/VulkanResources.cpp
    Source/Vulkan/VulkanMemory.cpp
    Source/Vulkan/VulkanSwapchain.cpp
    Source/Vulkan/VulkanPipelineCache.cpp
    Source/Vulkan/VulkanQueryPool.cpp
    Source/Vulkan/VulkanQueueFamilySelection.cpp
    Source/Vulkan/VulkanSharedInstance.cpp
)

set(RENDERING_DIRECTX12_SOURCES
    Source/DirectX12/D3D12Device.cpp
    Source/DirectX12/D3D12CommandList.cpp
    Source/DirectX12/D3D12Resources.cpp
    Source/DirectX12/D3D12Memory.cpp
    Source/DirectX12/D3D12Swapchain.cpp
)

set(RENDERING_WEBGPU_SOURCES
    Source/WebGPU/WebGpuDevice.cpp
    Source/WebGPU/WebGpuCommandList.cpp
    Source/WebGPU/WebGpuConversions.cpp
    Source/WebGPU/WebGpuPipeline.cpp
    Source/WebGPU/WebGpuQueryPool.cpp
    Source/WebGPU/WebGpuResources.cpp
    Source/WebGPU/WebGpuSurfaceBridge.cpp
    Source/WebGPU/WebGpuSwapchain.cpp
    Source/WebGPU/WebGpuUnsupported.cpp
)

# Only the surface bridge consumes Platform's header-only native-window access.
# Standalone renderer consumers need no Platform runtime objects for this.
set_property(SOURCE Source/WebGPU/WebGpuSurfaceBridge.cpp APPEND PROPERTY
    INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/Engine/Modules/Platform/Include)

# The window frame reads GE_ASYNC_COMPUTE through Platform's header-only
# environment switch, the one rule every GE_ switch follows.
set_property(SOURCE Source/Core/RenderGraph/RGWindowFrame.cpp APPEND PROPERTY
    INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/Engine/Modules/Platform/Include)

set(RENDERING_METAL_SOURCES
    # Single TU that defines the metal-cpp *_PRIVATE_IMPLEMENTATION symbols.
    Source/Metal/MetalCppImpl.cpp
    Source/Metal/MetalDevice.cpp
    Source/Metal/MetalHdrOutput.cpp
    Source/Metal/MetalCommandList.cpp
    Source/Metal/MetalResources.cpp
    Source/Metal/MetalTransientDescriptorArena.cpp
    Source/Metal/MetalSwapchain.cpp
    Source/Metal/MetalLayerBridge.cpp
    Source/Metal/MetalShaderTranslator.cpp
    Source/Metal/MetalQueryPool.cpp
    Source/Metal/MetalSubmitQueue.cpp
    Source/Metal/MetalAccelerationStructures.cpp
    Source/Metal/MetalIndirectCountResources.cpp
    Source/Metal/MetalIndirectCommandArena.cpp
    Source/Metal/MetalIndirectCountEncoder.cpp
)

# Create main library
add_library(GameEngineRendering OBJECT
	${RENDERING_CORE_SOURCES}
)

    # Core engine dependencies
	target_link_libraries(GameEngineRendering PUBLIC Logger)
	# Types: header-only basic types module exposing "Types/Types.h" used by
	#        public rendering headers such as Rendering/CameraTypes.h.
	target_link_libraries(GameEngineRendering PUBLIC Types)
	# Mathematics: canonical GLM-backed math module exposing "Mathematics/..." headers
	target_link_libraries(GameEngineRendering PUBLIC Mathematics)
	# FileSystem: publishing a shader package or a dumped source goes through
	# the one implementation that knows which hosts can safely rename a file.
	# PRIVATE — Engine.dll splices FileSystem in itself; test executables that
	# link this object library directly name FileSystemObjects instead.
	target_link_libraries(GameEngineRendering PRIVATE FileSystem)

	# SkyRenderer loads the single full-moon texture directly for the procedural sky.
	# The stb implementation is supplied by the Engine target in app builds.
	find_package(stb CONFIG QUIET)
	if(TARGET stb::stb)
		target_link_libraries(GameEngineRendering PRIVATE stb::stb)
		target_compile_definitions(GameEngineRendering PRIVATE GE_HAVE_STB)
	else()
		find_path(STB_INCLUDE_DIRS "stb_image.h")
		if(STB_INCLUDE_DIRS)
			target_include_directories(GameEngineRendering PRIVATE ${STB_INCLUDE_DIRS})
			target_compile_definitions(GameEngineRendering PRIVATE GE_HAVE_STB)
		endif()
	endif()

# Prevent PDB contention when /MP is enabled by allowing shared PDB access
if(MSVC)
	target_compile_options(GameEngineRendering PRIVATE /FS /bigobj)
endif()
# Precompiled headers to speed up compilation for this large module
# Use standard library headers only to avoid target coupling
if (COMMAND target_precompile_headers)
    target_precompile_headers(GameEngineRendering PRIVATE
        <vector> <string> <memory> <unordered_map> <unordered_set>
        <optional> <algorithm> <span> <cstdint> <cstring>
    )
endif()

# Add platform-specific sources
if(RENDERING_HAS_VULKAN)
    target_sources(GameEngineRendering PRIVATE ${RENDERING_VULKAN_SOURCES})
    target_sources(GameEngineRendering PRIVATE Source/Vulkan/DescriptorSetAllocator.cpp)
    target_sources(GameEngineRendering PRIVATE Source/Vulkan/VulkanDescriptorBufferPool.cpp)

    target_include_directories(GameEngineRendering PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Source/Vulkan)
    # Core/EngineVersion.h (the root CMakeLists writes it from VERSION) for the Vulkan
    # instance's engineVersion.
    target_include_directories(GameEngineRendering PRIVATE "${CMAKE_BINARY_DIR}/generated")
endif()

# Materials, shader-graph and shader-package plumbing are API-agnostic (the
# document/graph model feeds every backend); only the Vulkan sources above and
# the Vulkan/VMA links below belong under the Vulkan gate.
# Link json if available (fetch if missing)
find_package(nlohmann_json CONFIG QUIET)
    if(NOT nlohmann_json_FOUND)
        message(STATUS "nlohmann_json not found; fetching via FetchContent...")
        FetchContent_Declare(
            nlohmann_json
            GIT_REPOSITORY https://github.com/nlohmann/json.git
            GIT_TAG        v3.11.3
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(nlohmann_json)
        if(TARGET nlohmann_json::nlohmann_json)
            set(nlohmann_json_FOUND TRUE)
        endif()
    endif()
    if(nlohmann_json_FOUND)
        # Materials
        target_sources(GameEngineRendering PRIVATE
            Source/Materials/LegacyMaterialLanes.cpp
            Source/Materials/MaterialDocument.cpp
            Source/Materials/MaterialBuilder.cpp
            Source/Materials/MaterialBuildService.cpp
            Source/Materials/MaterialKeywordDerivation.cpp
            Source/Materials/MaterialPropertyBinding.cpp
            Source/Materials/MaterialShaderIncludeRoots.cpp
            Source/Materials/MaterialValidation.cpp
            Source/Materials/ShaderCompileService.cpp
            Source/Materials/ShaderIncludeClosure.cpp
            Source/Materials/ShaderSourceCache.cpp
            Source/Materials/SlangCompileLane.cpp
            Source/Materials/ShaderLayoutShape.cpp
            Source/Materials/ShaderReflection.cpp
            Source/Materials/ShaderMetaJson.cpp
            Source/Materials/ShaderMetaValidation.cpp
            Source/Materials/ShaderComposer.cpp
            Source/Materials/ShaderPropertyTable.cpp
            Source/Materials/ShaderPropertyTableCache.cpp
            Source/Materials/ShaderCapabilityDetector.cpp
            Source/Materials/SurfaceShaderTemplate.cpp
            Source/ShaderGraph/SgTypes.cpp
            Source/ShaderGraph/SgTagParser.cpp
            Source/ShaderGraph/SgGlslSignatureReader.cpp
            Source/ShaderGraph/SgPseudoNodes.cpp
            Source/ShaderGraph/SgNodeReflector.cpp
            Source/ShaderGraph/SgGraphCompiler.cpp
            Source/ShaderGraph/SgGraphFileIO.cpp
            Source/ShaderGraph/SgPropertyBinding.cpp
        )
        # Shader package IO (shaderpkg)
        target_sources(GameEngineRendering PRIVATE
            Source/ShaderCache/ShaderMetaBinary.cpp
            Source/ShaderCache/ShaderPackageContainer.cpp
            Source/ShaderCache/ShaderPackageIO.cpp
        )
        target_include_directories(GameEngineRendering PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Include)
        target_link_libraries(GameEngineRendering PUBLIC nlohmann_json::nlohmann_json)
            # Reflection cache
            target_sources(GameEngineRendering PRIVATE
                Source/Materials/ReflectionCache.cpp
                Include/Rendering/Materials/ReflectionCache.h
            )

        message(STATUS "nlohmann_json found and linked")
    else()
        message(WARNING "nlohmann_json not found; ShaderMetaJson will fail to compile if used")
    endif()

    # SPIR-V Reflection integration via vcpkg (required when enabled)
    if (RENDERING_ENABLE_SPIRV_REFLECTION)
        find_package(SPIRV-Reflect CONFIG QUIET)
        if (TARGET SPIRV-Reflect::SPIRV-Reflect)
            target_link_libraries(GameEngineRendering PUBLIC SPIRV-Reflect::SPIRV-Reflect)
        else()
            find_package(unofficial-spirv-reflect CONFIG REQUIRED)
            target_link_libraries(GameEngineRendering PUBLIC unofficial::spirv-reflect)
        endif()
	        # vcpkg-installed spirv_reflect.h defaults to including "./include/spirv/unified1/spirv.h",
	        # which does not resolve in standard package layouts. Prefer the system SPIR-V headers.
	        target_compile_definitions(GameEngineRendering PUBLIC SPIRV_REFLECT_USE_SYSTEM_SPIRV_H=1)
        target_compile_definitions(GameEngineRendering PUBLIC RENDERING_ENABLE_SPIRV_REFLECTION=1)
        message(STATUS "SPIR-V reflection enabled and linked")
    else()
        target_compile_definitions(GameEngineRendering PUBLIC RENDERING_ENABLE_SPIRV_REFLECTION=0)
    endif()

    # Shader compilation via shaderc. Not editor-only: ShaderCompileService is the
    # runtime GLSL->SPIR-V compiler behind every material variant, and nothing
    # prebakes material variants at package time, so a SHIPPED game compiles them
    # on demand and needs shaderc too (the Player gets it through Engine).
    # vcpkg exposes this as the 'unofficial-shaderc' package; the cmake/ports
    # overlay builds it as a DLL on dynamic-linkage triplets (x64-windows), so
    # prefer unofficial::shaderc::shaderc_shared and fall back to the static
    # target that static-linkage triplets (macOS/Linux) export.
    find_package(shaderc CONFIG QUIET)
    if (TARGET shaderc::shaderc OR TARGET shaderc::shaderc_combined)
        if (TARGET shaderc::shaderc)
            target_link_libraries(GameEngineRendering PUBLIC shaderc::shaderc)
            message(STATUS "shaderc found and linked (shaderc::shaderc)")
        else()
            target_link_libraries(GameEngineRendering PUBLIC shaderc::shaderc_combined)
            message(STATUS "shaderc found and linked (shaderc::shaderc_combined)")
        endif()
        target_compile_definitions(GameEngineRendering PUBLIC RENDERING_HAS_SHADERC=1)
    else()
        find_package(unofficial-shaderc CONFIG QUIET)
        if (TARGET unofficial::shaderc::shaderc_shared)
            target_link_libraries(GameEngineRendering PUBLIC unofficial::shaderc::shaderc_shared)
            target_compile_definitions(GameEngineRendering PUBLIC RENDERING_HAS_SHADERC=1)
            message(STATUS "shaderc found and linked (unofficial::shaderc::shaderc_shared)")
        elseif (TARGET unofficial::shaderc::shaderc)
            target_link_libraries(GameEngineRendering PUBLIC unofficial::shaderc::shaderc)
            target_compile_definitions(GameEngineRendering PUBLIC RENDERING_HAS_SHADERC=1)
            message(STATUS "shaderc found and linked (unofficial::shaderc::shaderc)")
        elseif (WIN32)
            # Without shaderc, material shader compilation silently compiles out
            # (RENDERING_HAS_SHADERC=0) in the editor AND in shipped games. The
            # vcpkg manifest declares shaderc on every platform, so on Windows a
            # missing target is always a broken install, never a supported
            # configuration.
            message(FATAL_ERROR "shaderc: no usable target "
                "(expected unofficial::shaderc::shaderc_shared or unofficial::shaderc::shaderc). "
                "The vcpkg install is stale or incomplete: reconfigure with "
                "VCPKG_MANIFEST_INSTALL=ON so the cmake/ports/shaderc overlay is (re)installed.")
        else()
            target_compile_definitions(GameEngineRendering PUBLIC RENDERING_HAS_SHADERC=0)
            message(STATUS "shaderc not found; material shader compilation disabled")
        endif()
    endif()

if(RENDERING_HAS_VULKAN)
    target_include_directories(GameEngineRendering PUBLIC ${Vulkan_INCLUDE_DIRS})
    target_link_libraries(GameEngineRendering PUBLIC ${Vulkan_LIBRARIES})
endif()
# Link GLFW globally so dependents (including tests) resolve WSI symbols when the library is built with GLFW support
if(EMSCRIPTEN)
    # GLFW headers live in the emscripten sysroot; the implementation is the
    # runtime's JS GLFW, pulled in at executable link.
    target_link_options(GameEngineRendering INTERFACE "SHELL:-s USE_GLFW=3")
else()
    # We already require glfw3 via find_package above
    target_link_libraries(GameEngineRendering PUBLIC glfw)
endif()
# Propagate HAS_GLFW define to dependents that may use conditional compilation
target_compile_definitions(GameEngineRendering PUBLIC HAS_GLFW=1)

# Mirror the Engine-level profiling switch so RenderGraph can strip per-pass
# timestamp queries in shipping builds. Options are defined in Engine's top
# CMakeLists.txt; fall back to ON if someone builds the Rendering module
# standalone.
if(NOT DEFINED ENABLE_GPU_PROFILING)
    set(ENABLE_GPU_PROFILING ON)
endif()
target_compile_definitions(GameEngineRendering PUBLIC GE_ENABLE_GPU_PROFILING=$<BOOL:${ENABLE_GPU_PROFILING}>)

if(RENDERING_HAS_VULKAN)
    target_compile_definitions(GameEngineRendering PUBLIC RENDERING_HAS_VULKAN=1)
    # Enable debug barrier capture in Debug builds so tests can introspect barrier details.
    # Must be PUBLIC so the VulkanDevice class layout is consistent across all translation units.
    target_compile_definitions(GameEngineRendering PUBLIC $<$<CONFIG:Debug>:GE_ENABLE_DEBUG_BARRIERS=1>)

    # Link VMA (Vulkan Memory Allocator) interface target from vcpkg
    if(VMA_FOUND)
        target_link_libraries(GameEngineRendering PUBLIC GPUOpen::VulkanMemoryAllocator)
        target_compile_definitions(GameEngineRendering PUBLIC RENDERING_HAS_VMA=1)
        # Only add materials sources if nlohmann_json is available (guard against missing dep)
        if(nlohmann_json_FOUND)
            target_sources(GameEngineRendering PRIVATE Source/Materials/ShaderReflection.cpp)
            target_sources(GameEngineRendering PRIVATE Source/Materials/ShaderMetaJson.cpp)
            target_include_directories(GameEngineRendering PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/Include)
        endif()
        message(STATUS "VMA linked to GameEngineRendering (GPUOpen::VulkanMemoryAllocator)")
    endif()
endif()

# Link with GenerationalVector library for high-performance resource management
# If the target is not available in this sub-build, fall back to header-only include path
if(TARGET GenerationalVector)
    target_link_libraries(GameEngineRendering PUBLIC GenerationalVector)
else()
    message(STATUS "GenerationalVector target not found in this build - using header include path fallback")
    target_include_directories(GameEngineRendering PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../GenerationalVector/Include)
endif()

# Make sure shaders are compiled before building the library
add_dependencies(GameEngineRendering CompileShaders)

if(RENDERING_HAS_DIRECTX12)
    target_sources(GameEngineRendering PRIVATE ${RENDERING_DIRECTX12_SOURCES})
    target_link_libraries(GameEngineRendering PUBLIC d3d12 dxgi dxguid)
    target_compile_definitions(GameEngineRendering PUBLIC RENDERING_HAS_DIRECTX12=1)
endif()

if(RENDERING_HAS_WEBGPU)
    target_sources(GameEngineRendering PRIVATE ${RENDERING_WEBGPU_SOURCES})
    target_compile_definitions(GameEngineRendering PUBLIC RENDERING_HAS_WEBGPU=1)
    if(EMSCRIPTEN)
        # emdawnwebgpu supplies webgpu.h at compile and the Dawn JS bindings
        # at link; the port flag must reach every TU that includes webgpu.h
        # and every executable that links this library.
        target_compile_options(GameEngineRendering PUBLIC "--use-port=emdawnwebgpu")
        target_link_options(GameEngineRendering INTERFACE "--use-port=emdawnwebgpu")
    else()
        target_include_directories(GameEngineRendering SYSTEM PRIVATE ${WGPU_NATIVE_INCLUDE_DIR})
        target_link_libraries(GameEngineRendering PUBLIC ${WGPU_NATIVE_LIBRARY})
        if(APPLE)
            # wgpu-native's Metal backend needs the surface's CAMetalLayer plumbing.
            target_link_libraries(GameEngineRendering PUBLIC
                "-framework Metal"
                "-framework QuartzCore")
        elseif(WIN32)
            # wgpu-native is a static Rust library; its std runtime imports the NT
            # file/pipe entry points, the user-profile lookup and Winsock directly,
            # and the prebuilt archive carries no link directives for them.
            target_link_libraries(GameEngineRendering PUBLIC ntdll userenv ws2_32)
        endif()
    endif()
endif()

if(RENDERING_HAS_METAL)
    target_sources(GameEngineRendering PRIVATE ${RENDERING_METAL_SOURCES})
    target_compile_definitions(GameEngineRendering PUBLIC RENDERING_HAS_METAL=1)

    # Vendored metal-cpp headers (no vcpkg port exists). SYSTEM silences the
    # vendor headers under -Wall -Wextra.
    target_include_directories(GameEngineRendering SYSTEM PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../ThirdParty/metal-cpp)

    target_link_libraries(GameEngineRendering PUBLIC
        "-framework Metal"
        "-framework QuartzCore"
        "-framework Foundation"
        "-framework CoreGraphics"
        objc)

    # SPIRV-Cross MSL translation (used from Metal pipeline creation).
    find_package(spirv_cross_core CONFIG REQUIRED)
    find_package(spirv_cross_glsl CONFIG REQUIRED)
    find_package(spirv_cross_msl CONFIG REQUIRED)
    target_link_libraries(GameEngineRendering PRIVATE
        spirv-cross-msl spirv-cross-glsl spirv-cross-core)
endif()

# Include directories
target_include_directories(GameEngineRendering
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Include>
        $<INSTALL_INTERFACE:include>
        ${RENDERDOC_APP_API_INCLUDE_DIR}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

# Compiler-specific settings
if(MSVC)
    # /we4062: a switch with no `default:` that misses an enumerator is an error.
    # Adding a value to a backend-facing enum (BufferMemoryUsage, TextureFormat,
    # ResourceState) otherwise falls through silently on every backend that did
    # not get taught about it, and a residency or format value resolved by
    # accident is not visible in any test that does not already know to look.
    # C4062 is off by default at /W4 and only fires where there is no `default:`,
    # so it enforces exactly the switches that opted into exhaustiveness.
    target_compile_options(GameEngineRendering PRIVATE /W4 /wd4100 /we4062)  # /wd4100: unused parameters
    target_compile_definitions(GameEngineRendering PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
else()
    # -Werror=switch is the GCC/Clang counterpart of MSVC's /we4062 above.
    target_compile_options(GameEngineRendering PRIVATE -Wall -Wextra -Wno-unused-parameter -Werror=switch)
endif()

# Link with existing engine components (disabled for standalone build)
# if(TARGET Engine)
#     target_link_libraries(GameEngineRendering PUBLIC Engine)
# endif()

message(STATUS "Cascaded Shadow Mapping example configured with proper architecture demonstration!")

# ShaderReflect CLI tool: build when tools, tests, or examples are enabled.
# Also build it when shaders are compiled so we can package built-ins into `.shaderpkg`.
#
# GE_SHADERREFLECT_EXECUTABLE: optional host-native ShaderReflect for cross-compiles
# (e.g. x64 Steam Deck target built on linux/arm64). SPIR-V reflection is platform-
# independent; only the tool binary must run on the build host.
set(GE_SHADERREFLECT_EXECUTABLE "" CACHE FILEPATH
    "Host-native ShaderReflect for cross-compile .shaderpkg packaging")
if(GE_SHADERREFLECT_EXECUTABLE AND NOT EXISTS "${GE_SHADERREFLECT_EXECUTABLE}")
    message(FATAL_ERROR
        "GE_SHADERREFLECT_EXECUTABLE='${GE_SHADERREFLECT_EXECUTABLE}' does not exist")
endif()

if(BUILD_TOOLS OR BUILD_TESTING OR GLSLC_EXECUTABLE)
    if(NOT GE_SHADERREFLECT_EXECUTABLE)
        if(NOT RENDERING_ENABLE_SPIRV_REFLECTION)
            message(FATAL_ERROR "ShaderReflect requires RENDERING_ENABLE_SPIRV_REFLECTION=ON")
        endif()
        include(${CMAKE_CURRENT_SOURCE_DIR}/Tools/ShaderReflect/AddShaderReflectTargets.cmake)
        ge_add_shader_reflect_targets(RENDERING_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
    else()
        message(STATUS "Using host ShaderReflect for packaging: ${GE_SHADERREFLECT_EXECUTABLE}")
    endif()
endif()

# Package built-in shaders into `.shaderpkg` files in `${CMAKE_BINARY_DIR}/Shaders`.
# A `.shaderpkg` carries compiled SPIR-V plus reflection metadata, and is what the
# render-graph nodes load by name. The loose `.spv` outputs staged alongside are
# ALSO read at runtime — sky/IBL/skinning/shadow-depth compute and fullscreen
# passes load `Shaders/<name>.spv` directly.
#
# All .shaderpkg outputs are collected into a single CompileShaderPkgs target
# (created at the end of this block); a GLOBAL property lets the function bodies
# append into the same list across scopes. One target rather than one per package
# keeps ~40 MSBuild projects of pure scheduler overhead out of the build.
if (TARGET CompileShaders AND (TARGET ShaderReflect OR GE_SHADERREFLECT_EXECUTABLE))
    if(GE_SHADERREFLECT_EXECUTABLE)
        set(_GE_SHADERREFLECT_CMD "${GE_SHADERREFLECT_EXECUTABLE}")
        set(_GE_SHADERREFLECT_TOOL_DEP "")
    else()
        set(_GE_SHADERREFLECT_CMD "$<TARGET_FILE:ShaderReflect>")
        set(_GE_SHADERREFLECT_TOOL_DEP ShaderReflect)
    endif()

    set_property(GLOBAL PROPERTY GE_SHADERPKG_OUTPUTS "")

    function(_ge_add_shaderpkg_compute pkg_name cs_base)
        set(_OUT "${CMAKE_BINARY_DIR}/Shaders/${pkg_name}.shaderpkg")
        set(_CS  "${CMAKE_BINARY_DIR}/Shaders/${cs_base}.spv")
        set(_PKG_DEPENDS "${_CS}")
        if(_GE_SHADERREFLECT_TOOL_DEP)
            list(APPEND _PKG_DEPENDS ${_GE_SHADERREFLECT_TOOL_DEP})
        endif()
        add_custom_command(
            OUTPUT  "${_OUT}"
            COMMAND "${_GE_SHADERREFLECT_CMD}" --cs "${_CS}" --out "${_OUT}"
            DEPENDS ${_PKG_DEPENDS}
            COMMENT "Packaging compute shaderpkg: ${pkg_name}"
            VERBATIM
        )
        set_property(GLOBAL APPEND PROPERTY GE_SHADERPKG_OUTPUTS "${_OUT}")
    endfunction()

    function(_ge_add_shaderpkg_graphics pkg_name vs_base fs_base)
        set(_OUT "${CMAKE_BINARY_DIR}/Shaders/${pkg_name}.shaderpkg")
        set(_VS  "${CMAKE_BINARY_DIR}/Shaders/${vs_base}.spv")
        set(_FS  "${CMAKE_BINARY_DIR}/Shaders/${fs_base}.spv")
        set(_PKG_DEPENDS "${_VS}" "${_FS}")
        if(_GE_SHADERREFLECT_TOOL_DEP)
            list(APPEND _PKG_DEPENDS ${_GE_SHADERREFLECT_TOOL_DEP})
        endif()
        add_custom_command(
            OUTPUT  "${_OUT}"
            COMMAND "${_GE_SHADERREFLECT_CMD}" --vs "${_VS}" --fs "${_FS}" --out "${_OUT}"
            DEPENDS ${_PKG_DEPENDS}
            COMMENT "Packaging graphics shaderpkg: ${pkg_name}"
            VERBATIM
        )
        set_property(GLOBAL APPEND PROPERTY GE_SHADERPKG_OUTPUTS "${_OUT}")
    endfunction()

    # Built-ins referenced directly in runtime code
    _ge_add_shaderpkg_graphics(encode_srgb fullscreen_noinput.vert encode_srgb.frag)
    _ge_add_shaderpkg_graphics(pixelperfect_upscale fullscreen_noinput.vert pixelperfect_upscale.frag)
    _ge_add_shaderpkg_graphics(solidcolor fullscreen_noinput.vert solidcolor.frag)
    _ge_add_shaderpkg_graphics(letterbox_overlay fullscreen_noinput.vert letterbox_overlay.frag)
    _ge_add_shaderpkg_graphics(selection_outline fullscreen_noinput.vert selection_outline.frag)
    _ge_add_shaderpkg_graphics(selection_mask selection_mask.vert selection_mask.frag)
    _ge_add_shaderpkg_graphics(selection_mask_alpha selection_mask_alpha.vert selection_mask_alpha.frag)
    # Default world debug pipeline used by RenderServices (Editor Scene/Game view)
    _ge_add_shaderpkg_graphics(world_debug world_debug.vert world_debug.frag)
    _ge_add_shaderpkg_graphics(world_depthonly world_debug.vert world_depthonly.frag)
    _ge_add_shaderpkg_graphics(world_debug_forwardplus world_debug_forwardplus.vert world_debug_forwardplus.frag)
    # Editor SceneView gizmo/grid pipelines
    _ge_add_shaderpkg_graphics(editor_lines lines.vert lines.frag)
    _ge_add_shaderpkg_graphics(markup_glow markup_glow.vert markup_glow.frag)
    # The glow over a multisampled pane: the same fragment source reading its scene depth as a
    # sampler2DMS (desktop only: never in the web cook).
    ge_compile_shader(
        OUTPUT ${SHADER_OUTPUT_DIR}/markup_glow_ms.frag.spv
        SOURCE ${SHADER_SOURCE_DIR}/markup_glow.frag
        DEFINES -DGE_MARKUP_GLOW_MS_DEPTH
        DEPENDS ${GE_SHADER_COMMON_INCLUDES}
        COMMENT "Compiling fragment shader: markup_glow_ms.frag")
    _ge_add_shaderpkg_graphics(markup_glow_ms markup_glow.vert markup_glow_ms.frag)
    # A region's display: its walls and lid from a vertex buffer, the glow's fragment stage.
    _ge_add_shaderpkg_graphics(markup_glow_mesh markup_glow_mesh.vert markup_glow.frag)
    _ge_add_shaderpkg_graphics(markup_glow_mesh_ms markup_glow_mesh.vert markup_glow_ms.frag)
    _ge_add_shaderpkg_graphics(sky_render fullscreen_noinput.vert sky_render.frag)
    _ge_add_shaderpkg_graphics(lens_flare lens_flare.vert lens_flare.frag)
    _ge_add_shaderpkg_graphics(sun_glare sun_glare.vert sun_glare.frag)
    # MSAA variant: same fragment stage, a sampler2DMS depth probe in the vertex stage.
    _ge_add_shaderpkg_graphics(sun_glare_ms sun_glare_ms.vert sun_glare.frag)
    _ge_add_shaderpkg_compute(sky_transmittance_lut sky_transmittance_lut.comp)
    _ge_add_shaderpkg_compute(sky_multiscatter_lut sky_multiscatter_lut.comp)
    _ge_add_shaderpkg_compute(sky_view_lut sky_view_lut.comp)
    _ge_add_shaderpkg_graphics(sky_stars_billboard sky_stars_billboard.vert sky_stars_billboard.frag)
    _ge_add_shaderpkg_compute(volumetric_fog_cull volumetric_fog_cull.comp)
    # Ocean. The module asks ShaderCompileService for these by name; without a
    # cooked package the request falls through to a runtime GLSL compile, which
    # only a developer desktop can serve.
    _ge_add_shaderpkg_compute(ocean_fft_init ocean_fft_spectrum_init.comp)
    _ge_add_shaderpkg_compute(ocean_fft_update ocean_fft_spectrum_update.comp)
    _ge_add_shaderpkg_compute(ocean_fft_h ocean_fft_horizontal.comp)
    _ge_add_shaderpkg_compute(ocean_fft_v ocean_fft_vertical.comp)
    _ge_add_shaderpkg_compute(ocean_combine ocean_combine.comp)
    _ge_add_shaderpkg_compute(ocean_albedo_sim ocean_albedo_sim.comp)
    _ge_add_shaderpkg_compute(ocean_wavemask_sim ocean_wavemask_sim.comp)
    _ge_add_shaderpkg_compute(ocean_clip_sim ocean_clip_sim.comp)
    _ge_add_shaderpkg_compute(ocean_flow_sim ocean_flow_sim.comp)
    _ge_add_shaderpkg_compute(ocean_foam_sim ocean_foam_sim.comp)
    _ge_add_shaderpkg_compute(ocean_dynwaves_sim ocean_dynwaves_sim.comp)
    _ge_add_shaderpkg_compute(ocean_dynwaves_query ocean_dynwaves_query.comp)
    _ge_add_shaderpkg_compute(ocean_gpu_query ocean_gpu_query.comp)
    _ge_add_shaderpkg_compute(ocean_height_query ocean_height_query.comp)
    _ge_add_shaderpkg_compute(ocean_raster_depth_convert ocean_raster_depth_convert.comp)
    _ge_add_shaderpkg_compute(ocean_seafloor_depth ocean_seafloor_depth.comp)
    _ge_add_shaderpkg_compute(ocean_shadow_sim ocean_shadow_sim.comp)
    _ge_add_shaderpkg_compute(ocean_spline_raster_depth ocean_spline_raster_depth.comp)
    _ge_add_shaderpkg_compute(ocean_spline_raster_flow ocean_spline_raster_flow.comp)
    _ge_add_shaderpkg_compute(ocean_spline_raster_clip ocean_spline_raster_clip.comp)
    _ge_add_shaderpkg_compute(ocean_spline_raster_albedo ocean_spline_raster_albedo.comp)
    _ge_add_shaderpkg_compute(ocean_spray_sim ocean_spray_gpu_sim.comp)
    _ge_add_shaderpkg_compute(ocean_spray_cull ocean_spray_gpu_cull.comp)
    _ge_add_shaderpkg_graphics(ocean_spray_draw ocean_spray_gpu.vert ocean_spray_gpu.frag)
    _ge_add_shaderpkg_graphics(ocean_underwater ocean_fullscreen.vert ocean_underwater.frag)
    _ge_add_shaderpkg_graphics(ocean_underwater_resolve ocean_fullscreen.vert ocean_resolve.frag)
    _ge_add_shaderpkg_graphics(ocean_scenegrab_resolve ocean_fullscreen.vert ocean_scene_resolve.frag)
    _ge_add_shaderpkg_graphics(ocean_reflected_caustics ocean_fullscreen.vert ocean_lightshafts.frag)
    _ge_add_shaderpkg_graphics(ocean_reflected_caustics_resolve ocean_fullscreen.vert ocean_resolve.frag)
    # The shadowed underwater pass is the same fragment source with a define, so
    # it needs its own SPIR-V before it can have its own package — the packaging
    # helpers take a compiled .spv, not compile options.
    ge_compile_shader(
        OUTPUT ${SHADER_OUTPUT_DIR}/ocean_underwater_shadowed.frag.spv
        SOURCE ${GE_OCEAN_SHADER_DIR}/ocean_underwater.frag
        DEFINES -DOCEAN_UNDERWATER_SHADOWS
        DEPENDS ${GE_SHADER_COMMON_INCLUDES}
        COMMENT "Compiling fragment shader: ocean_underwater_shadowed.frag")
    _ge_add_shaderpkg_graphics(ocean_underwater_shadowed ocean_fullscreen.vert ocean_underwater_shadowed.frag)
    _ge_add_shaderpkg_compute(volumetric_fog_media volumetric_fog_media.comp)
    _ge_add_shaderpkg_compute(volumetric_fog_light volumetric_fog_light.comp)
    # Device-coverage probe for the camera-relative sector decode (test-only).
    _ge_add_shaderpkg_compute(render_origin_decode_probe render_origin_decode_probe.comp)
    _ge_add_shaderpkg_compute(volumetric_fog_filter volumetric_fog_filter.comp)
    _ge_add_shaderpkg_compute(volumetric_fog_integrate volumetric_fog_integrate.comp)
    _ge_add_shaderpkg_compute(volumetric_fog_temporal volumetric_fog_temporal.comp)
    _ge_add_shaderpkg_graphics(volumetric_fog_composite fullscreen_noinput.vert volumetric_fog_composite.frag)
    _ge_add_shaderpkg_compute(volumetric_clouds_shape_noise volumetric_clouds_shape_noise.comp)
    _ge_add_shaderpkg_compute(volumetric_clouds_detail_noise volumetric_clouds_detail_noise.comp)
    _ge_add_shaderpkg_compute(volumetric_clouds_noise_normalize volumetric_clouds_noise_normalize.comp)
    _ge_add_shaderpkg_graphics(volumetric_clouds_march fullscreen_noinput.vert volumetric_clouds_march.frag)
    _ge_add_shaderpkg_graphics(volumetric_clouds_resolve fullscreen_noinput.vert volumetric_clouds_resolve.frag)
    _ge_add_shaderpkg_graphics(volumetric_clouds_composite fullscreen_noinput.vert volumetric_clouds_composite.frag)

    # RT shadow-mask lane (DirectionalShadowMode::RayTraced): ray-query mask pass
    # plus its plane-aware spatial denoise (plain compute, generic glob rule).
    _ge_add_shaderpkg_compute(rt_shadow_mask rt_shadow_mask.comp)
    _ge_add_shaderpkg_compute(rt_shadow_denoise rt_shadow_denoise.comp)
    # Terrain shadows: the sun-space clearance map bake (TerrainShadowBake).
    _ge_add_shaderpkg_compute(terrain_shadow_bake terrain_shadow_bake.comp)
    _ge_add_shaderpkg_compute(screen_space_shadow screen_space_shadow.comp)
    _ge_add_shaderpkg_compute(screen_space_shadow_prepare screen_space_shadow_prepare.comp)
    _ge_add_shaderpkg_compute(screen_space_shadow_resolve screen_space_shadow_resolve.comp)
    _ge_add_shaderpkg_compute(screen_space_shadow_match screen_space_shadow_match.comp)
    _ge_add_shaderpkg_compute(ddgi_classify ddgi_classify.comp)
    _ge_add_shaderpkg_compute(ddgi_classify_sw ddgi_classify_sw.comp)
    _ge_add_shaderpkg_compute(ddgi_trace_hw ddgi_trace_hw.comp)
    _ge_add_shaderpkg_compute(ddgi_trace_sw ddgi_trace_sw.comp)
    _ge_add_shaderpkg_compute(ddgi_blend ddgi_blend.comp)
    _ge_add_shaderpkg_compute(ddgi_upload ddgi_upload.comp)
    _ge_add_shaderpkg_compute(ddgi_depth_blend ddgi_depth_blend.comp)
    _ge_add_shaderpkg_compute(ddgi_depth_upload ddgi_depth_upload.comp)
    _ge_add_shaderpkg_compute(ddgi_variability ddgi_variability.comp)
    _ge_add_shaderpkg_compute(ddgi_clear ddgi_clear.comp)
    _ge_add_shaderpkg_compute(ddgi_rough_blend ddgi_rough_blend.comp)
    _ge_add_shaderpkg_compute(ddgi_rough_upload ddgi_rough_upload.comp)
    _ge_add_shaderpkg_compute(ddgi_glossy_blend ddgi_glossy_blend.comp)
    _ge_add_shaderpkg_compute(ddgi_glossy_upload ddgi_glossy_upload.comp)
    _ge_add_shaderpkg_compute(ddgi_glossy_resolve ddgi_glossy_resolve.comp)
    _ge_add_shaderpkg_compute(ddgi_glossy_resolve_blur ddgi_glossy_resolve_blur.comp)
    _ge_add_shaderpkg_compute(ddgi_map_atlas_blit ddgi_map_atlas_blit.comp)
    _ge_add_shaderpkg_compute(ddgi_skin_positions ddgi_skin_positions.comp)
    _ge_add_shaderpkg_compute(ddgi_bvh_refit ddgi_bvh_refit.comp)

    # RenderPipeline node built-ins (Forward+ scaffolding)
    _ge_add_shaderpkg_compute(clustered_light_cull clustered_light_cull.comp)
    _ge_add_shaderpkg_compute(cluster_depth_minmax cluster_depth_minmax.comp)
    _ge_add_shaderpkg_compute(shadow_minmax_reduce shadow_minmax_reduce.comp)
    _ge_add_shaderpkg_graphics(cluster_debug_overlay fullscreen_noinput.vert cluster_debug_overlay.frag)
    _ge_add_shaderpkg_graphics(forwardplus_lighting fullscreen_noinput.vert forwardplus_lighting.frag)
    _ge_add_shaderpkg_graphics(shadow_debug_overlay fullscreen_noinput.vert shadow_debug_overlay.frag)
    _ge_add_shaderpkg_graphics(tonemap fullscreen_noinput.vert tonemap.frag)
    _ge_add_shaderpkg_graphics(copy fullscreen_noinput.vert copy.frag)
    _ge_add_shaderpkg_graphics(color_filter fullscreen_noinput.vert color_filter.frag)
    _ge_add_shaderpkg_graphics(hdr_color_fx fullscreen_noinput.vert hdr_color_fx.frag)
    _ge_add_shaderpkg_graphics(cas fullscreen_noinput.vert cas.frag)
    _ge_add_shaderpkg_graphics(fast_blur_dof fullscreen_noinput.vert fast_blur_dof.frag)
    _ge_add_shaderpkg_graphics(heat_distortion fullscreen_noinput.vert heat_distortion.frag)
    _ge_add_shaderpkg_compute(sssr_classify sssr_classify.comp)
    _ge_add_shaderpkg_compute(sssr_prepare_args sssr_prepare_args.comp)
    _ge_add_shaderpkg_compute(sssr_intersect sssr_intersect.comp)
    _ge_add_shaderpkg_compute(sssr_reproject sssr_reproject.comp)
    _ge_add_shaderpkg_compute(sssr_prefilter sssr_prefilter.comp)
    _ge_add_shaderpkg_compute(sssr_temporal sssr_temporal.comp)
    _ge_add_shaderpkg_compute(sssr_composite sssr_composite.comp)
    _ge_add_shaderpkg_graphics(height_fog fullscreen_noinput.vert height_fog.frag)
    _ge_add_shaderpkg_graphics(fog_glow_prefilter fullscreen_noinput.vert fog_glow_prefilter.frag)
    _ge_add_shaderpkg_graphics(fog_glow_fast fullscreen_noinput.vert fog_glow_fast.frag)
    _ge_add_shaderpkg_graphics(fog_glow_downsample fullscreen_noinput.vert fog_glow_downsample.frag)
    _ge_add_shaderpkg_graphics(fog_glow_gather fullscreen_noinput.vert fog_glow_gather.frag)
    _ge_add_shaderpkg_graphics(fog_glow_composite fullscreen_noinput.vert fog_glow_composite.frag)
    _ge_add_shaderpkg_graphics(atmospheric_cloud_layer fullscreen_noinput.vert atmospheric_cloud_layer.frag)
    _ge_add_shaderpkg_graphics(post_fx_ldr_stack fullscreen_noinput.vert post_fx_ldr_stack.frag)
    _ge_add_shaderpkg_graphics(vhs_frame_feedback fullscreen_noinput.vert vhs_frame_feedback.frag)
    _ge_add_shaderpkg_graphics(vhs_date_burn fullscreen_noinput.vert vhs_date_burn.frag)
    _ge_add_shaderpkg_graphics(vhs_composite_signal fullscreen_noinput.vert vhs_composite_signal.frag)
    _ge_add_shaderpkg_graphics(vhs_interference fullscreen_noinput.vert vhs_interference.frag)
    _ge_add_shaderpkg_graphics(vhs_signal_glitches fullscreen_noinput.vert vhs_signal_glitches.frag)
    _ge_add_shaderpkg_graphics(vhs_rf_dropouts fullscreen_noinput.vert vhs_rf_dropouts.frag)
    _ge_add_shaderpkg_graphics(vhs_chroma_bands fullscreen_noinput.vert vhs_chroma_bands.frag)
    _ge_add_shaderpkg_graphics(vhs fullscreen_noinput.vert vhs.frag)
    _ge_add_shaderpkg_graphics(chromatic_aberration fullscreen_noinput.vert chromatic_aberration.frag)
    _ge_add_shaderpkg_graphics(film_simulation_grain fullscreen_noinput.vert film_simulation_grain.frag)
    _ge_add_shaderpkg_graphics(film_simulation_artifacts fullscreen_noinput.vert film_simulation_artifacts.frag)
    _ge_add_shaderpkg_graphics(bloom_octave_gather fullscreen_noinput.vert bloom_octave_gather.frag)
    _ge_add_shaderpkg_graphics(bloom_lens_dirt_gather fullscreen_noinput.vert bloom_lens_dirt_gather.frag)
    _ge_add_shaderpkg_graphics(bloom_lens_dirt fullscreen_noinput.vert bloom_lens_dirt.frag)
    _ge_add_shaderpkg_graphics(halation_prefilter fullscreen_noinput.vert halation_prefilter.frag)
    _ge_add_shaderpkg_graphics(halation_blur_h fullscreen_noinput.vert halation_blur_h.frag)
    _ge_add_shaderpkg_graphics(halation_composite fullscreen_noinput.vert halation_composite.frag)
    _ge_add_shaderpkg_compute(ffx_dof_prepare ffx_dof_prepare.comp)
    _ge_add_shaderpkg_compute(ffx_dof_tile ffx_dof_tile.comp)
    _ge_add_shaderpkg_compute(ffx_dof_dilate ffx_dof_dilate.comp)
    _ge_add_shaderpkg_compute(ffx_dof_blur ffx_dof_blur.comp)
    _ge_add_shaderpkg_compute(ffx_dof_composite ffx_dof_composite.comp)
    _ge_add_shaderpkg_graphics(crt fullscreen_noinput.vert crt.frag)
    _ge_add_shaderpkg_graphics(bloom_threshold fullscreen_noinput.vert bloom_threshold.frag)
    _ge_add_shaderpkg_graphics(bloom_blur_h fullscreen_noinput.vert bloom_blur_h.frag)
    _ge_add_shaderpkg_graphics(bloom_blur_v fullscreen_noinput.vert bloom_blur_v.frag)
    _ge_add_shaderpkg_graphics(bloom_downsample fullscreen_noinput.vert bloom_downsample.frag)
    _ge_add_shaderpkg_graphics(bloom_upsample fullscreen_noinput.vert bloom_upsample.frag)
    _ge_add_shaderpkg_graphics(bloom_combine fullscreen_noinput.vert bloom_combine.frag)
    _ge_add_shaderpkg_graphics(msm_write fullscreen_noinput.vert msm_write.frag)
    _ge_add_shaderpkg_graphics(msm_blur_h fullscreen_noinput.vert msm_blur_h.frag)
    _ge_add_shaderpkg_graphics(msm_blur_v fullscreen_noinput.vert msm_blur_v.frag)
    _ge_add_shaderpkg_compute(msm_cascade msm_cascade.comp)
    _ge_add_shaderpkg_compute(frustum_culling frustum_culling.comp)
    _ge_add_shaderpkg_compute(hzb_build hzb_build.comp)
    _ge_add_shaderpkg_compute(hzb_spd hzb_spd.comp)
    _ge_add_shaderpkg_compute(hzb_spd_finalize hzb_spd_finalize.comp)
    # Fused MIN+MAX variant: one depth read builds both the occlusion pyramid
    # and the SSR traversal pyramid (used when a view needs both).
    _ge_add_shaderpkg_compute(hzb_spd_minmax hzb_spd_minmax.comp)
    _ge_add_shaderpkg_compute(hzb_spd_minmax_finalize hzb_spd_minmax_finalize.comp)
    _ge_add_shaderpkg_compute(hzb_culling hzb_culling.comp)
    # Sorted transparent GPU path (transparency-scale S1/S2): the reusable GPU
    # sort primitive plus the fused runtime drain (gather + key + sort +
    # scatter — one dispatch per view, see sorted_transparent_drain.comp).
    _ge_add_shaderpkg_compute(sorted_transparent_sort sorted_transparent_sort.comp)
    _ge_add_shaderpkg_compute(sorted_transparent_drain sorted_transparent_drain.comp)
    _ge_add_shaderpkg_compute(visibility_union visibility_union.comp)
    _ge_add_shaderpkg_compute(runtime_visibility_aggregate runtime_visibility_aggregate.comp)
    _ge_add_shaderpkg_compute(draw_command_scatter draw_command_scatter.comp)
    _ge_add_shaderpkg_compute(draw_stream_group_compact draw_stream_group_compact.comp)
    _ge_add_shaderpkg_compute(draw_command_scatter_compact draw_command_scatter_compact.comp)
    _ge_add_shaderpkg_compute(draw_command_scatter_stats draw_command_scatter_stats.comp)
    _ge_add_shaderpkg_compute(draw_command_scatter_compact_stats draw_command_scatter_compact_stats.comp)
    _ge_add_shaderpkg_compute(depth_reduce depth_reduce.comp)
    _ge_add_shaderpkg_compute(depth_reduce_ms depth_reduce_ms.comp)
    _ge_add_shaderpkg_compute(depth_resolve depth_resolve.comp)
    _ge_add_shaderpkg_compute(depth_copy depth_copy.comp)
    if (BUILD_TESTING)
        _ge_add_shaderpkg_compute(gtao_sectors_probe gtao_sectors_probe.comp)
    endif()
    _ge_add_shaderpkg_compute(gtao_prepare gtao_prepare.comp)
    _ge_add_shaderpkg_compute(gtao_depth_mip gtao_depth_mip.comp)
    _ge_add_shaderpkg_compute(gtao_temporal gtao_temporal.comp)
    _ge_add_shaderpkg_compute(gtao gtao.comp)
    _ge_add_shaderpkg_compute(gtao_blur gtao_blur.comp)
    _ge_add_shaderpkg_compute(gtao_upsample gtao_upsample.comp)
    _ge_add_shaderpkg_compute(auto_exposure_histogram auto_exposure_histogram.comp)
    _ge_add_shaderpkg_compute(auto_exposure_resolve auto_exposure_resolve.comp)
    # Temporal AA (TAA P0): movers motion-vector raster + accumulate/resolve.
    # TAAU adds the upscale resolve variant (internal->display) + the display
    # depth reconstitution for overlay depth testing.
    _ge_add_shaderpkg_compute(taa_resolve taa_resolve.comp)
    _ge_add_shaderpkg_compute(taa_resolve_upscale taa_resolve_upscale.comp)
    _ge_add_shaderpkg_graphics(taa_motion_vectors taa_motion_vectors.vert taa_motion_vectors.frag)
    _ge_add_shaderpkg_compute(fxaa fxaa.comp)
    _ge_add_shaderpkg_compute(fxaa_quality fxaa_quality.comp)
    _ge_add_shaderpkg_compute(fxaa_resolve fxaa_resolve.comp)
    _ge_add_shaderpkg_compute(smaa_edges smaa_edges.comp)
    _ge_add_shaderpkg_compute(smaa_weights smaa_weights.comp)
    _ge_add_shaderpkg_compute(smaa_blend smaa_blend.comp)
    # Skinned movers: same fragment stage, dual-skinned vertex stage.
    _ge_add_shaderpkg_graphics(taa_motion_vectors_skinned taa_motion_vectors_skinned.vert taa_motion_vectors.frag)
    _ge_add_shaderpkg_graphics(taau_depth_upsample fullscreen_noinput.vert taau_depth_upsample.frag)
    _ge_add_shaderpkg_graphics(depth_copy_sample0 fullscreen_noinput.vert depth_copy_sample0.frag)
    _ge_add_shaderpkg_graphics(spatial_upscale fullscreen_noinput.vert spatial_upscale.frag)

    # Slang-lane spike sources: staged beside the packaged shaders so tests
    # (HzbSlangParityTests) and the Editor resolve them from the build root.
    set(_GE_SLANG_LANE_STAGED "")
    foreach(_slang_src hzb_build.slang ge_types.slang)
        set(_slang_out "${CMAKE_BINARY_DIR}/Shaders/Slang/${_slang_src}")
        add_custom_command(OUTPUT "${_slang_out}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SHADER_SOURCE_DIR}/Slang/${_slang_src}" "${_slang_out}"
            DEPENDS "${SHADER_SOURCE_DIR}/Slang/${_slang_src}"
            COMMENT "Staging Slang lane source: ${_slang_src}"
            VERBATIM)
        list(APPEND _GE_SLANG_LANE_STAGED "${_slang_out}")
    endforeach()

    # Finalize: single CompileShaderPkgs target gating every .shaderpkg output.
    get_property(_GE_SHADERPKG_OUTPUTS GLOBAL PROPERTY GE_SHADERPKG_OUTPUTS)
    add_custom_target(CompileShaderPkgs ALL
        DEPENDS ${_GE_SHADERPKG_OUTPUTS} ${_GE_SLANG_LANE_STAGED}
        COMMENT "Packaging shaders into .shaderpkg"
    )
    if(_GE_SHADERREFLECT_TOOL_DEP)
        add_dependencies(CompileShaderPkgs CompileShaders ShaderReflect)
    else()
        add_dependencies(CompileShaderPkgs CompileShaders)
    endif()
    unset(_GE_SHADERREFLECT_CMD)
    unset(_GE_SHADERREFLECT_TOOL_DEP)
endif()

# CI guard: ensure public Rendering headers remain Vulkan-free
add_test(
    NAME RenderingPublicHeadersNoVulkan
    COMMAND ${CMAKE_COMMAND}
        -DRENDERING_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/Tools/CheckRenderingPublicHeadersNoVulkan.cmake
)
# The check above, run on fixture trees: a name that only contains VK_ or Vk passes,
# and a Vulkan token is refused by the pattern that names it.
set(_GE_NO_VULKAN_FIXTURES ${CMAKE_CURRENT_SOURCE_DIR}/Tests/Fixtures/PublicHeadersNoVulkan)
add_test(
    NAME RenderingPublicHeadersNoVulkan.AcceptsIdentifierContainingVk
    COMMAND ${CMAKE_COMMAND}
        -DRENDERING_ROOT=${_GE_NO_VULKAN_FIXTURES}/IdentifierContainingVk
        -P ${CMAKE_CURRENT_SOURCE_DIR}/Tools/CheckRenderingPublicHeadersNoVulkan.cmake
)
add_test(
    NAME RenderingPublicHeadersNoVulkan.RefusesVulkanToken
    COMMAND ${CMAKE_COMMAND}
        -DRENDERING_ROOT=${_GE_NO_VULKAN_FIXTURES}/VulkanToken
        -P ${CMAKE_CURRENT_SOURCE_DIR}/Tools/CheckRenderingPublicHeadersNoVulkan.cmake
)
set_tests_properties(RenderingPublicHeadersNoVulkan.RefusesVulkanToken PROPERTIES
    PASS_REGULAR_EXPRESSION "Forbidden Vulkan symbol '[^']*VK_[^']*' found in[^/]*/[^']*/VulkanToken/")
unset(_GE_NO_VULKAN_FIXTURES)

# Test Device.h compilation removed - file was deleted

# Macro: give Rendering test targets the shader roots Tests/TestUtils.h resolves
# instead of probing relative paths — the source tree for tests that assert on the
# GLSL text, and (via StagedTestPaths.h) the staged mirror for tests that compose
# it. A target that composes must also depend on StageTestAssets; that target is
# defined in Tests/, added after this directory, so the dependency is wired from
# the central list in Tests/CMakeLists.txt.
macro(rendering_test_shader_paths TARGET)
    target_compile_definitions(${TARGET} PRIVATE
        RENDERING_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        RENDERING_SHADER_OUTPUT_DIR="${CMAKE_BINARY_DIR}/Shaders")
    target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Tests)
endmacro()

# Helper: splice Logger's object files into a GameEngineRendering consumer.
#
# Tests link the GameEngineRendering OBJECT library directly (not via the Engine
# target). Logger is itself an OBJECT library that GameEngineRendering links
# PUBLIC, and OBJECT->OBJECT object files are not spliced transitively to a
# downstream consumer on the VS generator, so Logger::Log::* comes up unresolved.
# Splice Logger's .obj files in directly, the same way the Engine target does
# (Engine/CMakeLists.txt).
function(rendering_test_link_logger target)
	target_sources(${target} PRIVATE $<TARGET_OBJECTS:Logger>)
endfunction()

# Helper: supply the OBJECT-library object files plus the stb_image impl that
# GameEngineRendering's consumers need but do not receive transitively.
#
# In addition to Logger (see rendering_test_link_logger), SkyRenderer.cpp pulls
# in stb_image; the implementation TU lives in the Engine target for app builds,
# so provide the private-copy impl here. Targets that already compile their own
# stb_image_impl.cpp must call rendering_test_link_logger instead to avoid an
# LNK2005 duplicate-symbol on stbi_*.
function(rendering_test_link_stb target)
	rendering_test_link_logger(${target})
	target_sources(${target} PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/stb_image_impl.cpp)
	target_compile_definitions(${target} PRIVATE GE_HAVE_STB)
	if(TARGET stb::stb)
		target_link_libraries(${target} PRIVATE stb::stb)
	elseif(STB_INCLUDE_DIRS)
		target_include_directories(${target} PRIVATE ${STB_INCLUDE_DIRS})
	endif()
endfunction()

# GPU Culling automated test suite (built when BUILD_TESTING is enabled)
if(BUILD_TESTING)

	# Parity test: legacy 4x SubmitView vs unified SubmitCascadeGroup.
	add_executable(RenderingCascadeCullingUnifiedParityTests Tests/CascadeCullingUnifiedParityTests.cpp)
	target_link_libraries(RenderingCascadeCullingUnifiedParityTests PRIVATE GameEngineRendering FileSystemObjects)
	rendering_test_link_stb(RenderingCascadeCullingUnifiedParityTests)
	rendering_test_shader_paths(RenderingCascadeCullingUnifiedParityTests)
	# CompileShaderPkgs, not CompileShaders: the parity run loads
	# frustum_culling.shaderpkg, and the .spv target does not produce one.
	add_dependencies(RenderingCascadeCullingUnifiedParityTests CompileShaderPkgs)

	# Minimal Compute Test for debugging compute shader issues
	add_executable(RenderingMinimalComputeTest Tests/MinimalComputeTest.cpp)
	target_link_libraries(RenderingMinimalComputeTest PRIVATE GameEngineRendering FileSystemObjects)
	rendering_test_link_stb(RenderingMinimalComputeTest)
	rendering_test_shader_paths(RenderingMinimalComputeTest)
	# Minimal Compute Test variant: host-visible direct read
	add_executable(RenderingMinimalComputeHostVisibleTest Tests/MinimalComputeHostVisibleTest.cpp)
	target_link_libraries(RenderingMinimalComputeHostVisibleTest PRIVATE GameEngineRendering FileSystemObjects)
	rendering_test_link_stb(RenderingMinimalComputeHostVisibleTest)
	rendering_test_shader_paths(RenderingMinimalComputeHostVisibleTest)
endif()
# Try to find GoogleTest (test configures only); if not available, fetch it.
# Ungated, this find + network fetch ran in every configure and its
# GTest_FOUND kept the GTest_FOUND-gated suites below alive with
# BUILD_TESTING=OFF.
if(BUILD_TESTING)
    find_package(GTest CONFIG QUIET)
    if(NOT GTest_FOUND)
        message(STATUS "GoogleTest not found, fetching via FetchContent...")
        FetchContent_Declare(
            googletest
            GIT_REPOSITORY https://github.com/google/googletest.git
            GIT_TAG        v1.14.0
            GIT_SHALLOW    TRUE
        )
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(googletest)
        # Create imported target aliases for consistency if not already defined
        if(NOT TARGET GTest::gtest)
            add_library(GTest::gtest ALIAS gtest)
        endif()
        if(NOT TARGET GTest::gtest_main)
            add_library(GTest::gtest_main ALIAS gtest_main)
        endif()
        set(GTest_FOUND TRUE)
    endif()
endif()

# Matrix Conversion Validation test suite
if(BUILD_TESTING)
find_package(GTest CONFIG QUIET)
# If GTest was brought in via FetchContent, ensure GTest_FOUND reflects it
if(NOT GTest_FOUND AND TARGET GTest::gtest)
    set(GTest_FOUND TRUE)
endif()
if(GTest_FOUND OR TARGET GTest::gtest)
    add_executable(RenderingMatrixConversionValidationTests Tests/MatrixConversionValidationTests.cpp)
    target_link_libraries(RenderingMatrixConversionValidationTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    rendering_test_shader_paths(RenderingMatrixConversionValidationTests)

    add_executable(ShaderGraphCompilerTests Tests/ShaderGraphCompilerTests.cpp)
    target_link_libraries(ShaderGraphCompilerTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    # rendering_test_link_stb splices Logger's objects in; do not also link Logger
    # directly or its .obj files get pulled in twice (LNK2005).
    rendering_test_link_stb(ShaderGraphCompilerTests)
    rendering_test_shader_paths(ShaderGraphCompilerTests)
    # Its shader-graph node staging follows the output-layout sweep at the end of
    # this file: the staging is keyed by the directory the executable lands in.
    gtest_discover_tests(ShaderGraphCompilerTests)

    add_executable(RenderingReverseZMathTests Tests/ReverseZMathTests.cpp)
    set_target_properties(RenderingReverseZMathTests PROPERTIES FOLDER "Rendering/Tests")
    # Engine/Include carries the header-only reverse-Z camera math the suite
    # exercises (Engine/Rendering/CameraUtils.h); GameEngineRendering does not
    # export that directory.
    target_include_directories(RenderingReverseZMathTests PRIVATE
        ${CMAKE_SOURCE_DIR}/Engine/Include
    )
    target_link_libraries(RenderingReverseZMathTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    rendering_test_shader_paths(RenderingReverseZMathTests)
    add_test(NAME RenderingReverseZMathTests COMMAND RenderingReverseZMathTests)

    # The sun's path from latitude and day of year (Rendering/Sky/SolarPath.h), and the sky's
    # sun and moon placement built on it: pure CPU maths, no device and no shaders.
    add_executable(RenderingSolarPathTests Tests/SolarPathTests.cpp)
    set_target_properties(RenderingSolarPathTests PROPERTIES FOLDER "Rendering/Tests")
    target_link_libraries(RenderingSolarPathTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    add_test(NAME RenderingSolarPathTests COMMAND RenderingSolarPathTests)

    add_executable(RenderingTextureFormatTablesTests Tests/TextureFormatTablesTests.cpp)
    set_target_properties(RenderingTextureFormatTablesTests PROPERTIES FOLDER "Rendering/Tests")
    target_link_libraries(RenderingTextureFormatTablesTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    rendering_test_shader_paths(RenderingTextureFormatTablesTests)
    add_test(NAME RenderingTextureFormatTablesTests COMMAND RenderingTextureFormatTablesTests)

    # Global resource managers (Texture/Buffer/Pipeline/Sampler): handle lifecycle
    # and the value-return contract that keeps their GenerationalVector storage
    # from escaping the accessor's lock. Header-only containers, so no device and
    # no shader paths — just the module for the headers.
    add_executable(ResourceHandleManagerTests Tests/ResourceHandleManagerTests.cpp)
    set_target_properties(ResourceHandleManagerTests PROPERTIES FOLDER "Rendering/Tests")
    target_link_libraries(ResourceHandleManagerTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    add_test(NAME ResourceHandleManagerTests COMMAND ResourceHandleManagerTests)

    # ── RenderGraph (render-graph v2) isolation tests ───────────────────────────────
    # Shared test-support pass-name queries (TestSupport/RGPassQuery.h): the
    # exact/subtree/family matchers every suite that asserts on render-graph
    # pass names goes through. INTERFACE = include dir only — consumers already
    # link the right engine flavour (Engine.dll import lib OR the static
    # GameEngineRendering), and linking GameEngineRendering from here would
    # double-define its symbols inside Engine-linked test exes.
    add_library(RenderGraphTestSupport INTERFACE)
    target_include_directories(RenderGraphTestSupport INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/TestSupport)

    # The new immediate-mode core is compiled directly into the test exe (not yet
    # into GameEngineRendering — that is Stage 2b). Device-touching RenderGraph code binds
    # to the engine's REAL IDevice/TextureDesc/handles, so the tests link
    # GameEngineRendering and create a real headless device (no mock, no
    # production disconnect). Pure-algorithm tests (cull/schedule/barriers) need no
    # device. Add RenderGraph sources/tests here as the module grows.
    add_executable(RenderGraphTests
        Tests/RenderGraph/RGArenaTests.cpp
        Tests/RenderGraph/RGGraphTests.cpp
        Tests/RenderGraph/RGScheduleTests.cpp
        Tests/RenderGraph/RGPoolTests.cpp
        Tests/RenderGraph/RGUploadRingTests.cpp
        Tests/RenderGraph/RGAllocTests.cpp
        Tests/RenderGraph/RGBarrierTests.cpp
        Tests/RenderGraph/RGSubresourceTests.cpp
        Tests/RenderGraph/RGSubmissionTests.cpp
        Tests/RenderGraph/RGFrameTests.cpp
        Tests/RenderGraph/RGFrameDeviceRebuildTests.cpp
        Tests/RenderGraph/RGRecordTests.cpp
        Tests/RenderGraph/QueueOwnershipTransferExemptionTests.cpp
        Tests/RenderGraph/RGReadbackRingTests.cpp
        Tests/RenderGraph/RGContextTests.cpp
        Tests/RenderGraph/RGProfilingTests.cpp
        Tests/RenderGraph/RGGpuDrivenCoreTests.cpp
        Tests/RenderGraph/RGPassQueryTests.cpp
        Tests/RenderGraph/RGCopyPipelineTests.cpp
        Tests/RenderGraph/ShaderLoaderFallbackTests.cpp
    )
    # UNITY_BUILD OFF: each RenderGraph test .cpp defines same-named anonymous-namespace
    # helpers (Buf/Tex/Bake/ColorDesc/FramePools) that ODR-collide when unity-concatenated.
    set_target_properties(RenderGraphTests PROPERTIES FOLDER "Rendering/Tests/RenderGraph" UNITY_BUILD OFF)
    # Include dir (for "Rendering/Core/RenderGraph/..") + module root (for "Tests/RenderGraph/.." test headers).
    target_include_directories(RenderGraphTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Include ${CMAKE_CURRENT_SOURCE_DIR})
    if(RENDERING_HAS_VULKAN)
        # The retirement regression observes the real backend's timeline wait.
        target_include_directories(RenderGraphTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source ${VMA_INCLUDE_DIRS})
    endif()
    target_link_libraries(RenderGraphTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        RenderGraphTestSupport
        GTest::gtest
        GTest::gtest_main
    )
    if(MSVC)
        # Silence C6326 (/analyze) "constant compared with constant" noise emitted
        # by GTest EXPECT_EQ on literal expectations.
        target_compile_options(RenderGraphTests PRIVATE /wd6326)
    endif()
    rendering_test_shader_paths(RenderGraphTests)
    # CompileShaderPkgs, not CompileShaders: the GPU-driven suites load a
    # .shaderpkg, and the .spv target GameEngineRendering already pulls in does
    # not produce one.
    add_dependencies(RenderGraphTests CompileShaderPkgs)
    add_test(NAME RenderGraphTests COMMAND RenderGraphTests)

    # Queue-family selection policy: pure table tests, no device required.
    add_executable(VulkanQueueFamilySelectionTests Tests/VulkanQueueFamilySelectionTests.cpp)
    set_target_properties(VulkanQueueFamilySelectionTests PROPERTIES FOLDER "Rendering/Tests")
    target_link_libraries(VulkanQueueFamilySelectionTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    if(MSVC)
        # Silence C6326 (/analyze) "constant compared with constant" noise emitted
        # by GTest EXPECT_EQ on literal expectations.
        target_compile_options(VulkanQueueFamilySelectionTests PRIVATE /wd6326)
    endif()
    target_include_directories(VulkanQueueFamilySelectionTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME VulkanQueueFamilySelectionTests COMMAND VulkanQueueFamilySelectionTests)

    # Per-queue submit chokepoint: timeline-value allocation under concurrent submits. Pure
    # structural assertions on the values allocated, so no device is required — and none would
    # help, since validation cannot see a duplicated timeline signal.
    add_executable(VulkanQueueSubmitContextTests Tests/VulkanQueueSubmitContextTests.cpp)
    set_target_properties(VulkanQueueSubmitContextTests PROPERTIES FOLDER "Rendering/Tests")
    target_link_libraries(VulkanQueueSubmitContextTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    if(MSVC)
        # Silence C6326 (/analyze) "constant compared with constant" noise emitted
        # by GTest EXPECT_EQ on literal expectations.
        target_compile_options(VulkanQueueSubmitContextTests PRIVATE /wd6326)
    endif()
    target_include_directories(VulkanQueueSubmitContextTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME VulkanQueueSubmitContextTests COMMAND VulkanQueueSubmitContextTests)

    # Buffer residency: the memory-class -> VMA policy table, plus the resolved
    # memory type on the running device.
    add_executable(BufferResidencyTests Tests/BufferResidencyTests.cpp)
    set_target_properties(BufferResidencyTests PROPERTIES FOLDER "Rendering/Tests")
    target_link_libraries(BufferResidencyTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    if(MSVC)
        # Silence C6326 (/analyze) "constant compared with constant" noise emitted
        # by GTest EXPECT_EQ on literal expectations.
        target_compile_options(BufferResidencyTests PRIVATE /wd6326)
    endif()
    target_include_directories(BufferResidencyTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME BufferResidencyTests COMMAND BufferResidencyTests)

    # Device memory topology: the heap-walk derivation, plus the capability a
    # running device reports and where one buffer actually landed.
    add_executable(DeviceMemoryTopologyTests Tests/DeviceMemoryTopologyTests.cpp)
    set_target_properties(DeviceMemoryTopologyTests PROPERTIES FOLDER "Rendering/Tests")
    target_link_libraries(DeviceMemoryTopologyTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    if(MSVC)
        # Silence C6326 (/analyze) "constant compared with constant" noise emitted
        # by GTest EXPECT_EQ on literal expectations.
        target_compile_options(DeviceMemoryTopologyTests PRIVATE /wd6326)
    endif()
    target_include_directories(DeviceMemoryTopologyTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME DeviceMemoryTopologyTests COMMAND DeviceMemoryTopologyTests)

    # QueryPool Performance Tests
    # Device diagnostics tests: counters + ownership suppression smoke
    add_executable(DeviceDiagnosticsTests Tests/DeviceDiagnosticsTests.cpp)
    set_target_properties(DeviceDiagnosticsTests PROPERTIES FOLDER "Rendering/Tests")
    target_link_libraries(DeviceDiagnosticsTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    rendering_test_shader_paths(DeviceDiagnosticsTests)
    target_include_directories(DeviceDiagnosticsTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_dependencies(DeviceDiagnosticsTests CompileShaders)
    add_test(NAME DeviceDiagnosticsTests COMMAND DeviceDiagnosticsTests)

    # Device lifecycle tests: repeated headless create/destroy must not grow the
    # process (threads or private bytes).
    add_executable(DeviceLifecycleTests Tests/DeviceLifecycleTests.cpp)
    set_target_properties(DeviceLifecycleTests PROPERTIES FOLDER "Rendering/Tests")
    target_link_libraries(DeviceLifecycleTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    rendering_test_shader_paths(DeviceLifecycleTests)
    target_include_directories(DeviceLifecycleTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_dependencies(DeviceLifecycleTests CompileShaders)
    add_test(NAME DeviceLifecycleTests COMMAND DeviceLifecycleTests)

    # ACES 2 tier-table storage-class guard: pipeline-creation-only statistics
    # via VK_KHR_pipeline_executable_properties (no draw, no submission). Pins
    # that the tier tables stay in the Aces2Tables SSBO instead of being
    # materialized into per-invocation scratch memory (design doc B4).
    add_executable(Aces2PipelineStatsTests Tests/Aces2PipelineStatsTests.cpp)
    set_target_properties(Aces2PipelineStatsTests PROPERTIES FOLDER "Rendering/Tests")
    target_link_libraries(Aces2PipelineStatsTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    rendering_test_shader_paths(Aces2PipelineStatsTests)
    target_include_directories(Aces2PipelineStatsTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_dependencies(Aces2PipelineStatsTests CompileShaders)
    add_test(NAME Aces2PipelineStatsTests COMMAND Aces2PipelineStatsTests)

    # Deferred image-view destruction retires against the frame that references
    # the view, not the one before it. Own executable so the process-wide
    # VkInstance is created WITH the validation layer (the VUID is layer-reported).
    add_executable(DeferredViewDestroyTimelineTests Tests/DeferredViewDestroyTimelineTests.cpp)
    set_target_properties(DeferredViewDestroyTimelineTests PROPERTIES FOLDER "Rendering/Tests")
    target_link_libraries(DeferredViewDestroyTimelineTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    rendering_test_shader_paths(DeferredViewDestroyTimelineTests)
    target_include_directories(DeferredViewDestroyTimelineTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_dependencies(DeferredViewDestroyTimelineTests CompileShaders)
    add_test(NAME DeferredViewDestroyTimelineTests COMMAND DeferredViewDestroyTimelineTests)

    # A2.4-D5: per-thread command-pool free/used lists are level-segregated so a
    # retired secondary is never handed out as a primary. Device-gated (skips
    # without a Vulkan device).
    add_executable(CommandPoolLevelSegregationTests Tests/CommandPoolLevelSegregationTests.cpp)
    set_target_properties(CommandPoolLevelSegregationTests PROPERTIES FOLDER "Rendering/Tests")
    target_link_libraries(CommandPoolLevelSegregationTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    rendering_test_shader_paths(CommandPoolLevelSegregationTests)
    target_include_directories(CommandPoolLevelSegregationTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_dependencies(CommandPoolLevelSegregationTests CompileShaders)
    add_test(NAME CommandPoolLevelSegregationTests COMMAND CommandPoolLevelSegregationTests)

    # The per-thread command-pool cache is process-wide, so two devices alive at
    # once on one thread must still get separate pool entries. Device-gated
    # (skips without a Vulkan device).
    add_executable(ThreadCommandPoolDeviceIdentityTests Tests/ThreadCommandPoolDeviceIdentityTests.cpp)
    set_target_properties(ThreadCommandPoolDeviceIdentityTests PROPERTIES FOLDER "Rendering/Tests")
    target_link_libraries(ThreadCommandPoolDeviceIdentityTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    rendering_test_shader_paths(ThreadCommandPoolDeviceIdentityTests)
    target_include_directories(ThreadCommandPoolDeviceIdentityTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_dependencies(ThreadCommandPoolDeviceIdentityTests CompileShaders)
    add_test(NAME ThreadCommandPoolDeviceIdentityTests COMMAND ThreadCommandPoolDeviceIdentityTests)

    # GPU resource lifetime: destruction is always deferred outside teardown.
    # Device-gated — skips when no Vulkan device can be created.
    add_executable(DeferredDestructionTests Tests/DeferredDestructionTimelineTests.cpp)
    set_target_properties(DeferredDestructionTests PROPERTIES FOLDER "Rendering/Tests")
    target_link_libraries(DeferredDestructionTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    rendering_test_shader_paths(DeferredDestructionTests)
    target_include_directories(DeferredDestructionTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_dependencies(DeferredDestructionTests CompileShaders)
    add_test(NAME DeferredDestructionTests COMMAND DeferredDestructionTests)

    # Texture-view creation is concurrent by contract (ECS extraction workers
    # register bindless textures). Device-gated — skips without a Vulkan device.
    add_executable(TextureViewThreadingTests Tests/TextureViewThreadingTests.cpp)
    set_target_properties(TextureViewThreadingTests PROPERTIES FOLDER "Rendering/Tests")
    target_link_libraries(TextureViewThreadingTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    rendering_test_shader_paths(TextureViewThreadingTests)
    target_include_directories(TextureViewThreadingTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_dependencies(TextureViewThreadingTests CompileShaders)
    add_test(NAME TextureViewThreadingTests COMMAND TextureViewThreadingTests)

    # Q6 device-lost recovery: health state-machine unit tests (no GPU) plus
    # device-gated fault-injection / fence-poll tests (skip without a device).
    add_executable(DeviceRecoveryTests Tests/DeviceRecoveryTests.cpp)
    set_target_properties(DeviceRecoveryTests PROPERTIES FOLDER "Rendering/Tests")
    target_link_libraries(DeviceRecoveryTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    rendering_test_shader_paths(DeviceRecoveryTests)
    target_include_directories(DeviceRecoveryTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # The device-loss routing lint proves "no unrouted VK_ERROR_DEVICE_LOST site
    # exists", which requires reading the backend source rather than running it.
    # Developer lint only — nothing staged or shipped resolves a path this way.
    target_compile_definitions(DeviceRecoveryTests PRIVATE
        GE_VULKAN_BACKEND_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Source/Vulkan")
    add_dependencies(DeviceRecoveryTests CompileShaders)
    add_test(NAME DeviceRecoveryTests COMMAND DeviceRecoveryTests)

    # Per-device-generation VkSemaphore parity across an in-place device rebuild.
    # Needs a real window target (the rebuild's swapchain-recreation step is a
    # no-op without one), so it creates a hidden GLFW window like the HDR sibling
    # and skips without a device.
    add_executable(RenderingDeviceRebuildSemaphoreLedgerTests Tests/DeviceRebuildSemaphoreLedgerTests.cpp)
    set_target_properties(RenderingDeviceRebuildSemaphoreLedgerTests PROPERTIES FOLDER "Rendering/Tests")
    target_link_libraries(RenderingDeviceRebuildSemaphoreLedgerTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    target_compile_definitions(RenderingDeviceRebuildSemaphoreLedgerTests PRIVATE HAVE_GLFW=1)
    rendering_test_shader_paths(RenderingDeviceRebuildSemaphoreLedgerTests)
    target_include_directories(RenderingDeviceRebuildSemaphoreLedgerTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_dependencies(RenderingDeviceRebuildSemaphoreLedgerTests CompileShaders)
    add_test(NAME RenderingDeviceRebuildSemaphoreLedgerTests COMMAND RenderingDeviceRebuildSemaphoreLedgerTests)

    # Window-target ownership of the HDR request: a new target inherits the
    # active target's request, and per-target state survives target switches.
    # Needs real surfaces, so it creates hidden GLFW windows (glfw arrives
    # transitively from GameEngineRendering) and skips without a device.
    add_executable(RenderingWindowTargetHdrTests Tests/WindowTargetHdrRequestTests.cpp)
    set_target_properties(RenderingWindowTargetHdrTests PROPERTIES FOLDER "Rendering/Tests")
    target_link_libraries(RenderingWindowTargetHdrTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    target_compile_definitions(RenderingWindowTargetHdrTests PRIVATE HAVE_GLFW=1)
    rendering_test_shader_paths(RenderingWindowTargetHdrTests)
    target_include_directories(RenderingWindowTargetHdrTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_dependencies(RenderingWindowTargetHdrTests CompileShaders)
    add_test(NAME RenderingWindowTargetHdrTests COMMAND RenderingWindowTargetHdrTests)

    # P5: per-target swapchain-format query + Presented-arm consumption. Two
    # hidden GLFW windows diverge their SDR formats via the ten-bit preference
    # lever; the encode pass is then declared under the WRONG active target
    # with the right format supplied, so the run proves the parameter beats the
    # ambient read (and the control arm pins the ambient coupling it severs).
    # Loud SKIP when the agent cannot diverge the two formats. Needs staged
    # shaderpkgs for the encode pass.
    add_executable(RenderingWindowTargetPresentedFormatTests Tests/WindowTargetPresentedFormatTests.cpp)
    set_target_properties(RenderingWindowTargetPresentedFormatTests PROPERTIES FOLDER "Rendering/Tests")
    target_link_libraries(RenderingWindowTargetPresentedFormatTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    target_compile_definitions(RenderingWindowTargetPresentedFormatTests PRIVATE HAVE_GLFW=1)
    rendering_test_link_stb(RenderingWindowTargetPresentedFormatTests)
    rendering_test_shader_paths(RenderingWindowTargetPresentedFormatTests)
    target_include_directories(RenderingWindowTargetPresentedFormatTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_dependencies(RenderingWindowTargetPresentedFormatTests CompileShaderPkgs)
    add_test(NAME RenderingWindowTargetPresentedFormatTests COMMAND RenderingWindowTargetPresentedFormatTests)

    # The one-dither-owner invariant (landing 4). Three witnesses: the real
    # tonemap.frag declared twice into an `_SRGB` attachment differing only in
    # outEncoding (the pixel witness — no test could see arm 0 firing before
    # this); the terminal Finalize still dithering on the same workload; and the
    # SDR tonemap arm read against two live window targets driven by the same
    # ten-bit preference lever GE_FORCE_8BIT_SRGB_SWAPCHAIN pulls, verified in
    # both directions with a loud SKIP when the agent cannot diverge them.
    # Needs staged shaderpkgs (tonemap + encode_srgb).
    add_executable(TonemapDitherOwnerTests Tests/TonemapDitherOwnerTests.cpp)
    set_target_properties(TonemapDitherOwnerTests PROPERTIES FOLDER "Rendering/Tests")
    target_link_libraries(TonemapDitherOwnerTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    target_compile_definitions(TonemapDitherOwnerTests PRIVATE HAVE_GLFW=1)
    rendering_test_link_stb(TonemapDitherOwnerTests)
    rendering_test_shader_paths(TonemapDitherOwnerTests)
    target_include_directories(TonemapDitherOwnerTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_dependencies(TonemapDitherOwnerTests CompileShaderPkgs)
    add_test(NAME TonemapDitherOwnerTests COMMAND TonemapDitherOwnerTests)

    # Descriptor buffer tests: VK_EXT_descriptor_buffer allocator + layout bucket.
    # Runtime-gated — skip when the device or env flag are not available.
    add_executable(DescriptorBufferTests Tests/DescriptorBufferTests.cpp)
    set_target_properties(DescriptorBufferTests PROPERTIES FOLDER "Rendering/Tests")
    # Same rationale as ShaderReflect above: the rendering unity build pulls
    # SkyRenderer.cpp's LoadSkyTexture2D into GameEngineRendering.lib, which
    # references stbi_* symbols defined only in Engine.lib's stb_image_impl.cpp.
    # Tests that link GameEngineRendering but not Engine must compile a private
    # copy to resolve the symbols.
    target_sources(DescriptorBufferTests PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/stb_image_impl.cpp)
    target_compile_definitions(DescriptorBufferTests PRIVATE GE_HAVE_STB)
    if(TARGET stb::stb)
        target_link_libraries(DescriptorBufferTests PRIVATE stb::stb)
    elseif(STB_INCLUDE_DIRS)
        target_include_directories(DescriptorBufferTests PRIVATE ${STB_INCLUDE_DIRS})
    endif()
    target_link_libraries(DescriptorBufferTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    # Already compiles stb_image_impl.cpp above; only the Logger object splice is needed.
    rendering_test_link_logger(DescriptorBufferTests)
    rendering_test_shader_paths(DescriptorBufferTests)
    target_include_directories(DescriptorBufferTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME DescriptorBufferTests COMMAND DescriptorBufferTests)

    # Finalize `_SRGB`-destination round-trip gate (#767 P6b measurement 5):
    # renders a 256-level ENCODED ramp through the real AddSRGBEncodePassRG's
    # encoded-input arm into an `_SRGB` target and asserts the hardware ROP
    # re-encode reproduces every level byte-exact. Runtime-gated — skips
    # without a Vulkan device. A failure is a KILL condition for that arm, not
    # a tolerance to widen.
    add_executable(SrgbEncodeRampRoundTripTests Tests/SrgbEncodeRampRoundTripTests.cpp)
    set_target_properties(SrgbEncodeRampRoundTripTests PROPERTIES FOLDER "Rendering/Tests")
    target_link_libraries(SrgbEncodeRampRoundTripTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    rendering_test_link_stb(SrgbEncodeRampRoundTripTests)
    rendering_test_shader_paths(SrgbEncodeRampRoundTripTests)
    target_include_directories(SrgbEncodeRampRoundTripTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # CompileShaderPkgs, not CompileShaders: these suites load a .shaderpkg, and
    # the .spv target does not produce one. CompileShaderPkgs is an ALL target,
    # so a full build hides the difference — building this test alone does not,
    # and the pass then declares nothing and the suite fails on a staging error
    # that reads like a code failure.
    add_dependencies(SrgbEncodeRampRoundTripTests CompileShaderPkgs)
    add_test(NAME SrgbEncodeRampRoundTripTests COMMAND SrgbEncodeRampRoundTripTests)

    # Terminal sRGB encode dither guard: renders a synthetic gradient through
    # the real AddSRGBEncodePassRG offscreen and asserts run-length/deviation
    # statistics of the quantized output. Runtime-gated — skips without a
    # Vulkan device. Guards the class where the dither is silently dead
    # (broken TPDF, zeroed/dropped ditherLsb push constant) while all
    # plumbing logs stay healthy.
    add_executable(SrgbEncodeDitherTests Tests/SrgbEncodeDitherTests.cpp)
    set_target_properties(SrgbEncodeDitherTests PROPERTIES FOLDER "Rendering/Tests")
    target_link_libraries(SrgbEncodeDitherTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    # stb impl + Logger objects: same non-transitive OBJECT-library rationale
    # as DescriptorBufferTests above.
    rendering_test_link_stb(SrgbEncodeDitherTests)
    rendering_test_shader_paths(SrgbEncodeDitherTests)
    target_include_directories(SrgbEncodeDitherTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_dependencies(SrgbEncodeDitherTests CompileShaderPkgs)
    add_test(NAME SrgbEncodeDitherTests COMMAND SrgbEncodeDitherTests)

    # Temporal dither phase guard: encodes the dither guard's ramp across a
    # RANGE of movie ordinals and asserts every realisation keeps the dither's
    # statistics while no two realisations match, plus that both "off" arms
    # (toggle off, quantizer None) are byte-identical to the static pattern.
    # Own process for the same latch reason as the sibling encode suites.
    # Runtime-gated — skips without a Vulkan device.
    add_executable(SrgbEncodeTemporalDitherTests Tests/SrgbEncodeTemporalDitherTests.cpp)
    set_target_properties(SrgbEncodeTemporalDitherTests PROPERTIES FOLDER "Rendering/Tests")
    target_link_libraries(SrgbEncodeTemporalDitherTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    rendering_test_link_stb(SrgbEncodeTemporalDitherTests)
    rendering_test_shader_paths(SrgbEncodeTemporalDitherTests)
    target_include_directories(SrgbEncodeTemporalDitherTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_dependencies(SrgbEncodeTemporalDitherTests CompileShaderPkgs)
    add_test(NAME SrgbEncodeTemporalDitherTests COMMAND SrgbEncodeTemporalDitherTests)

    # Terminal-encode deband guard: renders the measured band class (multi-LSB
    # staircase the dither cannot break) plus a hard edge and a flat field
    # through the real AddSRGBEncodePassRG and asserts the staircase collapses
    # while edge contrast/sharpness and flat fields are preserved. Own process
    # so its environment pins don't collide with the dither guard's
    # (deband/dither settings latch once per process). Runtime-gated — skips
    # without a Vulkan device.
    add_executable(SrgbEncodeDebandTests Tests/SrgbEncodeDebandTests.cpp)
    set_target_properties(SrgbEncodeDebandTests PROPERTIES FOLDER "Rendering/Tests")
    target_link_libraries(SrgbEncodeDebandTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    rendering_test_link_stb(SrgbEncodeDebandTests)
    rendering_test_shader_paths(SrgbEncodeDebandTests)
    target_include_directories(SrgbEncodeDebandTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_dependencies(SrgbEncodeDebandTests CompileShaderPkgs)
    add_test(NAME SrgbEncodeDebandTests COMMAND SrgbEncodeDebandTests)

    # FinalizeQuantizer contract guard: pins the two non-default quantizers the
    # Player's SDR HUD chain is built on (None = the byte-exact terminal
    # transfer, Presented = filters sized past a float destination). Own process
    # so it can pin BOTH filters ON while asserting None defeats them
    # (deband/dither settings latch once per process). Runtime-gated — skips
    # without a Vulkan device.
    add_executable(SrgbEncodeQuantizerTests Tests/SrgbEncodeQuantizerTests.cpp)
    set_target_properties(SrgbEncodeQuantizerTests PROPERTIES FOLDER "Rendering/Tests")
    target_link_libraries(SrgbEncodeQuantizerTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    rendering_test_link_stb(SrgbEncodeQuantizerTests)
    rendering_test_shader_paths(SrgbEncodeQuantizerTests)
    target_include_directories(SrgbEncodeQuantizerTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_dependencies(SrgbEncodeQuantizerTests CompileShaderPkgs)
    add_test(NAME SrgbEncodeQuantizerTests COMMAND SrgbEncodeQuantizerTests)

    add_executable(RenderingQueryPoolPerformanceTests Tests/QueryPoolPerformanceTest.cpp)
    target_link_libraries(RenderingQueryPoolPerformanceTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    rendering_test_shader_paths(RenderingQueryPoolPerformanceTests)

    # New: ResourceBarrier and subresource tests
    add_executable(RenderingResourceBarrierTests
        Tests/ResourceBarrierTests.cpp
    )
    target_link_libraries(RenderingResourceBarrierTests PRIVATE
        GameEngineRendering
        FileSystemObjects
        GTest::gtest
        GTest::gtest_main
    )
    rendering_test_shader_paths(RenderingResourceBarrierTests)

else()
    message(WARNING "Google Test not found - Matrix validation tests will not be built")
endif()

endif()
# Add Vulkan headers for GLFW
if(RENDERING_HAS_VULKAN)
    if (TARGET RenderingMultipleTrianglesExample)
        target_include_directories(RenderingMultipleTrianglesExample PRIVATE ${Vulkan_INCLUDE_DIRS})
    endif()
    if (TARGET RenderingCascadedShadowExample)
        target_include_directories(RenderingCascadedShadowExample PRIVATE ${Vulkan_INCLUDE_DIRS})
    endif()
    if (RENDERING_ENABLE_GPU_CULLING AND TARGET RenderingVisualCullingExample)
        target_include_directories(RenderingVisualCullingExample PRIVATE ${Vulkan_INCLUDE_DIRS})
    endif()

    # Add VMA headers for examples that need them
    if(VMA_FOUND)
        if (TARGET RenderingMultipleTrianglesExample)
            target_include_directories(RenderingMultipleTrianglesExample PRIVATE ${VMA_INCLUDE_DIRS})
        endif()
        if (TARGET RenderingCascadedShadowExample)
            target_include_directories(RenderingCascadedShadowExample PRIVATE ${VMA_INCLUDE_DIRS})
        endif()
        if (RENDERING_ENABLE_GPU_CULLING AND TARGET RenderingVisualCullingExample)
            target_include_directories(RenderingVisualCullingExample PRIVATE ${VMA_INCLUDE_DIRS})
        endif()
    endif()
endif()

if (TARGET RenderingMultipleTrianglesExample)
    set_target_properties(RenderingMultipleTrianglesExample PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Examples"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/Examples"
    )
endif()

if (TARGET RenderingBindlessTextureExample)
    set_target_properties(RenderingBindlessTextureExample PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Examples"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/Examples"
    )
endif()

if (TARGET RenderingBindlessBufferExample)
    set_target_properties(RenderingBindlessBufferExample PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Examples"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/Examples"
    )
endif()

if (TARGET RenderingIntegratedBindlessExample)
    set_target_properties(RenderingIntegratedBindlessExample PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Examples"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/Examples"
    )
endif()

if (RENDERING_ENABLE_GPU_CULLING)
    if (TARGET RenderingGPUSceneExample)
        set_target_properties(RenderingGPUSceneExample PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            FOLDER "Rendering/Examples"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/Examples"
        )
    endif()

    if (TARGET RenderingGPUCullingExample)
        set_target_properties(RenderingGPUCullingExample PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            FOLDER "Rendering/Examples"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/Examples"
        )
    endif()

    if (TARGET RenderingVisualCullingExample)
        set_target_properties(RenderingVisualCullingExample PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            FOLDER "Rendering/Examples"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/Examples"
        )
    endif()
endif()

if(BUILD_TESTING)
if(RENDERING_HAS_VULKAN)
    target_include_directories(RenderingMinimalComputeHostVisibleTest PRIVATE
        ${Vulkan_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/Vulkan)
endif()
endif()

# Allow tests to include backend-specific headers when needed
if(BUILD_TESTING)
if(RENDERING_HAS_VULKAN)
    target_include_directories(RenderingMinimalComputeTest PRIVATE
        ${Vulkan_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/Vulkan)
endif()
endif()

if(BUILD_TESTING)
set_target_properties(RenderingMinimalComputeHostVisibleTest PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    FOLDER "Rendering/Tests"
)
endif()

if(BUILD_TESTING)
set_target_properties(RenderingMinimalComputeTest PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    FOLDER "Rendering/Tests"
)
endif()

if(BUILD_TESTING AND GTest_FOUND)
    set_target_properties(RenderingMatrixConversionValidationTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests"
    )

    set_target_properties(RenderingQueryPoolPerformanceTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests"
    )

if(BUILD_TESTING AND GTest_FOUND)
    add_executable(PipelineCacheTest Tests/PipelineCacheTest.cpp)
    target_link_libraries(PipelineCacheTest PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    target_include_directories(PipelineCacheTest PRIVATE ${CMAKE_SOURCE_DIR}/Tests) # TestTempDir.h
    rendering_test_shader_paths(PipelineCacheTest)
    set_target_properties(PipelineCacheTest PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests"
    )
endif()

if(BUILD_TESTING AND GTest_FOUND)
    add_test(NAME PipelineCacheTest COMMAND $<TARGET_FILE:PipelineCacheTest>)
endif()

endif()

# The following validation tests require GTest; only add when GTest is found
if(GTest_FOUND)

        # Descriptor set allocator stress tests
        add_executable(DescriptorSetAllocatorStressTests Tests/DescriptorSetAllocatorStressTests.cpp)
        target_link_libraries(DescriptorSetAllocatorStressTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
        rendering_test_shader_paths(DescriptorSetAllocatorStressTests)
        add_test(NAME DescriptorSetAllocatorStressTests COMMAND $<TARGET_FILE:DescriptorSetAllocatorStressTests>)

        # Compute pipeline reflection test
        add_executable(ComputePipelineReflectionTests Tests/ComputePipelineReflectionTests.cpp)
        target_link_libraries(ComputePipelineReflectionTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
        rendering_test_shader_paths(ComputePipelineReflectionTests)
        if (RENDERING_ENABLE_SPIRV_REFLECTION AND (SPIRV_REFLECT_LIBRARY OR TARGET unofficial::spirv-reflect OR TARGET SPIRV-Reflect::SPIRV-Reflect))
            target_compile_definitions(ComputePipelineReflectionTests PRIVATE RENDERING_ENABLE_SPIRV_REFLECTION=1)
            if (TARGET unofficial::spirv-reflect)
                target_link_libraries(ComputePipelineReflectionTests PRIVATE unofficial::spirv-reflect)
            elseif (TARGET SPIRV-Reflect::SPIRV-Reflect)
                target_link_libraries(ComputePipelineReflectionTests PRIVATE SPIRV-Reflect::SPIRV-Reflect)
            elseif (SPIRV_REFLECT_LIBRARY)
                target_link_libraries(ComputePipelineReflectionTests PRIVATE ${SPIRV_REFLECT_LIBRARY})
            endif()
        endif()
        add_test(NAME ComputePipelineReflectionTests COMMAND $<TARGET_FILE:ComputePipelineReflectionTests>)

        # MaterialBuilder tests

        # Graphics pipeline reflection test
        add_executable(GraphicsPipelineReflectionTests Tests/GraphicsPipelineReflectionTests.cpp)
        target_link_libraries(GraphicsPipelineReflectionTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
        rendering_test_shader_paths(GraphicsPipelineReflectionTests)
        target_include_directories(GraphicsPipelineReflectionTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

        if (RENDERING_ENABLE_SPIRV_REFLECTION AND (SPIRV_REFLECT_LIBRARY OR TARGET unofficial::spirv-reflect OR TARGET SPIRV-Reflect::SPIRV-Reflect))
            target_compile_definitions(GraphicsPipelineReflectionTests PRIVATE RENDERING_ENABLE_SPIRV_REFLECTION=1)
            if (TARGET unofficial::spirv-reflect)
                target_link_libraries(GraphicsPipelineReflectionTests PRIVATE unofficial::spirv-reflect)
            elseif (TARGET SPIRV-Reflect::SPIRV-Reflect)
                target_link_libraries(GraphicsPipelineReflectionTests PRIVATE SPIRV-Reflect::SPIRV-Reflect)
            elseif (SPIRV_REFLECT_LIBRARY)
                target_link_libraries(GraphicsPipelineReflectionTests PRIVATE ${SPIRV_REFLECT_LIBRARY})
            endif()
        endif()
        add_test(NAME GraphicsPipelineReflectionTests COMMAND $<TARGET_FILE:GraphicsPipelineReflectionTests>)

        add_executable(MaterialBuilderTests Tests/MaterialBuilderTests.cpp)
        target_link_libraries(MaterialBuilderTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
        rendering_test_shader_paths(MaterialBuilderTests)
        add_test(NAME MaterialBuilderTests COMMAND $<TARGET_FILE:MaterialBuilderTests>)

        add_executable(MaterialValidationTests Tests/MaterialValidationTests.cpp)
        target_link_libraries(MaterialValidationTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
        add_test(NAME MaterialValidationTests COMMAND $<TARGET_FILE:MaterialValidationTests>)

        # ShaderVariantKey + ShaderComposer tests (incl. package shader roots)
        # The relief march block of Shaders/Includes/parallax_occlusion.glsl, the steps view's colour
        # block of Shaders/Includes/parallax_steps_view.glsl and the relief's depth block of
        # Shaders/Includes/parallax_depth.glsl are EXTRACTED and compiled into this suite
        # as C++ through the shared GLSL shim, so ParallaxOcclusionTests execute the shipped code rather
        # than a copy of it. A missing or reordered marker fails the build.
        set(PARALLAX_OCCLUSION_GLSL ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Includes/parallax_occlusion.glsl)
        set(GLSL_HOST_SHIM_DIR ${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Tests)
        set(PARALLAX_OCCLUSION_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/ParallaxOcclusionExtracted.h)
        add_custom_command(
            OUTPUT ${PARALLAX_OCCLUSION_HEADER}
            COMMAND ${CMAKE_COMMAND} -DSRC=${PARALLAX_OCCLUSION_GLSL}
                                     -DOUT=${PARALLAX_OCCLUSION_HEADER}
                                     "-DBEGIN_MARKER=// GE_SHARED_PARALLAX_OCCLUSION_BEGIN"
                                     "-DEND_MARKER=// GE_SHARED_PARALLAX_OCCLUSION_END"
                                     -P ${GLSL_HOST_SHIM_DIR}/ExtractShaderBlock.cmake
            DEPENDS ${PARALLAX_OCCLUSION_GLSL}
                    ${GLSL_HOST_SHIM_DIR}/ExtractShaderBlock.cmake
            COMMENT "Extracting the relief march from parallax_occlusion.glsl"
            VERBATIM)
        set(PARALLAX_STEPS_VIEW_GLSL ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Includes/parallax_steps_view.glsl)
        set(PARALLAX_STEPS_VIEW_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/ParallaxStepsViewExtracted.h)
        add_custom_command(
            OUTPUT ${PARALLAX_STEPS_VIEW_HEADER}
            COMMAND ${CMAKE_COMMAND} -DSRC=${PARALLAX_STEPS_VIEW_GLSL}
                                     -DOUT=${PARALLAX_STEPS_VIEW_HEADER}
                                     "-DBEGIN_MARKER=// GE_SHARED_PARALLAX_STEPS_VIEW_BEGIN"
                                     "-DEND_MARKER=// GE_SHARED_PARALLAX_STEPS_VIEW_END"
                                     -P ${GLSL_HOST_SHIM_DIR}/ExtractShaderBlock.cmake
            DEPENDS ${PARALLAX_STEPS_VIEW_GLSL}
                    ${GLSL_HOST_SHIM_DIR}/ExtractShaderBlock.cmake
            COMMENT "Extracting the steps view colour from parallax_steps_view.glsl"
            VERBATIM)

        set(PARALLAX_DEPTH_GLSL ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/Includes/parallax_depth.glsl)
        set(PARALLAX_DEPTH_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/ParallaxDepthExtracted.h)
        add_custom_command(
            OUTPUT ${PARALLAX_DEPTH_HEADER}
            COMMAND ${CMAKE_COMMAND} -DSRC=${PARALLAX_DEPTH_GLSL}
                                     -DOUT=${PARALLAX_DEPTH_HEADER}
                                     "-DBEGIN_MARKER=// GE_SHARED_PARALLAX_DEPTH_BEGIN"
                                     "-DEND_MARKER=// GE_SHARED_PARALLAX_DEPTH_END"
                                     -P ${GLSL_HOST_SHIM_DIR}/ExtractShaderBlock.cmake
            DEPENDS ${PARALLAX_DEPTH_GLSL}
                    ${GLSL_HOST_SHIM_DIR}/ExtractShaderBlock.cmake
            COMMENT "Extracting the relief's depth from parallax_depth.glsl"
            VERBATIM)

        add_executable(ShaderVariantKeyTests
            ${PARALLAX_OCCLUSION_HEADER}
            ${PARALLAX_STEPS_VIEW_HEADER}
            ${PARALLAX_DEPTH_HEADER}
            Tests/ParallaxOcclusionTests.cpp
            Tests/RendererProfileTests.cpp
            Tests/ShaderVariantKeyTests.cpp
            Tests/MaterialKeywordDerivationTests.cpp
            Tests/ShaderCompileCacheKeyTests.cpp
            Tests/ShaderMetaBinaryTests.cpp
            Tests/ShaderPackageContainerTests.cpp
            Tests/ShaderSourceCacheTests.cpp
            Tests/ShaderComposeErrorSurfacingTests.cpp
            Tests/ShaderComposerPackageRootsTests.cpp
            Tests/ShaderComposerKeywordDiagnosticsTests.cpp
            Tests/ShaderComposerTextureSlotsTests.cpp
            Tests/ShaderComposerVertexModifierTests.cpp
            Tests/ShaderComposerPropertyEmissionTests.cpp
            Tests/ShaderPropertyTableTests.cpp
            Tests/LegacyMaterialLaneTests.cpp
            Tests/ShippedSurfaceComposeTests.cpp
            Tests/ParticleShaderComposeTests.cpp
            Tests/SurfaceShaderTemplateTests.cpp
            Tests/ProjectSurfaceSceneGrabTests.cpp
            Tests/ForwardAdapterVariantCompileTests.cpp
            Tests/VertexDeformationClockTests.cpp
            Tests/DeformationMotionVariantTests.cpp)
        target_link_libraries(ShaderVariantKeyTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
        # Module-private headers (Source/Materials/ShaderSourceCache.h) by module path.
        target_include_directories(ShaderVariantKeyTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${GLSL_HOST_SHIM_DIR} ${CMAKE_CURRENT_BINARY_DIR}/generated)
        rendering_test_shader_paths(ShaderVariantKeyTests)
        # Real extracted eztree package shaders (Arc 3a acceptance C) and the
        # shipped default-material assets — same source-tree anchoring the
        # suite already uses via RENDERING_SOURCE_DIR.
        target_compile_definitions(ShaderVariantKeyTests PRIVATE
            EZTREE_PACKAGE_SHADERS_DIR="${CMAKE_SOURCE_DIR}/Packages/eztree/Assets/Shaders"
            # The CBT terrain surface and the terrain grass surface are authored in C++
            # (CBTRenderFeature / TerrainGrassRenderFeature build their MaterialDocuments) rather
            # than by a .material file, so no shipped-asset scan reaches either — both have to be
            # named explicitly to be composed and compiled here.
            CBT_SHADER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/Engine/Modules/CBTTerrain/Shaders/CBT"
            TERRAIN_GRASS_SHADER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/Engine/Modules/TerrainGrass/Shaders/TerrainGrass")
        set_target_properties(ShaderVariantKeyTests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            FOLDER "Rendering/Tests"
        )
        add_test(NAME ShaderVariantKeyTests COMMAND $<TARGET_FILE:ShaderVariantKeyTests>)

        # ShaderCapabilityDetector tests
        add_executable(ShaderCapabilityDetectorTests Tests/ShaderCapabilityDetectorTests.cpp)
        target_link_libraries(ShaderCapabilityDetectorTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
        rendering_test_shader_paths(ShaderCapabilityDetectorTests)
        set_target_properties(ShaderCapabilityDetectorTests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            FOLDER "Rendering/Tests"
        )
        add_test(NAME ShaderCapabilityDetectorTests COMMAND $<TARGET_FILE:ShaderCapabilityDetectorTests>)

    # Push constants unit tests
    add_executable(RenderingPushConstantsTests Tests/PushConstantsTests.cpp)
        # Ring buffer copy path test
        add_executable(RingBufferCopyPathTests Tests/RingBufferCopyPathTests.cpp)
        target_link_libraries(RingBufferCopyPathTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
        rendering_test_shader_paths(RingBufferCopyPathTests)
        add_test(NAME RingBufferCopyPathTests COMMAND $<TARGET_FILE:RingBufferCopyPathTests>)
        # Descriptor set allocator threaded tests
        add_executable(DescriptorSetAllocatorThreadedTests Tests/DescriptorSetAllocatorThreadedTests.cpp)
        target_link_libraries(DescriptorSetAllocatorThreadedTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
        rendering_test_shader_paths(DescriptorSetAllocatorThreadedTests)
        add_test(NAME DescriptorSetAllocatorThreadedTests COMMAND $<TARGET_FILE:DescriptorSetAllocatorThreadedTests>)

        # Shader reflection integration tests
        add_executable(ShaderReflectIntegrationTests Tests/ShaderReflectIntegrationTests.cpp)
        target_link_libraries(ShaderReflectIntegrationTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
        rendering_test_shader_paths(ShaderReflectIntegrationTests)
        if (RENDERING_ENABLE_SPIRV_REFLECTION AND SPIRV_REFLECT_LIBRARY)
            target_link_libraries(ShaderReflectIntegrationTests PRIVATE ${SPIRV_REFLECT_LIBRARY})
        endif()
        add_test(NAME ShaderReflectIntegrationTests COMMAND $<TARGET_FILE:ShaderReflectIntegrationTests>)

        # Descriptor reflection tests (descriptor sets + push constants via reflection)
        add_executable(DescriptorReflectionTests Tests/DescriptorReflectionTests.cpp)
        target_link_libraries(DescriptorReflectionTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
        rendering_test_shader_paths(DescriptorReflectionTests)

            # New: Push constants by-name/by-id tests
            add_executable(PushConstantsByNameAndIdTests Tests/PushConstantsByNameAndIdTests.cpp)
            target_link_libraries(PushConstantsByNameAndIdTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
            rendering_test_shader_paths(PushConstantsByNameAndIdTests)
            add_test(NAME PushConstantsByNameAndIdTests COMMAND $<TARGET_FILE:PushConstantsByNameAndIdTests>)

        if (RENDERING_ENABLE_SPIRV_REFLECTION AND SPIRV_REFLECT_LIBRARY)
            target_link_libraries(DescriptorReflectionTests PRIVATE ${SPIRV_REFLECT_LIBRARY})
        endif()
        add_test(NAME DescriptorReflectionTests COMMAND $<TARGET_FILE:DescriptorReflectionTests>)

        # Push constants validation/enforcement tests
        add_executable(PushConstantsEnforcementTests Tests/PushConstantsEnforcementTests.cpp)
        target_link_libraries(PushConstantsEnforcementTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
        rendering_test_shader_paths(PushConstantsEnforcementTests)
        add_test(NAME PushConstantsEnforcementTests COMMAND $<TARGET_FILE:PushConstantsEnforcementTests>)

        # Descriptor set allocator interleaved tests
        add_executable(DescriptorSetAllocatorInterleavedTests Tests/DescriptorSetAllocatorInterleavedTests.cpp)
        target_link_libraries(DescriptorSetAllocatorInterleavedTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
        rendering_test_shader_paths(DescriptorSetAllocatorInterleavedTests)
        add_test(NAME DescriptorSetAllocatorInterleavedTests COMMAND $<TARGET_FILE:DescriptorSetAllocatorInterleavedTests>)
        if (RENDERING_ENABLE_SPIRV_REFLECTION)
            target_compile_definitions(ShaderReflectIntegrationTests PRIVATE RENDERING_ENABLE_SPIRV_REFLECTION=1)
        endif()

        # New: Pipeline layout deduplication tests
        add_executable(PipelineLayoutDedupTests Tests/PipelineLayoutDedupTests.cpp)
        target_link_libraries(PipelineLayoutDedupTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
        rendering_test_shader_paths(PipelineLayoutDedupTests)
        add_test(NAME PipelineLayoutDedupTests COMMAND $<TARGET_FILE:PipelineLayoutDedupTests>)

        # Non-building concrete-pipeline probe used by the record path.
        add_executable(PipelineWarmProbeTests Tests/PipelineWarmProbeTests.cpp)
        target_link_libraries(PipelineWarmProbeTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
        rendering_test_shader_paths(PipelineWarmProbeTests)
        add_test(NAME PipelineWarmProbeTests COMMAND $<TARGET_FILE:PipelineWarmProbeTests>)

        # Pipeline cache intern + concrete + LRU + L1 tests.
        add_executable(PipelineDescHashTests Tests/PipelineDescHashTests.cpp)
        # Same rationale as DescriptorBufferTests below: the rendering unity build
        # pulls SkyRenderer.cpp's LoadSkyTexture2D into GameEngineRendering.lib,
        # which references stbi_* symbols defined only in Engine.lib's
        # stb_image_impl.cpp. Tests that link GameEngineRendering without Engine
        # must compile a private copy to resolve the symbols.
        target_sources(PipelineDescHashTests PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/stb_image_impl.cpp)
        target_compile_definitions(PipelineDescHashTests PRIVATE GE_HAVE_STB)
        if(TARGET stb::stb)
            target_link_libraries(PipelineDescHashTests PRIVATE stb::stb)
        elseif(STB_INCLUDE_DIRS)
            target_include_directories(PipelineDescHashTests PRIVATE ${STB_INCLUDE_DIRS})
        endif()
        target_link_libraries(PipelineDescHashTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
        # Already compiles stb_image_impl.cpp above; only the Logger object splice is needed.
        rendering_test_link_logger(PipelineDescHashTests)
        rendering_test_shader_paths(PipelineDescHashTests)
        add_test(NAME PipelineDescHashTests COMMAND $<TARGET_FILE:PipelineDescHashTests>)

        add_executable(RenderingPushConstantsValidationTests Tests/PushConstantsValidationTests.cpp)
        target_link_libraries(RenderingPushConstantsValidationTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
        rendering_test_shader_paths(RenderingPushConstantsValidationTests)
	        # Offscreen dynamic rendering smoke
	        add_executable(OffscreenDynamicRenderingSmokeTests Tests/OffscreenDynamicRenderingSmokeTests.cpp)
	        target_link_libraries(OffscreenDynamicRenderingSmokeTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
	        rendering_test_shader_paths(OffscreenDynamicRenderingSmokeTests)
	        add_test(NAME OffscreenDynamicRenderingSmokeTests COMMAND $<TARGET_FILE:OffscreenDynamicRenderingSmokeTests>)

    # Fullscreen post (metadata) write validation
    # Dynamic rendering validation (format/count mismatch should not crash)
    add_executable(DynamicRenderingValidationTests Tests/DynamicRenderingValidationTests.cpp)
    target_link_libraries(DynamicRenderingValidationTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(DynamicRenderingValidationTests)
    add_test(NAME DynamicRenderingValidationTests COMMAND $<TARGET_FILE:DynamicRenderingValidationTests>)

    add_executable(FullscreenPostMetadataWritesTest Tests/FullscreenPostMetadataWritesTest.cpp)
    target_link_libraries(FullscreenPostMetadataWritesTest PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(FullscreenPostMetadataWritesTest)
    set_target_properties(FullscreenPostMetadataWritesTest PROPERTIES
        FOLDER "Rendering/Tests")
    # CompileShaderPkgs, not CompileShaders: this suite loads solidcolor.shaderpkg,
    # and the .spv target does not produce one.
    add_dependencies(FullscreenPostMetadataWritesTest CompileShaderPkgs)
    add_test(NAME FullscreenPostMetadataWritesTest COMMAND $<TARGET_FILE:FullscreenPostMetadataWritesTest>)

        add_test(NAME RenderingPushConstantsValidationTests COMMAND $<TARGET_FILE:RenderingPushConstantsValidationTests>)

        # ShaderMeta JSON tests
        add_executable(ShaderMetaJsonTests Tests/ShaderMetaJsonTests.cpp)
        # ShaderReflect CLI defined outside (gated by BUILD_TOOLS/BUILD_TESTING)
        # UBO compute readback regression test
        add_executable(UniformBufferComputeReadbackTests Tests/UniformBufferComputeReadbackTests.cpp)
        target_link_libraries(UniformBufferComputeReadbackTests PRIVATE
            GameEngineRendering
            FileSystemObjects
            GTest::gtest
            GTest::gtest_main
        )
        rendering_test_shader_paths(UniformBufferComputeReadbackTests)
        set_target_properties(UniformBufferComputeReadbackTests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            FOLDER "Rendering/Tests"
        )
        add_test(NAME UniformBufferComputeReadbackTests COMMAND $<TARGET_FILE:UniformBufferComputeReadbackTests>)

        target_link_libraries(ShaderMetaJsonTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
        rendering_test_shader_paths(ShaderMetaJsonTests)
        add_test(NAME ShaderMetaJsonTests COMMAND $<TARGET_FILE:ShaderMetaJsonTests>)

        # Reflection test for nested resources
        add_executable(ReflectNestedResourcesTests Tests/ReflectNestedResourcesTests.cpp)
        target_link_libraries(ReflectNestedResourcesTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
        rendering_test_shader_paths(ReflectNestedResourcesTests)
        add_test(NAME ReflectNestedResourcesTests COMMAND $<TARGET_FILE:ReflectNestedResourcesTests>)

        # Expanded metadata coverage tests (stage IO, SSBOs, samplers, local size)
        add_executable(ShaderReflectionMetadataCoverageTests Tests/ShaderReflectionMetadataCoverageTests.cpp)
        target_link_libraries(ShaderReflectionMetadataCoverageTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
        rendering_test_shader_paths(ShaderReflectionMetadataCoverageTests)
        set_target_properties(ShaderReflectionMetadataCoverageTests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            FOLDER "Rendering/Tests"
        )
        add_test(NAME ShaderReflectionMetadataCoverageTests COMMAND $<TARGET_FILE:ShaderReflectionMetadataCoverageTests>)
        # Additional reflection tests: spec constants, UBOs, conflict detection
        add_executable(ShaderSpecConstantsReflectionTests Tests/ShaderSpecConstantsReflectionTests.cpp)
        target_link_libraries(ShaderSpecConstantsReflectionTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
        rendering_test_shader_paths(ShaderSpecConstantsReflectionTests)
        set_target_properties(ShaderSpecConstantsReflectionTests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            FOLDER "Rendering/Tests"
        )
        add_test(NAME ShaderSpecConstantsReflectionTests COMMAND $<TARGET_FILE:ShaderSpecConstantsReflectionTests>)

        add_executable(ShaderUBOReflectionTests Tests/ShaderUBOReflectionTests.cpp)
        target_link_libraries(ShaderUBOReflectionTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
        rendering_test_shader_paths(ShaderUBOReflectionTests)
        set_target_properties(ShaderUBOReflectionTests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            FOLDER "Rendering/Tests"
        )
        add_test(NAME ShaderUBOReflectionTests COMMAND $<TARGET_FILE:ShaderUBOReflectionTests>)

        add_executable(ShaderReflectionConflictTests Tests/ShaderReflectionConflictTests.cpp)
        target_link_libraries(ShaderReflectionConflictTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
        rendering_test_shader_paths(ShaderReflectionConflictTests)
        set_target_properties(ShaderReflectionConflictTests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            FOLDER "Rendering/Tests"
        )
        add_test(NAME ShaderReflectionConflictTests COMMAND $<TARGET_FILE:ShaderReflectionConflictTests>)
        add_dependencies(ShaderReflectionMetadataCoverageTests CompileShaders)
        add_dependencies(ShaderSpecConstantsReflectionTests CompileShaders)
        add_dependencies(ShaderUBOReflectionTests CompileShaders)
        add_dependencies(ShaderReflectionConflictTests CompileShaders)

    # CLI smoke test for ShaderReflect when reflection is enabled AND available
    if (RENDERING_ENABLE_SPIRV_REFLECTION AND (SPIRV_REFLECT_LIBRARY OR TARGET unofficial::spirv-reflect OR TARGET SPIRV-Reflect::SPIRV-Reflect))
        # CLI spec-constants JSON emission test
        add_executable(ShaderReflectSpecConstantsCLITest Tests/ShaderReflectSpecConstantsCLITest.cpp)
        target_link_libraries(ShaderReflectSpecConstantsCLITest PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
        rendering_test_shader_paths(ShaderReflectSpecConstantsCLITest)
        set_target_properties(ShaderReflectSpecConstantsCLITest PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            FOLDER "Rendering/Tests"
        )
        add_test(NAME ShaderReflectSpecConstantsCLITest COMMAND $<TARGET_FILE:ShaderReflectSpecConstantsCLITest>)
        # CLI push-constant IDs test
        add_executable(ShaderReflectPushConstantIdsCLITest Tests/ShaderReflectPushConstantIdsCLITest.cpp)
        target_link_libraries(ShaderReflectPushConstantIdsCLITest PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
        rendering_test_shader_paths(ShaderReflectPushConstantIdsCLITest)
        set_target_properties(ShaderReflectPushConstantIdsCLITest PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            FOLDER "Rendering/Tests"
        )
        add_test(NAME ShaderReflectPushConstantIdsCLITest COMMAND $<TARGET_FILE:ShaderReflectPushConstantIdsCLITest>)
        if (TARGET ShaderReflect)
            add_dependencies(ShaderReflectPushConstantIdsCLITest CompileShaders ShaderReflect)
        endif()
        if (TARGET ShaderReflect)
            add_dependencies(ShaderReflectSpecConstantsCLITest CompileShaders ShaderReflect)
        endif()

        # Validation unit tests (pure C++)
        add_executable(ShaderMetaValidationTests Tests/ShaderMetaValidationTests.cpp)
        target_link_libraries(ShaderMetaValidationTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
        rendering_test_shader_paths(ShaderMetaValidationTests)
        set_target_properties(ShaderMetaValidationTests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            FOLDER "Rendering/Tests"
        )
        add_test(NAME ShaderMetaValidationTests COMMAND $<TARGET_FILE:ShaderMetaValidationTests>)

        # The triangle blobs are CompileShaders outputs in the build tree; the source tree holds
        # no SPIR-V.
        if (TARGET ShaderReflect)
            add_test(NAME ShaderReflect_CLISmoke_JSON
                     WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                     COMMAND $<TARGET_FILE:ShaderReflect> --vs ${CMAKE_BINARY_DIR}/Shaders/triangle.vert.spv --fs ${CMAKE_BINARY_DIR}/Shaders/triangle.frag.spv)
            add_test(NAME ShaderReflect_CLISmoke_Package
                     WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                     COMMAND $<TARGET_FILE:ShaderReflect> --vs ${CMAKE_BINARY_DIR}/Shaders/triangle.vert.spv --fs ${CMAKE_BINARY_DIR}/Shaders/triangle.frag.spv --out triangle.shaderpkg)
        endif()

        # CLI validation tests
        add_executable(ShaderReflectCLIValidationTests Tests/ShaderReflectCLIValidationTests.cpp)
        target_link_libraries(ShaderReflectCLIValidationTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
        rendering_test_shader_paths(ShaderReflectCLIValidationTests)
        set_target_properties(ShaderReflectCLIValidationTests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            FOLDER "Rendering/Tests"
        )
        add_test(NAME ShaderReflectCLIValidationTests COMMAND $<TARGET_FILE:ShaderReflectCLIValidationTests>)
        if (TARGET ShaderReflect)
            add_dependencies(ShaderReflectCLIValidationTests CompileShaders ShaderReflect)
        endif()
        add_executable(PushConstantIdAssignmentTests Tests/PushConstantIdAssignmentTests.cpp)
        target_link_libraries(PushConstantIdAssignmentTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
        rendering_test_shader_paths(PushConstantIdAssignmentTests)
        set_target_properties(PushConstantIdAssignmentTests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            FOLDER "Rendering/Tests"
        )
        add_test(NAME PushConstantIdAssignmentTests COMMAND $<TARGET_FILE:PushConstantIdAssignmentTests>)

    endif()

        # ShaderReflect still supports --inspect for .shaderpkg and basic JSON/meta emission,
        # but we no longer generate or validate `.shaderdesc` sidecars.

	    # Optional: compile mesh shader sample to SPIR-V if glslc is available.
	    # Reuse the global GLSLC_EXECUTABLE found earlier (which already prefers
	    # vcpkg shaderc tools and then system Vulkan SDK paths).
	    if (GLSLC_EXECUTABLE)
        set(MESH_MESH_SRC ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/mesh_triangle.mesh.glsl)
        set(MESH_FRAG_SRC ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/mesh_triangle.frag.glsl)
        set(MESH_MESH_SPV ${CMAKE_BINARY_DIR}/Shaders/mesh_triangle.mesh.spv)
        set(MESH_FRAG_SPV ${CMAKE_BINARY_DIR}/Shaders/mesh_triangle.frag.spv)
        add_custom_command(OUTPUT ${MESH_MESH_SPV}
            COMMAND ${GLSLC_EXECUTABLE} -fshader-stage=mesh --target-env=vulkan1.3 --target-spv=spv1.6 -o ${MESH_MESH_SPV} ${MESH_MESH_SRC}
            DEPENDS ${MESH_MESH_SRC}
            COMMENT "Compiling mesh shader: ${MESH_MESH_SRC} -> ${MESH_MESH_SPV}")
        add_custom_command(OUTPUT ${MESH_FRAG_SPV}
            COMMAND ${GLSLC_EXECUTABLE} -fshader-stage=frag --target-env=vulkan1.3 --target-spv=spv1.6 -o ${MESH_FRAG_SPV} ${MESH_FRAG_SRC}
            DEPENDS ${MESH_FRAG_SRC}

            COMMENT "Compiling fragment shader: ${MESH_FRAG_SRC} -> ${MESH_FRAG_SPV}")
        add_custom_target(CompileMeshTriangleShaders DEPENDS ${MESH_MESH_SPV} ${MESH_FRAG_SPV})
        # Fallback: glslangValidator if glslc not available or lacking mesh support
    else()
        find_program(GLSLANG_VALIDATOR_EXECUTABLE NAMES glslangValidator HINTS ENV VULKAN_SDK PATH_SUFFIXES Bin bin)
        if (GLSLANG_VALIDATOR_EXECUTABLE)
            set(MESH_MESH_SRC ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/mesh_triangle.mesh.glsl)
            set(MESH_FRAG_SRC ${CMAKE_CURRENT_SOURCE_DIR}/Shaders/mesh_triangle.frag.glsl)
            set(MESH_MESH_SPV ${CMAKE_BINARY_DIR}/Shaders/mesh_triangle.mesh.spv)
            set(MESH_FRAG_SPV ${CMAKE_BINARY_DIR}/Shaders/mesh_triangle.frag.spv)
            add_custom_command(OUTPUT ${MESH_MESH_SPV}
                COMMAND ${GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.3 -S mesh -o ${MESH_MESH_SPV} ${MESH_MESH_SRC}
                DEPENDS ${MESH_MESH_SRC}
                COMMENT "glslangValidator: ${MESH_MESH_SRC} -> ${MESH_MESH_SPV}")
            add_custom_command(OUTPUT ${MESH_FRAG_SPV}
                COMMAND ${GLSLANG_VALIDATOR_EXECUTABLE} -V --target-env vulkan1.3 -S frag -o ${MESH_FRAG_SPV} ${MESH_FRAG_SRC}
                DEPENDS ${MESH_FRAG_SRC}
                COMMENT "glslangValidator: ${MESH_FRAG_SRC} -> ${MESH_FRAG_SPV}")
            add_custom_target(CompileMeshTriangleShaders DEPENDS ${MESH_MESH_SPV} ${MESH_FRAG_SPV})
        else()
            message(STATUS "No mesh-capable GLSL compiler found; provide precompiled .spv for mesh sample or install glslc/glslangValidator")
        endif()
    endif()

    # The test asserts the mesh blob exists whenever the build compiles it.
    if (TARGET CompileMeshTriangleShaders AND TARGET ShaderReflectIntegrationTests)
        add_dependencies(ShaderReflectIntegrationTests CompileMeshTriangleShaders)
        target_compile_definitions(ShaderReflectIntegrationTests PRIVATE RENDERING_TEST_MESH_SHADER_COMPILED=1)
    endif()

    # Copy mesh sample SPIR-V to build dir for examples/tests
    if (EXISTS ${MESH_MESH_SPV})
        configure_file(${MESH_MESH_SPV} ${CMAKE_CURRENT_BINARY_DIR}/Shaders/mesh_triangle.mesh.spv COPYONLY)
    endif()
    if (EXISTS ${MESH_FRAG_SPV})
        configure_file(${MESH_FRAG_SPV} ${CMAKE_CURRENT_BINARY_DIR}/Shaders/mesh_triangle.frag.spv COPYONLY)
    endif()

    # Copy mesh sample SPIR-V to build dir for examples/tests
    if (EXISTS ${MESH_MESH_SPV})
        configure_file(${MESH_MESH_SPV} ${CMAKE_CURRENT_BINARY_DIR}/Shaders/mesh_triangle.mesh.spv COPYONLY)
    endif()
    if (EXISTS ${MESH_FRAG_SPV})
        configure_file(${MESH_FRAG_SPV} ${CMAKE_CURRENT_BINARY_DIR}/Shaders/mesh_triangle.frag.spv COPYONLY)
    endif()

        # Descriptor set allocator tests
        add_executable(DescriptorSetAllocatorTests Tests/DescriptorSetAllocatorTests.cpp)
        target_link_libraries(DescriptorSetAllocatorTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
        rendering_test_shader_paths(DescriptorSetAllocatorTests)
        add_test(NAME DescriptorSetAllocatorTests COMMAND $<TARGET_FILE:DescriptorSetAllocatorTests>)

        add_executable(MaterialBuilderMergeTests Tests/MaterialBuilderMergeTests.cpp)
        target_link_libraries(MaterialBuilderMergeTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
        rendering_test_shader_paths(MaterialBuilderMergeTests)
        add_test(NAME MaterialBuilderMergeTests COMMAND $<TARGET_FILE:MaterialBuilderMergeTests>)

        add_executable(MaterialBuilderFormatsTests Tests/MaterialBuilderFormatsTests.cpp)
        target_link_libraries(MaterialBuilderFormatsTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
        rendering_test_shader_paths(MaterialBuilderFormatsTests)
        add_test(NAME MaterialBuilderFormatsTests COMMAND $<TARGET_FILE:MaterialBuilderFormatsTests>)

    target_link_libraries(RenderingPushConstantsTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(RenderingPushConstantsTests)
    add_test(NAME RenderingPushConstantsTests COMMAND $<TARGET_FILE:RenderingPushConstantsTests>)

    # Ring buffer helpers tests
    add_executable(RenderingRingBufferHelpersTests Tests/RingBufferHelpersTests.cpp)
    target_link_libraries(RenderingRingBufferHelpersTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(RenderingRingBufferHelpersTests)
    add_test(NAME RenderingRingBufferHelpersTests COMMAND $<TARGET_FILE:RenderingRingBufferHelpersTests>)

endif()

if(BUILD_TESTING)

if(RENDERING_BUILD_TESTS)
    enable_testing()
    # Metal backend tests (macOS native Metal backend; sources added on main).
    # Every other present/windowed/aliasing/benchmark target from this block was
    # deleted by the RG2 cutover (sources no longer exist), so only the Metal
    # tests remain. Gated on RENDERING_HAS_METAL (off for the Vulkan/MoltenVK build).
    if(RENDERING_HAS_METAL)
        if(TARGET glfw)
            add_executable(RenderingMetalTriangleSmokeTests Tests/MetalTriangleSmokeTests.cpp)
            target_link_libraries(RenderingMetalTriangleSmokeTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main glfw)
            target_compile_definitions(RenderingMetalTriangleSmokeTests PRIVATE HAVE_GLFW=1)
            set_target_properties(RenderingMetalTriangleSmokeTests PROPERTIES
                CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON FOLDER "Rendering/Tests")
            add_test(NAME RenderingMetalTriangleSmokeTests COMMAND $<TARGET_FILE:RenderingMetalTriangleSmokeTests>)
            set_tests_properties(RenderingMetalTriangleSmokeTests PROPERTIES TIMEOUT 45 RUN_SERIAL TRUE LABELS "Present;Windowed;Metal")
        endif()

        add_executable(RenderingMetalBackendHeadlessTests Tests/MetalBackendHeadlessTests.cpp Tests/MetalHdrOutputTests.cpp)
        target_link_libraries(RenderingMetalBackendHeadlessTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
        # The tests read backend state through MetalDevice.h, which includes metal-cpp.
        target_include_directories(RenderingMetalBackendHeadlessTests SYSTEM PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../../../ThirdParty/metal-cpp)
        set_target_properties(RenderingMetalBackendHeadlessTests PROPERTIES
            CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON FOLDER "Rendering/Tests")
        add_test(NAME RenderingMetalBackendHeadlessTests COMMAND $<TARGET_FILE:RenderingMetalBackendHeadlessTests>)
        set_tests_properties(RenderingMetalBackendHeadlessTests PROPERTIES TIMEOUT 60 LABELS "Headless;Metal")
        # The same binary under Metal API validation, which aborts the process
        # on a usage violation instead of tolerating it.
        add_test(NAME RenderingMetalBackendHeadlessTests.MetalDebugLayer
                 COMMAND $<TARGET_FILE:RenderingMetalBackendHeadlessTests> --gtest_filter=MetalClear.*:MetalDebugLayer.*)
        set_tests_properties(RenderingMetalBackendHeadlessTests.MetalDebugLayer PROPERTIES
            TIMEOUT 60 LABELS "Headless;Metal"
            ENVIRONMENT "MTL_DEBUG_LAYER=1;MTL_DEBUG_LAYER_ERROR_MODE=assert")
    endif()

    # Ensure tests have a main if gtest_main wasn't linked for some reason
    if(NOT TARGET GTest::gtest_main)
        add_library(RenderingGTestMain OBJECT Tests/gtest_main_adapter.cpp)
        target_link_libraries(RenderingGTestMain PUBLIC GTest::gtest)
        target_compile_features(RenderingGTestMain PUBLIC cxx_std_20)
    endif()

    # Only register GoogleTest-based executables when GTest targets exist
    if(GTest_FOUND OR TARGET GTest::gtest)

        add_test(NAME RenderingMatrixConversionValidationTests
                 COMMAND $<TARGET_FILE:RenderingMatrixConversionValidationTests>)

        # (removed duplicate) add_test(NAME RenderingRenderPassBeginTests
#                  COMMAND $<TARGET_FILE:RenderingRenderPassBeginTests>)
    endif()

        # add_test for RenderingResourceBarrierTests only when the target exists (requires GTest)
        if(TARGET RenderingResourceBarrierTests)
            add_test(NAME RenderingResourceBarrierTests
                     COMMAND $<TARGET_FILE:RenderingResourceBarrierTests>)
        endif()

	    if (TARGET RenderingCascadeCullingUnifiedParityTests)
	        add_test(NAME RenderingCascadeCullingUnifiedParityTests
	                 COMMAND $<TARGET_FILE:RenderingCascadeCullingUnifiedParityTests>)
	        set_tests_properties(RenderingCascadeCullingUnifiedParityTests
	            PROPERTIES SKIP_RETURN_CODE 77)
	    endif()

    add_test(NAME RenderingMinimalComputeTest
             COMMAND $<TARGET_FILE:RenderingMinimalComputeTest>)

    add_test(NAME RenderingMinimalComputeHostVisibleTest
             COMMAND $<TARGET_FILE:RenderingMinimalComputeHostVisibleTest>)

    # Optional gtest XML output can be enabled via ctest args if needed
endif()
endif()

# Descriptor helper tests
if(GTest_FOUND)
    add_executable(DescriptorHelpersTests Tests/DescriptorHelpersTests.cpp)
    target_link_libraries(DescriptorHelpersTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(DescriptorHelpersTests)
    set_target_properties(DescriptorHelpersTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests"
    )
    add_test(NAME DescriptorHelpersTests COMMAND $<TARGET_FILE:DescriptorHelpersTests>)

    add_executable(TypedPushConstantsTests Tests/TypedPushConstantsTests.cpp)
    target_link_libraries(TypedPushConstantsTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(TypedPushConstantsTests)
    set_target_properties(TypedPushConstantsTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests"
    )
    add_test(NAME TypedPushConstantsTests COMMAND $<TARGET_FILE:TypedPushConstantsTests>)
    add_dependencies(TypedPushConstantsTests CompileShaders)
    # Dedicated tests for named push-constant ranges
    add_executable(PushConstantsNamedRangesTests Tests/PushConstantsNamedRangesTests.cpp)
    add_dependencies(PushConstantsNamedRangesTests CompileShaders)
    target_link_libraries(PushConstantsNamedRangesTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(PushConstantsNamedRangesTests)
    set_target_properties(PushConstantsNamedRangesTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests"
    )
    add_test(NAME PushConstantsNamedRangesTests COMMAND $<TARGET_FILE:PushConstantsNamedRangesTests>)

    # Smoke test for storage image descriptor helpers
    add_executable(StorageImageDescriptorHelpersTests Tests/StorageImageDescriptorHelpersTests.cpp)
    target_link_libraries(StorageImageDescriptorHelpersTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(StorageImageDescriptorHelpersTests)
    set_target_properties(StorageImageDescriptorHelpersTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests"
    )
    add_test(NAME StorageImageDescriptorHelpersTests COMMAND $<TARGET_FILE:StorageImageDescriptorHelpersTests>)

    add_executable(TextureViewDepthAspectTests Tests/TextureViewDepthAspectTests.cpp)
    target_link_libraries(TextureViewDepthAspectTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(TextureViewDepthAspectTests)
    set_target_properties(TextureViewDepthAspectTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests")
    add_test(NAME TextureViewDepthAspectTests COMMAND $<TARGET_FILE:TextureViewDepthAspectTests>)

    add_executable(ImageCopyMipReadbackTests Tests/ImageCopyMipReadbackTests.cpp)
    target_link_libraries(ImageCopyMipReadbackTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(ImageCopyMipReadbackTests)
    set_target_properties(ImageCopyMipReadbackTests PROPERTIES
        FOLDER "Rendering/Tests")
    add_test(NAME ImageCopyMipReadbackTests COMMAND $<TARGET_FILE:ImageCopyMipReadbackTests>)

    # Named push constant writer tests
    add_executable(NamedPushConstantWriterTests Tests/NamedPushConstantWriterTests.cpp)
    target_link_libraries(NamedPushConstantWriterTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(NamedPushConstantWriterTests)
    set_target_properties(NamedPushConstantWriterTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests")
    add_test(NAME NamedPushConstantWriterTests COMMAND $<TARGET_FILE:NamedPushConstantWriterTests>)
    # BufferRing tests
    add_executable(BufferRingTests Tests/BufferRingTests.cpp)
    target_link_libraries(BufferRingTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    # The map-failure test subclasses VulkanDevice, which lives under Source/.
    target_include_directories(BufferRingTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    rendering_test_shader_paths(BufferRingTests)
    set_target_properties(BufferRingTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests")
    add_test(NAME BufferRingTests COMMAND $<TARGET_FILE:BufferRingTests>)

    # FrameBufferAllocator end-to-end graphics test (allocator suballocation
    # bound as a UBO at a non-zero offset in a real draw)
    add_executable(FrameBufferAllocatorGraphicsE2ETest Tests/FrameBufferAllocatorGraphicsE2ETest.cpp)
    target_link_libraries(FrameBufferAllocatorGraphicsE2ETest PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(FrameBufferAllocatorGraphicsE2ETest)
    set_target_properties(FrameBufferAllocatorGraphicsE2ETest PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests")
    add_test(NAME FrameBufferAllocatorGraphicsE2ETest COMMAND $<TARGET_FILE:FrameBufferAllocatorGraphicsE2ETest>)

    add_executable(TextureUsagePolicyTests Tests/TextureUsagePolicyTests.cpp)
    target_link_libraries(TextureUsagePolicyTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(TextureUsagePolicyTests)
    set_target_properties(TextureUsagePolicyTests PROPERTIES
        FOLDER "Rendering/Tests")
    add_test(NAME TextureUsagePolicyTests COMMAND $<TARGET_FILE:TextureUsagePolicyTests>)

    # Its own executable: GE_STRICT_TEXTURE_USAGE resolves once per process, and the
    # policy suite needs the permissive backend to demonstrate the drift it detects.
    add_executable(TextureUsageStrictTests Tests/TextureUsageStrictTests.cpp)
    target_link_libraries(TextureUsageStrictTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(TextureUsageStrictTests)
    set_target_properties(TextureUsageStrictTests PROPERTIES
        FOLDER "Rendering/Tests")
    add_test(NAME TextureUsageStrictTests COMMAND $<TARGET_FILE:TextureUsageStrictTests>)

    add_executable(DirectTextureViewSamplingTests Tests/DirectTextureViewSamplingTests.cpp)
    target_link_libraries(DirectTextureViewSamplingTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(DirectTextureViewSamplingTests)
    set_target_properties(DirectTextureViewSamplingTests PROPERTIES
        FOLDER "Rendering/Tests")
    add_test(NAME DirectTextureViewSamplingTests COMMAND $<TARGET_FILE:DirectTextureViewSamplingTests>)

    add_executable(DynamicRenderingPipelineFormatsTests Tests/DynamicRenderingPipelineFormatsTests.cpp)
    target_link_libraries(DynamicRenderingPipelineFormatsTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(DynamicRenderingPipelineFormatsTests)
    set_target_properties(DynamicRenderingPipelineFormatsTests PROPERTIES
        FOLDER "Rendering/Tests")
    add_test(NAME DynamicRenderingPipelineFormatsTests COMMAND $<TARGET_FILE:DynamicRenderingPipelineFormatsTests>)

endif()
# Reflection and death tests
if(GTest_FOUND)
    # Descriptor validation negative tests (debug-assert based)
    add_executable(DescriptorValidationNegativeTests Tests/DescriptorValidationNegativeTests.cpp)
    target_link_libraries(DescriptorValidationNegativeTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(DescriptorValidationNegativeTests)
    set_target_properties(DescriptorValidationNegativeTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests"
    )
    add_test(NAME DescriptorValidationNegativeTests COMMAND $<TARGET_FILE:DescriptorValidationNegativeTests>)

    # Typed push constants UX tests
    add_executable(TypedPushConstantsUXTests Tests/TypedPushConstantsUXTests.cpp)
    target_link_libraries(TypedPushConstantsUXTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(TypedPushConstantsUXTests)
    set_target_properties(TypedPushConstantsUXTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests"
    )
    add_test(NAME TypedPushConstantsUXTests COMMAND $<TARGET_FILE:TypedPushConstantsUXTests>)

    add_executable(MaterialBuilderStageMaskMergeTests Tests/MaterialBuilderStageMaskMergeTests.cpp)
    target_link_libraries(MaterialBuilderStageMaskMergeTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(MaterialBuilderStageMaskMergeTests)
    set_target_properties(MaterialBuilderStageMaskMergeTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests"
    )
    add_test(NAME MaterialBuilderStageMaskMergeTests COMMAND $<TARGET_FILE:MaterialBuilderStageMaskMergeTests>)
endif()

if(GTest_FOUND)

    add_executable(PushConstantPolicyTests Tests/PushConstantPolicyTests.cpp)
    target_link_libraries(PushConstantPolicyTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(PushConstantPolicyTests)
    set_target_properties(PushConstantPolicyTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests"
    )
    add_test(NAME PushConstantPolicyTests COMMAND $<TARGET_FILE:PushConstantPolicyTests>)

    add_executable(ComputeReflectionTests Tests/ComputeReflectionTests.cpp)
    target_link_libraries(ComputeReflectionTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)
    rendering_test_shader_paths(ComputeReflectionTests)
    set_target_properties(ComputeReflectionTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests"
    )
    add_test(NAME ComputeReflectionTests COMMAND $<TARGET_FILE:ComputeReflectionTests>)

    add_executable(PushConstantStageMaskEndToEndTests Tests/PushConstantStageMaskEndToEndTests.cpp)
    target_link_libraries(PushConstantStageMaskEndToEndTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(PushConstantStageMaskEndToEndTests)
    set_target_properties(PushConstantStageMaskEndToEndTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests"
    )
    add_test(NAME PushConstantStageMaskEndToEndTests COMMAND $<TARGET_FILE:PushConstantStageMaskEndToEndTests>)

    add_executable(TypedPushConstantsDeathTests Tests/TypedPushConstantsDeathTests.cpp)
    target_link_libraries(TypedPushConstantsDeathTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(TypedPushConstantsDeathTests)
    set_target_properties(TypedPushConstantsDeathTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests"
    )
    # gtest ASSERT_DEATH statement blocks trip C4702 (unreachable code) under
    # MSVC warnings-as-errors — the unreachable tail after the aborting call is
    # the death-test pattern itself, not a defect. Scoped to the death targets.
    if(MSVC)
        target_compile_options(TypedPushConstantsDeathTests PRIVATE /wd4702)
    endif()
    add_test(NAME TypedPushConstantsDeathTests COMMAND $<TARGET_FILE:TypedPushConstantsDeathTests>)
    add_dependencies(TypedPushConstantsDeathTests CompileShaders)

    add_executable(FrameSlottedBufferWriteTests Tests/FrameSlottedBufferWriteTests.cpp)
    target_link_libraries(FrameSlottedBufferWriteTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    set_target_properties(FrameSlottedBufferWriteTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests"
    )
    # Same C4702 death-test pattern exemption as TypedPushConstantsDeathTests.
    if(MSVC)
        target_compile_options(FrameSlottedBufferWriteTests PRIVATE /wd4702)
    endif()
    add_test(NAME FrameSlottedBufferWriteTests COMMAND $<TARGET_FILE:FrameSlottedBufferWriteTests>)
endif()

# Typed push constants compute stage mask test
if(GTest_FOUND)
    add_executable(ComputePushConstantsStageMaskTest Tests/ComputePushConstantsStageMaskTest.cpp)
    target_link_libraries(ComputePushConstantsStageMaskTest PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(ComputePushConstantsStageMaskTest)
    set_target_properties(ComputePushConstantsStageMaskTest PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests"
    )
    add_test(NAME ComputePushConstantsStageMaskTest COMMAND $<TARGET_FILE:ComputePushConstantsStageMaskTest>)
    # Ensure shader compilation runs before this test
    add_dependencies(ComputePushConstantsStageMaskTest CompileShaders)
endif()

# Graphics push constants stage mask tests
if(GTest_FOUND)
    add_executable(GraphicsPushConstantsStageMaskTest Tests/GraphicsPushConstantsStageMaskTest.cpp)
    target_link_libraries(GraphicsPushConstantsStageMaskTest PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(GraphicsPushConstantsStageMaskTest)
    set_target_properties(GraphicsPushConstantsStageMaskTest PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests"
    )
    add_test(NAME GraphicsPushConstantsStageMaskTest COMMAND $<TARGET_FILE:GraphicsPushConstantsStageMaskTest>)
    # Ensure shader compilation runs before this test
    add_dependencies(GraphicsPushConstantsStageMaskTest CompileShaders)
    add_dependencies(PushConstantsByNameAndIdTests CompileShaders)

endif()

# Typed push constants death and offset tests
if(GTest_FOUND)
    add_executable(TypedPushConstantsComputeDeathTests Tests/TypedPushConstantsComputeDeathTests.cpp)
    target_link_libraries(TypedPushConstantsComputeDeathTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(TypedPushConstantsComputeDeathTests)
    set_target_properties(TypedPushConstantsComputeDeathTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests"
    )
    # Same C4702 death-test pattern exemption as TypedPushConstantsDeathTests.
    if(MSVC)
        target_compile_options(TypedPushConstantsComputeDeathTests PRIVATE /wd4702)
    endif()
    add_dependencies(TypedPushConstantsComputeDeathTests CompileShaders)
    add_test(NAME TypedPushConstantsComputeDeathTests COMMAND $<TARGET_FILE:TypedPushConstantsComputeDeathTests>)

    add_executable(TypedPushConstantsOffsetTests Tests/TypedPushConstantsOffsetTests.cpp)
    target_link_libraries(TypedPushConstantsOffsetTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(TypedPushConstantsOffsetTests)
    set_target_properties(TypedPushConstantsOffsetTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests"
    )
    add_dependencies(TypedPushConstantsOffsetTests CompileShaders)
    add_test(NAME TypedPushConstantsOffsetTests COMMAND $<TARGET_FILE:TypedPushConstantsOffsetTests>)
endif()

# Descriptor writer test
if(GTest_FOUND)
    add_executable(DescriptorWriterTests Tests/DescriptorWriterTests.cpp)
    target_link_libraries(DescriptorWriterTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(DescriptorWriterTests)
    set_target_properties(DescriptorWriterTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests"
    )
    add_test(NAME DescriptorWriterTests COMMAND $<TARGET_FILE:DescriptorWriterTests>)
endif()

# Named descriptor writer tests
if(GTest_FOUND)
    add_executable(NamedDescriptorWriterTests Tests/NamedDescriptorWriterTests.cpp)
    target_link_libraries(NamedDescriptorWriterTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(NamedDescriptorWriterTests)
    set_target_properties(NamedDescriptorWriterTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests"
    )
    add_test(NAME NamedDescriptorWriterTests COMMAND $<TARGET_FILE:NamedDescriptorWriterTests>)
endif()

# Typed push constants offset tests
if(GTest_FOUND)
    add_executable(TypedPushConstantsOffsetDeathTests Tests/TypedPushConstantsOffsetDeathTests.cpp)
    target_link_libraries(TypedPushConstantsOffsetDeathTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(TypedPushConstantsOffsetDeathTests)
    set_target_properties(TypedPushConstantsOffsetDeathTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests"
    )
    # Same C4702 death-test pattern exemption as TypedPushConstantsDeathTests.
    if(MSVC)
        target_compile_options(TypedPushConstantsOffsetDeathTests PRIVATE /wd4702)
    endif()
    add_dependencies(TypedPushConstantsOffsetDeathTests CompileShaders)
    add_test(NAME TypedPushConstantsOffsetDeathTests COMMAND $<TARGET_FILE:TypedPushConstantsOffsetDeathTests>)
endif()
# Descriptor set allocator stats tests
if(GTest_FOUND)
    add_executable(DescriptorSetAllocatorStatsTests Tests/DescriptorSetAllocatorStatsTests.cpp)
    target_link_libraries(DescriptorSetAllocatorStatsTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(DescriptorSetAllocatorStatsTests)
    set_target_properties(DescriptorSetAllocatorStatsTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests"
    )
    add_test(NAME DescriptorSetAllocatorStatsTests COMMAND $<TARGET_FILE:DescriptorSetAllocatorStatsTests>)
endif()

# Descriptor writer array tests
if(GTest_FOUND)
    add_executable(DescriptorWriterArrayTests Tests/DescriptorWriterArrayTests.cpp)
    target_link_libraries(DescriptorWriterArrayTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(DescriptorWriterArrayTests)
    set_target_properties(DescriptorWriterArrayTests PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
        FOLDER "Rendering/Tests"
    )
    add_test(NAME DescriptorWriterArrayTests COMMAND $<TARGET_FILE:DescriptorWriterArrayTests>)
endif()

message(STATUS "Multiple triangles example configured for architecture validation!")
message(STATUS "Bindless texture example configured for modern GPU-driven rendering!")
message(STATUS "Bindless buffer example configured for modern GPU-driven buffer management!")
message(STATUS "Integrated bindless + render graph example configured for seamless GPU-driven rendering!")
message(STATUS "Bindless performance test suite configured for comprehensive benchmarking!")
message(STATUS "GPU Scene Management example configured for Phase 2 GPU-driven rendering!")
if (RENDERING_ENABLE_GPU_CULLING)
    message(STATUS "GPU Culling Pipeline example configured for Phase 2 GPU-driven culling!")
    message(STATUS "Visual GPU Culling example configured for windowed demonstration!")
    message(STATUS "GPU Culling automated test suite configured for quality assurance!")
endif()

# Tests (disabled for now)
# if(RENDERING_BUILD_TESTS)
#     enable_testing()
#     add_subdirectory(Tests)
# endif()

# Installation (disabled for now)
# install(TARGETS GameEngineRendering
#     EXPORT GameEngineRenderingTargets
#     LIBRARY DESTINATION lib
#     ARCHIVE DESTINATION lib
#     RUNTIME DESTINATION bin
#     INCLUDES DESTINATION include
# )

# install(DIRECTORY Include/
#     DESTINATION include
#     FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
# )

# Export configuration (disabled for now)
# install(EXPORT GameEngineRenderingTargets
#     FILE GameEngineRenderingTargets.cmake
#     NAMESPACE GameEngine::
#     DESTINATION lib/cmake/GameEngineRendering
# )

# Generate config file (disabled for now)
# include(CMakePackageConfigHelpers)
# configure_package_config_file(
#     "${CMAKE_CURRENT_SOURCE_DIR}/Config.cmake.in"
#     "${CMAKE_CURRENT_BINARY_DIR}/GameEngineRenderingConfig.cmake"
#     INSTALL_DESTINATION lib/cmake/GameEngineRendering
# )

# install(FILES
#     "${CMAKE_CURRENT_BINARY_DIR}/GameEngineRenderingConfig.cmake"
#     DESTINATION lib/cmake/GameEngineRendering
# )

# Extra RenderGraph tests
if(GTest_FOUND OR TARGET GTest::gtest)

  add_executable(TextureViewTests Tests/TextureViewTests.cpp)
  # GameEngineRendering's unity build pulls SkyRenderer.cpp's LoadSkyTexture2D,
  # which references stbi_* symbols defined only in Engine.lib's stb_image_impl.cpp.
  # Tests that link GameEngineRendering but not Engine must compile a private copy.
  target_sources(TextureViewTests PRIVATE ${CMAKE_SOURCE_DIR}/Engine/Source/ThirdParty/stb_image_impl.cpp)
  target_compile_definitions(TextureViewTests PRIVATE GE_HAVE_STB)
  if(TARGET stb::stb)
    target_link_libraries(TextureViewTests PRIVATE stb::stb)
  elseif(STB_INCLUDE_DIRS)
    target_include_directories(TextureViewTests PRIVATE ${STB_INCLUDE_DIRS})
  endif()
  target_link_libraries(TextureViewTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
  # Already compiles stb_image_impl.cpp above; only the Logger object splice is needed.
  rendering_test_link_logger(TextureViewTests)
  rendering_test_shader_paths(TextureViewTests)
  set_target_properties(TextureViewTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    FOLDER "Rendering/Tests")
  add_test(NAME TextureViewTests COMMAND $<TARGET_FILE:TextureViewTests>)
endif()

# PBR end-to-end test (compute LUT -> PBR forward -> tonemap -> readback)
if(GTest_FOUND OR TARGET GTest::gtest)
  add_executable(PBRForwardEndToEndTests Tests/PBRForwardEndToEndTests.cpp)
  target_link_libraries(PBRForwardEndToEndTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
  target_include_directories(PBRForwardEndToEndTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  rendering_test_link_stb(PBRForwardEndToEndTests)
  rendering_test_shader_paths(PBRForwardEndToEndTests)

  add_executable(DescriptorBindStatsLayoutStableSwitchTests Tests/DescriptorBindStatsLayoutStableSwitchTests.cpp)
  target_link_libraries(DescriptorBindStatsLayoutStableSwitchTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
  rendering_test_shader_paths(DescriptorBindStatsLayoutStableSwitchTests)
  set_target_properties(DescriptorBindStatsLayoutStableSwitchTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    FOLDER "Rendering/Tests")
  add_test(NAME DescriptorBindStatsLayoutStableSwitchTests COMMAND $<TARGET_FILE:DescriptorBindStatsLayoutStableSwitchTests>)

add_executable(BarrierMemoryBatchTests Tests/BarrierMemoryBatchTests.cpp)
target_link_libraries(BarrierMemoryBatchTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
rendering_test_shader_paths(BarrierMemoryBatchTests)
target_include_directories(BarrierMemoryBatchTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
set_target_properties(BarrierMemoryBatchTests PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  FOLDER "Rendering/Tests")
add_test(NAME BarrierMemoryBatchTests COMMAND $<TARGET_FILE:BarrierMemoryBatchTests>)

  add_executable(BarrierBatchBufferDedupeTests Tests/BarrierBatchBufferDedupeTests.cpp)
  target_link_libraries(BarrierBatchBufferDedupeTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
  rendering_test_shader_paths(BarrierBatchBufferDedupeTests)
  target_include_directories(BarrierBatchBufferDedupeTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
  set_target_properties(BarrierBatchBufferDedupeTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    FOLDER "Rendering/Tests")
  add_test(NAME BarrierBatchBufferDedupeTests COMMAND $<TARGET_FILE:BarrierBatchBufferDedupeTests>)

  # ResourceState -> Vulkan stage/access/layout barrier mapping (no device needed)
  if(RENDERING_HAS_VULKAN)
    add_executable(VulkanResourceStateMappingTests Tests/VulkanResourceStateMappingTests.cpp)
    target_link_libraries(VulkanResourceStateMappingTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
    rendering_test_shader_paths(VulkanResourceStateMappingTests)
    target_include_directories(VulkanResourceStateMappingTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    set_target_properties(VulkanResourceStateMappingTests PROPERTIES
      CXX_STANDARD 20
      CXX_STANDARD_REQUIRED ON
      FOLDER "Rendering/Tests")
    add_test(NAME VulkanResourceStateMappingTests COMMAND $<TARGET_FILE:VulkanResourceStateMappingTests>)
  endif()

  # ACES tonemap parameter defaults test (reflection-driven)
  add_executable(TonemapAcesParamsTests Tests/TonemapAcesParamsTests.cpp)
  target_link_libraries(TonemapAcesParamsTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
  target_include_directories(TonemapAcesParamsTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  rendering_test_link_stb(TonemapAcesParamsTests)
  rendering_test_shader_paths(TonemapAcesParamsTests)
  set_target_properties(TonemapAcesParamsTests PROPERTIES
    FOLDER "Rendering/Tests")
  add_test(NAME TonemapAcesParamsTests COMMAND $<TARGET_FILE:TonemapAcesParamsTests>)

  # HDR highlight-extension contract for tonemap.frag (GPU, real SPIR-V).
  add_executable(TonemapHdrHeadroomTests Tests/TonemapHdrHeadroomTests.cpp)
  target_link_libraries(TonemapHdrHeadroomTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
  target_include_directories(TonemapHdrHeadroomTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  rendering_test_link_stb(TonemapHdrHeadroomTests)
  rendering_test_shader_paths(TonemapHdrHeadroomTests)
  # The suite reads tonemap.frag.spv, so the shader compile must have run.
  add_dependencies(TonemapHdrHeadroomTests CompileShaders)
  set_target_properties(TonemapHdrHeadroomTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    FOLDER "Rendering/Tests")
  add_test(NAME TonemapHdrHeadroomTests COMMAND $<TARGET_FILE:TonemapHdrHeadroomTests>)

  add_executable(CubeLutFileParserTests Tests/CubeLutFileParserTests.cpp)
  target_link_libraries(CubeLutFileParserTests PRIVATE GameEngineRendering FileSystemObjects GTest::gtest GTest::gtest_main)
  target_include_directories(CubeLutFileParserTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  set_target_properties(CubeLutFileParserTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    FOLDER "Rendering/Tests")
  add_test(NAME CubeLutFileParserTests COMMAND $<TARGET_FILE:CubeLutFileParserTests>)

  set_target_properties(PBRForwardEndToEndTests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    FOLDER "Rendering/Tests")
  add_dependencies(PBRForwardEndToEndTests CompileShaders)
  add_test(NAME PBRForwardEndToEndTests COMMAND $<TARGET_FILE:PBRForwardEndToEndTests>)
endif()

# Label a small subset of fast rendering tests for quick CI runs
# Note: these must be defined before setting properties; this block lives at file end and references existing tests by name
if (BUILD_TESTING)
    # These gtest-based unit tests are added when GTest is found
    set_tests_properties(DynamicRenderingValidationTests PROPERTIES LABELS "RenderingQuick")
endif()

# -----------------------------------------------------------------------------
# UI Shapes demo example (offscreen) and Shape Geometry tests
# -----------------------------------------------------------------------------

# Every executable that links the GameEngineRendering OBJECT library directly hits
# the same latent rot: OBJECT->OBJECT object files (Logger) and the stb_image impl
# pulled in by SkyRenderer are not spliced transitively on the VS generator, so
# they come up as unresolved externals at link time. Rather than hand-maintain a
# rendering_test_link_stb call next to each of the ~150 test targets above, sweep
# this directory's executables once: any that link GameEngineRendering and have
# not already been spliced (no Logger objects in their SOURCES yet — i.e. the
# targets that compile their own stb_image_impl.cpp call rendering_test_link_logger
# explicitly above) get the full helper here.
get_property(_GE_RENDERING_DIR_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_tgt IN LISTS _GE_RENDERING_DIR_TARGETS)
	get_target_property(_tgt_type ${_tgt} TYPE)
	if(NOT _tgt_type STREQUAL "EXECUTABLE")
		continue()
	endif()
	get_target_property(_tgt_libs ${_tgt} LINK_LIBRARIES)
	if(NOT _tgt_libs OR NOT "GameEngineRendering" IN_LIST _tgt_libs)
		continue()
	endif()
	# Skip targets already spliced (the 12 that call the helpers explicitly above):
	# their SOURCES carry the $<TARGET_OBJECTS:Logger> generator expression.
	get_target_property(_tgt_srcs ${_tgt} SOURCES)
	if(_tgt_srcs MATCHES "TARGET_OBJECTS:Logger")
		continue()
	endif()
	rendering_test_link_stb(${_tgt})
endforeach()
unset(_GE_RENDERING_DIR_TARGETS)

# Same sweep idea for output layout + runtime-DLL staging: every test executable
# in this directory (name ending in Test/Tests — excludes examples and the
# ShaderReflect CLI tool) lands in bin/<Config>/Tests and gets its runtime DLL
# closure (vcpkg gtest is a SHARED lib, plus shaderc etc.) staged beside the exe
# by ge_stage_runtime_dependencies, so each binary runs standalone from its own
# directory — no PATH help, no reliance on sibling targets having staged the
# DLLs first. Without this, exes default to the bin/<Config> ROOT where nothing
# stages gtest.dll and the loader fails with 0xC0000135. Not gated on
# BUILD_TESTING: gtest is a vcpkg default feature, so the GTest_FOUND-guarded
# test targets in this directory are defined even when BUILD_TESTING is OFF
# and must still land in Tests/ with their DLL closure.
if(COMMAND ge_set_output_tests)
	get_property(_GE_RENDERING_DIR_TARGETS DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
	foreach(_tgt IN LISTS _GE_RENDERING_DIR_TARGETS)
		get_target_property(_tgt_type ${_tgt} TYPE)
		if(NOT _tgt_type STREQUAL "EXECUTABLE" OR NOT _tgt MATCHES "Tests?$")
			continue()
		endif()
		ge_set_output_tests(${_tgt})
		ge_stage_runtime_dependencies(${_tgt})
	endforeach()
	unset(_GE_RENDERING_DIR_TARGETS)

	# ShaderGraphCompilerTests reflects the engine node library off disk; the
	# staging shares one recipe per output directory, so it needs the layout above.
	if(COMMAND ge_stage_shader_graph_nodes AND TARGET ShaderGraphCompilerTests)
		ge_stage_shader_graph_nodes(ShaderGraphCompilerTests)
	endif()
endif()

# WebGPU backend bring-up smoke (headless device, buffers, textures). Lives
# here rather than Engine/CMakeLists.txt because RENDERING_HAS_WEBGPU is
# resolved in this directory, which the root processes AFTER Engine/; the
# Engine target already exists at this point.
if(RENDERING_HAS_WEBGPU AND BUILD_TESTING AND TARGET Engine)
    find_package(GTest CONFIG QUIET)
    if(GTest_FOUND OR TARGET GTest::gtest)
        add_executable(WebGpuDeviceSmokeTests
            ${CMAKE_SOURCE_DIR}/Engine/Tests/WebGpuDeviceSmokeTests.cpp)
        target_link_libraries(WebGpuDeviceSmokeTests PRIVATE Engine GTest::gtest GTest::gtest_main)
        set_target_properties(WebGpuDeviceSmokeTests PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED ON
            FOLDER "Engine/Tests"
        )
        ge_set_output_tests(WebGpuDeviceSmokeTests)
        ge_stage_runtime_dependencies(WebGpuDeviceSmokeTests)
        add_test(NAME WebGpuDeviceSmokeTests COMMAND $<TARGET_FILE:WebGpuDeviceSmokeTests>)
    endif()
endif()

# Shader-cook golden snapshots: cooks one shader per remediation class and
# byte-compares the WGSL against Tools/ShaderCook/golden/. Needs only host
# tools (glslc + the vendored naga/tint), no build products. Reports SKIPPED
# (exit 125) when naga has not been fetched (Tools/ShaderCook/toolchain/setup.py).
find_package(Python3 COMPONENTS Interpreter QUIET)
if(BUILD_TESTING AND GLSLC_EXECUTABLE AND Python3_Interpreter_FOUND)
    add_test(NAME ShaderCookGoldenTests
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Tools/ShaderCook/run_golden_tests.py)
    set_tests_properties(ShaderCookGoldenTests PROPERTIES SKIP_RETURN_CODE 125)
endif()
