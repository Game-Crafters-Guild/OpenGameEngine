# The vcpkg provisioning pin.
#
# vcpkg keys its binary cache on a per-port ABI hash, and that hash covers the
# vcpkg checkout, not only the port: the checkout's scripts/ports.cmake, the
# triplet's toolchain file out of the same checkout, and the resolved versions
# of vcpkg's ABI-sensitive tools all feed it (vcpkg records them in
# dependencies/vcpkg/buildtrees/<port>/<triplet>.vcpkg_abi_info.txt). Two trees
# share cache archives only when they resolve the same checkout and the same
# tools. Differ in either and every archive on the machine is a miss and every
# port compiles from source, with nothing in the output naming the reason.
#
# Two things are pinned here.
#
# The checkout: GE_VCPKG_COMMIT is what a tree provisioning its own
# dependencies/vcpkg lands on, instead of whatever upstream's default branch is
# that day. Only that provisioning checks it out. A tree that already holds a
# checkout is reported and never rewritten, so a bump does not move the machine:
# it splits it into two ABI families, and the binary cache carries both until
# the old one ages out. Whoever bumps the pin moves every checkout on the
# machine by hand, with the line the report below prints.
#
# That is where the pin differs from vcpkg-configuration.json's default-registry
# baseline, which every tree picks up on its next install. The two are
# independent and both are honoured: the baseline selects port *versions* out of
# the registry, this commit selects the vcpkg *tooling* the tree runs. vcpkg has
# no field for the second — nothing in vcpkg.json or vcpkg-configuration.json
# decides what the checkout is checked out at — so the pin lives here, where the
# configure that performs the clone reads it.
#
# The tools: vcpkg resolves an ABI-sensitive tool from
# <downloads>/tools/<tool>-<version>-<os> when that exact directory exists;
# failing that it takes the first version already on this system that satisfies
# the pin, and only downloads the pinned one when nothing does. The middle
# branch is the trap — a system PowerShell newer than the pin satisfies it, so
# the tool version recorded in every port's ABI becomes a property of the
# machine. Pointing every tree at one per-user downloads root and fetching the
# pinned tools into it makes that resolution identical everywhere, and costs a
# download once per machine instead of once per tree.

set(GE_VCPKG_COMMIT "ade921ed36f0414d40fafed83f1df7a95c6d1db4")

# Reports whether ${cloneDir} is checked out at GE_VCPKG_COMMIT. Runs on every
# configure, including the ones that provision nothing.
#
# A mismatch is a report, not a refusal: it costs time rather than correctness
# — the port versions still come from the registry baseline — and the checkout
# may be deliberate, someone testing a port bump. Refusing would leave no way
# forward other than editing the pin, and checking the pin out here would
# discard whatever that someone was doing. The refusals in this build live where
# state gets damaged (cmake/VcpkgInstallGuard.cmake); this is the other case.
function(ge_check_vcpkg_clone_pin cloneDir)
    if(NOT IS_DIRECTORY "${cloneDir}")
        return()
    endif()
    if(NOT IS_DIRECTORY "${cloneDir}/.git")
        message(STATUS
            "vcpkg checkout at ${cloneDir} is not a git checkout, so its commit cannot be "
            "compared against the pin ${GE_VCPKG_COMMIT} (cmake/VcpkgPin.cmake)")
        return()
    endif()

    execute_process(COMMAND git -C "${cloneDir}" rev-parse HEAD
        OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE rc ERROR_QUIET)
    if(NOT rc EQUAL 0)
        message(STATUS
            "vcpkg checkout at ${cloneDir}: git could not read HEAD (exit ${rc}), so its commit "
            "cannot be compared against the pin ${GE_VCPKG_COMMIT} (cmake/VcpkgPin.cmake)")
        return()
    endif()

    if(head STREQUAL "${GE_VCPKG_COMMIT}")
        message(STATUS "vcpkg checkout at the pinned commit ${GE_VCPKG_COMMIT}")
        return()
    endif()

    message(WARNING
        "vcpkg checkout is not at the pinned commit.\n"
        "    pinned (cmake/VcpkgPin.cmake): ${GE_VCPKG_COMMIT}\n"
        "    ${cloneDir}: ${head}\n"
        "  Every port's ABI hash covers this checkout's scripts/ports.cmake, its triplet\n"
        "  toolchain file and its pinned tool versions, so this tree misses every binary-cache\n"
        "  archive built from the pinned checkout and compiles the ports from source instead\n"
        "  (tens of minutes). Port versions are unaffected: those come from the\n"
        "  default-registry baseline in vcpkg-configuration.json.\n"
        "  To move this tree onto the pin (commit or stash local changes in that checkout\n"
        "  first — git refuses to check out over a modified tracked file):\n"
        "    git -C ${cloneDir} fetch origin ${GE_VCPKG_COMMIT} && git -C ${cloneDir} checkout --detach ${GE_VCPKG_COMMIT}\n"
        "  If the pin itself should move, change GE_VCPKG_COMMIT in cmake/VcpkgPin.cmake — that\n"
        "  decides what a newly provisioned tree lands on, and every checkout already on this\n"
        "  machine, including this one, still has to be moved by the line above.")
