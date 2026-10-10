#pragma once

#include <cstdint>
#include <string>
#include <functional>
#include <type_traits>
#include <utility>

#include "UI/Controls/FieldEventValue.h"
#include "UI/Controls/ScopedValueText.h"
#include "UI/UIElement.h"
#include "UI/UIStyle.h"
#include "UI/UIPlatform.h"
#include "UI/UIEvents.h"
#include "UI/UIManager.h"
#include "UI/Registration/ElementRegistration.h"
#include "Rendering/Text/FontAtlas.h"
#include "Input/KeyCodes.h"

#ifdef _DEBUG
#include "Logger/Logger.h"
#endif

namespace GameEngine {

#if !defined(GE_BASEFIELD_HAS_EQUAL_TRAIT)
#define GE_BASEFIELD_HAS_EQUAL_TRAIT 1
namespace Detail
{
template <typename T, typename = void>
struct HasEqualOperator : std::false_type
{
};

template <typename T>
struct HasEqualOperator<T, std::void_t<decltype(std::declval<const T&>() == std::declval<const T&>())>> : std::true_type
{
};
} // namespace Detail
#endif

namespace UI::Detail
{
// The runaway report, out of line and out of the public surface: it is an implementation
// detail of Field<T>::NotifyValue, and keeping it here rather than in namespace UI stops it
// reading like something a control is meant to call. Out of line specifically so this header
// does not pull the logger into every control and every consumer of one. Defined in
// BaseField.cpp; visible here only because the caller is a header-inline template.
void ReportReentrantValueWrite(const UIElement& element);
} // namespace UI::Detail

#ifdef _DEBUG
// Opt-in console diagnostics for pointer routing on fields. Disabled by
// default to keep debug builds quiet; flip to true when investigating
// pointer issues.
//
// This is a runtime flag rather than constexpr so that "if (flag)" checks
// do not trigger MSVC's C4127 (conditional expression is constant) warnings
// in templated code.
static bool kFieldPointerDebugConsole = false;
#endif

		    // Generic typed field with value-changing and value-changed callbacks.
		    // BaseField remains available as an alias for Field<std::string>.
			template<typename T>
			class Field : public UIElement {
		public:
		    using ValueType     = T;
		    using ValueCallback = std::function<void(const T&)>;

		    Field() = default;
		
		    virtual ~Field() = default;
		
		    // Value API
		    virtual const T& GetValue() const { return m_Value; }
		    virtual void SetValue(const T& v)
		    {
		        if constexpr (Detail::HasEqualOperator<T>::value)
		        {
		            if (m_Value == v)
		                return;
		        }
		        m_Value = v;
		        InvalidateValue();
		    }
	
	    // Set the value and tell NOBODY: neither the m_On* member slots nor the handler
	    // table, so no member callback runs, no UI.ValueChanging/UI.ValueChanged is
	    // dispatched, and no script subscriber hears about it. The value, the dirty marking
	    // and every visual consequence are identical to SetValue; only the announcement is
	    // withheld. Same name and same contract as Unity's SetValueWithoutNotify, so the
	    // habit transfers.
	    //
	    // This is the API for a write that must not be mistaken for the user changing the
	    // control: driving a control from the state it reflects (a per-frame HUD refresh),
	    // seeding a field before showing it, and above all writing from inside a handler for
	    // this same control — where a plain SetValue would otherwise be notifying from
	    // inside a notification. Field<T>::NotifyValue downgrades such a nested write to
	    // exactly these semantics and says so once per element, but saying it directly is
	    // clearer than being corrected.
	    //
	    // Concrete controls override this to keep their own presentation in step (Slider
	    // re-lays its thumb, ToggleBase re-applies the `checked` class) while still
	    // notifying nobody.
	    virtual void SetValueWithoutNotify(const T& v)
	    {
	        if constexpr (Detail::HasEqualOperator<T>::value)
	        {
	            if (m_Value == v)
	                return;
	        }
	        m_Value = v;
	        InvalidateValue();
	    }

