using GameEngine.Scripting;

namespace Demo
{
    // A C#-DEFINED ELEMENT TYPE. <hp-pips/> in GameHudInteractive.uxml is this class: the engine
    // has no such control, the tag exists because this attribute registered it, and the element
    // in the tree is a native proxy whose behaviour lives here.
    //
    // It is written to be WATCHABLE across a hot reload, which is what this fixture is for:
    //
    //   kBuildClass  a version marker put on the element by the constructor. Edit it, save, and
    //                the class on the live element changes — proof that the element came back as
    //                the NEW code rather than surviving as the old instance.
    //   Percent      an authored .uxml attribute. The engine replays it onto every fresh
    //                instance after a reload, so the bar returns to the width the DOCUMENT asked
    //                for, not to this class's default. That is the "re-materialize with state"
    //                half: the element keeps its identity, its place in the tree and its
    //                authored configuration across a reload of the assembly that defines it.
    //
    // Deleting this file (or just the attribute) is the other half of the loop: the tag does not
    // come back, and the element stays in the tree as an inert container carrying the engine's
    // ge-missing-type badge class.
    [UiElement("hp-pips")]
    public class HpPipsElement : Ui.Element
    {
        // Bumped by hand when hot-editing this file, so the live element names the build it is
        // running. It is a CSS class rather than a property because that is how a control tells
        // the tree about itself here — the stylesheet decides what it looks like.
        private const string kBuildClass = "pips-v1";

        private const float kLowPercent = 50f;

        private float m_Percent = 100f;

        // The line this prints is the loop's instrument, and it is what makes a run readable:
        // the BUILD tells you which edit of this file the live element is running, and the two
        // flags say whether the element was writable from here. Capture it by launching the
        // editor with stdout redirected — managed Console output does not reach -logfile.
        //
        // WHAT THE FLAGS MEAN, MEASURED. On the INITIAL document build both read False, because
        // the element is not yet reachable from any root — the factory mints it and the builder
        // parents it afterwards, so the write has nothing to land on. On re-materialization after
        // a hot reload both read True: that element is already in the tree. Writing from a
        // constructor is therefore not reliable yet, and it is a known defect rather than a rule
        // to design around (section 13.4 of the managed-UIElement design has the two ways to
        // close it). Attribute setters are not a way around it — authored attributes are applied
        // before the element is parented too.
        //
        // An earlier reading blamed the whole managed element-resolution path. That was wrong:
        // the Game View tab had never been activated, so no gameplay UI host was published and
        // every lookup missed. Activate the Game View before drawing conclusions here.
        public HpPipsElement()
        {
            bool alive = IsAlive;
            bool classed = AddClass(kBuildClass);
            System.Console.WriteLine($"[HpPips] ctor build={kBuildClass} alive={alive} addClass={classed}");
            Apply();
        }

        /// <summary>Fill width, 0..100, as authored in the document: <c>percent="72"</c>.</summary>
        [UiAttribute]
        public float Percent
        {
            set
            {
                m_Percent = value;
                Apply();
            }
        }

        private void Apply()
        {
            SetWidthPercent(m_Percent);
            if (m_Percent <= kLowPercent)
                AddClass("pips-low");
            else
                RemoveClass("pips-low");
        }
    }
}
