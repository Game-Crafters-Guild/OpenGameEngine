if(NOT DEFINED GE_SOURCE_DIR OR GE_SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR "StageThirdPartyNotices: GE_SOURCE_DIR is required")
endif()
if(NOT DEFINED GE_NOTICES_DEST OR GE_NOTICES_DEST STREQUAL "")
    message(FATAL_ERROR "StageThirdPartyNotices: GE_NOTICES_DEST is required")
endif()
if(NOT DEFINED GE_VCPKG_INSTALLED_DIR OR GE_VCPKG_INSTALLED_DIR STREQUAL "")
    message(FATAL_ERROR "StageThirdPartyNotices: GE_VCPKG_INSTALLED_DIR is required")
endif()
if(NOT DEFINED GE_VCPKG_TARGET_TRIPLET OR GE_VCPKG_TARGET_TRIPLET STREQUAL "")
    message(FATAL_ERROR "StageThirdPartyNotices: GE_VCPKG_TARGET_TRIPLET is required")
endif()

set(_ge_vcpkg_share "${GE_VCPKG_INSTALLED_DIR}/${GE_VCPKG_TARGET_TRIPLET}/share")

function(_ge_notice_copy src dst)
    if(NOT EXISTS "${src}")
        message(FATAL_ERROR "StageThirdPartyNotices: missing notice source: ${src}")
    endif()
    get_filename_component(_dst_dir "${dst}" DIRECTORY)
    file(MAKE_DIRECTORY "${_dst_dir}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${src}" "${dst}"
        RESULT_VARIABLE _copy_result
    )
    if(NOT _copy_result EQUAL 0)
        message(FATAL_ERROR "StageThirdPartyNotices: failed to copy ${src} to ${dst}")
    endif()
    set_property(GLOBAL APPEND PROPERTY GE_STAGED_NOTICE_FILES "${dst}")
endfunction()

function(_ge_notice_copy_vcpkg package src_name dst_dir dst_name)
    _ge_notice_copy(
        "${_ge_vcpkg_share}/${package}/${src_name}"
        "${GE_NOTICES_DEST}/${dst_dir}/${dst_name}")
endfunction()

# Preserve complete installed notices, including sidecar LICENSES/ directories that
# a port's copyright file can reference (for example KTX).
file(GLOB_RECURSE _ge_port_files LIST_DIRECTORIES false "${_ge_vcpkg_share}/*")
list(SORT _ge_port_files)
foreach(_ge_port_file IN LISTS _ge_port_files)
    file(RELATIVE_PATH _ge_port_relative "${_ge_vcpkg_share}" "${_ge_port_file}")
    string(TOLOWER "${_ge_port_relative}" _ge_port_lower)
    if(_ge_port_lower MATCHES "(^|/)[^/]*(license|notice|copyright|copying)[^/]*(/|$)")
        _ge_notice_copy("${_ge_port_file}" "${GE_NOTICES_DEST}/vcpkg/${_ge_port_relative}")
    endif()
endforeach()

_ge_notice_copy_vcpkg("ez-tree-upstream" "copyright" "EZTree" "LICENSE")
_ge_notice_copy_vcpkg("ez-tree-upstream" "EZTREE_UPSTREAM.json" "EZTree" "EZTREE_UPSTREAM.json")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/Packages/eztree/Assets/Textures/EZTree/NOTICE.md"
    "${GE_NOTICES_DEST}/EZTree/NOTICE.md")

# The desktop WebGPU backend and SDK shader cook share the pinned web toolchain.
# Keep its full permission texts with the shipped notices, including Rust dependencies.
_ge_notice_copy(
    "${GE_SOURCE_DIR}/Tools/Web/licenses/toolchain-notices.md"
    "${GE_NOTICES_DEST}/WebToolchain/THIRD_PARTY_NOTICES.md")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/Tools/Web/licenses/rust-dependency-notices.md"
    "${GE_NOTICES_DEST}/WebToolchain/RUST_DEPENDENCY_NOTICES.md")

