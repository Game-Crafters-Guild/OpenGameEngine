// The Asset View preview shows the RAW texture, so the previewed asset's own `Sliced`
// import setting must not change how the preview is laid out. Two halves have to meet for
// that, and both live in the preview element's override chain: the element asks for
// BackgroundSize::Contain, and it overrides the texture's intrinsic 9-slice with a
// border-image slice of zero. The renderer reads an override whose four insets are all zero
// as "un-sliced" and takes the background-size path (UIManager_PrimitiveGen.cpp,
// EmitBackgroundImagePrimitive); without the override, an intrinsically sliced texture fills
// the element rect instead and the image is distorted.
//
// Source-level, like the sibling guards in this target (PanelDefaultTabIconTests,
// TerrainInspectorSectionStyleTests): AssetViewPanel drives a video player, GPU texture
// uploads and the thumbnail service, so it cannot be constructed in this process. What is
// locked is that the two halves stay paired on the preview element — not how the rest of the
// panel is styled, and not the emitted geometry, which the renderer owns.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{

std::string ReadAssetViewPanelSource()
{
    const std::filesystem::path path =
        std::filesystem::path(GE_EDITOR_SOURCE_DIR) / "Source" / "Panels" / "AssetViewPanel.cpp";
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The one statement that configures the preview element: from `image->Overrides()` to the
// semicolon that ends the chained call. Scoping to it is the point — the panel sets the same
// properties on other elements, so searching the whole file would keep passing after the
// preview element itself lost the override.
std::string PreviewOverrideStatement(const std::string& source)
{
    const size_t start = source.find("image->Overrides()");
    if (start == std::string::npos)
        return {};
    const size_t end = source.find(';', start);
    if (end == std::string::npos)
        return {};
    return source.substr(start, end - start);
}

// The four inset arguments of `BorderImageSliceValue{...}`, in CSS order (top, right,
// bottom, left). Empty when the statement carries no such value.
std::vector<float> BorderImageSliceInsets(const std::string& statement)
{
    const std::string marker = "BorderImageSliceValue{";
    const size_t open = statement.find(marker);
    if (open == std::string::npos)
        return {};
    const size_t close = statement.find('}', open);
    if (close == std::string::npos)
        return {};

    const std::string argumentList =
        statement.substr(open + marker.size(), close - (open + marker.size()));

    std::vector<float> insets;
    std::istringstream fields(argumentList);
    std::string field;
    while (insets.size() < 4u && std::getline(fields, field, ','))
    {
        try
        {
            insets.push_back(std::stof(field));
        }
        catch (const std::exception&)
        {
            return {};
        }
    }
    return insets;
}

} // namespace

TEST(AssetViewPreviewSlice, PreviewElementOverridesTheTextureIntrinsicSliceWithZeroInsets)
{
    const std::string source = ReadAssetViewPanelSource();
    ASSERT_FALSE(source.empty()) << "AssetViewPanel.cpp did not read from GE_EDITOR_SOURCE_DIR";

    const std::string statement = PreviewOverrideStatement(source);
    ASSERT_FALSE(statement.empty())
        << "the preview element's `image->Overrides()` chain was not found; rename the probe "
           "with the code rather than deleting it";

    EXPECT_NE(statement.find("Style::BorderImageSlice"), std::string::npos)
        << "the preview takes the previewed texture's intrinsic 9-slice without this override, "
           "and a Sliced texture stretches to fill the panel";

    const std::vector<float> insets = BorderImageSliceInsets(statement);
    ASSERT_EQ(insets.size(), 4u) << "the override must carry four inset arguments";
    for (const float inset : insets)
        EXPECT_FLOAT_EQ(inset, 0.0f) << "only an all-zero slice reads as un-sliced";
}

TEST(AssetViewPreviewSlice, PreviewElementAsksForContainSizing)
{
    const std::string source = ReadAssetViewPanelSource();
    ASSERT_FALSE(source.empty()) << "AssetViewPanel.cpp did not read from GE_EDITOR_SOURCE_DIR";

    const std::string statement = PreviewOverrideStatement(source);
    ASSERT_FALSE(statement.empty()) << "the preview element's `image->Overrides()` chain was not found";

    EXPECT_NE(statement.find("BackgroundSizeMode::Contain"), std::string::npos)
        << "the zero slice only preserves a fit that the element asks for";
}
