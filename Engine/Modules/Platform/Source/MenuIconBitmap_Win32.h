#pragma once

#if defined(_WIN32)

#include <windows.h>

#include <string>

namespace GameEngine {
namespace Platform {

/// The edge length Windows draws a menu item image at, in physical pixels.
int MenuIconPixelSize();

/**
 * @brief Decode an engine image URI into a menu-sized icon bitmap.
 *
 * The result is a 32-bit top-down DIB holding **premultiplied** BGRA, which is
 * the only form `MENUITEMINFO::hbmpItem` composites with alpha: a bitmap
 * carrying no alpha channel is drawn as an opaque rectangle behind the label.
 *
 * `imagePath` is a CSS-style engine URI — `editor:Icons/foo.png`,
 * `@editor/Icons/foo.png`, or a path already absolute on disk. PNG and JPEG go
 * through the engine's stb decoder and SVG through its ThorVG rasterizer, so a
 * menu row shows the same artwork the UI does.
 *
 * @return nullptr when the URI names no readable file, carries an alias this
 *         layer cannot resolve, or decodes to nothing. The caller owns the
 *         bitmap and must `DeleteObject` it — Windows never frees an hbmpItem.
 */
HBITMAP CreateMenuIconBitmap(const std::string& imagePath, int width, int height);

} // namespace Platform
} // namespace GameEngine

#endif // _WIN32
