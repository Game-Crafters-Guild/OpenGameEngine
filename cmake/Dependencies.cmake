# GameEngine Automated Dependency Management
# This module handles automatic setup of third-party dependencies

# What vcpkg itself is — the commit a provisioned checkout lands on, and the
# tool versions it resolves — is pinned, because both enter every port's ABI
# hash and so decide whether this tree reads the shared binary cache at all.
include("${CMAKE_CURRENT_LIST_DIR}/VcpkgPin.cmake")
# The compiler is an input to every port's ABI hash too, and vcpkg resolves it
# from the environment of whichever process starts it.
include("${CMAKE_CURRENT_LIST_DIR}/VcpkgHostCompiler.cmake")

# In some workflows (e.g. lightweight Docker CI/test harnesses) we may want
# to skip the expensive vcpkg verify step and assume dependencies are already
# provisioned. Expose a toggle so callers can opt out without forking the
# entire dependency pipeline.
option(GE_SKIP_VCPKG_VERIFY "Skip verify_dependencies() vcpkg install step (assumes dependencies are already installed)" OFF)

# Detect architecture and OS for vcpkg triplets based on target platform
if(WIN32)
	set(_VCPKG_OS_SUFFIX "windows")
elseif(UNIX AND NOT APPLE)
	set(_VCPKG_OS_SUFFIX "linux")
elseif(APPLE)
	set(_VCPKG_OS_SUFFIX "osx")
else()
	message(FATAL_ERROR
		"Unsupported platform for automatic vcpkg triplet selection "
		"(CMAKE_SYSTEM_NAME='${CMAKE_SYSTEM_NAME}'). "
		"Override by setting VCPKG_TARGET_TRIPLET and CMAKE_TOOLCHAIN_FILE manually.")
endif()

if(CMAKE_GENERATOR_PLATFORM)
	# Use the explicitly set platform from CMake generator
	if(CMAKE_GENERATOR_PLATFORM MATCHES "ARM64|arm64")
		set(VCPKG_ARCH "arm64-${_VCPKG_OS_SUFFIX}")
		message(STATUS "Using ARM64 vcpkg packages (target platform: ${CMAKE_GENERATOR_PLATFORM}, os: ${_VCPKG_OS_SUFFIX})")
	elseif(CMAKE_GENERATOR_PLATFORM MATCHES "x64|X64")
		set(VCPKG_ARCH "x64-${_VCPKG_OS_SUFFIX}")
		message(STATUS "Using x64 vcpkg packages (target platform: ${CMAKE_GENERATOR_PLATFORM}, os: ${_VCPKG_OS_SUFFIX})")
	elseif(CMAKE_GENERATOR_PLATFORM MATCHES "Win32|x86")
		set(VCPKG_ARCH "x86-${_VCPKG_OS_SUFFIX}")
		message(STATUS "Using x86 vcpkg packages (target platform: ${CMAKE_GENERATOR_PLATFORM}, os: ${_VCPKG_OS_SUFFIX})")
	else()
		set(VCPKG_ARCH "x64-${_VCPKG_OS_SUFFIX}")
		message(STATUS "Using x64 vcpkg packages (unknown target platform: ${CMAKE_GENERATOR_PLATFORM}, os: ${_VCPKG_OS_SUFFIX})")
	endif()
	else()
        # Fallback to system/host detection (used on Ninja/Unix Makefiles, etc.)
        # CMAKE_SIZEOF_VOID_P is not reliably initialized before project(), so rely on
        # the reported processor instead.
        #
        # Note: On Apple Silicon, CMAKE_SYSTEM_PROCESSOR is often empty this early,
        # so fall back to CMAKE_HOST_SYSTEM_PROCESSOR when needed. If both are empty
        # (which can happen in some toolchain setups), default to ARM64 on Apple,
        # since modern macOS machines are predominantly Apple Silicon.
        set(_GE_DETECT_PROC "${CMAKE_SYSTEM_PROCESSOR}")
        if(_GE_DETECT_PROC STREQUAL "")
            set(_GE_DETECT_PROC "${CMAKE_HOST_SYSTEM_PROCESSOR}")
        endif()

        if(_GE_DETECT_PROC STREQUAL "")
            if(APPLE)
                # On Apple with unknown processor, default to arm64 (Apple Silicon).
                set(VCPKG_ARCH "arm64-${_VCPKG_OS_SUFFIX}")
                message(STATUS "Using ARM64 vcpkg packages (Apple default, processor: unknown, os: ${_VCPKG_OS_SUFFIX})")
            else()
                # Non-Apple host with unknown processor: fall back to x64.
                set(VCPKG_ARCH "x64-${_VCPKG_OS_SUFFIX}")
                message(STATUS "Using x64 vcpkg packages (system detection, processor: unknown, os: ${_VCPKG_OS_SUFFIX})")
            endif()
        # Include common ARM spellings (arm64, ARM64, aarch64).
        elseif(_GE_DETECT_PROC MATCHES "ARM64|arm64|aarch64")
            set(VCPKG_ARCH "arm64-${_VCPKG_OS_SUFFIX}")
            message(STATUS "Using ARM64 vcpkg packages (system detection, processor: ${_GE_DETECT_PROC}, os: ${_VCPKG_OS_SUFFIX})")
        elseif(_GE_DETECT_PROC MATCHES "^(i.86|x86)$")
            # True 32-bit x86 host
            set(VCPKG_ARCH "x86-${_VCPKG_OS_SUFFIX}")
            message(STATUS "Using x86 vcpkg packages (system detection, processor: ${_GE_DETECT_PROC}, os: ${_VCPKG_OS_SUFFIX})")
        elseif(_GE_DETECT_PROC MATCHES "x86_64|X86_64|amd64|AMD64")
            # 64-bit x86 hosts (Intel/AMD)
            set(VCPKG_ARCH "x64-${_VCPKG_OS_SUFFIX}")
            message(STATUS "Using x64 vcpkg packages (system detection, processor: ${_GE_DETECT_PROC}, os: ${_VCPKG_OS_SUFFIX})")
        else()
            # For any other reported processor value, choose a sensible default.
            if(APPLE)
                # On Apple, bias towards ARM64 for unknown strings (Apple Silicon).
                set(VCPKG_ARCH "arm64-${_VCPKG_OS_SUFFIX}")
                message(STATUS "Using ARM64 vcpkg packages (Apple default, processor: ${_GE_DETECT_PROC}, os: ${_VCPKG_OS_SUFFIX})")
            else()
                set(VCPKG_ARCH "x64-${_VCPKG_OS_SUFFIX}")
                message(STATUS "Using x64 vcpkg packages (system detection, processor: ${_GE_DETECT_PROC}, os: ${_VCPKG_OS_SUFFIX})")
            endif()
        endif()
    endif()

