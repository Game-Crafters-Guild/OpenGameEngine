// The scene decimal-integer token contract, at both layers that read one.
//
// A scene integer is exact or it is an error. Every reflected integer field parses in its own
// declared type, so an unsigned 64-bit field keeps its top bit — a signed intermediate cannot
// represent it, and saturating to INT64_MAX would store a plausible wrong number for a value the
// writer had emitted correctly. The generic SceneValue integer is held to the same rule, so a
// hand-written schema reading through it cannot be handed a saturated or prefix-parsed value.
//
// The grammar: surrounding scene whitespace, one optional leading '+' (or '-' when the
// destination is signed), then decimal digits, to the end of the token. Leading zeroes are fine.
// Hexadecimal spellings, exponents, digit separators, embedded NULs and trailing text are not,
// and neither is a value the destination type cannot hold.
//
// A refused assignment leaves the field at its default and keeps its authored text through
// SceneIO's degradation path, so the next save writes the original line back unchanged.

#include <gtest/gtest.h>

#include "ECS/ComponentFactory.h"
#include "ECS/ComponentFieldRegistry.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/Reflection.h"
#include "Scene/FieldSerializerRegistry.h"
#include "Scene/ReflectionSceneSchema.h"
#include "Scene/SceneIO.h"
#include "Scene/SceneSchemaRegistry.h"
#include "Scene/SceneValue.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>

using namespace GameEngine;

struct SceneIntegerProbe
{
    std::uint64_t Unsigned = 7;
    std::int64_t Signed = -7;
    std::uint32_t U32 = 11;
    std::int32_t I32 = -11;
};
GE_REFLECT(SceneIntegerProbe, Unsigned, Signed, U32, I32);

