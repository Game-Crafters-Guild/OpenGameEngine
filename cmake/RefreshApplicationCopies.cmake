# RefreshApplicationCopies.cmake
#
# Script-mode POST_BUILD step of a library the applications stage beside
# themselves (ge_refresh_application_copies in cmake/OutputLayout.cmake): copies
# the fresh link over the staged copy in each registered application directory.
# A directory without a staged copy is skipped: that application has not been
# built, and its own staging creates the copy when it is.
#
# Inputs:
# - GE_LIBRARY_FILE            : full path to the linked library
# - GE_APPLICATION_DIRECTORIES : application directories (CMake list)

include("${CMAKE_CURRENT_LIST_DIR}/GeStagedCopy.cmake")

get_filename_component(_ge_library_name "${GE_LIBRARY_FILE}" NAME)
foreach(_ge_application_dir IN LISTS GE_APPLICATION_DIRECTORIES)
  if(EXISTS "${_ge_application_dir}/${_ge_library_name}")
    # A copy renamed aside by an earlier refresh, while a running process held
    # it, is removed here once that process has let go.
    ge_staged_copy_cleanup("${_ge_application_dir}")
    ge_staged_copy("${GE_LIBRARY_FILE}" "${_ge_application_dir}" "RefreshApplicationCopies")
  endif()
endforeach()