# Keep managed package permission texts and their exact source inventory together.
_ge_notice_copy(
    "${GE_SOURCE_DIR}/Managed/THIRD_PARTY_NOTICES.md"
    "${GE_NOTICES_DEST}/Managed/THIRD_PARTY_NOTICES.md")
file(GLOB_RECURSE _ge_managed_notices LIST_DIRECTORIES false
    "${GE_SOURCE_DIR}/Managed/licenses/*")
foreach(_ge_managed_notice IN LISTS _ge_managed_notices)
    file(RELATIVE_PATH _ge_managed_relative "${GE_SOURCE_DIR}/Managed" "${_ge_managed_notice}")
    _ge_notice_copy("${_ge_managed_notice}" "${GE_NOTICES_DEST}/Managed/${_ge_managed_relative}")
endforeach()

# Roboto (Apache 2.0) and Roboto Mono (OFL 1.1): the editor UI fonts, the
# Player's default fonts and the default game UI font every export ships.
_ge_notice_copy(
    "${GE_SOURCE_DIR}/Apps/Editor/Assets/Fonts/LICENSE.txt"
    "${GE_NOTICES_DEST}/Roboto/LICENSE.txt")

_ge_notice_copy_vcpkg("diharaw-volumetric-fog-upstream" "copyright" "VolumetricFog" "LICENSE")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/Engine/Modules/VolumetricFog/ThirdParty/diharaw-volumetric-fog/UPSTREAM.md"
    "${GE_NOTICES_DEST}/VolumetricFog/UPSTREAM.md")

_ge_notice_copy_vcpkg("sanielx-height-fog-upstream" "copyright" "HeightFog" "LICENSE.md")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/Engine/Modules/HeightFog/ThirdParty/SanielX-Height-Fog/UPSTREAM.md"
    "${GE_NOTICES_DEST}/HeightFog/UPSTREAM.md")

_ge_notice_copy_vcpkg("mirzabeig-gpu-fog-particles-upstream" "copyright" "GPUFogParticles" "LICENSE.txt")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/Engine/Modules/GPUFogParticles/ThirdParty/MirzaBeig-GPU-Fog-Particles/UPSTREAM.md"
    "${GE_NOTICES_DEST}/GPUFogParticles/UPSTREAM.md")

_ge_notice_copy_vcpkg("crest-ocean-upstream" "copyright" "Ocean" "LICENSE")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/Engine/Modules/Ocean/NOTICE"
    "${GE_NOTICES_DEST}/Ocean/NOTICE")

_ge_notice_copy_vcpkg("godot-visual-shader-upstream" "copyright" "Godot" "LICENSE.txt")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/Engine/Modules/Graph/ThirdParty/visual-shader-upstream/UPSTREAM.md"
    "${GE_NOTICES_DEST}/Godot/UPSTREAM.md")

_ge_notice_copy_vcpkg("renderdoc-app-api-upstream" "copyright" "RenderDoc" "LICENSE.md")
_ge_notice_copy_vcpkg("renderdoc-app-api-upstream" "RENDERDOC_APP_API_UPSTREAM.json" "RenderDoc" "RENDERDOC_APP_API_UPSTREAM.json")

_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/metal-cpp/LICENSE.txt"
    "${GE_NOTICES_DEST}/metal-cpp/LICENSE.txt")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/metal-cpp/VENDORED.md"
    "${GE_NOTICES_DEST}/metal-cpp/VENDORED.md")

_ge_notice_copy(
    "${GE_SOURCE_DIR}/Engine/Source/ThirdParty/ufbx/LICENSE"
    "${GE_NOTICES_DEST}/ufbx/LICENSE")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/Engine/Source/ThirdParty/ufbx/UFBX_VERSION.txt"
    "${GE_NOTICES_DEST}/ufbx/UFBX_VERSION.txt")

