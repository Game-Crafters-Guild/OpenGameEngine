# Contract test for cmake/VcpkgInstallGuard.cmake, run by ctest as
#   cmake -DSCRIPT=<guard script> -DWORK=<scratch dir> -P <this file>
# Each case drives the guard in a child process, because a refusal is a
# message(FATAL_ERROR) and this driver has to outlive it to check the text.
#
# Covered: an install root that is a junction (a symlink off Windows) is
# refused and the message names the target and the fix, as is a plain path
# outside the build directory; the build directory's own root is not, however
# it is spelled. A root whose manifest-info.json names another tree is refused
# and the message names that tree, while a root recording this tree is not,
# however the tree is spelled; a root with no record, or an unreadable one, is
# not a refusal, and says so rather than passing silently once the root is
# populated. The installed-set record is written on first sight, accepts an
# unchanged set, refuses a changed version and an ABI-only change — the shape
# a toolset update takes — and keeps refusing until the build directory goes,
# rather than adopting the new set. A package added or removed since the
# record is not a refusal: nothing was compiled against an added one, and a
# removed one fails the link out loud.
if(NOT DEFINED SCRIPT OR NOT DEFINED WORK)
    message(FATAL_ERROR "VcpkgInstallGuardTest.cmake: SCRIPT and WORK are required")
endif()

set(_triplet "x64-windows")
set(_build "${WORK}/build")
set(_installed "${_build}/vcpkg_installed")

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${_build}")

# write_status(<paragraph>...) — each paragraph is a "\n"-joined package block.
function(write_status)
    set(_text "")
    foreach(_paragraph IN LISTS ARGN)
        string(APPEND _text "${_paragraph}\n\n")
    endforeach()
    file(WRITE "${_installed}/vcpkg/status" "${_text}")
endfunction()

function(package_paragraph name version portVersion abi status outVar)
    set(_text "Package: ${name}\nVersion: ${version}\n")
    if(NOT portVersion STREQUAL "")
        string(APPEND _text "Port-Version: ${portVersion}\n")
    endif()
    string(APPEND _text "Architecture: ${_triplet}\nMulti-Arch: same\nAbi: ${abi}\nStatus: ${status}")
    set(${outVar} "${_text}" PARENT_SCOPE)
endfunction()

# run_guard(<out rc> <out normalized stderr> <guard args>...)
function(run_guard rcVar textVar)
    execute_process(
        COMMAND ${CMAKE_COMMAND} ${ARGN} -P ${SCRIPT}
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    # CMake indents and may break the lines of a message(); collapse all
    # whitespace so an assertion on the text cannot fail on formatting.
    string(REGEX REPLACE "[ \t\r\n]+" " " _text "${_out}${_err}")
    set(${rcVar} "${_rc}" PARENT_SCOPE)
    set(${textVar} "${_text}" PARENT_SCOPE)
endfunction()

function(expect_guard_ok case)
    run_guard(_rc _text ${ARGN})
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "${case}: the guard refused a valid state (exit ${_rc}): ${_text}")
    endif()
endfunction()

# expect_record_holds(<case> <expected record line>...)
function(expect_record_holds case)
    file(READ "${_build}/ge-vcpkg-install.stamp" _record)
    foreach(_needle IN LISTS ARGN)
        string(FIND "${_record}" "${_needle}" _at)
        if(_at LESS 0)
            message(FATAL_ERROR
                "${case}: the record dropped \"${_needle}\".\n  Record: ${_record}")
        endif()
    endforeach()
endfunction()

# expect_record_lacks(<case> <record entry that must be absent>...)
function(expect_record_lacks case)
    file(READ "${_build}/ge-vcpkg-install.stamp" _record)
    foreach(_needle IN LISTS ARGN)
        string(FIND "${_record}" "${_needle}" _at)
        if(NOT _at LESS 0)
            message(FATAL_ERROR
                "${case}: the record holds \"${_needle}\".\n  Record: ${_record}")
        endif()
    endforeach()
endfunction()