	    // Callbacks
	    void SetOnValueChanging(ValueCallback cb) { m_OnValueChanging = std::move(cb); }
	    void SetOnValueChanged(ValueCallback cb)  { m_OnValueChanged  = std::move(cb); }
	
	    // Focus updates from UI manager (delivered via UI.FocusIn/UI.FocusOut events)
	    virtual void OnFocusChanged(bool /*focused*/) {}

    // Keyboard/text input routed from UI manager. Both return whether the field
    // acted on the input: that answer is the event's Handled flag, so it stops
    // bubbling here and, while a game is running, keeps the keystroke away from
    // gameplay. Return false for anything the field ignores — a key claimed but
    // not acted on is a key the game silently never receives.
    virtual bool OnChar(unsigned int /*codepoint*/) { return false; }
    virtual bool OnKey(int /*key*/, int /*mods*/, UI::IPlatformApi* /*platform*/) { return false; }

		    // Pointer interactions for caret/selection, provided with box geometry and style
    virtual void OnPointerDown(float /*mouseX*/, float /*mouseY*/,
                               float /*x*/, float /*y*/, float /*W*/, float /*H*/,
                               const ResolvedStyle& /*style*/, Rendering::Text::FontAtlas* /*font*/) {}
    virtual void OnPointerDrag(float /*mouseX*/, float /*mouseY*/,
                               float /*x*/, float /*y*/, float /*W*/, float /*H*/,
                               const ResolvedStyle& /*style*/, Rendering::Text::FontAtlas* /*font*/) {}

		protected:
	    void InvalidateValue()
	    {
	        // Text only needs layout when it contributes an intrinsic size. A
	        // field with both dimensions set keeps the same box, so SetValue can
	        // update its primitives without running Yoga. Non-text fields do not
	        // derive geometry from the stored value.
	        if (GetTextMeasurable())
	        {
	            const ResolvedStyle& style = GetResolvedStyle();
	            const bool definiteSize =
	                !style.Layout.Width.IsAuto() && !style.Layout.Height.IsAuto() &&
	                GetLayoutWidth() > 0.0f && GetLayoutHeight() > 0.0f;
	            if (definiteSize)
	            {
	                MarkContentDirty();
	                return;
	            }
	            MarkDirty(LayoutDirty | VisualDirty);
	            return;
	        }

	        MarkDirty(VisualDirty);
	    }

    // Text-backed fields need a font atlas before pointer routing. Controls such
    // as Slider only use geometry — if this returns false, routing continues
    // when ResolveFontForStyle yields nullptr (e.g. minimal UI contexts).
    virtual bool RequiresFontAtlasForPointerRouting() const { return true; }

		    // Helper used by pointer event routing to compute a style with inherited
		    // text properties (color, font, size, lineHeight) that matches
		    // UIManager's draw-time pipeline. The goal is that whatever
		    // TextInput::OnGeneratePrimitives sees for fontSize/color is *identical* to
		    // what pointer hit-testing uses, so caret maps and overlay diagnostics
		    // stay in the same coordinate system.
		    //
		    // The padding is replaced with the USED value the layout solve
		    // resolved, so `padding: 10%` reaches the box-model math as the
		    // length it laid out as and not as a bare `10`. That is the whole
		    // contract of this copy: everything downstream of it treats
		    // Layout.Padding as a length, exactly as CSS getComputedStyle does.
		static ResolvedStyle ComputePointerStyle(UIManager*, const UIElement* element)
		{
		    if (!element)
		        return ResolvedStyle{};
		    ResolvedStyle style = element->GetResolvedStyle();
		    style.Layout.Padding = element->GetLayoutPadding();
		    style.Layout.PaddingIsPercent = {};
		    return style;
	    }

