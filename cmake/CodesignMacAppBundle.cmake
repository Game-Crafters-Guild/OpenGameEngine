# CodesignMacAppBundle.cmake
#
# Post-build helper for macOS app bundles.
#
# Problem:
# - Xcode may codesign the bundle, but our post-build staging/fixups (vcpkg applocal,
#   install_name_tool rpath fixes, etc.) can modify binaries after that signing step.
# - Launching from Finder/Dock (LaunchServices) is much stricter about signature validity
#   than launching the inner binary directly from a terminal.
#
# Solution:
# - Re-sign the produced .app bundle at the end of the post-build pipeline.
# - Prefer an explicit GE_CODESIGN_IDENTITY when provided, then Xcode's signing
#   identity, then fall back to ad-hoc signing ("-").
#
# Inputs (via -D on cmake -P):
# - GE_APP_BUNDLE: Full path to the .app bundle directory (Editor.app)
# - GE_CODESIGN_IDENTITY: Optional stable signing identity name/hash.
# - GE_APP_EXECUTABLE: Optional executable path; when supplied, skip signing
#   until the executable exists (useful for pre-link staging targets).
# - GE_CODESIGN_SDK_LIBRARIES: ON signs only the libraries the bundle's staged
#   SDK links (their Frameworks targets), each in place, and leaves the bundle
#   unsigned. The Editor runs this before writing its engine_abi markers.
#

if(NOT APPLE)
  return()
endif()

if(NOT DEFINED GE_APP_BUNDLE OR GE_APP_BUNDLE STREQUAL "")
  message(FATAL_ERROR "CodesignMacAppBundle: GE_APP_BUNDLE must be set to a .app path")
endif()

if(DEFINED GE_APP_EXECUTABLE AND NOT EXISTS "${GE_APP_EXECUTABLE}")
  return()
endif()

if(NOT EXISTS "${GE_APP_BUNDLE}")
  message(FATAL_ERROR "CodesignMacAppBundle: bundle not found: ${GE_APP_BUNDLE}")
endif()

find_program(_ge_codesign codesign REQUIRED)

# Prefer an explicit identity from CMake, then Xcode's expanded signing identity
# when building from Xcode. If neither is present, use ad-hoc signing.
set(_identity "")
if(DEFINED GE_CODESIGN_IDENTITY AND NOT GE_CODESIGN_IDENTITY STREQUAL "")
  set(_identity "${GE_CODESIGN_IDENTITY}")
endif()
if(_identity STREQUAL "")
  set(_identity "$ENV{GE_CODESIGN_IDENTITY}")
endif()
if(_identity STREQUAL "")
  set(_identity "$ENV{EXPANDED_CODE_SIGN_IDENTITY}")
endif()
if(_identity STREQUAL "")
  set(_identity "-")
endif()

# The engine_abi markers beside prebuilt engine-package modules hash the staged
# SDK libraries' size and modification time, and signing a library changes
# both. The first signature replaces a linker's and changes the size, so the
# Editor signs these libraries (GE_CODESIGN_SDK_LIBRARIES) before it writes the
# markers; every later signature with the same identity keeps that size. SDK
# symlinks resolve to their Frameworks targets here, so the deduplicated and
# regular-file layouts behave identically.
file(GLOB _sdk_dylibs "${GE_APP_BUNDLE}/Contents/MacOS/SDK/lib/*/*.dylib")
set(_sdk_libraries "")
foreach(_sdk_dylib IN LISTS _sdk_dylibs)
  file(REAL_PATH "${_sdk_dylib}" _sdk_dylib_real)
  if(EXISTS "${_sdk_dylib_real}")
    list(FIND _sdk_libraries "${_sdk_dylib_real}" _sdk_library_index)
    if(_sdk_library_index EQUAL -1)
      list(APPEND _sdk_libraries "${_sdk_dylib_real}")
    endif()
  endif()
endforeach()

