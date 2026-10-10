// Component names and hashes must agree across MSVC, Clang and GCC.
// Static assertions enforce the current compiler's spelling; literals exercise
// every supported signature format regardless of the compiler running the test.

#include "ECS/ComponentTypeName.h"

#include "ECS/Components.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>

using GameEngine::ECS::ComponentTypeName;
using GameEngine::ECS::ComponentTypeNameCStr;
using GameEngine::ECS::ComponentTypeHash;
using GameEngine::ECS::Hash64;
using GameEngine::ECS::NormalizeComponentTypeName;
using GameEngine::ECS::RawComponentTypeName;

// ---- Fixture types ----
//
// Plain types in the test TU. Realistic stand-ins for engine components:
// POD struct in global scope, POD struct in a nested namespace, enum.

struct ProtoFooComponent
{
    float x;
    float y;
};

namespace ge_test_ns
{
struct ProtoBarComponent
{
    int value;
};
}

enum class ProtoColor : int { Red, Green, Blue };

template <class First, class Second>
struct ProtoPair
{
};

// ---- Compile-time invariants ----
//
// A compiler spelling change must fail compilation before names reach a registry.

static_assert(!ComponentTypeName<int>().empty(),                  "ComponentTypeName<int> empty");
static_assert(!ComponentTypeName<ProtoFooComponent>().empty(),    "ComponentTypeName<ProtoFooComponent> empty");
static_assert(!ComponentTypeName<ge_test_ns::ProtoBarComponent>().empty(),
              "ComponentTypeName<ge_test_ns::ProtoBarComponent> empty");
static_assert(!ComponentTypeName<ProtoColor>().empty(),           "ComponentTypeName<ProtoColor> empty");

// The spellings themselves, byte for byte. Non-emptiness and distinctness hold
// on any extraction that works at all; these are what actually pin the
// cross-compiler claim, because a name is what the component registries key on
// and what the Inspector renders. A compiler whose signature format changes
// (or whose prefix/suffix markers stop matching) fails HERE, at compile time,
// on that compiler — which is the only place the divergence can be caught
// without a machine of every kind.
static_assert(ComponentTypeName<ProtoFooComponent>() == "ProtoFooComponent",
              "ProtoFooComponent name is not the normalized spelling");
static_assert(ComponentTypeName<ge_test_ns::ProtoBarComponent>() == "ge_test_ns::ProtoBarComponent",
              "namespaced name is not the normalized spelling");
static_assert(ComponentTypeName<ProtoColor>() == "ProtoColor",
              "enum name is not the normalized spelling");
static_assert(ComponentTypeName<int>() == "int", "int name is not the normalized spelling");

// Different types must produce different names (and therefore different hashes).
static_assert(ComponentTypeName<int>() != ComponentTypeName<float>(),
              "int and float collide post-normalization");
static_assert(ComponentTypeName<ProtoFooComponent>() != ComponentTypeName<ge_test_ns::ProtoBarComponent>(),
              "ProtoFooComponent and ProtoBarComponent collide post-normalization");

// ComponentTypeHash is consteval and stable.
static_assert(ComponentTypeHash<int>() != 0,                "Hash64(\"int\") must be non-zero");
static_assert(ComponentTypeHash<int>() != ComponentTypeHash<float>(),
              "Hashes of int and float must differ");
static_assert(ComponentTypeHash<ProtoFooComponent>() != ComponentTypeHash<ge_test_ns::ProtoBarComponent>(),
              "Hashes of distinct component types must differ");

// Normalization rules, on the literal spellings each compiler produces.
consteval bool NormalizesTo(std::string_view raw, std::string_view expected)
{
    std::array<char, 256> buffer{};
    const std::size_t length = NormalizeComponentTypeName(raw, buffer.data());
    return std::string_view(buffer.data(), length) == expected;
}

static_assert(NormalizesTo("struct Foo", "Foo"),          "strip 'struct '");
static_assert(NormalizesTo("class Foo", "Foo"),           "strip 'class '");
static_assert(NormalizesTo("enum Foo", "Foo"),            "strip 'enum '");
static_assert(NormalizesTo("::Foo", "Foo"),               "strip leading ::");
static_assert(NormalizesTo("  Foo  ", "Foo"),             "trim whitespace");
static_assert(NormalizesTo("struct ns::Foo", "ns::Foo"),  "namespaced post-strip");