# Enable vcpkg manifest mode so MSBuild integration injects AdditionalDependencies
set(VCPKG_MANIFEST_MODE ON CACHE BOOL "" FORCE)
set(VCPKG_FEATURE_FLAGS "manifests" CACHE STRING "" FORCE)
# Propagate the computed triplet unless the caller/preset already pinned one
# (e.g. host arm64 ShaderReflect while cross-compiling x64 Steam Deck).
if(NOT DEFINED CACHE{VCPKG_TARGET_TRIPLET} OR VCPKG_TARGET_TRIPLET STREQUAL "")
    set(VCPKG_TARGET_TRIPLET "${VCPKG_ARCH}" CACHE STRING "" FORCE)
else()
    message(STATUS "Using explicit VCPKG_TARGET_TRIPLET: ${VCPKG_TARGET_TRIPLET}")
    # Every later install/verify must target the pinned triplet, not the
    # host-derived guess — with a cross triplet (wasm32-emscripten,
    # x64-steamdeck) the guess would install a whole second dependency set for
    # the wrong platform.
    set(VCPKG_ARCH "${VCPKG_TARGET_TRIPLET}")
endif()
# Explicitly point vcpkg to the manifest location (repo root)
set(VCPKG_MANIFEST_DIR "${CMAKE_SOURCE_DIR}" CACHE PATH "" FORCE)

# Overlay ports for macOS triplets only. vcpkg-configuration.json hands
# cmake/ports to every triplet, and an overlay re-keys its port's ABI on every
# triplet it reaches, so a port changed for macOS alone lives in
# cmake/macos-ports. It reaches vcpkg through the VCPKG_OVERLAY_PORTS
# environment variable, which every vcpkg install this configure runs inherits:
# the toolchain's manifest install at project(), install_required_packages()
# and verify_dependencies(). It is not wired through a preset the way the
# steamdeck presets pass cmake/steamdeck-ports, because a macOS configure needs
# it with or without a preset: vcpkg.json requests directxtex on osx, and a
# configure without the overlay fails ("directxtex is only supported on ...").
# Nor is it the toolchain's VCPKG_OVERLAY_PORTS variable, which the toolchain
# stores in CMakeCache.txt: a build directory that then configures a checkout
# without the directory fails with "Overlay path ... must be an existing
# directory". Windows and Linux keep the registry ports and their installed
# ABIs. The freshness guard (cmake/OverlayPortGuard.cmake) and the configure
# dependencies cover the directory on macOS. See cmake/ports/README.md.
set(GE_VCPKG_MACOS_OVERLAY_PORTS "")
if(VCPKG_ARCH MATCHES "-osx")
    set(GE_VCPKG_MACOS_OVERLAY_PORTS "${CMAKE_SOURCE_DIR}/cmake/macos-ports")
    # vcpkg splits the variable on the host's path-list separator, ':' on macOS.
    if("$ENV{VCPKG_OVERLAY_PORTS}" STREQUAL "")
        set(ENV{VCPKG_OVERLAY_PORTS} "${GE_VCPKG_MACOS_OVERLAY_PORTS}")
    else()
        set(ENV{VCPKG_OVERLAY_PORTS} "${GE_VCPKG_MACOS_OVERLAY_PORTS}:$ENV{VCPKG_OVERLAY_PORTS}")
    endif()
