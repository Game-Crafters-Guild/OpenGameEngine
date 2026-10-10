#pragma once

#include "Assets/BinaryAsset.h"
#include "Assets/ParserRegistry.h"

namespace GameEngine
{

// Native C++ user-script source (.cpp/.h/.hpp).
//
// C9 skeleton: classifies the file as AssetType::NativeSource and produces a
// lightweight placeholder asset so the source is tracked by the registry. It does
// NOT compile anything — turning native sources into a hot-reloadable user DLL is
// the NativeScripting module's job (C10+).
class CppSourceAssetParser final : public AssetParser
{
  public:
    AssetType GetAssetType() const override { return AssetType::NativeSource; }

    std::vector<std::string> GetSupportedExtensions() const override { return {".cpp", ".h", ".hpp"}; }

    AssetParseResult Parse(const AssetMetadata& metadata, [[maybe_unused]] AssetManager& assetManager) override
    {
        auto asset = std::make_shared<BinaryAsset>(metadata.Guid, metadata.Path, AssetType::NativeSource);
        return AssetParseResult(asset);
    }

    std::string GetName() const override { return "CppSourceAssetParser"; }

    int GetPriority() const override { return 100; }
};

} // namespace GameEngine
