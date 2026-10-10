# Contract test for cmake/GenerateEngineExportsDef.cmake, run by ctest as
#   cmake -DSCRIPT=<generator script> -DWORK=<scratch dir> -P <this file>
# Engine.dll's link reads the generated definition through /DEF:, and the
# Visual Studio link tracker relinks the DLL whenever that input is newer than
# the DLL. A run that produces the definition already on disk must therefore
# leave the file untouched, and a run that produces a different one must
# replace it. A run none of whose inputs changed since the last one skips the
# generation, the whole cost of a build that compiled nothing. The scratch
# object set is empty: the contract is about the output file, not the symbol
# filters, which every Engine.dll link exercises.
if(NOT DEFINED SCRIPT OR NOT DEFINED WORK)
    message(FATAL_ERROR "GenerateEngineExportsDefTest.cmake: SCRIPT and WORK are required")
endif()

set(_objects "${WORK}/objects")
set(_module_objects "${WORK}/module-objects.txt")
set(_def "${WORK}/exports.def")

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${_objects}")
file(WRITE "${_module_objects}" "")

# generate(<output var>): one run of the script; its report lands in <output var>.
function(generate out)
    execute_process(
        COMMAND ${CMAKE_COMMAND}
                -DENGINE_OBJ_DIR=${_objects}
                -DMODULE_OBJECTS_FILE=${_module_objects}
                -DOUTPUT_DEF=${_def}
                -P ${SCRIPT}
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err
        COMMAND_ERROR_IS_FATAL ANY)
    if(EXISTS "${_def}.staged")
        message(FATAL_ERROR "the staged definition was left beside exports.def")
    endif()
    set(${out} "${_out}${_err}" PARENT_SCOPE)
endfunction()

function(expect_reported report state)
    if(NOT report MATCHES "exports.def ${state} ")
        message(FATAL_ERROR "expected the run to report exports.def ${state}, it printed:\n${report}")
    endif()
endfunction()

function(def_timestamp out)
    file(TIMESTAMP "${_def}" _stamp "%Y-%m-%dT%H:%M:%S.%f")
    set(${out} "${_stamp}" PARENT_SCOPE)
endfunction()

# The first run writes the definition.
generate(_report)
if(NOT EXISTS "${_def}")
    message(FATAL_ERROR "the first run did not write exports.def")
endif()
file(READ "${_def}" _generated)
def_timestamp(_first)

# A run with no changed input leaves the file, and so the link, alone, and
# skips the generation.
generate(_report)
def_timestamp(_second)
if(NOT _second STREQUAL _first)
    message(FATAL_ERROR "an unchanged definition was rewritten (${_first} -> ${_second}); Engine.dll would relink")
endif()
expect_reported("${_report}" "up to date")

# A changed input regenerates, and an identical definition still keeps its timestamp.
file(TOUCH "${_module_objects}")
generate(_report)
def_timestamp(_third)
if(NOT _third STREQUAL _first)
    message(FATAL_ERROR "a regenerated identical definition was rewritten (${_first} -> ${_third}); Engine.dll would relink")
endif()
expect_reported("${_report}" unchanged)

# A different object set regenerates even when no input is newer than the record:
# the record written here is the newest file, so only the set comparison can see it.
file(WRITE "${_def}.inputs" "C:\\stale\\object.obj\n")
generate(_report)
expect_reported("${_report}" unchanged)

# A definition that differs from the generated one is replaced once an input changes.
file(WRITE "${_def}" "EXPORTS\n    StaleExport\n")
file(TOUCH "${_module_objects}")
generate(_report)
file(READ "${_def}" _replaced)
if(NOT _replaced STREQUAL _generated)
    message(FATAL_ERROR "a stale definition was not replaced; exports.def holds:\n${_replaced}")
endif()
expect_reported("${_report}" updated)