endif()

# vcpkg fails early if VCPKG_DEFAULT_BINARY_CACHE names a path that is not a
# directory, so create it when an override is set. With no override vcpkg uses
# its own per-user cache (%LOCALAPPDATA%/vcpkg/archives on Windows,
# ~/.cache/vcpkg/archives elsewhere), which every worktree shares.
if(DEFINED ENV{VCPKG_DEFAULT_BINARY_CACHE} AND NOT "$ENV{VCPKG_DEFAULT_BINARY_CACHE}" STREQUAL "")
    # Normalize path separators to a CMake-style path.
    file(TO_CMAKE_PATH "$ENV{VCPKG_DEFAULT_BINARY_CACHE}" _GE_VCPKG_CACHE_DIR)
    if(NOT IS_DIRECTORY "${_GE_VCPKG_CACHE_DIR}")
        if(EXISTS "${_GE_VCPKG_CACHE_DIR}")
            message(FATAL_ERROR
                "VCPKG_DEFAULT_BINARY_CACHE is set to '${_GE_VCPKG_CACHE_DIR}', "
                "which is not a directory. Please fix this path or remove the file.")
        endif()
        file(MAKE_DIRECTORY "${_GE_VCPKG_CACHE_DIR}")
        message(STATUS "Created vcpkg binary cache directory at: ${_GE_VCPKG_CACHE_DIR}")
    endif()
endif()



# Required packages list with architecture detection (informational; manifest drives installs)
set(REQUIRED_PACKAGES
    "gtest:${VCPKG_ARCH}"
    "concurrentqueue:${VCPKG_ARCH}"
    "nlohmann-json:${VCPKG_ARCH}"
    "benchmark:${VCPKG_ARCH}"
    "freetype:${VCPKG_ARCH}"
    "harfbuzz:${VCPKG_ARCH}"
    "glm:${VCPKG_ARCH}"
    "shaderc:${VCPKG_ARCH}"
    "vulkan:${VCPKG_ARCH}"
    # Add more packages here as needed
    # "fmt:${VCPKG_ARCH}"
    # "spdlog:${VCPKG_ARCH}"
)

# Platform detection
if(WIN32)
    set(VCPKG_BOOTSTRAP_SCRIPT "bootstrap-vcpkg.bat")
    set(VCPKG_EXECUTABLE "vcpkg.exe")
elseif(UNIX AND NOT APPLE)
    set(VCPKG_BOOTSTRAP_SCRIPT "bootstrap-vcpkg.sh")
    set(VCPKG_EXECUTABLE "vcpkg")
elseif(APPLE)
    set(VCPKG_BOOTSTRAP_SCRIPT "bootstrap-vcpkg.sh")
    set(VCPKG_EXECUTABLE "vcpkg")
else()
    message(FATAL_ERROR
        "Unsupported platform for automatic vcpkg setup "
        "(CMAKE_SYSTEM_NAME='${CMAKE_SYSTEM_NAME}'). "
        "Provide your own vcpkg by setting CMAKE_TOOLCHAIN_FILE to an existing "
        "scripts/buildsystems/vcpkg.cmake and VCPKG_TARGET_TRIPLET to a valid triplet.")
endif()