# expect_guard_refused(<case> <expected substring>... -- <guard args>...)
function(expect_guard_refused case)
    set(_expected "")
    set(_args "")
    set(_inArgs FALSE)
    foreach(_item IN LISTS ARGN)
        if(_item STREQUAL "--")
            set(_inArgs TRUE)
        elseif(_inArgs)
            list(APPEND _args "${_item}")
        else()
            list(APPEND _expected "${_item}")
        endif()
    endforeach()
    run_guard(_rc _text ${_args})
    if(_rc EQUAL 0)
        message(FATAL_ERROR "${case}: the guard accepted a state it must refuse")
    endif()
    foreach(_needle IN LISTS _expected)
        string(FIND "${_text}" "${_needle}" _at)
        if(_at LESS 0)
            message(FATAL_ERROR
                "${case}: the refusal does not state \"${_needle}\".\n  Message: ${_text}")
        endif()
    endforeach()
endfunction()

# --- install root -----------------------------------------------------------

file(MAKE_DIRECTORY "${_installed}/vcpkg")
expect_guard_ok("a real install root"
                -DCHECK=root -DINSTALLED_DIR=${_installed} -DBUILD_DIR=${_build})
expect_guard_ok("an install root that does not exist yet"
                -DCHECK=root -DINSTALLED_DIR=${WORK}/fresh/vcpkg_installed
                -DBUILD_DIR=${WORK}/fresh)
# The same root reached through an unnormalized spelling is still this build
# directory's own.
expect_guard_ok("an install root spelled with a parent-directory step"
                -DCHECK=root -DINSTALLED_DIR=${_build}/sub/../vcpkg_installed
                -DBUILD_DIR=${_build})

# A peer tree's install root, passed as a plain path: the 2026-07-26 spelling
# of the same sharing the link check refuses.
set(_peerInstalled "${WORK}/peer-tree/build/vcpkg_installed")
file(MAKE_DIRECTORY "${_peerInstalled}/vcpkg")
expect_guard_refused("a plain install root outside the build directory"
    "outside this build directory" "unset VCPKG_INSTALLED_DIR" "${_peerInstalled}"
    -- -DCHECK=root -DINSTALLED_DIR=${_peerInstalled} -DBUILD_DIR=${_build})
# A sibling whose name merely starts with the build directory's name is outside
# it: the comparison is by path component, not by string prefix.
expect_guard_refused("an install root in a sibling of the build directory"
    "outside this build directory"
    -- -DCHECK=root -DINSTALLED_DIR=${_build}-2/vcpkg_installed -DBUILD_DIR=${_build})

# The link is planted inside its own build directory, so only the link can be
# what the guard refuses here.
set(_linkedBuild "${WORK}/linked-build")
set(_link "${_linkedBuild}/vcpkg_installed")
file(MAKE_DIRECTORY "${_linkedBuild}")
if(CMAKE_HOST_WIN32)
    file(TO_NATIVE_PATH "${_link}" _nativeLink)
    file(TO_NATIVE_PATH "${_installed}" _nativeTarget)
    execute_process(COMMAND cmd /c mklink /J "${_nativeLink}" "${_nativeTarget}"
                    RESULT_VARIABLE _linkRc OUTPUT_QUIET ERROR_QUIET)
    set(_linkNoun "junction or symbolic link")
else()
    execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink "${_installed}" "${_link}"
                    RESULT_VARIABLE _linkRc OUTPUT_QUIET ERROR_QUIET)
    set(_linkNoun "symbolic link")
endif()
if(_linkRc EQUAL 0)
    expect_guard_refused("a linked install root"
        "is a ${_linkNoun} to" "delete it and reconfigure" "restores its own install"
        -- -DCHECK=root -DINSTALLED_DIR=${_link} -DBUILD_DIR=${_linkedBuild})
else()
    # Never let "could not set the trap" read as "the trap held".
    message(FATAL_ERROR
        "VcpkgInstallGuardTest: could not create a ${_linkNoun} at ${_link} (exit ${_linkRc}); "
        "the linked-install-root case cannot be exercised here")
endif()

# --- which tree produced the install root ------------------------------------

# write_manifest_info(<install root> <manifest dir>) — the record vcpkg writes
# next to an install, with the manifest path spelled natively and JSON-escaped
# the way vcpkg spells it on this host.
function(write_manifest_info root manifestDir)
    file(TO_NATIVE_PATH "${manifestDir}/vcpkg.json" _native)
    string(REPLACE "\\" "\\\\" _escaped "${_native}")
    file(WRITE "${root}/vcpkg/manifest-info.json" "{\n  \"manifest-path\": \"${_escaped}\"\n}\n")
endfunction()

