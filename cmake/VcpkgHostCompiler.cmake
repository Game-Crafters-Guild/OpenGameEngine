# The C and C++ compilers vcpkg builds this tree's packages with, held to the
# pair the build directory's first configure resolved.
#
# vcpkg identifies the compiler by running CMake's compiler detection in the
# environment it inherits, and the compiler binary's hash enters every
# package's ABI. A configure the build system re-runs (an edited CMakeLists.txt,
# a CONFIGURE_DEPENDS glob that changed) inherits the build system's
# environment, not the shell's: xcodebuild puts its toolchain's usr/bin first on
# PATH, so detection inside an Xcode build finds
# .../XcodeDefault.xctoolchain/usr/bin/c++ where the configure from a terminal
# found /usr/bin/c++. Every package's ABI then changes, vcpkg replaces the whole
# installed set, and ge_check_vcpkg_install_set_unchanged() refuses the build
# directory, rightly: its objects were compiled against the other set.
#
# So the first configure records CC and CXX as its environment resolves them
# (the exported value, else the first cc and c++ on PATH, found with
# find_program, which is what vcpkg's own detection found before), and every configure exports the
# recorded pair to the vcpkg processes it starts. Recording what the shell
# resolves, rather than naming a compiler, keeps every package's ABI what it was,
# so the binary cache other trees filled still restores.
#
# Windows hosts hold nothing: vcpkg resolves MSVC through the Visual Studio
# installation there, not through PATH.

# Records ${cacheVariable} for this build directory once: the value of
# ${environmentVariable}, or else the first ${program} on PATH.
function(z_ge_vcpkg_record_host_compiler cacheVariable environmentVariable program)
    if(DEFINED CACHE{${cacheVariable}})
        return()
    endif()
    if(NOT "$ENV{${environmentVariable}}" STREQUAL "")
        set(compiler "$ENV{${environmentVariable}}")
    else()
        find_program(compiler NAMES ${program} NO_CACHE)
        if(NOT compiler)
            # Nothing to hold; project() reports the missing compiler.
            return()
        endif()
    endif()
    set(${cacheVariable} "${compiler}" CACHE INTERNAL
        "The ${environmentVariable} vcpkg builds this build directory's packages with, recorded at its first configure")
endfunction()

# Exports the held pair as CC and CXX into this configure's environment, which
# every vcpkg process it starts inherits: the toolchain's manifest install at
# project() and a first configure's own install. Call before either can run.
function(ge_vcpkg_hold_host_compilers)
    if(CMAKE_HOST_WIN32)
        return()
    endif()
    z_ge_vcpkg_record_host_compiler(GE_VCPKG_HOST_C_COMPILER CC cc)
    z_ge_vcpkg_record_host_compiler(GE_VCPKG_HOST_CXX_COMPILER CXX c++)
    if(DEFINED CACHE{GE_VCPKG_HOST_C_COMPILER})
        set(ENV{CC} "${GE_VCPKG_HOST_C_COMPILER}")
    endif()
    if(DEFINED CACHE{GE_VCPKG_HOST_CXX_COMPILER})
        set(ENV{CXX} "${GE_VCPKG_HOST_CXX_COMPILER}")
    endif()
endfunction()

# Sets ${outVar} to the `cmake -E env` arguments that give a vcpkg process the
# held pair, for a vcpkg run after project(), where a cross toolchain
# (Emscripten) may have exported its own compiler into this process; on
# Windows, the arguments that unset CC and CXX.
function(ge_vcpkg_host_compiler_env outVar)
    set(arguments --unset=CC --unset=CXX)
    if(NOT CMAKE_HOST_WIN32)
        if(DEFINED CACHE{GE_VCPKG_HOST_C_COMPILER})
            list(APPEND arguments "CC=${GE_VCPKG_HOST_C_COMPILER}")
        endif()
        if(DEFINED CACHE{GE_VCPKG_HOST_CXX_COMPILER})
            list(APPEND arguments "CXX=${GE_VCPKG_HOST_CXX_COMPILER}")
        endif()
    endif()
    set(${outVar} ${arguments} PARENT_SCOPE)
endfunction()
