#include "Inspectors/AnimatorInspector.h"
#include "Inspectors/AnimationPreviewManager.h"
#include "PlayMode/AnimationPlayModeHelpers.h"

#include "InspectorRegistry.h"

#include "AssetCore/AssetTypes.h"
#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Components/Animation/Animator.h"
#include "Components/Animation/AnimatorRef.h"
#include "Components/Animation/SkeletonRef.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Core/Engine.h"
#include "ECSModules/Rendering/ClipStore.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "ECS/World.h"
#include "Types/Types.h"

#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "UI/Controls/TextField.h"
#include "UI/StyleProperties.h"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{

namespace
{

// Apply a clip change live (in preview or play mode) by loading the clip and
// assigning to all AnimatorRef components in the entity's subtree.
void ApplyClipChangeLive(ECS::World& world, ECS::EntityHandle rootEntity,
                         const GUID& clipGuid)
{
    if (clipGuid.IsNull())
        return;

    // If preview is active, use the manager (reuses cached subtree)
    if (Editor::AnimationPreviewManager::IsPreviewActive(rootEntity))
    {
        Editor::AnimationPreviewManager::SwitchClip(world, rootEntity, clipGuid);
        return;
    }

    // Otherwise (play mode), load clip and apply directly
    std::vector<ECS::EntityHandle> subtree;
    Editor::CollectEntitiesInSubtree(world, rootEntity, subtree);

    auto& am = EngineCore::GetInstance().GetAssetManager();
    uint32_t clipIndex = Editor::ResolveClipIndex(clipGuid, am);
    if (clipIndex != 0)
        Editor::ApplyAnimatorPlayState(world, subtree, clipIndex);
}

// What the artist picked for Animator.clipGuid. A model container carries
// the embedded index so the Animator records the durable source pair; a
// bare GUID (standalone .anim, or a model without clips kept as a hint) is
// selected alone and drops any earlier pair.
struct ClipSelection
{
    GUID clipGuid;
    GUID modelGuid;
    uint32 animationIndex = 0;

    void ApplyTo(Components::Animator& animator) const
    {
        if (!modelGuid.IsNull())
            animator.SelectEmbeddedClip(modelGuid, animationIndex);
        else
            animator.SelectClip(clipGuid);
    }
};

constexpr const char* kNoEmbeddedClipOption = "none";

// Drag-drop / search-dialog selection: a Model asset (FBX/GLB) picked as the
// "animation source" resolves to its first embedded clip.
ClipSelection NormalizeClipSelection(const GUID& selected)
{
    ClipSelection selection{selected, GUID::Null()};
    if (selected.IsNull())
        return selection;
    auto& am = EngineCore::GetInstance().GetAssetManager();
    SharedPtr<Asset> picked = am.GetAsset(selected);
    if (!picked || picked->GetType() != AssetType::Model)
        return selection;
    // Force-load to populate m_EmbeddedClipGuids.
    Editor::TryAcquireModelAsset(am, selected);
    auto* modelAsset = dynamic_cast<ModelAsset*>(picked.get());
    if (modelAsset && !modelAsset->GetEmbeddedClipGuids().empty())
    {
        selection.clipGuid = modelAsset->GetEmbeddedClipGuids().front();
        selection.modelGuid = selected;
    }
    return selection;
}

// Read the current playback time from the first active AnimatorRef in the
// preview subtree (handles both single-submesh and multi-submesh models).
float ReadCurrentTime(ECS::World& world, ECS::EntityHandle rootEntity)
{
    const auto* state = Editor::AnimationPreviewManager::GetState(rootEntity);
    if (state)
    {
        for (ECS::EntityHandle e : state->subtree)
        {
            auto* anim = world.GetComponent<Components::AnimatorRef>(e);
            if (anim && anim->ClipIndex != 0)
                return anim->Time;
        }
        return 0.0f;
    }

    // Fallback: check root directly (play mode, no preview state)
    auto* anim = world.GetComponent<Components::AnimatorRef>(rootEntity);
    return (anim && anim->ClipIndex != 0) ? anim->Time : 0.0f;
}

// Format a float for display as a short string (e.g. "1.2s" or "0.5x").
std::string FormatValue(float v, const char* suffix)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f%s", v, suffix);
    return buf;
}

} // namespace

