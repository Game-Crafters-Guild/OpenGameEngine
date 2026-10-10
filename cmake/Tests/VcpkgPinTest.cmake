# Contract test for cmake/VcpkgPin.cmake, run by ctest as
#   cmake -DSCRIPT=<pin script> -DWORK=<scratch dir> -P <this file>
# Each case drives the pin functions in a child process, because one of them
# reports through message(FATAL_ERROR) and this driver has to outlive it to
# check the text. The scratch checkouts are real local git repositories, so the
# cases exercise git exactly as a configure does, offline.
#
# Covered: the recorded pin is a commit and not a branch name or a truncation; a
# checkout on the pin is reported as such and nothing is warned about; a
# checkout on another commit names both commits and the one line that moves it,
# and is a report rather than a refusal; a tree that has not provisioned vcpkg
# yet says nothing; a vcpkg directory that is not a git checkout says its commit
# cannot be compared instead of passing silently; and the provisioning path puts
# a fresh checkout on the pin, or fails loudly naming the manual command when
# the pin cannot be reached.
if(NOT DEFINED SCRIPT OR NOT DEFINED WORK)
    message(FATAL_ERROR "VcpkgPinTest.cmake: SCRIPT and WORK are required")
endif()

find_program(GIT_EXECUTABLE NAMES git)
if(NOT GIT_EXECUTABLE)
    message(FATAL_ERROR
        "VcpkgPinTest.cmake: git is not on PATH. The configure under test clones vcpkg with "
        "git, so a machine without it cannot build this repo either.")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

# --- the recorded pin itself -------------------------------------------------

include("${SCRIPT}")
# CMake's regex has no bounded repetition, so the width is a separate check.
string(LENGTH "${GE_VCPKG_COMMIT}" pinLength)
if(NOT GE_VCPKG_COMMIT MATCHES "^[0-9a-f]+$" OR NOT pinLength EQUAL 40)
    message(FATAL_ERROR
        "the recorded pin is not a full commit hash: GE_VCPKG_COMMIT='${GE_VCPKG_COMMIT}'. "
        "A branch name or an abbreviation would let two trees provision different checkouts.")
endif()

# make_checkout(<dir> <out first commit> <out second commit>) — a two-commit
# local repository with no remote.
function(make_checkout dir firstVar secondVar)
    file(MAKE_DIRECTORY "${dir}")
    execute_process(COMMAND ${GIT_EXECUTABLE} init --quiet "${dir}" COMMAND_ERROR_IS_FATAL ANY)
    foreach(subject IN ITEMS first second)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} -C "${dir}"
                    -c user.name=test -c user.email=test@example.invalid
                    commit --quiet --allow-empty -m "${subject}"
            COMMAND_ERROR_IS_FATAL ANY)
        execute_process(COMMAND ${GIT_EXECUTABLE} -C "${dir}" rev-parse HEAD
            OUTPUT_VARIABLE sha OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
        set(${subject}Sha "${sha}")
    endforeach()
    set(${firstVar} "${firstSha}" PARENT_SCOPE)
    set(${secondVar} "${secondSha}" PARENT_SCOPE)
endfunction()

# run_pin(<out rc> <out normalized output> <pin> <call>) — drives one pin
# function against ${pin}, which the driver sets after including the script so a
# case can pin a commit that exists in its own scratch checkout.
function(run_pin rcVar textVar pin call)
    set(driver "${WORK}/drive.cmake")
    file(WRITE "${driver}"
        "include(\"${SCRIPT}\")\n"
        "set(GE_VCPKG_COMMIT \"${pin}\")\n"
        "${call}\n")
    execute_process(COMMAND ${CMAKE_COMMAND} -P "${driver}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    # CMake indents and may break the lines of a message(); collapse all
    # whitespace so an assertion on the text cannot fail on formatting.
    string(REGEX REPLACE "[ \t\r\n]+" " " text "${out}${err}")
    set(${rcVar} "${rc}" PARENT_SCOPE)
    set(${textVar} "${text}" PARENT_SCOPE)
endfunction()

function(expect_states case text)
    foreach(needle IN LISTS ARGN)
        string(FIND "${text}" "${needle}" at)
        if(at LESS 0)
            message(FATAL_ERROR "${case}: the report does not state \"${needle}\".\n  Message: ${text}")
        endif()
    endforeach()
endfunction()

function(expect_silent_about case text)
    foreach(needle IN LISTS ARGN)
        string(FIND "${text}" "${needle}" at)
        if(NOT at LESS 0)
            message(FATAL_ERROR "${case}: the report states \"${needle}\" and must not.\n  Message: ${text}")
        endif()
    endforeach()
endfunction()

# --- a tree that has not provisioned vcpkg -----------------------------------

run_pin(rc text "0123456789012345678901234567890123456789"
        "ge_check_vcpkg_clone_pin(\"${WORK}/absent\")")
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "a tree without a vcpkg checkout: the check failed (exit ${rc}): ${text}")
endif()
string(STRIP "${text}" stripped)
if(NOT stripped STREQUAL "")
    message(FATAL_ERROR
        "a tree without a vcpkg checkout: the check reported on a checkout that does not "
        "exist yet.\n  Message: ${text}")
endif()

# --- a vcpkg directory that is not a git checkout ----------------------------

file(MAKE_DIRECTORY "${WORK}/unpacked")
run_pin(rc text "0123456789012345678901234567890123456789"
        "ge_check_vcpkg_clone_pin(\"${WORK}/unpacked\")")
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "a vcpkg directory that is not a git checkout: the check failed (exit ${rc}): ${text}")
endif()
expect_states("a vcpkg directory that is not a git checkout" "${text}"
    "is not a git checkout" "cannot be compared against the pin")