# Dependency paths
cmake_path(SET DEPENDENCIES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dependencies")
cmake_path(SET VCPKG_DIR "${DEPENDENCIES_DIR}/vcpkg")
# A tree configured with another checkout's vcpkg toolchain (a worktree given the
# primary checkout's scripts/buildsystems/vcpkg.cmake) has no vcpkg of its own:
# dependencies/vcpkg is untracked. Every vcpkg call below then uses the checkout
# that toolchain belongs to, the same vcpkg its manifest install at project() runs.
if(DEFINED CMAKE_TOOLCHAIN_FILE AND NOT CMAKE_TOOLCHAIN_FILE STREQUAL "")
    cmake_path(ABSOLUTE_PATH CMAKE_TOOLCHAIN_FILE BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
               NORMALIZE OUTPUT_VARIABLE _ge_toolchain_path)
    cmake_path(GET _ge_toolchain_path PARENT_PATH _ge_toolchain_dir)
    cmake_path(GET _ge_toolchain_dir PARENT_PATH _ge_toolchain_scripts_dir)
    cmake_path(GET _ge_toolchain_scripts_dir PARENT_PATH _ge_toolchain_vcpkg_root)
    if(EXISTS "${_ge_toolchain_path}"
       AND _ge_toolchain_path MATCHES "/scripts/buildsystems/vcpkg[.]cmake$")
        set(VCPKG_DIR "${_ge_toolchain_vcpkg_root}")
    endif()
    unset(_ge_toolchain_path)
    unset(_ge_toolchain_dir)
    unset(_ge_toolchain_scripts_dir)
    unset(_ge_toolchain_vcpkg_root)
endif()
cmake_path(SET VCPKG_TOOLCHAIN_FILE "${VCPKG_DIR}/scripts/buildsystems/vcpkg.cmake")
cmake_path(SET VCPKG_EXECUTABLE_PATH "${VCPKG_DIR}/${VCPKG_EXECUTABLE}")

# Prime a cold vcpkg binary cache from the repo's `vcpkg-cache` GitHub release
# so a first configure restores prebuilt ports instead of compiling them from
# source. Called before project(), so it covers every configure entry point:
# `cmake --preset`, IDE configures, and CI.
#
# Cold = no archive under the directory the effective VCPKG_BINARY_SOURCES
# actually consults (an ABI-exact check would need a bootstrapped vcpkg, which
# a fresh clone doesn't have yet). Warm caches return after a glob. Every
# failure path is non-fatal: the configure proceeds and vcpkg builds
# third-party code from source. Opt out with GE_NO_CACHE_FETCH=1.
function(prime_vcpkg_binary_cache)
    if(DEFINED ENV{GE_NO_CACHE_FETCH} AND NOT "$ENV{GE_NO_CACHE_FETCH}" STREQUAL ""
       AND NOT "$ENV{GE_NO_CACHE_FETCH}" STREQUAL "0")
        message(STATUS "vcpkg binary cache priming skipped (GE_NO_CACHE_FETCH is set)")
        return()
    endif()

    # Resolve the local cache directory the configured binary sources read.
    # A `files,<path>` entry (CI points one at the workspace; a machine
    # environment can add a shared archive) wins over the per-user `default`
    # location; a config with only remote providers (http/nuget/...) has
    # nothing local to prime.
    set(_sources "$ENV{VCPKG_BINARY_SOURCES}")
    set(_cache_dir "")
    if(_sources MATCHES "(^|;)files,([^;,]+)")
        set(_cache_dir "${CMAKE_MATCH_2}")
    elseif(_sources STREQUAL "" OR _sources MATCHES "(^|;)default(,|;|$)")
        if(DEFINED ENV{VCPKG_DEFAULT_BINARY_CACHE} AND NOT "$ENV{VCPKG_DEFAULT_BINARY_CACHE}" STREQUAL "")
            set(_cache_dir "$ENV{VCPKG_DEFAULT_BINARY_CACHE}")
        elseif(WIN32)
            set(_cache_dir "$ENV{LOCALAPPDATA}/vcpkg/archives")
        elseif(DEFINED ENV{XDG_CACHE_HOME} AND NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
            set(_cache_dir "$ENV{XDG_CACHE_HOME}/vcpkg/archives")
        else()
            set(_cache_dir "$ENV{HOME}/.cache/vcpkg/archives")
        endif()
    else()
        return()
    endif()
    file(TO_CMAKE_PATH "${_cache_dir}" _cache_dir)

    # vcpkg shards archives as <2-hex-char prefix>/<abi>.zip; accept a flat
    # layout too.
    file(GLOB _archives "${_cache_dir}/*.zip" "${_cache_dir}/*/*.zip")
    if(_archives)
        return()
    endif()

    find_program(_gh_cli NAMES gh NO_CACHE)
    if(NOT _gh_cli)
        message(STATUS
            "vcpkg binary cache is cold and GitHub CLI (gh) is not installed; this configure "
            "compiles third-party ports from source (tens of minutes). To prime instead: "
            "install gh, run 'gh auth login', and reconfigure — or run "
            "Tools/Scripts/fetch-vcpkg-cache.ps1 (.sh on macOS/Linux) manually.")
        return()
    endif()

    set(_triplet "${VCPKG_TARGET_TRIPLET}")
    if(_triplet STREQUAL "")
        set(_triplet "${VCPKG_ARCH}")
    endif()

    message(STATUS
        "vcpkg binary cache is cold — fetching prebuilt '${_triplet}' archives from the "
        "'vcpkg-cache' release into ${_cache_dir} (opt out: GE_NO_CACHE_FETCH=1)...")

    if(WIN32)
        find_program(_ps NAMES pwsh powershell NO_CACHE)
        set(_fetch_cmd "${_ps}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Tools/Scripts/fetch-vcpkg-cache.ps1" -Triplet "${_triplet}")
    else()
        set(_fetch_cmd bash "${CMAKE_CURRENT_SOURCE_DIR}/Tools/Scripts/fetch-vcpkg-cache.sh" --triplet "${_triplet}")
    endif()
    # Output streams to the console so download progress and script errors stay
    # visible. VCPKG_DEFAULT_BINARY_CACHE pins the script's destination to the
    # directory resolved above (which may come from a `files,` source rather
    # than the environment).
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env "VCPKG_DEFAULT_BINARY_CACHE=${_cache_dir}" ${_fetch_cmd}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE _fetch_rc
    )
    if(NOT _fetch_rc EQUAL 0)
        message(WARNING
            "vcpkg cache fetch failed (gh auth? network? no archive published for triplet "
            "'${_triplet}'?). Continuing — third-party ports build from source. Manual "
            "command: Tools/Scripts/fetch-vcpkg-cache.ps1 (.sh on macOS/Linux)")
    endif()
endfunction()

# Function to check if vcpkg is properly installed
function(check_vcpkg_installation)
    if(NOT EXISTS "${VCPKG_EXECUTABLE_PATH}")
        message(STATUS "vcpkg executable not found at: ${VCPKG_EXECUTABLE_PATH}")
        return()
    endif()

    if(NOT EXISTS "${VCPKG_TOOLCHAIN_FILE}")
        message(STATUS "vcpkg toolchain file not found at: ${VCPKG_TOOLCHAIN_FILE}")
        return()
    endif()

    set(VCPKG_READY TRUE PARENT_SCOPE)
endfunction()

# Function to clone and bootstrap vcpkg
function(setup_vcpkg)
    message(STATUS "=== GameEngine Dependency Setup ===")
    message(STATUS "Setting up automated dependency management...")

    # Create dependencies directory
    if(NOT EXISTS "${DEPENDENCIES_DIR}")
        file(MAKE_DIRECTORY "${DEPENDENCIES_DIR}")
        message(STATUS "Created dependencies directory: ${DEPENDENCIES_DIR}")
    endif()

    # Clone vcpkg if it doesn't exist
    if(NOT EXISTS "${VCPKG_DIR}")
        message(STATUS "Cloning vcpkg from Microsoft repository...")
        execute_process(
            COMMAND git clone https://github.com/Microsoft/vcpkg.git "${VCPKG_DIR}"
            RESULT_VARIABLE GIT_RESULT
            OUTPUT_VARIABLE GIT_OUTPUT
            ERROR_VARIABLE GIT_ERROR
        )

        if(NOT GIT_RESULT EQUAL 0)
            message(FATAL_ERROR
                "Failed to auto-clone vcpkg into ${VCPKG_DIR}.\n"
                "  Git error:\n"
                "    ${GIT_ERROR}\n"
                "\n"
                "  To unblock the build, you can:\n"
                "    - Reuse an existing vcpkg by pointing VCPKG_ROOT at it and switching to\n"
                "      a non-local preset (e.g. vs2022-x64-unity or vs2026-x64-unity).\n"
                "    - Manually clone vcpkg yourself at the pinned commit, then re-run cmake:\n"
                "        git clone https://github.com/Microsoft/vcpkg.git ${VCPKG_DIR}\n"
                "        git -C ${VCPKG_DIR} checkout --detach ${GE_VCPKG_COMMIT}\n"
                "    - Ensure 'git' is installed and on PATH if it is not already.")
        endif()

        message(STATUS "✓ vcpkg cloned successfully")
        ge_vcpkg_checkout_pin("${VCPKG_DIR}")
    else()
        message(STATUS "✓ vcpkg directory already exists")
    endif()

    # Bootstrap vcpkg if not already done
    if(NOT EXISTS ${VCPKG_EXECUTABLE_PATH})
        message(STATUS "Bootstrapping vcpkg...")
        # Make bootstrap script executable on Unix systems
        if(UNIX OR APPLE)
            execute_process(
                COMMAND chmod +x "${VCPKG_DIR}/${VCPKG_BOOTSTRAP_SCRIPT}"
                RESULT_VARIABLE CHMOD_RESULT
            )
            if(NOT CHMOD_RESULT EQUAL 0)
                message(WARNING "Failed to make bootstrap script executable, continuing anyway...")
            endif()
        endif()
        # Run the bootstrap script
        execute_process(
            COMMAND "${VCPKG_DIR}/${VCPKG_BOOTSTRAP_SCRIPT}"
            WORKING_DIRECTORY ${VCPKG_DIR}
            RESULT_VARIABLE BOOTSTRAP_RESULT
            OUTPUT_VARIABLE BOOTSTRAP_OUTPUT
            ERROR_VARIABLE BOOTSTRAP_ERROR
        )

        if(NOT BOOTSTRAP_RESULT EQUAL 0)
            message(FATAL_ERROR
                "Failed to bootstrap vcpkg in ${VCPKG_DIR}.\n"
                "  Bootstrap error:\n"
                "    ${BOOTSTRAP_ERROR}\n"
                "\n"
                "  Common causes:\n"
                "    - Missing C++ build tools (MSVC on Windows, gcc/clang on Linux/macOS).\n"
                "    - Antivirus quarantining the bootstrap script or its temporary files.\n"
                "    - Corrupt vcpkg checkout — try removing ${VCPKG_DIR} and re-running cmake.\n"
                "  Run ${VCPKG_DIR}/${VCPKG_BOOTSTRAP_SCRIPT} manually to see the full output.")
        endif()

        message(STATUS "✓ vcpkg bootstrapped successfully")
    else()
        message(STATUS "✓ vcpkg already bootstrapped")
    endif()
endfunction()

# Function to install required packages.
# vcpkg's stdout/stderr stream live to the parent terminal so the user can see
# build progress (this can take 30+ minutes on a fresh clone). We deliberately
# do NOT capture into OUTPUT_VARIABLE/ERROR_VARIABLE because vcpkg writes its
# real failure details to stdout, and capturing them swallows the output the
# user needs to diagnose the problem.
function(install_required_packages)
    message(STATUS "Installing vcpkg manifest dependencies for ${VCPKG_ARCH} (this can take 30+ minutes on a fresh clone)...")

    # Install into the same root the vcpkg toolchain uses at project(), so it
    # sees the manifest as satisfied and skips its own install. Without an
    # explicit --x-install-root this manifest-mode run defaults to
    # <repo>/vcpkg_installed, leaving a full duplicate tree at the repo root
    # that nothing reads.
    #
    # VCPKG_INSTALLED_DIR wins when it is set. It is the same variable the
    # toolchain honours, so pinning it here keeps the two in agreement, and on
    # every reconfigure it is the toolchain's own cached value — this build
    # tree's own root. A value pointing outside this build directory is refused
    # before any installer runs (cmake/VcpkgInstallGuard.cmake): each tree keeps
    # its own install, and what the trees share is the ABI-keyed binary cache.
    if(DEFINED VCPKG_INSTALLED_DIR AND NOT VCPKG_INSTALLED_DIR STREQUAL "")
        set(_ge_vcpkg_install_root "${VCPKG_INSTALLED_DIR}")
        message(STATUS "  vcpkg install root: ${_ge_vcpkg_install_root} (from VCPKG_INSTALLED_DIR)")
    else()
        set(_ge_vcpkg_install_root "${CMAKE_BINARY_DIR}/vcpkg_installed")
    endif()

    # The clean flags drop per-port buildtrees/packages once each port is
    # installed and its binary-cache archive written (measured residue of a
    # full from-source configure: buildtrees ~10.4 GB, packages ~3.1 GB); a
    # FAILED port keeps its buildtree, so the install-*.log diagnostics below
    # stay reachable.
    execute_process(
        COMMAND ${VCPKG_EXECUTABLE_PATH} install --triplet ${VCPKG_ARCH}
                --x-install-root=${_ge_vcpkg_install_root}
                --clean-buildtrees-after-build --clean-packages-after-build
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        RESULT_VARIABLE MANIFEST_INSTALL_RESULT
    )

    if(NOT MANIFEST_INSTALL_RESULT EQUAL 0)
        message(FATAL_ERROR
            "vcpkg failed to install manifest dependencies for triplet '${VCPKG_ARCH}' (exit ${MANIFEST_INSTALL_RESULT}).\n"
            "  See vcpkg's output above for the specific failure.\n"
            "\n"
            "  Common causes:\n"
            "    - Network failure while downloading sources.\n"
            "    - A specific port failed to build — check the per-port logs in\n"
            "        ${VCPKG_DIR}/buildtrees/<port-name>/install-*.log\n"
            "    - Missing build tools (Visual Studio C++ workload, CMake, ninja, perl, etc.).\n"
            "    - Wrong triplet for the host (override with -DVCPKG_TARGET_TRIPLET=...).")
    endif()

    message(STATUS "✓ vcpkg manifest dependencies installed")
endfunction()

# Main dependency setup function
function(setup_dependencies)
    message(STATUS "Checking dependency requirements...")

    # Check if vcpkg is ready
    check_vcpkg_installation()

    if(NOT VCPKG_READY)
        message(STATUS "vcpkg not ready, setting up dependencies...")
        setup_vcpkg()
        # Before the first installer, because the tool versions it resolves are
        # part of every package's ABI hash and so of every binary-cache lookup.
        ge_seed_vcpkg_pinned_tools("${VCPKG_EXECUTABLE_PATH}")
        install_required_packages()
        # Mark that we just ran the manifest install so verify_dependencies()
        # can skip the redundant duplicate run later. Using a global property
        # because verify_dependencies() runs in a different function scope.
        set_property(GLOBAL PROPERTY GE_VCPKG_INSTALLED_THIS_CONFIGURE TRUE)
        message(STATUS "=== Dependency Setup Complete ===")
    else()
        message(STATUS "✓ Dependencies already configured")
    endif()

    # Set toolchain file for this build
    if(EXISTS ${VCPKG_TOOLCHAIN_FILE})
        set(CMAKE_TOOLCHAIN_FILE ${VCPKG_TOOLCHAIN_FILE} CACHE STRING "vcpkg toolchain file" FORCE)
        message(STATUS "✓ Using vcpkg toolchain: ${VCPKG_TOOLCHAIN_FILE}")
    else()
        message(FATAL_ERROR
            "vcpkg toolchain file not found after setup:\n"
            "    ${VCPKG_TOOLCHAIN_FILE}\n"
            "  setup_vcpkg() reported success but the expected toolchain file is missing.\n"
            "  This usually means bootstrap-vcpkg exited 0 without producing scripts/buildsystems/.\n"
            "  Try removing ${VCPKG_DIR} entirely and re-running cmake.")
    endif()
endfunction()

# Function to verify dependencies are working
function(verify_dependencies)
    if(GE_SKIP_VCPKG_VERIFY)
        message(STATUS "Skipping verify_dependencies() because GE_SKIP_VCPKG_VERIFY=ON (assuming vcpkg deps are already installed)")
        return()
    endif()

    message(STATUS "Verifying dependency installation...")

    # Skip the redundant manifest install if setup_dependencies() just ran one
    # in this configure pass. Re-running back-to-back wastes time AND is a
    # genuine extra failure point (observed: vcpkg can exit non-zero on the
    # second invocation without writing anything to stderr).
    get_property(_GE_VCPKG_FRESH GLOBAL PROPERTY GE_VCPKG_INSTALLED_THIS_CONFIGURE)
    if(_GE_VCPKG_FRESH)
        message(STATUS "✓ vcpkg manifest dependencies just installed by setup_dependencies(); skipping duplicate install")
    else()
        # Ensure all manifest dependencies are installed for this triplet before proceeding.
        # Stream output live so users see progress and any failure detail directly.
        # Same install root and cleanup flags as install_required_packages() —
        # and it must resolve the root the SAME way, or the two disagree and this
        # "verify" step silently builds a second copy of everything.
        if(DEFINED VCPKG_INSTALLED_DIR AND NOT VCPKG_INSTALLED_DIR STREQUAL "")
            set(_ge_vcpkg_install_root "${VCPKG_INSTALLED_DIR}")
        else()
            set(_ge_vcpkg_install_root "${CMAKE_BINARY_DIR}/vcpkg_installed")
        endif()
        message(STATUS "Ensuring vcpkg manifest dependencies are installed (triplet: ${VCPKG_ARCH})")
        # Hand vcpkg the held compilers (cmake/VcpkgHostCompiler.cmake), not
        # this process's CC/CXX: a cross-toolchain configure (Emscripten)
        # exports the cross compiler into the process env, and vcpkg's
        # HOST-triplet detect_compiler would inherit it and try to build
        # arm64-osx tool ports with emcc.
        ge_vcpkg_host_compiler_env(_ge_vcpkg_compiler_env)
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E env ${_ge_vcpkg_compiler_env}
                    ${VCPKG_EXECUTABLE_PATH} install --triplet ${VCPKG_ARCH}
                    --x-install-root=${_ge_vcpkg_install_root}
                    --clean-buildtrees-after-build --clean-packages-after-build
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            RESULT_VARIABLE MANIFEST_VERIFY_RESULT
            # Prevent CI/test harnesses from hanging indefinitely on a held vcpkg lock;
            # treat long waits as a hard error instead.
            TIMEOUT 900
        )
        if(NOT MANIFEST_VERIFY_RESULT EQUAL 0)
            message(FATAL_ERROR
                "vcpkg failed to verify/install manifest dependencies for triplet '${VCPKG_ARCH}' (exit ${MANIFEST_VERIFY_RESULT}).\n"
                "  See vcpkg's output above for the specific failure.\n"
                "\n"
                "  Common causes:\n"
                "    - Network failure while downloading sources.\n"
                "    - A specific port failed to build — check the per-port logs in\n"
                "        ${VCPKG_DIR}/buildtrees/<port-name>/install-*.log\n"
                "    - Held vcpkg lock from a concurrent process (timed out after 900s).\n"
                "  To skip this verify step entirely, configure with -DGE_SKIP_VCPKG_VERIFY=ON.")
        endif()
        message(STATUS "✓ vcpkg manifest dependencies verified for ${VCPKG_ARCH}")
    endif()


    # GTest discovery is gated on BUILD_TESTING everywhere. This root-scope
    # find used to run unconditionally and seeded GTest_FOUND for every
    # GTest_FOUND-gated test block, defining dozens of test targets in
    # BUILD_TESTING=OFF configures (the PR #421 review's "target rot").
    # Contract: GTest exists if and only if BUILD_TESTING is ON.
    if(BUILD_TESTING)
        find_package(GTest CONFIG QUIET)
        if(GTest_FOUND)
            message(STATUS "✓ Google Test found and ready")
        else()
            message(WARNING "Google Test not found - tests may not build correctly")
        endif()
    endif()

    # Try to find ConcurrentQueue
    find_package(concurrentqueue CONFIG QUIET)
    if(concurrentqueue_FOUND)
        message(STATUS "✓ concurrentqueue::concurrentqueue found and ready")
    else()
        message(WARNING "concurrentqueue (moodycamel) not found - high-performance queues may not be available")
    endif()

    # Try to find nlohmann_json and install if missing
    find_package(nlohmann_json CONFIG QUIET)
    if(nlohmann_json_FOUND)
        message(STATUS "✓ nlohmann_json found and ready")
    else()
        message(STATUS "nlohmann_json not found - installing via vcpkg...")
        execute_process(
            COMMAND ${VCPKG_EXECUTABLE_PATH} install nlohmann-json:${VCPKG_ARCH}
            WORKING_DIRECTORY ${VCPKG_DIR}
            RESULT_VARIABLE NLOHMANN_INSTALL_RESULT
            OUTPUT_VARIABLE NLOHMANN_INSTALL_OUTPUT
            ERROR_VARIABLE NLOHMANN_INSTALL_ERROR
        )
        if(NOT NLOHMANN_INSTALL_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to install nlohmann_json via vcpkg: ${NLOHMANN_INSTALL_ERROR}")
        endif()
        message(STATUS "✓ nlohmann_json installed via vcpkg")
    endif()

    # Try to find Google Benchmark and install if missing
    find_package(benchmark CONFIG QUIET)
    if(benchmark_FOUND OR TARGET benchmark::benchmark)
        message(STATUS "✓ Google Benchmark found and ready")
    else()
        message(STATUS "Google Benchmark not found - installing via vcpkg...")
        execute_process(
            COMMAND ${VCPKG_EXECUTABLE_PATH} install benchmark:${VCPKG_ARCH}
            WORKING_DIRECTORY ${VCPKG_DIR}
            RESULT_VARIABLE BENCHMARK_INSTALL_RESULT
            OUTPUT_VARIABLE BENCHMARK_INSTALL_OUTPUT
            ERROR_VARIABLE BENCHMARK_INSTALL_ERROR
        )
        if(NOT BENCHMARK_INSTALL_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to install Google Benchmark via vcpkg: ${BENCHMARK_INSTALL_ERROR}")
        endif()
        message(STATUS "✓ Google Benchmark installed via vcpkg")
    endif()
endfunction()

# Export functions for use in main CMakeLists.txt
# These will be called from the main CMakeLists.txt file