void RegisterAnimatorInspector()
{
    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto* animator = ctx.World->GetComponent<Components::Animator>(ctx.Entity);
        if (!animator)
        {
            InspectorUI::AddLine(ctx.Parent, "(Animator missing)");
            return;
        }

        // Capture the GetWorld callable rather than a raw World*, so the lambda
        // re-resolves the world at invocation time. AssetField's drop handler
        // can fire across frames (DragDropManager queues then commits), and the
        // panel's m_World can be reset/re-bound in between — a stale raw
        // pointer here would be a use-after-free in World::Query<Parent>().
        auto getWorld = ctx.GetWorld;
        ECS::World* w = ctx.World;
        ECS::EntityHandle e = ctx.Entity;
        Editor::EditorChangeNotifications* n = ctx.ChangeNotifications;
        Editor::UndoRedoService* undo = ctx.Undo;
        auto commitAnimator = [getWorld, e, n, undo](const char* actionName, auto&& apply)
        {
            ECS::World* world = getWorld ? getWorld() : nullptr;
            if (!world) return;
            InspectorDrag::CommitComponentWithUndo<Components::Animator>(world, e, n, undo,
                actionName,
                std::forward<decltype(apply)>(apply));
        };

        InspectorUI::AddTextBlock(ctx.Parent, "Playback Options", "inspector-section-subheader");

        InspectorDrag::AddToggleRow(
            ctx.Parent,
            "Auto-play in play mode",
            animator->autoPlayOnEnterPlayMode,
            [commitAnimator](bool v)
            {
                commitAnimator(
                    "Change Animator Auto-play On Enter",
                    [v](Components::Animator& u) { u.autoPlayOnEnterPlayMode = v; });
            }, "Start playing the selected animation when entering play mode");

        // --- Animation picker ---
        // Two ways to set Animator.clipGuid:
        //   1. Asset field (drag-drop a .fbx/.glb/.anim, or open the search
        //      dialog) — handles both embedded and external clips uniformly.
        //   2. Convenience dropdown of clips embedded in the model attached
        //      to this entity, if any.
        std::vector<ECS::EntityHandle> subtree;
        Editor::CollectEntitiesInSubtree(*w, e, subtree);
        const GUID modelGuid = Editor::FindFirstModelGuidInSubtree(*w, subtree);

        auto& am = EngineCore::GetInstance().GetAssetManager();

        // Kick the model load so its embedded clips are registered into the
        // ClipStore and we can list their names.
        // NOTE: TryAcquireModelAsset is now non-blocking — on a cache miss it
        // returns null while the load runs on a worker. The dropdown will simply
        // show no clips until the model is resident and the inspector is next
        // rebuilt (e.g. on reselect). Acceptable; preview playback is driven by
        // AnimationPreviewManager's pending-retry pump instead.
        ModelAsset* modelAsset = nullptr;
        if (!modelGuid.IsNull())
        {
            SharedPtr<Asset> asset = Editor::TryAcquireModelAsset(am, modelGuid);
            modelAsset = dynamic_cast<ModelAsset*>(asset.get());
        }

        // Asset-field row: write any clip GUID directly. Accepts Animation
        // assets and Model assets (latter resolves to first embedded clip).
        InspectorUI::AddAssetFieldRow(
            ctx.Parent, "Clip", animator->clipGuid.ToGuid(),
            { AssetType::Animation, AssetType::Model },
            &am.GetRegistry(),
            [getWorld, e, n, undo](const GUID& picked) {
                ECS::World* world = getWorld ? getWorld() : nullptr;
                if (!world) return;
                const ClipSelection selection = NormalizeClipSelection(picked);
                auto* comp = world->GetComponent<Components::Animator>(e);
                if (!comp) return;

                if (undo)
                {
                    InspectorDrag::CommitComponentWithUndo<Components::Animator>(world, e, n, undo,
                        "Change Animator Clip",
                        [selection](Components::Animator& u) { selection.ApplyTo(u); });
                }
                else
                {
                    Components::Animator updated = *comp;
                    selection.ApplyTo(updated);
                    Editor::CommitComponentUpdate(world, e, n, updated);
                }
                ApplyClipChangeLive(*world, e, selection.clipGuid);
            },
            nullptr,
            "Animation clip asset, or a model asset with embedded animation clips.");

        InspectorUI::AddAssetFieldRow(
            ctx.Parent, "Timeline", animator->timelineGuid.ToGuid(),
            { AssetType::Timeline },
            &am.GetRegistry(),
            [getWorld, e, n, undo](const GUID& picked) {
                ECS::World* world = getWorld ? getWorld() : nullptr;
                if (!world) return;
                InspectorDrag::CommitComponentWithUndo<Components::Animator>(world, e, n, undo,
                    "Change Animator Timeline",
                    [picked](Components::Animator& u)
                    {
                        u.timelineGuid.Set(picked);
                        u.source = Components::AnimatorPlaybackSource::Timeline;
                    });
            },
            nullptr,
            "Timeline asset used when this Animator plays timeline tracks.");

        InspectorUI::AddAssetFieldRow(
            ctx.Parent, "Controller", animator->controllerGuid.ToGuid(),
            { AssetType::AnimationController },
            &am.GetRegistry(),
            [getWorld, e, n, undo](const GUID& picked) {
                ECS::World* world = getWorld ? getWorld() : nullptr;
                if (!world) return;
                InspectorDrag::CommitComponentWithUndo<Components::Animator>(world, e, n, undo,
                    "Change Animator Controller",
                    [picked](Components::Animator& u)
                    {
                        u.controllerGuid.Set(picked);
                    });
            },
            nullptr,
            "AnimationController asset. Parsed on load; states and transitions are not evaluated yet.");

        InspectorUI::AddAssetFieldRow(
            ctx.Parent, "Graph", animator->graphGuid.ToGuid(),
            { AssetType::AnimationGraph },
            &am.GetRegistry(),
            [getWorld, e, n, undo](const GUID& picked) {
                ECS::World* world = getWorld ? getWorld() : nullptr;
                if (!world) return;
                InspectorDrag::CommitComponentWithUndo<Components::Animator>(world, e, n, undo,
                    "Change Animator Graph",
                    [picked](Components::Animator& u)
                    {
                        u.graphGuid.Set(picked);
                        u.source = Components::AnimatorPlaybackSource::Graph;
                        u.graphInstanceGuid = GUID{};
                        u.graphPaused = false;
                    });
            },
            nullptr,
            "Pose-graph asset (.animgraph). Evaluated at runtime. Node-graph authoring is not in this inspector.");

        if (animator->source == Components::AnimatorPlaybackSource::Graph)
        {
            InspectorDrag::AddToggleRow(
                ctx.Parent,
                "Apply Root Motion",
                animator->rootMotionLocal,
                [commitAnimator](bool v)
                {
                    commitAnimator(
                        "Change Animator Apply Root Motion",
                        [v](Components::Animator& u) { u.rootMotionLocal = v; });
                },
                "Extract bone-0 travel from the pose graph and drive CharacterController (or Transform) in local space.");
        }

        InspectorDrag::AddToggleRow(
            ctx.Parent,
            "Loop",
            animator->loop,
            [commitAnimator](bool v)
            {
                commitAnimator(
                    "Change Animator Loop",
                    [v](Components::Animator& u) { u.loop = v; });
            }, "Loop playback when the selected animation reaches the end");

        InspectorDrag::AddToggleRow(ctx.Parent, "Active", animator->active,
            [commitAnimator](bool v)
            {
                commitAnimator("Change Animator Active",
                    [v](Components::Animator& u) { u.active = v; });
            }, "Disable to keep this Animator from starting playback on enter play mode");

        auto [sourceSpeedLabel, sourceSpeedSlider] = InspectorDrag::AddSliderRow(
            ctx.Parent, "Speed Scale", animator->speedScale, 0.0f, 3.0f,
            "Playback speed multiplier saved on the Animator");
        sourceSpeedSlider->SetStep(0.05f);
        auto [previewSourceSpeed, commitSourceSpeed] =
            InspectorDrag::MakeComponentInteractiveHandlers<Components::Animator, float>(
                w, e, n, undo, "Change Animator Speed Scale",
                [](Components::Animator& u, float v) { u.speedScale = v; }, {}, getWorld);
        sourceSpeedSlider->SetOnValueChanging(std::move(previewSourceSpeed));
        sourceSpeedSlider->SetOnValueChanged(std::move(commitSourceSpeed));

        TextField* rootNodeField = InspectorUI::AddTextRow(
            ctx.Parent,
            "Root Node",
            std::string(animator->RootNode()),
            "Scene/entity path Timeline value tracks resolve against. Empty = this entity.");
        rootNodeField->SetOnCommit([rootNodeField, commitAnimator]()
        {
            const std::string value = rootNodeField->GetValue();
            commitAnimator("Change Animator Root Node",
                [value](Components::Animator& u) { u.SetRootNode(value); });
        });

        InspectorDrag::AddIntRowWithDrag(
            ctx.Parent,
            "Max Polyphony",
            static_cast<int>(animator->audioMaxPolyphony),
            [](int) {},
            [commitAnimator](int v)
            {
                const uint32 clamped = static_cast<uint32>(std::max(1, v));
                commitAnimator("Change Animation Audio Polyphony",
                    [clamped](Components::Animator& u) { u.audioMaxPolyphony = clamped; });
            },
            32,
            "Maximum simultaneous Timeline audio events on this Animator");

        auto [blendLabel, blendSlider] = InspectorDrag::AddSliderRow(
            ctx.Parent, "Blend Time", animator->blendSeconds, 0.0f, 5.0f,
            "Default crossfade duration for CrossFadeSeconds");
        blendSlider->SetStep(0.05f);
        auto [previewBlendTime, commitBlendTime] =
            InspectorDrag::MakeComponentInteractiveHandlers<Components::Animator, float>(
                w, e, n, undo, "Change Animator Blend Time",
                [](Components::Animator& u, float v) { u.blendSeconds = std::max(0.0f, v); }, {}, getWorld);
        blendSlider->SetOnValueChanging(std::move(previewBlendTime));
        blendSlider->SetOnValueChanged(std::move(commitBlendTime));

        // Embedded-clip dropdown: present only if the entity's model carries
        // any. Option values are embedded indices; picking records the model
        // and index as the clip's durable source.
        if (modelAsset && modelAsset->IsLoaded())
        {
            const auto& names = modelAsset->GetAnimationNames();
            const auto& embedGuids = modelAsset->GetEmbeddedClipGuids();
            if (!names.empty() && names.size() == embedGuids.size())
            {
                std::vector<Dropdown::Option> options;
                options.reserve(names.size() + 1);
                options.push_back({kNoEmbeddedClipOption, "(None)"});

                int selectedIdx = 0;
                for (size_t i = 0; i < names.size(); ++i)
                {
                    Dropdown::Option opt;
                    opt.value = std::to_string(i);
                    opt.label = names[i];
                    options.push_back(std::move(opt));
                    if (embedGuids[i] == animator->clipGuid.ToGuid())
                        selectedIdx = static_cast<int>(i) + 1;
                }

                Dropdown* dd = InspectorUI::AddDropdownRow(
                    ctx.Parent, "Embedded Clip", options, selectedIdx,
                    "Pick a clip embedded in this entity's model");
                dd->SetOnValueChanged([getWorld, e, n, undo, modelGuid](const std::string& value)
                {
                    ECS::World* world = getWorld ? getWorld() : nullptr;
                    if (!world) return;
                    auto* comp = world->GetComponent<Components::Animator>(e);
                    if (!comp) return;
                    ClipSelection selection;
                    uint32 animationIndex = 0;
                    // Anything but an index option — kNoEmbeddedClipOption included —
                    // is a clear: an option value the dropdown did not mint must not
                    // throw out of a UI callback.
                    if (std::from_chars(value.data(), value.data() + value.size(), animationIndex).ec
                        == std::errc{})
                    {
                        selection.animationIndex = animationIndex;
                        selection.modelGuid = modelGuid;
                        selection.clipGuid = ModelAsset::DeriveEmbeddedClipGuid(modelGuid, selection.animationIndex);
                    }
                    if (undo)
                    {
                        InspectorDrag::CommitComponentWithUndo<Components::Animator>(world, e, n, undo,
                            "Change Animator Clip",
                            [selection](Components::Animator& u) { selection.ApplyTo(u); });
                    }
                    else
                    {
                        Components::Animator updated = *comp;
                        selection.ApplyTo(updated);
                        Editor::CommitComponentUpdate(world, e, n, updated);
                    }
                    const GUID picked = selection.clipGuid;
                    if (!picked.IsNull())
                        ApplyClipChangeLive(*world, e, picked);
                    else
                        Editor::AnimationPreviewManager::DisablePreview(*world, e);
                });
            }
        }

        // --- Preview toggle + playback controls ---
        // Always create playback controls (inspector doesn't rebuild when preview
        // is toggled since the component signature doesn't change). We create the
        // container first to get a pointer, then add the toggle, then add the
        // container to the parent so the visual order is: toggle, controls.
        bool previewActive = Editor::AnimationPreviewManager::IsPreviewActive(e);

        const auto* previewState = Editor::AnimationPreviewManager::GetState(e);
        float duration = previewState ? previewState->duration : 1.0f;
        float currentSpeed = previewState ? previewState->speed : 1.0f;

        auto controlsContainer = std::make_unique<UIElement>();
        controlsContainer->Overrides().Set(Style::Display, previewActive ? DisplayMode::Flex : DisplayMode::None);
        controlsContainer->Overrides().Set(Style::FlexDir, FlexDirection::Column);
        UIElement* controlsPtr = controlsContainer.get();

        Toggle* previewToggle = InspectorDrag::AddToggleRow(
            ctx.Parent,
            "Preview",
            previewActive,
            [getWorld, e, n, undo, controlsPtr](bool v)
            {
                ECS::World* world = getWorld ? getWorld() : nullptr;
                if (!world) return;
                if (v)
                {
                    auto* comp = world->GetComponent<Components::Animator>(e);
                    GUID clipGuid = comp ? comp->clipGuid.ToGuid() : GUID::Null();
                    if (clipGuid.IsNull())
                    {
                        auto& am = EngineCore::GetInstance().GetAssetManager();
                        const Editor::DefaultEmbeddedClip fallback = Editor::ResolveDefaultEmbeddedClip(*world, e, am);
                        clipGuid = fallback.clipGuid;
                        if (!clipGuid.IsNull() && comp)
                        {
                            const ClipSelection selection{fallback.clipGuid, fallback.modelGuid};
                            if (undo)
                            {
                                InspectorDrag::CommitComponentWithUndo<Components::Animator>(
                                    world, e, n, undo,
                                    "Assign Default Animator Clip",
                                    [selection](Components::Animator& u) { selection.ApplyTo(u); });
                            }
                            else
                            {
                                Components::Animator updated = *comp;
                                selection.ApplyTo(updated);
                                Editor::CommitComponentUpdate(world, e, n, updated);
                            }
                        }
                    }

                    if (!Editor::AnimationPreviewManager::EnablePreview(*world, e, clipGuid))
                        return;
                }
                else
                {
                    Editor::AnimationPreviewManager::DisablePreview(*world, e);
                }

                // Show/hide playback controls immediately
                controlsPtr->Overrides().Set(Style::Display, v ? DisplayMode::Flex : DisplayMode::None);
            }, "Preview animation in edit mode");

        // Add controls container after the toggle in DOM order
        ctx.Parent->AddChild(std::move(controlsContainer));

        // Play/Pause toggle
        auto* animRef = w->GetComponent<Components::AnimatorRef>(e);
        bool isPaused = animRef && animRef->IsPaused();

        Toggle* pauseToggle = InspectorDrag::AddToggleRow(
            controlsPtr,
            "Paused",
            isPaused,
            [getWorld, e](bool v)
            {
                ECS::World* world = getWorld ? getWorld() : nullptr;
                if (!world) return;
                Editor::AnimationPreviewManager::SetPaused(*world, e, v);
            }, "Pause/resume animation playback");

        // Speed slider with value label
        auto [speedLabel, speedSlider] = InspectorDrag::AddSliderRow(
            controlsPtr, "Speed", currentSpeed, 0.0f, 2.0f,
            "Playback speed multiplier");
        speedSlider->SetStep(0.1f);

        auto speedValueLabel = std::make_unique<Label>();
        speedValueLabel->SetText(FormatValue(currentSpeed, "x"));
        speedValueLabel->Overrides().Set(Style::Width, StyleLength::Px(42.0f));
        Label* speedValuePtr = speedValueLabel.get();
        speedSlider->GetParent()->AddChild(std::move(speedValueLabel));

        speedSlider->SetOnValueChanged([getWorld, e, speedValuePtr](float v)
        {
            ECS::World* world = getWorld ? getWorld() : nullptr;
            if (!world) return;
            Editor::AnimationPreviewManager::SetSpeed(*world, e, v);
            speedValuePtr->SetText(FormatValue(v, "x"));
        });
        speedSlider->SetOnValueChanging([getWorld, e, speedValuePtr](float v)
        {
            ECS::World* world = getWorld ? getWorld() : nullptr;
            if (!world) return;
            Editor::AnimationPreviewManager::SetSpeed(*world, e, v);
            speedValuePtr->SetText(FormatValue(v, "x"));
        });

        // Time scrubber with value label
        float currentTime = ReadCurrentTime(*w, e);
        auto [timeLabel, timeSlider] = InspectorDrag::AddSliderRow(
            controlsPtr, "Time", currentTime, 0.0f, duration,
            "Scrub through the animation timeline");
        timeSlider->SetShowValueBubble(true);

        auto timeValueLabel = std::make_unique<Label>();
        timeValueLabel->SetText(FormatValue(currentTime, "s"));
        timeValueLabel->Overrides().Set(Style::Width, StyleLength::Px(42.0f));
        Label* timeValuePtr = timeValueLabel.get();
        timeSlider->GetParent()->AddChild(std::move(timeValueLabel));

        timeSlider->SetOnValueChanged([getWorld, e, timeValuePtr, pauseToggle](float v)
        {
            ECS::World* world = getWorld ? getWorld() : nullptr;
            if (!world) return;
            Editor::AnimationPreviewManager::SetPaused(*world, e, true);
            Editor::AnimationPreviewManager::SetTime(*world, e, v);
            timeValuePtr->SetText(FormatValue(v, "s"));
            pauseToggle->SetValueWithoutNotify(true);
        });
        timeSlider->SetOnValueChanging([getWorld, e, timeValuePtr, pauseToggle](float v)
        {
            ECS::World* world = getWorld ? getWorld() : nullptr;
            if (!world) return;
            Editor::AnimationPreviewManager::SetPaused(*world, e, true);
            Editor::AnimationPreviewManager::SetTime(*world, e, v);
            timeValuePtr->SetText(FormatValue(v, "s"));
            pauseToggle->SetValueWithoutNotify(true);
        });

        // Register simulation refresh callback to update visibility and values
        if (ctx.SimulationRefreshCallbacks)
        {
            Slider* timeScrubber = timeSlider;
            Label* timeValLabel = timeValuePtr;
            ctx.SimulationRefreshCallbacks->push_back(
                [getWorld, e, controlsPtr, previewToggle, timeScrubber, timeValLabel]()
            {
                ECS::World* world = getWorld ? getWorld() : nullptr;
                if (!world) return;
                // Guard against entity deletion while preview is active
                if (!world->IsValid(e))
                {
                    Editor::AnimationPreviewManager::DisablePreview(*world, e);
                    return;
                }

                bool active = Editor::AnimationPreviewManager::IsPreviewActive(e);
                controlsPtr->Overrides().Set(Style::Display, active ? DisplayMode::Flex : DisplayMode::None);
                previewToggle->SetValueWithoutNotify(active);

                if (!active)
                    return;

                float t = ReadCurrentTime(*world, e);
                timeScrubber->SetValueWithoutNotify(t);
                timeValLabel->SetText(FormatValue(t, "s"));

                const auto* state = Editor::AnimationPreviewManager::GetState(e);
                if (state && state->duration != timeScrubber->GetMax())
                    timeScrubber->SetMax(state->duration);
            });
        }
    };

    InspectorRegistry::Get().RegisterComponentInspector<Components::Animator>(std::move(fn));

    Editor::EditorComponentTraits traits;
    traits.EnableToggleTooltip =
        "Switch this component on or off. Off keeps its animation from playing; auto-play is the row below.";
    Editor::EditorComponentTraitsRegistry::Get().Register(ECS::GetComponentTypeId<Components::Animator>(),
                                                          std::move(traits));
}

} // namespace GameEngine