_ge_notice_copy(
    "${GE_SOURCE_DIR}/Engine/Source/ThirdParty/fbtBlend/LICENSE"
    "${GE_NOTICES_DEST}/fbtBlend/LICENSE")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/Engine/Source/ThirdParty/fbtBlend/UPDATE-NOTES.md"
    "${GE_NOTICES_DEST}/fbtBlend/UPDATE-NOTES.md")

_ge_notice_copy(
    "${GE_SOURCE_DIR}/Packages/fidelityfx-dof/LICENSE.txt"
    "${GE_NOTICES_DEST}/AMD-FidelityFX-DoF/LICENSE.txt")

_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/SENaturalBloomDirtyLens/LICENSE.txt"
    "${GE_NOTICES_DEST}/SE-Natural-Bloom-Dirty-Lens/LICENSE.txt")

_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/KinoBloom/LICENSE.md"
    "${GE_NOTICES_DEST}/KinoBloom/LICENSE.md")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/KinoBloom/UPSTREAM.md"
    "${GE_NOTICES_DEST}/KinoBloom/UPSTREAM.md")

_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/OCASMDepthVeil/LICENSE"
    "${GE_NOTICES_DEST}/OCASM-Depth-Veil/LICENSE")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/OCASMDepthVeil/UPSTREAM.md"
    "${GE_NOTICES_DEST}/OCASM-Depth-Veil/UPSTREAM.md")

_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/PunikontaVhs/LICENSE.md"
    "${GE_NOTICES_DEST}/Punikonta-VHS/LICENSE.md")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/PunikontaVhs/UPSTREAM.md"
    "${GE_NOTICES_DEST}/Punikonta-VHS/UPSTREAM.md")

_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/FidelityFX-Denoiser/LICENSE.txt"
    "${GE_NOTICES_DEST}/AMD-FidelityFX-Denoiser/LICENSE.txt")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/FidelityFX-Denoiser/UPSTREAM.md"
    "${GE_NOTICES_DEST}/AMD-FidelityFX-Denoiser/UPSTREAM.md")

_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/RunevisionErosionFilter/LICENSE-MPL-2.0.txt"
    "${GE_NOTICES_DEST}/Runevision-Erosion-Filter/LICENSE-MPL-2.0.txt")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/RunevisionErosionFilter/UPSTREAM.md"
    "${GE_NOTICES_DEST}/Runevision-Erosion-Filter/UPSTREAM.md")

_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/BendScreenSpaceShadows/LICENSE.txt"
    "${GE_NOTICES_DEST}/Bend-Screen-Space-Shadows/LICENSE.txt")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/BendScreenSpaceShadows/UPSTREAM.md"
    "${GE_NOTICES_DEST}/Bend-Screen-Space-Shadows/UPSTREAM.md")

# Vendored notices whose code or compiled shaders can enter the desktop/SDK output.
_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/BakingLabAces/LICENSE.txt"
    "${GE_NOTICES_DEST}/BakingLab-ACES/LICENSE.txt")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/BakingLabAces/UPSTREAM.md"
    "${GE_NOTICES_DEST}/BakingLab-ACES/UPSTREAM.md")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/KhronosPbrNeutral/LICENSE.txt"
    "${GE_NOTICES_DEST}/Khronos-PBR-Neutral/LICENSE.txt")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/KhronosPbrNeutral/UPSTREAM.md"
    "${GE_NOTICES_DEST}/Khronos-PBR-Neutral/UPSTREAM.md")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/MinimalAgX/LICENSE.txt"
    "${GE_NOTICES_DEST}/Minimal-AgX/LICENSE.txt")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/MinimalAgX/UPSTREAM.md"
    "${GE_NOTICES_DEST}/Minimal-AgX/UPSTREAM.md")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/OpenColorIO/LICENSE.txt"
    "${GE_NOTICES_DEST}/OpenColorIO/LICENSE.txt")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/OpenColorIO/UPSTREAM.md"
    "${GE_NOTICES_DEST}/OpenColorIO/UPSTREAM.md")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/SpeedballGi/LICENSE.md"
    "${GE_NOTICES_DEST}/Speedball-GI/LICENSE.md")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/SpeedballGi/UPSTREAM.md"
    "${GE_NOTICES_DEST}/Speedball-GI/UPSTREAM.md")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/Engine/Source/ThirdParty/miniz/LICENSE"
    "${GE_NOTICES_DEST}/miniz/LICENSE")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/Engine/Source/ThirdParty/miniz/VENDORED.md"
    "${GE_NOTICES_DEST}/miniz/VENDORED.md")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/Engine/Modules/VolumetricClouds/ThirdParty/SebLague-Clouds/LICENSE"
    "${GE_NOTICES_DEST}/SebLague-Clouds/LICENSE")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/Engine/Modules/VolumetricClouds/ThirdParty/SebLague-Clouds/UPSTREAM.md"
    "${GE_NOTICES_DEST}/SebLague-Clouds/UPSTREAM.md")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/GT7ToneMapper/LICENSE.txt"
    "${GE_NOTICES_DEST}/GT7-Tone-Mapper/LICENSE.txt")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/GT7ToneMapper/UPSTREAM.md"
    "${GE_NOTICES_DEST}/GT7-Tone-Mapper/UPSTREAM.md")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/GTAO/LICENSE.txt"
    "${GE_NOTICES_DEST}/GTAO/LICENSE.txt")
