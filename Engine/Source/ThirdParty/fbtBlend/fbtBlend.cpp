// Single-translation-unit shim for the header-only fbtBlend library.
// fbtBlend is "header-only" in the stb-style sense: define
// FBTBLEND_IMPLEMENTATION in exactly one .cpp before the include and the
// declarations + definitions both compile from the same header.
//
// Compression macros (must be defined here AND in any TU that includes
// fbtBlend.h afterwards if it queries enums; engine code that just opens a
// .blend through fbtBlendFile only needs them in this TU):
//   FBT_USE_GZ_FILE=1     - zlib-compressed .blend (Blender < 3.0)
//   FBT_USE_ZSTD_FILE=1   - zstd-compressed .blend (Blender >= 3.0)
//
// CMake (Engine/CMakeLists.txt) gates these via GE_FBTBLEND_HAVE_ZLIB /
// GE_FBTBLEND_HAVE_ZSTD; if either is unavailable on a build machine the
// corresponding compressed-file path is gracefully unsupported.

#if defined(GE_FBTBLEND_HAVE_ZLIB)
#  define FBT_USE_GZ_FILE 1
#endif

#if defined(GE_FBTBLEND_HAVE_ZSTD)
#  define FBT_USE_ZSTD_FILE 1
#endif

// Silence compiler warnings inside vendored upstream code. fbtBlend is
// pre-existing third-party source; we don't fix its warnings, we suppress
// them at the TU boundary.
#if defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable: 4189) // local variable initialized but not referenced
#  pragma warning(disable: 4244) // narrowing conversions
#  pragma warning(disable: 4245) // signed/unsigned mismatch on return
#  pragma warning(disable: 4267) // size_t narrowing
#  pragma warning(disable: 4305) // truncation
#  pragma warning(disable: 4309) // truncation of constant
#  pragma warning(disable: 4456) // shadowing locals
#  pragma warning(disable: 4457) // hides function parameter
#  pragma warning(disable: 4458) // hides class member
#  pragma warning(disable: 4459) // hides global declaration
#  pragma warning(disable: 4505) // unreferenced static
#  pragma warning(disable: 4554) // operator precedence
#  pragma warning(disable: 4838) // narrowing conversion
#  pragma warning(disable: 4996) // POSIX deprecation
#  pragma warning(disable: 6326) // potential comparison of two constants
#elif defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#  pragma GCC diagnostic ignored "-Wunused-variable"
#  pragma GCC diagnostic ignored "-Wunused-parameter"
#  pragma GCC diagnostic ignored "-Wsign-compare"
#  pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

#define FBTBLEND_IMPLEMENTATION
#include "fbtBlend.h"

#if defined(_MSC_VER)
#  pragma warning(pop)
#elif defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
