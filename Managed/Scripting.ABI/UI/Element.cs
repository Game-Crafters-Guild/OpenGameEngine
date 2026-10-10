using System;

namespace GameEngine.Scripting
{
    public static partial class Ui
    {
        /// <summary>
        /// A live UI element, addressed by its native instance id.
        /// <para>
        /// <b>It owns nothing.</b> There is no Dispose and no finalizer, because there is no
        /// native allocation behind this object to free — the element belongs to the retained
        /// UI tree, exactly as it did before any script looked at it. Forgetting a wrapper
        /// leaks nothing.
        /// </para>
        /// <para>
        /// Every operation re-resolves the instance id and returns false if it no longer
        /// resolves. That is how a script learns its element died; re-<see cref="FindElement"/>
        /// to pick up a replacement. Nothing here throws across the native boundary.
        /// A successful write is not necessarily a VISIBLE write: an Overlay document
        /// suppressed by a ready Fullscreen document is hidden with display:none, so writes
        /// land and show up when it is shown again, while pointer events never fire because a
        /// hidden subtree does not hit-test.
        /// </para>
        /// <para>
        /// <c>==</c> is plain C# reference equality and <c>null</c> means null. This
        /// deliberately does NOT copy UnityEngine.Object, which overloads <c>==</c> so a
        /// destroyed object compares equal to null while still being a live reference —
        /// <c>?.</c> and <c>??</c> bypass that overload and disagree with <c>== null</c>.
        /// Ask <see cref="IsAlive"/> instead.
        /// </para>
        /// <para>
        /// <b>Subclass for composition, subscribe for behaviour.</b> A custom control is a
        /// subclass that owns state and child wrappers and wires its own subscriptions — there is
        /// nothing virtual to override. In THIS pattern — a wrapper constructed over an element
        /// the document already contains — the constructor is the place to wire them, because the
        /// element it wraps is already in the tree. A <see cref="UiElementAttribute"/> type is the
        /// other pattern and does not have that property yet; see the protected constructor
        /// below before writing to the element from one.
        /// <code>
        /// class HealthBar : Ui.Element
        /// {
        ///     private readonly Ui.Element m_Fill;
        ///     public HealthBar(Ui.Element root, Ui.Element fill) : base(root) { m_Fill = fill; }
        ///     public void SetPercent(float p) => m_Fill.SetWidthPercent(p * 100f);
        /// }
        /// </code>
        /// Constructing a wrapper CLAIMS that element's identity, so every later
        /// <see cref="FindElement"/> for it returns your instance.
        /// </para>
        /// </summary>
        public class Element
        {
            /// <summary>
            /// The element's process-wide native instance id. Never reused, so it either names
            /// this element or nothing at all.
            /// </summary>
            internal readonly ulong InstanceId;

            internal Element(ulong instanceId)
            {
                InstanceId = instanceId;
                ElementRegistry.Claim(this);
            }

            // The element a [UiElement] type is being constructed over, handed from the factory
            // to the parameterless constructor below.
            //
            // A thread-static rather than a constructor parameter so that a user's type needs
            // nothing but `public HealthBar() { }` — a required `base(instanceId)` would put an
            // engine detail in every custom control's signature, and passing it after
            // construction would leave InstanceId invalid inside the constructor, which is
            // exactly where a control wires its children and subscriptions. Set and cleared
            // around one non-reentrant call in ElementTypes.Create.
            [ThreadStatic] private static ulong s_ConstructingInstanceId;

            internal static ulong ConstructingInstanceId
            {
                get => s_ConstructingInstanceId;
                set => s_ConstructingInstanceId = value;
            }

            /// <summary>
            /// The constructor a <see cref="UiElementAttribute"/> type inherits. The engine
            /// builds the native element first and constructs this over it.
            /// <para>
            /// <b>Writes from a constructor body are not reliable yet, and this is a known
            /// defect rather than a rule to design around.</b> On the INITIAL document build the
            /// element is not root-reachable when its factory runs, so
            /// <see cref="IsAlive"/> reads false and every write is silently dropped; the same
            /// constructor re-materialized into an already-mounted tree after a hot reload reads
            /// true and its writes land. <see cref="UiAttributeAttribute"/> members are NOT a way
            /// around it: the builder applies authored attributes before it parents the element
            /// too, so a setter that writes is dropped for the same reason. Do not read this
            /// asymmetry as the intended contract — which of the two ways to close it is taken is
            /// an open design item (section 13.4 of the managed-UIElement design).
            /// </para>
            /// </summary>
            /// <exception cref="InvalidOperationException">
            /// Thrown when constructed directly. A custom element type is instantiated by the
            /// engine when its tag appears in a document; there is no element to wrap otherwise.
            /// </exception>
            protected Element()
            {
                ulong id = s_ConstructingInstanceId;
                if (id == 0)
                {
                    throw new InvalidOperationException(
                        "A [UiElement] type is constructed by the engine when its tag appears in " +
                        "a .uxml document, so it cannot be constructed directly. To wrap an " +
                        "element the document already contains, use the Element(Element) " +
                        "constructor instead.");
                }
                InstanceId = id;
                ElementRegistry.Claim(this);
            }

