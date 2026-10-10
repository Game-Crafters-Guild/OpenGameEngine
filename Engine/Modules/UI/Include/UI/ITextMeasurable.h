#pragma once

#include <memory>
#include <string>

namespace GameEngine {

struct ResolvedStyle;

// Interface implemented by controls that participate in intrinsic text
// measurement and rendering (Label, TextInput, TextArea). Subclasses set
// UIElement::m_TextMeasurable in their constructor; UIManager queries it
// via GetTextMeasurable() instead of dynamic_cast.
// Also owns the per-element text shaping cache so non-text elements
// pay zero cost.
class ITextMeasurable
{
public:
    struct TextMeasureInfo
    {
        bool        HasText         = false;
        std::string Text;
        float       ExplicitFontSize = 0.0f;
    };

    ITextMeasurable();
    virtual ~ITextMeasurable();
    virtual void GetTextMeasureInfo(TextMeasureInfo& info,
                                    const ResolvedStyle& style) const = 0;
    virtual bool AllowWrapForMeasure() const { return false; }

    // Controls that render their own text (e.g. TextArea with per-line
    // caching) return true to skip the generic EmitTextPrimitives path.
    virtual bool HandlesOwnTextRendering() const { return false; }

    // Per-element text shaping cache (Pimpl). Lazy-allocated on first use.
    struct TextShapeCache;
    TextShapeCache& GetOrCreateTextShapeCache() const;

    // Discard cached glyph placements (e.g. when the owning UIManager changes
    // and the FontAtlas/texture registry is no longer valid).
    void ResetTextShapeCache() const;

    // Whether the most recent emission truncated this element's run via
    // text-overflow: ellipsis. The tooltip overlay treats a truncated label
    // as its own tooltip, so hover reveals the full text.
    bool WasLastRunEllipsized() const;

private:
    mutable std::unique_ptr<TextShapeCache> m_TextShapeCache;
};

} // namespace GameEngine
