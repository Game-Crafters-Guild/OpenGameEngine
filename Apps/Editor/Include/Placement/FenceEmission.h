#pragma once

#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Placement/FenceLayout.h"
#include "Types/Types.h"

#include <span>
#include <vector>

namespace GameEngine
{
struct Mesh;
}

namespace GameEngine::Editor
{

// Mesh-identity signature space: one base per role, so two roles drawing the
// same pool slot never compare equal and a rebuild that keeps every mesh
// rewrites transforms instead of churning entities. Posts take their slot as it
// is.
inline constexpr uint32 kSignatureSpanBase = 1000u;
inline constexpr uint32 kSignatureCrestBase = 2000u;
inline constexpr uint32 kSignatureCapBase = 3000u;
inline constexpr uint32 kSignatureGateBase = 4000u;
// A piece drawing a cut variant signs with that variant's mesh handle under
// this flag: a rebuild that moves it onto another variant is a new mesh.
inline constexpr uint64 kSignatureMitreVariant = 1ull << 63;

// One active pool entry resolved to what a piece instances: its mesh and
// material, and the bounds the layout measured it by.
struct FencePoolPiece
{
    Components::LocalBounds Bounds{};
    Components::MeshRenderer Renderer{};
    // The CPU mesh the renderer draws, which a mitred piece is cut from.
    // Null when the model holds no such submesh.
    const Mesh* SourceMesh = nullptr;
};

// The resolved pools one fence rebuild draws from, each in active-slot order.
// The bounds are the ones the layout was given, slot for slot.
struct FenceEmissionPools
{
    std::span<const FencePoolPiece> Posts;
    std::span<const FencePoolPiece> Spans;
    std::span<const FencePoolPiece> Crests;
    std::span<const FencePoolPiece> Caps;
    std::span<const FencePoolPiece> Gates;
    std::span<const FencePieceBounds> SpanBounds;
    std::span<const FencePieceBounds> GateBounds;
    std::span<const FencePieceBounds> CrestBounds;
    std::span<const FencePieceBounds> CapBounds;
    // The axis of the footprint the stations were laid out on, which stands in
    // for every member of the post family.
    PieceAxis PostAxis = PieceAxis::Z;
    uint32 Seed = 0;
    // The cut variants mitred spans and registered crests draw (fence design
    // section 4c), and which one each span and crest of the layout draws: an
    // index into Variants, or SplineLayout::kNoMitreVariant for its pool
    // piece. Parallel to the layout's Spans and Crests; empty draws every
    // piece from its pool.
    std::span<const FencePoolPiece> Variants;
    std::span<const uint32> SpanVariants;
    std::span<const uint32> CrestVariants;
};

// What a piece is, independent of where it falls in the emission list: a
// station by its global index, a span by the authored point opening its run
// and its ordinal in that run — a wall and the gate that replaces it are the
// same span — and a crest piece by its cell ordinal, caps apart from crests. A
// rebuild keeps a piece's entity for as long as its identity survives, so a
// picked span stays the entity the author picked when a gate, an opening or a
// changed count elsewhere moves everything after it in the list. Two caps can
// share a cell; pieces of one identity are matched in emission order.
enum class FencePieceRole : uint8
{
    Station,
    Span,
    Crest,
};

struct FencePieceIdentity
{
    FencePieceRole Role = FencePieceRole::Station;
    uint32 Primary = 0;
    uint32 Secondary = 0;

    bool operator==(const FencePieceIdentity&) const = default;
};

// One piece a fence rebuild instances.
struct FenceEmission
{
    // Points into the layout result the list was built from.
    const TilePose* Pose = nullptr;
    // Points into the pool it was drawn from.
    const FencePoolPiece* Source = nullptr;
    // Scale along the piece's own along-path axis.
    float32 LengthScale = 1.0f;
    // The axis the pose was BUILT on, carried through to the transform so the
    // piece is laid along the axis its length was measured on.
    PieceAxis Axis = PieceAxis::Z;
    uint64 Signature = 0;
    const char* RoleLabel = "";
    // Ordinal within the role, for the hierarchy label.
    uint32 LabelIndex = 0;
    // The layout's span a span or gate piece stands for; null for every other
    // role. Points into the layout result, like Pose.
    const FenceSpan* Span = nullptr;
    FencePieceIdentity Identity{};
};

// The pieces of one fence layout in the order BuildFenceLayout spent the piece
// budget in: stations (only when the post pool has members), then spans — a
// gate among them drawn from the gate pool — then the crest row with its caps. A crest or cap carries the LengthScale the
// layout gave it; a span or crest the pools name a variant for draws that
// variant. The result points into `built` and into the pools, so it
// lives no longer than either.
std::vector<FenceEmission> BuildFenceEmissions(const FenceLayoutResult& built,
                                               const FenceEmissionPools& pools);

} // namespace GameEngine::Editor
