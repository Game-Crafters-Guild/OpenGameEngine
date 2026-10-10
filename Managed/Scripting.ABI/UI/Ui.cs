using System;
using System.Runtime.InteropServices;
using System.Text;

namespace GameEngine.Scripting
{
    /// <summary>
    /// Managed façade over the game UI, split the same way the native ABI is: one entry point
    /// knows what an entity is (<see cref="FindElement"/>, which resolves an element inside a
    /// UIDocument's mounted subtree), and everything you then do to that element hangs off a
    /// <see cref="Element"/> wrapper that knows nothing about entities, documents or the ECS.
    ///
    /// Safety: the binding never holds a native UIElement pointer. A wrapper carries one
    /// integer — the element's process-wide instance id, which is never reused — and every
    /// operation re-resolves it inside the engine. A .uxml hot-reload that replaced the
    /// element, play-stop, a disabled UIDocument or host teardown therefore makes the wrapper
    /// report not-alive forever after, rather than aliasing whatever took its place.
    ///
    /// The wrapper owns NOTHING native: no IDisposable, no finalizer. The element's lifetime
    /// belongs to the retained tree. The only thing that ever needs revoking is a
    /// SUBSCRIPTION, and an undisposed one is revoked for you when its scripts load context
    /// unloads (see the event accessors on <see cref="Element"/>).
    ///
    /// Resolve once, keep the wrapper: <see cref="FindElement"/> walks mounted documents by
    /// string id, and it returns the SAME C# instance for the same live element every time,
    /// so a subclass you built over an element keeps its state across lookups.
    ///
    /// Threading: UI state isn't internally locked; the host ticks it on the main thread. Call
    /// these from a system's main-thread update, and subscribe/unsubscribe from there too.
    /// Event callbacks fire on the main thread during the host's event routing.
    /// </summary>
    public static partial class Ui
    {
        private const string kLib = "GameEngine.Native";

        // Document layer — the only P/Invoke here that mentions an entity.
        // entityId == 0 searches every mounted document; otherwise scopes to that UIDocument.
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_GameUI_FindElement(ulong entityId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string id, out ulong outInstanceId);

        // Element layer — instance id and nothing else.
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_UIElement_SetWidthPercent(ulong instanceId, float pct);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_UIElement_SetLabelText(ulong instanceId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string text);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_UIElement_SetClass(ulong instanceId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string className, int on);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_UIElement_IsAlive(ulong instanceId, out int outAlive);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_UIElement_GetTagId(ulong instanceId, out ulong outTagId);

        // UI-module layer: neither an entity nor an element. Asking the engine what a tag name
        // hashes to is what keeps the hash a single source of truth — a managed re-implementation
        // would map every button onto the base wrapper the day the hash changed, silently.
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_UI_GetTagId(
            [MarshalAs(UnmanagedType.LPUTF8Str)] string tagLower, out ulong outTagId);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern unsafe int GE_UIElement_RegisterEvent(ulong instanceId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string eventName,
            delegate* unmanaged[Cdecl]<NativeEventData*, nint, void> cb, nint user, out ulong outToken);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_UIElement_UnregisterEvent(ulong instanceId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string eventName, ulong token);

        // Typed value accessors — one export per value SHAPE, so a wrapper property is a
        // P/Invoke and nothing else: no string parse, no attribute-handler walk. `notify`
        // non-zero routes the control's own SetValue (its clamping AND its own notification
        // contract); zero routes SetValueWithoutNotify.
        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_UIElement_GetValueFloat(ulong instanceId, out float outValue);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_UIElement_SetValueFloat(ulong instanceId, float value, int notify);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_UIElement_GetValueBool(ulong instanceId, out int outValue);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_UIElement_SetValueBool(ulong instanceId, int value, int notify);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern unsafe int GE_UIElement_GetValueText(ulong instanceId, byte* buffer,
            int bufferLen, out int outLen);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_UIElement_SetValueText(ulong instanceId,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string text, int notify);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_UIElement_GetScrollOffset(ulong instanceId, out float outX,
            out float outY);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_UIElement_SetScrollX(ulong instanceId, float x);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_UIElement_SetScrollY(ulong instanceId, float y);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_UIElement_GetDropdownSelectedIndex(ulong instanceId, out int outIndex);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern int GE_UIElement_SetDropdownSelectedIndex(ulong instanceId, int index,
            int notify);

        [DllImport(kLib, CallingConvention = CallingConvention.Cdecl)]
        private static extern unsafe int GE_UIElement_GetDropdownSelectedLabel(ulong instanceId,
            byte* buffer, int bufferLen, out int outLen);

