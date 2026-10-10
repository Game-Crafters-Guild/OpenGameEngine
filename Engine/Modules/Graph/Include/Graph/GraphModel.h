#pragma once

#include "Graph/GraphValue.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine {
namespace Graph {

inline constexpr int kModelFormatVersion = 2;

inline constexpr std::string_view kKindIdGameLogic = "game_logic";
inline constexpr std::string_view kKindIdMaterial = "material";
inline constexpr std::string_view kKindIdAnimation = "animation";

/**
 * Builtin palettes. Additional graph kinds Register() a string id; they do
 * not add enumerators. Model.KindId stores the registry id.
 */
enum class Kind {
    GameLogic,
    Material
};

/**
 * Port direction: input (receives data/flow) or output (sends data/flow).
 */
enum class PortDirection {
    In,
    Out
};

struct Port {
    std::string Id;
    PortDirection Direction = PortDirection::In;
    std::string DataType;
    std::string DisplayName;
    GraphObject Passthrough;
};

struct Node {
    std::string Id;
    std::string TypeId;
    float PositionX = 0.f;
    float PositionY = 0.f;
    std::vector<Port> Ports;
    GraphObject Parameters;
    GraphObject Extensions;
    GraphObject Passthrough;

    /** Write `key` from text without changing an existing value's Kind.
     *  Missing keys become String. Returns false if the current kind rejects `text`. */
    bool AssignParameterFromText(const std::string& key, std::string_view text);
};

struct Edge {
    std::string Id;
    std::string SourceNodeId;
    std::string SourcePortId;
    std::string TargetNodeId;
    std::string TargetPortId;
    GraphObject Passthrough;
};

struct Texture {
    std::string Name;
    std::string Guid;
    std::string Hint;
};

struct Variable {
    std::string Name;
    std::string Type = "float";
    std::string Value = "0";
    bool IsPublic = false;
    bool IsGlobal = false;
    std::uint64_t CreatedOrder = 0;
    std::string RangeMin;
    std::string RangeMax;
    std::string Hint;
};

struct Viewport {
    float PanX = 0.f;
    float PanY = 0.f;
    float Zoom = 1.f;
};

/**
 * In-memory graph model: nodes, edges, viewport.
 * Serializes to/from JSON for .graph assets.
 */
struct Model {
    int Version = kModelFormatVersion;
    std::string KindId = std::string(kKindIdGameLogic);
    std::vector<Node> Nodes;
    std::vector<Edge> Links;
    std::vector<Variable> Variables;
    std::vector<Texture> Textures;
    std::string LightingModel;
    /** Shader variant defines (`@sg-variant`) the graph opts into. */
    std::vector<std::string> VariantDefines;
    /** Verbatim `@sg-*` tag lines this build does not recognize, re-emitted on
        save so a newer tool's data survives a round trip. */
    std::vector<std::string> UnknownSgTags;
    Graph::Viewport Viewport;
    GraphObject Extensions;
    GraphObject Passthrough;

    /** Generate a new unique node id (e.g. "node_0", "node_1"). */
    std::string GenerateNodeId() const;
    /** Generate a new unique edge id (e.g. "link_0"). */
    std::string GenerateLinkId() const;
    /** Find node by id; returns nullptr if not found. */
    Node* FindNode(const std::string& nodeId);
    const Node* FindNode(const std::string& nodeId) const;
    /** Find edge by id; returns nullptr if not found. */
    Edge* FindLink(const std::string& linkId);
    /** Validate: no duplicate links, valid node/port references. Returns true if valid. */
    bool Validate() const;
};

/** Serialize Graph::Model to JSON string. */
std::string ToJson(const Model& model);
/** Deserialize Graph::Model from JSON string. Returns true on success. */
bool FromJson(const std::string& json, Model& outModel);

} // namespace Graph
} // namespace GameEngine
