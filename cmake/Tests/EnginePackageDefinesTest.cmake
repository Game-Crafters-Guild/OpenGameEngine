# Contract test for cmake/EnginePackageDefines.cmake, run by ctest as
#   cmake -DSCRIPT=<derivation script> -DWORK=<scratch dir> -P <this file>
#
# The derivation decides the compile defines a shipped engine-package prebuilt
# is built and stamped with. The editor derives the same list in C++ and hashes
# it into the digest it checks the stamped engine_abi marker against, so a
# derivation that differs in ONE element or in ORDER makes every shipped
# prebuilt unusable and the first project open compiles each module from source.
#
# The C++ side of the same rule is gated by AssetSystemTests
# (Tests/Assets/PackageSystemTests.cpp, `PackageResolverDefines` and
# `PackageCodeModules`), which asserts these same names and this same
# dependencies-first order — so a change to one rule leaves the other's suite red.
#
# Covered: the define rule on the four names PackageResolver.h documents; a
# package with no dependencies; identity taken from the manifest name rather
# than the directory name; and the dependency order — every dependency's
# effective defines first, in dependency-NAME order rather than the order the
# manifest lists them, appended once, with the package's own define last. A
# dependency that ships with no manifest is refused loudly.
if(NOT DEFINED SCRIPT OR NOT DEFINED WORK)
    message(FATAL_ERROR "EnginePackageDefinesTest.cmake: SCRIPT and WORK are required")
endif()

include("${SCRIPT}")

set(_root "${WORK}/Packages")
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${_root}")

# declare_package(<dir> <manifest name> [<dependency name>...]) — the dependency
# names are written in the order given, so a test can list them unsorted.
function(declare_package dir name)
    set(_json "{\n  \"name\": \"${name}\",\n  \"version\": \"1.0.0\"")
    if(ARGN)
        set(_json "${_json},\n  \"dependencies\": {")
        set(_separator "")
        foreach(_dependency IN LISTS ARGN)
            set(_json "${_json}${_separator}\n    \"${_dependency}\": \"1.0.0\"")
            set(_separator ",")
        endforeach()
        set(_json "${_json}\n  }")
    endif()
    file(WRITE "${_root}/${dir}/package.json" "${_json}\n}\n")
endfunction()

# expect_define(<case> <package name> <expected define>)
function(expect_define case name expected)
    ge_package_define("${name}" _actual)
    if(NOT _actual STREQUAL "${expected}")
        message(FATAL_ERROR "${case}: '${name}' derived '${_actual}', expected '${expected}'")
    endif()
endfunction()

# expect_module_defines(<case> <package dir> <expected ;-list>)
function(expect_module_defines case dir expected)
    ge_engine_package_module_defines("${_root}" "${dir}" _actual)
    if(NOT "${_actual}" STREQUAL "${expected}")
        message(FATAL_ERROR "${case}: '${dir}' derived '${_actual}', expected '${expected}'")
    endif()
endfunction()

# --- the define rule, on the names PackageResolver.h documents ---------------

expect_define("a plain name" "ocean-pack" "GE_PACKAGE_OCEAN_PACK")
expect_define("a scoped, dotted name" "@studio/water.core" "GE_PACKAGE_STUDIO_WATER_CORE")
expect_define("a digit-leading name" "2d-tools" "GE_PACKAGE_2D_TOOLS")
expect_define("an underscored name" "grid_tools" "GE_PACKAGE_GRID_TOOLS")

# --- a package with no dependencies ------------------------------------------

declare_package(git-vcs "git-vcs")
expect_module_defines("a package with no dependencies" "git-vcs" "GE_PACKAGE_GIT_VCS")

# --- identity is the manifest name, not the directory name -------------------
# The resolver indexes engine packages by their manifest `name`, so a directory
# that disagrees with its manifest must still derive the runtime's define.

declare_package(water-checkout "@studio/water.core")
expect_module_defines("a directory name that disagrees with the manifest"
    "water-checkout" "GE_PACKAGE_STUDIO_WATER_CORE")

# --- dependency order --------------------------------------------------------
# alpha depends on delta and charlie (listed in that order); both depend on
# bravo. The resolver walks dependencies sorted by name and appends each one's
# effective defines once, transitively, before the package's own define.

declare_package(pkg-a "alpha" "delta" "charlie")
declare_package(pkg-c "charlie" "bravo")
declare_package(pkg-d "delta" "bravo")
declare_package(pkg-b "bravo")
expect_module_defines("a package with transitive dependencies" "pkg-a"
    "GE_PACKAGE_BRAVO;GE_PACKAGE_CHARLIE;GE_PACKAGE_DELTA;GE_PACKAGE_ALPHA")

# --- a dependency that ships with no manifest --------------------------------
# A refusal is a message(FATAL_ERROR), so this driver has to outlive it.

declare_package(pkg-orphan "orphan" "absent-package")
set(_driver "${WORK}/drive.cmake")
file(WRITE "${_driver}"
    "include(\"${SCRIPT}\")\n"
    "ge_engine_package_module_defines(\"${_root}\" \"pkg-orphan\" _defines)\n")
execute_process(COMMAND ${CMAKE_COMMAND} -P ${_driver}
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
string(REGEX REPLACE "[ \t\r\n]+" " " _text "${_out}${_err}")
if(_rc EQUAL 0)
    message(FATAL_ERROR "a dependency with no manifest: the derivation accepted it")
endif()
string(FIND "${_text}" "engine package dependency 'absent-package' has no manifest" _at)
if(_at LESS 0)
    message(FATAL_ERROR "a dependency with no manifest: the refusal does not name it.\n  Message: ${_text}")
endif()

file(REMOVE_RECURSE "${WORK}")