namespace
{
template <class T>
class SceneIntegerCodec : public testing::Test
{
};
using IntegerTypes = testing::Types<std::int8_t, std::int16_t, std::int32_t, std::int64_t,
                                    std::uint8_t, std::uint16_t, std::uint32_t, std::uint64_t>;
TYPED_TEST_SUITE(SceneIntegerCodec, IntegerTypes);

template <class T>
const Scene::IFieldSerializer* Codec()
{
    ECS::FieldInfo field{};
    field.Type = ECS::FieldTypeIdOf<T>();
    field.Size = sizeof(T);
    return Scene::FieldSerializerRegistry::Resolve(field);
}

template <class T>
bool ReadInteger(std::string_view text, T& value, std::string& error)
{
    ECS::FieldInfo field{};
    field.Type = ECS::FieldTypeIdOf<T>();
    field.Size = sizeof(T);
    return Codec<T>()->Read(text, reinterpret_cast<std::byte*>(&value), field, {}, &error);
}

TYPED_TEST(SceneIntegerCodec, ReadsExactBoundsAndDecimalSpellings)
{
    using T = TypeParam;
    for (T expected : {T{0}, T{1}, std::numeric_limits<T>::min(), std::numeric_limits<T>::max()})
    {
        T actual = T{7};
        std::string error;
        const std::string text = std::to_string(expected);
        ASSERT_TRUE(ReadInteger(text, actual, error)) << text << ": " << error;
        EXPECT_EQ(actual, expected);
    }
    for (const auto text : {"+42", "00042", " \t42\r\n", " \t+00042 ", "\r\n42"})
    {
        T actual = T{7};
        std::string error;
        ASSERT_TRUE(ReadInteger(text, actual, error)) << text << ": " << error;
        EXPECT_EQ(actual, T{42});
    }
    if constexpr (std::is_signed_v<T>)
    {
        T actual = T{7};
        std::string error;
        ASSERT_TRUE(ReadInteger("-00042", actual, error));
        EXPECT_EQ(actual, T{-42});
    }
}

TYPED_TEST(SceneIntegerCodec, RejectsOverflowWithoutAssigning)
{
    using T = TypeParam;
    const std::string positive = []
    {
        if constexpr (sizeof(T) == 8 && !std::is_signed_v<T>)
            return std::string("18446744073709551616");
        else
            return std::to_string(static_cast<std::uint64_t>(std::numeric_limits<T>::max()) + 1);
    }();
    const std::string negative = []
    {
        if constexpr (!std::is_signed_v<T>)
            return std::string("-1");
        else if constexpr (sizeof(T) == 8)
            return std::string("-9223372036854775809");
        else
            return std::to_string(static_cast<std::int64_t>(std::numeric_limits<T>::min()) - 1);
    }();
    for (const auto& text : {positive, negative, std::string(128, '9')})
    {
        T actual = T{7};
        std::string error;
        EXPECT_FALSE(ReadInteger(text, actual, error)) << text;
        EXPECT_EQ(actual, T{7}) << text;
        EXPECT_FALSE(error.empty());
    }
}

TYPED_TEST(SceneIntegerCodec, RejectsNonIntegerAndPartialTokensWithoutAssigning)
{
    using T = TypeParam;
    for (const auto text : {"", " ", "+", "-", "+-1", "++1", "+ 42", "- 42", "1.0", "1e2",
                            "0x10", "0XFF", "42tail", "42 1", "true", "\"42\"", "(42)", "1_000",
                            "\f42", "\v42", "4\xc2\xb2"})
    {
        T actual = T{7};
        std::string error;
        EXPECT_FALSE(ReadInteger(text, actual, error)) << text;
        EXPECT_EQ(actual, T{7}) << text;
        EXPECT_FALSE(error.empty()) << text;
    }
    T actual = T{7};
    std::string error;
    EXPECT_FALSE(ReadInteger(std::string_view("42\0tail", 7), actual, error));
    EXPECT_EQ(actual, T{7});
    if constexpr (!std::is_signed_v<T>)
    {
        EXPECT_FALSE(ReadInteger("-0", actual, error));
        EXPECT_EQ(actual, T{7});
    }
}

// The generic SceneValue integer, which every hand-written schema reads through.
TEST(SceneValueInteger, ParsesCompleteTokensAndRefusesTheRest)
{
    for (const auto text : {"42", "+42", "00042", " \t42\r\n", "-42"})
    {
        Scene::SceneValue value;
        ASSERT_TRUE(Scene::ParseValue(text, value, nullptr)) << text;
        EXPECT_EQ(value.Kind, Scene::SceneValueKind::Int) << text;
    }
    // An out-of-range or partial token is not an integer. It reaches the schema as something
    // else, which the schema reports — never as a saturated or prefix-parsed number.
    for (const auto text : {"9223372036854775808", "-9223372036854775809", "18446744073709551615",
                            "0x10", "42tail"})
    {
        Scene::SceneValue value;
        Scene::ParseValue(text, value, nullptr);
        EXPECT_NE(value.Kind, Scene::SceneValueKind::Int) << text;
    }
}

// A 32-hex-digit GUID whose characters happen to all be decimal digits must stay a GUID token.
TEST(SceneValueInteger, AnAllDigitGuidIsNotAnInteger)
{
    Scene::SceneValue value;
    ASSERT_TRUE(Scene::ParseValue("12345678901234567890123456789012", value, nullptr));
    EXPECT_NE(value.Kind, Scene::SceneValueKind::Int);
    EXPECT_EQ(value.StringValue, "12345678901234567890123456789012");
}

// A named-enum field also accepts a bare integer, for scenes written before the enumerator names
// were serialized. That integer must fit the field's bytes.
enum class SceneIntegerProbeMode : std::uint8_t
{
    Off = 0,
    On = 1
};
constexpr ECS::EnumNameValue kProbeModeNames[] = {{"Off", 0}, {"On", 1}};

ECS::FieldInfo ProbeModeField()
{
    ECS::FieldInfo field{};
    field.Type = ECS::FieldTypeIdOf<std::underlying_type_t<SceneIntegerProbeMode>>();
    field.Size = sizeof(SceneIntegerProbeMode);
    field.EnumNames = kProbeModeNames;
    return field;
}

TEST(SceneEnumLegacyInteger, AcceptsAValueThatFitsAndRefusesOneThatDoesNot)
{
    const ECS::FieldInfo field = ProbeModeField();
    const Scene::IFieldSerializer* codec = Scene::FieldSerializerRegistry::Resolve(field);
    ASSERT_NE(codec, nullptr);

    auto read = [&](std::string_view text, SceneIntegerProbeMode& value, std::string& error)
    { return codec->Read(text, reinterpret_cast<std::byte*>(&value), field, {}, &error); };

    SceneIntegerProbeMode value = SceneIntegerProbeMode::Off;
    std::string error;
    ASSERT_TRUE(read("On", value, error)) << error;
    EXPECT_EQ(value, SceneIntegerProbeMode::On);
    ASSERT_TRUE(read("0", value, error)) << error;
    EXPECT_EQ(value, SceneIntegerProbeMode::Off);
    // In the table by value but not by name: still a legal legacy integer.
    ASSERT_TRUE(read("1", value, error)) << error;
    EXPECT_EQ(value, SceneIntegerProbeMode::On);

    for (const auto text : {"256", "-1", "18446744073709551615"})
    {
        value = SceneIntegerProbeMode::On;
        error.clear();
        EXPECT_FALSE(read(text, value, error)) << text;
        EXPECT_EQ(value, SceneIntegerProbeMode::On) << text;
        EXPECT_FALSE(error.empty()) << text;
    }
}

// The registries are process-wide, so the probe joins them once. Registering per test would log
// an override warning for every case after the first.
void EnsureProbeRegistered()
{
    static const bool registered = []
    {
        const auto type = ECS::GetComponentTypeId<SceneIntegerProbe>();
        ECS::ComponentFieldRegistry::Register(type, ECS::Reflection<SceneIntegerProbe>::Fields,
                                              ECS::ComponentTypeName<SceneIntegerProbe>());
        const SceneIntegerProbe defaults{};
        ECS::ComponentFactory::RegisterDefaultBytes(type, &defaults, sizeof(defaults), true);
        // Use the actual reflection schema through SceneIO's ordinary extensible registry.
        Scene::SceneSchemaRegistry::Register(
            std::make_unique<Scene::ReflectionSceneSchema>(type, "SceneIntegerProbe"));
        return true;
    }();
    ASSERT_TRUE(registered);
}

class SceneIntegerIO : public testing::Test
{
  protected:
    std::filesystem::path directory;