set(_ownTree "${WORK}/own-tree")
set(_ownBuild "${_ownTree}/build")
set(_ownInstalled "${_ownBuild}/vcpkg_installed")
file(MAKE_DIRECTORY "${_ownInstalled}/vcpkg")
set(_producerArgs -DCHECK=producer -DINSTALLED_DIR=${_ownInstalled}
                  -DMANIFEST_DIR=${_ownTree} -DBUILD_DIR=${_ownBuild})

# The positive control: the record every install this configure performs writes.
write_manifest_info("${_ownInstalled}" "${_ownTree}")
expect_guard_ok("an install root this tree produced" ${_producerArgs})

# The same tree spelled another way is still this tree.
expect_guard_ok("an install root recorded through an unnormalized spelling"
                -DCHECK=producer -DINSTALLED_DIR=${_ownInstalled}
                -DMANIFEST_DIR=${_ownTree}/build/.. -DBUILD_DIR=${_ownBuild})

# A record naming another tree: a build directory copied from one, or one whose
# install was written through a link. The root is a real directory in the right
# place, so no other check in this module fires on it.
write_manifest_info("${_ownInstalled}" "${WORK}/peer-tree")
expect_guard_refused("an install root another tree produced"
    "written by another tree" "delete ${_ownBuild} and reconfigure"
    "peer-tree/vcpkg.json"
    -- ${_producerArgs})

# Not a manifest configure: nothing produced the root, so there is nothing to
# compare it against.
expect_guard_ok("a configure with no manifest directory"
                -DCHECK=producer -DINSTALLED_DIR=${_ownInstalled} -DMANIFEST_DIR=
                -DBUILD_DIR=${_ownBuild})

# Absence is not a refusal: a root nothing has installed into yet has no record,
# and says nothing about one.
file(REMOVE "${_ownInstalled}/vcpkg/manifest-info.json")
run_guard(_rc _text ${_producerArgs})
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
        "an install root with no record: the guard refused (exit ${_rc}): ${_text}")
endif()
string(STRIP "${_text}" _stripped)
if(NOT _stripped STREQUAL "")
    message(FATAL_ERROR
        "an install root with no record: the guard reported on a root nothing has installed "
        "into.\n  Message: ${_text}")
endif()

# Once the root is populated, though, a missing record means the check can no
# longer run, and "not checked" must never be mistakable for "checked and ours".
file(WRITE "${_ownInstalled}/vcpkg/status" "")
run_guard(_rc _text ${_producerArgs})
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
        "a populated install root with no record: the guard refused (exit ${_rc}): ${_text}")
endif()
string(FIND "${_text}" "NOT checked" _at)
if(_at LESS 0)
    message(FATAL_ERROR
        "a populated install root with no record: the guard passed it in silence.\n"
        "  Message: ${_text}")
endif()

# An unreadable record is the same case, and must not be read as another tree's.
file(WRITE "${_ownInstalled}/vcpkg/manifest-info.json" "{ this is not json")
run_guard(_rc _text ${_producerArgs})
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR
        "an unreadable install-root record: the guard refused (exit ${_rc}): ${_text}")
endif()
string(FIND "${_text}" "NOT checked" _at)
if(_at LESS 0)
    message(FATAL_ERROR
        "an unreadable install-root record: the guard passed it in silence.\n"
        "  Message: ${_text}")
endif()

file(REMOVE_RECURSE "${_ownTree}")

# --- installed set ----------------------------------------------------------

set(_setArgs -DCHECK=set -DINSTALLED_DIR=${_installed} -DTRIPLET=${_triplet} -DBUILD_DIR=${_build})

expect_guard_ok("an install root with no status database" ${_setArgs})
if(EXISTS "${_build}/ge-vcpkg-install.stamp")
    message(FATAL_ERROR "an unpopulated install root must not be recorded as a package set")
endif()

# Not a vcpkg configure at all — the check owns that precondition, rather than
# each call site deciding whether to call it.
expect_guard_ok("a configure with no install root"
                -DCHECK=set -DINSTALLED_DIR= -DTRIPLET=${_triplet} -DBUILD_DIR=${_build})
if(EXISTS "${_build}/ge-vcpkg-install.stamp")
    message(FATAL_ERROR "a configure with no install root must not be recorded as a package set")
endif()