_ge_notice_copy(
    "${GE_SOURCE_DIR}/ThirdParty/GTAO/UPSTREAM.md"
    "${GE_NOTICES_DEST}/GTAO/UPSTREAM.md")

# The aggregate is plain text: NUL and the other C0 control bytes except tab, LF and
# CR are dropped, and an RTF notice is reduced to the text it displays.
string(ASCII 1 2 3 4 5 6 7 8 11 12 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31
    _ge_control_bytes)

# CMake regular expressions stop at the first NUL, so "^.*" matches exactly the bytes
# before it; LENGTH and SUBSTRING see the whole string.
function(_ge_notice_drop_nul text_var)
    set(_text "${${text_var}}")
    while(TRUE)
        string(REGEX MATCH "^.*" _head "${_text}")
        string(LENGTH "${_head}" _head_length)
        string(LENGTH "${_text}" _text_length)
        if(_head_length EQUAL _text_length)
            break()
        endif()
        string(SUBSTRING "${_text}" ${_head_length} 1 _stop)
        if(_stop MATCHES "^.")
            message(FATAL_ERROR "StageThirdPartyNotices: notice text stopped matching at a byte that is not NUL")
        endif()
        math(EXPR _tail_start "${_head_length} + 1")
        string(SUBSTRING "${_text}" ${_tail_start} -1 _tail)
        set(_text "${_head}${_tail}")
    endwhile()
    set(${text_var} "${_text}" PARENT_SCOPE)
endfunction()

