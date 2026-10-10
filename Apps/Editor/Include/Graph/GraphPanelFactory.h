#pragma once

#include "AssetCore/GUID.h"
#include "EditorPanelIds.h"
#include "Graph/GraphModel.h"

#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace GameEngine {

class GraphPanel;

/** Constructs ShaderGraphPanel, AnimationGraphPanel, or GameLogicGraphPanel.
 *  Empty kindId constructs ShaderGraphPanel. Unknown non-empty ids return
 *  nullptr — they must not silently become Game Logic. */
std::unique_ptr<GraphPanel> CreateGraphPanelForKind(std::string_view kindId);

/** Dock tab that hosts `kindId`. NodeGraph is the shader-graph tab id so saved
 *  layouts keep working. Nullptr when the kind has no panel class. */
inline const char* GraphDockPanelIdForKind(std::string_view kindId)
{
    if (kindId == Graph::kKindIdMaterial)
        return EditorPanelIds::NodeGraph;
    if (kindId == Graph::kKindIdAnimation)
        return EditorPanelIds::AnimationGraph;
    if (kindId == Graph::kKindIdGameLogic)
        return EditorPanelIds::GameLogicGraph;
    return nullptr;
}

inline bool IsKnownGraphKindId(std::string_view kindId)
{
    return GraphDockPanelIdForKind(kindId) != nullptr;
}

/** Per-asset dock tab id. A null GUID returns the kind template id so the
 *  layout's Shader/Animation/Game Logic tabs stay the scratch panels.
 *  A second file of the same kind gets `{template}:{compactGuid}`. */
inline std::string MakeGraphDockIdForAsset(std::string_view kindId, const GUID& guid)
{
    const char* prefix = GraphDockPanelIdForKind(kindId);
    if (!prefix)
        return {};
    if (guid.IsNull())
        return std::string(prefix);

    const std::string compact = guid.ToCompactString();
    std::string id;
    id.reserve(std::strlen(prefix) + 1 + compact.size());
    id.append(prefix);
    id.push_back(':');
    id.append(compact);
    return id;
}

} // namespace GameEngine
