#include "StyleApplier.h"

#include <cstddef>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace GameEngine
{

using PropertyApplier = void (*)(ResolvedStyle&, const StyleValue&);

// LRU cache for shared font-family vectors. Avoids heap allocation when
// multiple elements resolve to the same CSS font-family list. The cache
// holds 4 entries because typical stylesheets declare 1-3 distinct
// font-family stacks; 4 covers that with a small margin.
static constexpr size_t kFontFamilyCacheSize = 4;
struct FontFamilyCacheEntry { std::shared_ptr<const std::vector<std::string>> Ptr; };
static thread_local FontFamilyCacheEntry s_FontFamilyCache[kFontFamilyCacheSize];
static thread_local size_t s_FontFamilyCacheNext = 0;

static void ApplyFontFamily(ResolvedStyle& rs, const StyleValue& v)
{
    if (auto* p = std::get_if<std::vector<std::string>>(&v))
    {
        if (!rs.Visual.FontFamily || *rs.Visual.FontFamily != *p)
        {
            bool found = false;
            for (auto& e : s_FontFamilyCache)
            {
                if (e.Ptr && *e.Ptr == *p) { rs.Visual.FontFamily = e.Ptr; found = true; break; }
            }
            if (!found)
            {
                auto ptr = std::make_shared<const std::vector<std::string>>(*p);
                s_FontFamilyCache[s_FontFamilyCacheNext % kFontFamilyCacheSize].Ptr = ptr;
                ++s_FontFamilyCacheNext;
                rs.Visual.FontFamily = std::move(ptr);
            }
        }
        rs.Visual.HasFontFamily = true;
    }
}

// Table is positionally indexed by StylePropertyId enum value: adding an id
// without adding its lambda at the matching position silently misapplies every
// property after it. The array is deliberately left unbounded so the assertion
// below compares the initializer count against the enum; giving it an explicit
// `[_Count]` bound would pad the tail with nullptr and make that comparison
// `_Count == _Count`, which cannot fail. The assertion catches a wrong count,
// not a wrong order — a swap of two same-typed entries still compiles.
static const PropertyApplier kAppliers[] = {
    /* Unknown     */ nullptr,
    /* Display     */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<DisplayMode>(&v)) rs.Layout.DisplayMode = *p;
    },
    /* FlexDir */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<FlexDirection>(&v)) { rs.Layout.FlexDirection = *p; rs.Layout.HasFlexDirection = true; }
    },
    /* FlexWrap */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<bool>(&v)) rs.Layout.FlexWrap = *p;
    },
    /* AlignItems */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<AlignItems>(&v)) rs.Layout.AlignItems = *p;
    },
    /* JustifyContent */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<JustifyContent>(&v)) rs.Layout.JustifyContent = *p;
    },
    /* AlignSelf */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<AlignItems>(&v)) rs.Layout.AlignSelf = *p;
    },
    /* AlignContent */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<AlignContent>(&v)) rs.Layout.AlignContent = *p;
    },
    /* Direction */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<Direction>(&v)) rs.Layout.Direction = *p;
    },
    /* Position */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<PositionType>(&v)) rs.Layout.PositionType = *p;
    },
    /* PositionLeft */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<StyleLength>(&v)) rs.Layout.PositionLeft = *p;
    },
    /* PositionTop */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<StyleLength>(&v)) rs.Layout.PositionTop = *p;
    },
    /* PositionRight */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<StyleLength>(&v)) rs.Layout.PositionRight = *p;
    },
    /* PositionBottom */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<StyleLength>(&v)) rs.Layout.PositionBottom = *p;
    },
    /* Order */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<int>(&v)) rs.Layout.Order = *p;
    },
    /* ZIndex */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<int>(&v)) rs.Layout.ZIndex = *p;
    },
    /* FlexGrow */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<float>(&v)) rs.Layout.FlexGrow = *p;
    },
    /* FlexShrink */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<float>(&v)) rs.Layout.FlexShrink = *p;
    },
    /* FlexBasis */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<StyleLength>(&v)) rs.Layout.FlexBasis = *p;
    },
    /* Visibility */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<bool>(&v))
        {
            rs.Visual.Visible = *p;
            rs.Visual.HasVisibility = true;
        }
    },
    /* PointerEvents */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<bool>(&v))
        {
            rs.Visual.PointerEvents = *p;
            rs.Visual.HasPointerEvents = true;
        }
    },
    /* Opacity */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<float>(&v)) rs.Visual.LocalOpacity = *p;
    },
    /* Overflow */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<Overflow>(&v)) rs.Layout.Overflow = *p;
    },
    /* OverflowX */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<Overflow>(&v)) rs.Layout.OverflowX = *p;
    },
    /* OverflowY */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<Overflow>(&v)) rs.Layout.OverflowY = *p;
    },
    /* Cursor */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<CursorStyle>(&v)) { rs.Visual.Cursor = *p; rs.Visual.HasCursor = true; }
    },
    /* TextAlign */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<TextAlign>(&v)) { rs.Visual.TextAlign = *p; rs.Visual.HasTextAlign = true; }
    },
    /* WordBreak */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<WordBreak>(&v)) { rs.Visual.WordBreak = *p; rs.Visual.HasWordBreak = true; }
    },
    /* OverflowWrap */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<OverflowWrap>(&v)) { rs.Visual.OverflowWrap = *p; rs.Visual.HasOverflowWrap = true; }
    },
    /* WhiteSpace */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<WhiteSpace>(&v)) { rs.Visual.WhiteSpace = *p; rs.Visual.HasWhiteSpace = true; }
    },
    /* TextOverflow */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<TextOverflowMode>(&v)) rs.Visual.TextOverflow = *p;
    },
    /* FontSize */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<StyleLength>(&v)) SetFontSize(rs.Visual, *p);
    },
    /* LineHeight */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<float>(&v)) { rs.Visual.LineHeight = *p; rs.Visual.HasLineHeight = true; }
    },
    /* LetterSpacing */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<float>(&v)) { rs.Visual.LetterSpacing = *p; rs.Visual.HasLetterSpacing = true; }
    },
    /* FontFamily */ ApplyFontFamily,
    /* FontWeight */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<int>(&v)) { rs.Visual.FontWeight = *p; rs.Visual.HasFontWeight = true; }
    },
    /* FontStyle */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<FontStyle>(&v)) { rs.Visual.FontStyle = *p; rs.Visual.HasFontStyle = true; }
    },
    /* FontVariant */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<FontVariant>(&v)) { rs.Visual.FontVariant = *p; rs.Visual.HasFontVariant = true; }
    },
    /* Color */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<uint32_t>(&v)) { rs.Visual.Color = *p; rs.Visual.HasColor = true; }
    },
    /* BackgroundColor */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<uint32_t>(&v)) rs.Visual.BackgroundColor = *p;
    },
    /* BackgroundImage */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<BackgroundImageSource>(&v)) {
            rs.Visual.BackgroundImage.Source = *p;
            rs.Visual.BackgroundImage.HasImage = p->HasImage();
        }
    },
    /* BackgroundSize */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<BackgroundSizeValue>(&v)) {
            rs.Visual.BackgroundImage.SizeMode = p->Mode;
            rs.Visual.BackgroundImage.SizeX = p->SizeX;
            rs.Visual.BackgroundImage.SizeXIsPercent = p->SizeXIsPercent;
            rs.Visual.BackgroundImage.SizeY = p->SizeY;
            rs.Visual.BackgroundImage.SizeYIsPercent = p->SizeYIsPercent;
        }
    },
    /* BackgroundPosition */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<BackgroundPositionValue>(&v)) {
            rs.Visual.BackgroundImage.PosX = p->X;
            rs.Visual.BackgroundImage.PosXIsPercent = p->XIsPercent;
            rs.Visual.BackgroundImage.PosY = p->Y;
            rs.Visual.BackgroundImage.PosYIsPercent = p->YIsPercent;
        }
    },
    /* BackgroundRepeat */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<BackgroundRepeat>(&v)) rs.Visual.BackgroundImage.Repeat = *p;
    },
    /* BackgroundTint */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<uint32_t>(&v)) {
            rs.Visual.BackgroundImage.HasTint = true;
            rs.Visual.BackgroundImage.Tint = *p;
        }
    },
    /* BackgroundImageSaturation */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<float>(&v)) {
            float s = *p;
            if (s < 0.0f) s = 0.0f;
            if (s > 1.0f) s = 1.0f;
            rs.Visual.BackgroundImage.Saturation = s;
        }
    },
    /* BorderColor */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<BorderColorsTRBL>(&v)) rs.Visual.BorderColor = *p;
    },
    /* BorderTopColor */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<uint32_t>(&v)) rs.Visual.BorderColor.Top = *p;
    },
    /* BorderRightColor */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<uint32_t>(&v)) rs.Visual.BorderColor.Right = *p;
    },
    /* BorderBottomColor */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<uint32_t>(&v)) rs.Visual.BorderColor.Bottom = *p;
    },
    /* BorderLeftColor */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<uint32_t>(&v)) rs.Visual.BorderColor.Left = *p;
    },
    /* BorderWidth */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<Box4>(&v)) rs.Layout.BorderWidth = *p;
    },
    /* BorderTopWidth */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<float>(&v)) rs.Layout.BorderWidth.Top = *p;
    },
    /* BorderRightWidth */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<float>(&v)) rs.Layout.BorderWidth.Right = *p;
    },
    /* BorderBottomWidth */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<float>(&v)) rs.Layout.BorderWidth.Bottom = *p;
    },
    /* BorderLeftWidth */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<float>(&v)) rs.Layout.BorderWidth.Left = *p;
    },
    /* BorderStyle */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<BorderStyle>(&v)) rs.Visual.BorderStyle = *p;
    },
    // The shorthand is pixels-only, so it clears the per-corner percentage flags
    // an earlier longhand may have raised — both outputs are written every time,
    // or a stale flag would reinterpret the new pixel value as a percentage.
    /* BorderRadius */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<CornerRadiiTLTRBRBL>(&v)) {
            rs.Visual.BorderRadius = *p;
            rs.Visual.BorderRadiusIsPercent = {};
        }
    },
    /* BorderTopLeftRadius */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<CornerRadiusValue>(&v)) SetBorderRadiusCorner(
            rs.Visual.BorderRadius.TopLeft, rs.Visual.BorderRadiusIsPercent.TopLeft, *p);
    },
    /* BorderTopRightRadius */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<CornerRadiusValue>(&v)) SetBorderRadiusCorner(
            rs.Visual.BorderRadius.TopRight, rs.Visual.BorderRadiusIsPercent.TopRight, *p);
    },
    /* BorderBottomRightRadius */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<CornerRadiusValue>(&v)) SetBorderRadiusCorner(
            rs.Visual.BorderRadius.BottomRight, rs.Visual.BorderRadiusIsPercent.BottomRight, *p);
    },
    /* BorderBottomLeftRadius */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<CornerRadiusValue>(&v)) SetBorderRadiusCorner(
            rs.Visual.BorderRadius.BottomLeft, rs.Visual.BorderRadiusIsPercent.BottomLeft, *p);
    },
    /* Margin (shorthand) */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<StyleBox>(&v)) {
            rs.Layout.Margin = p->Values;
            rs.Layout.MarginIsPercent = p->IsPercent;
            rs.Layout.MarginIsAuto = p->IsAuto;
        }
    },
    /* MarginTop */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<StyleLength>(&v))
            SetMarginEdge(rs.Layout.Margin.Top, rs.Layout.MarginIsPercent.Top, rs.Layout.MarginIsAuto.Top, *p);
    },
    /* MarginRight */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<StyleLength>(&v))
            SetMarginEdge(rs.Layout.Margin.Right, rs.Layout.MarginIsPercent.Right, rs.Layout.MarginIsAuto.Right, *p);
    },
    /* MarginBottom */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<StyleLength>(&v))
            SetMarginEdge(rs.Layout.Margin.Bottom, rs.Layout.MarginIsPercent.Bottom, rs.Layout.MarginIsAuto.Bottom, *p);
    },
    /* MarginLeft */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<StyleLength>(&v))
            SetMarginEdge(rs.Layout.Margin.Left, rs.Layout.MarginIsPercent.Left, rs.Layout.MarginIsAuto.Left, *p);
    },
    /* Padding (shorthand) */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<StyleBox>(&v)) {
            rs.Layout.Padding.Top = p->Values.Top; rs.Layout.PaddingIsPercent.Top = p->IsPercent.Top;
            rs.Layout.Padding.Right = p->Values.Right; rs.Layout.PaddingIsPercent.Right = p->IsPercent.Right;
            rs.Layout.Padding.Bottom = p->Values.Bottom; rs.Layout.PaddingIsPercent.Bottom = p->IsPercent.Bottom;
            rs.Layout.Padding.Left = p->Values.Left; rs.Layout.PaddingIsPercent.Left = p->IsPercent.Left;
        }
    },
    /* PaddingTop */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<StyleLength>(&v)) { rs.Layout.Padding.Top = p->Value; rs.Layout.PaddingIsPercent.Top = p->IsPercent(); }
    },
    /* PaddingRight */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<StyleLength>(&v)) { rs.Layout.Padding.Right = p->Value; rs.Layout.PaddingIsPercent.Right = p->IsPercent(); }
    },
    /* PaddingBottom */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<StyleLength>(&v)) { rs.Layout.Padding.Bottom = p->Value; rs.Layout.PaddingIsPercent.Bottom = p->IsPercent(); }
    },
    /* PaddingLeft */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<StyleLength>(&v)) { rs.Layout.Padding.Left = p->Value; rs.Layout.PaddingIsPercent.Left = p->IsPercent(); }
    },
    /* Gap */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<StyleLength>(&v))
        {
            SetGapGutter(rs.Layout.Gap, rs.Layout.GapIsPercent, *p);
            SetGapGutter(rs.Layout.RowGap, rs.Layout.RowGapIsPercent, *p);
            SetGapGutter(rs.Layout.ColumnGap, rs.Layout.ColumnGapIsPercent, *p);
        }
    },
    /* RowGap */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<StyleLength>(&v)) SetGapGutter(rs.Layout.RowGap, rs.Layout.RowGapIsPercent, *p);
    },
    /* ColumnGap */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<StyleLength>(&v)) SetGapGutter(rs.Layout.ColumnGap, rs.Layout.ColumnGapIsPercent, *p);
    },
    /* Width */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<StyleLength>(&v)) rs.Layout.Width = *p;
    },
    /* Height */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<StyleLength>(&v)) rs.Layout.Height = *p;
    },
    /* MinWidth */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<StyleLength>(&v)) rs.Layout.MinWidth = *p;
    },
    /* MinHeight */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<StyleLength>(&v)) rs.Layout.MinHeight = *p;
    },
    /* MaxWidth */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<StyleLength>(&v)) rs.Layout.MaxWidth = *p;
    },
    /* MaxHeight */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<StyleLength>(&v)) rs.Layout.MaxHeight = *p;
    },
    /* AspectRatio */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<float>(&v)) rs.Layout.AspectRatio = *p;
    },
    /* BoxShadow */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<BoxShadowValue>(&v)) {
            rs.Visual.ShadowOffsetX = p->OffsetX;
            rs.Visual.ShadowOffsetY = p->OffsetY;
            rs.Visual.ShadowSoftness = p->Blur;
            rs.Visual.ShadowColor = p->Color;
            rs.Visual.ShadowInset = p->Inset;
        }
    },
    /* Glow */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<GlowValue>(&v)) {
            rs.Visual.GlowRadius = p->Radius;
            rs.Visual.GlowColor = p->Color;
        }
    },
    /* Transition   */ nullptr,
    /* CustomVar    */ nullptr,
    /* BorderImageSlice */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<BorderImageSliceValue>(&v)) {
            rs.Visual.BackgroundImage.HasBorderImage = true;
            rs.Visual.BackgroundImage.BiSlice[0] = p->Top;
            rs.Visual.BackgroundImage.BiSlice[1] = p->Right;
            rs.Visual.BackgroundImage.BiSlice[2] = p->Bottom;
            rs.Visual.BackgroundImage.BiSlice[3] = p->Left;
            rs.Visual.BackgroundImage.BiSliceFill = p->Fill;
        }
    },
    /* BorderImageRepeat */ [](ResolvedStyle& rs, const StyleValue& v) {
        // Only border-image-slice enables the override (it supplies the insets the
        // override needs). repeat alone refines a slice that is also present; on its
        // own it is a no-op so it can't clobber a texture's intrinsic slice.
        if (auto* p = std::get_if<BorderImageRepeatValue>(&v)) {
            rs.Visual.BackgroundImage.BiRepeatX = p->X;
            rs.Visual.BackgroundImage.BiRepeatY = p->Y;
        }
    },
    /* OutlineWidth */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<float>(&v)) rs.Visual.OutlineWidth = (*p < 0.0f) ? 0.0f : *p;
    },
    /* OutlineStyle */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<BorderStyle>(&v)) rs.Visual.OutlineStyle = *p;
    },
    /* OutlineColor */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<uint32_t>(&v)) {
            rs.Visual.OutlineColor = *p;
            rs.Visual.HasOutlineColor = true;
        }
    },
    /* OutlineOffset */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<float>(&v)) rs.Visual.OutlineOffset = *p;
    },
    /* TextShadowOffsetX */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<float>(&v)) rs.Visual.TextEffects.ShadowOffsetX = *p;
    },
    /* TextShadowOffsetY */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<float>(&v)) rs.Visual.TextEffects.ShadowOffsetY = *p;
    },
    /* TextShadowBlur */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<float>(&v)) rs.Visual.TextEffects.ShadowBlur = *p;
    },
    /* TextShadowColor */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<uint32_t>(&v)) rs.Visual.TextEffects.ShadowColor = *p;
    },
    /* TextGlowRadius */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<float>(&v)) rs.Visual.TextEffects.GlowRadius = *p;
    },
    /* TextGlowColor */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<uint32_t>(&v)) rs.Visual.TextEffects.GlowColor = *p;
    },
    /* TextOutlineWidth */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<float>(&v)) rs.Visual.TextEffects.OutlineWidth = *p;
    },
    /* TextOutlineColor */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<uint32_t>(&v)) rs.Visual.TextEffects.OutlineColor = *p;
    },
    /* SelectionColor */ [](ResolvedStyle& rs, const StyleValue& v) {
        if (auto* p = std::get_if<uint32_t>(&v)) rs.Visual.SelectionColor = *p;
    },
    /* DeferredDecl */ nullptr,
};


static_assert(std::size(kAppliers) == static_cast<size_t>(StylePropertyId::_Count),
              "kAppliers needs exactly one entry per StylePropertyId, in enum order");

void ApplyOverridesToResolvedStyle(ResolvedStyle& rs, const StyleOverrides& overrides)
{
    if (overrides.PropCount() == 0)
        return;

    overrides.ForEachProp([&](StylePropertyId id, const StyleOverrideEntry& entry)
    {
        if (entry.keyword != StyleKeyword::None)
            return;

        auto idx = static_cast<size_t>(id);
        if (idx < std::size(kAppliers) && kAppliers[idx])
            kAppliers[idx](rs, entry.value);
    });

    rs.Visual.Opacity = rs.Visual.LocalOpacity;
}

void ResolveUsedBorderWidths(ResolvedStyle& rs)
{
    if (rs.Visual.BorderStyle != BorderStyle::None)
        return;
    rs.Layout.BorderWidth = Box4{};
}

} // namespace GameEngine
