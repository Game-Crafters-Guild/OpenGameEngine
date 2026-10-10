// Native C++ port of @dgreenheck/ez-tree procedural generation.
// Portions adapted from EZ-Tree, MIT License.
// Copyright (c) 2024 Daniel Greenheck.

#include "EZTree/EZTreeGenerator.h"

#include "Assets/ModelAsset.h"
#include "EZTree/EZTreeRng.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <glm/ext/quaternion_float.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <limits>

namespace GameEngine::EZTree
{
namespace
{

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;

struct Branch
{
    uint32 id = 0;
    glm::vec3 origin{0.0f};
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
    float length = 0.0f;
    float radius = 0.0f;
    uint32 level = 0;
    uint32 sectionCount = 0;
    uint32 segmentCount = 0;
};

struct Section
{
    glm::vec3 origin{0.0f};
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
};

struct MeshBuilder
{
    Mesh mesh;

    uint32 AddVertex(const glm::vec3& p, const glm::vec3& n, float u, float v)
    {
        Vertex vertex{};
        vertex.Position[0] = p.x;
        vertex.Position[1] = p.y;
        vertex.Position[2] = p.z;
        vertex.Normal[0] = n.x;
        vertex.Normal[1] = n.y;
        vertex.Normal[2] = n.z;
        vertex.TexCoords[0] = u;
        vertex.TexCoords[1] = v;
        mesh.Vertices.push_back(vertex);
        return static_cast<uint32>(mesh.Vertices.size() - 1u);
    }

    void AddTri(uint32 a, uint32 b, uint32 c)
    {
        mesh.Indices.push_back(a);
        mesh.Indices.push_back(b);
        mesh.Indices.push_back(c);
    }
};

float Length(const glm::vec3& v)
{
    return glm::length(v);
}

glm::vec3 NormalizeOr(const glm::vec3& v, const glm::vec3& fallback)
{
    const float len = Length(v);
    if (len <= 1e-6f)
        return fallback;
    return v / len;
}

glm::vec3 ToGlm(const Mathematics::Vector3& v)
{
    return {v.x, v.y, v.z};
}

Mathematics::Vector3 ToVector3(const glm::vec3& v)
{
    return {v.x, v.y, v.z};
}

glm::quat QuatFromUnitVectors(const glm::vec3& from, const glm::vec3& to)
{
    const glm::vec3 f = NormalizeOr(from, {0.0f, 1.0f, 0.0f});
    const glm::vec3 t = NormalizeOr(to, {0.0f, 1.0f, 0.0f});
    const float dot = glm::dot(f, t);
    if (dot > 0.999999f)
        return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (dot < -0.999999f)
    {
        glm::vec3 axis = glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), f);
        if (glm::length(axis) < 1e-6f)
            axis = glm::cross(glm::vec3(0.0f, 0.0f, 1.0f), f);
        return glm::angleAxis(kPi, glm::normalize(axis));
    }
    const glm::vec3 c = glm::cross(f, t);
    return glm::normalize(glm::quat(1.0f + dot, c.x, c.y, c.z));
}

glm::quat RotateTowards(const glm::quat& from, const glm::quat& to, float maxRadians)
{
    if (maxRadians <= 0.0f)
        return from;
    const float cosTheta = std::clamp(std::abs(glm::dot(glm::normalize(from), glm::normalize(to))), 0.0f, 1.0f);
    const float angle = 2.0f * std::acos(cosTheta);
    if (angle <= 1e-6f)
        return to;
    const float t = std::min(1.0f, maxRadians / angle);
    return glm::normalize(glm::slerp(from, to, t));
}

