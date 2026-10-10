// Win32 menu items composite `hbmpItem` only when it is a 32-bit
// premultiplied-BGRA DIB; hand Windows anything else and the icon draws as an
// opaque rectangle behind the label. That is invisible to a compile and to any
// test that only asks "did we get a bitmap back", so these run the real decode
// over real staged artwork and assert the pixel contract itself.
//
// Both authored formats are covered on purpose: the editor ships its panel
// icons as PNG and SVG, and the two travel through different decoders (stb vs
// ThorVG) that meet only at this function.

#if defined(_WIN32)

#include "MenuIconBitmap_Win32.h"

#include <gtest/gtest.h>

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

using GameEngine::Platform::CreateMenuIconBitmap;
using GameEngine::Platform::MenuIconPixelSize;

namespace
{

struct ScopedBitmap
{
    HBITMAP Handle = nullptr;
    explicit ScopedBitmap(HBITMAP h) : Handle(h) {}
    ~ScopedBitmap() { if (Handle) DeleteObject(Handle); }
    ScopedBitmap(const ScopedBitmap&) = delete;
    ScopedBitmap& operator=(const ScopedBitmap&) = delete;
};

struct BitmapPixels
{
    std::vector<uint8_t> Bgra;
    int Width = 0;
    int Height = 0;
    int BitCount = 0;
};

// Requesting DIBSECTION rather than BITMAP also proves the handle is a DIB
// section at all — GetObject returns sizeof(DIBSECTION) only for one. Row
// order is not read back from here: GDI reports biHeight as the absolute
// height, so orientation is asserted from the pixels instead.
bool ReadBitmap(HBITMAP bitmap, BitmapPixels& out)
{
    DIBSECTION dib = {};
    if (GetObjectW(bitmap, sizeof(dib), &dib) != sizeof(dib))
        return false;
    out.Width = dib.dsBm.bmWidth;
    out.Height = dib.dsBm.bmHeight < 0 ? -dib.dsBm.bmHeight : dib.dsBm.bmHeight;
    out.BitCount = dib.dsBm.bmBitsPixel;
    if (!dib.dsBm.bmBits || out.Width <= 0 || out.Height <= 0)
        return false;
    const size_t bytes = static_cast<size_t>(out.Width) * out.Height * (out.BitCount / 8);
    out.Bgra.assign(static_cast<const uint8_t*>(dib.dsBm.bmBits),
                    static_cast<const uint8_t*>(dib.dsBm.bmBits) + bytes);
    return true;
}

uint8_t AlphaAt(const BitmapPixels& px, int x, int y)
{
    return px.Bgra[(static_cast<size_t>(y) * px.Width + x) * 4u + 3u];
}

// The defining property of premultiplied alpha: no channel may exceed alpha.
// Straight alpha violates it on every antialiased edge, so this distinguishes
// the two rather than merely describing the buffer.
void ExpectPremultiplied(const BitmapPixels& px, const char* what)
{
    ASSERT_EQ(px.BitCount, 32) << what << ": hbmpItem needs a 32-bit DIB to carry alpha";

    size_t opaqueish = 0;
    size_t violations = 0;
    for (size_t i = 0; i + 3 < px.Bgra.size(); i += 4)
    {
        const uint8_t a = px.Bgra[i + 3];
        if (px.Bgra[i] > a || px.Bgra[i + 1] > a || px.Bgra[i + 2] > a)
            ++violations;
        if (a > 0)
            ++opaqueish;
    }
    EXPECT_EQ(violations, 0u) << what << ": channel exceeded alpha — buffer is straight, not premultiplied";
    EXPECT_GT(opaqueish, 0u) << what << ": decoded to a fully transparent image";
}

} // namespace

TEST(MenuIconBitmapTests, MenuIconPixelSizeIsPositive)
{
    EXPECT_GT(MenuIconPixelSize(), 0);
}

