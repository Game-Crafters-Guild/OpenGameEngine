cmake_minimum_required(VERSION 3.25)

# Refuse a clone path the dependency builds cannot take (a space, or under
# Program Files) before anything provisions or runs vcpkg. See cmake/ClonePathGuard.cmake.
include(cmake/ClonePathGuard.cmake)
ge_check_clone_path("${CMAKE_SOURCE_DIR}")

# Prevent in-source builds with helpful error message
get_filename_component(srcdir "${CMAKE_SOURCE_DIR}" REALPATH)
get_filename_component(bindir "${CMAKE_BINARY_DIR}" REALPATH)

if("${srcdir}" STREQUAL "${bindir}")
	    message(FATAL_ERROR
	        "\n"
	        "❌ In-source builds are not allowed!\n"
	        "\n"
	        "To build this project, run one of these commands instead:\n"
	        "\n"
	        "  On Windows (Visual Studio):\n"
	        "    cmake -B build -G \"Visual Studio 18 2026\" -A x64\n"
	        "\n"
	        "  On Linux (Ninja):\n"
	        "    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=DebugFast\n"
	        "\n"
	        "This will:\n"
	        "  • Create a 'build' directory for all build files\n"
	        "  • Keep your source directory clean\n"
	        "  • Generate project files in build/\n"
	        "\n"
	        "Then build with:\n"
	        "    cmake --build build --config DebugFast\n"
	        "\n"
	        "Or open the solution (Visual Studio generators):\n"
	        "    start build/GameEngine.slnx\n"
	        "\n"
	    )
endif()

# Disable vcpkg's applocal.ps1 post-build DLL staging. Two reasons:
#
# 1. We do our own runtime-DLL staging via `ge_stage_runtime_dependencies`
#    (cmake/OutputLayout.cmake) using CMake's TARGET_RUNTIME_DLLS. vcpkg's
#    applocal running on top is duplicate work.
#
# 2. vcpkg's cmake toolchain caches `Z_VCPKG_POWERSHELL_PATH` at first
#    configure. If neither `pwsh` nor `powershell` is on PATH at that
#    moment, vcpkg silently falls back to the literal string
#    `powershell.exe` (see `z_vcpkg_set_powershell_path` in
#    `dependencies/vcpkg/scripts/buildsystems/vcpkg.cmake`). Every later
#    build then invokes that bare string from MSBuild — which inherits a
#    minimal PATH and exits 9009 if `C:\Windows\System32` isn't on it.
#    Result: intermittent post-build failures on environments where only
#    PowerShell 7 is available via WindowsApps. Skipping applocal entirely
#    sidesteps the cached-path problem completely.
#
# Must be set BEFORE the vcpkg toolchain loads (i.e. before the project()
# call below resolves `CMAKE_TOOLCHAIN_FILE`).
set(VCPKG_APPLOCAL_DEPS OFF CACHE BOOL "vcpkg redundant — we stage runtime DLLs ourselves via ge_stage_runtime_dependencies" FORCE)

# Include automated dependency management
include(cmake/Dependencies.cmake)

# Refuse a vcpkg install root that belongs to another tree — a junction or
# symlink, or a plain path outside this build directory — before anything
# installs into it: the write rewrites the dependencies under every build
# directory that shares it. The root is resolved exactly as the vcpkg toolchain
# resolves it (dependencies/vcpkg/scripts/buildsystems/vcpkg.cmake): an
# explicit VCPKG_INSTALLED_DIR first, then the toolchain's own
# _VCPKG_INSTALLED_DIR, then this build tree's copy — a root the guard does not
# resolve is a root it does not check. See cmake/VcpkgInstallGuard.cmake.
include(cmake/VcpkgInstallGuard.cmake)
if(DEFINED VCPKG_INSTALLED_DIR AND NOT VCPKG_INSTALLED_DIR STREQUAL "")
    set(_ge_vcpkg_install_root "${VCPKG_INSTALLED_DIR}")
elseif(DEFINED _VCPKG_INSTALLED_DIR AND NOT _VCPKG_INSTALLED_DIR STREQUAL "")
    set(_ge_vcpkg_install_root "${_VCPKG_INSTALLED_DIR}")
else()
    set(_ge_vcpkg_install_root "${CMAKE_BINARY_DIR}/vcpkg_installed")
endif()
ge_check_vcpkg_install_root_not_shared("${_ge_vcpkg_install_root}" "${CMAKE_BINARY_DIR}")
# And refuse a root this build directory owns but another tree produced, which
# the check above cannot see. Here, ahead of every other report, so a build
# directory in that state never reads as consistent on its way to the refusal.
ge_check_vcpkg_install_root_written_here(
    "${_ge_vcpkg_install_root}" "${VCPKG_MANIFEST_DIR}" "${CMAKE_BINARY_DIR}")
unset(_ge_vcpkg_install_root)

# Report a vcpkg checkout that is not on the recorded commit, and point vcpkg's
# downloads at the per-user root every tree shares — both before anything runs
# vcpkg, including its bootstrap. The checkout and the tool versions it resolves
# are inputs to every port's ABI hash, so a tree that differs in either reads
# none of the machine's binary cache. See cmake/VcpkgPin.cmake.
ge_check_vcpkg_clone_pin("${VCPKG_DIR}")
ge_vcpkg_use_shared_downloads_root()

# Hold the compilers vcpkg builds with to the pair this build directory's first
# configure resolved, before anything runs vcpkg. A configure that xcodebuild
# re-runs has Xcode's toolchain first on PATH, and a compiler vcpkg resolved
# afresh there changes every package's ABI. See cmake/VcpkgHostCompiler.cmake.
ge_vcpkg_hold_host_compilers()

# Prime a cold vcpkg binary cache from the repo's `vcpkg-cache` release before
# anything can trigger a vcpkg install (the auto-provision below, or the vcpkg
# toolchain at project()). Runs on every configure entry point; a warm cache
# returns after a glob; every failure is non-fatal.
prime_vcpkg_binary_cache()

# Setup dependencies before project declaration so vcpkg toolchain is
# available for find_package calls. Run setup when the toolchain file is
# unset OR points at a path that does not exist yet — the latter happens on
# a fresh clone where a preset hard-codes
# dependencies/vcpkg/scripts/buildsystems/vcpkg.cmake but vcpkg has not been
# provisioned. Without the second clause the auto-clone never fires and
# project() aborts trying to load the missing toolchain file.
set(_GE_TOOLCHAIN_PATH "${CMAKE_TOOLCHAIN_FILE}")
if(_GE_TOOLCHAIN_PATH AND NOT IS_ABSOLUTE "${_GE_TOOLCHAIN_PATH}")
    set(_GE_TOOLCHAIN_PATH "${CMAKE_SOURCE_DIR}/${_GE_TOOLCHAIN_PATH}")
endif()
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE OR NOT EXISTS "${_GE_TOOLCHAIN_PATH}")
    if(DEFINED CMAKE_TOOLCHAIN_FILE AND NOT EXISTS "${_GE_TOOLCHAIN_PATH}")
        message(STATUS
            "Configured CMAKE_TOOLCHAIN_FILE='${CMAKE_TOOLCHAIN_FILE}' does not exist yet — "
            "bootstrapping vcpkg into ${CMAKE_SOURCE_DIR}/dependencies/vcpkg/ "
            "(first-run only; can take several minutes). "
            "To skip the auto-bootstrap, set VCPKG_ROOT to an existing vcpkg checkout "
            "and use a non-local preset (e.g. vs2026-x64-unity).")
    endif()
    setup_dependencies()
endif()
unset(_GE_TOOLCHAIN_PATH)

# Resolve the pinned tools before project() loads the vcpkg toolchain, because
# that toolchain runs its own manifest install with the versions it finds then.
# A tree that provisioned itself above has already run this; the call is a no-op
# the second time.
ge_seed_vcpkg_pinned_tools("${VCPKG_EXECUTABLE_PATH}")

# MSVC debug-info format must be decided before project(): CMP0141 is read at
# language initialization, and under OLD /Zi is baked into the default Debug
# flags where neither this variable nor any preset can reach it. With the
# policy NEW the format comes from CMAKE_MSVC_DEBUG_INFORMATION_FORMAT; the
# default below reproduces the classic formats (Debug/DebugFast/RelWithDebInfo
# compile with /Zi, Release with none) and stays overridable so a preset can
# select Embedded (/Z7) where compile cacheability matters — sccache cannot
# cache shared-PDB /Zi compiles. The linker emits a full PDB either way.
if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
    if(NOT DEFINED CMAKE_MSVC_DEBUG_INFORMATION_FORMAT)
        set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
            "$<$<CONFIG:Debug,DebugFast,RelWithDebInfo>:ProgramDatabase>")
    endif()
endif()

# Single-config generators (Ninja, Makefiles) build DebugFast unless CMAKE_BUILD_TYPE
# names another configuration. Cached before project(): MSVC's platform module
# otherwise caches Debug while the languages initialize.
get_property(_ge_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _ge_is_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE DebugFast CACHE STRING
        "Build configuration: DebugFast, Debug, RelWithDebInfo, Release or MinSizeRel" FORCE)
endif()

# The engine version comes from the root VERSION file (cmake/EngineVersion.cmake):
# GE_ENGINE_VERSION is the full string, prerelease suffix included, and
# GE_ENGINE_VERSION_NUMERIC its <year>.<month>.<patch> part, which project() and
# the macOS bundle keys take.
include(cmake/EngineVersion.cmake)
ge_read_engine_version("${CMAKE_CURRENT_SOURCE_DIR}/VERSION" GE_ENGINE_VERSION GE_ENGINE_VERSION_NUMERIC)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

