#include "UI/Controls/Button.h"

#include "Input/KeyCodes.h"
#include "UI/Controls/Label.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/ModuleOwnedHandlers.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"

#include <utility>

namespace GameEngine
{
float Button::WidthForText(std::string_view text) const
{
    UIManager* ui = GetOwnerManager();
    if (!ui || !m_LabelChild)
        return 0.0f;
    const float measured = ui->MeasureTextWidth(*m_LabelChild, text);
    if (measured <= 0.0f)
        return 0.0f;
    const auto& labelLayout = m_LabelChild->GetResolvedStyle().Layout;
    return measured + UI::Layout::HorizontalInsetPx(*this) + UI::Layout::HorizontalInsetPx(*m_LabelChild) +
           labelLayout.Margin.Left + labelLayout.Margin.Right;
}

Button::Button()
{
    // Control styling is an Editor-shipped UIStyle asset.
    AddClass("button");
    RequestSubtreeStyleAssetPath("UI/controls/Button.css", "editor");

    // Buttons are focusable by default so they can receive keyboard events
    // (e.g., Space/Enter activation) without UIManager having to special-case
    // their type.
    SetFocusable(true);
    // Create a child label to present the text visually
    EnsureLabel();
}

void Button::EnsureLabel()
{
    if (m_LabelChild)
        return;
    auto lbl = std::make_unique<Label>();
    m_LabelChild = lbl.get();
    // Give the label a helpful class for styling if desired
    m_LabelChild->AddClass("button-text");
    AddChild(std::move(lbl));
}

void Button::SetText(const std::string& t)
{
    m_Text = t;
    EnsureLabel();
    m_LabelChild->SetText(m_Text);
    // Layout may change based on text size
    MarkDirty(LayoutDirty | VisualDirty);
}

void Button::SetOnMouseDown(ClickHandler handler)
{
    m_OnMouseDown = std::move(handler);
}

void Button::SetOnClick(EventHandler handler)
{
    // Revoke first, so a handler replacing itself from inside its own dispatch still
    // ends with exactly one subscription. UnregisterEventHandler keeps an executing
    // callable alive and lets DrainInactiveHandlers erase it once the outermost frame
    // unwinds, which is what makes
    // that case safe rather than a free-under-own-frame.
    if (m_OnClickToken)
        UnregisterEventHandler(m_OnClickToken);
    m_OnClickToken = {};
    // Passed through unwrapped: the ownership stamp reads the callable's own
    // target_type, so wrapping it here would attribute a user module's callback to
    // Engine.dll and strand it at unmap.
    if (handler)
        m_OnClickToken = RegisterEventHandler(kEventButtonClick, std::move(handler));
}

void Button::TriggerClick(int mods)
{
    // Clicks are handler-table subscriptions only: additive, keyed and individually
    // revocable, so a second listener (the scripting ABI, a panel, a test) can never
    // destroy a registration it did not make. The one narrowing is SetOnClick, whose
    // single slot replaces its OWN previous subscription and nothing else.
    // Not bubbled — dispatched on this button alone.
    UIEvent e{};
    e.Id = kEventButtonClick;
    e.Target = this;
    e.CurrentTarget = this;
    e.Mods = mods;
    // Compile-time id: with nothing subscribed this is one bit test, no map probe.
    DispatchEvent<kEventButtonClick>(e);
}

bool Button::IsPointInside(float x, float y) const
{
    float lx = GetLayoutX();
    float ly = GetLayoutY();
    float w = GetLayoutWidth();
    float h = GetLayoutHeight();
    return (x >= lx && y >= ly && x < (lx + w) && y < (ly + h));
}

void Button::OnEvent(UIEvent& e)
{
    if (e.Id == kEventMouseDown)
    {
        // Left-click.
        if (e.Button == 0)
        {
            m_Armed = true;
            AddClass("pressed");
            if (m_OnMouseDown)
                m_OnMouseDown(*this);
            e.Capture(this); // ensure we receive MouseUp even if pointer leaves
            e.Stop();
            return;
        }

        // A right press is not a button gesture and Button does not claim one: it bubbles,
        // so a container's context menu still sees it. An element that wants its own menu
        // attaches a ContextMenuManipulator, which arms and claims it there.
    }
    else if (e.Id == kEventMouseMove)
    {
        if (m_Armed)
        {
            // Update pressed visuals based on pointer position
            if (IsPointInside(e.X, e.Y))
            {
                if (!HasClass("pressed"))
                {
                    AddClass("pressed");
                }
            }
            else
            {
                if (HasClass("pressed"))
                {
                    RemoveClass("pressed");
                }
            }
            e.Stop();
        }
    }
    else if (e.Id == kEventMouseCancel)
    {
        // The press ended without a release: the pointer left the surface still holding
        // the button. Disarm and drop the visual, activate nothing. Left unstopped so
        // ancestors tracking the same gesture are cancelled too.
        if (HasClass("pressed"))
            RemoveClass("pressed");
        m_Armed = false;
    }
    else if (e.Id == kEventMouseUp)
    {
        if (m_Armed && e.Button == 0)
        {
            bool inside = IsPointInside(e.X, e.Y);
            if (HasClass("pressed"))
                RemoveClass("pressed");
            if (inside)
            {
                TriggerClick(e.Mods);
            }
            e.Stop();
        }

        if (e.Button == 0)
            m_Armed = false;
    }
    else if (e.Id == kEventKeyDown)
    {
        // Keyboard activation: when the button is focused, Space/Enter should
        // trigger a click using the same event pipeline as mouse input.
        if (e.Key == Input::kKeyCode_Space || e.Key == Input::kKeyCode_Enter)
        {
            TriggerClick(e.Mods);
            e.Stop();
        }
    }
}

template <typename Fn>
void Button::ForEachOwnedSlot(Fn&& fn)
{
    fn(m_OnMouseDown);
}

template <typename Fn>
void Button::ForEachOwnedSlot(Fn&& fn) const
{
    fn(m_OnMouseDown);
}

std::size_t Button::CountMemberSlotsOwnedByImage(std::uint64_t base, std::uint64_t size) const
{
    std::size_t n = 0;
    ForEachOwnedSlot([&](const auto& slot) {
        if (slot.OwnedByImage(base, size))
            ++n;
    });
    return n;
}

void Button::CollectMemberSlotsOwnedByImage(std::uint64_t base, std::uint64_t size,
                                            std::uint32_t& slotMask)
{
    ForEachOwnedSlot([&](auto& slot) {
        if (!slot.OwnedByImage(base, size))
            return;
        // A slot running its callable keeps it: the quiesce ledger then still
        // counts it, refuses the unmap, and the frame returns into a mapped
        // image. Same rule the handler table follows.
        if (slot.IsExecuting())
            return;
        slotMask |= slot.SlotBit();
    });
}

std::size_t Button::ReleaseCollectedMemberSlots(std::uint32_t slotMask)
{
    std::size_t revoked = 0;
    // Declared before the walk so it outlives it: a released callable's captures
    // are module code, and that code can clear the other slot or destroy this
    // button. Nothing holds a slot — or `this` — when the sink drops them.
    UI::RevokedCallableSink doomed;
    // Re-checked rather than assumed: a release can still run module code in place
    // (the residual on ModuleOwnedCallback::Release), and that code can re-enter
    // and clear the second slot. Counting a slot that is already gone would
    // inflate the number the unload log reports.
    ForEachOwnedSlot([&](auto& slot) {
        if ((slotMask & slot.SlotBit()) == 0)
            return;
        if (slot.Release(doomed))
            ++revoked;
    });
    return revoked;
}

std::size_t Button::DropMemberSlotStampsOutsideMappedImages()
{
    std::size_t dropped = 0;
    ForEachOwnedSlot([&](auto& slot) {
        if (slot.DropStampIfUnmapped())
            ++dropped;
    });
    return dropped;
}

} // namespace GameEngine