// MSVC keeps the class-key on every template argument, separates arguments with
// a bare comma and closes nested templates with "> >"; Clang does none of that,
// and GCC writes only the "> >". Every spelling must normalize to the Clang one.
static_assert(NormalizesTo("A<struct B>", "A<B>"),        "strip a template argument's class-key");
static_assert(NormalizesTo("A<B>", "A<B>"),               "Clang template spelling is unchanged");
static_assert(NormalizesTo("struct ns::Pair<class ns::A,enum ns::Mode>", "ns::Pair<ns::A, ns::Mode>"),
              "MSVC template argument list");
static_assert(NormalizesTo("ns::Pair<ns::A, ns::Mode>", "ns::Pair<ns::A, ns::Mode>"),
              "Clang template argument list is unchanged");
static_assert(NormalizesTo("struct A<struct B<struct C,int> >", "A<B<C, int>>"), "MSVC nested template");
static_assert(NormalizesTo("A<B<C, int>>", "A<B<C, int>>"),                     "Clang nested template");
static_assert(NormalizesTo("A<B<C, int> >", "A<B<C, int>>"),                    "GCC nested template");
static_assert(NormalizesTo("A<struct B,int>", "A<B, int>"), "a keyword-free argument after a comma");
static_assert(NormalizesTo("unsigned int", "unsigned int"), "interior spaces of a type name are kept");

static_assert(RawComponentTypeName(
    "consteval std::string_view GameEngineComponentTypeNameDetail::RawSignatureFor() "
    "[with T = int; std::string_view = std::basic_string_view<char>]") == "int");
static_assert(RawComponentTypeName(
    "consteval std::string_view GameEngineComponentTypeNameDetail::RawSignatureFor() "
    "[with T = GameEngine::ECS::Disabled; std::string_view = std::basic_string_view<char>]") ==
    "GameEngine::ECS::Disabled");
static_assert(NormalizesTo(RawComponentTypeName(
    "consteval std::string_view GameEngineComponentTypeNameDetail::RawSignatureFor() "
    "[with T = GameEngine::ECS::ComponentDisabled<ProtoPair<ProtoFooComponent, int> >; "
    "std::string_view = std::basic_string_view<char> ]"),
    "GameEngine::ECS::ComponentDisabled<ProtoPair<ProtoFooComponent, int>>"));
static_assert(RawComponentTypeName(
    "consteval std::string_view RawSignatureFor() "
    "[with T = CharacterTag<';'>; std::string_view = std::basic_string_view<char>]") ==
    "CharacterTag<';'>");
static_assert(RawComponentTypeName(
    "constexpr auto RawSignatureFor() [with T = ValueTag<';'>]") == "ValueTag<';'>");
static_assert(RawComponentTypeName("constexpr auto RawSignatureFor() [with T = int]") == "int");
static_assert(RawComponentTypeName("std::string_view RawSignatureFor() [T = int]") == "int");
static_assert(NormalizesTo(RawComponentTypeName(
    "class std::basic_string_view<char> __cdecl RawSignatureFor<struct ns::Pair<struct ns::Foo,int> >(void)"),
    "ns::Pair<ns::Foo, int>"));
static_assert(RawComponentTypeName("unrecognized signature").empty());
static_assert(RawComponentTypeName("RawSignatureFor() [with T = int").empty());

// Types from the extraction function's namespace must retain their qualification.
static_assert(ComponentTypeName<GameEngine::ECS::Disabled>() == "GameEngine::ECS::Disabled");
static_assert(ComponentTypeName<GameEngine::ECS::ComponentDisabled<GameEngine::ECS::Disabled>>() ==
    "GameEngine::ECS::ComponentDisabled<GameEngine::ECS::Disabled>");
static_assert(ComponentTypeHash<GameEngine::ECS::ComponentDisabled<GameEngine::ECS::Disabled>>() ==
    Hash64("GameEngine::ECS::ComponentDisabled<GameEngine::ECS::Disabled>"));

