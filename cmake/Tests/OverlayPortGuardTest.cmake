# Contract test for cmake/OverlayPortGuard.cmake, run by ctest as
#   cmake -DSCRIPT=<guard script> -DWORK=<scratch dir> -P <this file>
# Each case drives the guard in a child process, because a refusal is a
# message(FATAL_ERROR) and this driver has to outlive it to check the text.
#
# Covered: a port installed at the version cmake/ports/ declares passes; one
# installed older is refused, naming the port, both versions and the
# reconfigure that fixes it; a port whose status paragraph carries a semicolon
# is compared like any other, rather than reading as not installed; a port
# absent from the install root is reported as not checked, never as current;
# and on a macOS triplet a port in cmake/macos-ports/
# (GE_VCPKG_MACOS_OVERLAY_PORTS) is held to the same rule, named by its own
# directory, while off macOS that directory is not read; an install newer than
# that directory declares is accepted with a warning naming the directory.
if(NOT DEFINED SCRIPT OR NOT DEFINED WORK)
    message(FATAL_ERROR "OverlayPortGuardTest.cmake: SCRIPT and WORK are required")
endif()

set(_source "${WORK}/source")
set(_installed "${_source}/build/preset/vcpkg_installed")
set(_macosPorts "${_source}/cmake/macos-ports")

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${_installed}/vcpkg")

# The guard is a function over directory-scope variables, so the child process
# needs a driver that sets them and calls it. write_driver() sets the triplet
# (also the one install_port() records) and GE_VCPKG_MACOS_OVERLAY_PORTS the
# way Dependencies.cmake does: cmake/macos-ports/ for *-osx triplets, empty
# otherwise.
set(_driver "${WORK}/drive.cmake")
function(write_driver triplet)
    set(_macosDir "")
    if(triplet MATCHES "-osx")
        set(_macosDir "${_macosPorts}")
    endif()
    file(WRITE "${_driver}"
        "set(CMAKE_CURRENT_SOURCE_DIR \"${_source}\")\n"
        "set(CMAKE_BINARY_DIR \"${_source}/build/preset\")\n"
        "set(VCPKG_INSTALLED_DIR \"${_installed}\")\n"
        "set(VCPKG_TARGET_TRIPLET \"${triplet}\")\n"
        "set(GE_VCPKG_MACOS_OVERLAY_PORTS \"${_macosDir}\")\n"
        "include(\"${SCRIPT}\")\n"
        "ge_check_overlay_port_freshness()\n")
    set(_triplet "${triplet}" PARENT_SCOPE)
endfunction()
write_driver(x64-windows)

# declare_port(<name> <version> <port-version> [<overlay dir>]) — writes
# <overlay dir>/<name>/vcpkg.json, cmake/ports/ by default.
function(declare_port name version portVersion)
    set(_overlayDir "${_source}/cmake/ports")
    if(ARGC GREATER 3)
        set(_overlayDir "${ARGV3}")
    endif()
    file(WRITE "${_overlayDir}/${name}/vcpkg.json"
        "{\n  \"name\": \"${name}\",\n  \"version\": \"${version}\",\n"
        "  \"port-version\": ${portVersion}\n}\n")
endfunction()

# install_port(<name> <version> <port-version> <description>) — appends one
# status paragraph. The description is written through file(APPEND) rather than
# a list, so a semicolon in it survives into the file.
function(install_port name version portVersion description)
    file(APPEND "${_installed}/vcpkg/status"
        "Package: ${name}\nVersion: ${version}\nPort-Version: ${portVersion}\n"
        "Description: ${description}\nArchitecture: ${_triplet}\nMulti-Arch: same\n"
        "Status: install ok installed\n\n")
endfunction()

function(reset_install_root)
    file(REMOVE "${_installed}/vcpkg/status")
    file(WRITE "${_installed}/vcpkg/status" "")
endfunction()

# run_guard(<out rc> <out normalized output>)
function(run_guard rcVar textVar)
    execute_process(COMMAND ${CMAKE_COMMAND} -P ${_driver}
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    # CMake indents and may break the lines of a message(); collapse all
    # whitespace so an assertion on the text cannot fail on formatting.
    string(REGEX REPLACE "[ \t\r\n]+" " " _text "${_out}${_err}")
    set(${rcVar} "${_rc}" PARENT_SCOPE)
    set(${textVar} "${_text}" PARENT_SCOPE)
endfunction()

# expect_guard_ok(<case> <expected substring>...)
function(expect_guard_ok case)
    run_guard(_rc _text)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "${case}: the guard refused a valid state (exit ${_rc}): ${_text}")
    endif()
    foreach(_needle IN LISTS ARGN)
        string(FIND "${_text}" "${_needle}" _at)
        if(_at LESS 0)
            message(FATAL_ERROR "${case}: the report does not state \"${_needle}\".\n  Message: ${_text}")
        endif()
    endforeach()
