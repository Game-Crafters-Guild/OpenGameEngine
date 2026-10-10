#pragma once

#include "Graph/GraphModel.h"
#include "Graph/NodeParamOption.h"
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace GameEngine {

/**
 * Metadata for a single port template (used when creating a node from a type).
 */
struct NodePortTemplate {
    std::string Id;
    Graph::PortDirection Direction = Graph::PortDirection::In;
    std::string DataType;
    std::string DisplayName;
    /* Editing range for the port's unconnected value; vector ports clamp per component. */
    std::optional<float> Min;
    std::optional<float> Max;
};

struct NodeParamSpec {
    std::string Id;
    Graph::GraphValue Default;
    /** Non-empty = enum: every editor surface offers exactly these choices.
        The wire format stays the option's Value string. */
    Graph::NodeParamOptions Options;
    /* Editing range; vector params clamp per component. */
    std::optional<float> Min;
    std::optional<float> Max;
};

/** Resolved [Min, Max] editing range for one node value key. */
struct NodeValueRange {
    float Min = 0.0f;
    float Max = 0.0f;
};

/**
 * Metadata for a node type: name, category, ports, and parameter schema.
 */
struct NodeTypeMeta {
    std::string TypeId;
    std::string DisplayName;
    std::string Category;
    std::vector<NodePortTemplate> Ports;
    std::vector<NodeParamSpec> Parameters;
    /* Icon filename stem under editor:Icons/GraphNodes/ (empty = no icon).
       Each kind attaches its own stems at registration; the registry itself
       knows no kind-specific mapping. */
    std::string IconStem;
};

/**
 * Range for an editable node value: the parameter schema entry wins, else the
 * input port of the same id. Every float edit surface (node rows, inspector)
 * clamps through this so a value can never commit out of range.
 */
std::optional<NodeValueRange> FindNodeValueRange(const NodeTypeMeta* meta, std::string_view id);

/**
 * Registry of node types per graph kind. Used to create new nodes and build context menus.
 */
class GraphNodeRegistry {
public:
    static GraphNodeRegistry& Get();

    void Register(std::string_view kindId, NodeTypeMeta meta);
    /** Category glyph stems per kind, keyed by the first path segment of the category. */
    void RegisterCategoryIcon(std::string_view kindId, std::string_view category, std::string_view stem);
    std::string CategoryIconStem(std::string_view kindId, std::string_view category) const;
    const NodeTypeMeta* Find(std::string_view kindId, const std::string& typeId) const;
    /** Resolves editor/shader-graph aliases (e.g. Output → SurfaceOutput) for material. */
    const NodeTypeMeta* FindNodeMeta(std::string_view kindId, const std::string& typeId) const;
    /** Fills empty node port lists from the registry so the canvas can draw ports and wires. */
    void HydrateGraphModelPorts(Graph::Model& model) const;
    std::vector<std::string> GetCategories(std::string_view kindId) const;
    std::vector<NodeTypeMeta> GetTypesInCategory(std::string_view kindId, const std::string& category) const;
    std::vector<NodeTypeMeta> GetAllTypes(std::string_view kindId) const;
    std::unordered_map<std::string, std::string> GetDefaultParameters(std::string_view kindId,
                                                                      const std::string& typeId) const;
    std::unordered_map<std::string, Graph::GraphValue> GetDefaultParameterValues(
        std::string_view kindId, const std::string& typeId) const;
    void ApplyDefaultParameters(std::string_view kindId, const std::string& typeId,
                                Graph::GraphObject& parameters) const;

    /** Create a new Graph::Node from a type (id/position set by caller). */
    Graph::Node CreateNode(std::string_view kindId, const std::string& typeId, const std::string& nodeId,
                        float positionX, float positionY) const;

    /** Loads shader-graph node definitions into the material palette (retries if startup reflection failed). */
    void EnsureMaterialShaderGraphTypesRegistered();

    /** Decoration a kind applies to every type registered under it, before the
        type is stored — icon stems, known parameter defaults, editing ranges.
        The registry does not know what any of those mean; it only knows to ask.
        Registered by the kind's own catalogue. */
    using MetaDecorator = std::function<void(NodeTypeMeta&)>;
    void SetMetaDecorator(std::string_view kindId, MetaDecorator decorator);

    /** A kind's answer for a type id it does not store under that exact name --
        an alias, or an older spelling. Returns an empty string when it has no
        other name to offer. Lets a lookup miss be resolved by the kind that
        owns the vocabulary rather than by the registry. */
    using TypeAliasResolver = std::function<std::string(const std::string& typeId)>;
    void SetTypeAliasResolver(std::string_view kindId, TypeAliasResolver resolver);

private:
    GraphNodeRegistry();
    void RegisterGameLogicTypes();
    void RegisterMaterialConstantsAndOrganization();
    void RegisterMaterialTypesFromShaderGraph();
    void RegisterMaterialTypes();
    void RegisterAnimationTypes();

    std::unordered_map<std::string, MetaDecorator> m_MetaDecorators;
    std::unordered_map<std::string, TypeAliasResolver> m_TypeAliasResolvers;

    std::unordered_map<std::string, std::unordered_map<std::string, NodeTypeMeta>> m_ByKindId;
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> m_CategoryIconStems;
};

} // namespace GameEngine