# Covers the RTF that rich-edit controls write: destination groups, control words,
# \par, \line and \tab, and escaped braces and backslashes. Character escapes (\' and
# \u) are refused rather than guessed.
function(_ge_notice_rtf_to_text text_var path)
    set(_rtf "${${text_var}}")
    if(_rtf MATCHES "\\\\'|\\\\u-?[0-9]")
        message(FATAL_ERROR "StageThirdPartyNotices: ${path} uses RTF character escapes the aggregate cannot decode; commit a plain-text copy of the notice and stage that instead")
    endif()
    string(ASCII 1 _backslash)
    string(ASCII 2 _open)
    string(ASCII 3 _close)
    # Ends a control word where an unwrapped group began or ended.
    string(ASCII 4 _group_edge)
    string(REPLACE "\\\\" "${_backslash}" _rtf "${_rtf}")
    string(REPLACE "\\{" "${_open}" _rtf "${_rtf}")
    string(REPLACE "\\}" "${_close}" _rtf "${_rtf}")
    string(REGEX REPLACE "[\r\n]" "" _rtf "${_rtf}")
    set(_previous "")
    while(NOT _rtf STREQUAL _previous)
        set(_previous "${_rtf}")
        string(REGEX REPLACE "{\\\\\\*[^{}]*}" "" _rtf "${_rtf}")
        string(REGEX REPLACE "{\\\\(fonttbl|colortbl|stylesheet|info)[^{}]*}" "" _rtf "${_rtf}")
        string(REGEX REPLACE "{\\\\f[0-9]+[^{}]*}" "" _rtf "${_rtf}")
        string(REGEX REPLACE "{([^{}]*)}" "${_group_edge}\\1${_group_edge}" _rtf "${_rtf}")
    endwhile()
    # Longer control words that begin with par, line or tab (\pard, \pararsid) go first.
    string(REGEX REPLACE "\\\\(par|line|tab)[a-z]+(-?[0-9]+)? ?" "" _rtf "${_rtf}")
    string(REGEX REPLACE "\\\\(par|line) ?" "\n" _rtf "${_rtf}")
    string(REGEX REPLACE "\\\\tab ?" "\t" _rtf "${_rtf}")
    string(REGEX REPLACE "\\\\[a-z]+(-?[0-9]+)? ?" "" _rtf "${_rtf}")
    string(REPLACE "${_group_edge}" "" _rtf "${_rtf}")
    string(REPLACE "\\~" " " _rtf "${_rtf}")
    string(REPLACE "\\-" "" _rtf "${_rtf}")
    string(REPLACE "\\_" "-" _rtf "${_rtf}")
    string(REPLACE "${_backslash}" "\\" _rtf "${_rtf}")
    string(REPLACE "${_open}" "{" _rtf "${_rtf}")
    string(REPLACE "${_close}" "}" _rtf "${_rtf}")
    set(${text_var} "${_rtf}" PARENT_SCOPE)
endfunction()

# One readable aggregate travels with the existing per-component notice files.
get_property(_ge_notice_files GLOBAL PROPERTY GE_STAGED_NOTICE_FILES)
list(REMOVE_DUPLICATES _ge_notice_files)
list(SORT _ge_notice_files)
set(_ge_notice_book "${GE_NOTICES_DEST}/THIRD_PARTY_NOTICES.md")
file(WRITE "${_ge_notice_book}" "# Third-party notices\n\nThese texts cover the installed dependency triplet and bundled source components. The triplet can include build-only and optional ports; inclusion does not assert that each port is linked into every executable. Component source records state their versions and scope.\n\n")
foreach(_ge_notice_file IN LISTS _ge_notice_files)
    file(RELATIVE_PATH _ge_relative_notice "${GE_NOTICES_DEST}" "${_ge_notice_file}")
    file(READ "${_ge_notice_file}" _ge_notice_text)
    _ge_notice_drop_nul(_ge_notice_text)
    string(REGEX REPLACE "[${_ge_control_bytes}]" "" _ge_notice_text "${_ge_notice_text}")
    get_filename_component(_ge_notice_extension "${_ge_notice_file}" LAST_EXT)
    string(TOLOWER "${_ge_notice_extension}" _ge_notice_extension)
    if(_ge_notice_extension STREQUAL ".rtf")
        _ge_notice_rtf_to_text(_ge_notice_text "${_ge_notice_file}")
        string(APPEND _ge_relative_notice " (text of the RTF document)")
    endif()
    set(_ge_fence_length 3)
    string(REGEX MATCHALL "`+" _ge_backtick_runs "${_ge_notice_text}")
    foreach(_ge_run IN LISTS _ge_backtick_runs)
        string(LENGTH "${_ge_run}" _ge_run_length)
        if(_ge_run_length GREATER_EQUAL _ge_fence_length)
            math(EXPR _ge_fence_length "${_ge_run_length} + 1")
        endif()
    endforeach()
    string(REPEAT "`" ${_ge_fence_length} _ge_fence)
    file(APPEND "${_ge_notice_book}" "## ${_ge_relative_notice}\n\n${_ge_fence}\n${_ge_notice_text}\n${_ge_fence}\n\n")
endforeach()