        private enum TextSource
        {
            FieldValue,
            DropdownLabel,
        }

        // Covers virtually every real field value and dropdown label in one call; anything
        // longer costs a single exact-size retry, never a truncated answer.
        private const int kTextReadStackBytes = 256;

        /// <summary>
        /// The shared read behind the string-returning properties. The native contract is
        /// copy-what-fits, always-report-the-full-length: one stack buffer covers virtually
        /// every real value, and a longer one costs a single exact-size retry. Returning a
        /// string allocates by nature — these are PROPERTY reads; the event dispatch path
        /// never comes through here, which is what <see cref="Utf8Text"/> exists for.
        /// </summary>
        private static unsafe string ReadElementText(ulong instanceId, TextSource source)
        {
            int Get(byte* buffer, int bufferLen, out int outLen)
                => source == TextSource.FieldValue
                       ? GE_UIElement_GetValueText(instanceId, buffer, bufferLen, out outLen)
                       : GE_UIElement_GetDropdownSelectedLabel(instanceId, buffer, bufferLen, out outLen);

            byte* stack = stackalloc byte[kTextReadStackBytes];
            if (Get(stack, kTextReadStackBytes, out int len) != 0 || len == 0)
                return string.Empty;
            if (len <= kTextReadStackBytes)
                return Encoding.UTF8.GetString(stack, len);

            byte[] heap = new byte[len];
            fixed (byte* p = heap)
            {
                if (Get(p, len, out int retryLen) != 0 || retryLen == 0)
                    return string.Empty;
                return Encoding.UTF8.GetString(p, Math.Min(retryLen, len));
            }
        }

        /// <summary>
        /// Mirror of the ABI's GE_UIEventData. Blittable and read-only for the duration of the
        /// callback; the binding copies what it needs into the public args structs.
        /// <para>
        /// The offsets are the contract, and nothing here enforces them: the callback is bound as
        /// a pointer, so the runtime reads native bytes through THIS layout with no marshaller
        /// reconciling the two, and a field inserted, reordered or resized re-points every read
        /// below without failing to compile. <c>UiEventDataLayoutTests</c> (Managed/InteropTests)
        /// is the offset pin on this side; the static_assert block under GE_UIEventData in
        /// Engine/Include/Scripting/ScriptingABI.h is the one on the other. Changing this struct
        /// means changing both.
        /// </para>
        /// </summary>
        internal struct NativeEventData
        {
            internal ulong ElementInstanceId;
            internal ulong EventId;
            internal float X;
            internal float Y;
            internal int Button;
            internal int Mods;
            internal float Value;
            internal float ScrollX;
            internal float ScrollY;
            // UTF-8 bytes with an explicit length, NOT NUL-terminated; null/0 for events that
            // carry no text. Valid only for the duration of the callback — native points this
            // at a dispatch-owned copy, and the binding hands it on as a Utf8Text view that
            // the compiler refuses to let outlive the handler.
            internal nint Text;
            internal uint TextLen;
        }

        /// <summary>
        /// The native event names this binding subscribes by. The ABI hashes them, so these
        /// strings — not a managed enum — are the only thing that has to match the engine, and
        /// a name the engine does not know is rejected at subscribe time rather than becoming a
        /// subscription that can never fire.
        /// </summary>
        internal static class EventNames
        {
            internal const string ButtonClick = "UI.ButtonClick";
            internal const string MouseEnter = "UI.MouseEnter";
            internal const string MouseMove = "UI.MouseMove";
            internal const string FocusIn = "UI.FocusIn";
            internal const string ValueChanging = "UI.ValueChanging";
            internal const string ValueChanged = "UI.ValueChanged";
            internal const string ScrollOffsetChanged = "UI.ScrollOffsetChanged";
            // The element joined, or left, a tree that reaches the panel's root — UI
            // Toolkit's AttachToPanelEvent / DetachFromPanelEvent. Announced once per
            // frame from the settle, so a reload that re-parents an element without
            // replacing it raises neither.
            //
            // DetachedFromPanel fires for a keep-alive unlink (next frame) AND for
            // destruction (synchronously, before teardown), so unsubscribing from external
            // state in it is sound: an element announced attached is always announced
            // detached before it goes away.
            internal const string AttachedToPanel = "UI.AttachedToPanel";
            internal const string DetachedFromPanel = "UI.DetachedFromPanel";
        }

