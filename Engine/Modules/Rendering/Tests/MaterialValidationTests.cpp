// ValidateMaterialAgainstMeta: which authored keys count as known, which warn,
// and what the warning says. The laneless-declaration rows are the load-bearing
// cases: a laneless declaration is an adapter read no surface stores — the
// composer folded it to its default — so an authored key for it is dead and
// must warn with the declare-it fix-it, while the transitional StandardPBR
// fill's seeded keys stay silent (the fill puts them in every document).

#include <gtest/gtest.h>

#include "Rendering/Materials/MaterialValidation.h"

#include <string>

using namespace GameEngine;
using namespace GameEngine::Rendering;

namespace
{

ShaderProperty Declared(const char* name, ShaderPropertyType type, bool hasLane)
{
    ShaderProperty p{};
    p.Name = name;
    p.Type = type;
    p.HasLane = hasLane;
    return p;
}

std::string Messages(const ValidationReport& r)
{
    std::string out;
    for (const auto& i : r.Issues)
        out += i.Code + ": " + i.Message + "\n";
    return out;
}

} // namespace

TEST(MaterialValidation, DeclaredKeysAreKnownAndProduceNoIssues)
{
    ShaderMeta meta{};
    meta.DeclaredProperties.push_back(Declared("tint", ShaderPropertyType::Color, true));
    meta.DeclaredProperties.push_back(Declared("pulseSpeed", ShaderPropertyType::Float, true));

    MaterialValidationInput in{};
    in.propertyNames = {"tint", "pulseSpeed"};

    const auto report = ValidateMaterialAgainstMeta(meta, in);
    EXPECT_TRUE(report.Issues.empty()) << Messages(report);
}

TEST(MaterialValidation, AuthoredKeyForLanelessDeclarationWarnsWithDeclareFixIt)
{
    // alphaCutoff is an adapter read; on a surface that does not declare it the
    // authored value compiles away to the constant default. Silence here is the
    // reviewed bug: the key must warn, and the warning must say how to fix it.
    ShaderMeta meta{};
    meta.DeclaredProperties.push_back(Declared("tint", ShaderPropertyType::Color, true));
    meta.DeclaredProperties.push_back(Declared("alphaCutoff", ShaderPropertyType::Float, false));

    MaterialValidationInput in{};
    in.propertyNames = {"tint", "alphaCutoff"};

    const auto report = ValidateMaterialAgainstMeta(meta, in);
    ASSERT_EQ(report.Issues.size(), 1u) << Messages(report);
    EXPECT_EQ(report.Issues[0].Code, "UnknownMaterialProperty");
    EXPECT_EQ(report.Issues[0].Severity, IssueSeverity::Warning);
    EXPECT_NE(report.Issues[0].Message.find("// @property float alphaCutoff"), std::string::npos)
        << report.Issues[0].Message;
}

TEST(MaterialValidation, SeededKeysMatchingLanelessDeclarationsStaySilent)
{
    // The StandardPBR parse-time fill seeds these keys into every document;
    // warning on them would flag every material with a declared surface.
    ShaderMeta meta{};
    meta.DeclaredProperties.push_back(Declared("tint", ShaderPropertyType::Color, true));
    meta.DeclaredProperties.push_back(Declared("specularIor", ShaderPropertyType::Float, false));
    meta.DeclaredProperties.push_back(Declared("transmissionColor", ShaderPropertyType::Color, false));
    meta.DeclaredProperties.push_back(Declared("transmissionWeight", ShaderPropertyType::Float, false));

    MaterialValidationInput in{};
    in.propertyNames = {"specularIor", "transmissionColor", "transmissionWeight"};

    const auto report = ValidateMaterialAgainstMeta(meta, in);
    EXPECT_TRUE(report.Issues.empty()) << Messages(report);
}

TEST(MaterialValidation, UnknownKeySuggestsNearestDeclaredNameNotALanelessOne)
{
    ShaderMeta meta{};
    meta.DeclaredProperties.push_back(Declared("pulseSpeed", ShaderPropertyType::Float, true));
    meta.DeclaredProperties.push_back(Declared("specularIor", ShaderPropertyType::Float, false));

    MaterialValidationInput in{};
    in.propertyNames = {"pulseSpede", "specularIol"};

    const auto report = ValidateMaterialAgainstMeta(meta, in);
    ASSERT_EQ(report.Issues.size(), 2u) << Messages(report);
    const std::string all = Messages(report);
    EXPECT_NE(all.find("did you mean 'pulseSpeed'"), std::string::npos) << all;
    EXPECT_EQ(all.find("did you mean 'specularIor'"), std::string::npos)
        << "a laneless name is not authorable, so it is no typo suggestion\n" << all;
}
