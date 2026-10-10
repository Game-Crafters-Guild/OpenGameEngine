#include "Terrain/TerrainRuleRows.h"

#include "InspectorRegistry.h"

#include "Components/Terrain/Terrain.h"
#include "Terrain/TerrainTypes.h"
#include "Components/Terrain/TerrainModifierEffects.h"
#include "ECS/World.h"
#include "Editor/Entities/EditorECSHelpers.h"
#include "EditorChangeNotifications.h"
#include "Inspectors/InspectorComponentRowHelpers.h"
#include "Inspectors/InspectorComponentSection.h"
#include "Inspectors/InspectorDragHelpers.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/TerrainRuleNotices.h"
#include "Terrain/TerrainInteractiveModifierEdit.h"
#include "Terrain/TerrainRoleMaterials.h"
#include "Terrain/TerrainRuleConditionVocabulary.h"
#include "Terrain/TerrainRuleWeightProfileStrip.h"
#include "Thumbnails/IThumbnailProvider.h"
#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/Foldout.h"
#include "UI/Controls/Label.h"
#include "UI/EditorIcons.h"
#include "UI/Controls/CollapsibleInfoCard.h"
#include "UI/InfoCard.h"
#include "UI/Layout/ElementOverrideHelpers.h"
#include "UI/StyleProperties.h"
#include "UndoRedo/UndoRedoService.h"