void ComputeBounds(Mesh& mesh)
{
    if (mesh.Vertices.empty())
    {
        mesh.MinBounds[0] = mesh.MinBounds[1] = mesh.MinBounds[2] = 0.0f;
        mesh.MaxBounds[0] = mesh.MaxBounds[1] = mesh.MaxBounds[2] = 0.0f;
        return;
    }

    glm::vec3 mn(std::numeric_limits<float>::max());
    glm::vec3 mx(-std::numeric_limits<float>::max());
    for (const auto& v : mesh.Vertices)
    {
        const glm::vec3 p(v.Position[0], v.Position[1], v.Position[2]);
        mn = glm::min(mn, p);
        mx = glm::max(mx, p);
    }
    mesh.MinBounds[0] = mn.x;
    mesh.MinBounds[1] = mn.y;
    mesh.MinBounds[2] = mn.z;
    mesh.MaxBounds[0] = mx.x;
    mesh.MaxBounds[1] = mx.y;
    mesh.MaxBounds[2] = mx.z;
}

Mathematics::BoundingBox BoundsFromMesh(const Mesh& mesh)
{
    return Mathematics::BoundingBox::FromMinMax(
        {mesh.MinBounds[0], mesh.MinBounds[1], mesh.MinBounds[2]},
        {mesh.MaxBounds[0], mesh.MaxBounds[1], mesh.MaxBounds[2]});
}

const BranchOverride* FindOverride(const TreeOptions& options, uint32 branchId, uint32 level)
{
    for (uint32 i = 0; i < options.branchOverrideCount && i < options.branchOverrides.size(); ++i)
    {
        const BranchOverride& ov = options.branchOverrides[i];
        if (!ov.enabled)
            continue;
        if (ov.branchId == branchId || (ov.branchId == 0 && ov.level == level))
            return &ov;
    }
    return nullptr;
}

std::vector<uint32> ShuffledIndices(Rng& rng, uint32 count)
{
    std::vector<uint32> indices(count);
    for (uint32 i = 0; i < count; ++i)
        indices[i] = i;

    for (uint32 i = count; i > 1u; --i)
    {
        const uint32 j = static_cast<uint32>(std::floor(rng.Random(static_cast<float>(i), 0.0f)));
        std::swap(indices[i - 1u], indices[std::min(j, i - 1u)]);
    }
    return indices;
}

void AppendMesh(Mesh& dst, const Mesh& src)
{
    const uint32 base = static_cast<uint32>(dst.Vertices.size());
    dst.Vertices.insert(dst.Vertices.end(), src.Vertices.begin(), src.Vertices.end());
    dst.Indices.reserve(dst.Indices.size() + src.Indices.size());
    for (uint32 index : src.Indices)
        dst.Indices.push_back(base + index);
}

void AddCylinder(MeshBuilder& out,
                 const glm::vec3& center,
                 const glm::vec3& axis,
                 float length,
                 float radius,
                 uint32 segments)
{
    segments = std::max(3u, segments);
    const glm::vec3 y = NormalizeOr(axis, {0.0f, 1.0f, 0.0f});
    const glm::vec3 helper = std::abs(y.y) < 0.95f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 x = NormalizeOr(glm::cross(helper, y), {1.0f, 0.0f, 0.0f});
    const glm::vec3 z = NormalizeOr(glm::cross(y, x), {0.0f, 0.0f, 1.0f});
    const glm::vec3 a = center - y * (length * 0.5f);
    const glm::vec3 b = center + y * (length * 0.5f);
    const uint32 base = static_cast<uint32>(out.mesh.Vertices.size());

    for (uint32 ring = 0; ring < 2; ++ring)
    {
        for (uint32 i = 0; i <= segments; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(segments);
            const float angle = 2.0f * kPi * t;
            const glm::vec3 n = glm::normalize(x * std::cos(angle) + z * std::sin(angle));
            const glm::vec3 p = (ring == 0 ? a : b) + n * radius;
            out.AddVertex(p, n, t, static_cast<float>(ring));
        }
    }

    const uint32 stride = segments + 1u;
    for (uint32 i = 0; i < segments; ++i)
    {
        const uint32 v1 = base + i;
        const uint32 v2 = base + i + 1u;
        const uint32 v3 = v1 + stride;
        const uint32 v4 = v2 + stride;
        out.AddTri(v1, v3, v2);
        out.AddTri(v2, v3, v4);
    }
}

} // namespace

