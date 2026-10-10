#pragma once

#include "UI/UIStyle.h"

#include <span>
#include <string>
#include <string_view>

namespace GameEngine {
namespace UIStyleQuery {

/**
 * @brief Read back the `background-image` a stylesheet cascade computes for a class.
 *
 * Answers "which image does `.<className>` mean?" for consumers the cascade
 * cannot style — the platform context menu's rows, which live outside the
 * retained UI tree — so the URL is authored once, in CSS, instead of being
 * duplicated into C++.
 *
 * The answer comes from the real cascade: a detached `Button` wearing the class
 * is run through `CSSParser::ComputeStyleFor`, so specificity, source order,
 * `!important` and `var()` all behave exactly as they do for a styled element.
 * `Button` is the probe because it is what the shipped stylesheets style —
 * nearly every icon is authored `.foo, button.foo, .button.foo`, and a Button
 * carries the `button` tag and the `button` class, so all three spellings
 * resolve to one answer.
 *
 * The probe has no parent, deliberately: descendant selectors stay unmatched, so
 * a rule saying how the class looks inside some container cannot claim to say
 * what the class *is*. Element state is default, so `:hover`/`:active` forms are
 * likewise ignored.
 *
 * @param sheets    The cascade, lowest precedence first (UIManager::GetStylesheets()).
 * @param className Class name without the leading dot.
 * @return The URL as authored, source alias included ("editor:Icons/foo.png").
 *         Empty when the cascade computes no image for the class, or when it
 *         computes one with no authored path — `url(guid:...)` has no path to
 *         hand back.
 */
std::string ResolveClassBackgroundImageUrl(std::span<const StylesheetHandle> sheets,
                                           std::string_view className);

} // namespace UIStyleQuery
} // namespace GameEngine