project(GameEngine VERSION ${GE_ENGINE_VERSION_NUMERIC} LANGUAGES C CXX)
message(STATUS "Engine version: ${GE_ENGINE_VERSION}")

# The version as C++ constants, for the few sources that show or encode it. A
# generated header rather than a compile definition: a version bump recompiles
# only the files that include it, not every target that links Engine.
string(REPLACE "." ";" _ge_version_parts "${GE_ENGINE_VERSION_NUMERIC}")
list(GET _ge_version_parts 0 GE_ENGINE_VERSION_YEAR)
list(GET _ge_version_parts 1 GE_ENGINE_VERSION_MONTH)
list(GET _ge_version_parts 2 GE_ENGINE_VERSION_PATCH)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Engine/Include/Core/EngineVersion.h.in"
               "${CMAKE_BINARY_DIR}/generated/Core/EngineVersion.h" @ONLY)

# Our patched overlay ports (cmake/ports, and cmake/macos-ports on macOS, which
# Dependencies.cmake passes to vcpkg) must be configure dependencies, or
# patching one silently does nothing. vcpkg installs at configure time and
# registers only vcpkg.json and vcpkg-configuration.json as configure
# dependencies, so editing a portfile or a port patch changes nothing the build
# system tracks: `cmake --build` skips the reconfigure and keeps linking the
# previously installed library, with no warning at any layer. The glob is
# CONFIGURE_DEPENDS too, so that *adding* a port is caught and not only editing
# an existing one.
#
# Tracking is necessary, not sufficient: a reconfigure installs nothing in a
# tree with VCPKG_MANIFEST_INSTALL=OFF (e.g. redirected at a shared
# vcpkg_installed). ge_check_overlay_port_freshness() — called after
# verify_dependencies(), once every configure-time installer has run — is what
# hard-fails a configure whose install root predates a port bump.
set(GE_OVERLAY_PORT_GLOBS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ports/*")
if(GE_VCPKG_MACOS_OVERLAY_PORTS)
    list(APPEND GE_OVERLAY_PORT_GLOBS "${GE_VCPKG_MACOS_OVERLAY_PORTS}/*")
endif()
file(GLOB_RECURSE GE_OVERLAY_PORT_FILES CONFIGURE_DEPENDS ${GE_OVERLAY_PORT_GLOBS})
if(GE_OVERLAY_PORT_FILES)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${GE_OVERLAY_PORT_FILES})
endif()

# Xcode: generate scheme files as part of project generation.
# Otherwise, Xcode will prompt to "create schemes" repeatedly because the generated
# .xcodeproj is re-generated by ZERO_CHECK / reconfigure.
if(CMAKE_GENERATOR STREQUAL "Xcode")
  set(CMAKE_XCODE_GENERATE_SCHEME ON CACHE BOOL "Generate Xcode schemes for CMake targets" FORCE)
endif()

# Shared CMake helpers
include(cmake/OutputLayout.cmake)
include(cmake/StageManagedAssemblies.cmake)
include(cmake/McpServer.cmake)
ge_add_mcp_server_target()

# Enable position-independent code on non-Windows so that static libraries
# (Engine, ECS, AssetRuntime, etc.) can be safely linked into shared objects
# such as GameEngine.Native.so on Linux.
if(NOT WIN32)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# POSIX runtime-loader path defaults so that consumer executables (Editor,
# Player, every test exe) find Engine.dylib / libEngine.so when it sits in
# the same directory as the exe. Without this, after the Phase 1 SHARED
# flip, dlopen would fail at startup on macOS/Linux.
# Windows has no equivalent — DLLs in the same directory as the exe are
# found via the standard DLL search order.
if(APPLE)
    set(CMAKE_BUILD_RPATH "@executable_path;@loader_path")
    set(CMAKE_INSTALL_RPATH "@executable_path;@loader_path")
    set(CMAKE_MACOSX_RPATH ON)
elseif(UNIX)
    set(CMAKE_BUILD_RPATH "$ORIGIN")
    set(CMAKE_INSTALL_RPATH "$ORIGIN")
endif()

if(UNIX AND NOT APPLE)
    include(CheckLinkerFlag)
    check_linker_flag(CXX "-fuse-ld=mold" LINKER_SUPPORTS_MOLD)
    if(LINKER_SUPPORTS_MOLD)
        message(STATUS "Using mold linker")
        add_link_options(-fuse-ld=mold)
    endif()
endif()

# Global feature toggles
# Enforce Vulkan clip space depth range [0,1] across all targets using GLM
add_compile_definitions(GLM_FORCE_DEPTH_ZERO_TO_ONE)

# Windows header hygiene to avoid winsock.h vs winsock2.h conflicts and min/max macro pollution
if(WIN32)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# Ensure _DEBUG is defined for Debug configuration on non-MSVC compilers
# (MSVC defines _DEBUG automatically for Debug builds)
if(NOT MSVC)
    add_compile_definitions($<$<CONFIG:Debug>:_DEBUG>)
endif()

# GE_DEV_DIAG: opt-in developer diagnostics (extra logging, phase timings).
# Active in Debug + DebugFast + RelWithDebInfo; compiled out of Release / MinSizeRel.
add_compile_definitions($<$<OR:$<CONFIG:Debug>,$<CONFIG:DebugFast>,$<CONFIG:RelWithDebInfo>>:GE_DEV_DIAG=1>)

# GE_DEBUG_INSTRUMENTATION: 1 where the engine carries its debug-only tripwires
# and validators, 0 where it does not. Public engine headers declare DATA MEMBERS
# under it, so it is part of the engine's binary interface: every translation unit
# that includes an engine header must use the value the Engine library was built
# with, or one class has two layouts and the two sides read each other's members
# at the wrong offsets.
#
# That is why the switch belongs to the engine and not to NDEBUG. A consumer's
# build system derives NDEBUG from the CONSUMER's configuration: a packaged game
# compiles Release (NDEBUG) while the Engine.dll it links is often DebugFast (no
# NDEBUG). This value travels with the engine instead — through the SDK's generated
# find_package config and the NativeScripting SDK manifest, both in
# cmake/GameEngineSDK.cmake — and EngineCore::Initialize refuses a host that
# disagrees with the engine it loaded.
#
# The set of configurations that carry the instrumentation is spelled here and
# nowhere else: the NativeScripting SDK manifests reuse
# GE_DEBUG_INSTRUMENTATION_VALUE instead of restating it.
set(GE_DEBUG_INSTRUMENTATION_VALUE "$<IF:$<OR:$<CONFIG:Debug>,$<CONFIG:DebugFast>>,1,0>")
add_compile_definitions(GE_DEBUG_INSTRUMENTATION=${GE_DEBUG_INSTRUMENTATION_VALUE})

# UISystem removed during redesign; keep permanently disabled

# WebAssembly target: one static binary (no Engine DLL, no dynamic native
# modules), wasm platform paths. Auto-detected from the Emscripten toolchain;
# the explicit option exists so a desktop configure can dry-run the static
# link graph.
option(GE_WASM "Target WebAssembly (single static binary linking)" OFF)
if(EMSCRIPTEN)
    set(GE_WASM ON CACHE BOOL "Target WebAssembly (single static binary linking)" FORCE)

    # Threading ABI is a link-graph-wide decision on wasm: every object in a
    # shared-memory binary needs -pthread (atomics + bulk-memory), and the
    # vcpkg triplet must match (wasm32-emscripten vs wasm32-emscripten-nothreads).
    # The single-threaded config is the no-COOP/COEP fallback: JobSystem runs
    # inline with 0 workers.
    option(GE_WASM_SINGLE_THREAD "Single-threaded wasm fallback ABI (no -pthread, inline JobSystem)" OFF)
    if(GE_WASM_SINGLE_THREAD)
        add_compile_definitions(GE_WASM_SINGLE_THREAD=1)
    else()
        add_compile_options(-pthread)
        # Pool is pre-spawned so worker startup never blocks the main thread.
        add_link_options(-pthread "SHELL:-s PTHREAD_POOL_SIZE=8")
    endif()
    # Per-executable link budget (memory model). ASYNCIFY is intentionally not
    # global: only targets that pump GPU/browser waits need it, and it taxes
    # every function it instruments.
    # The C++ shadow stack is a fixed linear-memory region, and running off the
    # bottom of it writes over whatever is below rather than faulting. Exposed
    # as a cache variable because it is a measurable quantity under
    # investigation, not a constant: GE_WASM_ASSERTIONS turns on the check that
    # reports an overflow instead of letting it corrupt.
    set(GE_WASM_STACK_SIZE "1048576" CACHE STRING "C++ shadow stack size for wasm executables, in bytes")
    add_link_options(
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s MAXIMUM_MEMORY=4GB"
        "SHELL:-s STACK_SIZE=${GE_WASM_STACK_SIZE}"
        # emscripten_fetch's JS library, behind Platform's HttpClient. Root
        # scope because HttpClient lives in the Platform archive inside Engine,
        # so any executable can pull its object in; without -sFETCH such a link
        # fails on the fetch symbols.
        "SHELL:-s FETCH"
    )

    # Emscripten's runtime checks. A link-time setting, not a compile-time one:
    # it selects the assertions-enabled variants of the system libraries
    # (WasmFS, pthread, libc) and the JS-side argument and lifecycle checks, so
    # turning it on relinks rather than rebuilds. Off by default because it
    # costs size and speed in the shipped artifact; on, it is what turns a wasm
    # trap somewhere downstream into an abort at the misuse.
    option(GE_WASM_ASSERTIONS "Link wasm executables with emscripten's runtime assertions" OFF)
    if(GE_WASM_ASSERTIONS)
        add_link_options("SHELL:-s ASSERTIONS=2")
    endif()

    # Keep the wasm name section, so a browser stack trace names functions
    # instead of numbering them. Separate from the assertions knob on purpose:
    # this changes no code, only what the binary carries, so a trace can be read
    # without also changing what the build does. Function *indices* are not
    # comparable between two binaries — read names, never indices, and only from
    # the binary that produced the trace.
    option(GE_WASM_SYMBOL_NAMES "Emit the wasm name section (readable stack traces)" OFF)
    if(GE_WASM_SYMBOL_NAMES)
        add_link_options(--profiling-funcs)
    endif()

    # STACK_SIZE above buys linear memory for the C++ shadow stack. It does not
    # buy call-stack depth: a wasm frame's locals live in the engine's own
    # native stack, which the browser fixes at roughly 1 MB and no page can
    # raise. At -O0 clang emits one wasm local per SSA temp and the baseline
    # compiler gives each a native slot, so a Yoga layout frame costs tens of
    # KB against that 1 MB. Measured on the Editor: -O0 exhausts the stack
    # after 16 layout levels and throws RangeError on the first frame, before
    # anything is drawn; -O2 runs the same tree past 300 frames. The fat frames
    # are Yoga's and the engine's, not the app target's, so a per-target
    # optimisation flag cannot fix it -- the floor would have to cover the
    # vcpkg triplet too. A UI-bearing wasm target is therefore a non-Debug
    # target; Apps/Editor/CMakeLists.txt refuses to build WebEditor in Debug
    # rather than let it fail in the browser.

    # KTX's imported target ships the link settings its own wasm demos want,
    # and usage requirements carry them into every executable that links the
    # engine — including "-s STACK_SIZE=96kb", which lands after the budget
    # above and wins, because emcc takes the last -s. The main-thread stack is
    # an application decision, and 96 KB is far below what a UI layout pass
    # recurses through. Resolve the package once here, at a scope every
    # subdirectory inherits (Engine and UI each call find_package(Ktx), and a
    # per-directory imported target would keep its own copy of the flag), and
    # drop that one entry — the port's other settings stay as they are.
    # Link contract for a target that mounts OPFS (Platform::Web::
    # MountPersistentStorage). Two parts that only work together, kept in one
    # variable so a target cannot pick up one of them:
    #
    #  WASMFS   - carries the OPFS backend at all. The legacy JS filesystem has
    #             no OPFS; without this the link fails on the backend symbol.
    #
    # ASYNCIFY is deliberately NOT here. Backend creation spawns its worker from
    # a pthread, and the proxied file operations block on a worker that runs in
    # parallel, so nothing on this path needs the main thread to unwind:
    # WebStorageBrowserSmoke passes its full round-trip with ASYNCIFY unlinked.
    # A target that needs it needs it for its own reason and asks for it there.
    #
    #  DEFAULT_LIBRARY_FUNCS_TO_INCLUDE
    #           - JS library symbols this build uses but nothing declares a
    #             dependency on, so the linker would drop them:
    #             * $stringToUTF8OnStack works around an emscripten 6.0.5 bug --
    #               libwasmfs_opfs.js calls it when listing a directory but omits
    #               it from _wasmfs_opfs_get_entries__deps. The listing then
    #               throws ReferenceError into a bare `catch` that reports EIO,
    #               and every readdir/opendir/directory_iterator on OPFS fails as
    #               "I/O error" while open/read/write/stat all work. Upstream
    #               tests miss it because a larger link pulls the symbol in
    #               anyway. Force-including a symbol that is already present is a
    #               no-op, so this stays correct once emscripten fixes the deps.
    #             * $stringToNewUTF8 is used by EM_ASM blocks that return a JS
    #               string to C (the host reading its URL). EM_ASM bodies are
    #               opaque to the dependency scanner, so a use inside one never
    #               keeps a symbol alive.
    #             Keep this a single flag: emcc takes the last -s for a given
    #             setting, so a second DEFAULT_LIBRARY_FUNCS_TO_INCLUDE anywhere
    #             on a link line silently discards this one.
    set(GE_WASM_OPFS_LINK_OPTIONS
        "SHELL:-s WASMFS"
        [[SHELL:-s DEFAULT_LIBRARY_FUNCS_TO_INCLUDE=$stringToUTF8OnStack,$stringToNewUTF8]])

    # Allocator selection, for hunting heap corruption. Empty means the setting
    # is left wherever the link line puts it. "emmalloc-memvalidate" is the same
    # allocator the build already uses, with its own structural checks compiled
    # in, so a corrupted free list aborts at the operation that first sees the
    # damage rather than trapping in whichever allocation happens next. Do not
    # reach for a *different* allocator to make a symptom move: that changes the
    # heap layout, so it relocates the trap without saying anything about who
    # wrote out of bounds.
    set(GE_WASM_MALLOC "" CACHE STRING
        "-s MALLOC for wasm executables (empty = leave as linked; e.g. emmalloc-memvalidate)")
    if(GE_WASM_MALLOC)
        add_link_options("SHELL:-s MALLOC=${GE_WASM_MALLOC}")
    endif()

    find_package(Ktx CONFIG QUIET)
    if(TARGET KTX::ktx)
        get_target_property(_ge_ktx_link_options KTX::ktx INTERFACE_LINK_OPTIONS)
        if(_ge_ktx_link_options)
            list(FILTER _ge_ktx_link_options EXCLUDE REGEX "STACK_SIZE")
            # The port also carries -s MALLOC. It lands after everything set
            # here and emcc takes the last -s, so the knob above is inert while
            # it is present. Dropped only when the knob asks for a specific
            # allocator; with the knob empty the port's choice stands.
            if(GE_WASM_MALLOC)
                list(FILTER _ge_ktx_link_options EXCLUDE REGEX "MALLOC")
            endif()
            set_target_properties(KTX::ktx PROPERTIES
                INTERFACE_LINK_OPTIONS "${_ge_ktx_link_options}")
        endif()
    endif()
endif()

# Toggle scripting/native hosting related builds (default ON for normal developer builds)
option(ENABLE_SCRIPTING "Build scripting/native shim and managed integration targets" ON)
if(EMSCRIPTEN)
    # C# on wasm is Phase 5 of the web platform plan (browser-wasm Mono);
    # CoreCLR/nethost hosting does not exist on this target. The web Player
    # builds the engine with the managed integration compiled out.
    set(ENABLE_SCRIPTING OFF CACHE BOOL "" FORCE)
endif()
# Use mimalloc for the host process (Editor + native modules). Prefer mimalloc's
# official override library when available; otherwise fall back to local overrides.
option(ENABLE_MIMALLOC "Enable mimalloc allocator for the Editor/host process" OFF)
# Optional components (OFF by default for faster builds of non-scripting extras)
option(BUILD_EXAMPLES "Build example executables" OFF)
option(BUILD_TOOLS "Build developer tools/CLIs" OFF)
option(BUILD_BENCHMARKS "Build benchmark executables" OFF)

# Platform services backend: exactly one provider module is linked per build.
# Game-facing APIs (PlatformServices facade, Platform.ABI) stay identical
# across backends; only the provider behind the GE_PlatformProviderV1 table
# changes. Future values: Steam, Xbox, PlayStation, ...
set(GE_PLATFORM_BACKEND "Local" CACHE STRING "Platform services provider linked into this build")
set(_ge_known_platform_backends Local)
set_property(CACHE GE_PLATFORM_BACKEND PROPERTY STRINGS ${_ge_known_platform_backends})
if(NOT GE_PLATFORM_BACKEND IN_LIST _ge_known_platform_backends)
    message(FATAL_ERROR "Unknown GE_PLATFORM_BACKEND '${GE_PLATFORM_BACKEND}'. Known backends: ${_ge_known_platform_backends}")
endif()


# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Multi-config generators have no CMAKE_BUILD_TYPE (single-config ones get DebugFast
# above project()). Configure-time checks that read it (Logger source location, the
# job tests' CRT leak checks) see Debug in every configuration of a multi-config tree.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# DebugFast: daily-driver dev config. Same source-level debugging as Debug (/Od + PDB symbols)
# but skips the MSVC debug-CRT / iterator-debug / /RTC1 overhead and defaults Vulkan validation
# layers OFF. Vcpkg binds $<CONFIG:Debug> to debug libs and any other config to release libs,
# so DebugFast automatically links the release CRT variants — ABI-consistent with /MD.
# Debug stays unchanged so AI agents / tooling that rely on its safety nets are unaffected.
# DebugFast is the first configuration, so the IDE and Ninja Multi-Config default to it;
# a tree configured before it moved first is reordered on its next configure.
if(CMAKE_CONFIGURATION_TYPES)
    list(REMOVE_ITEM CMAKE_CONFIGURATION_TYPES DebugFast)
    list(PREPEND CMAKE_CONFIGURATION_TYPES DebugFast)
    set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}"
        CACHE STRING "Configurations" FORCE)
endif()
# Seed DebugFast flags from Debug, then strip /RTC1 (runtime checks are the big single
# overhead that /W4 + ASAN already cover). /MDd → /MD is handled by CMAKE_MSVC_RUNTIME_LIBRARY.
set(CMAKE_C_FLAGS_DEBUGFAST                "${CMAKE_C_FLAGS_DEBUG}"                CACHE STRING "")
set(CMAKE_CXX_FLAGS_DEBUGFAST              "${CMAKE_CXX_FLAGS_DEBUG}"              CACHE STRING "")
set(CMAKE_EXE_LINKER_FLAGS_DEBUGFAST       "${CMAKE_EXE_LINKER_FLAGS_DEBUG}"       CACHE STRING "")
set(CMAKE_SHARED_LINKER_FLAGS_DEBUGFAST    "${CMAKE_SHARED_LINKER_FLAGS_DEBUG}"    CACHE STRING "")
set(CMAKE_STATIC_LINKER_FLAGS_DEBUGFAST    "${CMAKE_STATIC_LINKER_FLAGS_DEBUG}"    CACHE STRING "")
set(CMAKE_MODULE_LINKER_FLAGS_DEBUGFAST    "${CMAKE_MODULE_LINKER_FLAGS_DEBUG}"    CACHE STRING "")

# Route IMPORTED targets (vcpkg deps) in DebugFast to their Release variants.
# Without this, CMake's fallback for unknown configs can pick the first entry of
# IMPORTED_CONFIGURATIONS (often DEBUG first via vcpkg) — causing /MDd .lib to be
# linked into a /MD engine, which fails the linker's ABI guards.
set(CMAKE_MAP_IMPORTED_CONFIG_DEBUGFAST "Release;RelWithDebInfo;MinSizeRel;" CACHE STRING "DebugFast falls back to Release-variant IMPORTED libs" FORCE)

if(MSVC)
    # Strip /RTC1 from DebugFast only. Debug keeps it.
    foreach(flag_var CMAKE_C_FLAGS_DEBUGFAST CMAKE_CXX_FLAGS_DEBUGFAST)
        string(REPLACE "/RTC1" "" ${flag_var} "${${flag_var}}")
    endforeach()

    # CRT: /MDd only for Debug; /MD for everything else including DebugFast. This is also
    # CMake's default when CMP0091 is NEW (cmake_minimum_required >= 3.15), but setting it
    # explicitly makes the intent visible and survives triplet/preset overrides.
    if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif()

    # Treat third-party headers as "external" and silence their warnings.
    # This keeps our build output focused on actionable warnings in our code.
    #
    # - /external:anglebrackets: treat #include <...> as external headers
    # - /external:W0: warning level 0 for external headers
    #
    # Note: /experimental:external is ignored on newer toolsets, required on older ones.
    # /analyze:external- must NOT go on this unconditional line: any /analyze:* option
    # on a raw cl command line (Ninja generators) implicitly enables code analysis,
    # which would drag full /analyze into DebugFast/Release and trip their /WX.
    # It lives with the per-config /analyze flags below. (MSBuild routes the flag
    # through a vcxproj property, so the VS generator masks the hazard.)
    add_compile_options(/experimental:external /external:anglebrackets /external:W0)
endif()

# GE_DEBUGFAST: compile-time marker for source code to detect the fast dev config
# (used to flip defaults for Vulkan validation, log verbosity, etc.).
add_compile_definitions($<$<CONFIG:DebugFast>:GE_DEBUGFAST=1>)

# Debug/DebugFast link with CMake's default /DEBUG (= /DEBUG:FULL + /INCREMENTAL).
# /DEBUG:FASTLINK is gone: the VS2026 linker dropped it (LNK4315) and already
# linked FULL regardless, so forcing it only produced warning spam.

# Testing toggle
option(BUILD_TESTING "Enable building tests" OFF)
include(CTest)
if(BUILD_TESTING)
  enable_testing()
endif()

# The allocation counter's hook: every test executable, and the engine's images under
# GE_DEBUG_INSTRUMENTATION. Also refuses ENABLE_MIMALLOC where the hook is compiled.
include(cmake/AllocationHook.cmake)

# Verify dependencies are working
verify_dependencies()

# Overlay-port freshness guard. Must run after verify_dependencies(): both the
# vcpkg toolchain (inside project(), when VCPKG_MANIFEST_INSTALL=ON) and
# verify_dependencies() install at configure time, and the guard compares
# against the post-install state — any earlier and it would fail a good fresh
# configure. See cmake/OverlayPortGuard.cmake and cmake/ports/README.md.
include(cmake/OverlayPortGuard.cmake)
ge_check_overlay_port_freshness()

# Record the installed package set for this build directory, and refuse to
# continue once a package it already compiled against is installed at a
# different version or ABI. Shares the ordering constraint above: the
# comparison is against the post-install state.
ge_check_vcpkg_install_set_unchanged(
    "${VCPKG_INSTALLED_DIR}" "${VCPKG_TARGET_TRIPLET}" "${CMAKE_BINARY_DIR}")

# Output directories - all build artifacts go to build directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Per-configuration output directories
foreach(CONFIG ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${CONFIG} CONFIG_UPPER)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR}/bin/${CONFIG})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR}/lib/${CONFIG})
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR}/lib/${CONFIG})
endforeach()

# pkg-config is a property of the build HOST, not the target: WIN32 describes
# what is being built for, so a Windows host cross-compiling to Emscripten took
# this branch and hard-failed on a tool Windows does not ship. Nothing needs it
# to be REQUIRED either — the one consumer, Engine/CMakeLists.txt's ThorVG
# lookup, runs its own find_package(PkgConfig QUIET) and falls back to
# find_path/find_library where pkg-config is absent.
if(NOT CMAKE_HOST_WIN32)
    find_package(PkgConfig REQUIRED)
endif()

# Find .NET hosting libraries (nethost) with architecture detection
include(cmake/DotnetHostVersions.cmake)
if(WIN32)
    # Detect target architecture — prefer the generator platform (VS generators)
    # over CMAKE_SYSTEM_PROCESSOR which reports the host, not the target.
    if(CMAKE_GENERATOR_PLATFORM MATCHES "ARM64|arm64")
        set(DOTNET_ARCH "arm64")
        message(STATUS "Detected ARM64 architecture for .NET hosting (Windows)")
    elseif(CMAKE_GENERATOR_PLATFORM MATCHES "x64|X64")
        set(DOTNET_ARCH "x64")
        message(STATUS "Detected x64 architecture for .NET hosting (Windows)")
    elseif(CMAKE_GENERATOR_PLATFORM MATCHES "Win32|x86")
        set(DOTNET_ARCH "x86")
        message(STATUS "Detected x86 architecture for .NET hosting (Windows)")
    elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
            set(DOTNET_ARCH "arm64")
            message(STATUS "Detected ARM64 architecture for .NET hosting (Windows)")
        else()
            set(DOTNET_ARCH "x64")
            message(STATUS "Detected x64 architecture for .NET hosting (Windows)")
        endif()
    else()
        set(DOTNET_ARCH "x86")
        message(STATUS "Detected x86 architecture for .NET hosting (Windows)")
    endif()

    # Try to find .NET installation with architecture detection
    # Try current architecture first, then fallback to x64
    set(DOTNET_ARCHITECTURES ${DOTNET_ARCH})
    if(NOT DOTNET_ARCH STREQUAL "x64")
        list(APPEND DOTNET_ARCHITECTURES "x64")
    endif()

    foreach(ARCH ${DOTNET_ARCHITECTURES})
        message(STATUS "Searching for .NET hosting libraries for ${ARCH} architecture (Windows)...")

        # Get all available versions for this architecture
        file(GLOB DOTNET_HOST_VERSIONS "C:/Program Files/dotnet/packs/Microsoft.NETCore.App.Host.win-${ARCH}/*")

        if(DOTNET_HOST_VERSIONS)
            # Try the newest version first
            ge_dotnet_versions_newest_first(DOTNET_HOST_VERSIONS)

            foreach(VERSION_PATH ${DOTNET_HOST_VERSIONS})
                set(DOTNET_NATIVE_PATH "${VERSION_PATH}/runtimes/win-${ARCH}/native")
                message(STATUS "Checking: ${DOTNET_NATIVE_PATH}")

                if(EXISTS "${DOTNET_NATIVE_PATH}/nethost.lib")
                    set(NETHOST_LIBRARY "${DOTNET_NATIVE_PATH}/nethost.lib")
                    set(NETHOST_DLL "${DOTNET_NATIVE_PATH}/nethost.dll")
                    set(NETHOST_INCLUDE_DIR "${DOTNET_NATIVE_PATH}")
                    set(DETECTED_DOTNET_ARCH "${ARCH}")
                    message(STATUS "Found .NET hosting library for ${ARCH}: ${NETHOST_LIBRARY}")
                    break()
                endif()
            endforeach()

            if(NETHOST_LIBRARY)
                break()
            endif()
        else()
            message(STATUS "No .NET host packages found for ${ARCH} architecture (Windows)")
        endif()
    endforeach()

	if(NOT NETHOST_LIBRARY)
	    message(WARNING ".NET hosting library (nethost) not found for any architecture on Windows (tried: ${DOTNET_ARCHITECTURES})")
	    message(WARNING "CoreCLR integration will be limited on Windows.")
	    message(STATUS "Available .NET host packages:")
	    file(GLOB ALL_DOTNET_HOSTS "C:/Program Files/dotnet/packs/Microsoft.NETCore.App.Host.*")
	    foreach(HOST_PACK ${ALL_DOTNET_HOSTS})
	        message(STATUS "  ${HOST_PACK}")
	    endforeach()
	else()
	    message(STATUS "Using .NET hosting libraries for ${DETECTED_DOTNET_ARCH} architecture (Windows)")
	    # Store the detected architecture for use in other CMake files
	    set(DOTNET_TARGET_ARCH "${DETECTED_DOTNET_ARCH}" CACHE STRING "Detected .NET target architecture")
	endif()

elseif(APPLE)
	# macOS: locate libnethost.dylib from the installed .NET SDK/runtime
	# Prefer DOTNET_ROOT when set, otherwise fall back to the standard install root
	set(DOTNET_ROOT_HINT "$ENV{DOTNET_ROOT}")
	if(DOTNET_ROOT_HINT STREQUAL "")
	    set(DOTNET_ROOT_HINT "/usr/local/share/dotnet")
	endif()

    # Derive RID based on architecture (osx-x64 / osx-arm64)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        # CMAKE_SYSTEM_PROCESSOR is typically "arm64" on Apple Silicon; handle
        # that as well as "ARM64"/"aarch64" to avoid mis-detecting the RID.
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
            set(DOTNET_OSX_RID "osx-arm64")
        else()
            set(DOTNET_OSX_RID "osx-x64")
        endif()
    else()
        # 32-bit macOS is effectively unsupported; fall back to x86 RID for completeness
        set(DOTNET_OSX_RID "osx-x86")
    endif()

	set(DOTNET_HOST_PACK_ROOT "${DOTNET_ROOT_HINT}/packs/Microsoft.NETCore.App.Host.${DOTNET_OSX_RID}")
	file(GLOB DOTNET_HOST_VERSIONS "${DOTNET_HOST_PACK_ROOT}/*")

	if(DOTNET_HOST_VERSIONS)
	    # Try the newest version first
	    ge_dotnet_versions_newest_first(DOTNET_HOST_VERSIONS)

	    foreach(VERSION_PATH ${DOTNET_HOST_VERSIONS})
	        set(DOTNET_NATIVE_PATH "${VERSION_PATH}/runtimes/${DOTNET_OSX_RID}/native")
	        message(STATUS "Checking: ${DOTNET_NATIVE_PATH}")

	        if(EXISTS "${DOTNET_NATIVE_PATH}/libnethost.dylib")
	            set(NETHOST_LIBRARY "${DOTNET_NATIVE_PATH}/libnethost.dylib")
	            set(NETHOST_INCLUDE_DIR "${DOTNET_NATIVE_PATH}")
	            set(DETECTED_DOTNET_ARCH "${DOTNET_OSX_RID}")
	            message(STATUS "Found .NET hosting library for ${DOTNET_OSX_RID}: ${NETHOST_LIBRARY}")
	            break()
	        endif()
	    endforeach()
	endif()

	if(NOT NETHOST_LIBRARY)
	    message(WARNING "libnethost.dylib not found under ${DOTNET_ROOT_HINT}; CoreCLR hosting will be disabled on macOS.")
	    message(STATUS "Searched under: ${DOTNET_HOST_PACK_ROOT}")
	else()
	    message(STATUS "Using .NET hosting library on macOS: ${NETHOST_LIBRARY}")
	endif()

elseif(UNIX AND NOT APPLE)
    # Linux: locate libnethost.so from the installed .NET SDK/runtime
    # Prefer DOTNET_ROOT when set, otherwise fall back to the standard install root
    set(DOTNET_ROOT_HINT "$ENV{DOTNET_ROOT}")
    if(DOTNET_ROOT_HINT STREQUAL "")
        set(DOTNET_ROOT_HINT "/usr/share/dotnet")
    endif()

    # Derive RID based on architecture (x64 / arm64)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        # On many Linux distros CMAKE_SYSTEM_PROCESSOR is "aarch64" or
        # "arm64" for ARM64; handle both as well as uppercase ARM64.
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
            set(DOTNET_LINUX_RID "linux-arm64")
        else()
            set(DOTNET_LINUX_RID "linux-x64")
        endif()
    else()
        set(DOTNET_LINUX_RID "linux-x86")
    endif()

    set(DOTNET_HOST_PACK_ROOT "${DOTNET_ROOT_HINT}/packs/Microsoft.NETCore.App.Host.${DOTNET_LINUX_RID}")
    file(GLOB DOTNET_HOST_VERSIONS "${DOTNET_HOST_PACK_ROOT}/*")

    if(DOTNET_HOST_VERSIONS)
        ge_dotnet_versions_newest_first(DOTNET_HOST_VERSIONS)

        foreach(VERSION_PATH ${DOTNET_HOST_VERSIONS})
            set(DOTNET_NATIVE_PATH "${VERSION_PATH}/runtimes/${DOTNET_LINUX_RID}/native")
            message(STATUS "Checking: ${DOTNET_NATIVE_PATH}")

            if(EXISTS "${DOTNET_NATIVE_PATH}/libnethost.so")
                set(NETHOST_LIBRARY "${DOTNET_NATIVE_PATH}/libnethost.so")
                set(NETHOST_INCLUDE_DIR "${DOTNET_NATIVE_PATH}")
                set(DETECTED_DOTNET_ARCH "${DOTNET_LINUX_RID}")
                message(STATUS "Found .NET hosting library for ${DOTNET_LINUX_RID}: ${NETHOST_LIBRARY}")
                break()
            endif()
        endforeach()
    endif()

    if(NOT NETHOST_LIBRARY)
        message(WARNING "libnethost.so not found under ${DOTNET_ROOT_HINT}; CoreCLR hosting will be disabled on Linux.")
        message(STATUS "Searched under: ${DOTNET_HOST_PACK_ROOT}")
    else()
        message(STATUS "Using .NET hosting library on Linux: ${NETHOST_LIBRARY}")
    endif()
endif()

# If the .NET hosting library (nethost) was found, make its availability macro and
# headers visible to all targets. This ensures that CoreCLRHost and any types that
# embed it (e.g. ScriptManager) have a consistent layout across all translation
# units, avoiding ODR/ABI mismatches that can manifest as heap buffer overflows
# when constructing objects with std::mutex members.
if(NETHOST_LIBRARY)
    add_compile_definitions(NETHOST_AVAILABLE)
    include_directories(${NETHOST_INCLUDE_DIR})
endif()

# Helper: copy .NET hosting runtime DLLs (nethost.dll and hostfxr.dll) next to a target after build
function(ge_add_dotnet_hosting_runtime target_name)
  if(WIN32 AND DEFINED NETHOST_DLL AND EXISTS "${NETHOST_DLL}")
    # Always copy nethost.dll from the detected SDK pack path
    add_custom_command(TARGET ${target_name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${NETHOST_DLL}"
        "$<TARGET_FILE_DIR:${target_name}>/nethost.dll"
      COMMENT "Copying nethost.dll for .NET hosting (${DETECTED_DOTNET_ARCH} architecture)")

    # Best-effort: also stage hostfxr.dll from the latest installed hostfxr
    file(GLOB HOSTFXR_VERSIONS "C:/Program Files/dotnet/host/fxr/*")
    if(HOSTFXR_VERSIONS)
      ge_dotnet_versions_newest_first(HOSTFXR_VERSIONS)
      list(GET HOSTFXR_VERSIONS 0 LATEST_HOSTFXR_VERSION)
      set(HOSTFXR_DLL "${LATEST_HOSTFXR_VERSION}/hostfxr.dll")
      if(EXISTS "${HOSTFXR_DLL}")
        add_custom_command(TARGET ${target_name} POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${HOSTFXR_DLL}"
            "$<TARGET_FILE_DIR:${target_name}>/hostfxr.dll"
          COMMENT "Copying hostfxr.dll (latest) for .NET hosting")
      endif()
    endif()
  endif()
endfunction()

#
# Helper: copy MSVC AddressSanitizer runtime DLLs next to an executable target.
#
# Why: When building with MSVC /fsanitize=address, the produced executables depend on
# `clang_rt.asan_dynamic-x86_64.dll` at runtime. Visual Studio sets PATH when launching
# from the IDE, but CTest / command-line runs may fail (or hang behind a modal loader
# error) if the DLL isn't staged next to the executable.
#
set(GE_MSVC_ASAN_RUNTIME_DLLS "" CACHE STRING "MSVC ASan runtime DLLs to stage next to executables when ENABLE_ASAN=ON")

if(WIN32 AND MSVC AND ENABLE_ASAN)
  # For MSVC, the ASan runtime DLL typically lives next to cl.exe in the selected toolset bin dir.
  get_filename_component(_ge_msvc_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)

  set(_ge_asan_candidates
      "${_ge_msvc_bin_dir}/clang_rt.asan_dynamic-x86_64.dll"
      "${_ge_msvc_bin_dir}/clang_rt.asan_dbg_dynamic-x86_64.dll"
  )

  set(_ge_asan_found "")
  foreach(_ge_asan_dll IN LISTS _ge_asan_candidates)
    if(EXISTS "${_ge_asan_dll}")
      list(APPEND _ge_asan_found "${_ge_asan_dll}")
    endif()
  endforeach()

  if(_ge_asan_found)
    set(GE_MSVC_ASAN_RUNTIME_DLLS "${_ge_asan_found}" CACHE STRING "MSVC ASan runtime DLLs to stage next to executables when ENABLE_ASAN=ON" FORCE)
  else()
    message(WARNING
      "ENABLE_ASAN=ON but could not locate MSVC ASan runtime DLLs next to compiler: ${_ge_msvc_bin_dir}. "
      "Running ASan-built executables from CTest/CLI may fail unless the toolset directory is on PATH "
      "or the runtime DLLs are copied next to the executable.")
  endif()
endif()

function(ge_add_asan_runtime target_name)
  if(NOT (WIN32 AND MSVC AND ENABLE_ASAN))
    return()
  endif()
  if(NOT GE_MSVC_ASAN_RUNTIME_DLLS)
    return()
  endif()

  foreach(_ge_asan_dll IN LISTS GE_MSVC_ASAN_RUNTIME_DLLS)
    if(EXISTS "${_ge_asan_dll}")
      get_filename_component(_ge_asan_name "${_ge_asan_dll}" NAME)
      add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${_ge_asan_dll}"
          "$<TARGET_FILE_DIR:${target_name}>/${_ge_asan_name}"
        COMMENT "Copying MSVC AddressSanitizer runtime: ${_ge_asan_name}"
      )
    endif()
  endforeach()
endfunction()


# Platform-specific settings
if(WIN32)
	add_definitions(-DPLATFORM_WINDOWS)
	set(PLATFORM_LIBS "")
elseif(UNIX AND NOT APPLE)
	add_definitions(-DPLATFORM_LINUX)
	set(PLATFORM_LIBS pthread dl)
elseif(APPLE)
	add_definitions(-DPLATFORM_MACOS)
	set(PLATFORM_LIBS pthread dl)
endif()

option(GE_STEAMDECK_BUILD "Enable Steam Deck targeted build markers and defaults" OFF)
if(GE_STEAMDECK_BUILD)
    if(NOT (UNIX AND NOT APPLE))
        message(WARNING "GE_STEAMDECK_BUILD is intended for Linux/SteamOS builds.")
    endif()
    add_compile_definitions(GE_STEAMDECK_BUILD=1 PLATFORM_STEAMDECK=1)
endif()

# When enabled, treat warnings as errors on non-MSVC compilers in dev configs.
option(ENABLE_WERROR_NON_MSVC "Treat warnings as errors on non-MSVC compilers (Debug/RelWithDebInfo)" OFF)

# Compiler-specific options
if(MSVC)
	# MSVC: strict, modern C++ with high warnings.
	#  - /permissive-               : standards-conforming mode (closer to GCC/Clang)
	#  - /Zc:preprocessor           : conforming preprocessor
	#  - /Zc:__cplusplus            : make __cplusplus report the actual standard level
	#  - /W4                        : high warning level
	#  - /WX (Debug/RelWithDebInfo) : treat *compiler* warnings as errors in dev configs
	#  - /MP                        : multi-processor compilation
	#  - /analyze (Debug only)      : static analysis where iteration speed doesn't
	#                                 matter; RelWithDebInfo is the perf-verification
	#                                 config and analysis would roughly double its
	#                                 compile time for coverage Debug already provides.
	#                                 All three /analyze flags travel together — a
	#                                 stranded /analyze:* option implicitly enables
	#                                 analysis on raw command lines (Ninja).
	#  - /analyze:WX-               : keep analysis warnings as warnings even under /WX
	#  - /analyze:external-         : skip analysis diagnostics inside external headers
	add_compile_options(
		/W4
		/permissive-
		/Zc:preprocessor
		/Zc:__cplusplus
		/MP
		$<$<CONFIG:Debug>:/WX>
		$<$<CONFIG:DebugFast>:/WX>
		$<$<CONFIG:RelWithDebInfo>:/WX>
		$<$<CONFIG:Debug>:/analyze>
		$<$<CONFIG:Debug>:/analyze:WX->
		$<$<CONFIG:Debug>:/analyze:external->
	)
	add_definitions(-D_CRT_SECURE_NO_WARNINGS)
else()
	# GCC/Clang: strong warnings everywhere; -Werror only when explicitly requested.
	add_compile_options(
		-Wall
		-Wextra
		-Wpedantic
		-Wno-unused-but-set-parameter
		-Wno-comment
		-Wno-unused-function
		-Wno-unused-but-set-variable
		-Wno-unused-variable
		-Wno-missing-field-initializers
		-Wno-unused-parameter
		$<$<CXX_COMPILER_ID:GNU>:-Wno-changes-meaning>
	)
	if(ENABLE_WERROR_NON_MSVC)
		add_compile_options(
			$<$<CONFIG:Debug>:-Werror>
			$<$<CONFIG:RelWithDebInfo>:-Werror>
		)
	endif()
endif()

# AddressSanitizer toggle (Debug config by default)
option(ENABLE_ASAN "Enable AddressSanitizer (applies to Debug config)" OFF)
if(ENABLE_ASAN)
	message(STATUS "AddressSanitizer: ENABLED")
	if(MSVC)
		# MSVC ASan requires dynamic runtime (/MD, /MDd)
		set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
		# /fsanitize=address is incompatible with /RTC* runtime checks; strip them from Debug flags
		foreach(flag_var CMAKE_C_FLAGS_DEBUG CMAKE_CXX_FLAGS_DEBUG)
			string(REPLACE "/RTC1" "" ${flag_var} "${${flag_var}}")
		endforeach()
		# Apply only to Debug config by default
		add_compile_options($<$<CONFIG:Debug>:/fsanitize=address>)
		# Disable STL container annotations to avoid ODR mismatches with third-party libs (vcpkg
		# deps are built without ASan). _DISABLE_STL_ANNOTATION is the master switch covering
		# vector/string/optional/etc.; MSVC 14.51's STL added an 'annotate_optional' linker key
		# that the individual vector+string disables miss, causing LNK2038/LNK1319.
		add_compile_definitions($<$<CONFIG:Debug>:_DISABLE_STL_ANNOTATION=1>)
		# Avoid incremental linking for better ASan behavior
		add_link_options($<$<CONFIG:Debug>:/INCREMENTAL:NO>)
	else()
		# clang/gcc. Quote the genex and separate the flags with ';': an unquoted space splits
		# it into two list items and the compiler receives the literal '$<1:-fsanitize=address'.
		add_compile_options("$<$<CONFIG:Debug>:-fsanitize=address;-fno-omit-frame-pointer>")
		add_link_options("$<$<CONFIG:Debug>:-fsanitize=address>")
	endif()

	# Expose a compile-time define so code/tests can relax perf thresholds under ASan.
	add_compile_definitions($<$<CONFIG:Debug>:GE_ENABLE_ASAN=1>)
endif()

# Clang-Tidy integration (runs during CMake builds if available)
option(ENABLE_CLANG_TIDY "Run clang-tidy on C++ sources during builds" OFF)
if(ENABLE_CLANG_TIDY)
	find_program(CLANG_TIDY_EXE NAMES clang-tidy clang-tidy.exe)
	if(CLANG_TIDY_EXE)
		# Let .clang-tidy file control checks; just enable the tool
		set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
		message(STATUS "Clang-Tidy enabled: ${CLANG_TIDY_EXE}")
	else()
		message(WARNING "Clang-Tidy requested but not found; skipping static analysis in builds")
	endif()
endif()

# Formatting helper targets (Windows): run Tools/Scripts/format-code.ps1
# These can be invoked explicitly via:
#   cmake --build . --target format
#   cmake --build . --target format-dryrun
# and individual targets can optionally depend on them if desired.
if(WIN32)
	find_program(GE_POWERSHELL_EXE NAMES pwsh.exe pwsh powershell.exe powershell)
	if(GE_POWERSHELL_EXE)
		add_custom_target(format
			COMMAND "${GE_POWERSHELL_EXE}" -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/Tools/Scripts/format-code.ps1"
			WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
			COMMENT "Formatting C++/C#/CMake sources via format-code.ps1"
			VERBATIM
		)
		add_custom_target(format-dryrun
			COMMAND "${GE_POWERSHELL_EXE}" -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/Tools/Scripts/format-code.ps1" -DryRun
			WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
			COMMENT "Dry-run formatting (no files modified) via format-code.ps1"
			VERBATIM
		)
	else()
		message(STATUS "PowerShell not found; 'format' targets will not be available on this platform.")
	endif()
endif()


# Include directories (removed global include paths; targets declare their own)

# Add subdirectories in dependency order

# Emscripten builds compile the engine library graph (Phase 4 of the web
# platform plan): Foundation through Rendering/UI, minus Scripting's managed
# half (CoreCLR hosting; Phase 5 owns the wasm story). The headless smoke
# executables in Tests/Wasm gate the set under node. Desktop GE_WASM=ON without
# the Emscripten toolchain still dry-runs the full static link graph below.
if(EMSCRIPTEN)
    add_subdirectory(Engine/Modules/Logger)
    add_subdirectory(Engine/Modules/Events)
    add_subdirectory(Engine/Modules/GenerationalVector)
    add_subdirectory(Engine/Modules/Types)
    add_subdirectory(Engine/Modules/AssetCore)
    add_subdirectory(Engine/Modules/JobSystem)
    add_subdirectory(Engine/Modules/Mathematics)
    add_subdirectory(Engine/Modules/Serialization)
    add_subdirectory(Engine/Modules/Noise)
    add_subdirectory(Engine/Modules/Memory)
    add_subdirectory(Engine/Modules/Animation)
    add_subdirectory(Engine/ECSModules/Components)
    add_subdirectory(Engine/Modules/ECS)
    add_subdirectory(Engine/Modules/Platform)
    add_subdirectory(Engine/Modules/FileSystem)
    add_subdirectory(Engine/Modules/FileWatcher)
    add_subdirectory(Engine/Modules/Scheduler)
    add_subdirectory(Engine/Modules/NumericExpression)
    add_subdirectory(Engine/Modules/AssetDatabase)
    add_subdirectory(Engine/Modules/AssetRuntime)
    add_subdirectory(Engine/Modules/Audio)
    add_subdirectory(Engine/Modules/Input)
    add_subdirectory(Engine/Modules/Physics)
    add_subdirectory(Engine/Modules/PhysicsECS)
    add_subdirectory(Engine/Modules/Pathfinding)
    add_subdirectory(Engine/Modules/Text)
    add_subdirectory(Engine/Modules/Rendering)
    add_subdirectory(Engine/Modules/Graph)
    add_subdirectory(Engine/Modules/UI)
    # Phase 6 (web Player): the Engine umbrella and the world modules it
    # splices. Scripting compiles its stub path (no nethost on wasm;
    # ENABLE_SCRIPTING is forced OFF above, so GE_ENABLE_SCRIPTING=0).
    add_subdirectory(Engine/Modules/PluginAPI)
    add_subdirectory(Engine/Modules/Scripting)
    add_subdirectory(Engine/Modules/NativeScripting)
    add_subdirectory(Engine/Modules/PlatformServices)
    add_subdirectory(Engine/Modules/PlatformProviders/${GE_PLATFORM_BACKEND})
    add_subdirectory(Engine/Modules/Scene)
    add_subdirectory(Engine/Modules/SceneBvh)
    add_subdirectory(Engine/Modules/Particles)
    add_subdirectory(Engine/Modules/VolumetricFog)
    add_subdirectory(Engine/Modules/HeightFog)
    add_subdirectory(Engine/Modules/GPUFogParticles)
    add_subdirectory(Engine/Modules/Terrain)
    add_subdirectory(Engine/Modules/PageStreaming)
    add_subdirectory(Engine/Modules/TerrainECS)
    add_subdirectory(Engine/Modules/TerrainGrass)
    add_subdirectory(Engine/Modules/CBTTerrain)
    add_subdirectory(Engine/Modules/CBTTerrainECS)
    add_subdirectory(Engine/Modules/Ocean)
    add_subdirectory(Engine/Modules/Spline)
    add_subdirectory(Engine/Modules/SplineGeometry)
    add_subdirectory(Engine/Modules/SplineLayout)
    add_subdirectory(Engine/Modules/SplineECS)
    add_subdirectory(Engine/Modules/MarkupECS)
    add_subdirectory(Engine/Modules/PathfindingECS)
    # This list must cover every entry of Engine/CMakeLists.txt's
    # GE_ENGINE_MODULES: Engine splices each one's objects in, so a module
    # added there but not added here fails at generate time with only
    # "no such target exists" to go on.
    add_subdirectory(Engine)
    # Phase 7 (web Editor): the two modules the Editor splices as objects
    # rather than reaching through Engine's link interface.
    add_subdirectory(Engine/Modules/MeshPicking)
    add_subdirectory(Engine/Modules/VCSIntegration)
    # Apps consume the engine-package helpers (ge_add_engine_package_module,
    # ge_stage_packages); the desktop path includes this after its
    # module graph, which this early-return branch never reaches.
    include(cmake/Packages.cmake)
    add_subdirectory(Apps/Player)
    add_subdirectory(Apps/Editor)
    add_subdirectory(Apps/WebLibrary)
    add_subdirectory(Tests/Wasm)
    return()
endif()

# Logger is the foundation - no dependencies
add_subdirectory(Engine/Modules/Logger)        # Unified logging system for all modules
# GenerationalVector is a standalone utility library - no dependencies
add_subdirectory(Engine/Modules/GenerationalVector)  # Generational handle system
# Types: cross-cutting basic types (Types/Types.h)
add_subdirectory(Engine/Modules/Types)
# Event: header-only multicast event with self-detaching handles (Events/Event.h)
add_subdirectory(Engine/Modules/Events)
# Native plugin lifecycle API used by Engine and Editor extension modules.
add_subdirectory(Engine/Modules/PluginAPI)
# AssetCore depends on Logger + Types
add_subdirectory(Engine/Modules/AssetCore)     # Core asset interfaces and types
# JobSystem depends on Logger + Types
add_subdirectory(Engine/Modules/JobSystem)     # Task execution and dependency management
# ECS depends on Logger + JobSystem
# Mathematics: shared math primitives (e.g., Ray3D) for Engine + Editor + runtime
add_subdirectory(Engine/Modules/Mathematics)
add_subdirectory(Engine/Modules/Serialization)
# Noise depends on Types: shared CPU noise primitives for terrain and any other sampler
add_subdirectory(Engine/Modules/Noise)
add_subdirectory(Engine/Modules/Animation)     # Animation pose, graph nodes, retargeting primitives

# ECSModules/Components: shared ECS/authoring component headers
# (prevents Engine -> PhysicsECS -> Engine circular dependency)
add_subdirectory(Engine/ECSModules/Components)

add_subdirectory(Engine/Modules/ECS)           # Entity Component System
# FileWatcher depends only on Logger; used by Scripting and Engine services
add_subdirectory(Engine/Modules/FileWatcher)     # Standalone file watching

# NativeScripting depends on Logger + Types + FileWatcher; native C++ user-script
# hot-reload loader.
add_subdirectory(Engine/Modules/NativeScripting)

# Scripting depends on Logger + AssetCore + JobSystem; Engine depends on Scripting + ECS
add_subdirectory(Engine/Modules/Scripting)     # CoreCLR hosting and script management
add_subdirectory(Engine/Modules/Platform)       # Platform abstractions stub
# FileSystem depends on Logger + JobSystem + Platform: filesystem operations
# whose std::filesystem spelling is wrong on at least one platform (OPFS).
add_subdirectory(Engine/Modules/FileSystem)
add_subdirectory(Engine/Modules/PlatformServices) # Platform-blind services facade (accounts/saves/achievements)
add_subdirectory(Engine/Modules/PlatformProviders/${GE_PLATFORM_BACKEND}) # Selected platform provider
add_subdirectory(Engine/Modules/Memory)         # Memory manager stub
add_subdirectory(Engine/Modules/AssetDatabase)  # Git-friendly authoritative asset DB + derived cache
add_subdirectory(Engine/Modules/AssetRuntime)   # Runtime asset services (managers/graph/tasks/streaming)

add_subdirectory(Engine/Modules/Audio)          # Audio system module (miniaudio backend)

add_subdirectory(Engine/Modules/Input)          # Input system module

# Physics modules (Engine assembles them)
add_subdirectory(Engine/Modules/Physics)
add_subdirectory(Engine/Modules/PhysicsECS)

# Pathfinding modules
add_subdirectory(Engine/Modules/Pathfinding)
add_subdirectory(Engine/Modules/PathfindingECS)

# CPU mesh picking primitives (BVH, ray-triangle). Editor-side glue (asset
# lookup, terrain raymarch, scene walk) lives in Apps/Editor/Source/Picking.
add_subdirectory(Engine/Modules/MeshPicking)

# Scene-level CPU spatial index (TLAS over per-entity AABBs). Shared by
# picking, marquee select, AI line-of-sight, audio occlusion, streaming.
add_subdirectory(Engine/Modules/Scene)

# GPU-consumable threaded BVH (build/refit/pack) for the software ray-traversal
# lane used where hardware ray queries are unavailable.
add_subdirectory(Engine/Modules/SceneBvh)

# Terrain modules
add_subdirectory(Engine/Modules/Terrain)
add_subdirectory(Engine/Modules/PageStreaming) # Terrain page stores, cooker, page loader
add_subdirectory(Engine/Modules/TerrainECS)
add_subdirectory(Engine/Modules/TerrainGrass)

# Ocean rendering module
add_subdirectory(Engine/Modules/Ocean)

# Spline modules
add_subdirectory(Engine/Modules/Spline)
add_subdirectory(Engine/Modules/SplineGeometry)
add_subdirectory(Engine/Modules/SplineLayout)
add_subdirectory(Engine/Modules/SplineECS)
add_subdirectory(Engine/Modules/MarkupECS)

# Rendering post-process modules
add_subdirectory(Engine/Modules/VolumetricFog)
add_subdirectory(Engine/Modules/HeightFog)
add_subdirectory(Engine/Modules/GPUFogParticles)
add_subdirectory(Engine/Modules/Particles) # Neutral portable particle components

# First-party packages (Packages/): staging helpers + eztree preset
# discovery. The eztree package (runtime AND editor halves) ships entirely as
# Packages/eztree native modules — no engine or editor code compiles in.
include(cmake/Packages.cmake)

add_subdirectory(Engine)
# Always build Rendering; gate only UI and Editor
add_subdirectory(Engine/Modules/Text) # Text stack (FreeType/HarfBuzz/SDF)

add_subdirectory(Engine/Modules/Rendering) # Modern GPU-driven rendering library
# CBT/LEB terrain render core (concurrent binary tree). Added after Rendering so
# it can link GameEngineRendering and reuse the GTest target the Rendering module
# establishes. Standalone in C1 (module + kernels + tests); Engine/ECS wiring is C2.
add_subdirectory(Engine/Modules/CBTTerrain)
add_subdirectory(Engine/Modules/CBTTerrainECS) # CBT terrain Engine integration (feature/node/ECS, C3)
add_subdirectory(Engine/Modules/Scheduler) # Reusable time-based scheduler module
add_subdirectory(Engine/Modules/NumericExpression) # TinyExpr-backed math expression evaluation
add_subdirectory(Engine/Modules/UI) # UI system module
add_subdirectory(Engine/Modules/VCSIntegration) # Common VCS abstraction layer
add_subdirectory(Engine/Modules/Graph) # Graph model and asset module

option(BUILD_EDITOR "Build the Editor application" ON)

# Managed (.NET) assembly targets. Included here, ahead of every subdirectory
# that declares a dependency on them (Apps/Editor, Apps/Player, Tests), because
# a consumer configured first sees if(TARGET SomeAssembly) as false and silently
# drops the dependency.
include(cmake/ManagedAssemblies.cmake)

if(BUILD_EDITOR)
    add_subdirectory(Apps/Editor) # Editor application
endif()

option(BUILD_PLAYER "Build the standalone Player runtime" ON)
# The editor's standalone-record path (Movie Recorder panel launching Player.exe
# with --write-movie) requires this ON; shipped games leave it OFF.
option(PLAYER_MOVIE_RECORDER "Compile the movie recorder (--write-movie capture/encode) into the Player" OFF)
if(BUILD_PLAYER)
    add_subdirectory(Apps/Player) # Standalone Player runtime
    # The editor's MovieRecorder + Build/export panels invoke the staged Player.exe, so building the
    # Editor must also produce the Player. Cheap in practice: the Player links Engine's import lib,
    # so it only relinks when Engine's exported-symbol set changes — not on every Engine edit.
    if(TARGET Editor)
        add_dependencies(Editor Player)
    endif()
endif()

# UISystem and Editor removed during redesign

if(BUILD_EXAMPLES)
  add_subdirectory(Examples)      # standalone demos
endif()
if(BUILD_TESTING)
  add_subdirectory(Tests)
endif()
if(BUILD_TOOLS)
  add_subdirectory(Tools)
endif()
if(ENABLE_SCRIPTING)
  # Convenience target to build only scripting-related artifacts
  # - Engine (contains CoreCLRHost and scripting code)
  # - GameEngine.Native (C ABI shim)
  # - Managed HotReload/CoreBridge (via ScriptingAbiIntegrationTests dependency)
  # - ScriptingAbiIntegrationTests (links the above and stages managed outputs)
  add_custom_target(ScriptingOnly
      COMMENT "Build scripting native and managed artifacts (Engine + GameEngine.Native + ScriptingAbiIntegrationTests)"
  )
  add_dependencies(ScriptingOnly Engine GameEngine.Native)
  if(TARGET ScriptingAbiIntegrationTests)
    add_dependencies(ScriptingOnly ScriptingAbiIntegrationTests)
  endif()
  if(TARGET ScriptingDiag)
    add_dependencies(ScriptingOnly ScriptingDiag)
  endif()
endif()

# NOTE: Assets and examples are staged at build time by individual targets (POST_BUILD)
# so outputs stay isolated (Editor vs Tests vs Demos) and always up-to-date.

# Hot-reload stress tests for race condition detection
if(BUILD_TESTING)
    # Find Google Test
    find_package(GTest REQUIRED)

    add_executable(HotReloadStressTests
        Tests/HotReloadStressTests.cpp
    )

    # Engine ONLY — no direct AssetCore/JobSystem link. Those are OBJECT
    # modules already spliced into Engine.dll; linking them here again gives
    # the exe a second copy of every module static (two m_NextTaskId counters
    # → TaskId collisions in a shared pool's registry, issue #340). Headers
    # and symbols resolve through Engine's INTERFACE + export table.
    target_link_libraries(HotReloadStressTests
        PRIVATE
        Engine
        GTest::gtest
        GTest::gtest_main
    )

    # Output layout: bin/<Config>/Tests
    ge_set_output_tests(HotReloadStressTests)
    ge_stage_runtime_dependencies(HotReloadStressTests)

    # Add to CTest
    add_test(NAME HotReloadStressTests COMMAND $<TARGET_FILE:HotReloadStressTests>)

    set_tests_properties(HotReloadStressTests PROPERTIES TIMEOUT 120 LABELS "Stress;HotReload")

    message(STATUS "Hot-reload stress tests configured")
endif()

# Debug-server / tool-registry drift guard: every RegisterHandler() in
# Apps/Editor/Source/DebugServer must either be exposed as a tool in
# mcp/src/tools (MCP + CLI) or be listed in mcp/ipc-method-coverage.json
# with a reason for staying raw-only. Reads both sides as text, so it needs
# neither a built editor nor a built mcp/dist.
if(BUILD_TESTING)
    find_program(GE_NODE_EXECUTABLE NAMES node node.exe)
    if(GE_NODE_EXECUTABLE)
        add_test(NAME EditorIpcMethodDrift
            COMMAND ${GE_NODE_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/Tests/Mcp/ipc-method-drift.mjs)
        set_tests_properties(EditorIpcMethodDrift PROPERTIES TIMEOUT 60 LABELS "Editor")
    else()
        message(STATUS "node not found - EditorIpcMethodDrift test not registered")
    endif()
endif()

# Editor attribution guard: every MCP/CLI answer that came from an editor must
# name which editor answered. Several editors run at once and a call that lands
# on the wrong one succeeds silently, so this drives the CLI against fake
# editors on several ports and fails if the reported endpoint is missing,
# constant, taken from configuration instead of the socket, dropped on a
# refusal, or invented when nothing answered. Needs no editor binary; the first
# run builds mcp/dist, which is what the budget is sized for.
#
# The 600s TIMEOUT sits above the harness's own 540s total budget on purpose:
# the harness names the arm that hung, ctest only reports that something did,
# so the backstop must be the one that never fires first.
if(BUILD_TESTING)
    find_program(GE_NODE_EXECUTABLE NAMES node node.exe)
    if(GE_NODE_EXECUTABLE)
        add_test(NAME McpEditorEndpointReport
            COMMAND ${GE_NODE_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/Tests/Mcp/mcp-editor-endpoint-check.mjs)
        set_tests_properties(McpEditorEndpointReport PROPERTIES TIMEOUT 600 LABELS "Editor")
    else()
        message(STATUS "node not found - McpEditorEndpointReport test not registered")
    endif()
endif()

# The AI Assistant runs the MCP server staged beside the editor executable. Called
# here, in the directory that defines McpServer, once the Editor target exists.
if(TARGET Editor)
    ge_stage_mcp_server_beside(Editor)
endif()

# The MCP server as the AI Assistant attaches it: GE_MCP_TOOLS serves exactly the
# named tools and resources and refuses an unknown name at start, every connection
# binds with GE_ASSISTANT_TOKEN first and a refused binding refuses every call, and
# no tool writes a file or starts a process without asking the editor first
# (assistant_authorize). Fake editor, no editor binary; the first run may build
# mcp/dist, hence the McpEditorEndpointReport budget.
if(BUILD_TESTING)
    find_program(GE_NODE_EXECUTABLE NAMES node node.exe)
    if(GE_NODE_EXECUTABLE)
        add_test(NAME McpAssistantAttachment
            COMMAND ${GE_NODE_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/Tests/Mcp/mcp-assistant-check.mjs)
        set_tests_properties(McpAssistantAttachment PROPERTIES TIMEOUT 600 LABELS "Editor")
    else()
        message(STATUS "node not found - McpAssistantAttachment test not registered")
    endif()
endif()

# rgp_capture vendor gate: the Radeon GPU Profiler flow is UNVERIFIED on AMD —
# written on an NVIDIA machine — so the behaviour that must not rot is the gate
# refusing on non-AMD hardware while naming what it actually found, and the
# output scan refusing to report a stale .rgp as a fresh capture. Drives the CLI
# against fake editors reporting each vendor, so it needs no Radeon GPU and no
# editor binary. Same 600s budget rationale as McpEditorEndpointReport: the
# first run may build mcp/dist.
if(BUILD_TESTING)
    find_program(GE_NODE_EXECUTABLE NAMES node node.exe)
    if(GE_NODE_EXECUTABLE)
        add_test(NAME McpRgpCaptureGate
            COMMAND ${GE_NODE_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/Tests/Mcp/rgp-capture-check.mjs)
        set_tests_properties(McpRgpCaptureGate PROPERTIES TIMEOUT 600 LABELS "Editor")
    else()
        message(STATUS "node not found - McpRgpCaptureGate test not registered")
    endif()
endif()

# Web library gates, both needing no wasm build (the first run installs the
# package's pinned typescript): WebLibraryDeclarations compiles the public
# TypeScript declarations under Apps/WebLibrary/ts, their type tests and the
# documentation's examples under `tsc --noEmit`; WebLibraryFacade builds the
# facade and runs its node tests against a stubbed engine module.
if(BUILD_TESTING)
    find_program(GE_NODE_EXECUTABLE NAMES node node.exe)
    if(GE_NODE_EXECUTABLE)
        add_test(NAME WebLibraryDeclarations
            COMMAND ${GE_NODE_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/Apps/WebLibrary/ts/test/check-declarations.mjs)
        set_tests_properties(WebLibraryDeclarations PROPERTIES TIMEOUT 300 LABELS "Web")
        add_test(NAME WebLibraryFacade
            COMMAND ${GE_NODE_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/Apps/WebLibrary/ts/test/run-tests.mjs)
        set_tests_properties(WebLibraryFacade PROPERTIES TIMEOUT 300 LABELS "Web"
            RESOURCE_LOCK WebLibraryPackage)
        set_tests_properties(WebLibraryDeclarations PROPERTIES RESOURCE_LOCK WebLibraryPackage)
    else()
        message(STATUS "node not found - WebLibrary tests not registered")
    endif()
endif()

# Standalone CLI: gameenginecli — the mcp/ tool registry packaged as a native
# executable (Node Single Executable Application). End users need no node
# installed: the binary embeds the runtime and speaks TCP to the editor's
# debug server directly. Node/npm are BUILD-time requirements only, and the
# repo toolchain already requires them (mcp build, drift gate, converters).
# SEA does not cross-compile — each platform's build produces its own binary,
# like every other target. Opt-in: --target GameEngineCli; stages into
# bin/<config>/ beside the other binaries.
find_program(GE_NODE_EXECUTABLE NAMES node node.exe)
# npm.cmd first: on Windows the extensionless `npm` beside node.exe is a POSIX
# shell script, and running it outside a POSIX shell resolves platform-specific
# packages wrongly. npm.cmd only exists on Windows, so mac/linux fall through.
find_program(GE_NPM_EXECUTABLE NAMES npm.cmd npm)
if(GE_NODE_EXECUTABLE AND GE_NPM_EXECUTABLE)
    add_custom_target(GameEngineCli
        COMMAND ${GE_NPM_EXECUTABLE} install --no-audit --no-fund
        COMMAND ${GE_NPM_EXECUTABLE} run build
        COMMAND ${GE_NODE_EXECUTABLE} scripts/build-sea.mjs --out-dir ${CMAKE_BINARY_DIR}/bin/$<CONFIG>
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/mcp
        COMMENT "Packaging gameenginecli (standalone, Node SEA)"
        VERBATIM)
else()
    message(STATUS "node/npm not found - GameEngineCli target not registered")
endif()
