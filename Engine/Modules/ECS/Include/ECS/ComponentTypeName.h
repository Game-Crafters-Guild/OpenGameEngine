#pragma once

// Compile-time, cross-compiler-stable component type identity.
//
// Identity is stable across DLL boundaries on Itanium-ABI platforms
// when CXX_VISIBILITY_PRESET=hidden hides type_info RTTI symbols.
// The consteval substring extraction produces a
// normalized type name from the compiler's signature macro; Hash64 turns that
// into a 64-bit identity.
//
// Cross-compiler byte-equality of the normalized name is enforced at compile
// time, on whichever compiler is building: ComponentTypeNameTest.cpp
// static_asserts the extracted spelling of its fixture types against the
// expected literals, so a compiler whose signature format diverges fails the
// build rather than silently registering a different name.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// GCC omits enclosing namespaces from template argument spellings. Keep the
// signature acquisition outside GameEngine so component names stay qualified.
namespace GameEngineComponentTypeNameDetail
{

// ---- Per-compiler signature acquisition ---------------------------------

template <class T>
consteval std::string_view RawSignatureFor()
{
#if defined(__clang__)
    // Clang in any target uses Itanium-style "[T = X]" markers in both
    // __FUNCSIG__ and __PRETTY_FUNCTION__ when defined. Prefer
    // __PRETTY_FUNCTION__ for universal availability.
    return __PRETTY_FUNCTION__;
#elif defined(_MSC_VER)
    return __FUNCSIG__;
#elif defined(__GNUC__)
    return __PRETTY_FUNCTION__;
#else
#  error "Unsupported compiler for consteval ComponentTypeName extraction"
#endif
}

} // namespace GameEngineComponentTypeNameDetail

