#include "Inspectors/AudioEmitterInspector.h"

#include "InspectorRegistry.h"

#include "Components/Audio/AudioEmitter.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Editor/Entities/EditorECSHelpers.h"

#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UI/Controls/Dropdown.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace GameEngine
{

void RegisterAudioEmitterInspector()
{
    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
        {
            return;
        }

        auto* emitter = ctx.World->GetComponent<Components::AudioEmitter>(ctx.Entity);
        if (!emitter)
        {
            InspectorUI::AddLine(ctx.Parent, "(Audio Emitter missing)");
            return;
        }

        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;
        using namespace InspectorDrag;
        auto extras = GetAdditionalEntities(ctx);

        // Channel / bus: routes playback to the selected mixer bus (Master, Music, SFX, UI, VO, Aux).
        {
            UIElement* row = InspectorUI::AddRow(ctx.Parent);
            InspectorUI::AddLabel(row, "Channel");
            UIElement* fieldContainer = InspectorUI::AddFieldContainer(row);
            auto dd = std::make_unique<Dropdown>();
            std::vector<Dropdown::Option> opts = {
                {"0", "Master"},
                {"1", "Music"},
                {"2", "SFX"},
                {"3", "UI"},
                {"4", "VO"},
                {"5", "Aux"},
            };
            int sel = static_cast<int>(emitter->bus);
            if (sel < 0 || sel > 5)
                sel = 2;
            dd->SetOptions(opts, sel);
            dd->SetOnValueChanged([w, e, n, undo, extras](const std::string& value)
                                 {
                                     auto* comp = w->GetComponent<Components::AudioEmitter>(e);
                                     if (!comp)
                                         return;
                                     int idx = 0;
                                     try
                                     {
                                         idx = std::stoi(value);
                                     }
                                     catch (const std::exception&)
                                     {
                                         return;
                                     }
                                     if (idx < 0 || idx > 5)
                                         return;
                                     if (undo)
                                     {
                                         auto target = extras.empty()
                                             ? MakeComponentSnapshotTarget<Components::AudioEmitter>(w, e, n, "Change Audio Emitter Bus")
                                             : MakeMultiComponentSnapshotTarget<Components::AudioEmitter>(
                                                   w, e, extras, n, "Change Audio Emitter Bus");
                                         auto edit = undo->BeginInteractiveEdit("Change Audio Emitter Bus", std::move(target));
                                         {
                                             auto* c = w->GetComponent<Components::AudioEmitter>(e);
                                             if (!c)
                                             {
                                                 edit.Commit(); // restores before-state
                                                 return;
                                             }
                                             Components::AudioEmitter updated = *c;
                                             updated.bus = static_cast<uint16_t>(idx);
                                             w->AddComponentImmediate(e, updated);
                                         }
                                         for (auto& ex : extras)
                                         {
                                             auto* c = w->GetComponent<Components::AudioEmitter>(ex);
                                             if (!c) continue;
                                             Components::AudioEmitter u = *c;
                                             u.bus = static_cast<uint16_t>(idx);
                                             w->AddComponentImmediate(ex, u);
                                         }
                                         edit.Commit();
                                         return;
                                     }

                                     Components::AudioEmitter updated = *comp;
                                     updated.bus = static_cast<uint16_t>(idx);
                                     Editor::CommitComponentUpdate(w, e, n, updated);
                                     for (auto& ex : extras)
                                     {
                                         auto* c = w->GetComponent<Components::AudioEmitter>(ex);
                                         if (!c) continue;
                                         Components::AudioEmitter u = *c;
                                         u.bus = static_cast<uint16_t>(idx);
                                         Editor::CommitComponentUpdate(w, ex, n, u);
                                     }
                                 });
            dd->AddClass("inspector-dropdown");
            fieldContainer->AddChild(std::move(dd));
        }

        // Spatialized
        AddToggleRow(ctx.Parent, "Spatialized", emitter->spatialized,
                     [w, e, n, undo, extras](bool v)
                     {
                         if (undo)
                         {
                             auto target = extras.empty()
                                 ? MakeComponentSnapshotTarget<Components::AudioEmitter>(
                                       w, e, n, "Change Audio Emitter Spatialized")
                                 : MakeMultiComponentSnapshotTarget<Components::AudioEmitter>(
                                       w, e, extras, n, "Change Audio Emitter Spatialized");
                             auto edit = undo->BeginInteractiveEdit("Change Audio Emitter Spatialized", std::move(target));
                             {
                                 auto* c = w->GetComponent<Components::AudioEmitter>(e);
                                 if (!c)
                                 {
                                     edit.Commit();
                                     return;
                                 }
                                 Components::AudioEmitter updated = *c;
                                 updated.spatialized = v;
                                 w->AddComponentImmediate(e, updated);
                             }
                             for (auto& ex : extras)
                             {
                                 auto* c = w->GetComponent<Components::AudioEmitter>(ex);
                                 if (!c) continue;
                                 Components::AudioEmitter u = *c;
                                 u.spatialized = v;
                                 w->AddComponentImmediate(ex, u);
                             }
                             edit.Commit();
                             return;
                         }

                         auto* comp = w->GetComponent<Components::AudioEmitter>(e);
                         if (!comp)
                             return;
                         Components::AudioEmitter updated = *comp;
                         updated.spatialized = v;
                         Editor::CommitComponentUpdate(w, e, n, updated);
                         for (auto& ex : extras)
                         {
                             auto* c = w->GetComponent<Components::AudioEmitter>(ex);
                             if (!c) continue;
                             Components::AudioEmitter u = *c;
                             u.spatialized = v;
                             Editor::CommitComponentUpdate(w, ex, n, u);
                         }
                     }, "Attenuate sound based on distance from the listener");

        // Loop
        AddToggleRow(ctx.Parent, "Loop", emitter->loop,
                     [w, e, n, undo, extras](bool v)
                     {
                         if (undo)
                         {
                             auto target = extras.empty()
                                 ? MakeComponentSnapshotTarget<Components::AudioEmitter>(w, e, n, "Change Audio Emitter Loop")
                                 : MakeMultiComponentSnapshotTarget<Components::AudioEmitter>(
                                       w, e, extras, n, "Change Audio Emitter Loop");
                             auto edit = undo->BeginInteractiveEdit("Change Audio Emitter Loop", std::move(target));
                             {
                                 auto* c = w->GetComponent<Components::AudioEmitter>(e);
                                 if (!c)
                                 {
                                     edit.Commit();
                                     return;
                                 }
                                 Components::AudioEmitter updated = *c;
                                 updated.loop = v;
                                 w->AddComponentImmediate(e, updated);
                             }
                             for (auto& ex : extras)
                             {
                                 auto* c = w->GetComponent<Components::AudioEmitter>(ex);
                                 if (!c) continue;
                                 Components::AudioEmitter u = *c;
                                 u.loop = v;
                                 w->AddComponentImmediate(ex, u);
                             }
                             edit.Commit();
                             return;
                         }

                         auto* comp = w->GetComponent<Components::AudioEmitter>(e);
                         if (!comp)
                             return;
                         Components::AudioEmitter updated = *comp;
                         updated.loop = v;
                         Editor::CommitComponentUpdate(w, e, n, updated);
                         for (auto& ex : extras)
                         {
                             auto* c = w->GetComponent<Components::AudioEmitter>(ex);
                             if (!c) continue;
                             Components::AudioEmitter u = *c;
                             u.loop = v;
                             Editor::CommitComponentUpdate(w, ex, n, u);
                         }
                     }, "Loop playback when the audio clip ends");

        // PlayOnStart
        AddToggleRow(ctx.Parent, "PlayOnStart", emitter->playOnStart,
                     [w, e, n, undo, extras](bool v)
                     {
                         if (undo)
                         {
                             auto target = extras.empty()
                                 ? MakeComponentSnapshotTarget<Components::AudioEmitter>(w, e, n, "Change Audio Emitter Play On Start")
                                 : MakeMultiComponentSnapshotTarget<Components::AudioEmitter>(
                                       w, e, extras, n, "Change Audio Emitter Play On Start");
                             auto edit = undo->BeginInteractiveEdit("Change Audio Emitter Play On Start", std::move(target));
                             {
                                 auto* c = w->GetComponent<Components::AudioEmitter>(e);
                                 if (!c)
                                 {
                                     edit.Commit();
                                     return;
                                 }
                                 Components::AudioEmitter updated = *c;
                                 updated.playOnStart = v;
                                 w->AddComponentImmediate(e, updated);
                             }
                             for (auto& ex : extras)
                             {
                                 auto* c = w->GetComponent<Components::AudioEmitter>(ex);
                                 if (!c) continue;
                                 Components::AudioEmitter u = *c;
                                 u.playOnStart = v;
                                 w->AddComponentImmediate(ex, u);
                             }
                             edit.Commit();
                             return;
                         }

                         auto* comp = w->GetComponent<Components::AudioEmitter>(e);
                         if (!comp)
                             return;
                         Components::AudioEmitter updated = *comp;
                         updated.playOnStart = v;
                         Editor::CommitComponentUpdate(w, e, n, updated);
                         for (auto& ex : extras)
                         {
                             auto* c = w->GetComponent<Components::AudioEmitter>(ex);
                             if (!c) continue;
                             Components::AudioEmitter u = *c;
                             u.playOnStart = v;
                             Editor::CommitComponentUpdate(w, ex, n, u);
                         }
                     }, "Automatically begin playback when the scene starts");

        // Volume
        AddComponentFloatRowWithDrag<Components::AudioEmitter>(ctx.Parent, "Volume", emitter->volume, w, e, n, undo,
            "Change Audio Emitter Volume",
            [](Components::AudioEmitter& u, float v) { u.volume = v; },
            emitter->volume,
            "Playback volume (0 = silent, 1 = full)", extras);

        AddComponentFloatRowWithDrag<Components::AudioEmitter>(ctx.Parent, "Pitch", emitter->pitch, w, e, n, undo,
            "Change Audio Emitter Pitch",
            [](Components::AudioEmitter& u, float v) { u.pitch = v; },
            emitter->pitch,
            "Playback pitch multiplier (1 = normal, 2 = one octave up)", extras);
    };

    InspectorRegistry::Get().RegisterComponentInspector<Components::AudioEmitter>(std::move(fn));

    Editor::EditorComponentTraits traits;
    traits.EnableToggleTooltip = "Switch this component on or off. Off stops its sound.";
    Editor::EditorComponentTraitsRegistry::Get().Register(
        ECS::GetComponentTypeId<Components::AudioEmitter>(), std::move(traits));
}

} // namespace GameEngine
