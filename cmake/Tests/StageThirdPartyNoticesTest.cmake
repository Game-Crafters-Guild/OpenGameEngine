# Standalone notice-staging contract, without compiling or starting an application.
if(NOT DEFINED SCRIPT OR NOT DEFINED SOURCE OR NOT DEFINED WORK)
    message(FATAL_ERROR "StageThirdPartyNoticesTest: SCRIPT, SOURCE and WORK are required")
endif()
set(_share "${WORK}/installed/test-triplet/share")
set(_out "${WORK}/out")
foreach(_port IN ITEMS ez-tree-upstream diharaw-volumetric-fog-upstream
        sanielx-height-fog-upstream mirzabeig-gpu-fog-particles-upstream
        crest-ocean-upstream godot-visual-shader-upstream renderdoc-app-api-upstream
        transitive-only-port)
    file(WRITE "${_share}/${_port}/copyright" "Copyright ${_port}\nPermission text; keep this complete.\n```nested fence```\n")
endforeach()
file(WRITE "${_share}/transitive-only-port/LICENSES/nested/part.txt" "Sidecar permission text; retained verbatim.\n")
file(WRITE "${_share}/ez-tree-upstream/EZTREE_UPSTREAM.json" "{}\n")
file(WRITE "${_share}/renderdoc-app-api-upstream/RENDERDOC_APP_API_UPSTREAM.json" "{}\n")
# Remove only the outputs asserted below so a previous successful run cannot mask an omission.
file(REMOVE "${_out}/vcpkg/transitive-only-port/copyright"
            "${_out}/vcpkg/transitive-only-port/LICENSES/nested/part.txt"
            "${_out}/THIRD_PARTY_NOTICES.md")
execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DGE_SOURCE_DIR=${SOURCE}" "-DGE_NOTICES_DEST=${_out}"
    "-DGE_VCPKG_INSTALLED_DIR=${WORK}/installed" "-DGE_VCPKG_TARGET_TRIPLET=test-triplet"
    -P "${SCRIPT}" COMMAND_ERROR_IS_FATAL ANY)

function(expect_copy source destination)
    if(NOT EXISTS "${destination}")
        message(FATAL_ERROR "notice missing from output: ${destination}")
    endif()
    file(SHA256 "${source}" _source_hash)
    file(SHA256 "${destination}" _destination_hash)
    if(NOT _source_hash STREQUAL _destination_hash)
        message(FATAL_ERROR "notice text changed: ${destination}")
    endif()
    file(READ "${source}" _text)
    file(READ "${_out}/THIRD_PARTY_NOTICES.md" _book)
    string(FIND "${_book}" "${_text}" _position)
    if(_position EQUAL -1)
        message(FATAL_ERROR "aggregate notice omits full text: ${source}")
    endif()
endfunction()

expect_copy("${_share}/transitive-only-port/copyright"
            "${_out}/vcpkg/transitive-only-port/copyright")
expect_copy("${_share}/transitive-only-port/LICENSES/nested/part.txt"
            "${_out}/vcpkg/transitive-only-port/LICENSES/nested/part.txt")
expect_copy("${SOURCE}/ThirdParty/BakingLabAces/LICENSE.txt" "${_out}/BakingLab-ACES/LICENSE.txt")
expect_copy("${SOURCE}/ThirdParty/KhronosPbrNeutral/LICENSE.txt" "${_out}/Khronos-PBR-Neutral/LICENSE.txt")
expect_copy("${SOURCE}/ThirdParty/MinimalAgX/LICENSE.txt" "${_out}/Minimal-AgX/LICENSE.txt")
expect_copy("${SOURCE}/ThirdParty/OpenColorIO/LICENSE.txt" "${_out}/OpenColorIO/LICENSE.txt")
expect_copy("${SOURCE}/ThirdParty/SpeedballGi/LICENSE.md" "${_out}/Speedball-GI/LICENSE.md")
expect_copy("${SOURCE}/Engine/Source/ThirdParty/miniz/LICENSE" "${_out}/miniz/LICENSE")
expect_copy("${SOURCE}/Engine/Modules/VolumetricClouds/ThirdParty/SebLague-Clouds/LICENSE"
            "${_out}/SebLague-Clouds/LICENSE")
expect_copy("${SOURCE}/ThirdParty/GT7ToneMapper/LICENSE.txt" "${_out}/GT7-Tone-Mapper/LICENSE.txt")
expect_copy("${SOURCE}/ThirdParty/GTAO/LICENSE.txt" "${_out}/GTAO/LICENSE.txt")
file(READ "${_out}/THIRD_PARTY_NOTICES.md" _book)
string(FIND "${_book}" "````\nCopyright transitive-only-port" _fenced)
if(_fenced EQUAL -1)
    message(FATAL_ERROR "aggregate notice does not protect nested Markdown fences")
endif()
message(STATUS "StageThirdPartyNoticesTest: passed")