Mathematics::Vector3 GetNearestTrellisPoint(const TreeOptions& options, const Mathematics::Vector3& position)
{
    const TreeOptions sanitized = SanitizeOptions(options);
    const auto& t = sanitized.trellis;
    const float minX = t.position.x - t.width * 0.5f;
    const float maxX = t.position.x + t.width * 0.5f;
    const float minY = t.position.y;
    const float maxY = t.position.y + t.height;
    const float spacing = std::max(0.001f, t.spacing);

    const float clampedX = std::clamp(position.x, minX, maxX);
    const float clampedY = std::clamp(position.y, minY, maxY);

    const float nearestHLineY = std::round((clampedY - minY) / spacing) * spacing + minY;
    const float finalHLineY = std::clamp(nearestHLineY, minY, maxY);
    const float nearestVLineX = std::round((clampedX - minX) / spacing) * spacing + minX;
    const float finalVLineX = std::clamp(nearestVLineX, minX, maxX);

    const glm::vec3 p(position.x, position.y, position.z);
    const glm::vec3 h(clampedX, finalHLineY, t.position.z);
    const glm::vec3 v(finalVLineX, clampedY, t.position.z);
    return ToVector3(glm::distance(p, h) < glm::distance(p, v) ? h : v);
}

TrellisForceResult CalculateTrellisForce(const TreeOptions& options, const Mathematics::Vector3& position, float radius)
{
    const TreeOptions sanitized = SanitizeOptions(options);
    TrellisForceResult out{};
    if (!sanitized.trellis.enabled)
        return out;

    const Mathematics::Vector3 nearest = GetNearestTrellisPoint(sanitized, position);
    const glm::vec3 p = ToGlm(position);
    const glm::vec3 n = ToGlm(nearest);
    const float distance = glm::distance(p, n);
    if (distance > sanitized.trellis.forceMaxDistance || distance < 0.001f || radius <= 0.001f)
        return out;

    const float distanceFactor = 1.0f - std::pow(distance / sanitized.trellis.forceMaxDistance, sanitized.trellis.forceFalloff);
    out.active = true;
    out.direction = ToVector3(glm::normalize(n - p));
    out.strength = sanitized.trellis.forceStrength * distanceFactor / radius;
    return out;
}

