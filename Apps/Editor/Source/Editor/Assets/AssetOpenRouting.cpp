#include "Editor/Assets/AssetOpenRouting.h"

#include "Graph/GraphKindChrome.h"

#include "AssetCore/AssetTypes.h"

#include <string>

namespace GameEngine::Editor
{

AssetOpenTarget ResolveAssetOpenTarget(std::string_view lowercaseExtension,
                                       const AssetOpenRoutingInputs& inputs)
{
    const std::string_view ext = lowercaseExtension;
    if (ext == ".scene")
        return AssetOpenTarget::Scene;

    if (ext == ".cs")
        return inputs.OpenCSharpInScriptEditor ? AssetOpenTarget::ScriptEditor
                                                : AssetOpenTarget::ExternalScriptEditor;

    const AssetType type = GetAssetTypeFromExtension(std::string(ext));
    if (type == AssetType::NativeSource)
        return inputs.OpenNativeSourceInScriptEditor ? AssetOpenTarget::ScriptEditor
                                                      : AssetOpenTarget::ExternalIde;

    if (type == AssetType::Material)
        return AssetOpenTarget::Inspector;

    if (ExtensionOpensInGraphPanel(ext))
    {
        // A hand-written .glsl is text; only a graph-generated one (and only when
        // the user wants it that way) belongs in the Material Graph.
        if (ext == ".glsl" && !inputs.GlslOpensInMaterialGraph)
            return AssetOpenTarget::ScriptEditor;
        return AssetOpenTarget::NodeGraph;
    }

    if (ext == ".timeline")
        return AssetOpenTarget::Timeline;
    if (ext == ".clipset")
        return AssetOpenTarget::ClipEditor;
    if (ext == ".anim" || ext == ".animation" || ext == ".fbx" || ext == ".gltf" || ext == ".glb")
        return AssetOpenTarget::Animation;

    return AssetOpenTarget::OperatingSystem;
}

} // namespace GameEngine::Editor
