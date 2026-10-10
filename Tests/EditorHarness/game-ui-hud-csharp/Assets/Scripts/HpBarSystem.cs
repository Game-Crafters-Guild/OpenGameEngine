using System;
using GameEngine.Scripting;

namespace Demo
{
    // C# counterpart of the C++ HpBarSystem (Tests/EditorHarness/game-ui-hud). Demonstrates
    // a MANAGED user script reacting to a game-UI click end-to-end, through the Ui binding:
    //   1. resolve "damage-btn", "hp-fill" and "hp-label" once they have mounted (Ui.FindElement),
    //   2. subscribe to the button's Clicked event, and
    //   3. on click, decrement HP and re-drive the bar (SetWidthPercent) + label (SetText).
    // Resolution happens ONCE per element and the script keeps the wrappers. FindElement types
    // them from the element's own tag, so "damage-btn" comes back as a Ui.Button with a Clicked
    // event and "hp-label" as a Ui.Label with SetText — a wrong id shows up as a failed cast here
    // rather than as a call that silently does nothing.
    //
    // Nothing here holds a native pointer: a wrapper is one integer (an instance id that is never
    // reused), every call re-resolves it inside the engine, and the wrapper owns nothing — there
    // is no Dispose on it, only on what a subscription costs.
    //
    // Elements can die under a script (a .uxml hot reload that replaced them, a document unmount),
    // and they do not have to die together — a reload can replace the button while the fill and
    // label survive. The recovery is the one the API's contract implies: ask IsAlive, and drop the
    // wrappers so the next tick re-resolves as soon as ANY of them stops answering yes. That is why
    // the wiring below is written as "unwired until every element resolves" rather than "wired once
    // forever", and why liveness is asked of all three rather than inferred from one.
    //
    // A GameSystem is created on play-enter (OnCreate) and destroyed on play-exit (OnDestroy), so
    // HP starts fresh every play session. OnDestroy unsubscribes: play-stop leaves the HUD tree
    // standing (so edit-mode :hover/:active keep working), which means a listener left subscribed
    // would still be attached to a live button. The label's baked "-- / --" placeholder becoming
    // "100 / 100" is direct proof this system ran.
    //
    // Managed GameSystems tick in the editor's play mode (PlayModeManager drives the managed
    // OnTick) AND in the standalone Player: PlayerNeedsManagedSystemBridge is
    // `useNativeAOT || !config.disableClr`, so a CoreCLR Player registers ManagedSystemBridge too
    // (Engine/Source/Engine/Build/PackagedGameLayout.cpp).
    public class HpBarSystem : GameSystem
    {
        private const int kLowHealthPercent = 50;

        private int m_Hp = 100;
        private Ui.Element? m_Fill;
        private Ui.Label? m_Label;
        private Ui.Button? m_Button;

        public override void OnCreate()
        {
            // Fresh play session — full HP, not yet wired (defensive even if the instance is reused).
            m_Hp = 100;
            Unwire();
        }

        // Wired means all three wrappers exist AND all three still resolve. Checking only that
        // the button reference is non-null would hold a destroyed button forever; checking only
        // the fill would do the same while the bar kept animating, so nothing would look wrong.
        private bool IsWired =>
            m_Button != null && m_Button.IsAlive &&
            m_Fill != null && m_Fill.IsAlive &&
            m_Label != null && m_Label.IsAlive;

        public override void OnUpdate(float deltaTime)
        {
            if (IsWired)
                return;
            // Something we held died. Unsubscribe before re-resolving: the subscription is keyed
            // by the OLD element, and this last -= is what takes the native entry away and frees
            // the GCHandle rooting the listener. Re-resolving without it strands both.
            Unwire();

            // Poll until the subtree has mounted (async .uxml/.css load); FindElement returns null
            // until then. Resolving all three together keeps the wired state all-or-nothing.
            var button = Ui.FindElement("damage-btn") as Ui.Button;
            Ui.Element? fill = Ui.FindElement("hp-fill");
            var label = Ui.FindElement("hp-label") as Ui.Label;
            if (button == null || fill == null || label == null)
                return;

            m_Fill = fill;
            m_Label = label;
            m_Button = button;
            // Additive: subscribing here never displaces the control's own click handling or
            // another script's listener.
            m_Button.Clicked += OnTakeDamage;
            ApplyHp(); // label "-- / --" -> "100 / 100"
        }

        public override void OnDestroy()
        {
            // Play stopped: stop responding and reset the display. Unsubscribing here is also what
            // keeps the native subscription from outliving this scripts context — see
            // Ui.Element.PointerEntered for what happens when a script forgets.
            m_Hp = 100;
            ApplyHp();
            Unwire();
        }

        private void OnTakeDamage(Ui.ClickArgs args)
        {
            m_Hp = Math.Max(0, m_Hp - 10);
            ApplyHp();
        }

        private void Unwire()
        {
            if (m_Button != null)
                m_Button.Clicked -= OnTakeDamage;
            m_Button = null;
            m_Fill = null;
            m_Label = null;
        }

        // Push the current HP at the HUD. Writes to an element that died are harmless no-ops that
        // report false; noticing that and re-resolving belongs to OnUpdate, which asks IsAlive of
        // every element rather than inferring the answer from one write's return value.
        private void ApplyHp()
        {
            if (m_Fill == null || m_Label == null)
                return;
            m_Fill.SetWidthPercent(m_Hp);
            m_Label.SetText($"{m_Hp} / 100");
            // State goes across as a CSS class, never as a colour: the stylesheet decides what
            // "low health" looks like, so an artist can restyle it without touching this script.
            if (m_Hp <= kLowHealthPercent)
                m_Fill.AddClass("low-health");
            else
                m_Fill.RemoveClass("low-health");
        }
    }
}
