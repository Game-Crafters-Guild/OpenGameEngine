# The inputs of one managed (.NET) project's build, for the DEPENDS of the step
# that runs `dotnet build` on it.
#
# dotnet writes a project's outputs into the project directory itself: obj/
# (generated .cs files, the NuGet .props and .targets) and bin/. The globs here
# are CONFIGURE_DEPENDS, so the build system re-globs them before every build
# and re-runs CMake when the result differs. A glob that can see obj/ differs
# after every dotnet build that writes a new output there (the first build of a
# configuration, a source generator's first output), so the next build re-runs
# CMake for nothing; filtering the result afterwards does not help, because the
# build system re-globs the patterns, not the filtered list. These patterns
# therefore never reach obj/ or bin/: the project directory's own files, plus a
# recursive glob under every top-level entry not named obj or bin. file(GLOB)
# cannot exclude, so "every name except obj and bin" is spelled out as the
# patterns below. They also leave out names that start with a dot, which the
# .NET SDK does not compile either. A source added anywhere else, a new
# subdirectory included, is still seen at the next build.
include_guard(GLOBAL)

# Sets ${outVar} to every .cs, .csproj, .props and .targets file under
# ${projectDir}, except under its top-level obj/ and bin/, and registers the
# globs so that adding or removing such a file re-runs CMake.
function(ge_managed_project_sources outVar projectDir)
    # Every top-level entry name other than obj, bin and a dot name: names that
    # start with neither b nor o, then the b... and o... names that are not
    # exactly bin or obj. The glob is case-insensitive on macOS and Windows,
    # where the file system is too, so OBJ and Bin are left out there as well.
    # Local to the function: include_guard(GLOBAL) skips this file in every
    # directory scope after the first, so a file-level variable would be unset
    # where a later directory calls the function.
    set(subdirectoryPatterns
        "[!.bo]*"
        "b" "b[!i]*" "bi" "bi[!n]*" "bin?*"
        "o" "o[!b]*" "ob" "ob[!j]*" "obj?*")
    set(rootPatterns "")
    set(nestedPatterns "")
    foreach(extension IN ITEMS cs csproj props targets)
        list(APPEND rootPatterns "${projectDir}/*.${extension}")
        foreach(subdir IN LISTS subdirectoryPatterns)
            list(APPEND nestedPatterns "${projectDir}/${subdir}/*.${extension}")
        endforeach()
    endforeach()
    file(GLOB rootSources CONFIGURE_DEPENDS ${rootPatterns})
    file(GLOB_RECURSE nestedSources CONFIGURE_DEPENDS ${nestedPatterns})
    set(${outVar} ${rootSources} ${nestedSources} PARENT_SCOPE)
endfunction()