        /// <summary>
        /// Resolve an element by id inside a mounted UIDocument subtree. Returns null while the
        /// subtree is still loading (poll across frames), when nothing matches, or when the id
        /// is null/empty — an empty id is rejected rather than matched, because natively it
        /// would match the first element that has no id.
        /// <para>
        /// The returned wrapper is typed from the element's own tag: a &lt;button&gt; comes back
        /// as <see cref="Button"/>, a &lt;label&gt; as <see cref="Label"/>, anything else as
        /// <see cref="Element"/>. It is also the SAME instance every lookup returns for that
        /// element while it lives, so state on a subclass of yours survives re-resolution.
        /// </para>
        /// <para>
        /// entityId == 0 searches every mounted document and resolves a shared id to the lowest
        /// owning entity id, so the winner is stable. Pass the owning UIDocument entity to scope
        /// the search to one subtree.
        /// </para>
        /// </summary>
        public static Element? FindElement(string elementId, ulong entityId = 0)
        {
            if (GE_GameUI_FindElement(entityId, elementId ?? string.Empty, out ulong instanceId) != 0)
                return null;
            return ElementRegistry.Resolve(instanceId);
        }

        /// <summary>What a pointer event carries. Positions are in the host's UI space.</summary>
        public readonly struct PointerArgs
        {
            internal PointerArgs(float x, float y, int mods)
            {
                X = x;
                Y = y;
                Mods = mods;
            }

            /// <summary>Pointer X in the host's UI space.</summary>
            public readonly float X;

            /// <summary>Pointer Y in the host's UI space.</summary>
            public readonly float Y;

            /// <summary>Modifier-key bitmask as the engine reports it.</summary>
            public readonly int Mods;
        }

        /// <summary>
        /// What a click carries. Deliberately no position: a Button also activates from
        /// Space/Enter while focused, which has no click point, so there would be no honest
        /// value to put there. Read the position from a pointer event if you need it.
        /// </summary>
        public readonly struct ClickArgs
        {
            internal ClickArgs(int mods) => Mods = mods;

            /// <summary>Modifier-key bitmask as the engine reports it.</summary>
            public readonly int Mods;
        }

        /// <summary>
        /// What a value change carries: the value the control now holds. Raised by the
        /// control's own notification, so it is the clamped, quantised, committed value —
        /// exactly what a drag produces — not the raw thing someone assigned.
        /// <para>
        /// Only controls whose value type fits this payload raise it (a float slider, a bool
        /// toggle); subscribing on one that does not is refused at subscribe time rather than
        /// becoming an event that never arrives. A string-valued control raises the same
        /// native events with a <see cref="TextValueArgs"/> payload instead.
        /// </para>
        /// </summary>
        public readonly struct ValueArgs
        {
            internal ValueArgs(float value) => Value = value;

            /// <summary>The control's new value. A bool control reports 0 or 1.</summary>
            public readonly float Value;

            /// <summary>The same value read as a flag, for bool-valued controls.</summary>
            public bool AsBool => Value != 0.0f;
        }

        /// <summary>
        /// What a STRING-VALUED control's value change carries: the new value as a
        /// <see cref="Utf8Text"/> view. A TextField reports its text; a Dropdown reports the
        /// selected option's VALUE string (read the label from the wrapper if you need it).
        /// <para>
        /// A ref struct, deliberately: the view points at native bytes that are valid only for
        /// this dispatch, so the compiler refuses every way to keep it — fields, closures,
        /// boxing. Keeping the text means saying so: <c>args.Text.ToString()</c> is the one
        /// visible copy. Comparing and measuring cost nothing.
        /// </para>
        /// </summary>
        public readonly ref struct TextValueArgs
        {
            internal TextValueArgs(Utf8Text text) => Text = text;

            /// <summary>The control's new value, as UTF-8 bytes valid for this handler call.</summary>
            public readonly Utf8Text Text;
        }

        /// <summary>
        /// What a scroll-offset change carries: where the view now sits, in pixels. Not a
        /// wheel delta — this is the settled offset, coalesced to one event per frame however
        /// many writes produced it.
        /// </summary>
        public readonly struct ScrollOffsetArgs
        {
            internal ScrollOffsetArgs(float scrollX, float scrollY)
            {
                ScrollX = scrollX;
                ScrollY = scrollY;
            }

            /// <summary>Horizontal scroll offset in pixels.</summary>
            public readonly float ScrollX;

            /// <summary>Vertical scroll offset in pixels.</summary>
            public readonly float ScrollY;
        }
    }
}