// PNG, through the engine's stb decoder.
TEST(MenuIconBitmapTests, DecodesAPngIconToPremultipliedBgra)
{
    const int size = MenuIconPixelSize();
    ScopedBitmap bmp(CreateMenuIconBitmap("editor:Icons/save.png", size, size));
    ASSERT_NE(bmp.Handle, nullptr) << "editor:Icons/save.png did not resolve or decode";

    BitmapPixels px;
    ASSERT_TRUE(ReadBitmap(bmp.Handle, px));
    EXPECT_EQ(px.Width, size);
    EXPECT_EQ(px.Height, size);
    ExpectPremultiplied(px, "save.png");
}

// SVG, through ThorVG. Four shipped panel icons are SVG, so a build without
// SVG support silently drops them from the menu — this is what says so.
TEST(MenuIconBitmapTests, DecodesAnSvgIconToPremultipliedBgra)
{
    const int size = MenuIconPixelSize();
    ScopedBitmap bmp(CreateMenuIconBitmap("editor:Icons/DockLog.svg", size, size));
    ASSERT_NE(bmp.Handle, nullptr)
        << "editor:Icons/DockLog.svg did not rasterize; SVG panel icons will be missing from native menus";

    BitmapPixels px;
    ASSERT_TRUE(ReadBitmap(bmp.Handle, px));
    EXPECT_EQ(px.Width, size);
    EXPECT_EQ(px.Height, size);
    ExpectPremultiplied(px, "DockLog.svg");
}

// Row order, asserted from the pixels rather than from the DIB header, because
// GDI hands biHeight back as an absolute value. The fixture is opaque across
// its top half and fully transparent across its bottom half, so a bottom-up
// buffer — an upside-down icon in the menu — inverts this and fails.
TEST(MenuIconBitmapTests, WritesTheTopSourceRowIntoTheTopDestinationRow)
{
    ScopedBitmap bmp(CreateMenuIconBitmap("editor:Icons/menu-icon-orientation.png", 8, 8));
    ASSERT_NE(bmp.Handle, nullptr) << "orientation fixture did not resolve or decode";

    BitmapPixels px;
    ASSERT_TRUE(ReadBitmap(bmp.Handle, px));
    ASSERT_EQ(px.Width, 8);
    ASSERT_EQ(px.Height, 8);

    EXPECT_EQ(AlphaAt(px, 0, 0), 255) << "top row is transparent — the DIB is bottom-up";
    EXPECT_EQ(AlphaAt(px, 4, 1), 255);
    EXPECT_EQ(AlphaAt(px, 0, 7), 0) << "bottom row is opaque — the DIB is bottom-up";
    EXPECT_EQ(AlphaAt(px, 4, 6), 0);
}

// The `@editor/` spelling is the same mount as `editor:`.
TEST(MenuIconBitmapTests, AcceptsBothEditorAliasSpellings)
{
    const int size = MenuIconPixelSize();
    ScopedBitmap colon(CreateMenuIconBitmap("editor:Icons/save.png", size, size));
    ScopedBitmap slash(CreateMenuIconBitmap("@editor/Icons/save.png", size, size));
    ASSERT_NE(colon.Handle, nullptr);
    EXPECT_NE(slash.Handle, nullptr) << "@editor/ spelling did not resolve";
}

TEST(MenuIconBitmapTests, YieldsNothingWhenTheUriCannotBeResolved)
{
    const int size = MenuIconPixelSize();

    // No file behind it.
    ScopedBitmap missing(CreateMenuIconBitmap("editor:Icons/no-such-icon-here.png", size, size));
    EXPECT_EQ(missing.Handle, nullptr);

    // An alias only the AssetManager can resolve must not be mistaken for a
    // relative path under the editor mount.
    ScopedBitmap foreign(CreateMenuIconBitmap("project:Art/thing.png", size, size));
    EXPECT_EQ(foreign.Handle, nullptr);

    ScopedBitmap empty(CreateMenuIconBitmap("", size, size));
    EXPECT_EQ(empty.Handle, nullptr);

    ScopedBitmap degenerate(CreateMenuIconBitmap("editor:Icons/save.png", 0, 0));
    EXPECT_EQ(degenerate.Handle, nullptr);
}

#endif // _WIN32