#include "AssetCore/Asset.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Core/Engine.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace
{

using namespace Components;
using namespace InspectorDrag;
namespace Vocab = Editor::TerrainRuleVocabulary;

using RulesEffect = TerrainSurfaceRulesEffect;
using Kind = TerrainRuleConditionKind;
using Curve = TerrainRuleFalloffCurve;

constexpr float kProfileStripHeightPx = 26.0f;

// Mirrors the "Add Effect" button one level up. Capped to the panel width by a max-width
// override, so a narrow Inspector shrinks it rather than clipping it.
constexpr float kAddButtonWidthPx = 360.0f;

// The albedo thumbnail beside the result-material picker. Sized to the picker's
// own row rather than to the image: it identifies the material at a glance and
// the library inspector's cards are where a texture is actually inspected.
constexpr float kMaterialThumbnailSizePx = 16.0f;
constexpr float kMaterialThumbnailGapPx = 6.0f;
// Asked of the thumbnail service in pixels. Below its cache's long edge, so the
// request is answered rather than declined to the full-resolution source.
constexpr int kMaterialThumbnailRequestPx = 32;

// The feather slider spans a quarter of the condition's domain. A feather wider
// than that is a ramp with no plateau left, which the numeric field still
// accepts — the slider covers the useful range rather than the legal one.
constexpr float kFeatherSliderMaxFraction = 0.25f;

// The flex gap ApplySliderWithValueContainerStyle puts between a slider and its
// value field. Mirrored rather than shared because that helper writes the number
// inline; if it moves, the strip drifts off the track and the band edge stops
// sitting under its own handle.
constexpr float kSliderValueGapPx = 4.0f;

// Digits a numeric field shows. FloatField's UNFOCUSED formatter is already
// adaptive, but its FOCUSED one is a shortest-round-trip print — and a label
// scrub leaves the field focused, so mid-drag it rendered the raw float
// (4.7999997, whose caret sat on the decimal point and made it ambiguous with
// 47,999,997). Pinning the places fixes both display modes at once.
constexpr int kUnitFieldDecimals = 2; // Strength, which has no condition kind

// Fields take their digits from the SAME place the captions do, so the two can
// never disagree about how precise this kind is.
int BandFieldDecimals(Kind kind) { return Vocab::ConditionDecimals(kind); }

// The band a two-handle drag reports: both handles move through one callback, so
// the edit is the pair, never one end of it.
struct RuleBand
{
    float32 Low = 0.0f;
    float32 High = 1.0f;
};

// ---- shared edit plumbing --------------------------------------------------

// What a callback needs to commit a rules edit.
//
// Not the InspectorContext itself: that carries a std::function for every editor
// service an inspector might reach for, plus the selection vector, and a full
// rule set builds a couple of hundred callbacks, every one of which would copy
// all of it. The Inspector rebuilds on each discrete edit, so
// that cost lands on every dropdown change rather than once. The refresh is held
// behind a shared_ptr so copying this stays a pointer bump.
struct RuleEditTarget
{
    ECS::World* World = nullptr;
    ECS::EntityHandle Entity{};
    Editor::EditorChangeNotifications* Notifications = nullptr;
    Editor::UndoRedoService* Undo = nullptr;
    std::shared_ptr<std::function<void()>> Refresh;
};

RuleEditTarget MakeEditTarget(const InspectorContext& ctx)
{
    RuleEditTarget target;
    target.World = ctx.World;
    target.Entity = ctx.Entity;
    target.Notifications = ctx.ChangeNotifications;
    target.Undo = ctx.Undo;
    target.Refresh = std::make_shared<std::function<void()>>(ctx.RequestInspectorRefresh);
    return target;
}

// Drops keyboard focus (and with it the reverse-video selection and the caret)
// off whatever numeric field held it.
//
// Called when a SIBLING control starts a drag. A field keeps focus when the
// pointer goes down elsewhere — deliberately, there is a UI test asserting that
// — so dragging Strength left the Band field showing selected text with a caret
// abutting its digits, which reads as a value being edited by the drag. Nothing
// here touches the shared caret policy; it only stops pointing at this row.
void DropNumericFieldFocus(UIElement* anyElementInRow)
{
    if (!anyElementInRow)
        return;
    if (UIManager* manager = anyElementInRow->GetOwnerManager())
        manager->ClearFocus();
}

// Rebuilds the Inspector. Only for edits that change what rows EXIST or what a
// row is called — never for a drag, which would destroy the widget under the
// pointer mid-gesture.
void RequestRebuild(const RuleEditTarget& target)
{
    if (target.Refresh && *target.Refresh)
        (*target.Refresh)();
}

// A discrete edit: one undo entry, applied immediately. Used by every control
// that is not dragged (dropdowns, add / remove).
template <typename MutateFn>
void EditRules(const RuleEditTarget& target, const std::string& name, MutateFn&& mutate)
{
    CommitComponentWithUndo<RulesEffect>(target.World, target.Entity, target.Notifications,
                                         target.Undo, name, std::forward<MutateFn>(mutate));
}

// Wraps an interactive (preview, commit) pair so the terrain preview-cadence
// throttle is held for the whole gesture.
//
// Without this the modifier system never sees a drag: its coalescing branch is
// gated on TerrainService::IsInteractiveModifierEdit, and a rules edit on a
// GLOBAL volume dirties every resident tile, so each mouse-move would re-bake
// the world. The arm is shared by BOTH lambdas so that destroying the widget —
// which is what an Inspector rebuild does mid-drag, and the mouse-up that would
// have released then never arrives — releases it too.
//
// That release-on-destruction path is argued from the ownership graph (the
// lambdas own the arm, the control owns the lambdas, the Inspector owns the
// control) and pinned by TerrainInteractiveEditArm's own destruction test. It is
// NOT covered by a test that destroys a live widget mid-drag; EditorTests builds
// no Inspector.
template <typename V>
std::pair<std::function<void(V)>, std::function<void(V)>>
HoldThrottleForGesture(std::pair<std::function<void(V)>, std::function<void(V)>> handlers,
                       UIElement* focusHost)
{
    auto arm = std::make_shared<Editor::TerrainInteractiveModifierEditArm>();
    auto preview = std::move(handlers.first);
    auto commit = std::move(handlers.second);
    return {[arm, preview, focusHost](V v) {
                // First preview of a gesture: whatever numeric field still holds
                // focus is not the thing being dragged, and its selection and
                // caret read as an edit in progress on the wrong control.
                if (!arm->IsArmed())
                    DropNumericFieldFocus(focusHost);
                arm->Arm();
                preview(v);
            },
            // Commits the value first: the release lets the settle bake run on a
            // later frame, and it must read the committed value, not the one
            // before it.
            [arm, commit](V v) {
                commit(v);
                arm->Release();
            }};
}

// `onAfter` runs once per preview and once on commit, AFTER the edit has been
// mirrored to every co-selected entity — which is why it takes no value and
// re-reads the primary instead. Optional: rows with nothing to redraw omit it.
template <typename V, typename ApplyFn>
std::pair<std::function<void(V)>, std::function<void(V)>>
MakeRuleDragHandlers(const InspectorContext& ctx, const std::string& name, ApplyFn&& apply,
                     std::function<void()> onAfter = {})
{
    auto handlers = HoldThrottleForGesture<V>(
        MakeComponentInteractiveHandlers<RulesEffect, V>(
            ctx.World, ctx.Entity, ctx.ChangeNotifications, ctx.Undo, name,
            std::forward<ApplyFn>(apply), GetAdditionalEntities(ctx), ctx.GetWorld),
        ctx.Parent);

    if (!onAfter)
        return handlers;

    auto preview = std::move(handlers.first);
    auto commit = std::move(handlers.second);
    return {[preview, onAfter](V v) {
                preview(v);
                onAfter();
            },
            [commit, onAfter](V v) {
                commit(v);
                onAfter();
            }};
}

// ---- small UI pieces -------------------------------------------------------

// Channel labels with no terrain to read a binding from. An empty option list
// would leave a picker that cannot be opened or read.
std::vector<Dropdown::Option> ChannelIndexOptions()
{
    std::vector<Dropdown::Option> options;
    // Qualified: `using namespace Components` puts the Terrain COMPONENT in scope
    // alongside the GameEngine::Terrain namespace this constant lives in.
    for (uint32 i = 0; i < ::GameEngine::Terrain::kMaxTerrainMaterialLayers; ++i)
        options.push_back({std::to_string(i), "Channel " + std::to_string(i)});
    return options;
}

// The one thing this panel is for, in the shape the panel above it already gives that role: the
// wide centred button "Add Effect" wears one level up. Adding a rule is the same kind of act as
// adding an effect — it is what the author came here to do — so it gets the same affordance
// rather than a field-column button that reads as one more property.
//
// A button that vanishes at the cap leaves the author hunting for it, and one that stays and
// silently does nothing is worse; `disabled` keeps it in place, greyed and inert.
//
// Disabled carries NO tooltip. At the cap the notice directly beneath states the reason
// permanently, and a bubble anchored to the button opened straight over that notice, hiding its
// second line. One always-visible surface beats two when one of them covers the other.
Button* AddPrimaryAddButton(UIElement* parent, const std::string& text, const char* tooltip,
                            UIElement::EventHandler onClick, bool disabled = false)
{
    if (!parent)
        return nullptr;

    auto button = std::make_unique<Button>();
    Button* raw = button.get();
    raw->SetText(text);
    raw->AddClass("inspector-add-component-button");
    raw->Overrides()
        .Set(Style::Width, StyleLength::Px(kAddButtonWidthPx))
        .Set(Style::MaxWidth, StyleLength::Percent(100.0f))
        .Set(Style::AlignSelf, AlignItems::Center)
        .Set(Style::MarginTop, StyleLength::Px(6.0f));

    if (disabled)
    {
        raw->AddClass("disabled");
    }
    else
    {
        if (tooltip)
            raw->SetTooltip(tooltip);
        raw->RegisterEventHandler(kEventButtonClick, std::move(onClick));
    }

    parent->AddChild(std::move(button));
    return raw;
}

// The albedo the rule's result material shades with, as a small image beside the
// picker — or nothing at all.
//
// NOTHING AT ALL is the answer, not a fallback, whenever an image does not
// resolve. An element with an unresolvable background renders as an EMPTY chip
// (no colour is set and the tint resets), indistinguishable from a thumbnail
// that merely failed to load. That ambiguity is what retired the tint swatch
// this replaces — its white fallback matched a genuinely white tint — and an
// empty chip here would reintroduce the same defect under a new name.
//
// The albedo TEXTURE is shown rather than the tint because it is the half that
// differs between materials: a rock and a grass both carry white tints and never
// the same image.
//
// SYNCHRONOUS by construction. IThumbnailProvider::GetOrRequest invokes onReady
// from a decode worker (TextureThumbnailHandler.h) and nothing here may touch the
// UI tree off the UI thread, so this takes the immediate answer and passes no
// callback at all. For a texture under the assets root that answer is non-empty:
// the service hands back the source image while its downscaled cache entry is
// generated, and the request this queues is what makes the next build cheap.
void AddMaterialThumbnail(UIElement* field, IThumbnailProvider* thumbnails,
                          const GUID& albedoTexture, const std::string& materialName)
{
    if (!field || !thumbnails || albedoTexture.IsNull())
        return;

    EngineCore& engine = EngineCore::GetInstance();
    if (!engine.IsInitialized())
        return;

    AssetMetadata metadata{};
    if (!engine.GetAssetManager().GetRegistry().TryGetAssetMetadata(albedoTexture, metadata))
        return;
    // Type-gated before the request, not after it: GetOrRequest dispatches on the
    // asset's type, and a slot pointing at a model or a material would queue a
    // live orbit render for a 16px chip and answer with an `engine:` resource
    // name this row has no business binding.
    if (metadata.Type != AssetType::Texture || metadata.Path.empty())
        return;

    const std::string image =
        thumbnails->GetOrRequest(metadata.Path, kMaterialThumbnailRequestPx, nullptr);
    if (image.empty())
        return;

    auto thumbnail = std::make_unique<UIElement>();
    thumbnail->Overrides()
        .Set(Style::Width, StyleLength::Px(kMaterialThumbnailSizePx))
        .Set(Style::Height, StyleLength::Px(kMaterialThumbnailSizePx))
        .Set(Style::MinWidth, StyleLength::Px(kMaterialThumbnailSizePx))
        .Set(Style::FlexShrink, 0.0f);
    UI::Layout::SetBackgroundPath(*thumbnail, image);
    // The material's NAME. The picker beside it already says what the row does,
    // and the one thing a 16px image cannot state is which of two similar-looking
    // materials it came from.
    thumbnail->SetTooltip(materialName);
    field->AddChild(std::move(thumbnail));
}

// ---- the condition widget --------------------------------------------------

// Re-maps a band when its unit changes. The two slope units are different
// MEASUREMENTS, not one rescaled, so no conversion is correct — keeping the same
// fraction of the domain at least keeps the band alive and visible, where a
// clamp would collapse a 34-90 degree band onto the single value 1.0.
void RemapBandToDomain(TerrainRuleCondition& condition, Vocab::ConditionDomain from,
                       Vocab::ConditionDomain to)
{
    const float32 fromSpan = from.Max - from.Min;
    const float32 toSpan = to.Max - to.Min;
    if (!(fromSpan > 0.0f) || !(toSpan > 0.0f))
        return;
    const float32 scale = toSpan / fromSpan;
    condition.Min = to.Min + (condition.Min - from.Min) * scale;
    condition.Max = to.Min + (condition.Max - from.Min) * scale;
    condition.Feather *= scale;
    // Float error must not turn a whole-domain band into one that is a hair
    // short of it — that band is the no-op a fresh condition starts as.
    condition.Min = std::max(condition.Min, to.Min);
    condition.Max = std::min(condition.Max, to.Max);
}

// Every per-condition visual refresh in one rule, so a RULE-scoped control can
// reach them.
//
// The rule's Strength row is built before its conditions and lives in a
// different function, so it had no way to reach their captions — which is why
// the caption kept claiming "full weight" after Strength was dragged to 0.5.
// A shared_ptr, not a reference: these closures outlive the builder.
using ConditionRefreshers = std::shared_ptr<std::vector<std::function<void()>>>;

void RefreshAllConditions(const ConditionRefreshers& refreshers)
{
    if (!refreshers)
        return;
    for (const auto& refresh : *refreshers)
        if (refresh)
            refresh();
}

void AddConditionRows(const InspectorContext& ctx, UIElement* parent,
                      const TerrainSurfaceRule& rule, uint32 ruleIndex, uint32 conditionIndex,
                      float32 terrainHeightMetres, const ConditionRefreshers& refreshers)
{
    const RuleEditTarget target = MakeEditTarget(ctx);
    const TerrainRuleCondition& condition = rule.Conditions[conditionIndex];
    const Vocab::ConditionDomain domain =
        Vocab::ConditionAuthoringDomain(condition.Kind, terrainHeightMetres);
    const float32 domainSpan = std::max(domain.Max - domain.Min, 1e-3f);

    const std::string idSuffix =
        " (rule " + std::to_string(ruleIndex + 1) + ", condition "
        + std::to_string(conditionIndex + 1) + ")";

    // ---- kind ----
    {
        std::vector<Dropdown::Option> options;
        for (uint32 k = 0; k <= static_cast<uint32>(Kind::Noise); ++k)
        {
            options.push_back({std::to_string(k),
                               std::string(Vocab::ConditionKindLabel(static_cast<Kind>(k)))});
        }
        auto* dd = InspectorUI::AddDropdownRow(
            parent, "Measures", options, static_cast<int>(condition.Kind),
            "What this condition bands. Changing the unit re-maps the band to the same fraction "
            "of the new range - the two slope units are different measurements, not one "
            "rescaled, so check the band after switching.");
        dd->SetOnValueChanged([target, ruleIndex, conditionIndex, terrainHeightMetres](
                                  const std::string& value) {
            const Kind newKind = static_cast<Kind>(std::stoi(value));
            EditRules(target, "Condition Measures", [=](RulesEffect& u) {
                TerrainRuleCondition& c = u.Rules[ruleIndex].Conditions[conditionIndex];
                RemapBandToDomain(c, Vocab::ConditionAuthoringDomain(c.Kind, terrainHeightMetres),
                                  Vocab::ConditionAuthoringDomain(newKind, terrainHeightMetres));
                c.Kind = newKind;
            });
            RequestRebuild(target); // the unit, domain and help all change
        });
        // NO tooltip on the control. TooltipOverlay walks ancestors for the
        // text and anchors the bubble to the element that owns it — the
        // COLLAPSED dropdown — and the option list opens in exactly that space,
        // under a z-index the bubble outranks. Hovering an option therefore
        // covered the first rows of the list it was documenting, including the
        // selected one. The help reads better as a line that is simply always
        // there, under the row rather than over it.
        InspectorUI::AddInfoCard(parent, std::string(Vocab::ConditionKindHelp(condition.Kind)));
    }

    // ---- the band: one row, two handles ----
    // Low field | two-handle slider | high field. The slider is for reaching a
    // value, the fields are for STATING one: an author cannot type 30 into a
    // track, and two rules cannot share an exact edge (grass <=30, rock >30 —
    // the second thing anyone authors) when the only input is 0.18 deg per pixel
    // of hand precision.
    UIElement* bandRow = InspectorUI::AddRow(parent);
    InspectorUI::AddLabel(bandRow, "Band",
             "The range this condition is fully open over. Type an exact edge in either field, or "
             "drag a handle. The strip below plots the weight every value in the range receives.");
    UIElement* bandField = InspectorUI::AddFieldContainer(bandRow);
    ApplySliderWithValueContainerStyle(bandField);

    auto lowFieldOwned = std::make_unique<FloatField>();
    FloatField* lowField = lowFieldOwned.get();
    lowField->SetValue(condition.Min);
    lowField->SetFixedDecimalPlaces(BandFieldDecimals(condition.Kind));
    ApplySliderFloatValueFieldStyle(lowField);
    bandField->AddChild(std::move(lowFieldOwned));

    Slider* band = AddInspectorSlider(bandField, condition.Max, domain.Min, domain.Max,
                                      /*step=*/0.0f, /*showValueBubble=*/true,
                                      kInspectorWideSliderMinWidthPx);
    band->SetRangeMode(true);
    // Slider enforces low <= high, so an INVERTED band (Min > Max — a legal
    // stored state, and the one that kills a row) draws here re-ordered rather
    // than as authored. The text is the authority and says so twice: the summary
    // below reads "Empty band", and the row's own notice names it with the fix.
    // Writing the re-ordered pair back to remove the mismatch would be the
    // widget quietly editing data the author never touched. The FIELDS show the
    // authored values, so the inversion stays visible as numbers.
    band->SetRangeValues(condition.Min, condition.Max);

    auto highFieldOwned = std::make_unique<FloatField>();
    FloatField* highField = highFieldOwned.get();
    highField->SetValue(condition.Max);
    highField->SetFixedDecimalPlaces(BandFieldDecimals(condition.Kind));
    ApplySliderFloatValueFieldStyle(highField);
    bandField->AddChild(std::move(highFieldOwned));

    // The strip is a second view of the SLIDER'S axis, so its plot box has to be
    // the slider's TRACK box — not merely the same column.
    //
    // Sharing the column is not enough: that column also holds the two 50 px
    // value fields, so the strip's box is wider than the track by a field and a
    // gap at each end. Mapping the same domain fraction across two different
    // widths puts the handle and its own edge together at 0 and at the domain
    // maximum, where the ends pin, and furthest apart in the middle — which is
    // exactly where a band edge usually sits.
    //
    // So the strip is inset by what stands between the column edge and the track:
    // the value field, the flex gap, and the slider's own track padding. The
    // Slider header states this obligation for exactly this case — companion UI
    // that maps values to x "must mirror" GetTrackPaddingPx().
    UIElement* profileRow = InspectorUI::AddRow(parent);
    InspectorUI::AddLabel(profileRow, "");
    UIElement* profileField = InspectorUI::AddFieldContainer(profileRow);
    ApplySliderWithValueContainerStyle(profileField);

    const float32 trackInset =
        kInspectorSliderValueFieldWidthPx + kSliderValueGapPx + band->GetTrackPaddingPx();

    auto profile = std::make_unique<Editor::TerrainRuleWeightProfileStrip>();
    auto* profileRaw = profile.get();
    profileRaw->Overrides()
        .Set(Style::Height, StyleLength::Px(kProfileStripHeightPx))
        .Set(Style::MinHeight, StyleLength::Px(kProfileStripHeightPx))
        .Set(Style::FlexGrow, 1.0f)
        .Set(Style::MarginLeft, StyleLength::Px(trackInset))
        .Set(Style::MarginRight, StyleLength::Px(trackInset));
    profileRaw->SetCondition(condition, domain);
    profileField->AddChild(std::move(profile));

    auto summary = std::make_unique<EditorUI::CollapsibleInfoCard>(
        std::string(Vocab::ConditionBandSummary(condition, rule)));
    auto* summaryRaw = summary.get();
    parent->AddChild(std::move(summary));

    // Live redraw of the strip, the sentence, the two fields and the band
    // tooltip from the values being dragged.
    //
    // Reads the PRIMARY entity's condition rather than taking whatever copy an
    // apply lambda was handed: with several volumes co-selected the edit is
    // mirrored to each of them in turn, so refreshing from inside apply would
    // leave the strip showing whichever entity happened to be applied last.
    // The row is drawn for the primary, so the primary is what it must show.
    auto refreshVisuals = [profileRaw, summaryRaw, lowField, highField, band, domain, target,
                           ruleIndex, conditionIndex]() {
        if (!target.World || !target.World->IsValid(target.Entity))
            return;
        const auto* live = target.World->GetComponent<RulesEffect>(target.Entity);
        if (!live || ruleIndex >= live->RuleCount)
            return;
        const TerrainSurfaceRule& liveRule = live->Rules[ruleIndex];
        const TerrainRuleCondition& c = liveRule.Conditions[conditionIndex];
        profileRaw->SetCondition(c, domain);
        // Carries the LIVE rule: the plot is the condition's own weight (see the
        // vocabulary's decision note), so the sentence is the only place that can
        // stop claiming "full weight" when the row scales it or replaces outright.
        summaryRaw->SetText(Vocab::ConditionBandSummary(c, liveRule));
        // WithoutNotify: these are mirrors of the edit in flight, not new edits.
        lowField->SetValueWithoutNotify(c.Min);
        highField->SetValueWithoutNotify(c.Max);
        // The tooltip used to repeat the row's own name over the strip it
        // covered. It carries the live band instead, and nothing else does.
        band->SetTooltip(Vocab::ConditionBandRangeText(c));
    };
    band->SetTooltip(Vocab::ConditionBandRangeText(condition));
    if (refreshers)
        refreshers->push_back(refreshVisuals);

    // The feather row's own widgets, written back by a refresh that OUTLIVES the
    // function building them. Declared before the row because the refresh is
    // shared with the band, and filled in once the row exists.
    //
    // Held by shared_ptr and captured BY VALUE, which is the whole point. The
    // obvious spelling — two local pointers captured by REFERENCE and assigned
    // after the row is built — compiles, reads correctly, and dangles the instant
    // this function returns: the closures are owned by the controls, which outlive
    // this frame by the life of the Inspector. It crashed on the first feather
    // label-scrub, dereferencing a dead stack slot whose contents had since been
    // overwritten with something non-null enough to pass the guard.
    struct FeatherMirrors
    {
        Slider* Track = nullptr;
        FloatField* Value = nullptr;
    };
    auto featherMirrors = std::make_shared<FeatherMirrors>();

    auto refreshFeather = [refreshVisuals, featherMirrors, target, ruleIndex, conditionIndex]() {
        refreshVisuals();
        if (!target.World || !target.World->IsValid(target.Entity))
            return;
        const auto* live = target.World->GetComponent<RulesEffect>(target.Entity);
        if (!live || ruleIndex >= live->RuleCount)
            return;
        const float32 feather = live->Rules[ruleIndex].Conditions[conditionIndex].Feather;
        if (featherMirrors->Track)
            featherMirrors->Track->SetValueWithoutNotify(feather);
        if (featherMirrors->Value)
            featherMirrors->Value->SetValueWithoutNotify(feather);
    };

    auto applyBand = [ruleIndex, conditionIndex](RulesEffect& u, RuleBand b) {
        TerrainRuleCondition& c = u.Rules[ruleIndex].Conditions[conditionIndex];
        c.Min = b.Low;
        c.Max = b.High;
    };

    auto bandHandlers =
        MakeRuleDragHandlers<RuleBand>(ctx, "Condition Band", applyBand, refreshVisuals);
    auto bandPreview = std::move(bandHandlers.first);
    auto bandCommit = std::move(bandHandlers.second);

    // BOTH handles route through this one callback, and its argument is always
    // the HIGH value (the low thumb notifies without changing Field's value), so
    // the pair is read off the slider rather than taken from the argument.
    band->SetOnValueChanging([band, bandPreview](const float&) {
        bandPreview(RuleBand{band->GetRangeStart(), band->GetValue()});
    });
    band->SetOnValueChanged([band, bandCommit](const float&) {
        bandCommit(RuleBand{band->GetRangeStart(), band->GetValue()});
    });

    // Typed edges commit directly. Each field owns ONE end, and neither reorders
    // the pair: typing a low above the high is how an author says "empty band",
    // and the row notice already names that state and its fix.
    lowField->SetOnValueChanged([band, highField, bandCommit](const float& v) {
        bandCommit(RuleBand{v, highField->GetValue()});
        band->SetRangeValues(v, highField->GetValue());
    });
    highField->SetOnValueChanged([band, lowField, bandCommit](const float& v) {
        bandCommit(RuleBand{lowField->GetValue(), v});
        band->SetRangeValues(lowField->GetValue(), v);
    });

    // ---- feather + curve ----
    {
        auto handlers = MakeRuleDragHandlers<float>(
            ctx, "Condition Feather",
            [ruleIndex, conditionIndex](RulesEffect& u, float v) {
                u.Rules[ruleIndex].Conditions[conditionIndex].Feather = std::max(0.0f, v);
            },
            refreshFeather);
        // Field AND slider, like the band and Strength. Feather was the one
        // field-only control in the row, so the three quantities an author
        // tunes together each took a different gesture.
        auto row = AddSliderWithFloatValueRow(
            parent, "Feather", condition.Feather, 0.0f, kFeatherSliderMaxFraction * domainSpan,
            "How far PAST each edge the weight takes to reach zero, in this condition's own unit. "
            "0 is a hard edge - and a hard edge between two materials is a hard seam on the "
            "ground. Type a value to exceed the slider's range.");
        auto preview = std::move(handlers.first);
        auto commit = std::move(handlers.second);
        row.Slider->SetOnValueChanging([preview](const float& v) { preview(v); });
        row.Slider->SetOnValueChanged([commit](const float& v) { commit(v); });
        row.ValueField->SetOnValueChanged([commit](const float& v) { commit(v); });
        row.ValueField->SetFixedDecimalPlaces(BandFieldDecimals(condition.Kind));
        featherMirrors->Track = row.Slider;
        featherMirrors->Value = row.ValueField;
    }

    {
        static const std::vector<Dropdown::Option> kCurveOptions = {
            {"0", "Linear"},
            {"1", "Smoothstep"},
        };
        auto* dd = InspectorUI::AddDropdownRow(
            parent, "Feather Shape", kCurveOptions, static_cast<int>(condition.FalloffCurve),
            "The ramp's shape across the feather. Linear is the default and the only shape that "
            "keeps the edge exactly as soft as the Feather; Smoothstep rounds the corners.");
        dd->SetOnValueChanged([target, ruleIndex, conditionIndex, refreshVisuals](
                                  const std::string& value) {
            const Curve curve = static_cast<Curve>(std::stoi(value));
            EditRules(target, "Feather Shape", [=](RulesEffect& u) {
                u.Rules[ruleIndex].Conditions[conditionIndex].FalloffCurve = curve;
            });
            refreshVisuals();
        });
    }

    // ---- noise field, only where it is read ----
    if (condition.Kind == Kind::Noise)
    {
        auto frequencyHandlers = MakeRuleDragHandlers<float>(
            ctx, "Noise Frequency",
            [ruleIndex, conditionIndex](RulesEffect& u, float v) {
                u.Rules[ruleIndex].Conditions[conditionIndex].NoiseFrequency =
                    std::max(0.0001f, v);
            });
        AddFloatRowWithDrag(
            parent, "Noise Frequency", condition.NoiseFrequency,
            std::move(frequencyHandlers.first), std::move(frequencyHandlers.second),
            /*defaultValue=*/0.02f,
            "Higher is finer: low values give broad blotches, high values a fine speckle. "
            "Per condition, so a coarse rule and a fine one can sit in the same rule set.");

        auto seedHandlers = MakeRuleDragHandlers<int>(
            ctx, "Noise Seed",
            [ruleIndex, conditionIndex](RulesEffect& u, int v) {
                u.Rules[ruleIndex].Conditions[conditionIndex].NoiseSeed =
                    static_cast<uint32>(std::max(0, v));
            });
        AddIntRowWithDrag(
            parent, "Noise Seed", static_cast<int>(condition.NoiseSeed),
            std::move(seedHandlers.first), std::move(seedHandlers.second),
            /*defaultValue=*/0,
            "Change to get a different pattern at the same frequency.");
    }

}

// ---- the actions a rule and a condition offer -------------------------------
//
// Each is the whole edit, so the header menu is the only place it is spelled out. They live here
// rather than inside the menu construction because a menu entry is a label and an icon over one
// of these — the edit is not a property of how it is presented.

void RemoveCondition(const RuleEditTarget& target, uint32 ruleIndex, uint32 conditionIndex)
{
    EditRules(target, "Remove Rule Condition", [=](RulesEffect& u) {
        TerrainSurfaceRule& r = u.Rules[ruleIndex];
        for (uint32 i = conditionIndex; i + 1 < r.ConditionCount; ++i)
            r.Conditions[i] = r.Conditions[i + 1];
        if (r.ConditionCount > 0)
        {
            r.Conditions[r.ConditionCount - 1] = TerrainRuleCondition{};
            --r.ConditionCount;
        }
    });
    RequestRebuild(target);
}

void AddCondition(const RuleEditTarget& target, uint32 ruleIndex, float32 terrainHeightMetres)
{
    EditRules(target, "Add Rule Condition", [=](RulesEffect& u) {
        TerrainSurfaceRule& r = u.Rules[ruleIndex];
        if (r.ConditionCount >= kMaxTerrainRuleConditions)
            return;
        r.Conditions[r.ConditionCount] =
            Vocab::MakeDefaultCondition(Kind::SlopeDegrees, terrainHeightMetres);
        ++r.ConditionCount;
    });
    RequestRebuild(target);
}

void RemoveRule(const RuleEditTarget& target, uint32 ruleIndex)
{
    EditRules(target, "Remove Surface Rule", [=](RulesEffect& u) {
        for (uint32 i = ruleIndex; i + 1 < u.RuleCount; ++i)
            u.Rules[i] = u.Rules[i + 1];
        if (u.RuleCount > 0)
        {
            u.Rules[u.RuleCount - 1] = TerrainSurfaceRule{};
            --u.RuleCount;
        }
    });
    RequestRebuild(target);
}

// ---- one rule row ----------------------------------------------------------

void AddRuleRow(const InspectorContext& ctx, UIElement* parent, const RulesEffect& effect,
                uint32 ruleIndex, const Components::Terrain* terrain,
                TerrainMaterialLibraryAsset* library, float32 terrainHeightMetres)
{
    const RuleEditTarget target = MakeEditTarget(ctx);
    const TerrainSurfaceRule& rule = effect.Rules[ruleIndex];
    auto conditionRefreshers = std::make_shared<std::vector<std::function<void()>>>();

    // A section, not a bare foldout: the rule reads as one of the panel's own blocks, headed by
    // the same bar every other terrain heading wears. Keyed by index so a rule the author
    // collapsed stays collapsed through the rebuild every discrete edit triggers.
    Foldout* section = InspectorUI::AddComponentSection(
        parent, "TerrainSurfaceRules/Rule" + std::to_string(ruleIndex),
        "Rule " + std::to_string(ruleIndex + 1) + " - "
            + (terrain ? Editor::TerrainRoleMaterials::RoleLabel(*terrain, library,
                                                                rule.MaterialSlot)
                       : std::to_string(rule.MaterialSlot)));
    if (!section)
        return;

    UIElement* body = section->GetContentContainer();
    if (!body)
        return;

    // The rule's own actions, on its header. Laid out as buttons they cost a row each and stacked
    // up between this rule's last condition and the next rule's heading, where they read as the
    // rule's content rather than as things you do to it.
    const uint32 conditionCount =
        std::min<uint32>(rule.ConditionCount, kMaxTerrainRuleConditions);
    const bool conditionsFull = conditionCount >= kMaxTerrainRuleConditions;
    InspectorUI::AddSectionHeaderMenu(
        section, ctx.Window,
        {{"Add Condition", EditorIcons::kPlus, !conditionsFull,
          conditionsFull ? "limit reached" : "",
          [target, ruleIndex, terrainHeightMetres] {
              AddCondition(target, ruleIndex, terrainHeightMetres);
          }},
         {"Remove Rule", EditorIcons::kTrash, true, "",
          [target, ruleIndex] { RemoveRule(target, ruleIndex); }}});

    // The row's own state, at the row. A summary elsewhere is not a substitute:
    // at a real Inspector width the author dragging rule 6's band cannot see the
    // top of the effect.
    Editor::AddRuleRowStateNotice(body, rule, ruleIndex);

    // ---- result material ----
    //
    // The picker, and the material's albedo image beside it when one resolves.
    // The image answers "which material" faster than a name does once a library
    // holds eight of them, and it is the half a name cannot carry — two rows
    // reading "Rock 02" and "Rock 03" look identical until the ground does.
    //
    // The field container is set to a ROW explicitly: .inspector-field declares no
    // flex-direction, so it lays out as a COLUMN, which is what stacked the tint
    // swatch this replaces above its own dropdown. The picker's CSS width is 100%,
    // so it also needs a flex basis of 0 to share the row rather than claim it.
    {
        const std::vector<Dropdown::Option> options =
            terrain ? Editor::TerrainRoleMaterials::BuildRoleOptions(*terrain, library)
                    : ChannelIndexOptions();
        const int selected = std::clamp(static_cast<int>(rule.MaterialSlot), 0,
                                        static_cast<int>(options.size()) - 1);

        UIElement* row = InspectorUI::AddRow(body);
        InspectorUI::AddLabel(row, "Material",
                 "The material this rule writes where its conditions are met.");
        UIElement* field = InspectorUI::AddFieldContainer(row);
        field->Overrides()
            .Set(Style::FlexDir, FlexDirection::Row)
            .Set(Style::AlignItems, AlignItems::Center)
            .Set(Style::Gap, StyleLength::Px(kMaterialThumbnailGapPx));

        if (terrain)
        {
            AddMaterialThumbnail(field, ctx.Thumbnails,
                                 Editor::TerrainRoleMaterials::RoleAlbedoTexture(
                                     *terrain, library, rule.MaterialSlot),
                                 Editor::TerrainRoleMaterials::RoleLabel(*terrain, library,
                                                                        rule.MaterialSlot));
        }

        auto dropdown = std::make_unique<Dropdown>();
        Dropdown* dd = dropdown.get();
        dd->SetOptions(options, selected);
        dd->AddClass("inspector-dropdown");
        dd->Overrides()
            .Set(Style::FlexGrow, 1.0f)
            .Set(Style::FlexShrink, 1.0f)
            .Set(Style::FlexBasis, StyleLength::Px(0.0f))
            .Set(Style::MinWidth, StyleLength::Px(0.0f));
        field->AddChild(std::move(dropdown));

        dd->SetOnValueChanged([target, ruleIndex](const std::string& value) {
            EditRules(target, "Rule Material", [=](RulesEffect& u) {
                u.Rules[ruleIndex].MaterialSlot = static_cast<uint32>(std::stoi(value));
            });
            RequestRebuild(target); // the foldout title and the thumbnail both change
        });
    }

    // ---- blend / replace ----
    {
        static const std::vector<Dropdown::Option> kModeOptions = {
            {"0", "Blend"},
            {"1", "Replace"},
        };
        auto* dd = InspectorUI::AddDropdownRow(
            body, "Mode", kModeOptions, rule.Replace ? 1 : 0,
            "Blend adds this rule's weight to what earlier rules and paint left, then "
            "renormalizes. Replace lerps every channel toward this material, so the rule reads "
            "as that material alone.");
        dd->SetOnValueChanged([target, ruleIndex, conditionRefreshers](const std::string& value) {
            EditRules(target, "Rule Mode", [=](RulesEffect& u) {
                u.Rules[ruleIndex].Replace = value == "1";
            });
            // The captions say what the rule does with each condition's weight,
            // and the two modes do different things with it. Same channel the
            // Strength row needs, and for the same reason: Mode is built in the
            // rule's scope and the captions in each condition's.
            RefreshAllConditions(conditionRefreshers);
        });
    }

    {
        auto row = AddSliderWithFloatValueRow(
            body, "Strength", rule.Strength, 0.0f, 1.0f,
            "Master weight for the row, multiplied into its conditions' product.");
        row.ValueField->SetFixedDecimalPlaces(kUnitFieldDecimals);
        auto handlers = MakeRuleDragHandlers<float>(
            ctx, "Rule Strength",
            [ruleIndex](RulesEffect& u, float v) {
                u.Rules[ruleIndex].Strength = std::clamp(v, 0.0f, 1.0f);
            },
            [slider = row.Slider, field = row.ValueField, target, ruleIndex,
             conditionRefreshers]() {
                if (!target.World || !target.World->IsValid(target.Entity))
                    return;
                const auto* live = target.World->GetComponent<RulesEffect>(target.Entity);
                if (!live || ruleIndex >= live->RuleCount)
                    return;
                const float32 strength = live->Rules[ruleIndex].Strength;
                slider->SetValueWithoutNotify(strength);
                field->SetValueWithoutNotify(strength);
                // The captions state what the rule does with this condition's
                // weight, so they are part of Strength's output, not a
                // neighbouring row that happens to mention it.
                RefreshAllConditions(conditionRefreshers);
            });
        auto preview = std::move(handlers.first);
        auto commit = std::move(handlers.second);
        row.Slider->SetOnValueChanging([preview](const float& v) { preview(v); });
        row.Slider->SetOnValueChanged([commit](const float& v) { commit(v); });
        row.ValueField->SetOnValueChanged([commit](const float& v) { commit(v); });
    }

    // ---- conditions ----
    for (uint32 c = 0; c < conditionCount; ++c)
    {
        // Nested inside the rule's section, so the stylesheet indents its bar under the rule's
        // title rather than pulling it back out to the panel edges.
        Foldout* conditionSection = InspectorUI::AddComponentSection(
            body,
            "TerrainSurfaceRules/Rule" + std::to_string(ruleIndex) + "/Condition"
                + std::to_string(c),
            "Condition " + std::to_string(c + 1) + " - "
                + std::string(Vocab::ConditionKindLabel(rule.Conditions[c].Kind)));

        InspectorUI::AddSectionHeaderMenu(
            conditionSection, ctx.Window,
            {{"Remove Condition", EditorIcons::kTrash, true, "",
              [target, ruleIndex, c] { RemoveCondition(target, ruleIndex, c); }}});

        if (UIElement* conditionBody =
                conditionSection ? conditionSection->GetContentContainer() : nullptr)
            AddConditionRows(ctx, conditionBody, rule, ruleIndex, c, terrainHeightMetres,
                             conditionRefreshers);
    }

    // The cap stays a NOTICE in the body even though the action moved to the header: a greyed
    // menu row only says so once the menu is open, and the reason a rule stopped accepting
    // conditions has to be readable without going looking for it.
    if (conditionsFull)
        Editor::AddConditionCapNotice(body, ruleIndex, conditionCount);
}

} // namespace