endfunction()

# expect_guard_refused(<case> <expected substring>...)
function(expect_guard_refused case)
    run_guard(_rc _text)
    if(_rc EQUAL 0)
        message(FATAL_ERROR "${case}: the guard accepted a state it must refuse")
    endif()
    foreach(_needle IN LISTS ARGN)
        string(FIND "${_text}" "${_needle}" _at)
        if(_at LESS 0)
            message(FATAL_ERROR
                "${case}: the refusal does not state \"${_needle}\".\n  Message: ${_text}")
        endif()
    endforeach()
endfunction()

# --- a port nobody installed -------------------------------------------------

declare_port(freshport 2.0 0)
reset_install_root()
expect_guard_ok("an overlay port that is not installed" "freshness NOT checked")

# --- installed at the declared version ---------------------------------------

reset_install_root()
install_port(freshport 2.0 0 "plain prose")
expect_guard_ok("an overlay port installed at the declared version" "all 1 installed" "and current")

# --- installed older than the declared version -------------------------------

reset_install_root()
install_port(freshport 1.0 0 "plain prose")
expect_guard_refused("a stale overlay port"
    "Stale overlay port: 'freshport' is installed as 1.0#0"
    "cmake/ports/freshport declares 2.0#0"
    "VCPKG_MANIFEST_INSTALL=ON")

# --- a semicolon in the port's status paragraph ------------------------------
# The trap: unescaped, a ";" splits the matched paragraph into two list
# elements, neither holding both the Version and the Status line, and the port
# reads as not installed at all — so its staleness is never compared.

reset_install_root()
install_port(freshport 1.0 0 "does a; also does b")
expect_guard_refused("a stale overlay port whose description contains a semicolon"
    "Stale overlay port: 'freshport' is installed as 1.0#0")

reset_install_root()
install_port(freshport 2.0 0 "does a; also does b")
expect_guard_ok("a current overlay port whose description contains a semicolon" "all 1 installed" "and current")

# --- a port in cmake/macos-ports/ --------------------------------------------
# vcpkg reads these ports on macOS triplets only, so there the guard must glob
# the directory too, or a stale one links silently; elsewhere vcpkg installs
# the registry port, which the directory's declaration does not describe.

declare_port(macosport 3.0 1 "${_macosPorts}")
reset_install_root()
install_port(freshport 2.0 0 "plain prose")
install_port(macosport 3.0 0 "plain prose")
expect_guard_ok("a registry port off macOS, with a macOS-only overlay of the same name"
    "all 1 installed" "and current")

write_driver(arm64-osx)
reset_install_root()
install_port(freshport 2.0 0 "plain prose")
install_port(macosport 3.0 1 "plain prose")
expect_guard_ok("a macOS-only overlay port installed at the declared version" "all 2 installed" "and current")

reset_install_root()
install_port(freshport 2.0 0 "plain prose")
install_port(macosport 3.0 0 "plain prose")
expect_guard_refused("a stale macOS-only overlay port"
    "Stale overlay port: 'macosport' is installed as 3.0#0"
    "cmake/macos-ports/macosport declares 3.0#1")

# An install newer than the declaration (an older branch against a newer
# install root) is accepted with a warning, which names the port's directory.
reset_install_root()
install_port(freshport 2.0 0 "plain prose")
install_port(macosport 3.0 2 "plain prose")
expect_guard_ok("a macOS-only overlay port installed at a newer port-version"
    "'macosport' is installed as 3.0#2 but cmake/macos-ports/macosport declares only 3.0#1"
    "1 mismatched")

reset_install_root()
install_port(freshport 2.0 0 "plain prose")
install_port(macosport 3.1 0 "plain prose")
expect_guard_ok("a macOS-only overlay port installed at a newer version"
    "'macosport' is installed as 3.1#0 but cmake/macos-ports/macosport declares 3.0#1"
    "1 mismatched")

file(REMOVE_RECURSE "${WORK}")
