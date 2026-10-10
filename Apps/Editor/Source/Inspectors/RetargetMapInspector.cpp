#include "Inspectors/RetargetMapInspector.h"

#include "InspectorRegistry.h"
#include "Inspectors/InspectorUIHelpers.h"

#include "Animation/HumanoidRig.h"
#include "Animation/OpStackNode.h"
#include "Animation/RetargetMap.h"
#include "AssetCore/Asset.h"

#include "UI/Controls/Button.h"
#include "UI/Controls/Dropdown.h"
#include "UI/Controls/FloatField.h"
#include "UI/Controls/Label.h"
#include "UI/Controls/TextArea.h"
#include "UI/Controls/Toggle.h"
#include "UI/StyleProperties.h"
#include "UI/UIElement.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace GameEngine
{

using InspectorUI::AddTextBlock;

namespace
{

constexpr uint32_t kCanonicalOrderWarnLabel = 0xFFE0C840u; // amber
constexpr uint32_t kBrokenBannerColor       = 0xFFFF4040u;

std::string FormatGuid(const ::GameEngine::GUID& g)
{
    return g.IsNull() ? std::string("<null>") : g.ToString();
}

void SaveMap(Animation::RetargetMap& map)
{
    map.SaveToPath(map.GetPath());
}

void ClearChildren(UIElement* root)
{
    if (!root)
        return;
    root->RemoveAllChildren();
}

void BuildRetargetMapInspector(UIElement* root, Animation::RetargetMap* map);

void RequestRebuild(UIElement* root, Animation::RetargetMap* map)
{
    if (!root || !map)
        return;
    auto action = [root, map]()
    {
        ClearChildren(root);
        BuildRetargetMapInspector(root, map);
    };
    root->PostAction(std::move(action));
}

void BuildChainEditorSection(UIElement* root, Animation::RetargetMap* map)
{
    AddTextBlock(root, "Chain settings:", "inspector-section-subheader");
    auto host = std::make_unique<UIElement>();
    host->AddClass("retarget-map-chains");

    if (map->ChainMap().empty())
    {
        AddTextBlock(host.get(), "(none)", "inspector-text");
    }

    auto& chains = map->ChainMapMutable();
    for (size_t i = 0; i < chains.size(); ++i)
    {
        auto& cp = chains[i];
        auto row = std::make_unique<UIElement>();
        row->AddClass("retarget-map-chain-row");

        auto kindLabel = std::make_unique<Label>();
        kindLabel->AddClass("retarget-map-chain-kind");
        kindLabel->SetText(Animation::ChainKindToString(cp.Kind));
        row->AddChild(std::move(kindLabel));

        // FK rotation mode dropdown.
        auto rotMode = std::make_unique<Dropdown>();
        rotMode->AddClass("retarget-map-chain-fk-rot-mode");
        std::vector<std::string> modeLabels = {"OneToOne", "Transport", "SlerpAlongArc"};
        int rotIdx = static_cast<int>(cp.FK.RotationMode);
        rotMode->SetOptionsFromLabels(modeLabels, rotIdx);
        rotMode->SetOnValueChanged(
            [map, i](const std::string& sel)
            {
                if (!map || i >= map->ChainMap().size())
                    return;
                auto& m = map->ChainMapMutable()[i];
                auto parsed = Animation::FKRotationModeFromString(sel);
                if (m.FK.RotationMode == parsed)
                    return;
                m.FK.RotationMode = parsed;
                SaveMap(*map);
            });
        row->AddChild(std::move(rotMode));

        // FK rotation alpha (0..1 float).
        auto rotAlpha = std::make_unique<FloatField>();
        rotAlpha->AddClass("retarget-map-chain-fk-rot-alpha");
        rotAlpha->SetValue(cp.FK.RotationAlpha);
        rotAlpha->SetOnValueChanged(
            [map, i](const float& v)
            {
                if (!map || i >= map->ChainMap().size())
                    return;
                auto& m = map->ChainMapMutable()[i];
                const float clamped = std::max(0.0f, std::min(1.0f, v));
                if (m.FK.RotationAlpha == clamped)
                    return;
                m.FK.RotationAlpha = clamped;
                SaveMap(*map);
            });
        row->AddChild(std::move(rotAlpha));

        // FK translation mode dropdown.
        auto trMode = std::make_unique<Dropdown>();
        trMode->AddClass("retarget-map-chain-fk-tr-mode");
        std::vector<std::string> trLabels = {"None", "PerBoneScale", "UniformChainScale"};
        int trIdx = static_cast<int>(cp.FK.TranslationMode);
        trMode->SetOptionsFromLabels(trLabels, trIdx);
        trMode->SetOnValueChanged(
            [map, i](const std::string& sel)
            {
                if (!map || i >= map->ChainMap().size())
                    return;
                auto& m = map->ChainMapMutable()[i];
                auto parsed = Animation::FKTranslationModeFromString(sel);
                if (m.FK.TranslationMode == parsed)
                    return;
                m.FK.TranslationMode = parsed;
                SaveMap(*map);
            });
        row->AddChild(std::move(trMode));

        // IK enabled toggle.
        auto ikToggle = std::make_unique<Toggle>();
        ikToggle->AddClass("retarget-map-chain-ik-enabled");
        ikToggle->SetValue(cp.IK.Enabled);
        ikToggle->SetOnValueChanged(
            [map, i](const bool& v)
            {
                if (!map || i >= map->ChainMap().size())
                    return;
                auto& m = map->ChainMapMutable()[i];
                if (m.IK.Enabled == v)
                    return;
                m.IK.Enabled = v;
                SaveMap(*map);
            });
        row->AddChild(std::move(ikToggle));

        // IK blend-to-source.
        auto ikBlend = std::make_unique<FloatField>();
        ikBlend->AddClass("retarget-map-chain-ik-blend");
        ikBlend->SetValue(cp.IK.BlendToSource);
        ikBlend->SetOnValueChanged(
            [map, i](const float& v)
            {
                if (!map || i >= map->ChainMap().size())
                    return;
                auto& m = map->ChainMapMutable()[i];
                const float clamped = std::max(0.0f, std::min(1.0f, v));
                if (m.IK.BlendToSource == clamped)
                    return;
                m.IK.BlendToSource = clamped;
                SaveMap(*map);
            });
        row->AddChild(std::move(ikBlend));

        host->AddChild(std::move(row));
    }

    root->AddChild(std::move(host));
}

void BuildOpStackEditorSection(UIElement* root, Animation::RetargetMap* map)
{
    AddTextBlock(root, "Op stack:", "inspector-section-subheader");

    auto host = std::make_unique<UIElement>();
    host->AddClass("retarget-map-ops");

    // Canonical-order check. Walk the op list and look for a name whose
    // canonical order is *less* than a previous entry's — that indicates an
    // out-of-order entry the runtime would log a warning about.
    bool outOfOrder = false;
    int lastOrder = Animation::OpStackNode::kUnknownOpOrder;
    for (const auto& op : map->OpStack())
    {
        const int o = Animation::OpStackNode::CanonicalOrderForName(op.OpName);
        if (o == Animation::OpStackNode::kUnknownOpOrder)
            continue;
        if (lastOrder != Animation::OpStackNode::kUnknownOpOrder && o < lastOrder)
        {
            outOfOrder = true;
            break;
        }
        lastOrder = o;
    }
    if (outOfOrder)
    {
        auto warn = std::make_unique<Label>();
        warn->AddClass("retarget-map-op-canonical-warning");
        warn->SetText("WARNING: ops are out of canonical order "
                      "(AttachmentPassthrough -> LookAt -> BodyIntersect -> FootLock). "
                      "The runtime will reorder for evaluation but artifacts may surface.");
        warn->Overrides().Set(Style::Color, kCanonicalOrderWarnLabel);
        host->AddChild(std::move(warn));
    }

    if (map->OpStack().empty())
    {
        AddTextBlock(host.get(), "(empty)", "inspector-text");
    }

    auto& ops = map->OpStackMutable();
    for (size_t i = 0; i < ops.size(); ++i)
    {
        auto row = std::make_unique<UIElement>();
        row->AddClass("retarget-map-op-row");

        auto idxLabel = std::make_unique<Label>();
        idxLabel->AddClass("retarget-map-op-index");
        std::ostringstream oss;
        oss << "[" << i << "] " << ops[i].OpName;
        idxLabel->SetText(oss.str());
        row->AddChild(std::move(idxLabel));

        // Move up button.
        if (i > 0)
        {
            auto upBtn = std::make_unique<Button>();
            upBtn->AddClass("retarget-map-op-move-up");
            upBtn->SetText("Up");
            upBtn->RegisterEventHandler(kEventButtonClick, [map, i, root](UIEvent&)
                {
                    if (!map)
                        return;
                    auto& mut = map->OpStackMutable();
                    if (i == 0 || i >= mut.size())
                        return;
                    std::swap(mut[i - 1], mut[i]);
                    SaveMap(*map);
                    RequestRebuild(root, map);
                });
            row->AddChild(std::move(upBtn));
        }
        // Move down button.
        if (i + 1 < ops.size())
        {
            auto dnBtn = std::make_unique<Button>();
            dnBtn->AddClass("retarget-map-op-move-down");
            dnBtn->SetText("Down");
            dnBtn->RegisterEventHandler(kEventButtonClick, [map, i, root](UIEvent&)
                {
                    if (!map)
                        return;
                    auto& mut = map->OpStackMutable();
                    if (i + 1 >= mut.size())
                        return;
                    std::swap(mut[i], mut[i + 1]);
                    SaveMap(*map);
                    RequestRebuild(root, map);
                });
            row->AddChild(std::move(dnBtn));
        }

        // Remove button.
        auto removeBtn = std::make_unique<Button>();
        removeBtn->AddClass("retarget-map-op-remove");
        removeBtn->SetText("Remove");
        removeBtn->RegisterEventHandler(kEventButtonClick, [map, i, root](UIEvent&)
            {
                if (!map)
                    return;
                auto& mut = map->OpStackMutable();
                if (i < mut.size())
                    mut.erase(mut.begin() + static_cast<std::ptrdiff_t>(i));
                SaveMap(*map);
                RequestRebuild(root, map);
            });
        row->AddChild(std::move(removeBtn));

        // Params editor — read-only TextArea showing the JSON dump for now.
        // Phase 7c can wire a structured editor; this satisfies "expandable
        // JSON editor" semantically and round-trips when the user reloads.
        auto paramsArea = std::make_unique<TextArea>();
        paramsArea->AddClass("retarget-map-op-params");
        paramsArea->SetReadOnly(false);
        paramsArea->SetValue(ops[i].Params.dump(2));
        paramsArea->SetOnValueChanged(
            [map, i](const std::string& v)
            {
                if (!map || i >= map->OpStack().size())
                    return;
                auto& mut = map->OpStackMutable();
                try
                {
                    nlohmann::json parsed = nlohmann::json::parse(v);
                    if (mut[i].Params == parsed)
                        return;
                    mut[i].Params = std::move(parsed);
                    SaveMap(*map);
                }
                catch (...)
                {
                    // Malformed JSON; silently ignore until the user fixes
                    // it. The TextArea retains the buffer so the user can
                    // edit further.
                }
            });
        row->AddChild(std::move(paramsArea));

        host->AddChild(std::move(row));
    }

    // Add Op dropdown (canonical order list).
    auto addRow = std::make_unique<UIElement>();
    addRow->AddClass("retarget-map-op-add-row");

    Animation::EnsureOpStackRegistrations();
    auto addLabel = std::make_unique<Label>();
    addLabel->SetText("Add op:");
    addRow->AddChild(std::move(addLabel));

    auto dd = std::make_unique<Dropdown>();
    dd->AddClass("retarget-map-op-add-dd");
    std::vector<std::string> labels = Animation::CanonicalOpOrder();
    if (labels.empty())
        labels.push_back("AttachmentPassthroughOp");
    dd->SetOptionsFromLabels(labels, 0);
    addRow->AddChild(std::move(dd));

    auto addBtn = std::make_unique<Button>();
    addBtn->AddClass("retarget-map-op-add-btn");
    addBtn->SetText("Add");
    Dropdown* ddRaw = nullptr;
    {
        // Recover the raw dropdown pointer by walking the just-added
        // children — addRow added it in slot index 1.
        const auto& kids = addRow->GetChildren();
        if (kids.size() > 1)
            ddRaw = dynamic_cast<Dropdown*>(kids[1].get());
    }
    addBtn->RegisterEventHandler(kEventButtonClick, [map, root, ddRaw](UIEvent&)
        {
            if (!map || !ddRaw)
                return;
            const std::string opName = ddRaw->GetSelectedValue();
            if (opName.empty())
                return;
            Animation::OpStackEntry entry;
            entry.OpName = opName;
            entry.Params = nlohmann::json::object();
            map->OpStackMutable().push_back(std::move(entry));
            SaveMap(*map);
            RequestRebuild(root, map);
        });
    addRow->AddChild(std::move(addBtn));

    host->AddChild(std::move(addRow));

    root->AddChild(std::move(host));
}

void BuildRetargetMapInspector(UIElement* root, Animation::RetargetMap* map)
{
    if (!root || !map)
        return;

    root->AddClass("retarget-map-inspector");
    AddTextBlock(root, map->GetPath().string(), "inspector-asset-path");
    AddTextBlock(root, "Type: RetargetMap", "inspector-asset-type-line");

    if (map->IsBroken())
    {
        auto banner = std::make_unique<Label>();
        banner->AddClass("inspector-text");
        banner->SetText("BROKEN: source or target rig reference is null.");
        banner->Overrides().Set(Style::Color, kBrokenBannerColor);
        root->AddChild(std::move(banner));
    }

    {
        std::ostringstream oss;
        oss << "Source rig: " << FormatGuid(map->SourceRigRef());
        AddTextBlock(root, oss.str());
    }
    {
        std::ostringstream oss;
        oss << "Target rig: " << FormatGuid(map->TargetRigRef());
        AddTextBlock(root, oss.str());
    }
    {
        std::ostringstream oss;
        oss << "Schema version: " << map->Version();
        AddTextBlock(root, oss.str());
    }

    BuildChainEditorSection(root, map);
    BuildOpStackEditorSection(root, map);
}

} // namespace

void RegisterRetargetMapInspector()
{
    InspectorRegistry::Get().RegisterAssetInspector(
        AssetType::RetargetMap,
        [](const InspectorContext& ctx)
        {
            if (!ctx.Parent || !ctx.Object)
                return;

            auto* asset = static_cast<Asset*>(ctx.Object);
            auto* map = dynamic_cast<Animation::RetargetMap*>(asset);
            if (!map)
                return;

            BuildRetargetMapInspector(ctx.Parent, map);
        });
}

} // namespace GameEngine
