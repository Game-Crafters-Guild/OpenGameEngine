#include "UI/Controls/TextField.h"
#include "UI/TextSegmentation.h"
#include "UI/Utf8Helpers.h"
#include "Input/InputSystem.h"
#include "Input/KeyCodes.h"
#include "Platform/SystemMetrics.h"
#include "Types/ColorUtils.h"
#include "UI/CaretBlink.h"
#include "UI/Parsers/CSSParser.h"
#include "UI/UIManager.h"
#include "UI/UIStyle.h"

#include "Rendering/Text/FontAtlas.h"
#include "Rendering/Text/TextLayout.h"

#include "Logger/Logger.h"
#include "../UIAttributeAccess.h"
#include "../UIManager_Internal.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <limits>

#include <sstream>

namespace GameEngine
{

using namespace Rendering::Text;
using namespace Rendering::Geometry;

#ifdef _DEBUG
// Opt-in diagnostics for TextInput interactions. Disabled by default; flip
// this on temporarily when chasing caret/selection issues.
// Use a non-constexpr flag so "if (flag)" checks do not produce MSVC's
// C4127 (conditional expression is constant) warnings.
static bool kTextInputPointerDebug = false;
#endif

namespace
{
static inline std::uint64_t Fnv1a64TextField(const void* data, size_t size, std::uint64_t seed = 14695981039346656037ull)
{
    const std::uint8_t* p = reinterpret_cast<const std::uint8_t*>(data);
    std::uint64_t h = seed;
    for (size_t i = 0; i < size; ++i)
    {
        h ^= (std::uint64_t)p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static inline std::uint64_t HashString64TextField(const std::string& s, std::uint64_t seed)
{
    return s.empty() ? seed : Fnv1a64TextField(s.data(), s.size(), seed);
}
} // namespace

// Find the caret index for a given logical screen X by snapping to the
// *nearest* caret position. The map is the measure cache's physical-px caret
// array (see TextInput::PointerGeometry); each entry's logical screen X is
// origin + map[i] * invContentScale.
//
// Nearest-boundary is the browser rule, measured against Chrome/150: a pointer
// in the left half of a glyph resolves to that glyph's own boundary and one in
// the right half to the next boundary. Every pointer gesture goes through here,
// word selection included — there is no second notion of "which character the
// pointer is over".
//
// The strict `<` is load-bearing, not a stylistic choice. The caret map is
// indexed per byte and every byte of a multi-byte cluster carries the cluster's
// X (FontAtlas::BuildCaretMapUtf8 backfills them), so equal distances are
// common. Keeping the first index of such a run is what stops a click from
// placing the caret inside a character. Chrome breaks an exact tie the other
// way, which is only reachable at 1/64px and is not worth trading that
// invariant for.
static int FindCaretIndexForScreenX(const std::vector<float>& caretMapPhysical,
                                    float textOriginScreenX,
                                    float invContentScale,
                                    float mouseX)
{
    if (caretMapPhysical.empty())
        return 0;

    const int numCarets = (int)caretMapPhysical.size();

    int bestIndex = 0;
    float bestDist = std::fabs(mouseX - (textOriginScreenX + caretMapPhysical[0] * invContentScale));

    for (int i = 1; i < numCarets; ++i)
    {
        float caretX = textOriginScreenX + caretMapPhysical[i] * invContentScale;
        float d = std::fabs(mouseX - caretX);
        if (d < bestDist)
        {
            bestDist = d;
            bestIndex = i;
        }
    }

    return bestIndex;
}

TextInput::PointerGeometry TextInput::BuildPointerGeometry(const ResolvedStyle& style,
                                                           FontAtlas* font,
                                                           float x, float y,
                                                           float W, float H) const
{
    float cs = 1.0f;
    if (UIManager* owner = GetOwnerManager())
        cs = owner->GetContentScale();
    cs = std::max(0.01f, cs);

    PointerGeometry geo;
    geo.InvContentScale = 1.0f / cs;

    // Same physical pixel size and measure cache as the render path
    // (EmitTextPrimitives / BuildTextInputOverlays); positions convert back
    // to logical px, the space pointer events arrive in. Measuring at the
    // unscaled size instead would diverge from the drawn glyphs at
    // fractional content scales (hinting + trunc(size * scale)).
    const float px = std::max(1.0f, style.Visual.FontSize * cs);
    const float lsPx = style.Visual.LetterSpacing * cs;
    const FontAtlas::MeasureResult& measure = EnsureMeasureCache(font, px, lsPx);
    geo.CaretXByBytePhysical = &measure.caretXByByte;
    const float textWidth = measure.metrics.width * geo.InvContentScale;

    // Content box in logical px — the same box-model rules as rendering
    // (scale 1 on the logical rect). `style` is a pointer-routing style, whose
    // padding Field::ComputePointerStyle already replaced with the used length;
    // this path never sees a percentage.
    const ContentBox cb =
        ComputeContentBox(x, y, W, H, style.Layout.Padding, style.Layout.BorderWidth, 1.0f);

    float xBase = cb.X;
    if (style.Visual.TextAlign == TextAlign::Center)
        xBase = cb.X + (cb.W - textWidth) * 0.5f;
    else if (style.Visual.TextAlign == TextAlign::Right)
        xBase = cb.X + cb.W - textWidth;

    // Overflowing content is left-anchored and scrolled — the anchor rule
    // EmitTextPrimitives applies. The render path owns m_TextScrollX; clamp
    // defensively against the current text.
    float scrollX = 0.0f;
    if (textWidth > cb.W + 0.5f)
    {
        m_TextScrollX = std::clamp(m_TextScrollX, 0.0f, textWidth - cb.W);
        scrollX = m_TextScrollX;
        xBase = cb.X;
    }
    else
    {
        m_TextScrollX = 0.0f;
    }

    // The run origin the caret and selection are drawn from
    // (BuildTextInputOverlays), in logical px. Any offset applied here and not
    // there maps a click to a caret the user does not see under the pointer.
    geo.TextOriginX = xBase - scrollX;
    return geo;
}

// --- Input handlers ---
void TextInput::OnPointerDown(float mouseX, float /*mouseY*/,
                              float x, float y, float W, float H,
                              const ResolvedStyle& style, FontAtlas* font)
{
    if (!font)
        return;

    // Double-click selects the word under the pointer, the way a browser text
    // box does — including the run of whitespace that trails it.
    constexpr float kDoubleClickTolerancePx = 20.0f;
    auto now = std::chrono::steady_clock::now();
    if ((now - m_LastPointerDownTime) < Platform::GetDoubleClickInterval() &&
        std::fabs(mouseX - m_LastPointerDownX) <= kDoubleClickTolerancePx)
    {
        m_LastPointerDownTime = now;
        m_LastPointerDownX = mouseX;
        SelectWordAt(mouseX, x, y, W, H, style, font);
        return;
    }
    m_LastPointerDownTime = now;
    m_LastPointerDownX = mouseX;
    // A plain press owns the caret from here; only the paths below that select
    // a range re-arm the hold.
    m_HoldPressSelectionUntilPointerMoves = false;

    // Ensure the caret is immediately visible when the user clicks inside the
    // field instead of waiting for the blink timer to turn it on.
    BumpCaretForceVisible();

    const PointerGeometry geo = BuildPointerGeometry(style, font, x, y, W, H);
    int oldCaret = m_CaretIndex;
    m_CaretIndex = FindCaretIndexForScreenX(*geo.CaretXByBytePhysical, geo.TextOriginX,
                                            geo.InvContentScale, mouseX);

    int mods = 0;
    if (UIManager* owner = GetOwnerManager())
        mods = owner->GetModifierKeys();
    // Shift+click is the only extend gesture. Ctrl/Cmd+click places the caret
    // exactly like a plain click, which is what browsers and native text fields
    // do on every platform.
    const bool extendSelection = (mods & Input::kModShift) != 0;
    if (extendSelection)
    {
        const int len = static_cast<int>(GetValue().size());
        const int click = m_CaretIndex;
        int anchor = oldCaret;
        const bool hasSel = HasSelection();
        if (hasSel)
        {
            const int a = std::min(m_SelectionStart, m_SelectionEnd);
            const int b = std::max(m_SelectionStart, m_SelectionEnd);
            if (click < a) anchor = b;
            else if (click > b) anchor = a;
            else
            {
                const int oc = std::clamp(oldCaret, 0, len);
                anchor = (oc == a) ? b : a;
            }
        }
        m_SelectionStart = std::min(anchor, click);
        m_SelectionEnd = std::max(anchor, click);
    }
    else
    {
        m_SelectionStart = m_SelectionEnd = m_CaretIndex;
    }
    // Caret/selection visuals moved; request a geometry rebuild (but not per-frame).
    MarkDirty(VisualDirty);

#ifdef _DEBUG
    static int sTextInputPointerDownLogsRemaining = 256;
    if (kTextInputPointerDebug && sTextInputPointerDownLogsRemaining > 0)
    {
        --sTextInputPointerDownLogsRemaining;
        const std::string tag = UIAttributeAccess::GetDebugTypeName(*this);
        const std::string& id = GetId();
	        Logger::Log::Info(
	            "[UI TextInput] PointerDown tag='{}', id='{}', mouseX={}, textOriginScreenX={}, scrollX={}, oldCaret={}, newCaret={}, text='{}'",
	            tag,
	            id.empty() ? "<no-id>" : id.c_str(),
	            mouseX,
	            geo.TextOriginX,
	            m_TextScrollX,
	            oldCaret,
	            m_CaretIndex,
	            GetValue());
	        // For harder-to-reproduce editor issues, also dump the full caret map in
	        // screen space along with distances to the click. This mirrors the logic
	        // in FindCaretIndexForScreenX so a single log line can explain why a
	        // particular caret index was chosen.
		        if (!geo.CaretXByBytePhysical->empty())
	        {
	            std::ostringstream oss;
	            oss.setf(std::ios::fixed, std::ios::floatfield);
	            oss.precision(2);
	            float bestDistDebug = std::numeric_limits<float>::max();
	            int bestIndexDebug = 0;
		            for (size_t i = 0; i < geo.CaretXByBytePhysical->size(); ++i)
		            {
		                float caretScreenX = geo.TextOriginX + (*geo.CaretXByBytePhysical)[i] * geo.InvContentScale;
	                float dist = std::fabs(mouseX - caretScreenX);
	                if (dist < bestDistDebug)
	                {
	                    bestDistDebug = dist;
	                    bestIndexDebug = (int)i;
	                }
	                if (i > 0)
	                    oss << ", ";
	                oss << i << ":" << caretScreenX << "(d=" << dist << ")";
	            }
	            Logger::Log::Info(
	                "[UI TextInput] PointerDown caretMap tag='{}', id='{}', chosenCaret={}, bestIndexByScan={}, mouseX={}, carets=[{}]",
	                tag,
	                id.empty() ? "<no-id>" : id.c_str(),
	                m_CaretIndex,
	                bestIndexDebug,
	                mouseX,
	                oss.str());
	        }
    }
#endif
}

void TextInput::BeginEditingSession()
{
    if (!m_IsEditing)
    {
        m_OriginalValue = GetValue();
        m_IsEditing = true;
    }
}

void TextInput::CommitEditing()
{
    if (!m_IsEditing)
    {
        return;
    }

    m_IsEditing = false;
    if (m_OnCommit)
    {
        m_OnCommit();
    }
}

void TextInput::CancelEditing()
{
    if (!m_IsEditing)
        return;

    SetValue(m_OriginalValue);
    m_CaretIndex = (int)m_OriginalValue.size();
    ClearSelection();
    m_IsEditing = false;
    if (m_OnCommit)
    {
        m_OnCommit();
    }
}

void TextInput::OnPointerDrag(float mouseX, float /*mouseY*/,
                              float x, float y, float W, float H,
                              const ResolvedStyle& style, FontAtlas* font)
{
    if (!font)
        return;

    constexpr float kSpuriousDragTolerancePx = 20.0f;
    if (m_HoldPressSelectionUntilPointerMoves)
    {
        if (std::fabs(mouseX - m_LastPointerDownX) <= kSpuriousDragTolerancePx)
            return;
        m_HoldPressSelectionUntilPointerMoves = false;
    }

    // Keep the caret/selection visible while the user is dragging.
    BumpCaretForceVisible();

    const PointerGeometry geo = BuildPointerGeometry(style, font, x, y, W, H);
#ifdef _DEBUG
    int oldCaret = m_CaretIndex;
    int oldSelStart = m_SelectionStart;
    int oldSelEnd = m_SelectionEnd;
#endif
    m_CaretIndex = FindCaretIndexForScreenX(*geo.CaretXByBytePhysical, geo.TextOriginX,
                                            geo.InvContentScale, mouseX);
    m_SelectionEnd = m_CaretIndex;
    // Caret/selection visuals moved; request a geometry rebuild (but not per-frame).
    MarkDirty(VisualDirty);

#ifdef _DEBUG
    static int sTextInputPointerDragLogsRemaining = 256;
    if (kTextInputPointerDebug && sTextInputPointerDragLogsRemaining > 0)
    {
        --sTextInputPointerDragLogsRemaining;
        const std::string tag = UIAttributeAccess::GetDebugTypeName(*this);
        const std::string& id = GetId();
	        Logger::Log::Info(
	            "[UI TextInput] PointerDrag tag='{}', id='{}', mouseX={}, textOriginScreenX={}, scrollX={}, oldCaret={}, newCaret={}, oldSel=({},{}), newSel=({},{}), text='{}'",
	            tag,
	            id.empty() ? "<no-id>" : id.c_str(),
	            mouseX,
	            geo.TextOriginX,
	            m_TextScrollX,
	            oldCaret,
	            m_CaretIndex,
	            oldSelStart,
	            oldSelEnd,
	            m_SelectionStart,
	            m_SelectionEnd,
	            GetValue());
		        if (!geo.CaretXByBytePhysical->empty())
	        {
	            std::ostringstream oss;
	            oss.setf(std::ios::fixed, std::ios::floatfield);
	            oss.precision(2);
	            float bestDistDebug = std::numeric_limits<float>::max();
	            int bestIndexDebug = 0;
		            for (size_t i = 0; i < geo.CaretXByBytePhysical->size(); ++i)
		            {
		                float caretScreenX = geo.TextOriginX + (*geo.CaretXByBytePhysical)[i] * geo.InvContentScale;
	                float dist = std::fabs(mouseX - caretScreenX);
	                if (dist < bestDistDebug)
	                {
	                    bestDistDebug = dist;
	                    bestIndexDebug = (int)i;
	                }
	                if (i > 0)
	                    oss << ", ";
	                oss << i << ":" << caretScreenX << "(d=" << dist << ")";
	            }
	            Logger::Log::Info(
	                "[UI TextInput] PointerDrag caretMap tag='{}', id='{}', chosenCaret={}, bestIndexByScan={}, mouseX={}, carets=[{}]",
	                tag,
	                id.empty() ? "<no-id>" : id.c_str(),
	                m_CaretIndex,
	                bestIndexDebug,
	                mouseX,
	                oss.str());
	        }
    }
#endif
}

bool TextInput::OnChar(unsigned int codepoint)
{
    if (!Utf8::IsTextInputCodepoint(codepoint))
        return false;

    // Typing is an explicit caret interaction; force it visible for a short
    // window so arrow-key navigation and character input do not result in a
    // "hidden" caret immediately after the key press.
    BumpCaretForceVisible();

    BeginEditingSession();
    std::string s = GetValue();
    if (HasSelection())
    {
        int a = std::min(m_SelectionStart, m_SelectionEnd);
        int b = std::max(m_SelectionStart, m_SelectionEnd);
        a = std::clamp(a, 0, (int)s.size());
        b = std::clamp(b, 0, (int)s.size());
        s.erase((size_t)a, (size_t)(b - a));
        m_CaretIndex = a;
    }
    size_t pos = (size_t)std::clamp(m_CaretIndex, 0, (int)s.size());
    std::string ch = Utf8::Encode((uint32_t)codepoint);
    s.insert(pos, ch);
    m_CaretIndex = (int)(pos + ch.size());
    // The caret moved past whatever was selected before; leaving the old pair
    // in place would anchor the next shift-extend at a stale position.
    ClearSelection();
    SetValue(s);
    NotifyValueChanging();
    NotifyValueChanged();
    return true;
}

// Returns whether the keystroke was the field's to answer. Single-line: it does
// not claim Up/Down/PageUp/PageDown, so a search field stacked over a result
// list leaves those to the list. Claiming is per-key rather than "a field has
// focus" — that distinction is what keeps gameplay keys flowing while a viewport
// or a toggle holds focus.
bool TextInput::OnKey(int key, int mods, UI::IPlatformApi* platform)
{
    // A modifier's own press carries no text and belongs to whoever reads held
    // state; function keys are application-level. Neither is the field's.
    if (Input::IsModifierKey(key) || Input::IsFunctionKey(key))
        return false;

    std::string s = GetValue();
    bool shift = (mods & Input::kModShift) != 0;
    bool primaryMod = Input::IsPrimaryShortcutModifier(mods);

    // Many keys only move the caret/selection without changing the underlying text.
    // Request a geometry rebuild so the caret/selection visuals update immediately.
    MarkDirty(VisualDirty);

    // Keep the caret visibly solid while keys are held by resetting the
    // frame counter on every key event (press or repeat). This works in
    // tandem with the render-time decrement to avoid post-keypress flicker.
    BumpCaretForceVisible();

    // Best-effort local clipboard fallback: if the platform clipboard API
    // cannot round-trip text (e.g., platform-specific limitations), we still
    // want editor-to-editor copy/paste to work reliably.
    static std::string s_LastCopiedText;

    auto deleteSelection = [&]() -> bool
    {
        if (!HasSelection())
            return false;
        int a = std::clamp(std::min(m_SelectionStart, m_SelectionEnd), 0, (int)s.size());
        int b = std::clamp(std::max(m_SelectionStart, m_SelectionEnd), 0, (int)s.size());
        s.erase((size_t)a, (size_t)(b - a));
        m_CaretIndex = a;
        ClearSelection();
        return true;
    };

    // Clipboard shortcuts (best-effort: modifier bits are approximate here)
    if (primaryMod)
    {
        // Use shared keycode constants from Input/KeyCodes.h so behavior stays
        // consistent across controls.
        // Ctrl/Cmd+A: select all text in the field.
        if (key == Input::kKeyCode_A)
        {
            m_SelectionStart = 0;
            m_SelectionEnd = (int)s.size();
            m_CaretIndex = m_SelectionEnd;
            return true;
        }

        if (key == Input::kKeyCode_C || key == Input::kKeyCode_X)
        {
            if (platform && HasSelection())
            {
                int a = std::clamp(std::min(m_SelectionStart, m_SelectionEnd), 0, (int)s.size());
                int b = std::clamp(std::max(m_SelectionStart, m_SelectionEnd), 0, (int)s.size());
                std::string slice = s.substr((size_t)a, (size_t)(b - a));
                platform->SetClipboardText(slice.c_str());
                // Cache locally so paste can still work even if the platform
                // clipboard string cannot be read back for some reason.
                s_LastCopiedText = slice;
                if (key == Input::kKeyCode_X)
                {
                    BeginEditingSession();
                    s.erase((size_t)a, (size_t)(b - a));
                    m_CaretIndex = a;
                    ClearSelection();
                    SetValue(s);
                    NotifyValueChanging();
                    NotifyValueChanged();
                }
            }
            // Copy and cut are the field's gesture whether or not a selection
            // existed to act on, the same way an end-stop arrow key still
            // belongs to the field.
            return true;
        }
        else if (key == Input::kKeyCode_V)
        {
            // Prefer platform clipboard contents when available, but fall
            // back to the last copied text from this control if the
            // platform clipboard is empty/unavailable.
            std::string clip;
            if (platform)
            {
                clip = platform->GetClipboardText();
            }
            if (clip.empty())
            {
                clip = s_LastCopiedText;
            }
            if (!clip.empty())
            {
                deleteSelection();
                BeginEditingSession();
                size_t p = (size_t)std::clamp(m_CaretIndex, 0, (int)s.size());
                s.insert(p, clip);
                m_CaretIndex = (int)(p + clip.size());
                ClearSelection();
                SetValue(s);
                NotifyValueChanging();
                NotifyValueChanged();
            }
            return true;
        }
    }

    // Important key handling: navigation/editing plus Enter/Escape.
    switch (key)
    {
    case Input::kKeyCode_Backspace:
        if (deleteSelection())
        {
            BeginEditingSession();
            SetValue(s);
            NotifyValueChanging();
            NotifyValueChanged();
        }
        else
        {
            size_t pos = (size_t)std::clamp(m_CaretIndex, 0, (int)s.size());
            if (pos > 0)
            {
                BeginEditingSession();
                size_t prev = Utf8::Prev(s, pos);
                s.erase(prev, pos - prev);
                m_CaretIndex = (int)prev;
                ClearSelection();
                SetValue(s);
                NotifyValueChanging();
                NotifyValueChanged();
            }
        }
        return true;
    case Input::kKeyCode_Delete:
        if (deleteSelection())
        {
            BeginEditingSession();
            SetValue(s);
            NotifyValueChanging();
            NotifyValueChanged();
        }
        else
        {
            size_t pos = (size_t)std::clamp(m_CaretIndex, 0, (int)s.size());
            if (pos < s.size())
            {
                BeginEditingSession();
                size_t next = Utf8::Next(s, pos);
                s.erase(pos, next - pos);
                ClearSelection();
                SetValue(s);
                NotifyValueChanging();
                NotifyValueChanged();
            }
        }
        return true;
    case Input::kKeyCode_Left:
    {
        size_t pos = (size_t)std::clamp(m_CaretIndex, 0, (int)s.size());
        int newCaret = primaryMod ? (int)TextSegmentation::PrevBoundary(s, pos)
                                  : (int)Utf8::Prev(s, pos);
        if (shift)
        {
            AnchorSelectionForExtend();
            m_CaretIndex = newCaret;
            m_SelectionEnd = m_CaretIndex;
        }
        else
        {
            m_CaretIndex = newCaret;
            ClearSelection();
        }
        return true;
    }
    case Input::kKeyCode_Right:
    {
        size_t pos = (size_t)std::clamp(m_CaretIndex, 0, (int)s.size());
        int newCaret = primaryMod ? (int)TextSegmentation::NextBoundary(s, pos)
                                  : (int)Utf8::Next(s, pos);
        if (shift)
        {
            AnchorSelectionForExtend();
            m_CaretIndex = newCaret;
            m_SelectionEnd = m_CaretIndex;
        }
        else
        {
            m_CaretIndex = newCaret;
            ClearSelection();
        }
        return true;
    }
    case Input::kKeyCode_Home:
        if (shift)
        {
            AnchorSelectionForExtend();
            m_CaretIndex = 0;
            m_SelectionEnd = m_CaretIndex;
        }
        else
        {
            m_CaretIndex = 0;
            ClearSelection();
        }
        return true;
    case Input::kKeyCode_End:
        if (shift)
        {
            AnchorSelectionForExtend();
            m_CaretIndex = (int)s.size();
            m_SelectionEnd = m_CaretIndex;
        }
        else
        {
            m_CaretIndex = (int)s.size();
            ClearSelection();
        }
        return true;
    case Input::kKeyCode_Enter:
    case Input::kKeyCode_NumPadEnter:
        // Commit the current value, then select the whole (reformatted) value so
        // the next keystroke replaces it. This makes entering many values into
        // the same field fast: type, Enter, type, Enter, ... one Enter per value,
        // no mouse or Ctrl+A. For single-line TextInput we treat Enter as "done";
        // multi-line controls (TextArea) use a different class. NumPad Enter
        // behaves identically.
        CommitEditing();
        SelectAll();
        return true;
    case Input::kKeyCode_Escape:
    {
        // Revert to the value that was present when the field gained focus
        // and cancel the editing session. With no session to cancel this key
        // was not ours: Escape then belongs to the popup or the play session
        // behind us, which is what lets one Escape clear a search field and
        // the next close the dialog holding it.
        const bool wasEditing = m_IsEditing;
        CancelEditing();
        return wasEditing;
    }
    default:
        // The paired character event is what this field acts on, so a keystroke
        // that composes one is claimed here. Chords this field implements
        // (select-all, copy/cut/paste, word-wise Left/Right) already returned
        // above; a chord that composes nothing — primary-modifier anywhere,
        // Alt+letter outside macOS — bubbles so the application's binding for
        // it still fires with the caret sitting here. Vertical and page
        // navigation is deliberately absent too, so a list under a search
        // field still receives it.
        return Input::ComposesTextInput(key, mods);
    }
}

void TextInput::OnFocusChanged(bool focused)
{
    if (focused)
    {
        // Capture the current value at the start of a focus session so that
        // Escape can restore it, but do not move the caret or selection.
        //
        // Caret placement is owned by the actual interaction that caused
        // focus (mouse click, drag, keyboard navigation). Overwriting the
        // caret here would fight with TextInput::OnPointerDown/OnPointerDrag
        // and make click-based caret placement impossible.
        m_OriginalValue = GetValue();
        m_IsEditing = false; // BeginEditingSession will mark true on first edit.
        // Leave m_CaretIndex / selection as-is.

        // When a field gains focus we want the caret to be clearly visible
        // immediately, regardless of the blink phase.
        BumpCaretForceVisible();
        MarkDirty(VisualDirty);
    }
    else
    {
        // Losing focus commits any in-progress edit and clears selection.
        CommitEditing();
        ClearSelection();
        MarkDirty(VisualDirty);
    }
}

// Called from the interactions that move the caret (keyboard navigation,
// typing, pointer down/drag, focus change) so it is never in its hidden half
// at the moment the user looks for where it went.
void TextInput::BumpCaretForceVisible()
{
    float now = 0.0f;
    if (UIManager* ui = GetOwnerManager())
    {
        now = ui->GetTimeSeconds();
    }
    // Read per bump rather than cached, for the same reason the render path
    // reads the rate per frame: an OS caret setting changed while the editor
    // runs takes effect without a restart.
    m_CaretForceVisible.Bump(now, Platform::GetCaretBlinkHalfPeriod());
}

bool TextInput::HasSelection() const
{
    return m_SelectionStart >= 0 && m_SelectionEnd >= 0 && m_SelectionStart != m_SelectionEnd;
}

void TextInput::AnchorSelectionForExtend()
{
    if (!HasSelection())
        m_SelectionStart = m_CaretIndex;
}

void TextInput::ClearSelection()
{
    m_SelectionStart = m_SelectionEnd = -1;
}

void TextInput::SelectAll()
{
    const int len = static_cast<int>(GetValue().size());
    m_SelectionStart = 0;
    m_SelectionEnd = len;
    m_CaretIndex = len;
    m_HoldPressSelectionUntilPointerMoves = true;
    BumpCaretForceVisible();
    MarkDirty(VisualDirty);
}

void TextInput::SelectWordAt(float mouseX, float x, float y, float W, float H,
                             const ResolvedStyle& style, FontAtlas* font)
{
    const std::string& text = GetValue();
    if (text.empty())
    {
        m_CaretIndex = 0;
        ClearSelection();
        BumpCaretForceVisible();
        MarkDirty(VisualDirty);
        return;
    }

    // The click resolves to a caret boundary, the same way a single click does,
    // and the segment is selected from there. Measured against Chrome: word
    // selection flips at the glyph midpoint exactly as caret placement does.
    const PointerGeometry geo = BuildPointerGeometry(style, font, x, y, W, H);
    const int boundary = FindCaretIndexForScreenX(*geo.CaretXByBytePhysical, geo.TextOriginX,
                                                  geo.InvContentScale, mouseX);
    const TextSegmentation::ByteRange word =
        TextSegmentation::DoubleClickSelectionAt(text, static_cast<size_t>(boundary));

    m_SelectionStart = static_cast<int>(word.Begin);
    m_SelectionEnd = static_cast<int>(word.End);
    m_CaretIndex = m_SelectionEnd;
    m_HoldPressSelectionUntilPointerMoves = true;
    BumpCaretForceVisible();
    MarkDirty(VisualDirty);
}

const Rendering::Text::FontAtlas::MeasureResult& TextInput::EnsureMeasureCache(
    Rendering::Text::FontAtlas* font, float pixelSize, float letterSpacingPx) const
{
    const std::string& value = GetValue();
    const std::uintptr_t fptr = reinterpret_cast<std::uintptr_t>(font);
    std::uint64_t key = 14695981039346656037ull;
    // FNV-1a over value bytes, pixelSize, letter-spacing, and font pointer
    for (char c : value)
    {
        key ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        key *= 1099511628211ull;
    }
    auto fnv = [](const void* data, size_t len, std::uint64_t h) -> std::uint64_t {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= 1099511628211ull; }
        return h;
    };
    key = fnv(&pixelSize, sizeof(pixelSize), key);
    key = fnv(&letterSpacingPx, sizeof(letterSpacingPx), key);
    key = fnv(&fptr, sizeof(fptr), key);

    if (!m_MeasureCacheValid || m_MeasureCacheKey != key)
    {
        m_MeasureCache = font->MeasureText(value, pixelSize, letterSpacingPx);
        m_MeasureCacheKey = key;
        m_MeasureCacheValid = true;
    }
    return m_MeasureCache;
}

void TextInput::UpdateScrollToCaret(FontAtlas* font, float pixelSize,
                                    float letterSpacingPx, float innerWidth,
                                    float contentScale) const
{
    if (!font || innerWidth <= 0.0f)
    {
        m_TextScrollX = 0.0f;
        return;
    }

    const FontAtlas::MeasureResult& measure =
        EnsureMeasureCache(font, pixelSize, letterSpacingPx);
    const float textWidth = measure.metrics.width;

    // Fitting content never scrolls (safety — the caller already gates on
    // the shaped width overflowing the box).
    if (textWidth <= innerWidth)
    {
        m_TextScrollX = 0.0f;
        return;
    }

    const float cs = std::max(0.01f, contentScale);
    const float maxScroll = textWidth - innerWidth;
    float scroll = std::clamp(m_TextScrollX * cs, 0.0f, maxScroll);

    const std::vector<float>& caretMap = measure.caretXByByte;
    float caretLocal = 0.0f;
    if (!caretMap.empty())
    {
        const size_t idx = static_cast<size_t>(
            std::clamp(m_CaretIndex, 0, static_cast<int>(caretMap.size()) - 1));
        caretLocal = caretMap[idx];
    }

    // Slide the window the minimal amount that keeps the caret visible.
    if (caretLocal - scroll > innerWidth)
        scroll = caretLocal - innerWidth;
    else if (caretLocal - scroll < 0.0f)
        scroll = caretLocal;

    m_TextScrollX = std::clamp(scroll, 0.0f, maxScroll) / cs;
}

} // namespace GameEngine