		    // Default event bridge: map generic focus and text/key events onto
	    // the existing hooks so derived fields (including TextInput/TextFieldBase)
	    // keep their behavior while UIManager remains type-agnostic. Pointer
	    // events are also translated into the existing pointer hooks using the
	    // element's layout rect and computed style.
		    void OnEvent(UIEvent& e) override
	    {
	        if (e.Id == kEventFocusIn)
	        {
	            OnFocusChanged(true);
	            return;
	        }
	        if (e.Id == kEventFocusOut)
	        {
	            OnFocusChanged(false);
	            return;
	        }
		        // Keyboard and text both belong to the focus target: OnChar is only
		        // ever delivered there, so a field claiming a key while merely hovered
		        // eats a keystroke it can never act on. The same gate is what stops a
		        // hovered field editing itself on Backspace and the arrow keys, which
		        // need no character at all.
		        if (e.Id == kEventTextInput || e.Id == kEventKeyDown)
		        {
		            UIManager* owner = GetOwnerManager();
		            if (!owner || !IsFocusTargetForId(owner->GetFocusedElementId()))
		                return;
		        }
		        if (e.Id == kEventTextInput)
		        {
		            if (OnChar(e.Codepoint))
		                e.Stop();
		            return;
		        }
		        if (e.Id == kEventKeyDown)
		        {
		            if (OnKey(e.Key, e.Mods, e.Platform))
		                e.Stop();
		            return;
		        }

	        // Pointer routing: convert generic mouse events into pointer helpers
	        // when the event is targeting this field. We only care about the
	        // primary button for text editing.
	        if (e.CurrentTarget == this &&
	            (e.Id == kEventMouseDown || e.Id == kEventMouseMove))
	        {
	            UIManager* owner = GetOwnerManager();
	        #ifdef _DEBUG
	            auto debugTag_ = [this]() { return UIRegistration::ElementFactoryRegistry::Instance().GetTagForType(typeid(*this)); };
	            const bool debugThis = (kFieldPointerDebugConsole && debugTag_() == "textinput");
	            static int sFieldPointerLogsRemaining = 64;
	        #endif
	            if (!owner)
	            {
	            #ifdef _DEBUG
	                if (debugThis && sFieldPointerLogsRemaining > 0)
	                {
	                    --sFieldPointerLogsRemaining;
	                    const std::string& id = GetId();
	                    Logger::Log::Info(
	                        "[UI Field] Pointer skip: reason='no-owner' tag='{}', id='{}'",
	                        debugTag_(),
	                        id.empty() ? "<no-id>" : id.c_str());
	                }
	            #endif
	                return;
	            }
	
	            const bool isDown = (e.Id == kEventMouseDown);
	            if (isDown)
	            {
	                // Text editing is a primary-button interaction. Button 1 is the
	                // RIGHT button (GLFW numbering, Input/KeyCodes.h) — claiming it
	                // here swallows every right-button gesture that starts over a
	                // field, such as the graph canvas's right-drag pan.
	                if (e.Button != Input::kMouseButton_Left)
	                {
	                #ifdef _DEBUG
	                    if (debugThis && sFieldPointerLogsRemaining > 0)
	                    {
	                        --sFieldPointerLogsRemaining;
	                        const std::string& id = GetId();
	                        Logger::Log::Info(
	                            "[UI Field] Pointer skip: reason='button' tag='{}', id='{}', button={}",
	                            debugTag_(),
	                            id.empty() ? "<no-id>" : id.c_str(),
	                            e.Button);
	                    }
	                #endif
	                    return;
	                }
	            }
	            else
	            {
	                // Only treat MouseMove as drag while the mouse is held.
	                if (!owner->IsMouseDown())
	                {
	                #ifdef _DEBUG
	                    if (debugThis && sFieldPointerLogsRemaining > 0)
	                    {
	                        --sFieldPointerLogsRemaining;
	                        const std::string& id = GetId();
	                        Logger::Log::Info(
	                            "[UI Field] Pointer skip: reason='no-drag' tag='{}', id='{}'",
	                            debugTag_(),
	                            id.empty() ? "<no-id>" : id.c_str());
	                    }
	                #endif
	                    return;
	                }
	            }
	
	            float x = GetLayoutX();
	            float y = GetLayoutY();
	            float W = GetLayoutWidth();
	            float H = GetLayoutHeight();
	            if (W <= 0.0f || H <= 0.0f)
	            {
	            #ifdef _DEBUG
	                if (debugThis && sFieldPointerLogsRemaining > 0)
	                {
	                    --sFieldPointerLogsRemaining;
	                    const std::string& id = GetId();
	                    Logger::Log::Info(
	                        "[UI Field] Pointer skip: reason='zero-size' tag='{}', id='{}', W={}, H={}",
	                        debugTag_(),
	                        id.empty() ? "<no-id>" : id.c_str(),
	                        W, H);
	                }
	            #endif
	                return;
	            }
	
	            ResolvedStyle style = ComputePointerStyle(owner, this);
	            Rendering::Text::FontAtlas* font = owner->ResolveFontForStyle(style);
	
	            if (!font && RequiresFontAtlasForPointerRouting())
	            {
	            #ifdef _DEBUG
	                if (debugThis && sFieldPointerLogsRemaining > 0)
	                {
	                    --sFieldPointerLogsRemaining;
	                    const std::string& id = GetId();
	                    Logger::Log::Info(
	                        "[UI Field] Pointer skip: reason='no-font' tag='{}', id='{}'",
	                        debugTag_(),
	                        id.empty() ? "<no-id>" : id.c_str());
	                }
	            #endif
	                return;
	            }
	
	        #ifdef _DEBUG
	            if (debugThis && sFieldPointerLogsRemaining > 0)
	            {
	                --sFieldPointerLogsRemaining;
	                const std::string& id = GetId();
	                Logger::Log::Info(
	                    "[UI Field] Pointer dispatch: kind='{}' tag='{}', id='{}', mouseX={}, x={}, W={}, H={}, font={}",
	                    isDown ? "down" : "drag",
	                    debugTag_(),
	                    id.empty() ? "<no-id>" : id.c_str(),
	                    e.X,
	                    x,
	                    W,
	                    H,
	                    static_cast<void*>(font));
	            }
	        #endif
	
	            if (isDown)
	            {
	                OnPointerDown(e.X, e.Y, x, y, W, H, style, font);
	                // Capture mouse so subsequent moves/ups are delivered even if
	                // the pointer leaves the element's bounds.
	                e.Capture(this);
	                e.Stop();
	            }
	            else
	            {
	                OnPointerDrag(e.X, e.Y, x, y, W, H, style, font);
	                e.Stop();
	            }
	        }
    }

