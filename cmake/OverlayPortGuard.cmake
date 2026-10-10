# Overlay-port freshness guard.
#
# The overlay ports in cmake/ports/, and on macOS those in cmake/macos-ports/
# (GE_VCPKG_MACOS_OVERLAY_PORTS, set by Dependencies.cmake for *-osx
# triplets), only reach a build if the vcpkg install
# root the build links against was populated AFTER the port changed. Tracking
# the port files as configure dependencies (see the CMAKE_CONFIGURE_DEPENDS
# block in the top-level CMakeLists.txt) makes an edit trigger a reconfigure,
# but a reconfigure installs nothing in a tree with VCPKG_MANIFEST_INSTALL=OFF,
# so such a tree silently links the pre-patch library forever. This guard
# closes that gap: at the end of dependency setup it compares every overlay
# port's declared version + port-version against what
# ${VCPKG_INSTALLED_DIR}/vcpkg/status records as installed, and hard-fails the
# configure when the installed one is older.
#
# The guard relies on the companion policy in cmake/ports/README.md: every
# behavioural edit to an overlay port bumps its "port-version". An unbumped
# edit is invisible to vcpkg and to this guard alike.
#
# ORDERING: ge_check_overlay_port_freshness() must run after every
# configure-time installer has finished:
#   1. the vcpkg toolchain's manifest install, which runs inside project()
#      when VCPKG_MANIFEST_INSTALL=ON, and
#   2. verify_dependencies() (cmake/Dependencies.cmake), which runs a manifest
#      install into the build tree's own vcpkg_installed unless
#      GE_SKIP_VCPKG_VERIFY=ON.
# Called any earlier it would compare against the pre-install state and fail a
# perfectly good fresh configure.

# Escape hatch: downgrades the stale-port hard failure to a warning that
# repeats on EVERY configure, so a mid-task build can get through without the
# guard being deleted. The build links the pre-patch library while this is ON.
# See cmake/ports/README.md.
option(GE_ALLOW_STALE_OVERLAY_PORTS
    "Configure despite stale overlay ports (warns every configure; links the OLD libraries)"
    OFF)

# Orders two vcpkg version texts of the same declared port.
# Result (in ${outVar}): OLDER / NEWER (installed relative to declared),
# EQUAL, or UNORDERABLE (mixed or free-form version schemes).
function(z_ge_overlay_version_order installed declared outVar)
    if(installed STREQUAL declared)
        set(${outVar} EQUAL PARENT_SCOPE)
        return()
    endif()
    # Dotted-numeric ("version"/"version-semver" without prerelease tags).
    if(installed MATCHES "^[0-9]+(\\.[0-9]+)*$" AND declared MATCHES "^[0-9]+(\\.[0-9]+)*$")
        if(installed VERSION_LESS declared)
            set(${outVar} OLDER PARENT_SCOPE)
        else()
            set(${outVar} NEWER PARENT_SCOPE)
        endif()
        return()
    endif()
    # "version-date" (YYYY-MM-DD): fixed-width, so lexicographic order is
    # chronological order.
    if(installed MATCHES "^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]$"
       AND declared MATCHES "^[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]$")
        if(installed STRLESS declared)
            set(${outVar} OLDER PARENT_SCOPE)
        else()
            set(${outVar} NEWER PARENT_SCOPE)
        endif()
        return()
    endif()
    # "version-string" or mixed schemes: no defined order.
    set(${outVar} UNORDERABLE PARENT_SCOPE)
endfunction()