# --- a checkout on the pin ---------------------------------------------------

make_checkout("${WORK}/clone" firstSha secondSha)
run_pin(rc text "${secondSha}" "ge_check_vcpkg_clone_pin(\"${WORK}/clone\")")
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "a checkout on the pin: the check failed (exit ${rc}): ${text}")
endif()
expect_states("a checkout on the pin" "${text}" "vcpkg checkout at the pinned commit ${secondSha}")
expect_silent_about("a checkout on the pin" "${text}" "is not at the pinned commit")

# --- a checkout on another commit --------------------------------------------
# The report names both commits, because "wrong commit" without the two values
# is not actionable, and states the one line that moves this tree onto the pin.

run_pin(rc text "${firstSha}" "ge_check_vcpkg_clone_pin(\"${WORK}/clone\")")
if(NOT rc EQUAL 0)
    message(FATAL_ERROR
        "a checkout on another commit: the check refused (exit ${rc}) where it must report: ${text}")
endif()
expect_states("a checkout on another commit" "${text}"
    "vcpkg checkout is not at the pinned commit"
    "${firstSha}"
    "${secondSha}"
    "fetch origin ${firstSha}"
    "checkout --detach ${firstSha}"
    "GE_VCPKG_COMMIT in cmake/VcpkgPin.cmake")

# --- provisioning puts a fresh checkout on the pin ---------------------------

run_pin(rc text "${firstSha}" "ge_vcpkg_checkout_pin(\"${WORK}/clone\")")
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "provisioning onto the pin: the checkout failed (exit ${rc}): ${text}")
endif()
expect_states("provisioning onto the pin" "${text}" "vcpkg checked out at the pinned commit ${firstSha}")
execute_process(COMMAND ${GIT_EXECUTABLE} -C "${WORK}/clone" rev-parse HEAD
    OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT head STREQUAL "${firstSha}")
    message(FATAL_ERROR
        "provisioning onto the pin: the checkout is at ${head}, not the pin ${firstSha}")
endif()

# --- provisioning when the pin cannot be reached -----------------------------
# The scratch checkout has no remote, so the fetch fallback cannot rescue it.

run_pin(rc text "ffffffffffffffffffffffffffffffffffffffff"
        "ge_vcpkg_checkout_pin(\"${WORK}/clone\")")
if(rc EQUAL 0)
    message(FATAL_ERROR
        "provisioning at an unreachable pin: the configure continued on the wrong commit: ${text}")
endif()
expect_states("provisioning at an unreachable pin" "${text}"
    "could not be checked out at the pinned commit"
    "checkout --detach ffffffffffffffffffffffffffffffffffffffff")

file(REMOVE_RECURSE "${WORK}")
