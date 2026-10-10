#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include "UI/ModuleOwnedCallback.h"
#include "UI/UIElement.h"

namespace GameEngine {

class Label;

class Button : public UIElement {
public:
    Button();
    ~Button() override = default;

    // Text content API
    void SetText(const std::string& t);
    const std::string& GetText() const { return m_Text; }
    // The width, in logical px, the button needs to show `text` whole: the text measured in
    // its label's face plus the button's padding and border. 0 before the label has been
    // laid out as text.
    float WidthForText(std::string_view text) const;

    // Event handling
    void OnEvent(UIEvent& e) override;

    using ClickHandler = std::function<void(Button&)>;
    void SetOnMouseDown(ClickHandler handler);

    // The one click source. An armed left-release inside the button, a Space/Enter
    // activation while focused, and a programmatic caller all land here, and all of
    // them reach subscribers as kEventButtonClick carrying the modifier bitmask
    // (Input::kModShift, etc.) in UIEvent::Mods. Subscribe with RegisterEventHandler,
    // or with SetOnClick below when one callback per button is all you need.
    void TriggerClick(int mods = 0);

    // Convenience over the handler table: one click callback, set the familiar way.
    // It registers an ordinary kEventButtonClick subscription and keeps its token —
    // there is no member dispatch path and no ordering privilege, so a callback set
    // here fires in registration order among the table's subscribers like any other.
    //
    // SETTER semantics, which is the whole difference from RegisterEventHandler: this
    // is ONE slot per button. A second SetOnClick unregisters the first, and
    // SetOnClick(nullptr) unregisters and leaves nothing. It only ever revokes the
    // subscription THIS API minted; registrations made directly through
    // RegisterEventHandler are untouched, so the table's additivity still holds for
    // every other subscriber.
    //
    // That single slot is the trap, and it is why this is convenience rather than the
    // primitive: two unrelated owners both reaching for SetOnClick on one button means
    // the second silently replaces the first. Anything that must coexist with another
    // subscriber calls RegisterEventHandler, which hands each caller a token it revokes
    // independently.
    //
    // The handler takes the whole event, so there is one signature instead of one per
    // payload — read e.Mods for the modifier bitmask, e.Target for the button, or
    // ignore the parameter entirely. It does NOT carry a pointer position: TriggerClick
    // leaves UIEvent::X/Y at zero, and keyboard activation has no position to give.
    //
    // For new code. The call sites migrated to RegisterEventHandler stay there; this
    // exists so future code wanting a single click callback need not spell out a
    // registration it will never revoke.
    void SetOnClick(EventHandler handler);

private:
    friend struct UIEventHandlerAccess;

    void EnsureLabel();
    bool IsPointInside(float x, float y) const;

    // Slot bit for the one member callback a native user module can own. A left click
    // arrives as kEventButtonClick, which the handler table already stamps and revokes;
    // the PRESS that precedes it has no event of its own, so this slot has no
    // handler-table equivalent and needs the member-slot revocation path below.
    static constexpr std::uint32_t kOnMouseDownSlot = 1u << 0;

    // Every slot, listed once. The four revocation virtuals below all walk this,
    // so a slot cannot be covered by some of them and missed by others. One entry
    // today; the walk is what keeps a second one from being covered unevenly.
    template <typename Fn>
    void ForEachOwnedSlot(Fn&& fn);
    template <typename Fn>
    void ForEachOwnedSlot(Fn&& fn) const;

    std::size_t CountMemberSlotsOwnedByImage(std::uint64_t base, std::uint64_t size) const override;
    void CollectMemberSlotsOwnedByImage(std::uint64_t base, std::uint64_t size,
                                        std::uint32_t& slotMask) override;
    std::size_t ReleaseCollectedMemberSlots(std::uint32_t slotMask) override;
    std::size_t DropMemberSlotStampsOutsideMappedImages() override;

    UI::ModuleOwnedCallback<void(Button&)> m_OnMouseDown{this, kOnMouseDownSlot};
    // SetOnClick's subscription. Deliberately NOT a stamped slot and not listed in
    // ForEachOwnedSlot: a token is two integers, never a callable, so it holds no
    // module code for an unmap to strand. The callback itself lives in the handler
    // table, which already stamps and revokes it — the wrapper hands it there
    // unwrapped precisely so the stamp resolves to the CALLER's image rather than to
    // Engine.dll (a nesting wrapper would hide it; see AdoptHandlerOwnerFrom).
    //
    // Which leaves this token stale after a revocation sweep, and that is safe by
    // construction rather than by care: keys come from a per-element monotonic counter
    // and are never reused, so re-presenting a swept token can only miss. It matches
    // the emptied entry until the outermost frame's drain erases it, and misses after —
    // both harmless, and ClearOwnerStamp is idempotent, so neither double-counts.
    // The token also never travels: it is stored on the element that minted it, which
    // is what makes the cross-element key ambiguity (UIElement.h, m_NextHandlerKey)
    // unreachable from here.
    EventHandlerToken m_OnClickToken{};
    std::string m_Text;
    Label* m_LabelChild = nullptr; // owned by UIElement children vector

    bool m_Armed = false; // true if mouse down started on this element
};

} // namespace GameEngine
