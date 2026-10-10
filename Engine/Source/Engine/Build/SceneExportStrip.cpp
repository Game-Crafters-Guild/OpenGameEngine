#include "Engine/Build/SceneExportStrip.h"

#include "Assets/Parsers/SceneAssetParser.h"
#include "ECS/ComponentFieldRegistry.h"
#include "Engine/Build/AssetCollector.h"
#include "Scene/SceneSchemaRegistry.h"

#include <fstream>
#include <functional>
#include <iterator>
#include <string_view>
#include <unordered_map>

namespace GameEngine
{
namespace
{
// A string hash that takes a string_view, so a lookup by a scene line's token allocates nothing.
struct TokenHash
{
    using is_transparent = void;
    size_t operator()(std::string_view text) const { return std::hash<std::string_view>{}(text); }
};

// Whether a scene file's component token names an editor-only component, resolved as the scene
// reader resolves it (SceneSchemaRegistry::Find: a hand-written schema's token, such as
// "Measure" for MeasureComponent, or a reflected component's name) and looked up once per token
// across the build's scenes.
class EditorOnlyComponentTokens
{
  public:
    bool Contains(std::string_view token)
    {
        if (const auto it = m_Known.find(token); it != m_Known.end())
            return it->second;
        return m_Known.emplace(std::string(token), Resolve(token)).first->second;
    }

  private:
    static bool Resolve(std::string_view token)
    {
        const Scene::ISceneComponentSchema* schema = Scene::SceneSchemaRegistry::Find(token);
        if (!schema)
            return false;
        ECS::ComponentTypeId typeId = schema->GetOwnedComponentType();
        if (typeId == 0)
            typeId = ECS::ComponentFieldRegistry::FindByName(schema->GetComponentName());
        return typeId != 0 && ECS::ComponentFieldRegistry::IsComponentEditorOnly(typeId);
    }

    std::unordered_map<std::string, bool, TokenHash, std::equal_to<>> m_Known;
};

bool StripStagedScene(const std::filesystem::path& stagedFile, EditorOnlyComponentTokens& editorOnly,
                      std::string& error)
{
    std::ifstream in(stagedFile, std::ios::binary);
    if (!in.is_open())
    {
        error = "cannot read the staged scene to strip its editor-only components";
        return false;
    }
    const std::string authored((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    const std::string exported = SceneAssetParser::StripEditorOnlyComponents(
        authored, [&editorOnly](std::string_view component) { return editorOnly.Contains(component); });
    if (exported == authored)
        return true;

    std::ofstream out(stagedFile, std::ios::binary | std::ios::trunc);
    out.write(exported.data(), static_cast<std::streamsize>(exported.size()));
    if (!out)
    {
        error = "cannot write the staged scene without its editor-only components";
        return false;
    }
    return true;
}
} // namespace

bool StripEditorOnlyComponentsFromStagedScenes(const std::filesystem::path& contentRoot, const AssetManifest& manifest,
                                               const std::function<bool()>& cancelRequested, std::string& error)
{
    error.clear();
    Scene::EnsureBuiltInSchemasRegistered();
    EditorOnlyComponentTokens editorOnly;
    for (const auto& entry : manifest.entries)
    {
        if (cancelRequested && cancelRequested())
            return false;
        if (entry.type != AssetType::Scene)
            continue;
        std::string reason;
        if (!StripStagedScene(contentRoot / entry.outputPath, editorOnly, reason))
        {
            error = entry.outputPath.generic_string() + ": " + reason;
            return false;
        }
    }
    return true;
}
} // namespace GameEngine