GeneratedTree Generator::Generate(const TreeOptions& rawOptions)
{
    const TreeOptions options = SanitizeOptions(rawOptions);
    GeneratedTree generated{};
    generated.branches.Name = "Tree Generator Branches";
    generated.leaves.Name = "Tree Generator Leaves";
    generated.trellis.Name = "Tree Generator Trellis";
    generated.combined.Name = "Tree Generator";

    MeshBuilder branches;
    MeshBuilder leaves;
    MeshBuilder trellis;
    Rng rng(options.seed);
    std::deque<Branch> queue;
    uint32 nextBranchId = 1;

    queue.push_back(Branch{
        nextBranchId++,
        glm::vec3(0.0f),
        glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
        options.branch.length[0],
        options.branch.radius[0],
        0u,
        std::max(1u, options.branch.sections[0]),
        std::max(3u, options.branch.segments[0]),
    });

    // Each leaf's wind seed, the same at every vertex of its cards (UV1.x, ez_tree_wind.glsl): the
    // golden-ratio sequence over the leaf index spreads consecutive leaves across [0, 1) without
    // drawing from the tree's random sequence, so the tree's shape does not change.
    uint32 leafIndex = 0;
    auto generateLeaf = [&](const glm::vec3& origin, const glm::quat& orientation)
    {
        constexpr float kGoldenRatioFraction = 0.6180339887f;
        const float windSeed = std::fmod(static_cast<float>(leafIndex++) * kGoldenRatioFraction, 1.0f);
        const float leafSize = options.leaves.size *
            (1.0f + rng.Random(options.leaves.sizeVariance, -options.leaves.sizeVariance));
        const float w = std::max(0.001f, leafSize);
        const float l = std::max(0.001f, leafSize);
        const uint32 uvColumns = std::max(1u, options.leaves.textureColumns);
        const uint32 uvRows = std::max(1u, options.leaves.textureRows);
        const uint32 uvCells = std::max(1u, uvColumns * uvRows);
        uint32 uvTile = std::min(options.leaves.textureTile, uvCells - 1u);
        if (options.leaves.randomTextureTile && uvCells > 1u)
            uvTile = std::min(static_cast<uint32>(std::floor(rng.Random(static_cast<float>(uvCells), 0.0f))), uvCells - 1u);
        const uint32 uvX = uvTile % uvColumns;
        const uint32 uvY = uvTile / uvColumns;
        const float u0 = static_cast<float>(uvX) / static_cast<float>(uvColumns);
        const float u1 = static_cast<float>(uvX + 1u) / static_cast<float>(uvColumns);
        const float v0 = static_cast<float>(uvY) / static_cast<float>(uvRows);
        const float v1 = static_cast<float>(uvY + 1u) / static_cast<float>(uvRows);

        auto createLeaf = [&](float rotation)
        {
            const uint32 base = static_cast<uint32>(leaves.mesh.Vertices.size());
            const glm::quat qRot = glm::angleAxis(rotation, glm::vec3(0.0f, 1.0f, 0.0f));
            const glm::quat q = orientation * qRot;
            const glm::vec3 local[4] = {
                {-w * 0.5f, l, 0.0f},
                {-w * 0.5f, 0.0f, 0.0f},
                { w * 0.5f, 0.0f, 0.0f},
                { w * 0.5f, l, 0.0f},
            };
            const float uv[4][2] = {{u0, v1}, {u0, v0}, {u1, v0}, {u1, v1}};
            const glm::vec3 faceNormal = NormalizeOr(q * glm::vec3(0.0f, 0.0f, 1.0f), {0.0f, 0.0f, 1.0f});
            for (uint32 i = 0; i < 4; ++i)
            {
                const glm::vec3 p = origin + q * local[i];
                const glm::vec3 n = options.leaves.roundedNormals
                    ? NormalizeOr(faceNormal + p - origin, faceNormal)
                    : faceNormal;
                leaves.AddVertex(p, n, uv[i][0], uv[i][1]);
                leaves.mesh.TexCoords1.push_back(windSeed);
                leaves.mesh.TexCoords1.push_back(0.0f);
            }
            leaves.AddTri(base, base + 1u, base + 2u);
            leaves.AddTri(base, base + 2u, base + 3u);
        };

        createLeaf(0.0f);
        if (options.leaves.billboard == BillboardMode::Double)
            createLeaf(kPi * 0.5f);
    };

    while (!queue.empty())
    {
        Branch branch = queue.front();
        queue.pop_front();
        ++generated.stats.generatedBranchCount;

        if (const BranchOverride* ov = FindOverride(options, branch.id, branch.level))
        {
            branch.length *= ov->lengthScale;
            branch.radius *= ov->radiusScale;
            branch.orientation = branch.orientation * glm::angleAxis(ov->angleOffsetDegrees * kDegToRad, glm::vec3(1.0f, 0.0f, 0.0f));
            branch.orientation = branch.orientation * glm::angleAxis(ov->twistOffset, glm::vec3(0.0f, 1.0f, 0.0f));
        }

        const uint32 indexOffset = static_cast<uint32>(branches.mesh.Vertices.size());
        glm::quat sectionOrientation = branch.orientation;
        glm::vec3 sectionOrigin = branch.origin;
        const float divisor = options.type == TreeType::Deciduous
            ? std::max(1.0f, static_cast<float>(std::max(1u, options.branch.levels) - 1u))
            : 1.0f;
        const float sectionLength = branch.length / static_cast<float>(std::max(1u, branch.sectionCount)) / divisor;
        std::vector<Section> sections;
        sections.reserve(branch.sectionCount + 1u);
        const uint32 wrapsX = std::max(1u, static_cast<uint32>(std::round(branch.radius * options.bark.textureScale.x)));

        for (uint32 i = 0; i <= branch.sectionCount; ++i)
        {
            float sectionRadius = branch.radius;
            if (i == branch.sectionCount && branch.level == options.branch.levels)
                sectionRadius = 0.001f;
            else if (options.type == TreeType::Deciduous)
                sectionRadius *= 1.0f - options.branch.taper[branch.level] * (static_cast<float>(i) / static_cast<float>(branch.sectionCount));
            else
                sectionRadius *= 1.0f - (static_cast<float>(i) / static_cast<float>(branch.sectionCount));
            sectionRadius = std::max(0.001f, sectionRadius);

            uint32 first = 0;
            for (uint32 j = 0; j < branch.segmentCount; ++j)
            {
                const float t = static_cast<float>(j) / static_cast<float>(branch.segmentCount);
                const float angle = 2.0f * kPi * t;
                const glm::vec3 radial(std::cos(angle), 0.0f, std::sin(angle));
                const glm::vec3 p = sectionOrigin + sectionOrientation * (radial * sectionRadius);
                const glm::vec3 n = NormalizeOr(sectionOrientation * radial, {0.0f, 1.0f, 0.0f});
                const uint32 idx = branches.AddVertex(p, n, t * static_cast<float>(wrapsX), (i % 2u) == 0u ? 0.0f : 1.0f);
                if (j == 0)
                    first = idx;
            }

            const Vertex& firstVertex = branches.mesh.Vertices[first];
            branches.mesh.Vertices.push_back(firstVertex);
            branches.mesh.Vertices.back().TexCoords[0] = static_cast<float>(wrapsX);

            sections.push_back({sectionOrigin, sectionOrientation, sectionRadius});
            sectionOrigin += sectionOrientation * glm::vec3(0.0f, sectionLength, 0.0f);

            const float gnarliness = std::max(1.0f, 1.0f / std::sqrt(sectionRadius)) * options.branch.gnarliness[branch.level];
            glm::vec3 euler = glm::eulerAngles(sectionOrientation);
            euler.x += rng.Random(gnarliness, -gnarliness);
            euler.z += rng.Random(gnarliness, -gnarliness);
            sectionOrientation = glm::quat(euler);

            glm::quat qSection = sectionOrientation;
            qSection = qSection * glm::angleAxis(options.branch.twist[branch.level], glm::vec3(0.0f, 1.0f, 0.0f));
            const glm::vec3 sectionUp = NormalizeOr(qSection * glm::vec3(0.0f, 1.0f, 0.0f), {0.0f, 1.0f, 0.0f});
            const glm::vec3 forceTarget = NormalizeOr(ToGlm(options.branch.forceDirection), {0.0f, 1.0f, 0.0f});
            glm::vec3 forceAxis = glm::cross(sectionUp, forceTarget);
            const float sinFull = glm::length(forceAxis);
            if (sinFull > 1e-6f)
            {
                forceAxis /= sinFull;
                const float fullAngle = std::atan2(sinFull, glm::dot(sectionUp, forceTarget));
                const float step = options.branch.forceStrength / sectionRadius;
                const float clampedStep = std::clamp(step, -fullAngle, fullAngle);
                qSection = glm::normalize(glm::angleAxis(clampedStep, forceAxis) * qSection);
            }

            if (options.trellis.enabled)
            {
                const auto trellisForce = CalculateTrellisForce(options, ToVector3(sectionOrigin), sectionRadius);
                if (trellisForce.active)
                {
                    const glm::quat qTrellis = QuatFromUnitVectors({0.0f, 1.0f, 0.0f}, ToGlm(trellisForce.direction));
                    qSection = RotateTowards(qSection, qTrellis, trellisForce.strength);
                }
            }
            sectionOrientation = glm::normalize(qSection);
        }

        const uint32 stride = branch.segmentCount + 1u;
        for (uint32 i = 0; i < branch.sectionCount; ++i)
        {
            for (uint32 j = 0; j < branch.segmentCount; ++j)
            {
                const uint32 v1 = indexOffset + i * stride + j;
                const uint32 v2 = indexOffset + i * stride + (j + 1u);
                const uint32 v3 = v1 + stride;
                const uint32 v4 = v2 + stride;
                branches.AddTri(v1, v3, v2);
                branches.AddTri(v2, v3, v4);
            }
        }

        if (options.type == TreeType::Deciduous && branch.level < options.branch.levels && !sections.empty())
        {
            const Section& last = sections.back();
            queue.push_back(Branch{
                nextBranchId++,
                last.origin,
                last.orientation,
                options.branch.length[branch.level + 1u],
                last.radius,
                branch.level + 1u,
                branch.sectionCount,
                branch.segmentCount,
            });
        }
        else if (options.type == TreeType::Deciduous && branch.level >= options.branch.levels && !sections.empty())
        {
            const Section& last = sections.back();
            generateLeaf(last.origin, last.orientation);
        }

        if (branch.level == options.branch.levels)
        {
            const uint32 leafCount = options.leaves.count;
            const float radialOffset = rng.Random();
            const float startMin = options.leaves.start;
            const float heightStep = leafCount > 0u ? (1.0f - startMin) / static_cast<float>(leafCount) : 0.0f;
            const std::vector<uint32> angleSlots = ShuffledIndices(rng, leafCount);
            for (uint32 i = 0; i < leafCount && sections.size() >= 2u; ++i)
            {
                const float leafStart = startMin + (static_cast<float>(i) + rng.Random()) * heightStep;
                const uint32 sectionIndex = std::min<uint32>(
                    static_cast<uint32>(std::floor(leafStart * static_cast<float>(sections.size() - 1u))),
                    static_cast<uint32>(sections.size() - 1u));
                const Section& a = sections[sectionIndex];
                const Section& b = sections[std::min<size_t>(sectionIndex + 1u, sections.size() - 1u)];
                const float denom = 1.0f / static_cast<float>(sections.size() - 1u);
                const float alpha = (leafStart - static_cast<float>(sectionIndex) / static_cast<float>(sections.size() - 1u)) / denom;
                const glm::vec3 origin = glm::mix(a.origin, b.origin, alpha);
                const glm::quat parent = glm::slerp(b.orientation, a.orientation, alpha);
                const float radialJitter = rng.Random(0.5f, -0.5f);
                const float radialAngle = 2.0f * kPi *
                    (radialOffset + (static_cast<float>(angleSlots[i]) + radialJitter) / static_cast<float>(leafCount));
                const glm::quat q1 = glm::angleAxis(options.leaves.angle * kDegToRad, glm::vec3(1.0f, 0.0f, 0.0f));
                const glm::quat q2 = glm::angleAxis(radialAngle, glm::vec3(0.0f, 1.0f, 0.0f));
                generateLeaf(origin, parent * (q2 * q1));
            }
        }
        else if (branch.level < options.branch.levels && sections.size() >= 2u)
        {
            const uint32 childCount = options.branch.children[branch.level];
            const float radialOffset = rng.Random();
            const uint32 level = branch.level + 1u;
            const float startMin = options.branch.start[level];
            const float heightStep = childCount > 0u ? (1.0f - startMin) / static_cast<float>(childCount) : 0.0f;
            const std::vector<uint32> angleSlots = ShuffledIndices(rng, childCount);
            for (uint32 i = 0; i < childCount; ++i)
            {
                const float childStart = startMin + (static_cast<float>(i) + rng.Random()) * heightStep;
                const uint32 sectionIndex = std::min<uint32>(
                    static_cast<uint32>(std::floor(childStart * static_cast<float>(sections.size() - 1u))),
                    static_cast<uint32>(sections.size() - 1u));
                const Section& a = sections[sectionIndex];
                const Section& b = sections[std::min<size_t>(sectionIndex + 1u, sections.size() - 1u)];
                const float denom = 1.0f / static_cast<float>(sections.size() - 1u);
                const float alpha = (childStart - static_cast<float>(sectionIndex) / static_cast<float>(sections.size() - 1u)) / denom;
                const glm::vec3 origin = glm::mix(a.origin, b.origin, alpha);
                const float radius = options.branch.radius[level] * ((1.0f - alpha) * a.radius + alpha * b.radius);
                const glm::quat parent = glm::slerp(b.orientation, a.orientation, alpha);
                const float radialJitter = rng.Random(0.5f, -0.5f);
                const float radialAngle = 2.0f * kPi *
                    (radialOffset + (static_cast<float>(angleSlots[i]) + radialJitter) / static_cast<float>(childCount));
                const glm::quat q1 = glm::angleAxis(options.branch.angle[level] * kDegToRad, glm::vec3(1.0f, 0.0f, 0.0f));
                const glm::quat q2 = glm::angleAxis(radialAngle, glm::vec3(0.0f, 1.0f, 0.0f));
                const float length = options.branch.length[level] *
                    (options.type == TreeType::Evergreen ? 1.0f - childStart : 1.0f);
                queue.push_back(Branch{
                    nextBranchId++,
                    origin,
                    parent * (q2 * q1),
                    length,
                    radius,
                    level,
                    std::max(1u, options.branch.sections[level]),
                    std::max(3u, options.branch.segments[level]),
                });
            }
        }
    }

    if (options.trellis.enabled && options.trellis.visible)
    {
        const auto& t = options.trellis;
        const float spacing = std::max(0.001f, t.spacing);
        const uint32 hLineCount = static_cast<uint32>(std::floor(t.height / spacing)) + 1u;
        for (uint32 i = 0; i < hLineCount; ++i)
        {
            AddCylinder(trellis,
                        {t.position.x, t.position.y + static_cast<float>(i) * spacing, t.position.z},
                        {1.0f, 0.0f, 0.0f},
                        t.width,
                        std::max(0.001f, t.cylinderRadius),
                        8u);
        }

        const uint32 vLineCount = static_cast<uint32>(std::floor(t.width / spacing)) + 1u;
        for (uint32 i = 0; i < vLineCount; ++i)
        {
            const float x = -t.width * 0.5f + static_cast<float>(i) * spacing;
            AddCylinder(trellis,
                        {t.position.x + x, t.position.y + t.height * 0.5f, t.position.z},
                        {0.0f, 1.0f, 0.0f},
                        t.height,
                        std::max(0.001f, t.cylinderRadius),
                        8u);
        }
    }

    generated.branches = std::move(branches.mesh);
    generated.leaves = std::move(leaves.mesh);
    generated.trellis = std::move(trellis.mesh);
    generated.stats.branchVertices = static_cast<uint32>(generated.branches.Vertices.size());
    generated.stats.branchIndices = static_cast<uint32>(generated.branches.Indices.size());
    generated.stats.leafVertices = static_cast<uint32>(generated.leaves.Vertices.size());
    generated.stats.leafIndices = static_cast<uint32>(generated.leaves.Indices.size());
    generated.stats.trellisVertices = static_cast<uint32>(generated.trellis.Vertices.size());
    generated.stats.trellisIndices = static_cast<uint32>(generated.trellis.Indices.size());

    ComputeBounds(generated.branches);
    ComputeBounds(generated.leaves);
    ComputeBounds(generated.trellis);
    AppendMesh(generated.combined, generated.branches);
    AppendMesh(generated.combined, generated.leaves);
    AppendMesh(generated.combined, generated.trellis);
    ComputeBounds(generated.combined);
    generated.bounds = BoundsFromMesh(generated.combined);
    generated.combined.MaterialIndex = 0;
    return generated;
}

} // namespace GameEngine::EZTree
