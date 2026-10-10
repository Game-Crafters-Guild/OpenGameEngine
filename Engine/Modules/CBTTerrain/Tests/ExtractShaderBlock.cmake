# Extracts a marked block out of a GLSL source into a C++ header, so that the tests compile the
# shader's own source rather than a copy of it. A copy can drift silently and a presence check only
# proves the text is there; the extracted block is the code the tests execute, so any edit to the
# shader is an edit to what they measure.
#
# Invoked by the build as:
#   cmake -DSRC=<shader.glsl> -DOUT=<header> -DBEGIN_MARKER=<text> -DEND_MARKER=<text>
#         -P ExtractShaderBlock.cmake
#
# A missing or reordered marker is a hard error: silently extracting nothing would leave the suite
# passing with no shader in it.

if(NOT DEFINED SRC OR NOT DEFINED OUT)
    message(FATAL_ERROR "ExtractShaderBlock: SRC and OUT are required")
endif()
if(NOT DEFINED BEGIN_MARKER OR NOT DEFINED END_MARKER)
    message(FATAL_ERROR "ExtractShaderBlock: BEGIN_MARKER and END_MARKER are required")
endif()

file(READ "${SRC}" _src)
string(FIND "${_src}" "${BEGIN_MARKER}" _begin)
string(FIND "${_src}" "${END_MARKER}" _end)
if(_begin EQUAL -1)
    message(FATAL_ERROR "ExtractShaderBlock: '${BEGIN_MARKER}' not found in ${SRC}")
endif()
if(_end EQUAL -1)
    message(FATAL_ERROR "ExtractShaderBlock: '${END_MARKER}' not found in ${SRC}")
endif()
if(NOT _end GREATER _begin)
    message(FATAL_ERROR "ExtractShaderBlock: markers are out of order in ${SRC}")
endif()

string(LENGTH "${BEGIN_MARKER}" _beginLength)
math(EXPR _from "${_begin} + ${_beginLength}")
math(EXPR _length "${_end} - ${_from}")
string(SUBSTRING "${_src}" ${_from} ${_length} _block)

get_filename_component(_srcName "${SRC}" NAME)

# Written through copy_if_different so an unchanged shader does not retrigger the compile.
file(WRITE "${OUT}.tmp"
     "// Extracted from ${_srcName} by ExtractShaderBlock.cmake. Edit the shader, not this file.\n${_block}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${OUT}.tmp" "${OUT}")
file(REMOVE "${OUT}.tmp")
