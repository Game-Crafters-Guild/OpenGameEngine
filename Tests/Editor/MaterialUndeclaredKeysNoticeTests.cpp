// The material inspector's undeclared-keys notice line model
// (Editor::UndeclaredKeyLines): an unknown key gets the nearest declared name;
// a key matching a laneless declaration — an adapter read no surface stores,
// folded to its default by the composer — gets the declare-it fix-it; the keys
// the transitional StandardPBR parse-time fill seeds into every document stay
// exempt. Leaf TU over the notice and the shader property table.

#include <gtest/gtest.h>

#include "Inspectors/UndeclaredKeysNotice.h"
#include "Rendering/Materials/ShaderPropertyTable.h"

#include <string>
#include <vector>

using namespace GameEngine;

namespace
{

// An adapter read (alphaCutoff, laneless without a surface declaration) plus a
// surface-declared property (tint, packed to a lane).
Rendering::ShaderPropertyTable DeclaredTable()
{
    return Rendering::BuildShaderPropertyTable(
        {{"// @property float alphaCutoff default=0.5 range=0,1\n", "adapter_forward.glsl",
          Rendering::ShaderPropertyOrigin::Adapter},
         {"// @property color tint default=1,1,1\n", "surface.glsl",
          Rendering::ShaderPropertyOrigin::Surface}});
}

std::string Joined(const std::vector<std::string>& lines)
{
    std::string out;
    for (const std::string& line : lines)
        out += line + "\n";
    return out;
}

} // namespace

TEST(MaterialUndeclaredKeysNotice, LanelessKeyGetsTheDeclareFixIt)
{
    const auto table = DeclaredTable();
    ASSERT_TRUE(table.Find("alphaCutoff"));
    ASSERT_FALSE(table.Find("alphaCutoff")->HasLane) << "premise: adapter-only reads pack no lane";

    const auto lines = Editor::UndeclaredKeyLines({"alphaCutoff"}, {"tint"}, &table);
    ASSERT_EQ(lines.size(), 1u) << Joined(lines);
    EXPECT_NE(lines[0].find("// @property float alphaCutoff"), std::string::npos) << lines[0];
}

TEST(MaterialUndeclaredKeysNotice, UnknownKeyGetsTheNearestDeclaredName)
{
    const auto table = DeclaredTable();
    const auto lines = Editor::UndeclaredKeyLines({"tnit"}, {"tint"}, &table);
    ASSERT_EQ(lines.size(), 1u) << Joined(lines);
    EXPECT_NE(lines[0].find("did you mean 'tint'"), std::string::npos) << lines[0];
}

TEST(MaterialUndeclaredKeysNotice, SeededFillKeysStayExempt)
{
    // The parse-time fill seeds these into every StandardPBR document; the
    // notice reporting them would flag every material with a declared surface.
    const auto table = DeclaredTable();
    const auto lines =
        Editor::UndeclaredKeyLines({"specularIor", "transmissionColor", "metallic", "opacity"}, {"tint"}, &table);
    EXPECT_TRUE(lines.empty()) << Joined(lines);
}

TEST(MaterialUndeclaredKeysNotice, NoTableMeansNearestNameLinesOnly)
{
    // The legacy project-surface path has no declared-property table.
    const auto lines = Editor::UndeclaredKeyLines({"customGlow"}, {}, nullptr);
    ASSERT_EQ(lines.size(), 1u) << Joined(lines);
    EXPECT_EQ(lines[0], "customGlow");
}

// A surface that declares nothing still reads the four generic lanes as
// Mat.uUser*, and MaterialRegistry wires user0..15 / userVec0..3 to them — the
// graph editor's live preview and the converted-surface corpus both author those
// keys. Calling them "never reaches the shader" is wrong, and it is the whole
// notice a preview material would show.
TEST(MaterialUndeclaredKeysNotice, GenericLaneKeysAreNotUndeclaredOnAnUndeclaredSurface)
{
    const std::vector<std::string> keys{"user0", "user15", "userVec3"};
    EXPECT_TRUE(Editor::UndeclaredKeyLines(keys, {}, nullptr).empty())
        << Joined(Editor::UndeclaredKeyLines(keys, {}, nullptr));
}

// Only the generic lanes themselves. A name that merely starts with "user" is a
// key nothing wires, and the reader still needs to be told.
TEST(MaterialUndeclaredKeysNotice, ANameThatOnlyLooksGenericIsStillReported)
{
    const std::vector<std::string> lines = Editor::UndeclaredKeyLines({"userScale"}, {}, nullptr);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines.front().find("userScale"), std::string::npos);
}

// A declared surface replaces the lane-name wiring wholesale, so there the
// generic keys really do store nothing — the exemption must not follow them.
TEST(MaterialUndeclaredKeysNotice, GenericLaneKeysAreUndeclaredOnADeclaredSurface)
{
    const Rendering::ShaderPropertyTable table = DeclaredTable();
    const std::vector<std::string> lines = Editor::UndeclaredKeyLines({"user0"}, {"tint"}, &table);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines.front().find("user0"), std::string::npos);
}