namespace GameEngine::ECS
{

// ---- Substring extraction -----------------------------------------------

consteval std::string_view RawComponentTypeName(std::string_view signature)
{
    constexpr std::string_view kGccPrefix = "[with T = ";
    constexpr std::string_view kClangPrefix = "[T = ";
    constexpr std::string_view kMsvcPrefix = "RawSignatureFor<";
    std::string_view prefix = kGccPrefix;
    std::string_view suffix = "]";
    auto prefixPosition = signature.find(prefix);
    const bool isGccSignature = prefixPosition != std::string_view::npos;
    if (prefixPosition == std::string_view::npos)
    {
        prefix = kClangPrefix;
        prefixPosition = signature.find(prefix);
    }
    if (prefixPosition == std::string_view::npos)
    {
        prefix = kMsvcPrefix;
        suffix = ">(void)";
        prefixPosition = signature.find(prefix);
    }
    if (prefixPosition == std::string_view::npos) return {};
    const auto start = prefixPosition + prefix.size();
    auto end = signature.rfind(suffix);
    if (end == std::string_view::npos || end < start) return {};
    if (isGccSignature)
    {
        // GCC appends alias substitutions after the template argument.
        const auto aliasStart = signature.find("; ", start);
        if (aliasStart < end) end = aliasStart;
    }
    return signature.substr(start, end - start);
}

// ---- Normalizer ---------------------------------------------------------
//
// MSVC spells every class-type name with its keyword, template arguments
// included, separates template arguments with a bare comma and closes nested
// templates with "> >"; Clang writes none of the keywords, ", " and ">>". The
// normalized spelling is the Clang one:
//   MSVC   "struct ns::Pair<struct ns::A<int> >,enum ns::Mode>"
//   Clang  "ns::Pair<ns::A<int>>, ns::Mode>"   (unchanged by normalization)
// so the name, and the id hashed from it, is the same on MSVC, Clang and GCC for
// class and enum types outside an anonymous namespace, and for templates whose
// arguments are such types. Examples of normalized names that still differ:
//                            MSVC                              Clang                         GCC
//   long                     long                              long                          long int
//   unsigned long            unsigned long                     unsigned long                 long unsigned int
//   long long                __int64                           long long                     long long int
//   unsigned long long       unsigned __int64                  unsigned long long            long long unsigned int
//   arrays                   int[3]                            int[3]                        int [3]
//   pointers                 Foo*                              Foo *                         Foo*
//   const pointers           const struct Foo*                 const Foo *                   const Foo*
//   const types              const struct Foo                  const Foo                     const Foo
//   enum template arguments  ValueTag<1>                       ValueTag<Mode::B>             ValueTag<Mode::B>
//   bool template arguments  ValueTag<1>                       ValueTag<true>                ValueTag<true>
//   anonymous namespaces     `anonymous-namespace'::LocalType  (anonymous namespace)::LocalType  {anonymous}::LocalType
//   function pointers        void(__cdecl *)(int)              void (*)(int)                 void (*)(int)
// GCC closes nested templates with "> >" as MSVC does.
// The rules: trim surrounding whitespace; at the start of the name
// and of every template argument drop one `struct `/`class `/`enum ` and a
// leading `::`; write ", " after every comma; drop the space in "> >".
// Removing interior tokens cannot be done in a view of the compiler's literal,
// so the result is written into per-type storage (ComponentTypeName<T> below).

consteval bool IsTypeNameSpace(char c)
{
    return c == ' ' || c == '\t';
}

consteval std::string_view TrimTypeName(std::string_view s)
{
    while (!s.empty() && IsTypeNameSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && IsTypeNameSpace(s.back())) s.remove_suffix(1);
    return s;
}

// Skips whitespace, one class-key and a leading `::` at a type's start.
consteval std::size_t SkipTypeNamePrefix(std::string_view s, std::size_t i)
{
    constexpr std::string_view kClassKeys[] = {"struct ", "class ", "enum "};
    while (i < s.size() && IsTypeNameSpace(s[i])) ++i;
    for (std::string_view key : kClassKeys)
    {
        if (s.substr(i, key.size()) == key)
        {
            i += key.size();
            break;
        }
    }
    if (s.substr(i, 2) == "::") i += 2;
    while (i < s.size() && IsTypeNameSpace(s[i])) ++i;
    return i;
}

// Writes the normalized spelling of `raw` to `out` when `out` is non-null and
// returns its length, so one call sizes the storage and a second fills it.
consteval std::size_t NormalizeComponentTypeName(std::string_view raw, char* out)
{
    const std::string_view s = TrimTypeName(raw);
    std::size_t length = 0;
    auto emit = [&](char c) {
        if (out) out[length] = c;
        ++length;
    };
    std::size_t i = SkipTypeNamePrefix(s, 0);
    while (i < s.size())
    {
        const char c = s[i];
        if (c == '<')
        {
            emit(c);
            i = SkipTypeNamePrefix(s, i + 1);
        }
        else if (c == ',')
        {
            emit(',');
            emit(' ');
            i = SkipTypeNamePrefix(s, i + 1);
        }
        else if (IsTypeNameSpace(c))
        {
            std::size_t next = i;
            while (next < s.size() && IsTypeNameSpace(s[next])) ++next;
            const bool closesNestedTemplate = next < s.size() && s[next] == '>' && i > 0 && s[i - 1] == '>';
            if (!closesNestedTemplate)
            {
                for (std::size_t k = i; k < next; ++k) emit(s[k]);
            }
            i = next;
        }
        else
        {
            emit(c);
            ++i;
        }
    }
    return length;
}

template <class T>
consteval auto BuildComponentTypeNameStorage()
{
    constexpr std::string_view raw = RawComponentTypeName(GameEngineComponentTypeNameDetail::RawSignatureFor<T>());
    std::array<char, NormalizeComponentTypeName(raw, nullptr) + 1> buffer{};
    NormalizeComponentTypeName(raw, buffer.data());
    return buffer;
}

// NUL-terminated normalized name of T, built once per type at compile time.
template <class T>
inline constexpr auto kComponentTypeNameStorage = BuildComponentTypeNameStorage<T>();

// Public: the normalized, cross-compiler-stable component type name.
template <class T>
consteval std::string_view ComponentTypeName()
{
    return {kComponentTypeNameStorage<T>.data(), kComponentTypeNameStorage<T>.size() - 1};
}

// ComponentTypeName<T>() as a NUL-terminated C string, for the APIs that need one.
template <class T>
inline const char* ComponentTypeNameCStr()
{
    return kComponentTypeNameStorage<T>.data();
}

// ---- 64-bit FNV-1a hash -------------------------------------------------

inline constexpr std::uint64_t kFnv1aOffsetBasis = 0xcbf29ce484222325ULL;
inline constexpr std::uint64_t kFnv1aPrime       = 0x00000100000001b3ULL;

// constexpr (not consteval) so it works in both contexts:
//  - inside consteval ComponentTypeHash<T>() the call is folded at compile time
//  - ComponentRegistry::RegisterBlobComponent calls it with a runtime string_view
// `h` continues a hash, so a name held in pieces hashes as their concatenation.
constexpr std::uint64_t Hash64(std::string_view s, std::uint64_t h = kFnv1aOffsetBasis)
{
    for (char c : s)
    {
        h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        h *= kFnv1aPrime;
    }
    return h;
}

// Public: the 64-bit consteval component type identity; GetComponentTypeId<T>()
// returns it.
template <class T>
consteval std::uint64_t ComponentTypeHash()
{
    return Hash64(ComponentTypeName<T>());
}

} // namespace GameEngine::ECS
