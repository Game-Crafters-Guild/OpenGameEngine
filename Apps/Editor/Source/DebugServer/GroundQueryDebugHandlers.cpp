#include "DebugServer/GroundQueryDebugHandlers.h"

#include "DebugServer/DebugServerReply.h"
#include "DebugServer/EditorDebugServer.h"
#include "Components/Name.h"
#include "Core/Engine.h"
#include "ECS/World.h"
#include "Placement/PlacementGroundGap.h"
#include "TerrainECS/PlanarHeightQuery.h"
#include "TerrainECS/TerrainGpuBake.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

namespace GameEngine
{

using json = nlohmann::json;

namespace
{

// One request samples a route, not a scene: a few thousand stations is the whole
// of a long approach road at 0.5 m spacing, and the cap keeps a typo from asking
// the editor to bilinear-sample a million points on the main thread.
constexpr std::size_t kMaxGroundSamples = 8192u;

// The composed-ground description shared by every handler in this file. Reported
// alongside the samples so a reading carries the terrain it was taken against:
// two runs that disagree are usually two different terrains, not two heights.
json DescribeSurface(const TerrainECS::PlanarHeightQuery& query)
{
    // Every footprint number comes from the query's own accessors, which report
    // the extent it SAMPLES over. Reading the fields directly would mix a
    // component-derived origin with a residency-derived lattice on a tiled
    // terrain, where the two disagree. The lattice origin is the footprint
    // origin, so it is reported once.
    return json{
        {"kind", query.Tiled ? "tiled" : "single"},
        {"originX", query.FootprintOriginX()},
        {"originZ", query.FootprintOriginZ()},
        {"originY", query.OriginY},
        {"sizeX", query.FootprintSizeX()},
        {"sizeZ", query.FootprintSizeZ()},
        {"heightScale", query.HeightScale},
        {"latticeSpacingX", query.LatticeSpacingX()},
        {"latticeSpacingZ", query.LatticeSpacingZ()},
        // A tiled terrain whose modifiers are all GPU-bakeable leaves the CPU
        // heightfield stale between bake and settle readback, so the same XZ can
        // read differently from what is drawn. Corridors, splines, stamps and
        // paint layers are never GPU-bakeable and force the whole bake back to
        // the CPU, which is why a corridored route reads authoritatively here —
        // but the caller is told the gate is armed rather than left to assume.
        {"gpuHeightBake", TerrainECS::IsGpuHeightBakeEnabled()}};
}

// A piece with no gap numbers says which cause left it that way, because the
// fixes differ: wait out a streaming tile, move a piece off the terrain edge,
// and stop asking a wall how far above the ground its underside floats.
const char* DescribeGapStatus(Editor::GroundGapStatus status)
{
    switch (status)
    {
    case Editor::GroundGapStatus::Measured:
        return "measured";
    case Editor::GroundGapStatus::NoGroundSampled:
        return "noGroundSampled";
    case Editor::GroundGapStatus::UndersideNearVertical:
        return "undersideNearVertical";
    case Editor::GroundGapStatus::DegenerateFootprint:
        return "degenerateFootprint";
    }
    return "unknown";
}

// "No planar terrain in this scene" is an answer, not a failure: the surface is
// absent on a planet, before provisioning, and in an empty scene, and a caller
// that cannot tell those from a malformed request will read zero as ground.
json UnresolvedSurface()
{
    return json{{"resolved", false},
                {"note", "no enabled planar terrain with resident heightfield data — a "
                         "spherical terrain, an unprovisioned one, or an empty scene"}};
}

} // namespace

void RegisterGroundQueryDebugHandlers(EditorDebugServer& server)
{
    // The composed ground the renderer and the conform placement actually stand
    // on: the CPU heightfield AFTER the modifier stack — flattens, corridors,
    // stamps and noise — not the base heightmap an offline model reads. That
    // difference is the whole point of the method: a scene-authored corridor
    // exists only here.
    //
    // Terrain only, by construction. This reads the heightfield directly rather
    // than tracing the scene, so it never reports a mesh, a prop or a bridge
    // deck as ground; a caller that wants "what would I land on" wants a conform
    // ray instead.
    server.RegisterHandler("sample_ground_height",
                           [](const EditorDebugServer::RequestContext& ctx) -> json
    {
        if (!ctx.params.contains("points") || !ctx.params["points"].is_array())
            return Editor::RefuseRequest("Missing points parameter — expected [[x, z], ...]");

        const auto& points = ctx.params["points"];
        if (points.size() > kMaxGroundSamples)
            return Editor::RefuseRequest("Too many points: " + std::to_string(points.size()) +
                                             " exceeds the " + std::to_string(kMaxGroundSamples) +
                                             " sample cap");

        // The whole request is validated BEFORE any scene state is read, so
        // "malformed" and "no surface here" cannot swap places with the scene: a
        // shape check inside the sampling loop answers [[1, 2, 3]] with the
        // terrain's absence in an empty scene and with the real complaint in a
        // terrain scene, which is the one thing a caller cannot debug.
        for (const auto& point : points)
        {
            if (!point.is_array() || point.size() != 2u || !point[0].is_number() ||
                !point[1].is_number())
                return Editor::RefuseRequest("Each point must be [x, z] — two numbers");
        }

        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return Editor::RefuseRequest("No world available");

        const TerrainECS::PlanarHeightQuery query =
            TerrainECS::ResolvePlanarHeightQuery(*world);
        if (!query.IsValid())
            return UnresolvedSurface();

        json samples = json::array();
        for (const auto& point : points)
        {
            const auto x = point[0].get<float32>();
            const auto z = point[1].get<float32>();

            // Outside the footprint there is no ground and never will be;
            // inside it but unsampleable means the tile has not streamed in.
            // Collapsing those into one "false" is how a caller ends up bedding
            // geometry into a streaming hole, so they stay separate.
            const bool inside = query.ContainsXZ(x, z);
            float32 y = 0.0f;
            const bool sampled = query.SampleHeight(x, z, y);

            json sample{{"x", x}, {"z", z}, {"inside", inside}, {"sampled", sampled}};
            if (sampled)
                sample["y"] = y;
            samples.push_back(std::move(sample));
        }

        return json{{"resolved", true},
                    {"surface", DescribeSurface(query)},
                    {"samples", std::move(samples)}};
    });

    // The signed ground-to-underside gap for every piece a spline route placed:
    // positive floating, negative buried. This is the quantity a conforming
    // placement actually fails on. Station tilt is not — flattening the ground
    // under a slab lowers its tilt and opens its gap at the same time, so tilt
    // improves as the defect worsens.
    server.RegisterHandler("get_placement_ground_gap",
                           [](const EditorDebugServer::RequestContext& ctx) -> json
    {
        // Validated before any state is read, for the reason the sampling handler
        // above states: a malformed request must not be answered with the scene's
        // condition.
        if (!ctx.params.contains("entityId"))
            return Editor::RefuseRequest("Missing entityId parameter — the spline route entity");
        if (!ctx.params["entityId"].is_number_unsigned())
            return Editor::RefuseRequest("entityId must be an unsigned number — the spline route entity");

        auto* world = EngineCore::GetInstance().GetPrimaryWorld();
        if (!world)
            return Editor::RefuseRequest("No world available");

        const ECS::EntityHandle route(ctx.params["entityId"].get<uint32_t>());
        if (!world->IsValid(route))
            return Editor::RefuseRequest("Invalid entity");

        const TerrainECS::PlanarHeightQuery query =
            TerrainECS::ResolvePlanarHeightQuery(*world);
        if (!query.IsValid())
            return UnresolvedSurface();

        std::vector<Editor::PieceGroundGap> gaps;
        Editor::MeasureRouteGroundGaps(*world, route, query, gaps);

        json pieces = json::array();
        float32 worstFloating = std::numeric_limits<float32>::lowest();
        float32 worstBuried = std::numeric_limits<float32>::max();
        uint32_t worstFloatingPiece = 0u;
        uint32_t worstBuriedPiece = 0u;
        uint32 measured = 0u;
        for (const Editor::PieceGroundGap& gap : gaps)
        {
            json piece{
                {"entityId", gap.Piece.id},
                {"status", DescribeGapStatus(gap.Status)},
                {"undersideCenterY", gap.UndersideCenterY},
                {"probesSampled", gap.ProbesSampled},
                {"probesRequested", gap.ProbesRequested},
                {"fullySampled", gap.FullySampled()}};

            // A display label, renumbered whenever spacing changes — readable,
            // never an identity. Match pieces by entityId.
            if (const auto* name = world->GetComponent<Components::Name>(gap.Piece))
                piece["name"] = std::string(name->View());

            // Per-probe detail is the point of probing five places: a summary
            // cannot say WHICH corner is buried, and on a tilted piece the answer
            // is not the one a box around it would nominate.
            if (gap.ProbesRequested > 0u)
            {
                json probes = json::array();
                for (const Editor::GroundGapProbe& probe : gap.Probes)
                {
                    json entry{{"x", probe.X},
                               {"z", probe.Z},
                               {"undersideY", probe.UndersideY},
                               {"sampled", probe.Sampled}};
                    if (probe.Sampled)
                    {
                        entry["groundY"] = probe.GroundY;
                        entry["gap"] = probe.Gap;
                    }
                    probes.push_back(std::move(entry));
                }
                piece["probes"] = std::move(probes);
            }

            if (gap.Status == Editor::GroundGapStatus::Measured)
            {
                piece["minGap"] = gap.MinGap;
                piece["maxGap"] = gap.MaxGap;
                piece["centerGap"] = gap.CenterGap;
                if (gap.MaxGap > worstFloating)
                {
                    worstFloating = gap.MaxGap;
                    worstFloatingPiece = gap.Piece.id;
                }
                if (gap.MinGap < worstBuried)
                {
                    worstBuried = gap.MinGap;
                    worstBuriedPiece = gap.Piece.id;
                }
                ++measured;
            }
            pieces.push_back(std::move(piece));
        }

        json summary{{"pieceCount", static_cast<uint32_t>(gaps.size())},
                     {"measuredCount", measured}};
        if (measured > 0u)
        {
            summary["worstFloating"] = worstFloating;
            summary["worstFloatingEntityId"] = worstFloatingPiece;
            summary["worstBuried"] = worstBuried;
            summary["worstBuriedEntityId"] = worstBuriedPiece;
        }

        return json{{"resolved", true},
                    {"surface", DescribeSurface(query)},
                    {"summary", std::move(summary)},
                    {"pieces", std::move(pieces)}};
    });
}

} // namespace GameEngine
