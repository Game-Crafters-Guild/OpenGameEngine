# The compile defines an engine package's native module is built with, derived
# from the package manifests exactly as the runtime derives them — the shipped
# prebuilt's engine_abi marker is a hash over this list, and the editor compiles
# the same module with the list it derives itself, so the two must agree
# character for character AND in order (HashEngineAbiInputs chains them).
#
# The two rules mirrored here, both in Engine/:
#   - PackageDefine (Assets/Packages/PackageResolver.cpp): "GE_PACKAGE_" + the
#     sanitized mount alias ('@' dropped, '/' '.' '_' written '-') upper-cased
#     with '-' written '_'.
#   - ComputeDerived (Assets/Packages/PackageCodeModules.cpp): a package's
#     EFFECTIVE defines are every dependency's effective defines first, in
#     dependency-name order and appended only when not already present, then the
#     package's own define last.
#
# A package's identity is its MANIFEST name, never its directory name: the
# resolver indexes <engine packages root>/*/package.json by `name`, and a
# directory whose manifest renames the package would otherwise derive a define
# no runtime ever defines.

# ge_package_define(<package manifest name> <out var>)
function(ge_package_define package_name out_var)
    string(REPLACE "@" "" _alias "${package_name}")
    string(REGEX REPLACE "[/._]" "-" _alias "${_alias}")
    string(REPLACE "-" "_" _define "${_alias}")
    string(TOUPPER "${_define}" _define)
    set(${out_var} "GE_PACKAGE_${_define}" PARENT_SCOPE)
endfunction()

# The directory under <root> whose manifest declares <name>, or empty.
function(_ge_engine_package_dir root name out_var)
    file(GLOB _manifests "${root}/*/package.json")
    foreach(_manifest IN LISTS _manifests)
        file(READ "${_manifest}" _json)
        string(JSON _candidate ERROR_VARIABLE _error GET "${_json}" name)
        if(_error STREQUAL "NOTFOUND" AND _candidate STREQUAL "${name}")
            get_filename_component(_dir "${_manifest}" DIRECTORY)
            set(${out_var} "${_dir}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

# The dependency names a manifest declares, in the order the resolver visits
# them: `dependencies` is a std::map there, so the visit order is sorted by name
# rather than the order the JSON document happens to list them in. CMake's JSON
# reader also returns members sorted; the sort states the order this derivation
# depends on instead of inheriting it from the reader.
function(_ge_engine_package_dependencies json package_name out_var)
    set(_dependencies "")
    string(JSON _object ERROR_VARIABLE _error GET "${json}" dependencies)
    if(NOT _error STREQUAL "NOTFOUND")
        set(${out_var} "" PARENT_SCOPE)
        return() # no `dependencies` member
    endif()
    string(JSON _count ERROR_VARIABLE _error LENGTH "${_object}")
    if(NOT _error STREQUAL "NOTFOUND")
        message(FATAL_ERROR "engine package '${package_name}': 'dependencies' is not an object")
    endif()
    if(_count GREATER 0)
        math(EXPR _last "${_count} - 1")
        foreach(_index RANGE 0 ${_last})
            string(JSON _name MEMBER "${_object}" ${_index})
            list(APPEND _dependencies "${_name}")
        endforeach()
        list(SORT _dependencies)
    endif()
    set(${out_var} "${_dependencies}" PARENT_SCOPE)
endfunction()

# The effective defines of the engine package whose manifest declares <name>.
# Recursive, like the transitive closure it reproduces; a dependency cycle (which
# the resolver rejects) exhausts CMake's recursion limit and fails the configure.
function(_ge_engine_package_effective_defines root package_name out_var)
    _ge_engine_package_dir("${root}" "${package_name}" _dir)
    if(NOT _dir)
        message(FATAL_ERROR
            "engine package dependency '${package_name}' has no manifest under '${root}' — a "
            "package that ships with the engine can only depend on packages that ship with it")
    endif()
    file(READ "${_dir}/package.json" _json)
    _ge_engine_package_dependencies("${_json}" "${package_name}" _dependencies)

    set(_defines "")
    foreach(_dependency IN LISTS _dependencies)
        _ge_engine_package_effective_defines("${root}" "${_dependency}" _dependency_defines)
        foreach(_define IN LISTS _dependency_defines)
            if(NOT "${_define}" IN_LIST _defines)
                list(APPEND _defines "${_define}")
            endif()
        endforeach()
    endforeach()
    ge_package_define("${package_name}" _own)
    if(NOT "${_own}" IN_LIST _defines)
        list(APPEND _defines "${_own}")
    endif()
    set(${out_var} "${_defines}" PARENT_SCOPE)
endfunction()

# ge_engine_package_module_defines(<engine packages root> <package dir> <out var>)
# The effective compile defines of every native module the package in
# <root>/<package dir> delivers.
function(ge_engine_package_module_defines root package_dir out_var)
    set(_manifest "${root}/${package_dir}/package.json")
    if(NOT EXISTS "${_manifest}")
        message(FATAL_ERROR "ge_engine_package_module_defines: no package manifest at '${_manifest}'")
    endif()
    file(READ "${_manifest}" _json)
    string(JSON _name ERROR_VARIABLE _error GET "${_json}" name)
    if(NOT _error STREQUAL "NOTFOUND")
        message(FATAL_ERROR "ge_engine_package_module_defines: '${_manifest}' declares no 'name'")
    endif()
    _ge_engine_package_effective_defines("${root}" "${_name}" _defines)
    set(${out_var} "${_defines}" PARENT_SCOPE)
endfunction()
