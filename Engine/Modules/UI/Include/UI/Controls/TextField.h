#pragma once

#include "Input/KeyCodes.h"
#include "UI/CaretBlink.h"
#include "UI/Controls/BaseField.h"
#include "UI/Controls/Label.h"
#include "UI/ITextMeasurable.h"
#include <atomic>
#include <vector>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace GameEngine
{

// TextInput is the low-level single-line text editing control: caret,
// selection, editing sessions, Enter/Escape/blur commit, and text geometry.
// It replaces the previous TextField UI role and is used by higher-level
// typed fields such as TextFieldBase<T> and the various numeric fields.
//
// UIManager performs hit-testing on the rendered box, but TextInput
// delegates focus/tab/keyboard routing to its owning field via the
// focus-proxy mechanism on UIElement. From the outside world focus is
// always expressed in terms of the field (TextField / FloatField /
// IntField / etc.), not the internal editor.
class TextInput : public BaseField, public ITextMeasurable
{
  public:
    TextInput() { m_TextMeasurable = this; }

    // Provide intrinsic text measurement so UIManager/Yoga can size text
    // editors without special-casing TextInput in UIManager.
    void GetTextMeasureInfo(ITextMeasurable::TextMeasureInfo& info,
                            const ResolvedStyle& style) const override
    {
        (void)style;
        info.HasText = true;
        info.Text = GetValue();
        info.ExplicitFontSize = 0.0f; // use style.fontSize
    }

    // Expose current editable text to generic UIElement helpers.
    const std::string& GetTextContent() const override { return GetValue(); }
    TextInput* GetAsTextInput() override { return this; }

    // Input hooks implemented by TextInput
    bool OnChar(unsigned int codepoint) override;
    bool OnKey(int key, int mods, UI::IPlatformApi* platform) override;
    void OnPointerDown(float mouseX, float mouseY,
                       float x, float y, float W, float H,
                       const ResolvedStyle& style, Rendering::Text::FontAtlas* font) override;
    void OnPointerDrag(float mouseX, float mouseY,
                       float x, float y, float W, float H,
                       const ResolvedStyle& style, Rendering::Text::FontAtlas* font) override;

    void OnFocusChanged(bool focused) override;

    // Caret/selection state accessors.
    int GetCaretIndex() const { return m_CaretIndex; }
    int GetSelectionStart() const { return m_SelectionStart; }
    int GetSelectionEnd() const { return m_SelectionEnd; }
    float GetCaretForceVisibleUntil() const { return m_CaretForceVisible.Until(); }
    float GetTextScrollX() const { return m_TextScrollX; }

    // Ensure the measure cache is populated for the current value and font.
    // Returns the caret map (X position per UTF-8 byte offset) and metrics.
    // letterSpacingPx is device px, like pixelSize; both key the cache.
    const Rendering::Text::FontAtlas::MeasureResult& EnsureMeasureCache(
        Rendering::Text::FontAtlas* font, float pixelSize, float letterSpacingPx) const;

    // Recompute m_TextScrollX so the caret stays visible. Called by the
    // render path (only for focused fields with overflowing text — it costs
    // a text measure) before glyph emission so glyphs, caret and selection
    // all read the same scroll value within a frame. innerWidth is the
    // content-box width in physical px (pixelSize is the physical font
    // size); the stored scroll is CSS-logical, hence contentScale. Const
    // because it runs from const geometry paths (m_TextScrollX is mutable,
    // like the measure cache).
    void UpdateScrollToCaret(Rendering::Text::FontAtlas* font, float pixelSize,
                             float letterSpacingPx, float innerWidth,
                             float contentScale) const;

    // Snap back to the value start. Used when the text fits or the field is
    // not being edited; costs nothing.
    void ResetTextScroll() const { m_TextScrollX = 0.0f; }

    /** Select all text in the field (selection 0..length, caret at end). */
    void SelectAll();

    // Callback invoked when an editing session is committed or cancelled
    // (Enter, Escape or blur after a change). Used by typed fields to perform
    // a final resolve step and fire ValueChanged.
    void SetOnCommit(const std::function<void()>& cb) { m_OnCommit = cb; }

  private:
    // caret/selection in UTF-8 byte indices
    int m_CaretIndex = 0;
    int m_SelectionStart = -1;
    int m_SelectionEnd = -1;

    // Internal horizontal scroll (CSS-logical px) for long single-line
    // content. This is independent of any outer ScrollView; it keeps the
    // caret visible without requiring panel-level horizontal scrolling.
    // Updated by UpdateScrollToCaret from the render path; physical-px
    // consumers (glyph emission, caret/selection overlays) multiply by the
    // content scale, pointer hit-testing uses it raw (logical space).
    mutable float m_TextScrollX = 0.0f;

    // Caret visibility boost stamped after any keyboard or pointer interaction.
    // The render path forces the caret visible while (currentTime < Until()).
    // Marked mutable so it can be updated from const geometry paths when needed.
    mutable UI::CaretForceVisibleDeadline m_CaretForceVisible;

    // Editing session state used for Escape-to-cancel and commit-on-blur/Enter.
    // m_OriginalValue captures the value when focus was gained; m_IsEditing
    // tracks whether an edit session is active.
    std::string m_OriginalValue;
    bool m_IsEditing = false;

    std::function<void()> m_OnCommit;

    // Cached shaping-aware measurement (metrics + caret map) for the current value.
    // This avoids repeated HarfBuzz/FreeType work when UI geometry is rebuilt for
    // reasons unrelated to this field (hover/selection elsewhere, etc.).
    mutable bool m_MeasureCacheValid = false;
    mutable std::uint64_t m_MeasureCacheKey = 0;
    mutable Rendering::Text::FontAtlas::MeasureResult m_MeasureCache;

    // Double-click detection state (used in OnPointerDown).
    std::chrono::steady_clock::time_point m_LastPointerDownTime{};
    float m_LastPointerDownX = 0.0f;

    // A selection a press produced — select-all on focus, or a double-clicked
    // word — must outlive the MouseMove platforms deliver at the press
    // position. Set when such a selection is made, cleared by the first drag
    // that actually leaves the press position.
    bool m_HoldPressSelectionUntilPointerMoves = false;

    /** Select the word under the pointer, with any whitespace trailing it. */
    void SelectWordAt(float mouseX, float x, float y, float W, float H,
                      const ResolvedStyle& style, Rendering::Text::FontAtlas* font);

    void BeginEditingSession();
    void CommitEditing();
    void CancelEditing();
    void BumpCaretForceVisible();

    // A non-empty range is selected. A collapsed pair (start == end) or an
    // unset one (-1) both mean "nothing selected" — rendering, copy and
    // delete-selection all agree on that.
    bool HasSelection() const;

    // Anchor a shift-extend. Extending an existing range keeps its anchor;
    // starting a new one anchors at the caret. Without this, a pair left
    // behind by an earlier interaction would anchor the extend at a position
    // the caret has since moved away from.
    void AnchorSelectionForExtend();

    void ClearSelection();

    // Zero-copy pointer hit-testing geometry, built from the same
    // physical-px shaping the render path uses so caret clicks land on the
    // drawn glyphs at any content scale. The caret map is borrowed from the
    // measure cache (valid until the next measure); an entry's logical
    // screen X is TextOriginX + CaretXByBytePhysical[i] * InvContentScale.
    // TextOriginX is the run origin the caret and selection are PAINTED from,
    // carrying no offset of its own: a pointer-only adjustment would map a
    // click to a caret the user does not see under the pointer. font must be
    // non-null.
    // Const like the other pointer-path geometry (clamps the mutable
    // m_TextScrollX defensively).
    struct PointerGeometry
    {
        const std::vector<float>* CaretXByBytePhysical = nullptr;
        float TextOriginX = 0.0f;
        float InvContentScale = 1.0f;
    };
    PointerGeometry BuildPointerGeometry(const ResolvedStyle& style,
                                         Rendering::Text::FontAtlas* font,
                                         float x, float y, float W, float H) const;
};

// Where a field's trailing suffix label sits (see TextFieldBase::SetSuffix).
enum class SuffixAlignment
{
    AfterText, // immediately after the value text
    FieldEnd,  // pinned to the trailing edge of the field (default)
};

// What a field's suffix says (see TextFieldBase::SetSuffix).
enum class SuffixRole
{
    Unit,    // a unit glyph beside the value ("%", "px"), drawn faint (default)
    Caption, // a word that reads the value ("(north)", "(21 June)"), drawn as readable text
};

// Generic typed field backed by a TextInput. This bridges between textual
// editing and a typed value T via parse/format/filter functions.
template <typename T>
class TextFieldBase : public Field<T>
{
  public:
    using ValueType = T;
    using ParseFunction = std::function<bool(const std::string&, T&, bool /*isFinal*/)>;
    using FormatFunction = std::function<std::string(const T&)>;
    using FilterFunction = std::function<std::string(const std::string&, bool /*isFinal*/)>;

    TextFieldBase()
    {
        // Text fields participate in keyboard focus by default. UIManager
        // relies on IsFocusable() when building the tab order and on
        // mouse-down to assign focus; no type-specific heuristics.
        this->SetFocusable(true);
        InitializeTextInput();
    }

    void SetParseFunction(ParseFunction fn) { m_Parse = std::move(fn); }
    void SetFormatFunction(FormatFunction fn) { m_Format = std::move(fn); }
    // Optional alternate formatting shown while the field has focus (e.g.
    // FloatField's full round-trip precision vs the shorter display form).
    // Unset means the displayed text is edited as-is.
    void SetEditFormatFunction(FormatFunction fn) { m_EditFormat = std::move(fn); }
    void SetFilterFunction(FilterFunction fn) { m_Filter = std::move(fn); }

    // Trailing label shown inside the field after the value: a faded unit ("px", "%") or,
    // with SuffixRole::Caption, a readable gloss ("(21 June)"). It is part of the field, not
    // the editable value: a press on it reaches the field. A caption stays on one line: where
    // the field is too narrow it is cut with an ellipsis before the value gives up more than a
    // fraction of a pixel, and resting the pointer on the cut caption shows the whole of it (a
    // truncated label is its own tooltip) unless the field or an element above it carries a
    // tooltip, which answers first. A unit suffix stays visible while editing; a Caption
    // describes the stored value, so it is hidden from focus until the edit ends. An edit that
    // leaves the value as it was brings the caption back at once; one that changes the value
    // keeps it hidden until the host passes the new value's caption here, so the old gloss
    // never shows beside the new value.
    // SetSuffixAlignment controls whether it hugs the value or sits at the field end.
    // Empty clears it.
    void SetSuffix(const std::string& suffix)
    {
        m_SuffixText = suffix;
        if (m_CaptionAwaitsNewText)
        {
            m_CaptionAwaitsNewText = false;
            m_CaptionHiddenWhileEditing = false;
        }
        if (suffix.empty())
        {
            if (m_Suffix)
                m_Suffix->SetText("");
            this->RemoveClass("has-suffix");
            return;
        }
        if (!m_Suffix)
        {
            auto label = std::make_unique<Label>();
            m_Suffix = label.get();
            m_Suffix->AddClass("field-suffix-slot");
            m_Suffix->RequestSubtreeStyleAssetPath("UI/controls/FieldSuffix/FieldSuffix.css", "editor");
            this->AddChild(std::move(label));
            ApplySuffixRole();
        }
        m_Suffix->SetText(m_CaptionHiddenWhileEditing ? std::string() : suffix);
        this->AddClass("has-suffix");
    }

    // A Caption suffix carries information the author reads, so it is drawn at the value's own
    // brightness (UI/controls/FieldSuffix/FieldSuffix.css) rather than as a faint unit glyph.
    void SetSuffixRole(SuffixRole role)
    {
        m_SuffixRole = role;
        ApplySuffixRole();
    }

    // Where the suffix sits along the field: right after the value text, or pinned
    // to the trailing edge of the field (the default).
    void SetSuffixAlignment(SuffixAlignment alignment)
    {
        m_SuffixAlignment = alignment;
        if (alignment == SuffixAlignment::AfterText)
            this->AddClass("suffix-after-text");
        else
            this->RemoveClass("suffix-after-text");
    }

    // Forward commit notification (Enter / Escape / blur) to the inner TextInput.
    void SetOnCommit(const std::function<void()>& cb)
    {
        if (m_Text) m_Text->SetOnCommit(cb);
    }

    // Configure what value empty text should preview while editing and what
    // value should be committed when the user finalizes an empty field.
    void SetEmptyValues(const T& previewEmpty, const T& commitEmpty)
    {
        m_EmptyPreview = previewEmpty;
        m_EmptyCommit = commitEmpty;
    }

    const T& GetValue() const override { return this->m_Value; }

    void SetValue(const T& v) override
    {
        m_Mixed = false;
        m_SuppressCallbacks = true;
        Field<T>::SetValue(v);

        if (m_Text)
        {
            if (m_Format)
            {
                m_Text->SetValue(m_Format(this->m_Value));
            }
        }
        m_SuppressCallbacks = false;
        RestoreCaptionIfItDescribesTheValue();
    }

    void SetValueWithoutNotify(const T& v) override
    {
        m_Mixed = false;
        m_SuppressCallbacks = true;
        Field<T>::SetValueWithoutNotify(v);
        RestoreCaptionIfItDescribesTheValue();
        if (m_Text && m_Format)
            m_Text->SetValue(m_Format(this->m_Value));
        m_SuppressCallbacks = false;
    }

    // Show an indeterminate "mixed" placeholder for multi-selection where the
    // selected objects disagree on this field. The underlying value is kept, so
    // editing the field clears the mixed state and commits normally; finalizing
    // WITHOUT editing keeps it mixed and commits nothing (no placeholder leaks in
    // as a value). The drawer sets the value first, then calls this.
    void SetMixed()
    {
        m_Mixed = true;
        SetTextSuppressed("\xE2\x80\x94");  // U+2014 em dash
    }

    bool IsMixed() const { return m_Mixed; }

  protected:
    // The limits a typed value is held to, applied to every value typing produces (each parsed
    // value and the empty-text value, while typing and on commit), whatever parse function the
    // host installs: a numeric field clamps to its range here, so typed text lands where a drag
    // would and the field shows what its host receives.
    virtual T ConstrainTypedValue(const T& value) const { return value; }

  public:

    // UIManager routes focus and keyboard/pointer input to the Field<T>
    // instance (treated as a BaseField for string fields). Forward these
    // hooks to the embedded TextInput so editing behavior lives there.
    void OnFocusChanged(bool focused) override
    {
        if (m_Text)
        {
            // Swap to the full-precision editing representation before the
            // editor captures its Escape-restore snapshot, so Escape lands
            // back on the same text the session started from.
            if (focused && !m_Mixed && m_EditFormat)
                SetTextSuppressed(m_EditFormat(this->m_Value));
            m_Text->OnFocusChanged(focused);
            // Blur without an edit skips the commit path entirely; restore
            // the display formatting either way (no-op when commit already
            // reformatted).
            if (!focused && !m_Mixed && m_Format)
                SetTextSuppressed(m_Format(this->m_Value));
            HideCaptionWhileEditing(focused);
            if (focused)
            {
                // Tab focus selects the whole value on every field. Focus that
                // arrived any other way (a mouse press, or a programmatic focus
                // request) only selects all on fields that opt in, so a click
                // keeps the caret OnPointerDown placed one frame earlier.
                UIManager* owner = this->GetOwnerManager();
                const bool keyboardFocus = owner && owner->IsFocusViaKeyboard();
                if (keyboardFocus || SelectsAllOnMouseFocus())
                    m_Text->SelectAll();
            }
        }
    }

    // A disabled control never activates. Focus and the pointer already skip a
    // disabled element and everything inside it; these gates are the control's
    // own guarantee, so a character that arrives anyway cannot become a typed
    // value its owner refuses to store.
    bool OnChar(unsigned int codepoint) override
    {
        if (!this->IsEnabled())
            return false;
        return m_Text ? m_Text->OnChar(codepoint) : false;
    }

    bool OnKey(int key, int mods, UI::IPlatformApi* platform) override
    {
        if (!this->IsEnabled())
            return false;
        if (m_OnEnter && (key == Input::kKeyCode_Enter || key == Input::kKeyCode_NumPadEnter))
        {
            m_OnEnter();
            return true;
        }
        return m_Text ? m_Text->OnKey(key, mods, platform) : false;
    }

    // Runs on Enter (or NumPad Enter) only, in place of the commit, for a field whose Enter
    // is an action ("send this comment"); the commit callback also fires on blur and Escape.
    void SetOnEnter(std::function<void()> onEnter) { m_OnEnter = std::move(onEnter); }

    /** Select all text in the field (for search boxes, value boxes, etc.). */
    void SelectAll()
    {
        if (m_Text)
            m_Text->SelectAll();
    }

    // Whether focus that did not arrive from the keyboard selects the whole
    // value. Numeric value/property fields default to on: "click and retype" is
    // the IDE convention they are edited with. TextField defaults to off so the
    // click's caret survives, the way a browser text box behaves. Tab focus
    // selects all either way (see OnFocusChanged). Settable per instance so an
    // inspector's free-text field can opt in without a subclass; the XML
    // attribute is selectallonmousefocus.
    void SetSelectsAllOnMouseFocus(bool selectsAll) { m_SelectsAllOnMouseFocus = selectsAll; }
    bool SelectsAllOnMouseFocus() const { return m_SelectsAllOnMouseFocus; }

    void OnPointerDown(float mouseX, float mouseY,
                       float /*x*/, float /*y*/, float /*W*/, float /*H*/,
                       const ResolvedStyle& /*outerStyle*/,
                       Rendering::Text::FontAtlas* font) override
    {
	        if (!this->IsEnabled())
	            return;
	        if (m_Text)
	        {
	            // Use the inner TextInput's layout coordinates and computed style,
	            // not the outer field's. The outer field may have different
	            // padding/position than the TextInput, which would cause caret
	            // placement to drift increasingly off for longer text.
	            float tx = m_Text->GetLayoutX();
	            float ty = m_Text->GetLayoutY();
	            float tw = m_Text->GetLayoutWidth();
	            float th = m_Text->GetLayoutHeight();
	            ResolvedStyle innerStyle = Field<T>::ComputePointerStyle(this->GetOwnerManager(), m_Text);
	            // For TextInput we want pointer-based measurement to use the same
	            // font atlas as rendering. TextInput::OnGeneratePrimitives always
	            // uses UIManager's font atlas,
	            // so prefer the owner's default atlas here instead of any
	            // family-specific atlas resolved for the outer field.
	            UIManager* owner = this->GetOwnerManager();
	            Rendering::Text::FontAtlas* textFont = font;
	            if (owner)
	            {
	                Rendering::Text::FontAtlas* defaultAtlas = owner->GetDefaultFontAtlas();
	                if (defaultAtlas)
	                    textFont = defaultAtlas;
	            }
	            m_Text->OnPointerDown(mouseX, mouseY, tx, ty, tw, th, innerStyle, textFont);
	        }
    }

    void OnPointerDrag(float mouseX, float mouseY,
                       float /*x*/, float /*y*/, float /*W*/, float /*H*/,
                       const ResolvedStyle& /*outerStyle*/,
                       Rendering::Text::FontAtlas* font) override
    {
	        if (!this->IsEnabled())
	            return;
	        if (m_Text)
	        {
	            // Use the inner TextInput's layout coordinates and computed style.
	            float tx = m_Text->GetLayoutX();
	            float ty = m_Text->GetLayoutY();
	            float tw = m_Text->GetLayoutWidth();
	            float th = m_Text->GetLayoutHeight();
	            ResolvedStyle innerStyle = Field<T>::ComputePointerStyle(this->GetOwnerManager(), m_Text);
	            UIManager* owner = this->GetOwnerManager();
	            Rendering::Text::FontAtlas* textFont = font;
	            if (owner)
	            {
	                Rendering::Text::FontAtlas* defaultAtlas = owner->GetDefaultFontAtlas();
	                if (defaultAtlas)
	                    textFont = defaultAtlas;
	            }
	            m_Text->OnPointerDrag(mouseX, mouseY, tx, ty, tw, th, innerStyle, textFont);
	        }
    }

  protected:
    TextInput* GetTextInput() const { return m_Text; }

    // Helper for derived numeric/text fields to configure the embedded
    // TextInput's identity in a consistent way.
    //
    // If idPrefix is non-empty and the TextInput currently has no id,
    // a unique id of the form "<idPrefix><N>" will be assigned. If
    // editorClass is non-empty, it will be added as a class on the
    // TextInput.
    void ConfigureEditorIdentity(const std::string& idPrefix,
                                 const std::string& editorClass)
    {
        TextInput* editor = GetTextInput();
        if (!editor)
            return;

        if (!idPrefix.empty() && editor->GetId().empty())
        {
            int id = s_NextEditorId.fetch_add(1, std::memory_order_relaxed);
            editor->SetId(idPrefix + std::to_string(id));
        }

        if (!editorClass.empty())
        {
            editor->AddClass(editorClass);
        }
    }

    // Set the embedded editor's displayed text directly, bypassing the
    // format/parse functions and without firing value callbacks. Used by
    // derived fields that decide the displayed string based on runtime
    // layout (e.g. width-aware compacting in FloatField).
    void SetEditorText(const std::string& text)
    {
        if (m_Text)
        {
            m_SuppressCallbacks = true;
            m_Text->SetValue(text);
            m_SuppressCallbacks = false;
        }
    }

    const std::string& GetEditorText() const
    {
        static const std::string kEmpty;
        return m_Text ? m_Text->GetValue() : kEmpty;
    }

  private:
    TextInput* m_Text = nullptr;
    Label* m_Suffix = nullptr;
    SuffixAlignment m_SuffixAlignment = SuffixAlignment::FieldEnd;
    SuffixRole m_SuffixRole = SuffixRole::Unit;
    std::string m_SuffixText;
    bool m_CaptionHiddenWhileEditing = false;
    // A commit changed the value: the caption stays hidden until SetSuffix brings the new one.
    bool m_CaptionAwaitsNewText = false;
    // The value when the caption was hidden, the one it describes; typing previews new values
    // into m_Value, so the commit compares against this.
    T m_ValueCaptionDescribes{};

    // A host that clamps a committed value back to the one the caption describes (a latitude typed
    // past 90 while at 90) never passes a new caption; the one it has is right again.
    void RestoreCaptionIfItDescribesTheValue()
    {
        if (!m_CaptionAwaitsNewText || !(this->m_Value == m_ValueCaptionDescribes))
            return;
        m_CaptionAwaitsNewText = false;
        HideCaptionWhileEditing(false);
    }

    // A caption glosses the stored value ("172 (21 June)"), so beside half-typed text ("21 Ju") it
    // would describe a value the field does not hold yet. Unit suffixes are never hidden.
    void HideCaptionWhileEditing(bool hidden)
    {
        hidden = hidden && m_SuffixRole == SuffixRole::Caption;
        if (!hidden && m_CaptionAwaitsNewText)
            return;
        if (hidden == m_CaptionHiddenWhileEditing)
            return;
        m_CaptionHiddenWhileEditing = hidden;
        if (hidden)
            m_ValueCaptionDescribes = this->m_Value;
        if (m_Suffix)
            m_Suffix->SetText(hidden ? std::string() : m_SuffixText);
    }

    void ApplySuffixRole()
    {
        if (!m_Suffix)
            return;
        const bool caption = m_SuffixRole == SuffixRole::Caption;
        if (caption)
        {
            m_Suffix->RemoveClass("field-suffix");
            m_Suffix->AddClass("field-caption");
        }
        else
        {
            m_Suffix->RemoveClass("field-caption");
            m_Suffix->AddClass("field-suffix");
        }
    }
    bool m_SuppressCallbacks = false;
    bool m_Mixed = false;
    bool m_SelectsAllOnMouseFocus = true;
    std::function<void()> m_OnEnter;
    ParseFunction m_Parse;
    FormatFunction m_Format;
    FormatFunction m_EditFormat;
    FilterFunction m_Filter;
    T m_EmptyPreview{};
    T m_EmptyCommit{};

    inline static std::atomic<int> s_NextEditorId{0};

    // Write the editor's text without echoing value-change callbacks back
    // into the typed field — the idiom for programmatic text sync.
    void SetTextSuppressed(const std::string& s)
    {
        if (!m_Text)
            return;
        m_SuppressCallbacks = true;
        m_Text->SetValue(s);
        m_SuppressCallbacks = false;
    }

    void InitializeTextInput()
    {
        auto text = std::make_unique<TextInput>();
        m_Text = text.get();
        // Internal editor: focus is owned by the outer field. This keeps
        // tab order and UIManager::m_FocusId expressed in terms of the
        // field rather than the TextInput itself.
        if (m_Text)
        {
            m_Text->SetFocusProxy(this);
            // Generic class so a field with a suffix can lay the editor out next to
            // the suffix label (see .has-suffix > .field-editor in core.css).
            m_Text->AddClass("field-editor");
            /* The control root is the focus-within host: a numeric field keeps
               its accent while the drag grip owns the pointer and the inner
               editor is not itself focused. One class here means styling can
               say that in a single selector. */
            this->AddClass("field-editor-host");
        }
        this->AddChild(std::move(text));

        if (m_Text)
        {
            // Live editing notifications are mapped to ValueChanging on the
            // typed field, while the commit callback performs a final resolve
            // and fires ValueChanged.
            m_Text->SetOnValueChanging([this](const std::string&)
                                       { OnTextEditingChanged(); });
            m_Text->SetOnValueChanged([this](const std::string&)
                                      { OnTextEditingChanged(); });
            m_Text->SetOnCommit([this]()
                                { OnTextCommit(); });
        }
    }

    void OnTextEditingChanged()
    {
        if (m_SuppressCallbacks || !m_Text)
            return;

        m_Mixed = false;  // a real user edit replaces the indeterminate state
        HideCaptionWhileEditing(true);

        std::string raw = m_Text->GetValue();
        std::string filtered = m_Filter ? m_Filter(raw, false) : raw;

        if (filtered != raw)
        {
            m_SuppressCallbacks = true;
            m_Text->SetValue(filtered);
            m_SuppressCallbacks = false;
        }

        T newValue = this->m_Value;
        bool hasNewValue = false;

        if (!filtered.empty())
        {
            if (m_Parse)
            {
                T parsed{};
                if (m_Parse(filtered, parsed, false))
                {
                    newValue = ConstrainTypedValue(parsed);
                    hasNewValue = true;
                }
            }
        }
        else
        {
            newValue = ConstrainTypedValue(m_EmptyPreview);
            hasNewValue = true;
        }

        if (hasNewValue && newValue != this->m_Value)
        {
            this->m_Value = newValue;
            this->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
            this->NotifyValueChanging();
        }
    }

    void OnTextCommit()
    {
        if (m_SuppressCallbacks || !m_Text)
            return;

        // Finalized while still showing the mixed placeholder (focused + blurred
        // without typing): keep it indeterminate, don't commit the placeholder.
        if (m_Mixed)
            return;

        std::string raw = m_Text->GetValue();
        std::string filtered = m_Filter ? m_Filter(raw, true) : raw;

        T resolved = this->m_Value;
        bool hasResolved = false;

        if (!filtered.empty())
        {
            if (m_Parse)
            {
                T parsed{};
                if (m_Parse(filtered, parsed, true))
                {
                    resolved = ConstrainTypedValue(parsed);
                    hasResolved = true;
                }
            }
        }
        else
        {
            resolved = ConstrainTypedValue(m_EmptyCommit);
            hasResolved = true;
        }

        m_SuppressCallbacks = true;
        this->m_Value = resolved;
        if (m_Text)
        {
            std::string display;
            if (m_Format)
                display = m_Format(this->m_Value);
            else
                display = filtered;

            if (m_Text->GetValue() != display)
            {
                m_Text->SetValue(display);
            }
        }
        m_SuppressCallbacks = false;
        if (m_CaptionHiddenWhileEditing && !(resolved == m_ValueCaptionDescribes))
            m_CaptionAwaitsNewText = true;
        else
            HideCaptionWhileEditing(false);

        this->MarkDirty(UIElement::LayoutDirty | UIElement::VisualDirty);
        this->NotifyValueChanged();
    }
};

// Non-templated string field built on top of TextFieldBase<std::string>.
// This represents a simple text value field backed by a TextInput editor.
class TextField : public TextFieldBase<std::string>
{
  public:
    using ValueType = std::string;

    TextField()
    {
        // For string fields the text is the value: identity filter/parse/format.
        SetFilterFunction([](const std::string& raw, bool /*isFinal*/)
                          { return raw; });

        SetParseFunction([](const std::string& text, std::string& out, bool /*isFinal*/)
                         {
	        out = text;
	        return true; });

        SetFormatFunction([](const std::string& v)
                          { return v; });

        SetEmptyValues(std::string{}, std::string{});
        SetValue(std::string{});

        // Free text — a name box, a search field. Clicking one means "put the
        // caret here", not "replace everything", so mouse focus leaves the
        // click's caret alone. An inspector field that wants the numeric-field
        // behaviour calls SetSelectsAllOnMouseFocus(true).
        SetSelectsAllOnMouseFocus(false);
    }
};

} // namespace GameEngine
