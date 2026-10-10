# Contract test for cmake/DotnetHostVersions.cmake, run by ctest as
#   cmake -DSCRIPT=<version script> -P <this file>
# Covered: installed host pack directories are ordered by version number, so a
# machine with .NET 8, 9 and 10 resolves nethost and hostfxr from 10.x, and two
# patches of one major order by patch number.
if(NOT DEFINED SCRIPT)
    message(FATAL_ERROR "DotnetHostVersionsTest.cmake: SCRIPT is required")
endif()
include("${SCRIPT}")

set(_root "C:/Program Files/dotnet/packs/Microsoft.NETCore.App.Host.win-x64")
set(_versions "${_root}/9.0.20" "${_root}/10.0.2" "${_root}/8.0.23" "${_root}/10.0.12" "${_root}/9.0.12")
ge_dotnet_versions_newest_first(_versions)

set(_expected "${_root}/10.0.12" "${_root}/10.0.2" "${_root}/9.0.20" "${_root}/9.0.12" "${_root}/8.0.23")
if(NOT _versions STREQUAL _expected)
    message(FATAL_ERROR
        "DotnetHostVersionsTest: expected newest-first by version number\n  ${_expected}\n"
        "got\n  ${_versions}")
endif()
message(STATUS "DotnetHostVersionsTest: passed")