    T m_Value{};

public:
    // Public notification methods - called by drag helpers to trigger callbacks after SetValue
    void NotifyValueChanging() { NotifyValue<kEventValueChanging>(m_OnValueChanging); }
    void NotifyValueChanged()  { NotifyValue<kEventValueChanged>(m_OnValueChanged); }

private:
    // Members first, then the handler table — Button::TriggerClick's order, and the one
    // every existing caller already sees. The member slots are last-writer-wins; the
    // table is additive, keyed and individually revocable, which is why subscribers that
    // must not destroy each other's registration (the scripting ABI, a manipulator, a
    // second listener) belong on the table. This ADDS a dispatch beside the members; it
    // does not migrate them.
    //
    // Cost with nothing subscribed — which is the common case, because the inspector
    // writes fields in bulk: one bit test against a literal. The compile-time-id
    // DispatchEvent overload takes the id as a template argument, so there is no hash of
    // the id and no map probe.
    template <EventId kEventId>
    void NotifyValue(const ValueCallback& members)
    {
        // RE-ENTRANCY IS PREVENTED AT THE ROOT, not bounded by a counter.
        //
        // A handler may write the field it is handling. Letting that RECURSE has no natural
        // end: the control's unchanged-value early-out stops a handler that writes back what it
        // read, but not a non-converging cycle — two handlers clamping toward different targets,
        // or one deriving a new value from the old — where every write is a real change. Before
        // this that was an unbounded stack overflow, through the m_On* member callbacks as much
        // as through the events, because the engine had no guard here at all.
        //
        // So while this element's notification is in flight, a nested SetValue on it DOWNGRADES
        // to SetValueWithoutNotify semantics: the write still happens, with the control's normal
        // clamping and validation and dirty marking, and it simply does not notify. Recursion is
        // then structurally impossible rather than merely bounded — one notification pass per
        // external change, always — and a cross-element clamp fight (A writes B, B writes A)
        // terminates because B's write back into A does not re-notify.
        //
        // THE SEMANTIC EDGE, stated because it is a real one: an adjustment made from inside a
        // handler is NOT announced as a second event. Subscribers keep the payload they were
        // given, while the field holds the adjusted value. That is inherent to without-notify
        // semantics and it is the handler author's choice to make; a handler that genuinely
        // wants the new value announced should defer the write (UIElement::PostAction) so it
        // lands as its own external change with its own notification.
        //
        // The normal path never reaches here. A control's own clamping, quantisation and
        // range-pinning run inside SetValue BEFORE it notifies, so the value a handler receives
        // is already the committed one. Reaching this means a handler wrote back, and the
        // diagnostic names the API that expresses that intent directly.
        if (m_Notifying)
        {
            if (!m_ReentrantWriteReported)
            {
                m_ReentrantWriteReported = true;
                UI::Detail::ReportReentrantValueWrite(*this);
            }
            return;
        }

        m_Notifying = true;
        // Scoped, not a matched pair: a handler that throws must not leave the flag raised,
        // which would make this field ignore every later notification for the rest of its life.
        struct NotifyScope
        {
            explicit NotifyScope(bool& f) : Flag(f) {}
            ~NotifyScope() { Flag = false; }
            bool& Flag;
        } notifyScope(m_Notifying);

        // Member slot first, then the handler table — Button::TriggerClick's order. The slots
        // are last-writer-wins; the table is additive, keyed and individually revocable, which
        // is why subscribers that must not destroy each other's registration (the scripting ABI,
        // a manipulator, a second listener) belong on it. This ADDS a dispatch beside the
        // members; it does not migrate them.
        if (members)
            members(m_Value);

        if constexpr (FieldEventValue<T>::kTextPayload)
        {
            // TEXT PAYLOADS COPY BEFORE THEY DISPATCH — the lifetime guarantee, not an
            // optimisation. A handler may write this very field mid-dispatch (the downgrade
            // above makes that an ordinary, non-notifying write), and that write reallocates
            // m_Value's buffer — so an event pointing at m_Value would hand LATER subscribers
            // a dangling view. The scope owns a per-thread buffer the value is copied into,
            // giving every subscriber in this dispatch the value as it was when they were
            // told: the same by-value contract the numeric payload gets for free.
            //
            // The copy is gated on the presence bit, so the common case — programmatic text
            // writes with nothing subscribed — never pays it. The gate skips DispatchEvent
            // entirely, which also skips OnEvent. Deliberate, and only for this event kind:
            // value events are self-dispatched, so a control never legitimately learns of
            // its own value change through its own OnEvent (the numeric path below keeps
            // OnEvent unconditional, like every other dispatch). Gating dispatch — not just
            // the copy — also closes the OnEvent-registers-a-handler window: whenever a
            // table handler can observe the event, the copy was already taken.
            if (this->template HasSubscribedEventHandlers<kEventId>())
            {
                UI::Detail::ScopedValueText payload{FieldEventValue<T>::Text(m_Value)};
                UIEvent e{};
                e.Id = kEventId;
                e.Target = this;
                e.CurrentTarget = this;
                e.Text = payload.View();
                this->template DispatchEvent<kEventId>(e);
            }
        }
        else if constexpr (FieldEventValue<T>::kNumericPayload)
        {
            UIEvent e{};
            e.Id = kEventId;
            e.Target = this;
            e.CurrentTarget = this;
            e.Value = FieldEventValue<T>::Encode(m_Value);
            this->template DispatchEvent<kEventId>(e);
        }
    }

    // A notification is delivering on this element right now, so a nested write must not
    // notify. One bool: there is no depth to track when recursion cannot happen.
    bool m_Notifying = false;
    // Per ELEMENT, not per process: one field written from its own handler must not silence the
    // diagnostic for every other field in the editor, which is what a process-wide latch does
    // the moment two controls do it.
    bool m_ReentrantWriteReported = false;

    ValueCallback m_OnValueChanging;
    ValueCallback m_OnValueChanged;
};

// Backwards-compatible alias for string-based fields.
using BaseField = Field<std::string>;

} // namespace GameEngine
