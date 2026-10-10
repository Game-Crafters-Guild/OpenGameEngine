# Refuses a source tree whose path the dependency builds cannot take: a path
# with a space, or one under the Windows Program Files folders. Several vcpkg
# ports (harfbuzz's meson install among them) fail deep into the first configure
# on such a path, with an error that does not name the path. The top-level
# CMakeLists.txt calls this before anything provisions or runs vcpkg, which is
# the first point a configure can be stopped cheaply.

# ge_check_clone_path(<sourceDir>)
function(ge_check_clone_path sourceDir)
    file(TO_CMAKE_PATH "${sourceDir}" path)
    set(reason "")
    if(CMAKE_HOST_WIN32)
        string(TOLOWER "${path}" lowerPath)
        if(lowerPath MATCHES "^[a-z]:/program files( \\(x86\\))?(/|$)")
            set(reason "lies under Program Files")
        endif()
    endif()
    if(reason STREQUAL "" AND path MATCHES " ")
        set(reason "contains a space")
    endif()
    if(reason STREQUAL "")
        return()
    endif()

    if(CMAKE_HOST_WIN32)
        set(example [[C:\Dev\GameEngine]])
    else()
        set(example "~/Dev/GameEngine")
    endif()
    file(TO_NATIVE_PATH "${path}" nativePath)
    message(FATAL_ERROR
        "The source directory ${nativePath} ${reason}, which the dependency builds cannot "
        "handle. Clone the repository to a short path with no spaces outside Program Files, "
        "for example ${example}, and configure again.")
endfunction()

# Entry point for the contract test (cmake/Tests/ClonePathGuardTest.cmake),
# which checks paths that cannot be created on the machine running it.
if(CMAKE_SCRIPT_MODE_FILE)
    if(NOT DEFINED SOURCE_DIR)
        message(FATAL_ERROR "ClonePathGuard.cmake: SOURCE_DIR is required in script mode")
    endif()
    ge_check_clone_path("${SOURCE_DIR}")
endif()