endfunction()

# Checks GE_VCPKG_COMMIT out in a checkout this configure has just created.
# Fatal on failure: a fresh checkout on the wrong commit has nothing worth
# keeping, and every port behind it would compile from source.
function(ge_vcpkg_checkout_pin cloneDir)
    z_ge_vcpkg_checkout_commit(rc err "${cloneDir}")
    if(NOT rc EQUAL 0)
        # A checkout that does not contain the pin — a filtered or shallow
        # clone, or a pin that is not an ancestor of the default branch — can
        # still ask the remote for that one commit by name.
        execute_process(COMMAND git -C "${cloneDir}" fetch --quiet origin "${GE_VCPKG_COMMIT}"
            RESULT_VARIABLE fetchRc ERROR_QUIET)
        if(fetchRc EQUAL 0)
            z_ge_vcpkg_checkout_commit(rc err "${cloneDir}")
        endif()
    endif()
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR
            "vcpkg was cloned into ${cloneDir} but could not be checked out at the pinned "
            "commit ${GE_VCPKG_COMMIT} (cmake/VcpkgPin.cmake).\n"
            "  Git error:\n"
            "    ${err}\n"
            "  Every port's ABI hash covers the checkout, so continuing on another commit "
            "would compile every port from source instead of restoring it from the binary "
            "cache.\n"
            "  Run this by hand to see the full output, then re-run cmake:\n"
            "    git -C ${cloneDir} fetch origin ${GE_VCPKG_COMMIT} && git -C ${cloneDir} checkout --detach ${GE_VCPKG_COMMIT}")
    endif()
    message(STATUS "vcpkg checked out at the pinned commit ${GE_VCPKG_COMMIT}")
endfunction()

function(z_ge_vcpkg_checkout_commit rcVar errVar cloneDir)
    execute_process(COMMAND git -C "${cloneDir}" checkout --quiet --detach "${GE_VCPKG_COMMIT}"
        RESULT_VARIABLE rc ERROR_VARIABLE err)
    set(${rcVar} "${rc}" PARENT_SCOPE)
    set(${errVar} "${err}" PARENT_SCOPE)
endfunction()

