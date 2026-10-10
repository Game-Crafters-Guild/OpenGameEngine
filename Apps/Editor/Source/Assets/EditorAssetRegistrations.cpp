#include "Assets/EditorAssetRegistrations.h"

#include "AssetCore/AssetTypes.h"
#include "Graph/GraphAsset.h"
#include "UI/Assets/UILayoutAsset.h"
#include "UI/Assets/UIStyleAsset.h"

namespace GameEngine
{

void RegisterEditorAssetTypes(AssetTypeRegistry& typeRegistry)
{
    if (!typeRegistry.IsAssetTypeRegistered(AssetType::UILayout))
    {
        AssetTypeRegistration uiLayoutReg(
            AssetType::UILayout,
            GetDefaultExtensionsForAssetType(AssetType::UILayout),
            [](const AssetMetadata& md) -> SharedPtr<Asset>
            {
                return std::static_pointer_cast<Asset>(std::make_shared<UILayoutAsset>(md.Guid, md.Path));
            },
            "UI Layout (.uxml/.xml)",
            100);
        (void)typeRegistry.RegisterAssetType(uiLayoutReg);
    }

    if (!typeRegistry.IsAssetTypeRegistered(AssetType::UIStyle))
    {
        AssetTypeRegistration uiStyleReg(
            AssetType::UIStyle,
            GetDefaultExtensionsForAssetType(AssetType::UIStyle),
            [](const AssetMetadata& md) -> SharedPtr<Asset>
            {
                return std::static_pointer_cast<Asset>(std::make_shared<UIStyleAsset>(md.Guid, md.Path));
            },
            "UI Stylesheet (.css/.uss)",
            100);
        (void)typeRegistry.RegisterAssetType(uiStyleReg);
    }

    if (!typeRegistry.IsAssetTypeRegistered(AssetType::Graph))
    {
        AssetTypeRegistration graphReg(
            AssetType::Graph,
            GetDefaultExtensionsForAssetType(AssetType::Graph),
            [](const AssetMetadata& md) -> SharedPtr<Asset>
            {
                return std::static_pointer_cast<Asset>(std::make_shared<GraphAsset>(md.Guid, md.Path));
            },
            "Node Graph (.graph)",
            100);
        (void)typeRegistry.RegisterAssetType(graphReg);
    }
}

} // namespace GameEngine