// Template components must have the same name and id on every compiler.
static_assert(ComponentTypeName<GameEngine::ECS::ComponentDisabled<ProtoFooComponent>>() ==
                  "GameEngine::ECS::ComponentDisabled<ProtoFooComponent>",
              "template component name is not the normalized spelling");
static_assert(ComponentTypeName<ProtoPair<ProtoFooComponent, ge_test_ns::ProtoBarComponent>>() ==
                  "ProtoPair<ProtoFooComponent, ge_test_ns::ProtoBarComponent>",
              "two-argument template component name is not the normalized spelling");
static_assert(ComponentTypeName<ProtoPair<ProtoPair<ProtoFooComponent, int>, ProtoColor>>() ==
                  "ProtoPair<ProtoPair<ProtoFooComponent, int>, ProtoColor>",
              "nested template component name is not the normalized spelling");
static_assert(ComponentTypeHash<GameEngine::ECS::ComponentDisabled<ProtoFooComponent>>() ==
                  Hash64("GameEngine::ECS::ComponentDisabled<ProtoFooComponent>"),
              "a template component's id is not the hash of its normalized name");

// ---- Runtime tests (for visibility in test logs) ----

TEST(ComponentTypeNameTest, NamesAreNonEmpty)
{
    EXPECT_FALSE(ComponentTypeName<int>().empty());
    EXPECT_FALSE(ComponentTypeName<ProtoFooComponent>().empty());
    EXPECT_FALSE(ComponentTypeName<ge_test_ns::ProtoBarComponent>().empty());
}

TEST(ComponentTypeNameTest, NormalizationStripsCompilerArtifacts)
{
    // After normalization, the extracted name must NOT contain "struct ", "class ", or " enum ".
    const auto fooName = ComponentTypeName<ProtoFooComponent>();
    EXPECT_EQ(fooName.find("struct "), std::string_view::npos)
        << "Normalized name still contains 'struct ': '" << fooName << "'";
    EXPECT_EQ(fooName.find("class "),  std::string_view::npos);

    // The unqualified type name must be present.
    EXPECT_NE(fooName.find("ProtoFooComponent"), std::string_view::npos)
        << "Expected 'ProtoFooComponent' in '" << fooName << "'";
}

TEST(ComponentTypeNameTest, CStrIsTheNameTerminated)
{
    using DisabledFoo = GameEngine::ECS::ComponentDisabled<ProtoFooComponent>;
    EXPECT_EQ(std::string_view(ComponentTypeNameCStr<DisabledFoo>()), ComponentTypeName<DisabledFoo>());
    EXPECT_EQ(std::strlen(ComponentTypeNameCStr<ProtoFooComponent>()), ComponentTypeName<ProtoFooComponent>().size());
}

TEST(ComponentTypeNameTest, HashesAreNonZeroAndDistinct)
{
    EXPECT_NE(ComponentTypeHash<int>(),                0u);
    EXPECT_NE(ComponentTypeHash<ProtoFooComponent>(),  0u);
    EXPECT_NE(ComponentTypeHash<int>(),                ComponentTypeHash<float>());
    EXPECT_NE(ComponentTypeHash<ProtoFooComponent>(),  ComponentTypeHash<ge_test_ns::ProtoBarComponent>());
}

TEST(ComponentTypeNameTest, HashIsDeterministicAcrossInvocations)
{
    // Same compile-time hash both times — this is a sanity check that the
    // consteval evaluation isn't doing anything weird like time-based seeding.
    constexpr auto h1 = ComponentTypeHash<ProtoFooComponent>();
    constexpr auto h2 = ComponentTypeHash<ProtoFooComponent>();
    EXPECT_EQ(h1, h2);
}

TEST(ComponentTypeNameTest, NamesUseNamespaceQualification)
{
    // Namespaced types should carry their namespace in the normalized name so
    // identical class names in different namespaces don't collide.
    const auto barName = ComponentTypeName<ge_test_ns::ProtoBarComponent>();
    EXPECT_NE(barName.find("ge_test_ns"), std::string_view::npos)
        << "Expected 'ge_test_ns' in '" << barName << "'";
    EXPECT_NE(barName.find("ProtoBarComponent"), std::string_view::npos);
}