    void SetUp() override
    {
        EnsureProbeRegistered();
        directory = std::filesystem::temp_directory_path() /
                    ("SceneIntegerTests_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory);
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    static std::string ReadText(const std::filesystem::path& path)
    {
        std::ifstream file(path);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    static SceneIntegerProbe OnlyProbe(ECS::World& world)
    {
        SceneIntegerProbe result{};
        int count = 0;
        world.Query<ECS::Read<SceneIntegerProbe>>().Each(
            [&](const SceneIntegerProbe& value)
            { result = value; ++count; });
        EXPECT_EQ(count, 1);
        return result;
    }
};

class SceneUnsigned64RoundTrip : public SceneIntegerIO, public testing::WithParamInterface<std::uint64_t>
{
};

TEST_P(SceneUnsigned64RoundTrip, PreservesAllBitsThroughSaveLoadAndResave)
{
    const auto path = directory / "integers.scene";
    ECS::World source;
    SceneIntegerProbe expected;
    expected.Unsigned = GetParam();
    expected.Signed = std::numeric_limits<std::int64_t>::min();
    expected.U32 = std::numeric_limits<std::uint32_t>::max();
    expected.I32 = std::numeric_limits<std::int32_t>::min();
    source.Create(expected);
    ASSERT_TRUE(Scene::SaveSceneToFile(source, path));
    const std::string line = "SceneIntegerProbe.Unsigned = " + std::to_string(expected.Unsigned);
    EXPECT_NE(ReadText(path).find(line), std::string::npos);

    for (int pass = 0; pass < 2; ++pass)
    {
        ECS::World loaded;
        Scene::SceneLoadDegradation degradation;
        Scene::LoadOptions options;
        options.outDegradation = &degradation;
        ASSERT_TRUE(Scene::LoadSceneFromFile(loaded, path, options)) << Scene::GetLastSceneIOError().message;
        EXPECT_TRUE(degradation.skips.empty());
        const auto actual = OnlyProbe(loaded);
        EXPECT_EQ(actual.Unsigned, expected.Unsigned);
        EXPECT_EQ(actual.Signed, expected.Signed);
        EXPECT_EQ(actual.U32, expected.U32);
        EXPECT_EQ(actual.I32, expected.I32);
        ASSERT_TRUE(Scene::SaveSceneToFile(loaded, path));
        EXPECT_NE(ReadText(path).find(line), std::string::npos);
    }
}

INSTANTIATE_TEST_SUITE_P(DecimalDomain, SceneUnsigned64RoundTrip,
                         testing::Values(std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{0x7fffffffffffffff},
                                         std::uint64_t{0x8000000000000000}, std::uint64_t{0x8000000000000001},
                                         std::numeric_limits<std::uint64_t>::max()));

class SceneSigned64RoundTrip : public SceneIntegerIO, public testing::WithParamInterface<std::int64_t>
{
};

TEST_P(SceneSigned64RoundTrip, PreservesSignedValueOnAdditiveLoad)
{
    const auto path = directory / "signed.scene";
    ECS::World source;
    SceneIntegerProbe expected;
    expected.Signed = GetParam();
    source.Create(expected);
    ASSERT_TRUE(Scene::SaveSceneToFile(source, path));

    ECS::World loaded;
    SceneIntegerProbe prior;
    prior.Unsigned = 123;
    const auto existing = loaded.Create(prior).GetHandle();
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions options;
    options.mode = Scene::LoadMode::Additive;
    options.outDegradation = &degradation;
    ASSERT_TRUE(Scene::LoadSceneFromFile(loaded, path, options));
    EXPECT_TRUE(degradation.skips.empty());
    ASSERT_NE(loaded.GetComponent<SceneIntegerProbe>(existing), nullptr);
    EXPECT_EQ(loaded.GetComponent<SceneIntegerProbe>(existing)->Unsigned, 123u);
    int added = 0;
    loaded.Query<ECS::Read<SceneIntegerProbe>>().Each([&](const SceneIntegerProbe& value)
                                                      {
        if (value.Unsigned == expected.Unsigned)
        {
            ++added;
            EXPECT_EQ(value.Signed, expected.Signed);
        } });
    EXPECT_EQ(added, 1);
}

INSTANTIATE_TEST_SUITE_P(SignedDecimalDomain, SceneSigned64RoundTrip,
                         testing::Values(std::numeric_limits<std::int64_t>::min(), std::int64_t{-1},
                                         std::int64_t{0}, std::numeric_limits<std::int64_t>::max()));

struct RejectedInteger
{
    const char* field;
    const char* text;
};
void PrintTo(const RejectedInteger& value, std::ostream* out)
{
    *out << value.field << " = " << value.text;
}
class SceneIntegerDegradation : public SceneIntegerIO, public testing::WithParamInterface<RejectedInteger>
{
};

TEST_P(SceneIntegerDegradation, KeepsDefaultAndPreservesRejectedTextOnSave)
{
    const auto [field, text] = GetParam();
    const auto path = directory / "rejected.scene";
    const std::string assignment = std::string("SceneIntegerProbe.") + field + " = " + text;
    {
        std::ofstream file(path);
        file << "[scene name=\"Integers\" version=1]\n[entity id=\"probe\"]\n"
             << assignment << "\nSceneIntegerProbe.I32 = -42\n";
        ASSERT_TRUE(file.good());
    }
    ECS::World loaded;
    Scene::SceneLoadDegradation degradation;
    Scene::LoadOptions options;
    options.outDegradation = &degradation;
    ASSERT_TRUE(Scene::LoadSceneFromFile(loaded, path, options)) << Scene::GetLastSceneIOError().message;
    ASSERT_EQ(degradation.skips.size(), 1u);
    EXPECT_EQ(degradation.skips[0].field, field);
    EXPECT_TRUE(degradation.skips[0].preserved);
    const auto actual = OnlyProbe(loaded);
    EXPECT_EQ(actual.Unsigned, 7u);
    EXPECT_EQ(actual.Signed, -7);
    EXPECT_EQ(actual.U32, 11u);
    EXPECT_EQ(actual.I32, -42);
    ASSERT_TRUE(Scene::SaveSceneToFile(loaded, path));
    EXPECT_NE(ReadText(path).find(assignment), std::string::npos);
    ECS::World reloaded;
    degradation.skips.clear();
    ASSERT_TRUE(Scene::LoadSceneFromFile(reloaded, path, options));
    ASSERT_EQ(degradation.skips.size(), 1u);
    EXPECT_TRUE(degradation.skips[0].preserved);
    const auto again = OnlyProbe(reloaded);
    EXPECT_EQ(again.Unsigned, 7u);
    EXPECT_EQ(again.Signed, -7);
    EXPECT_EQ(again.U32, 11u);
    EXPECT_EQ(again.I32, -42);
}

INSTANTIATE_TEST_SUITE_P(InvalidDecimal, SceneIntegerDegradation,
                         testing::Values(RejectedInteger{"Unsigned", "18446744073709551616"},
                                         RejectedInteger{"Unsigned", "-1"},
                                         RejectedInteger{"Signed", "9223372036854775808"},
                                         RejectedInteger{"Signed", "-9223372036854775809"},
                                         RejectedInteger{"U32", "4294967296"},
                                         RejectedInteger{"Unsigned", "42tail"},
                                         RejectedInteger{"Unsigned", "0x10"},
                                         RejectedInteger{"Unsigned", "1e2"},
                                         RejectedInteger{"Signed", "1.0"}));
} // namespace