void AddTerrainSurfaceRuleRows(const InspectorContext& ctx, const RulesEffect& effect,
                               const Components::Terrain* terrain,
                               TerrainMaterialLibraryAsset* library)
{
    if (!ctx.Parent)
        return;

    const RuleEditTarget target = MakeEditTarget(ctx);
    const float32 terrainHeightMetres = terrain ? terrain->HeightScale : 0.0f;
    const uint32 ruleCount = std::min<uint32>(effect.RuleCount, kMaxTerrainSurfaceRules);

    Editor::AddSurfaceRulesEmptyNotice(ctx.Parent, ruleCount);

    InspectorUI::AddInfoCard(ctx.Parent, std::string(Vocab::SurfaceRulesSectionHelp()));

    // Add Rule LEADS the list. Under every rule and condition foldout it sat
    // roughly a fifth of a panel below the fold, so the affordance that starts
    // the whole workflow was the least reachable thing in the panel. The section
    // header would be its ideal home, but no Inspector API lets a component
    // inspector place a widget there — recorded as deferred rather than done.
    const bool rulesFull = ruleCount >= kMaxTerrainSurfaceRules;
    AddPrimaryAddButton(ctx.Parent, "Add Rule",
                    "Add a rule. A new rule starts with no conditions, so it covers the whole "
                    "volume until you give it one.",
                    [target](UIEvent&) {
                        EditRules(target, "Add Surface Rule", [](RulesEffect& u) {
                            if (u.RuleCount >= kMaxTerrainSurfaceRules)
                                return;
                            u.Rules[u.RuleCount] = TerrainSurfaceRule{};
                            ++u.RuleCount;
                        });
                        RequestRebuild(target);
                    },
                    rulesFull);
    if (rulesFull)
        Editor::AddRuleCapNotice(ctx.Parent, ruleCount);

    for (uint32 r = 0; r < ruleCount; ++r)
        AddRuleRow(ctx, ctx.Parent, effect, r, terrain, library, terrainHeightMetres);
}

} // namespace GameEngine