package_paragraph(curl 8.21.0 1 abi-curl-old "install ok installed" _curlOld)
package_paragraph(glfw3 3.4 "" abi-glfw "install ok installed" _glfw)
package_paragraph(shaderc 2026.3 "" abi-shaderc-old "install ok installed" _shadercOld)
# A feature paragraph repeats the package name and carries no version or ABI.
set(_curlFeature "Package: curl\nFeature: ssl\nDepends: openssl\nArchitecture: ${_triplet}\nStatus: install ok installed")
# Host-triplet ports are a different ABI universe and must never enter the set.
set(_hostOld "Package: hostonly\nVersion: 9.9\nArchitecture: arm64-windows\nAbi: abi-host-old\nStatus: install ok installed")
set(_hostNew "Package: hostonly\nVersion: 9.10\nArchitecture: arm64-windows\nAbi: abi-host-new\nStatus: install ok installed")
write_status("${_curlOld}" "${_curlFeature}" "${_glfw}" "${_shadercOld}" "${_hostOld}")

expect_guard_ok("first sight of an installed set" ${_setArgs})
if(NOT EXISTS "${_build}/ge-vcpkg-install.stamp")
    message(FATAL_ERROR "the first check must record the installed set")
endif()
expect_guard_ok("an unchanged installed set" ${_setArgs})

# Only the target triplet's packages are compared: a host-triplet port
# changing version is not this build's ABI.
write_status("${_curlOld}" "${_curlFeature}" "${_glfw}" "${_shadercOld}" "${_hostNew}")
expect_guard_ok("a changed package for another triplet" ${_setArgs})

# A package added since the record cannot invalidate an object that never
# included its headers, and one removed fails the link out loud.
package_paragraph(tinyexpr 1.1.1 "" abi-tinyexpr "install ok installed" _tinyexpr)
package_paragraph(glfw3 3.4 "" abi-glfw "purge ok not-installed" _glfwGone)
write_status("${_curlOld}" "${_curlFeature}" "${_glfw}" "${_shadercOld}" "${_hostNew}"
             "${_tinyexpr}" "${_glfwGone}")
expect_guard_ok("a package added and a package removed" ${_setArgs})
# What left the set stays in the record, so it is still compared if it returns.
expect_record_holds("a package removed from the set" "glfw3|3.4#0|abi-glfw")

# The status database is append-only, so the removal above is only real if the
# last paragraph wins — a build directory first configured against this status
# must not record the purged package at all. (Deleting the build directory
# takes the install root with it, exactly as it does on disk; the reconfigure
# restores it from the binary cache before the guard looks.)
file(REMOVE_RECURSE "${_build}")
file(MAKE_DIRECTORY "${_build}")
write_status("${_curlOld}" "${_curlFeature}" "${_glfw}" "${_shadercOld}" "${_hostNew}"
             "${_tinyexpr}" "${_glfwGone}")
expect_guard_ok("first sight of a set with a purged package" ${_setArgs})
expect_record_lacks("first sight of a set with a purged package" "glfw3|")

# A reinstall of a package this directory never compiled against is an
# addition, not a change.
package_paragraph(glfw3 3.5.1 "" abi-glfw-new "install ok installed" _glfwBack)
write_status("${_curlOld}" "${_curlFeature}" "${_shadercOld}" "${_hostNew}"
             "${_tinyexpr}" "${_glfwBack}")
expect_guard_ok("a removed package reinstalled at a new version" ${_setArgs})

# A baseline bump: same package, new version.
package_paragraph(curl 8.22.0 "" abi-curl-new "install ok installed" _curlNew)
write_status("${_curlNew}" "${_curlFeature}" "${_shadercOld}" "${_hostNew}"
             "${_tinyexpr}" "${_glfwBack}")
expect_guard_refused("a changed package version"
    "curl 8.21.0#1 -> 8.22.0#0" "delete ${_build} and reconfigure"
    -- ${_setArgs})

# The record must not be adopted by the failure it reported.
expect_guard_refused("a changed package version, checked twice"
    "curl 8.21.0#1 -> 8.22.0#0"
    -- ${_setArgs})

# A toolset update: same versions, every ABI hash new.
package_paragraph(shaderc 2026.3 "" abi-shaderc-new "install ok installed" _shadercNew)
write_status("${_curlOld}" "${_curlFeature}" "${_shadercNew}" "${_hostNew}"
             "${_tinyexpr}" "${_glfwBack}")
