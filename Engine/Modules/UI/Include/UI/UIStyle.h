#pragma once

#include "AssetCore/GUID.h"
#include "AssetCore/NineSlice.h"
#include "Mathematics/Easing.h"
#include "Types/FlatMap.h"
#include "Types/StringId.h"
#include "UI/Selectors/SelectorTypes.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace GameEngine
{

// ---- Basic style value types ----
// NOTE: DisplayMode interacts with flex-direction defaults as follows:
//  - Block / Inline: when no explicit FlexDirection is set on the layout
//    (HasFlexDirection == false), layout will treat these as column containers
//    so children stack vertically by default (HTML-like block behavior).
//  - Flex: when no explicit FlexDirection is set, layout defaults to Row,
//    matching CSS `display:flex` semantics.
//  - Any explicit FlexDirection (via CSS or code) always wins regardless of
//    DisplayMode; the flags are only used to choose sensible defaults.
enum class DisplayMode
{
    None,
    Block,
    Inline,
    Flex
};

// True for the display modes that are not flex containers. CSS block flow has
// no shrink step: such a container never shrinks a child to make it fit — the
// child keeps its size and overflows. This engine has no block formatting
// context and maps Block/Inline onto a Yoga column, which WOULD shrink them, so
// the container's mode is what decides its children's flex-shrink default
// (ApplyBlockFlowShrinkDefault, UIManager_Internal.h).
constexpr bool EstablishesBlockFlow(DisplayMode mode)
{
    return mode == DisplayMode::Block || mode == DisplayMode::Inline;
}

struct Box4
{
    float Top = 0.f;
    float Right = 0.f;
    float Bottom = 0.f;
    float Left = 0.f;
    bool operator==(const Box4&) const = default;
};

struct Box4b
{
    bool Top = false;
    bool Right = false;
    bool Bottom = false;
    bool Left = false;
    bool operator==(const Box4b&) const = default;
};

// Length value that can be Auto (unset), Pixels, or Percent.
// Used for properties where CSS distinguishes between "unset/auto" and a concrete length.
struct StyleLength
{
    enum class UnitType : uint8_t
    {
        Auto = 0,
        Px,
        Percent
    };

    UnitType Unit = UnitType::Auto;
    float Value = 0.0f; // for Px or Percent; ignored for Auto

    static StyleLength Auto() { return StyleLength{UnitType::Auto, 0.0f}; }
    static StyleLength Px(float px) { return StyleLength{UnitType::Px, px}; }
    static StyleLength Percent(float pct) { return StyleLength{UnitType::Percent, pct}; }

    bool IsAuto() const { return Unit == UnitType::Auto; }
    bool IsPercent() const { return Unit == UnitType::Percent; }
    bool IsPx() const { return Unit == UnitType::Px; }
    bool operator==(const StyleLength&) const = default;
};

// Per-edge border colors (ARGB 0xAARRGGBB), stored in TRBL order for consistency
// with Box4 (top/right/bottom/left). Default alpha==0 means "unset" (transparent).
struct BorderColorsTRBL
{
    uint32_t Top = 0x000000FFu;
    uint32_t Right = 0x000000FFu;
    uint32_t Bottom = 0x000000FFu;
    uint32_t Left = 0x000000FFu;
    bool operator==(const BorderColorsTRBL&) const = default;
};

// One corner's elliptical radii: X is the horizontal semi-axis, Y the vertical
// (css-backgrounds-3 §5.1). Equal axes are a circular corner. Values are in
// pixels unless a companion CornerAxisFlags marks an axis as a percentage.
struct CornerRadius
{
    float X = 0.0f;
    float Y = 0.0f;
    bool operator==(const CornerRadius&) const = default;
};

// Per-corner elliptical radii in TL/TR/BR/BL order.
struct CornerRadiiTLTRBRBL
{
    CornerRadius TopLeft{};
    CornerRadius TopRight{};
    CornerRadius BottomRight{};
    CornerRadius BottomLeft{};

    CornerRadiiTLTRBRBL() = default;
    // Circular corners: one radius per corner covers both axes. This is the
    // whole programmatic surface; an ellipse is authored through CSS.
    CornerRadiiTLTRBRBL(float tl, float tr, float br, float bl)
        : TopLeft{tl, tl}, TopRight{tr, tr}, BottomRight{br, br}, BottomLeft{bl, bl}
    {
    }
    bool operator==(const CornerRadiiTLTRBRBL&) const = default;
};

// Percentage markers for one corner's two radii, same X/Y meaning as
// CornerRadius.
struct CornerAxisFlags
{
    bool X = false;
    bool Y = false;
    bool operator==(const CornerAxisFlags&) const = default;
};

// Per-corner flag companion to CornerRadiiTLTRBRBL, same TL/TR/BR/BL order.
struct CornerFlagsTLTRBRBL
{
    CornerAxisFlags TopLeft{};
    CornerAxisFlags TopRight{};
    CornerAxisFlags BottomRight{};
    CornerAxisFlags BottomLeft{};
    bool operator==(const CornerFlagsTLTRBRBL&) const = default;
};

// The parsed value of a border-*-radius longhand: the horizontal then the
// vertical <length-percentage> of one corner (css-backgrounds-3 §5.1). A
// one-value declaration duplicates it into both axes.
struct CornerRadiusValue
{
    StyleLength Horizontal{};
    StyleLength Vertical{};
    bool operator==(const CornerRadiusValue&) const = default;
};

enum class FlexDirection
{
    Row,
    Column
};
// Cross-axis alignment, shared by align-items and align-self. `Auto` is
// meaningful only for align-self (defer to the container's align-items).
enum class AlignItems
{
    Stretch,
    FlexStart,
    Center,
    FlexEnd,
    Baseline,
    Auto
};

// Main-axis alignment (container)
enum class JustifyContent
{
    FlexStart,
    Center,
    FlexEnd,
    SpaceBetween,
    SpaceAround,
    SpaceEvenly
};

// Cross-line alignment (multi-line containers)
enum class AlignContent
{
    Stretch,
    FlexStart,
    Center,
    FlexEnd,
    SpaceBetween,
    SpaceAround,
    SpaceEvenly
};

// Text alignment (horizontal)
enum class TextAlign
{
    Left,
    Center,
    Right
};

// CSS word-break (css-text-3 5.1): where a line MAY break within a word.
// BreakWord is the legacy value css-text-3 defines as `word-break: normal`
// plus `overflow-wrap: anywhere`, which is why it lives here and not in
// OverflowWrap.
enum class WordBreak : uint8_t
{
    Normal,
    BreakAll,
    KeepAll,
    BreakWord
};

// CSS overflow-wrap (css-text-3 5.4): whether a word with no break opportunity
// may be broken rather than overflow its line. Independent of word-break, and
// the property `word-wrap` aliases. `anywhere` parses to BreakWord: the two
// differ only in the min-content contribution, which layout here does not
// compute separately.
enum class OverflowWrap : uint8_t
{
    Normal,
    BreakWord
};

// The two properties above are independent, but a line breaker takes a single
// policy, so collapse the pair at the point one is handed to it. break-all is
// the most permissive; an unbreakable word that would overflow may be broken
// when either overflow-wrap allows it or the legacy word-break value is in
// force.
constexpr WordBreak ResolveTextBreakPolicy(WordBreak wordBreak, OverflowWrap overflowWrap)
{
    if (wordBreak == WordBreak::BreakAll)
        return WordBreak::BreakAll;
    if (wordBreak == WordBreak::BreakWord || overflowWrap == OverflowWrap::BreakWord)
        return WordBreak::BreakWord;
    return wordBreak;
}

// Subset of CSS white-space that controls whether text wraps.
enum class WhiteSpace : uint8_t
{
    Normal,
    NoWrap,
    Pre
};

// CSS text-overflow: how an element's own single-line text run paints when it
// is wider than the content box. Ellipsis truncates the run and draws "…"
// after the last fully visible glyph; Clip (the default) hard-clips at the
// box. Applies only to a single-line run on an element whose overflow is not
// visible — multi-line text always clips. Not inherited, per CSS.
enum class TextOverflowMode : uint8_t
{
    Clip,
    Ellipsis
};

// Layout direction
enum class Direction
{
    Inherit,
    LTR,
    RTL
};

// Positioning (Yoga position type + offsets)
enum class PositionType
{
    Relative,
    Absolute
};

// Overflow / clipping
enum class Overflow
{
    Visible,
    Hidden
};

enum class BorderStyle : uint8_t
{
    None,
    Solid,
    Dashed,
    Dotted
};

// CSS line-width keywords (`thin` / `medium` / `thick`) in logical px, the
// values Blink resolves them to. `medium` is also the CSS initial value of
// outline-width.
inline constexpr float kThinLineWidthPx = 1.0f;
inline constexpr float kMediumLineWidthPx = 3.0f;
inline constexpr float kThickLineWidthPx = 5.0f;

// Cursor appearance (CSS cursor property)
enum class CursorStyle : uint8_t
{
    Auto,       // default cursor (arrow)
    Pointer,    // hand/pointing cursor (clickable)
    Text,       // I-beam cursor (text input)
    Crosshair,  // crosshair cursor
    Move,       // move/drag cursor
    ColResize,  // horizontal resize (col-resize)
    RowResize,  // vertical resize (row-resize)
    NorthwestSoutheastResize, // diagonal resize from top-left to bottom-right
    NotAllowed, // not-allowed/forbidden cursor
    Grab,       // grab hand cursor
    Grabbing    // grabbing hand cursor
};

// Font weight/style/variant (subset).
enum class FontStyle : uint8_t
{
    Normal,
    Italic,
    Oblique
};
enum class FontVariant : uint8_t
{
    Normal,
    SmallCaps
};

enum class BackgroundRepeat
{
    NoRepeat,
    Repeat,
    RepeatX,
    RepeatY
};
enum class BackgroundSizeMode
{
    Auto,
    Cover,
    Contain,
    Explicit
};

// Background image source override (image only; not repeat/size/position).
struct BackgroundImageSource
{
    enum class SourceKind : uint8_t
    {
        None = 0,
        Guid,
        Path,
        ResourceName
    };

    SourceKind Kind = SourceKind::None;
    GUID Guid = GUID::Null();
    // For Path or ResourceName kinds, holds the string value.
    std::string Value;
    // Optional explicit asset-source alias for Path resolution (e.g.
    // "editor", "project"). When non-empty, ResolveBackgroundImagePath
    // looks up the path under exactly this source instead of falling
    // through implicit priority. Set by `url("editor:path")` or
    // `url("@editor/path")` syntax in CSS.
    std::string SourceAlias;

    bool HasImage() const { return Kind != SourceKind::None; }
    bool operator==(const BackgroundImageSource&) const = default;
};

struct BackgroundImageStyle
{
    BackgroundImageSource Source{};
    bool HasImage = false;
    BackgroundRepeat Repeat = BackgroundRepeat::NoRepeat;
    BackgroundSizeMode SizeMode = BackgroundSizeMode::Auto;
    float SizeX = -1.0f;
    bool SizeXIsPercent = false;
    float SizeY = -1.0f;
    bool SizeYIsPercent = false;
    float PosX = 50.0f;
    bool PosXIsPercent = true;
    float PosY = 50.0f;
    bool PosYIsPercent = true;
    bool HasTint = false;
    uint32_t Tint = 0xFFFFFFFFu;
    float Saturation = 1.0f; // 1 = full color, 0 = grayscale (luma)

    // Per-element 9-slice override (CSS border-image-*). When HasBorderImage is set
    // (only border-image-slice sets it), these REPLACE the texture's intrinsic slice
    // metadata for this element. The slice SOURCE is reused from this element's
    // background-image; border-image-* has no effect on an element without one
    // (there is intentionally no border-image-source). Slice insets are in source
    // texels (CSS order: top/right/bottom/left); all-zero = un-sliced (stretched).
    // Repeat maps to the edge/center fill modes.
    bool HasBorderImage = false;
    float BiSlice[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // top, right, bottom, left
    bool BiSliceFill = true;                     // draw the center (CSS `fill`)
    NineSliceFill BiRepeatX = NineSliceFill::Stretch;
    NineSliceFill BiRepeatY = NineSliceFill::Stretch;
};

// CSS variable scope: structurally shared custom properties (`--vars`) with inheritance.
// Most elements will share the parent scope pointer; only elements that define
// custom properties allocate a new scope with local overrides.
struct CustomPropertyScope
{
    struct Entry
    {
        bool Invalid = false;
        std::string Value;

        enum class Kind : uint8_t { None, Float, Color, Length };
        mutable Kind CachedType = Kind::None;
        // The reading scope the cached value was computed for: 0 when Value has no var() and
        // the cache holds for every reader, else that reader's UniqueId.
        mutable uint64_t CachedScopeId = 0;
        mutable float CachedFloat = 0.0f;
        mutable uint32_t CachedColor = 0;
        mutable StyleLength CachedLength{};
    };

    std::shared_ptr<const CustomPropertyScope> Parent;
    FlatMap<StringId, Entry> Local;
    // Stable identity for memoization. Raw pointer addresses can be reused after
    // scopes are destroyed (especially in tests), so caches must not key solely
    // on pointer values.
    uint64_t UniqueId = 0;

    enum class LookupStatus : uint8_t
    {
        Missing = 0,
        Found,
        Invalid
    };

    LookupStatus Lookup(StringId name, const Entry*& outValue) const;
};

// CSS-wide keywords that can apply to (almost) any property value.
// See: https://www.w3.org/TR/css-values-4/#common-keywords
enum class StyleKeyword : uint8_t
{
    None = 0,
    Inherit,
    Initial,
    Unset
};

// Box with per-edge unit flags (used by margin/padding overrides). `IsAuto` is
// meaningful for margin only — CSS has no `padding: auto` — so the padding
// appliers ignore it and keep the zero in Values.
struct StyleBox
{
    Box4 Values{};
    Box4b IsPercent{};
    Box4b IsAuto{};
    bool operator==(const StyleBox&) const = default;
};

struct BackgroundSizeValue
{
    BackgroundSizeMode Mode = BackgroundSizeMode::Auto;
    float SizeX = -1.0f;
    bool SizeXIsPercent = false;
    float SizeY = -1.0f;
    bool SizeYIsPercent = false;
    bool operator==(const BackgroundSizeValue&) const = default;
};

struct BackgroundPositionValue
{
    float X = 50.0f;
    bool XIsPercent = true;
    float Y = 50.0f;
    bool YIsPercent = true;
    bool operator==(const BackgroundPositionValue&) const = default;
};

enum class StylePropertyId : uint16_t
{
    Unknown,
    Display,
    FlexDir,
    FlexWrap,
    AlignItems,
    JustifyContent,
    AlignSelf,
    AlignContent,
    Direction,
    Position,
    PositionLeft,
    PositionTop,
    PositionRight,
    PositionBottom,
    Order,
    ZIndex,
    FlexGrow,
    FlexShrink,
    FlexBasis,
    Visibility,
    PointerEvents,
    Opacity,
    Overflow,
    OverflowX,
    OverflowY,
    Cursor,
    TextAlign,
    WordBreak,
    OverflowWrap,
    WhiteSpace,
    TextOverflow,
    FontSize,
    LineHeight,
    LetterSpacing,
    FontFamily,
    FontWeight,
    FontStyle,
    FontVariant,
    Color,
    BackgroundColor,
    BackgroundImage,
    BackgroundSize,
    BackgroundPosition,
    BackgroundRepeat,
    BackgroundTint,
    BackgroundImageSaturation,
    BorderColor,
    BorderTopColor,
    BorderRightColor,
    BorderBottomColor,
    BorderLeftColor,
    BorderWidth,
    BorderTopWidth,
    BorderRightWidth,
    BorderBottomWidth,
    BorderLeftWidth,
    BorderStyle,
    BorderRadius,
    BorderTopLeftRadius,
    BorderTopRightRadius,
    BorderBottomRightRadius,
    BorderBottomLeftRadius,
    Margin,
    MarginTop,
    MarginRight,
    MarginBottom,
    MarginLeft,
    Padding,
    PaddingTop,
    PaddingRight,
    PaddingBottom,
    PaddingLeft,
    Gap,
    RowGap,
    ColumnGap,
    Width,
    Height,
    MinWidth,
    MinHeight,
    MaxWidth,
    MaxHeight,
    AspectRatio,
    BoxShadow,
    Glow,
    Transition,
    CustomVar,
    BorderImageSlice,
    BorderImageRepeat,
    OutlineWidth,
    OutlineStyle,
    OutlineColor,
    OutlineOffset,
    TextShadowOffsetX,
    TextShadowOffsetY,
    TextShadowBlur,
    TextShadowColor,
    TextGlowRadius,
    TextGlowColor,
    TextOutlineWidth,
    TextOutlineColor,
    SelectionColor,
    // Raw declaration captured when the value contains var(). This is expanded
    // and parsed at compute time after variable substitution so that var()
    // can appear anywhere, including inside shorthands.
    DeferredDecl,
    _Count
};

// Canonical property → invalidation impact (C-13). Single source of truth
// consumed by the dynamic style analysis (hover/pseudo escalation gating)
// and anything else deciding whether a property change can move layout or
// only repaint. Exhaustive switch, no default: a new StylePropertyId left
// unclassified falls out as {false,false} and the analysis treats the rule
// conservatively — but classify it here when adding one.
struct StylePropertyImpact
{
    bool Layout = false;
    bool Paint = false;
    // The change reaches DESCENDANTS, not only this element's own paint box.
    // A pure paint mark refreshes just the marked element —
    // UIManager::NotifyDirty_UpdateRegenFlag routes VisualDirty to the
    // per-element drain, which rewrites that element's primitive bytes and
    // never revisits a child — so a property flagged here has to escalate to a
    // full regen or its subtree keeps painting the previous value.
    //
    // Layout-impact properties never need the flag: they escalate already.
    bool Subtree = false;
};

constexpr StylePropertyImpact GetStylePropertyImpact(StylePropertyId id)
{
    switch (id)
    {
    // Layout: consumed by the Yoga solve (box metrics, flex, text measure).
    case StylePropertyId::Display:
    case StylePropertyId::FlexDir:
    case StylePropertyId::FlexWrap:
    case StylePropertyId::AlignItems:
    case StylePropertyId::JustifyContent:
    case StylePropertyId::AlignSelf:
    case StylePropertyId::AlignContent:
    case StylePropertyId::Direction:
    case StylePropertyId::Position:
    case StylePropertyId::PositionLeft:
    case StylePropertyId::PositionTop:
    case StylePropertyId::PositionRight:
    case StylePropertyId::PositionBottom:
    case StylePropertyId::Order:
    case StylePropertyId::FlexGrow:
    case StylePropertyId::FlexShrink:
    case StylePropertyId::FlexBasis:
    case StylePropertyId::WordBreak:
    case StylePropertyId::OverflowWrap:
    case StylePropertyId::WhiteSpace:
    case StylePropertyId::FontSize:
    case StylePropertyId::LineHeight:
    case StylePropertyId::LetterSpacing:
    case StylePropertyId::FontFamily:
    case StylePropertyId::FontWeight:
    case StylePropertyId::FontStyle:
    case StylePropertyId::FontVariant:
    case StylePropertyId::Margin:
    case StylePropertyId::MarginTop:
    case StylePropertyId::MarginRight:
    case StylePropertyId::MarginBottom:
    case StylePropertyId::MarginLeft:
    case StylePropertyId::Padding:
    case StylePropertyId::PaddingTop:
    case StylePropertyId::PaddingRight:
    case StylePropertyId::PaddingBottom:
    case StylePropertyId::PaddingLeft:
    case StylePropertyId::Gap:
    case StylePropertyId::RowGap:
    case StylePropertyId::ColumnGap:
    case StylePropertyId::Width:
    case StylePropertyId::Height:
    case StylePropertyId::MinWidth:
    case StylePropertyId::MinHeight:
    case StylePropertyId::MaxWidth:
    case StylePropertyId::MaxHeight:
    case StylePropertyId::AspectRatio:
        return {true, false};

    // Layout AND paint. Border widths inset the content box in the Yoga solve
    // (YogaAdapter::ApplyStyle) and are also what the border ring paints, so a
    // width change moves children, wrap widths and auto-grown heights as well
    // as the drawn ring. The per-axis overflow switches toggle scrollbars
    // (layout) and clipping (paint).
    case StylePropertyId::OverflowX:
    case StylePropertyId::OverflowY:
    case StylePropertyId::BorderWidth:
    case StylePropertyId::BorderTopWidth:
    case StylePropertyId::BorderRightWidth:
    case StylePropertyId::BorderBottomWidth:
    case StylePropertyId::BorderLeftWidth:
        return {true, true};

    // Paint only, and reaching descendants. Three mechanisms, all of them here:
    //   opacity    the emit walk MULTIPLIES it into every descendant primitive
    //              (UIManager_PrimitiveGen.cpp: ctx.EffectiveOpacity *= ...).
    //   overflow   it establishes the CLIP descendants are emitted against, and
    //              clip slots are assigned only by the full DFS.
    //   colour,    INHERITED and painted, so a descendant that states none of
    //   text-align its own has the parent's value baked into its glyphs, or
    //   visibility, into its selection highlight for selection-color.
    //   selection-color
    // Cursor and pointer-events are inherited too but are read live off
    // ResolvedStyle by hit-testing rather than baked into a primitive, so they
    // cannot go stale in the snapshot and stay off this list.
    case StylePropertyId::Opacity:
    case StylePropertyId::Overflow:
    case StylePropertyId::Visibility:
    case StylePropertyId::Color:
    case StylePropertyId::TextAlign:
    case StylePropertyId::SelectionColor:
        return {false, true, true};

    // Paint only, confined to the element itself.
    // TextOverflow never feeds the Yoga solve — the measure is of the full
    // text either way; only this element's own glyph emission changes.
    case StylePropertyId::TextOverflow:
    case StylePropertyId::ZIndex:
    case StylePropertyId::PointerEvents:
    case StylePropertyId::Cursor:
    case StylePropertyId::BackgroundColor:
    case StylePropertyId::BackgroundImage:
    case StylePropertyId::BackgroundSize:
    case StylePropertyId::BackgroundPosition:
    case StylePropertyId::BackgroundRepeat:
    case StylePropertyId::BackgroundTint:
    case StylePropertyId::BackgroundImageSaturation:
    case StylePropertyId::BorderColor:
    case StylePropertyId::BorderTopColor:
    case StylePropertyId::BorderRightColor:
    case StylePropertyId::BorderBottomColor:
    case StylePropertyId::BorderLeftColor:
    case StylePropertyId::BorderStyle:
    case StylePropertyId::BorderRadius:
    case StylePropertyId::BorderTopLeftRadius:
    case StylePropertyId::BorderTopRightRadius:
    case StylePropertyId::BorderBottomRightRadius:
    case StylePropertyId::BorderBottomLeftRadius:
    case StylePropertyId::TextShadowOffsetX:
    case StylePropertyId::TextShadowOffsetY:
    case StylePropertyId::TextShadowBlur:
    case StylePropertyId::TextShadowColor:
    case StylePropertyId::TextGlowRadius:
    case StylePropertyId::TextGlowColor:
    case StylePropertyId::TextOutlineWidth:
    case StylePropertyId::TextOutlineColor:
    case StylePropertyId::BoxShadow:
    case StylePropertyId::Glow:
    case StylePropertyId::BorderImageSlice:
    case StylePropertyId::BorderImageRepeat:
    // Outline is not part of the box model: it is drawn outside the border
    // edge and never feeds the Yoga solve, so it can only ever repaint.
    case StylePropertyId::OutlineWidth:
    case StylePropertyId::OutlineStyle:
    case StylePropertyId::OutlineColor:
    case StylePropertyId::OutlineOffset:
        return {false, true};

    // Special-cased by consumers (variable/deferred resolution, transition
    // declarations) or not a visual property at all.
    case StylePropertyId::Unknown:
    case StylePropertyId::Transition:
    case StylePropertyId::CustomVar:
    case StylePropertyId::DeferredDecl:
    case StylePropertyId::_Count:
        return {false, false};
    }
    return {false, false};
}

static_assert(static_cast<size_t>(StylePropertyId::DeferredDecl) + 1
           == static_cast<size_t>(StylePropertyId::_Count),
              "StylePropertyId must be contiguous -- _Count must follow the last entry");

struct CustomVarDecl
{
    std::string Name;
    std::string Value;
    bool operator==(const CustomVarDecl&) const = default;
};

struct DeferredDeclValue
{
    std::string Name;
    std::string Value;
    bool operator==(const DeferredDeclValue&) const = default;
};

struct BoxShadowValue
{
    float OffsetX = 0;
    float OffsetY = 0;
    float Blur = 0;
    uint32_t Color = 0x00000000u; // ARGB
    bool Inset = false;
    bool operator==(const BoxShadowValue&) const = default;
};

struct GlowValue
{
    float Radius = 0;
    uint32_t Color = 0x00000000u; // ARGB
    bool operator==(const GlowValue&) const = default;
};

struct TransitionEntry
{
    StylePropertyId Property = StylePropertyId::Unknown;
    float DurationSec = 0.0f;
    float DelaySec = 0.0f;
    Math::EasingFunction Easing = Math::EasingFunction::Ease;
    bool operator==(const TransitionEntry&) const = default;
};

// CSS `border-image-slice` value: insets in source texels (top/right/bottom/left)
// plus the `fill` keyword (draw the center).
struct BorderImageSliceValue
{
    float Top = 0.0f, Right = 0.0f, Bottom = 0.0f, Left = 0.0f;
    // Programmatic/zero-init default; the CSS parser overrides to false unless the
    // `fill` keyword is present (CSS border-image-slice semantics).
    bool Fill = true;
    bool operator==(const BorderImageSliceValue&) const = default;
};

// CSS `border-image-repeat` value mapped to the 9-slice fill modes (x = horizontal
// edges + center horizontally, y = vertical edges + center vertically).
struct BorderImageRepeatValue
{
    NineSliceFill X = NineSliceFill::Stretch;
    NineSliceFill Y = NineSliceFill::Stretch;
    bool operator==(const BorderImageRepeatValue&) const = default;
};

using StyleValue = std::variant<std::monostate,
                                DisplayMode,
                                FlexDirection,
                                AlignItems,
                                JustifyContent,
                                AlignContent,
                                Direction,
                                PositionType,
                                TextAlign,
                                WordBreak,
                                OverflowWrap,
                                WhiteSpace,
                                TextOverflowMode,
                                Overflow,
                                BorderStyle,
                                CursorStyle,
                                FontStyle,
                                FontVariant,
                                BackgroundRepeat,
                                BackgroundImageSource,
                                BackgroundSizeValue,
                                BackgroundPositionValue,
                                StyleLength,
                                StyleBox,
                                std::vector<std::string>,
                                uint32_t,
                                bool,
                                float,
                                int,
                                CustomVarDecl,
                                DeferredDeclValue,
                                Box4,
                                CornerRadiiTLTRBRBL,
                                CornerRadiusValue,
                                BorderColorsTRBL,
                                BoxShadowValue,
                                GlowValue,
                                BorderImageSliceValue,
                                BorderImageRepeatValue,
                                std::vector<TransitionEntry>>;

struct StyleProperty
{
    StylePropertyId PropertyId = StylePropertyId::Unknown;

    // CSS-wide keyword marker. When not None, the property value should be resolved
    // against the parent style and/or initial values during cascade resolution.
    StyleKeyword Keyword = StyleKeyword::None;

    // !important flag. When true, this declaration wins over any non-important
    // declaration regardless of selector specificity (within the same origin).
    bool Important = false;

    // Source info (best-effort line number in source stylesheet; -1 if unknown)
    int SourceLine = -1;

    StyleValue Value{};
};

struct CSSRule
{
    SelectorChain Selector{};
    std::vector<StyleProperty> Properties{};
    int Order = 0; // source order
    // Specificity lives on Selector.Spec as the full (A, B, C) triple.
};

// A stylesheet's tier in the cascade (CSS Cascade 4, "Cascade Sorting Order"): every UserAgent
// declaration loses to every Author declaration before specificity or source order is compared.
enum class StyleOrigin : uint8_t
{
    UserAgent, // the UI module's default stylesheet
    Author,    // themes and documents
};

// Stylesheet comprising a list of rules
struct Stylesheet
{
    std::vector<CSSRule> Rules;
    StyleOrigin Origin = StyleOrigin::Author;
    std::string SourceName; // best-effort stylesheet source path (if known)

    // Declaration names in this sheet that no parser row claims, deduped, in
    // first-seen order. An unregistered property is dropped whole, so without
    // this it leaves no trace at all. The parser only records the names: the
    // source path they have to be reported against is not set until the loader
    // fills SourceName, which happens after the parse returns.
    std::vector<std::string> UnknownProperties;

    // If this stylesheet originated from an AssetManager UIStyle asset, this is the owning
    // asset's GUID. This provides stable identity across hot reloads even when pointer
    // identity changes (e.g., segment model recreating sheet objects).
    GUID OwnerAssetGuid = GUID::Null();

    // Optional debug palette (kept from stub to avoid churn)
    std::unordered_map<std::string, uint32_t> DebugClassColors;
};

// Runtime-optimized rule index for faster CSS matching.
// Stores indices into Stylesheet::rules, partitioned by the rightmost selector term.
// All vectors preserve source order (ascending rule index).
struct StylesheetRuleIndex
{
    // All three indices are keyed by deterministic StringId. The cascade hot
    // path (CSSParser::ComputeStyleFor) iterates an element's class ids and
    // looks them up here directly, avoiding per-visit string hashing/compare.
    std::unordered_map<StringId, std::vector<uint32_t>> ById;
    std::unordered_map<StringId, std::vector<uint32_t>> ByClass;
    std::unordered_map<StringId, std::vector<uint32_t>> ByTag;
    std::vector<uint32_t> Universal;

    // P4 sibling style sharing: buckets holding a "share-sensitive" rule —
    // one whose match can differ between same-key siblings (sibling
    // combinators anywhere in the chain; positional/:empty/attribute
    // selectors on the rightmost compound). An element probing any
    // sensitive bucket is excluded from cascade sharing; everything else
    // shares safely because all remaining match inputs are part of the
    // share key (parent chain, classes, tag, dynamic state).
    std::unordered_set<StringId> ShareSensitiveClasses;
    std::unordered_set<StringId> ShareSensitiveTags;
    bool ShareSensitiveUniversal = false;
};

// Shared stylesheet reference used for deduplication and subtree attachment.
using StylesheetHandle = std::shared_ptr<const Stylesheet>;

} // namespace GameEngine
