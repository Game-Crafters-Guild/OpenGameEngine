#pragma once

/// @file
/// Warning-free anonymous structs inside anonymous unions.
///
/// The vector types expose their components both by name (`x`, `y`, `z`, `w`) and as an
/// array (`v`) through an anonymous union holding an anonymous struct. Anonymous structs
/// are a language extension that every supported compiler accepts; each one warns about it
/// under the tree's warning level (MSVC C4201 under `/W4 /WX`, Clang
/// `-Wgnu-anonymous-struct` and `-Wnested-anon-types`, GCC `-Wpedantic`).
/// `GE_ANONYMOUS_STRUCT_BEGIN` and `GE_ANONYMOUS_STRUCT_END` bracket the declaration and
/// silence exactly those diagnostics for it, nothing else.
///
/// A class with an anonymous union member cannot be decomposed with a structured binding
/// and cannot default its comparison operators; the vector types spell `operator==` out.

#if defined(__clang__)
#define GE_ANONYMOUS_STRUCT_BEGIN                                                                  \
    _Pragma("clang diagnostic push") _Pragma("clang diagnostic ignored \"-Wgnu-anonymous-struct\"") \
        _Pragma("clang diagnostic ignored \"-Wnested-anon-types\"")
#define GE_ANONYMOUS_STRUCT_END _Pragma("clang diagnostic pop")
#elif defined(_MSC_VER)
#define GE_ANONYMOUS_STRUCT_BEGIN __pragma(warning(push)) __pragma(warning(disable : 4201))
#define GE_ANONYMOUS_STRUCT_END __pragma(warning(pop))
#elif defined(__GNUC__)
#define GE_ANONYMOUS_STRUCT_BEGIN _Pragma("GCC diagnostic push") _Pragma("GCC diagnostic ignored \"-Wpedantic\"")
#define GE_ANONYMOUS_STRUCT_END _Pragma("GCC diagnostic pop")
#else
#define GE_ANONYMOUS_STRUCT_BEGIN
#define GE_ANONYMOUS_STRUCT_END
#endif
