#
# WriteMoltenVkIcdJson.cmake
#
# Writes a MoltenVK ICD manifest suitable for app-bundle staging.
#
# Required:
#   -DOUTPUT=/path/to/MoltenVK_icd.json
#
# Optional:
#   -DINPUT=/path/to/original/MoltenVK_icd.json
#
# The LunarG Vulkan SDK's MoltenVK_icd.json typically points at a library path like:
#   "../../../lib/libMoltenVK.dylib"
# which is correct *inside the SDK install*, but not inside our Editor.app bundle.
#
# We stage libMoltenVK.dylib next to this json (Contents/Resources/vulkan/icd.d).
#

if(NOT DEFINED OUTPUT OR OUTPUT STREQUAL "")
  message(FATAL_ERROR "WriteMoltenVkIcdJson: OUTPUT must be set to the target MoltenVK_icd.json path")
endif()

get_filename_component(_out_dir "${OUTPUT}" DIRECTORY)
# Relative to the manifest's own directory so the bundle is relocatable: the
# Vulkan loader resolves a relative library_path against the dir containing the
# ICD json, and libMoltenVK.dylib is staged right next to it. An absolute build-
# machine path here would break the moment the .app is moved or copied to another
# Mac (which is exactly what a shipped game bundle does).
set(_desired_library_path "./libMoltenVK.dylib")

set(_content "")
if(DEFINED INPUT AND NOT INPUT STREQUAL "" AND EXISTS "${INPUT}")
  file(READ "${INPUT}" _content)
else()
  # Fallback minimal manifest (should only be used when INPUT is not available).
  set(_content [=[
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "./libMoltenVK.dylib",
        "api_version": "1.0.0",
        "is_portability_driver": true
    }
}
]=])
endif()

# Patch library_path to point at the dylib staged alongside this manifest.
string(REGEX REPLACE "\"library_path\"[ \t]*:[ \t]*\"[^\"]*\"" "\"library_path\": \"${_desired_library_path}\"" _content "${_content}")

file(MAKE_DIRECTORY "${_out_dir}")

file(WRITE "${OUTPUT}" "${_content}\n")
