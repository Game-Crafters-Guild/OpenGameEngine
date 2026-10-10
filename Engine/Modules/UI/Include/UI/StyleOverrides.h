#pragma once

#include "Types/FlatMap.h"
#include "Types/StringId.h"
#include "UI/StyleProp.h"
#include "UI/UIStyle.h"

#include <optional>
#include <string>
#include <variant>

namespace GameEngine
{

struct StyleOverrideEntry
{
    StyleKeyword keyword = StyleKeyword::None;
    StyleValue value{};
};

// Sparse per-element style overrides using typed property keys.
// Uses FlatMap for O(log N) lookup with contiguous memory (typically <10 entries).
class StyleOverrides
{
  public:
    // Typed set: compile-time type safety via StyleProp<T>. Impact comes from
    // the canonical classification via StyleProp::Impact().
    template <typename T>
    StyleOverrides& Set(StyleProp<T> prop, const T& value)
    {
        SetImpl(prop.id, StyleOverrideEntry{StyleKeyword::None, StyleValue{value}}, prop.Impact());
        return *this;
    }

    // Set a CSS-wide keyword (inherit, initial, unset)
    template <typename T>
    StyleOverrides& SetKeyword(StyleProp<T> prop, StyleKeyword kw)
    {
        SetImpl(prop.id, StyleOverrideEntry{kw, {}}, prop.Impact());
        return *this;
    }

    // Reset (remove) a property override. Signals that the CSS cascade must
    // re-run to recover the base value for the removed property.
    template <typename T>
    StyleOverrides& Reset(StyleProp<T> prop)
    {
        if (m_Props.Erase(prop.id))
        {
            const bool layout = (prop.Impact() == StyleImpact::Layout);
            if (layout)
                m_LayoutDirty = true;
            else
                m_VisualDirty = true;
            m_NeedsCascadeRerun = true;
            NotifyDirty((layout ? Layout : Visual) | Cascade);
        }
        return *this;
    }

    // Typed get: returns std::optional<T>
    template <typename T>
    std::optional<T> Get(StyleProp<T> prop) const
    {
        if (const auto* entry = m_Props.Find(prop.id))
        {
            if (auto* val = std::get_if<T>(&entry->value))
                return *val;
        }
        return std::nullopt;
    }

    // Check if a property is overridden
    bool Has(StylePropertyId id) const { return m_Props.Contains(id); }

    // Dirty tracking
    bool IsLayoutDirty() const { return m_LayoutDirty; }
    bool IsVisualDirty() const { return m_VisualDirty; }
    bool NeedsCascadeRerun() const { return m_NeedsCascadeRerun; }
    void ClearDirty()
    {
        m_LayoutDirty = false;
        m_VisualDirty = false;
        m_NeedsCascadeRerun = false;
    }
    bool IsEmpty() const { return m_Props.Empty() && m_CustomVars.Empty(); }

    // Position-only fast path for virtualized recycling: updates the
    // PositionLeft/PositionTop entries WITHOUT raising layout dirt or a
    // cascade re-run. Callers must ensure the element is already
    // position:absolute with an explicit px size — committed rects are
    // patched by ApplyLayoutOverrideRects post-solve and primitives follow
    // via the drain, so Yoga participation is unnecessary for a pure move.
    void SetPositionOnlyPx(float left, float top);

    // Custom properties (CSS variables)
    void SetCustom(StringId name, std::string value);
    void SetCustomNumber(StringId name, float value);
    void SetCustomColor(StringId name, uint32_t argb);
    void ClearCustom(StringId name);
    const std::string* FindCustom(StringId name) const;

    // Iteration (for cascade resolver)
    template <typename Fn>
    void ForEachProp(Fn&& fn) const
    {
        m_Props.ForEach(std::forward<Fn>(fn));
    }
    template <typename Fn>
    void ForEachCustom(Fn&& fn) const
    {
        m_CustomVars.ForEach(std::forward<Fn>(fn));
    }

    size_t PropCount() const { return m_Props.Size(); }
    size_t CustomCount() const { return m_CustomVars.Size(); }

    void Clear()
    {
        const bool hadProps = !m_Props.Empty() || !m_CustomVars.Empty();
        m_Props.Clear();
        m_CustomVars.Clear();
        // If we actually removed overrides, the cascade must re-run so the
        // element falls back to its CSS base values. Without this, layout
        // stays stale (e.g. an overlay with a cleared Display:Flex override
        // remains laid out as flex even though CSS says display:none).
        if (hadProps) { NotifyDirty(Layout | Visual | Cascade); }
        if (hadProps)
        {
            m_LayoutDirty = true;
            m_VisualDirty = true;
            m_NeedsCascadeRerun = true;
        }
    }

    // Untyped set by property ID (for CSS parser integration where type is
    // already validated at parse time).
    void SetById(StylePropertyId id, const StyleValue& value, StyleImpact impact);
    void SetKeywordById(StylePropertyId id, StyleKeyword kw, StyleImpact impact);

    // Dirty-propagation callback (Slice 2 follow-up): previously override
    // mutations set m_LayoutDirty/m_VisualDirty/m_NeedsCascadeRerun internally
    // without routing through UIElement::MarkDirty, which meant SubtreeDirty
    // wasn't propagated up the ancestor chain and the fast path had to abort
    // whenever any descendant had override-dirty bits. UIElement now wires a
    // callback here on construction; every mutator fires it with the relevant
    // flag bits so MarkDirty runs with proper ancestor walk.
    enum DirtyBit : uint32_t
    {
        Layout  = 1u << 0,
        Visual  = 1u << 1,
        Cascade = 1u << 2,
    };
    using DirtyCallback = void(*)(void* ctx, uint32_t flags);
    void SetDirtyCallback(DirtyCallback cb, void* ctx)
    {
        m_DirtyCb = cb;
        m_DirtyCtx = ctx;
    }

  private:
    FlatMap<StylePropertyId, StyleOverrideEntry> m_Props;
    FlatMap<StringId, std::string> m_CustomVars;

    bool m_LayoutDirty = false;
    bool m_VisualDirty = false;
    bool m_NeedsCascadeRerun = false;

    DirtyCallback m_DirtyCb = nullptr;
    void*         m_DirtyCtx = nullptr;

    void NotifyDirty(uint32_t flags)
    {
        if (m_DirtyCb)
            m_DirtyCb(m_DirtyCtx, flags);
    }

    void SetImpl(StylePropertyId id, StyleOverrideEntry entry, StyleImpact impact);
};

} // namespace GameEngine