function(ge_check_overlay_port_freshness)
    if(NOT DEFINED VCPKG_INSTALLED_DIR)
        return() # not a vcpkg configure — nothing links these ports
    endif()

    set(statusFile "${VCPKG_INSTALLED_DIR}/vcpkg/status")
    if(NOT EXISTS "${statusFile}")
        # Unpopulated install root: nothing to compare against. Say so —
        # "not checked" must never be mistakable for "checked and current".
        message(STATUS
            "Overlay ports: no vcpkg status database in ${VCPKG_INSTALLED_DIR} "
            "— freshness NOT checked (install root not populated yet)")
        return()
    endif()
    file(READ "${statusFile}" statusText)
    # Normalize so every paragraph is "\nPackage: ...\n(line\n)*" regardless of
    # position in the file, and CRLF cannot break line-anchored matching.
    string(REPLACE "\r\n" "\n" statusText "${statusText}")
    set(statusText "\n${statusText}\n")
    # A ";" in the data (a Description carries prose) would otherwise split the
    # matched paragraph into two list elements, neither of which holds both the
    # Version and the Status line, and the port would read as not installed.
    string(REPLACE ";" "\;" statusText "${statusText}")

    set(portManifestGlobs "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ports/*/vcpkg.json")
    if(GE_VCPKG_MACOS_OVERLAY_PORTS)
        list(APPEND portManifestGlobs "${GE_VCPKG_MACOS_OVERLAY_PORTS}/*/vcpkg.json")
    endif()
    file(GLOB portManifests ${portManifestGlobs})
    set(checkedCount 0)
    set(mismatchCount 0)
    set(staleCount 0)
    foreach(manifestPath IN LISTS portManifests)
        file(READ "${manifestPath}" manifestJson)
        # The port's directory as the messages name it: cmake/ports/<port> or
        # cmake/macos-ports/<port>.
        cmake_path(GET manifestPath PARENT_PATH portDir)
        file(RELATIVE_PATH portDirText "${CMAKE_CURRENT_SOURCE_DIR}" "${portDir}")

        string(JSON portName ERROR_VARIABLE jsonError GET "${manifestJson}" "name")
        if(NOT jsonError STREQUAL "NOTFOUND")
            message(WARNING
                "Overlay port guard: cannot read \"name\" from ${manifestPath} (${jsonError}); "
                "skipping freshness check for this port.")
            continue()
        endif()

        set(declaredVersion "")
        foreach(versionKey version version-semver version-date version-string)
            string(JSON value ERROR_VARIABLE jsonError GET "${manifestJson}" "${versionKey}")
            if(jsonError STREQUAL "NOTFOUND")
                set(declaredVersion "${value}")
                break()
            endif()
        endforeach()
        if(declaredVersion STREQUAL "")
            message(WARNING
                "Overlay port guard: ${manifestPath} declares no version "
                "(none of version/version-semver/version-date/version-string); "
                "skipping freshness check for '${portName}'.")
            continue()
        endif()

        string(JSON declaredPortVersion ERROR_VARIABLE jsonError GET "${manifestJson}" "port-version")
        if(NOT jsonError STREQUAL "NOTFOUND")
            set(declaredPortVersion 0)
        endif()

        # Find the port's core status paragraph (feature paragraphs repeat the
        # package name with a "Feature:" line; skip those). A paragraph is
        # adopted wholesale — Version and Port-Version always come from the
        # same paragraph, never spliced across two (e.g. two architectures of
        # the same port when VCPKG_TARGET_TRIPLET is unset).
        set(installedVersion "")
        set(installedPortVersion 0)
        set(portInstalled FALSE)
        string(REGEX MATCHALL "\nPackage: ${portName}\n([^\n]+\n)*" paragraphs "${statusText}")
        foreach(paragraph IN LISTS paragraphs)
            if(paragraph MATCHES "\nFeature: ")
                continue()
            endif()
            if(NOT paragraph MATCHES "\nStatus: install ok installed\n")
                continue()
            endif()
            if(DEFINED VCPKG_TARGET_TRIPLET
               AND NOT paragraph MATCHES "\nArchitecture: ${VCPKG_TARGET_TRIPLET}\n")
                continue()
            endif()
            if(NOT paragraph MATCHES "\nVersion: ([^\n]*)\n")
                continue()
            endif()
            set(installedVersion "${CMAKE_MATCH_1}")
            set(installedPortVersion 0)
            set(portInstalled TRUE)
            if(paragraph MATCHES "\nPort-Version: ([0-9]+)\n")
                set(installedPortVersion "${CMAKE_MATCH_1}")
            endif()
        endforeach()

        if(NOT portInstalled)
            continue() # not installed at all: vcpkg installs it on first use
        endif()
        math(EXPR checkedCount "${checkedCount} + 1")

        z_ge_overlay_version_order("${installedVersion}" "${declaredVersion}" versionOrder)
        set(stale FALSE)
        if(versionOrder STREQUAL "EQUAL")
            if(installedPortVersion LESS declaredPortVersion)
                set(stale TRUE)
            elseif(installedPortVersion GREATER declaredPortVersion)
                math(EXPR mismatchCount "${mismatchCount} + 1")
                message(WARNING
                    "Overlay port guard: '${portName}' is installed as "
                    "${installedVersion}#${installedPortVersion} but ${portDirText} declares only "
                    "${declaredVersion}#${declaredPortVersion}. The install root is NEWER than "
                    "this source tree (older branch against a newer ${VCPKG_INSTALLED_DIR}?). "
                    "Dependencies will link the newer build of the port.")
            endif()
        elseif(versionOrder STREQUAL "OLDER")
            set(stale TRUE)
        elseif(versionOrder STREQUAL "NEWER")
            math(EXPR mismatchCount "${mismatchCount} + 1")
            message(WARNING
                "Overlay port guard: '${portName}' is installed as "
                "${installedVersion}#${installedPortVersion} but ${portDirText} declares "
                "${declaredVersion}#${declaredPortVersion}. The install root is NEWER than "
                "this source tree (older branch against a newer ${VCPKG_INSTALLED_DIR}?). "
                "Dependencies will link the newer build of the port.")
        else() # UNORDERABLE
            math(EXPR mismatchCount "${mismatchCount} + 1")
            message(WARNING
                "Overlay port guard: '${portName}' is installed as "
                "${installedVersion}#${installedPortVersion} but ${portDirText} declares "
                "${declaredVersion}#${declaredPortVersion}, and the two version texts have no "
                "defined order — cannot tell which is newer. If the port was just edited, "
                "treat this as stale and reinstall (see cmake/ports/README.md).")
        endif()

        if(stale)
            math(EXPR staleCount "${staleCount} + 1")
            # Ownership test is case-insensitive on Windows: CMake normalizes
            # CMAKE_BINARY_DIR to on-disk case, while a -DVCPKG_INSTALLED_DIR
            # keeps the caller's spelling, and the two must still compare equal.
            set(ownershipBinaryDir "${CMAKE_BINARY_DIR}")
            set(ownershipInstalledDir "${VCPKG_INSTALLED_DIR}")
            if(CMAKE_HOST_WIN32)
                string(TOLOWER "${ownershipBinaryDir}" ownershipBinaryDir)
                string(TOLOWER "${ownershipInstalledDir}" ownershipInstalledDir)
            endif()
            cmake_path(IS_PREFIX ownershipBinaryDir "${ownershipInstalledDir}" NORMALIZE ownInstallRoot)
            if(ownInstallRoot)
                set(fixText
                    "  Fix: reconfigure with manifest installs enabled so vcpkg upgrades the port:\n"
                    "      cmake --preset <this preset> -DVCPKG_MANIFEST_INSTALL=ON\n"
                    "  (and without -DGE_SKIP_VCPKG_VERIFY=ON, which also suppresses installs).")
            else()
                # A redirected root conventionally sits at
                # <source>/build/<preset>/vcpkg_installed; when that shape
                # holds, name the owning tree and preset concretely.
                cmake_path(GET VCPKG_INSTALLED_DIR PARENT_PATH ownerBuildDir)
                cmake_path(GET ownerBuildDir FILENAME ownerPreset)
                cmake_path(GET ownerBuildDir PARENT_PATH ownerBuildRoot)
                cmake_path(GET ownerBuildRoot FILENAME ownerBuildRootName)
                cmake_path(GET ownerBuildRoot PARENT_PATH ownerSourceDir)
                if(ownerBuildRootName STREQUAL "build" AND IS_DIRECTORY "${ownerSourceDir}")
                    set(ownerFixLine
                        "      cmake --preset ${ownerPreset}        # run in ${ownerSourceDir}\n")
                else()
                    set(ownerFixLine
                        "      (reconfigure whichever tree populated ${VCPKG_INSTALLED_DIR})\n")
                endif()
                set(fixText
                    "  This build redirects VCPKG_INSTALLED_DIR at an install root it does not own.\n"
                    "  Fix: reconfigure the source tree that OWNS that root (with manifest installs\n"
                    "  enabled) so vcpkg upgrades the port there — for this root:\n"
                    ${ownerFixLine}
                    "  Do NOT run a vcpkg install against a shared root from this tree.")
            endif()
            string(CONCAT staleText
                "Stale overlay port: '${portName}' is installed as "
                "${installedVersion}#${installedPortVersion} but ${portDirText} declares "
                "${declaredVersion}#${declaredPortVersion}.\n"
                "  The vcpkg install root this build links against\n"
                "      ${VCPKG_INSTALLED_DIR}\n"
                "  was populated before that port change, and nothing in this configure will\n"
                "  reinstall it, so every dependency consuming '${portName}' would silently link\n"
                "  the OLD library.\n"
                ${fixText}
                "
  If you must ship one build past this, -DGE_ALLOW_STALE_OVERLAY_PORTS=ON
"
                "  downgrades it to a warning that repeats every configure. It does not fix
"
                "  anything; the build still links the OLD library. See cmake/ports/README.md.
")
            if(GE_ALLOW_STALE_OVERLAY_PORTS)
                message(WARNING
                    "${staleText}\n"
                    "  GE_ALLOW_STALE_OVERLAY_PORTS is ON: continuing anyway — this build links the\n"
                    "  OLD '${portName}'. It is a cache variable: reconfigure with\n"
                    "  -DGE_ALLOW_STALE_OVERLAY_PORTS=OFF once the install root is current.")
            else()
                message(FATAL_ERROR "${staleText}")
            endif()
        endif()
    endforeach()

    # The summary must never read as an all-clear for a comparison that did
    # not happen: "current" is only claimed for ports actually checked, and a
    # zero-check run says NOT checked.
    list(LENGTH portManifests portCount)

    # An armed-but-clean hatch is the quiet way this guard stops guarding: a
    # cache variable set once during a real staleness outlives it and silently
    # downgrades every future hard-fail in this tree. While stale it is loud
    # already; clean is the case that needs the nag.
    if(GE_ALLOW_STALE_OVERLAY_PORTS AND staleCount EQUAL 0)
        message(STATUS
            "Overlay ports: GE_ALLOW_STALE_OVERLAY_PORTS is ON but nothing is stale — "
            "this tree cannot hard-fail on a stale port. Reconfigure with "
            "-DGE_ALLOW_STALE_OVERLAY_PORTS=OFF to re-arm the guard.")
    endif()

    if(staleCount GREATER 0)
        message(STATUS
            "Overlay ports: ${checkedCount}/${portCount} checked in ${VCPKG_INSTALLED_DIR} "
            "— ${staleCount} STALE, bypassed by GE_ALLOW_STALE_OVERLAY_PORTS (see warnings above)")
    elseif(mismatchCount GREATER 0)
        message(STATUS
            "Overlay ports: ${checkedCount}/${portCount} checked in ${VCPKG_INSTALLED_DIR} "
            "— ${mismatchCount} mismatched (see warnings above)")
    elseif(checkedCount EQUAL 0)
        message(STATUS
            "Overlay ports: none of the ${portCount} overlay ports are installed in "
            "${VCPKG_INSTALLED_DIR} — freshness NOT checked (vcpkg installs them on first use)")
    elseif(checkedCount LESS portCount)
        math(EXPR uncheckedCount "${portCount} - ${checkedCount}")
        message(STATUS
            "Overlay ports: ${checkedCount}/${portCount} installed in ${VCPKG_INSTALLED_DIR} "
            "and current — ${uncheckedCount} not installed, NOT checked "
            "(vcpkg installs them on first use)")
    else()
        message(STATUS
            "Overlay ports: all ${portCount} installed in ${VCPKG_INSTALLED_DIR} and current")
    endif()
endfunction()
