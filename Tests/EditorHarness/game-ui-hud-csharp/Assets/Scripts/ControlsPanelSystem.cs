using System;
using GameEngine.Scripting;

namespace Demo
{
    // The wrapper-wave fixture: a managed script driving and observing a real <slider>,
    // <toggle> and <textfield> end-to-end through the typed wrappers — Ui.Slider, Ui.Toggle,
    // Ui.TextField — with the row label under each control reporting what the script's event
    // handlers actually received. "volume: 62 (events 1)" on screen IS the proof that the
    // native control's own mutation path reached a C# handler with the right payload.
    //
    // The wiring, liveness and unsubscribe discipline are HpBarSystem.cs's, documented there.
    // What this fixture adds on top:
    //
    //   * SEEDING WITHOUT ANNOUNCING: OnCreate seeds every control through
    //     SetValueWithoutNotify, so the event counters on the labels must still read 0 until
    //     the USER touches a control. A count that starts at 1 means the silent write
    //     announced — the exact defect SetValueWithoutNotify exists to rule out.
    //
    //   * THE TEXT PAYLOAD AS A VIEW: the TextChanging/TextChanged handlers receive
    //     Ui.TextValueArgs, whose Text is a ref-struct UTF-8 view valid only for the call.
    //     Comparing (`args.Text.Equals(...)`) costs nothing; KEEPING the text is the one
    //     visible copy, ToString(). Storing the view itself does not compile.
    public class ControlsPanelSystem : GameSystem
    {
        private Ui.Slider? m_Slider;
        private Ui.Toggle? m_Toggle;
        private Ui.TextField? m_Field;
        private Ui.Label? m_VolumeLabel;
        private Ui.Label? m_MuteLabel;
        private Ui.Label? m_NameLabel;

        private int m_VolumeEvents;
        private int m_MuteEvents;
        private int m_NameChangingEvents;
        private int m_NameChangedEvents;

        public override void OnCreate()
        {
            m_VolumeEvents = 0;
            m_MuteEvents = 0;
            m_NameChangingEvents = 0;
            m_NameChangedEvents = 0;
            Unwire();
        }

        private bool IsWired =>
            m_Slider != null && m_Slider.IsAlive &&
            m_Toggle != null && m_Toggle.IsAlive &&
            m_Field != null && m_Field.IsAlive &&
            m_VolumeLabel != null && m_VolumeLabel.IsAlive &&
            m_MuteLabel != null && m_MuteLabel.IsAlive &&
            m_NameLabel != null && m_NameLabel.IsAlive;

        public override void OnUpdate(float deltaTime)
        {
            if (IsWired)
                return;
            Unwire();

            var slider = Ui.FindElement("volume-slider") as Ui.Slider;
            var toggle = Ui.FindElement("mute-toggle") as Ui.Toggle;
            var field = Ui.FindElement("name-field") as Ui.TextField;
            var volumeLabel = Ui.FindElement("volume-label") as Ui.Label;
            var muteLabel = Ui.FindElement("mute-label") as Ui.Label;
            var nameLabel = Ui.FindElement("name-label") as Ui.Label;
            if (slider == null || toggle == null || field == null ||
                volumeLabel == null || muteLabel == null || nameLabel == null)
                return;

            m_Slider = slider;
            m_Toggle = toggle;
            m_Field = field;
            m_VolumeLabel = volumeLabel;
            m_MuteLabel = muteLabel;
            m_NameLabel = nameLabel;

            m_Slider.ValueChanged += OnVolumeChanged;
            m_Toggle.ValueChanged += OnMuteChanged;
            m_Field.TextChanging += OnNameChanging;
            m_Field.TextChanged += OnNameChanged;

            // Seed the controls SILENTLY: the labels' event counters must stay 0 until the
            // user interacts. The reads below go through the typed getters, so the label
            // showing 35 also proves the write landed (clamped and quantised natively).
            m_Slider.SetValueWithoutNotify(35.0f);
            m_Toggle.SetValueWithoutNotify(false);
            m_Field.SetValueWithoutNotify("player-one");

            m_VolumeLabel.SetText($"volume: {m_Slider.Value:0} (events {m_VolumeEvents})");
            m_MuteLabel.SetText($"mute: {m_Toggle.IsChecked} (events {m_MuteEvents})");
            m_NameLabel.SetText($"name: '{m_Field.Text}' (changing {m_NameChangingEvents}, changed {m_NameChangedEvents})");
        }

        public override void OnDestroy() => Unwire();

        private void OnVolumeChanged(Ui.ValueArgs args)
        {
            ++m_VolumeEvents;
            m_VolumeLabel?.SetText($"volume: {args.Value:0} (events {m_VolumeEvents})");
        }

        private void OnMuteChanged(Ui.ValueArgs args)
        {
            ++m_MuteEvents;
            m_MuteLabel?.SetText($"mute: {args.AsBool} (events {m_MuteEvents})");
        }

        private void OnNameChanging(Ui.TextValueArgs args)
        {
            ++m_NameChangingEvents;
            WriteNameLabel(args.Text);
        }

        private void OnNameChanged(Ui.TextValueArgs args)
        {
            ++m_NameChangedEvents;
            WriteNameLabel(args.Text);
        }

        // The view is PASSED, not stored — a ref struct crosses a call boundary fine, which is
        // what lets both handlers share this. ToString() is the deliberate copy: putting the
        // text on screen is exactly the case that has to materialise it.
        private void WriteNameLabel(Ui.Utf8Text name)
            => m_NameLabel?.SetText(
                $"name: '{name.ToString()}' (changing {m_NameChangingEvents}, changed {m_NameChangedEvents})");

        private void Unwire()
        {
            if (m_Slider != null)
                m_Slider.ValueChanged -= OnVolumeChanged;
            if (m_Toggle != null)
                m_Toggle.ValueChanged -= OnMuteChanged;
            if (m_Field != null)
            {
                m_Field.TextChanging -= OnNameChanging;
                m_Field.TextChanged -= OnNameChanged;
            }
            m_Slider = null;
            m_Toggle = null;
            m_Field = null;
            m_VolumeLabel = null;
            m_MuteLabel = null;
            m_NameLabel = null;
        }
    }
}