if(GE_CODESIGN_SDK_LIBRARIES)
  foreach(_sdk_library IN LISTS _sdk_libraries)
    execute_process(COMMAND chmod u+w "${_sdk_library}")
    execute_process(
      COMMAND "${_ge_codesign}" --force --sign "${_identity}" "${_sdk_library}"
      RESULT_VARIABLE _sdk_rc
      ERROR_VARIABLE _sdk_err
    )
    if(NOT _sdk_rc EQUAL 0)
      message(FATAL_ERROR "CodesignMacAppBundle: codesign of '${_sdk_library}' failed (rc=${_sdk_rc})\n${_sdk_err}")
    endif()
  endforeach()
  return()
endif()

# Ensure all files in the bundle are writable so that xattr and codesign can
# modify them. vcpkg's applocal.py copies dylibs preserving read-only perms.
find_program(_ge_chmod chmod)
if(_ge_chmod)
  execute_process(
    COMMAND "${_ge_chmod}" -R u+w "${GE_APP_BUNDLE}"
  )
endif()

# Strip extended attributes (com.apple.provenance, quarantine, etc.) from the
# entire bundle before signing. Files written by certain macOS APIs carry these
# automatically, and codesign --deep rejects bundles that contain them.
find_program(_ge_xattr xattr)
if(_ge_xattr)
  execute_process(
    COMMAND "${_ge_xattr}" -cr "${GE_APP_BUNDLE}"
    RESULT_VARIABLE _xattr_rc
  )
endif()

# The deep sign below re-signs the SDK libraries with the same signature and
# size but updates their mtime; snapshot and restore it so the markers stay
# valid.
set(_mtime_paths "${_sdk_libraries}")
set(_mtime_stamps "")
if(_mtime_paths)
  find_program(_ge_touch touch REQUIRED)
  string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef _mtime_nonce)
  if(DEFINED ENV{TMPDIR} AND NOT "$ENV{TMPDIR}" STREQUAL "")
    set(_mtime_dir "$ENV{TMPDIR}/ge-codesign-mtimes-${_mtime_nonce}")
  else()
    set(_mtime_dir "/tmp/ge-codesign-mtimes-${_mtime_nonce}")
  endif()
  file(MAKE_DIRECTORY "${_mtime_dir}")
  set(_mtime_index 0)
  foreach(_mtime_path IN LISTS _mtime_paths)
    set(_mtime_stamp "${_mtime_dir}/${_mtime_index}")
    file(TOUCH "${_mtime_stamp}")
    execute_process(
      COMMAND "${_ge_touch}" -r "${_mtime_path}" "${_mtime_stamp}"
      RESULT_VARIABLE _touch_snapshot_rc
    )
    if(NOT _touch_snapshot_rc EQUAL 0)
      message(FATAL_ERROR
        "CodesignMacAppBundle: failed to snapshot mtime for '${_mtime_path}'")
    endif()
    list(APPEND _mtime_stamps "${_mtime_stamp}")
    math(EXPR _mtime_index "${_mtime_index} + 1")
  endforeach()
endif()

execute_process(
  COMMAND "${_ge_codesign}" --force --deep --sign "${_identity}" "${GE_APP_BUNDLE}"
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE _err
)

if(_mtime_paths)
  list(LENGTH _mtime_paths _mtime_count)
  math(EXPR _mtime_last "${_mtime_count} - 1")
  foreach(_mtime_index RANGE "${_mtime_last}")
    list(GET _mtime_paths "${_mtime_index}" _mtime_path)
    list(GET _mtime_stamps "${_mtime_index}" _mtime_stamp)
    execute_process(
      COMMAND "${_ge_touch}" -r "${_mtime_stamp}" "${_mtime_path}"
      RESULT_VARIABLE _touch_restore_rc
    )
    if(NOT _touch_restore_rc EQUAL 0)
      message(FATAL_ERROR
        "CodesignMacAppBundle: failed to restore mtime for '${_mtime_path}'")
    endif()
  endforeach()
  file(REMOVE_RECURSE "${_mtime_dir}")
endif()

if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "CodesignMacAppBundle: codesign failed (rc=${_rc})\n${_err}")
endif()
