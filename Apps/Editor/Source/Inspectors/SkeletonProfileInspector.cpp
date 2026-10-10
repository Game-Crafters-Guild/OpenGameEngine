#include "Inspectors/SkeletonProfileInspector.h"

#include "InspectorRegistry.h"
#include "Inspectors/InspectorUIHelpers.h"

#include "Animation/HumanBone.h"
#include "Animation/SkeletonProfile.h"
#include "AssetCore/Asset.h"

#include "UI/Controls/Label.h"
#include "UI/UIElement.h"

#include <sstream>
#include <string>

namespace GameEngine
{

using InspectorUI::AddTextBlock;

namespace
{

std::string FormatVec3(const Mathematics::Vector3& v)
{
    std::ostringstream oss;
    oss << "(" << v.x << ", " << v.y << ", " << v.z << ")";
    return oss.str();
}

void BuildSkeletonProfileInspector(UIElement* root, Animation::SkeletonProfile* profile)
{
    if (!root || !profile)
        return;

    AddTextBlock(root, profile->GetPath().string(), "inspector-asset-path");
    AddTextBlock(root, "Type: SkeletonProfile (read-only)", "inspector-asset-type-line");

    {
        std::ostringstream oss;
        oss << "Name: " << (profile->Name().empty() ? "<unnamed>" : profile->Name());
        AddTextBlock(root, oss.str());
    }
    {
        std::ostringstream oss;
        oss << "Description: " << profile->Description();
        AddTextBlock(root, oss.str());
    }
    {
        std::ostringstream oss;
        oss << "Schema version: " << profile->Version();
        AddTextBlock(root, oss.str());
    }
    {
        std::ostringstream oss;
        oss << "Bone count: " << profile->Bones().size();
        AddTextBlock(root, oss.str(), "inspector-section-subheader");
    }

    AddTextBlock(root, "Bones:", "inspector-section-subheader");

    auto listHost = std::make_unique<UIElement>();
    listHost->AddClass("inspector-skeleton-profile-bones");

    for (const auto& bone : profile->Bones())
    {
        std::ostringstream oss;
        oss << Animation::HumanBoneToString(bone.Bone)
            << "  parent=" << Animation::HumanBoneToString(bone.Parent)
            << "  rest=" << FormatVec3(bone.RestTranslation);
        AddTextBlock(listHost.get(), oss.str(), "inspector-text");
    }

    root->AddChild(std::move(listHost));
}

} // namespace

void RegisterSkeletonProfileInspector()
{
    InspectorRegistry::Get().RegisterAssetInspector(
        AssetType::SkeletonProfile,
        [](const InspectorContext& ctx)
        {
            if (!ctx.Parent || !ctx.Object)
                return;

            auto* asset = static_cast<Asset*>(ctx.Object);
            auto* profile = dynamic_cast<Animation::SkeletonProfile*>(asset);
            if (!profile)
                return;

            BuildSkeletonProfileInspector(ctx.Parent, profile);
        });
}

} // namespace GameEngine