# Points vcpkg's downloads root at a per-user directory every tree on the
# machine shares, so the pinned tools are fetched once rather than once per
# tree. It is the sibling of the binary cache vcpkg already keeps there, and it
# is resolved the way prime_vcpkg_binary_cache() resolves that cache. An
# environment that already names a root keeps it (the Steam Deck container
# sets one).
#
# Only the download location is shared. Each tree still keeps its own install
# root, which is what cmake/VcpkgInstallGuard.cmake exists to enforce: a
# download is an immutable version-stamped artifact, an install root is build
# input.
function(ge_vcpkg_use_shared_downloads_root)
    if(DEFINED ENV{VCPKG_DOWNLOADS} AND NOT "$ENV{VCPKG_DOWNLOADS}" STREQUAL "")
        message(STATUS "vcpkg downloads root: $ENV{VCPKG_DOWNLOADS} (from the environment)")
        return()
    endif()

    # The downloads root is a host location, so it follows the host system even
    # when the target is not this one (wasm, Steam Deck). With nothing naming a
    # per-user directory, vcpkg keeps its downloads under the checkout: still
    # correct, just once per tree, and said out loud so that "no shared root"
    # is never mistaken for "this line did not run".
    if(CMAKE_HOST_WIN32)
        set(homeVar "LOCALAPPDATA")
        set(root "$ENV{LOCALAPPDATA}/vcpkg/downloads")
    elseif(NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
        set(homeVar "XDG_CACHE_HOME")
        set(root "$ENV{XDG_CACHE_HOME}/vcpkg/downloads")
    else()
        set(homeVar "HOME")
        set(root "$ENV{HOME}/.cache/vcpkg/downloads")
    endif()
    if("$ENV{${homeVar}}" STREQUAL "")
        message(STATUS
            "vcpkg downloads root: this tree's own dependencies/vcpkg/downloads — ${homeVar} is "
            "not set, so there is no per-user directory to share with the other trees on this "
            "machine, and each one downloads vcpkg's tools for itself")
        return()
    endif()

    file(TO_CMAKE_PATH "${root}" root)
    file(MAKE_DIRECTORY "${root}")
    set(ENV{VCPKG_DOWNLOADS} "${root}")
    message(STATUS "vcpkg downloads root: ${root} (shared by every tree on this machine)")
endfunction()

# Resolves the tools whose versions enter every port's ABI hash, fetching the
# pinned version when this machine has not got it yet, and reports what each one
# resolved to. That report is the whole diagnosis when a configure misses the
# cache: a path outside the downloads root is a system tool of some other
# version, and names the reason every archive missed.
#
# Runs before any installer and at most once per configure. vcpkg's own manifest
# install at project() is one of those installers and takes no arguments from
# this build, so the seeding is done to the downloads root that install reads
# rather than passed on a command line.
function(ge_seed_vcpkg_pinned_tools vcpkgExe)
    get_property(alreadyResolved GLOBAL PROPERTY GE_VCPKG_TOOLS_RESOLVED)
    if(alreadyResolved)
        return()
    endif()

    # Claim the one shot only once there is something to resolve with, so a call
    # made before vcpkg is bootstrapped cannot consume it and leave the real
    # call a silent no-op.
    if(NOT EXISTS "${vcpkgExe}")
        return()
    endif()
    set_property(GLOBAL PROPERTY GE_VCPKG_TOOLS_RESOLVED TRUE)
    cmake_path(GET vcpkgExe PARENT_PATH vcpkgDir)

    set(downloads "$ENV{VCPKG_DOWNLOADS}")
    if(downloads STREQUAL "")
        set(downloads "${vcpkgDir}/downloads")
    endif()
    file(TO_CMAKE_PATH "${downloads}" downloads)

    # cmake drives the configure and build of every CMake-based port; vcpkg runs
    # its Windows post-build checks through powershell-core. Those are the two
    # tools that appear in this repo's ABI records.
    set(tools cmake)
    if(CMAKE_HOST_WIN32)
        list(APPEND tools powershell-core)
    endif()

    foreach(tool IN LISTS tools)
        z_ge_vcpkg_resolve_tool(path "${vcpkgExe}" "${vcpkgDir}" "${tool}")
        z_ge_vcpkg_path_is_under(pinned "${path}" "${downloads}")
        if(NOT pinned)
            # An unresolvable tool has no path to name, and "resolves to "
            # with nothing after it reads as a defect in this line.
            set(resolution "this machine resolves ${tool} to ${path}")
            if(path STREQUAL "")
                set(resolution
                    "this machine does not resolve ${tool} at all: ${vcpkgExe} fetch ${tool} --vcpkg-root=${vcpkgDir}")
            endif()
            message(STATUS "Fetching vcpkg's pinned ${tool} into ${downloads} (${resolution})")
            # --x-stderr-status keeps vcpkg's progress on stderr, where it still
            # reaches the console: this downloads tens of megabytes on a machine
            # without the tool, and silence there reads as a hang. What is left
            # on stdout is the resolved path, which the report below states in
            # full, so it is captured rather than echoed as a bare line.
            execute_process(
                COMMAND ${CMAKE_COMMAND} -E env "VCPKG_FORCE_DOWNLOADED_BINARIES=1"
                        "${vcpkgExe}" fetch "${tool}" "--vcpkg-root=${vcpkgDir}" --x-stderr-status
                WORKING_DIRECTORY "${vcpkgDir}"
                OUTPUT_QUIET)
            z_ge_vcpkg_resolve_tool(path "${vcpkgExe}" "${vcpkgDir}" "${tool}")
            z_ge_vcpkg_path_is_under(pinned "${path}" "${downloads}")
        endif()

        if(pinned)
            message(STATUS "vcpkg tool ${tool}: ${path}")
        elseif(path STREQUAL "")
            message(WARNING
                "vcpkg could not resolve ${tool} at all, so the version that will enter every\n"
                "  port's ABI hash is unknown and this configure may miss the whole binary cache.\n"
                "  vcpkg's own error is above. Run it directly to see the full output:\n"
                "    ${vcpkgExe} fetch ${tool} --vcpkg-root=${vcpkgDir}")
        else()
            message(WARNING
                "vcpkg resolved ${tool} to ${path}, outside its downloads root\n"
                "    ${downloads}\n"
                "  so it is a copy already present on this system rather than the version\n"
                "  ${vcpkgDir}/scripts/vcpkg-tools.json pins. The resolved version of this tool\n"
                "  is part of every port's ABI hash, so this configure misses every binary-cache\n"
                "  archive and compiles the ports from source (tens of minutes).\n"
                "  Fetch the pinned copy with VCPKG_FORCE_DOWNLOADED_BINARIES=1 set, which is\n"
                "  what makes vcpkg ignore the system copy, then reconfigure:\n"
                "    ${vcpkgExe} fetch ${tool} --vcpkg-root=${vcpkgDir}")
        endif()
    endforeach()
endfunction()

# Returns the path vcpkg resolves ${tool} to: the same resolution every
# installer in this configure performs, including its download of the pinned
# tool when nothing on this system satisfies the pin. --x-stderr-status moves
# vcpkg's progress lines off stdout, leaving the path and nothing else there;
# stderr is left to reach the console, because that download is tens of
# megabytes and silence reads as a hang.
function(z_ge_vcpkg_resolve_tool outVar vcpkgExe vcpkgDir tool)
    execute_process(
        COMMAND "${vcpkgExe}" fetch "${tool}" "--vcpkg-root=${vcpkgDir}" --x-stderr-status
        WORKING_DIRECTORY "${vcpkgDir}"
        OUTPUT_VARIABLE out OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        set(out "")
    endif()
    # The path is the last line: anything else vcpkg ever writes to stdout while
    # exiting 0 would otherwise be read as part of it, and turn a resolution
    # under the downloads root into a false "outside the downloads root".
    string(REGEX REPLACE "^.*\n" "" out "${out}")
    file(TO_CMAKE_PATH "${out}" out)
    set(${outVar} "${out}" PARENT_SCOPE)
endfunction()

# Prefix test over two CMake-style paths. Windows spells one directory in
# whatever case each producer used — vcpkg echoes the root it was handed, the
# environment supplies another — so compare case-insensitively there.
function(z_ge_vcpkg_path_is_under outVar path prefix)
    if("${path}" STREQUAL "" OR "${prefix}" STREQUAL "")
        set(${outVar} FALSE PARENT_SCOPE)
        return()
    endif()
    if(CMAKE_HOST_WIN32)
        string(TOLOWER "${path}" path)
        string(TOLOWER "${prefix}" prefix)
    endif()
    cmake_path(IS_PREFIX prefix "${path}" NORMALIZE under)
    set(${outVar} "${under}" PARENT_SCOPE)
endfunction()