expect_guard_refused("an ABI-only change"
    "shaderc 2026.3#0 (ABI only)" "delete ${_build} and reconfigure"
    -- ${_setArgs})

# Deleting the build directory is the whole remediation. The install root goes
# with it, exactly as it does on disk, and the reconfigure restores it from the
# binary cache before the guard looks.
file(REMOVE_RECURSE "${_build}")
file(MAKE_DIRECTORY "${_build}")
write_status("${_curlOld}" "${_curlFeature}" "${_shadercNew}" "${_hostNew}"
             "${_tinyexpr}" "${_glfwBack}")
expect_guard_ok("a build directory deleted after a refusal" ${_setArgs})
if(NOT EXISTS "${_build}/ge-vcpkg-install.stamp")
    message(FATAL_ERROR "a rebuilt build directory must record the installed set again")
endif()
expect_guard_ok("the rebuilt record" ${_setArgs})

# A short read of the status database — a half-written file — must never shrink
# the record: what is missing stays recorded, and is compared again when it
# comes back.
file(WRITE "${_installed}/vcpkg/status" "${_curlOld}\n\nPackage: shaderc\nVersion: 2026.3\nArch")
expect_guard_ok("a truncated status database" ${_setArgs})
expect_record_holds("a truncated status database"
    "tinyexpr|1.1.1#0|abi-tinyexpr" "glfw3|3.5.1#0|abi-glfw-new" "shaderc|2026.3#0|abi-shaderc-new")

write_status("${_curlOld}" "${_curlFeature}" "${_shadercNew}" "${_hostNew}"
             "${_tinyexpr}" "${_glfwBack}")
expect_guard_ok("the status database restored after a short read" ${_setArgs})

# A package that leaves the set keeps its recorded identity, so coming back at
# a different version is a comparison and not a fresh record — the shape a
# manifest feature turned off and on again takes across a version bump.
package_paragraph(tinyexpr 1.1.1 "" abi-tinyexpr "purge ok not-installed" _tinyexprGone)
write_status("${_curlOld}" "${_curlFeature}" "${_shadercNew}" "${_hostNew}"
             "${_tinyexpr}" "${_glfwBack}" "${_tinyexprGone}")
expect_guard_ok("a package purged from the set" ${_setArgs})
expect_record_holds("a package purged from the set" "tinyexpr|1.1.1#0|abi-tinyexpr")

package_paragraph(tinyexpr 2.0 "" abi-tinyexpr-new "install ok installed" _tinyexprBack)
write_status("${_curlOld}" "${_curlFeature}" "${_shadercNew}" "${_hostNew}"
             "${_tinyexpr}" "${_glfwBack}" "${_tinyexprGone}" "${_tinyexprBack}")
expect_guard_refused("a purged package reinstalled at a new version"
    "tinyexpr 1.1.1#0 -> 2.0#0" "delete ${_build} and reconfigure"
    -- ${_setArgs})

# A ";" inside a paragraph — a Description carries prose — must not split the
# package out of the comparison. Written directly rather than through
# write_status(), whose own paragraph list would split it the same way. The
# build directory starts again so the case stands on its own record.
file(REMOVE_RECURSE "${_build}")
file(MAKE_DIRECTORY "${_build}")
set(_semiHead "Package: semicolon-port\nVersion: 1.0\nDescription: parses a; and also a b")
set(_semiTail "Architecture: ${_triplet}\nMulti-Arch: same\nStatus: install ok installed")
file(WRITE "${_installed}/vcpkg/status"
     "${_curlOld}\n\n${_semiHead}\n${_semiTail}\nAbi: abi-semi-old\n\n${_glfwBack}\n\n")
expect_guard_ok("a package whose description contains a semicolon" ${_setArgs})
expect_record_holds("a package whose description contains a semicolon"
    "semicolon-port|1.0#0|abi-semi-old" "curl|8.21.0#1|abi-curl-old"
    "glfw3|3.5.1#0|abi-glfw-new")

file(WRITE "${_installed}/vcpkg/status"
     "${_curlOld}\n\n${_semiHead}\n${_semiTail}\nAbi: abi-semi-new\n\n${_glfwBack}\n\n")
expect_guard_refused("a changed package whose description contains a semicolon"
    "semicolon-port 1.0#0 (ABI only)" "delete ${_build} and reconfigure"
    -- ${_setArgs})

file(REMOVE_RECURSE "${WORK}")