            /// <summary>
            /// Build a custom control over an element the document already contains. The new
            /// instance takes over that element's identity: <see cref="FindElement"/> returns
            /// it from then on, so a later lookup does not hand back a bare
            /// <see cref="Element"/> and lose your state.
            /// </summary>
            protected Element(Element source)
                : this((source ?? throw new ArgumentNullException(nameof(source))).InstanceId)
            {
            }

            /// <summary>
            /// Does this element still resolve? False once it was destroyed, detached from the
            /// tree, or its host torn down — and false forever after, because instance ids are
            /// never reused.
            /// </summary>
            public bool IsAlive => GE_UIElement_IsAlive(InstanceId, out int alive) == 0 && alive != 0;

            /// <summary>
            /// Add a CSS class. This is how a HUD shows/hides and expresses state — there is no
            /// SetVisible, because visibility is a style the stylesheet owns. False if the
            /// element no longer resolves or the name is empty.
            /// </summary>
            public bool AddClass(string className)
                => GE_UIElement_SetClass(InstanceId, className ?? string.Empty, 1) == 0;

            /// <summary>Remove a CSS class. Removing one that is absent is a no-op, not a failure.</summary>
            public bool RemoveClass(string className)
                => GE_UIElement_SetClass(InstanceId, className ?? string.Empty, 0) == 0;

            /// <summary>Set a bar/fill element's width as a percentage (0..100).</summary>
            public bool SetWidthPercent(float pct)
                => GE_UIElement_SetWidthPercent(InstanceId, pct) == 0;

            /// <summary>
            /// The pointer entered this element (the hover transition, not every move).
            /// <para>
            /// Explicit accessors, not a field-like event: the FIRST <c>+=</c> creates exactly
            /// one native handler-table subscription and the LAST <c>-=</c> removes it, so N
            /// listeners cost one native entry and one reverse P/Invoke per occurrence — not N
            /// of each. An event nobody subscribed costs nothing at all; subscribing IS the
            /// wiring. Each listener is invoked inside its own try, so a throwing one neither
            /// stops the others nor reaches the native boundary.
            /// </para>
            /// <para>
            /// <c>+=</c> on an element that no longer resolves is a no-op that subscribes
            /// nothing, and the event stays subscribable. An undisposed subscription is revoked
            /// when its scripts load context unloads, so a forgotten <c>-=</c> across a hot
            /// reload does not pin that context — but unsubscribe when a listener's job ends
            /// anyway.
            /// </para>
            /// <para>
            /// Subscriptions belong to the ELEMENT, not to this object. Every wrapper for one
            /// element shares them, so a listener added through one wrapper can be removed
            /// through another — which is what makes a subclass built over an already-subscribed
            /// element, or a wrapper the GC collected and a lookup re-minted, behave as the same
            /// element rather than as a second one.
            /// </para>
            /// </summary>
            public event Action<PointerArgs> PointerEntered
            {
                add => EventSubscription.Add(InstanceId, EventNames.MouseEnter, value);
                remove => EventSubscription.Remove(InstanceId, EventNames.MouseEnter, value);
            }

            /// <summary>
            /// The pointer moved over this element. High-frequency by nature and it costs
            /// nothing until subscribed — that is the point of subscribing being the wiring —
            /// but a listener here runs once per motion event, so keep it cheap.
            /// </summary>
            public event Action<PointerArgs> MouseMoved
            {
                add => EventSubscription.Add(InstanceId, EventNames.MouseMove, value);
                remove => EventSubscription.Remove(InstanceId, EventNames.MouseMove, value);
            }

            /// <summary>This element gained keyboard focus.</summary>
            public event Action FocusGained
            {
                add => EventSubscription.Add(InstanceId, EventNames.FocusIn, value);
                remove => EventSubscription.Remove(InstanceId, EventNames.FocusIn, value);
            }
        }
    }
}
