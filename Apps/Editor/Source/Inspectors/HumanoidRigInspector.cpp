#include "Inspectors/HumanoidRigInspector.h"

#include "InspectorRegistry.h"
#include "Inspectors/InspectorUIHelpers.h"

#include "Animation/HumanBone.h"
#include "Animation/HumanoidNameMatcher.h"
#include "Animation/HumanoidRig.h"
#include "Animation/HumanoidRigEdit.h"
#include "AssetCore/Asset.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextField.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace GameEngine
{

using InspectorUI::AddTextBlock;

namespace
{

// Color codes used by the body-diagram-style status pills. ARGB.
constexpr uint32_t kStatusColorMapped       = 0xFF40C040u;
constexpr uint32_t kStatusColorRequired     = 0xFFE04040u;
constexpr uint32_t kStatusColorOptional     = 0xFF707070u;
constexpr uint32_t kAttachmentRowAccent     = 0xFF8FBCFFu;
constexpr uint32_t kIdentityRotationLabel   = 0xFF707070u;
constexpr uint32_t kPerturbedRotationLabel  = 0xFFE0C840u;

constexpr float kRotationIdentityEpsilonW = 0.99995f;

std::string FormatGuid(const ::GameEngine::GUID& g)
{
    return g.IsNull() ? std::string("<null>") : g.ToString();
}

bool IsRotationIdentity(const Mathematics::Quaternion& q)
{
    return q.GetGLM().w >= kRotationIdentityEpsilonW;
}

// Persist the rig back to its source path. Mirrors MaterialInspector's
// SaveDocToDisk pattern: silent on failure (the asset stays in-memory clean
// either way).
void SaveRig(Animation::HumanoidRig& rig)
{
    rig.SaveToPath(rig.GetPath());
}

// Find an existing canonical mapping or append a new one. Returns a
// reference to the mapping in BoneMapMutable().
Animation::HumanoidBoneMapping&
GetOrAddMappingFor(Animation::HumanoidRig& rig, Animation::HumanBone canonical)
{
    auto& bm = rig.BoneMapMutable();
    for (auto& m : bm)
    {
        if (m.Canonical == canonical)
            return m;
    }
    Animation::HumanoidBoneMapping m;
    m.Canonical = canonical;
    bm.push_back(m);
    return bm.back();
}

// Adds a child UIElement and returns the raw pointer for further mutation.
template <typename T>
T* AddChildPtr(UIElement* parent, std::unique_ptr<T> child)
{
    T* raw = child.get();
    if (parent)
        parent->AddChild(std::move(child));
    return raw;
}

void ClearChildren(UIElement* root)
{
    if (!root)
        return;
    root->RemoveAllChildren();
}

// The set of canonical bones surfaced in the body-diagram. Mirrors plan §4
// Phase 7b: the 22 anatomical body slots (no fingers — those get a separate
// list because each hand has 15 entries).
constexpr std::array<Animation::HumanBone, 22> kBodySlotsInOrder = {
    Animation::HumanBone::Hips,
    Animation::HumanBone::Spine,
    Animation::HumanBone::Chest,
    Animation::HumanBone::UpperChest,
    Animation::HumanBone::Neck,
    Animation::HumanBone::Head,
    Animation::HumanBone::LeftShoulder,
    Animation::HumanBone::LeftUpperArm,
    Animation::HumanBone::LeftLowerArm,
    Animation::HumanBone::LeftHand,
    Animation::HumanBone::RightShoulder,
    Animation::HumanBone::RightUpperArm,
    Animation::HumanBone::RightLowerArm,
    Animation::HumanBone::RightHand,
    Animation::HumanBone::LeftUpperLeg,
    Animation::HumanBone::LeftLowerLeg,
    Animation::HumanBone::LeftFoot,
    Animation::HumanBone::LeftToes,
    Animation::HumanBone::RightUpperLeg,
    Animation::HumanBone::RightLowerLeg,
    Animation::HumanBone::RightFoot,
    Animation::HumanBone::RightToes,
};

uint32_t StatusColorForSlot(const Animation::HumanoidBoneMapping* mapping,
                            Animation::HumanBone canonical)
{
    if (mapping && !mapping->SourceBoneName.empty())
        return kStatusColorMapped;
    if (Animation::HumanoidNameMatcher::IsRequiredBone(canonical))
        return kStatusColorRequired;
    return kStatusColorOptional;
}

// Forward decls: full builder + per-section helpers can call back into the
// rebuild pipeline.
void BuildHumanoidRigInspector(UIElement* root, Animation::HumanoidRig* rig);

void RequestRebuild(UIElement* root, Animation::HumanoidRig* rig)
{
    if (!root || !rig)
        return;
    auto action = [root, rig]()
    {
        ClearChildren(root);
        BuildHumanoidRigInspector(root, rig);
    };
    root->PostAction(std::move(action));
}

// Build the dropdown of source-bone names suggested by the matcher's known
// pattern table. The dropdown defaults to the current binding when present.
// In Phase 7b we don't have access to the live source skeleton from the
// inspector — we surface the SourceBoneName as a freeform TextField for
// fallback editing alongside the dropdown.
void BuildBoneSlotRow(UIElement* host,
                      Animation::HumanoidRig* rig,
                      Animation::HumanBone canonical,
                      const Animation::HumanoidBoneMapping* mapping)
{
    auto row = std::make_unique<UIElement>();
    row->AddClass("humanoid-rig-slot-row");

    // Status pill — colored square indicating mapped/required-unmapped/
    // optional-unmapped state.
    auto pill = std::make_unique<UIElement>();
    pill->AddClass("humanoid-rig-slot-pill");
    pill->Overrides().Set(Style::BackgroundColor, StatusColorForSlot(mapping, canonical));
    row->AddChild(std::move(pill));

    auto label = std::make_unique<Label>();
    label->AddClass("humanoid-rig-slot-name");
    label->SetText(Animation::HumanBoneToString(canonical));
    row->AddChild(std::move(label));

    auto field = std::make_unique<TextField>();
    field->AddClass("humanoid-rig-slot-source");
    field->SetValue(mapping ? mapping->SourceBoneName : std::string());
    field->SetOnValueChanged(
        [rig, canonical](const std::string& newValue)
        {
            if (!rig)
                return;
            auto& m = GetOrAddMappingFor(*rig, canonical);
            if (m.SourceBoneName == newValue)
                return;
            m.SourceBoneName = newValue;
            // Source-name change invalidates the cached resolver index; the
            // RetargetSession resolver repopulates it on next bind.
            m.CachedSourceIndex = ~0u;
            SaveRig(*rig);
        });
    row->AddChild(std::move(field));

    // Trash / clear binding button.
    auto clearBtn = std::make_unique<Button>();
    clearBtn->AddClass("humanoid-rig-slot-clear");
    clearBtn->SetText("X");
    clearBtn->RegisterEventHandler(kEventButtonClick, [rig, canonical, host](UIEvent&)
        {
            if (!rig)
                return;
            for (auto& m : rig->BoneMapMutable())
            {
                if (m.Canonical == canonical)
                {
                    m.SourceBoneName.clear();
                    m.CachedSourceIndex = ~0u;
                    break;
                }
            }
            SaveRig(*rig);
            // Walk up to the inspector root to request rebuild.
            UIElement* cur = host;
            while (cur && !cur->HasClass("humanoid-rig-inspector"))
                cur = cur->GetParent();
            if (cur)
                RequestRebuild(cur, rig);
        });
    row->AddChild(std::move(clearBtn));

    // Show retarget-pose status (identity vs perturbed) so the user sees at
    // a glance which bones have hand-edited deltas.
    if (mapping)
    {
        auto rotLabel = std::make_unique<Label>();
        rotLabel->AddClass("humanoid-rig-slot-rotation-status");
        rotLabel->SetText(IsRotationIdentity(mapping->RetargetPoseRotation)
                              ? std::string("identity")
                              : std::string("delta"));
        rotLabel->Overrides().Set(Style::Color,
                                  IsRotationIdentity(mapping->RetargetPoseRotation)
                                      ? kIdentityRotationLabel
                                      : kPerturbedRotationLabel);
        row->AddChild(std::move(rotLabel));
    }

    host->AddChild(std::move(row));
}

void BuildBodyDiagramSection(UIElement* root, Animation::HumanoidRig* rig)
{
    AddTextBlock(root, "Body diagram:", "inspector-section-subheader");

    auto host = std::make_unique<UIElement>();
    host->AddClass("humanoid-rig-body-diagram");

    for (auto canonical : kBodySlotsInOrder)
    {
        const Animation::HumanoidBoneMapping* mapping = nullptr;
        for (const auto& m : rig->BoneMap())
        {
            if (m.Canonical == canonical)
            {
                mapping = &m;
                break;
            }
        }
        BuildBoneSlotRow(host.get(), rig, canonical, mapping);
    }

    root->AddChild(std::move(host));
}

void BuildRetargetPoseButtonsSection(UIElement* root, Animation::HumanoidRig* rig)
{
    AddTextBlock(root, "Retarget pose authoring:", "inspector-section-subheader");

    auto row = std::make_unique<UIElement>();
    row->AddClass("humanoid-rig-retargetpose-row");

    {
        auto btn = std::make_unique<Button>();
        btn->AddClass("humanoid-rig-retargetpose-capture");
        btn->SetText("Capture from preview");
        btn->RegisterEventHandler(kEventButtonClick, [rig, root](UIEvent&)
            {
                if (!rig)
                    return;
                // Phase 7b CPU seam: the editor's AnimationPreviewManager
                // owns the live preview-pose; the inspector currently
                // lacks a stable hook into per-bone local rotations until
                // Phase 7c lands the embedded viewport. As a defensive
                // fallback we leave the rig untouched but stamp identity on
                // any bone whose source name is empty (consistent with the
                // matcher's "no source" behavior). See plan §4 Phase 7b.
                auto& bm = rig->BoneMapMutable();
                for (auto& m : bm)
                {
                    if (m.SourceBoneName.empty())
                        m.RetargetPoseRotation = Mathematics::Quaternion::Identity();
                }
                SaveRig(*rig);
                RequestRebuild(root, rig);
            });
        row->AddChild(std::move(btn));
    }
    {
        auto btn = std::make_unique<Button>();
        btn->AddClass("humanoid-rig-retargetpose-bake");
        btn->SetText("Bake from clip frame...");
        btn->RegisterEventHandler(kEventButtonClick, [rig](UIEvent&)
            {
                if (!rig)
                    return;
                // Phase 7c hook point: the actual clip + frame picker modal
                // ships alongside the embedded preview viewport. The
                // BakeRetargetPoseFromClipFrame helper is in place and
                // tested; this inspector entry just opens the modal. For
                // now, the click is a no-op so the button is visible
                // (and tested via the helper directly, not the click path).
                SaveRig(*rig);
            });
        row->AddChild(std::move(btn));
    }
    {
        auto btn = std::make_unique<Button>();
        btn->AddClass("humanoid-rig-retargetpose-force-identity");
        btn->SetText("Force bind = retarget pose");
        btn->RegisterEventHandler(kEventButtonClick, [rig, root](UIEvent&)
            {
                if (!rig)
                    return;
                Animation::ForceRetargetPoseToIdentity(*rig);
                SaveRig(*rig);
                RequestRebuild(root, rig);
            });
        row->AddChild(std::move(btn));
    }

    root->AddChild(std::move(row));
}

void BuildBodyProportionsSection(UIElement* root, Animation::HumanoidRig* rig)
{
    AddTextBlock(root, "Body proportions:", "inspector-section-subheader");
    auto host = std::make_unique<UIElement>();
    host->AddClass("humanoid-rig-proportions");

    auto addFloat = [&](const char* label, float* outRef, const char* cls)
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("humanoid-rig-proportions-row");
        auto lbl = std::make_unique<Label>();
        lbl->SetText(label);
        row->AddChild(std::move(lbl));

        auto field = std::make_unique<FloatField>();
        field->AddClass(cls);
        field->SetValue(*outRef);
        field->SetOnValueChanged(
            [rig, outRef](const float& v)
            {
                if (!rig)
                    return;
                if (*outRef == v)
                    return;
                *outRef = v;
                SaveRig(*rig);
            });
        row->AddChild(std::move(field));
        host->AddChild(std::move(row));
    };

    addFloat("HipHeight", &rig->ProportionsMutable().HipHeight,
             "humanoid-rig-prop-hip");
    addFloat("ShoulderWidth", &rig->ProportionsMutable().ShoulderWidth,
             "humanoid-rig-prop-shoulder");
    addFloat("LegLength", &rig->ProportionsMutable().LegLength,
             "humanoid-rig-prop-leg");
    addFloat("ArmLength", &rig->ProportionsMutable().ArmLength,
             "humanoid-rig-prop-arm");

    root->AddChild(std::move(host));
}

void BuildAttachmentsSection(UIElement* root, Animation::HumanoidRig* rig)
{
    AddTextBlock(root, "Attachments:", "inspector-section-subheader");

    auto host = std::make_unique<UIElement>();
    host->AddClass("humanoid-rig-attachments");

    if (rig->Attachments().empty())
    {
        AddTextBlock(host.get(), "(none)", "inspector-text");
    }
    else
    {
        const auto& attachments = rig->Attachments();
        for (size_t i = 0; i < attachments.size(); ++i)
        {
            const auto& a = attachments[i];
            auto row = std::make_unique<UIElement>();
            row->AddClass("humanoid-rig-attachment-row");

            auto pill = std::make_unique<UIElement>();
            pill->AddClass("humanoid-rig-attachment-pill");
            pill->Overrides().Set(Style::BackgroundColor, kAttachmentRowAccent);
            row->AddChild(std::move(pill));

            // Editable name field.
            auto nameField = std::make_unique<TextField>();
            nameField->AddClass("humanoid-rig-attachment-name");
            nameField->SetValue(a.Name);
            nameField->SetOnValueChanged(
                [rig, i](const std::string& newName)
                {
                    if (!rig)
                        return;
                    if (i >= rig->Attachments().size())
                        return;
                    auto& mut = rig->AttachmentsMutable();
                    if (mut[i].Name == newName)
                        return;
                    mut[i].Name = newName;
                    SaveRig(*rig);
                });
            row->AddChild(std::move(nameField));

            // Parent bone dropdown.
            auto dd = std::make_unique<Dropdown>();
            dd->AddClass("humanoid-rig-attachment-parent");
            std::vector<std::string> labels;
            labels.reserve(static_cast<size_t>(Animation::HumanBone::Count));
            for (uint32_t b = 0; b < static_cast<uint32_t>(Animation::HumanBone::Count); ++b)
            {
                labels.emplace_back(Animation::HumanBoneToString(static_cast<Animation::HumanBone>(b)));
            }
            int selectedIndex = static_cast<int>(a.ParentBone);
            dd->SetOptionsFromLabels(labels, selectedIndex);
            dd->SetOnValueChanged(
                [rig, i](const std::string& selectedLabel)
                {
                    if (!rig)
                        return;
                    if (i >= rig->Attachments().size())
                        return;
                    auto& mut = rig->AttachmentsMutable();
                    Animation::HumanBone parsed = Animation::HumanBoneFromString(selectedLabel);
                    if (mut[i].ParentBone == parsed)
                        return;
                    mut[i].ParentBone = parsed;
                    SaveRig(*rig);
                });
            row->AddChild(std::move(dd));

            // Mode dropdown.
            auto modeDd = std::make_unique<Dropdown>();
            modeDd->AddClass("humanoid-rig-attachment-mode");
            std::vector<std::string> modeLabels = {"CopyLocal", "Procedural", "Static"};
            int modeIndex = static_cast<int>(a.Mode);
            modeDd->SetOptionsFromLabels(modeLabels, modeIndex);
            modeDd->SetOnValueChanged(
                [rig, i](const std::string& selectedLabel)
                {
                    if (!rig)
                        return;
                    if (i >= rig->Attachments().size())
                        return;
                    auto& mut = rig->AttachmentsMutable();
                    auto parsed = Animation::AttachmentPassthroughModeFromString(selectedLabel);
                    if (mut[i].Mode == parsed)
                        return;
                    mut[i].Mode = parsed;
                    SaveRig(*rig);
                });
            row->AddChild(std::move(modeDd));

            // Remove button.
            auto removeBtn = std::make_unique<Button>();
            removeBtn->AddClass("humanoid-rig-attachment-remove");
            removeBtn->SetText("Remove");
            removeBtn->RegisterEventHandler(kEventButtonClick, [rig, i, root](UIEvent&)
                {
                    if (!rig)
                        return;
                    auto& mut = rig->AttachmentsMutable();
                    if (i < mut.size())
                        mut.erase(mut.begin() + static_cast<std::ptrdiff_t>(i));
                    SaveRig(*rig);
                    RequestRebuild(root, rig);
                });
            row->AddChild(std::move(removeBtn));

            host->AddChild(std::move(row));
        }
    }

    auto addBtn = std::make_unique<Button>();
    addBtn->AddClass("humanoid-rig-attachment-add");
    addBtn->SetText("Add Attachment");
    addBtn->RegisterEventHandler(kEventButtonClick, [rig, root](UIEvent&)
        {
            if (!rig)
                return;
            Animation::AttachmentBone fresh;
            fresh.Name = "NewAttachment";
            fresh.ParentBone = Animation::HumanBone::Chest;
            fresh.Mode = Animation::AttachmentPassthroughMode::CopyLocal;
            rig->AttachmentsMutable().push_back(fresh);
            SaveRig(*rig);
            RequestRebuild(root, rig);
        });
    host->AddChild(std::move(addBtn));

    root->AddChild(std::move(host));
}

void BuildChainsSection(UIElement* root, Animation::HumanoidRig* rig)
{
    AddTextBlock(root, "Chains:", "inspector-section-subheader");
    auto host = std::make_unique<UIElement>();
    host->AddClass("humanoid-rig-chains");
    if (rig->Chains().empty())
    {
        AddTextBlock(host.get(), "(none)", "inspector-text");
    }
    for (const auto& c : rig->Chains())
    {
        std::ostringstream oss;
        oss << Animation::ChainKindToString(c.Kind)
            << "  " << Animation::HumanBoneToString(c.Start)
            << " -> " << Animation::HumanBoneToString(c.End)
            << "  (" << c.IncludeBones.size() << " bones)";
        AddTextBlock(host.get(), oss.str(), "inspector-text");
    }
    root->AddChild(std::move(host));
}

void BuildHumanoidRigInspector(UIElement* root, Animation::HumanoidRig* rig)
{
    if (!root || !rig)
        return;

    root->AddClass("humanoid-rig-inspector");
    AddTextBlock(root, rig->GetPath().string(), "inspector-asset-path");
    AddTextBlock(root, "Type: HumanoidRig", "inspector-asset-type-line");

    {
        std::ostringstream oss;
        oss << "Profile GUID: " << FormatGuid(rig->ProfileRef());
        AddTextBlock(root, oss.str());
    }
    {
        std::ostringstream oss;
        oss << "Schema version: " << rig->Version();
        AddTextBlock(root, oss.str());
    }
    {
        std::ostringstream oss;
        oss << "Bone count: " << rig->BoneMap().size();
        AddTextBlock(root, oss.str(), "inspector-section-subheader");
    }

    BuildBodyDiagramSection(root, rig);
    BuildRetargetPoseButtonsSection(root, rig);
    BuildBodyProportionsSection(root, rig);
    BuildChainsSection(root, rig);
    BuildAttachmentsSection(root, rig);
}

} // namespace

void RegisterHumanoidRigInspector()
{
    InspectorRegistry::Get().RegisterAssetInspector(
        AssetType::HumanoidRig,
        [](const InspectorContext& ctx)
        {
            if (!ctx.Parent || !ctx.Object)
                return;

            auto* asset = static_cast<Asset*>(ctx.Object);
            auto* rig = dynamic_cast<Animation::HumanoidRig*>(asset);
            if (!rig)
                return;

            BuildHumanoidRigInspector(ctx.Parent, rig);
        });
}

} // namespace GameEngine
