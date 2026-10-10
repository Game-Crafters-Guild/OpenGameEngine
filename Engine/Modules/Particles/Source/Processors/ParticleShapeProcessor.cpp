#include "Particles/Processors/ParticleShapeProcessor.h"

#include "Mathematics/Interpolation.h"
#include "Particles/ParticleStackDocument.h"
#include "Processors/ParticleBuiltInProcessors.h"
#include "Processors/ParticleProcessorOptions.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

GE_REFLECT(GameEngine::Particles::ParticleShapeParameters, Shape, Radius, InnerRadius, Length, Angle, Extents, Offset,
           Surface, AlignToNormal);

namespace GameEngine::Particles
{
namespace
{
using Mathematics::Vector3;
using Parameters = ParticleShapeParameters;

constexpr float kPi = 3.14159265358979323846f;
constexpr uint32 kShapeStream = 16;
constexpr float kMinimumArea = 1e-10f;

constexpr std::array<ParticleEnumOption, 9> kShapes = {{
    {"sphere", "Sphere", static_cast<uint32>(ParticleShapeKind::Sphere), "particle-choice-shape"},
    {"hemisphere", "Hemisphere", static_cast<uint32>(ParticleShapeKind::Hemisphere), "particle-choice-shape"},
    {"box", "Box", static_cast<uint32>(ParticleShapeKind::Box), "particle-choice-shape"},
    {"circle", "Circle", static_cast<uint32>(ParticleShapeKind::Circle), "particle-choice-shape"},
    {"cone", "Cone", static_cast<uint32>(ParticleShapeKind::Cone), "particle-choice-shape"},
    {"cylinder", "Cylinder", static_cast<uint32>(ParticleShapeKind::Cylinder), "particle-choice-shape"},
    {"line", "Line", static_cast<uint32>(ParticleShapeKind::Line), "particle-choice-shape"},
    {"meshVertices", "Mesh Vertices", static_cast<uint32>(ParticleShapeKind::MeshVertices), "particle-choice-shape"},
    {"meshSurface", "Mesh Surface", static_cast<uint32>(ParticleShapeKind::MeshSurface), "particle-choice-shape"},
}};

ParticleShapeKind ShapeOf(const void* parameters)
{
    return static_cast<const Parameters*>(parameters)->Shape;
}

bool UsesRadius(const void* parameters)
{
    const auto shape = ShapeOf(parameters);
    return shape == ParticleShapeKind::Sphere || shape == ParticleShapeKind::Hemisphere ||
           shape == ParticleShapeKind::Circle || shape == ParticleShapeKind::Cylinder;
}

bool UsesInnerRadius(const void* parameters)
{
    const auto shape = ShapeOf(parameters);
    return shape == ParticleShapeKind::Circle || shape == ParticleShapeKind::Cylinder;
}

bool UsesLength(const void* parameters)
{
    const auto shape = ShapeOf(parameters);
    return shape == ParticleShapeKind::Cone || shape == ParticleShapeKind::Cylinder || shape == ParticleShapeKind::Line;
}

bool UsesAngle(const void* parameters)
{
    return ShapeOf(parameters) == ParticleShapeKind::Cone;
}

bool UsesExtents(const void* parameters)
{
    return ShapeOf(parameters) == ParticleShapeKind::Box;
}

bool HasSurfaceChoice(const void* parameters)
{
    const auto shape = ShapeOf(parameters);
    return shape != ParticleShapeKind::Line && shape != ParticleShapeKind::MeshVertices &&
           shape != ParticleShapeKind::MeshSurface;
}

constexpr ParticleParameterField kFields[] = {
    {.Name = "Shape", .Label = "Shape", .Tooltip = "Where newborn particles are placed", .Kind = ParticleParameterKind::Enum, .Options = kShapes},
    {.Name = "Radius", .Label = "Radius", .Kind = ParticleParameterKind::Float, .Minimum = 0.0f, .Visible = UsesRadius},
    {.Name = "InnerRadius", .Label = "Inner Radius", .Tooltip = "Leaves a hole in the middle, for rings", .Kind = ParticleParameterKind::Float, .Minimum = 0.0f, .Visible = UsesInnerRadius},
    {.Name = "Length", .Label = "Length", .Kind = ParticleParameterKind::Float, .Minimum = 0.0f, .Visible = UsesLength},
    {.Name = "Angle", .Label = "Cone Angle", .Tooltip = "Half angle in degrees", .Kind = ParticleParameterKind::Float, .Minimum = 0.0f, .Maximum = 89.9f, .Visible = UsesAngle},
    {.Name = "Extents", .Label = "Half Extents", .Kind = ParticleParameterKind::Vector3, .Minimum = 0.0f, .ComponentLabels = kParticleVectorLabels, .Visible = UsesExtents},
    {.Name = "Offset", .Label = "Offset", .Tooltip = "Shape center relative to the emitter", .Kind = ParticleParameterKind::Vector3, .ComponentLabels = kParticleVectorLabels},
    {.Name = "Surface", .Label = "Surface Only", .Tooltip = "Emit from the surface instead of the volume", .Kind = ParticleParameterKind::Bool, .Visible = HasSurfaceChoice},
    {.Name = "AlignToNormal", .Label = "Align Velocity to Normal", .Tooltip = "Turn each particle's velocity outward from the shape, keeping its speed", .Kind = ParticleParameterKind::Bool},
};

// The processor's geometry and, for a mesh surface, its cumulative triangle areas, so a sample
// is a binary search instead of a scan over every triangle.
struct CompiledShape
{
    const ParticleProcessorGeometry* Geometry = nullptr;
    std::vector<float> Cumulative;
};

Vector3 TriangleCross(const ParticleProcessorGeometry& geometry, size_t first)
{
    const auto& a = geometry.Vertices[geometry.Indices[first]];
    const auto& b = geometry.Vertices[geometry.Indices[first + 1]];
    const auto& c = geometry.Vertices[geometry.Indices[first + 2]];
    return Vector3::Cross(b - a, c - a);
}

std::shared_ptr<const void> Compile(const ParticleProcessorCompileInput& input)
{
    const auto& parameters = *static_cast<const Parameters*>(input.Parameters);
    auto shape = std::make_shared<CompiledShape>();
    shape->Geometry = input.Geometry;
    if (parameters.Shape == ParticleShapeKind::MeshSurface && input.Geometry)
    {
        const auto& geometry = *input.Geometry;
        shape->Cumulative.reserve(geometry.Indices.size() / 3);
        float total = 0.0f;
        for (size_t first = 0; first + 2 < geometry.Indices.size(); first += 3)
        {
            total += TriangleCross(geometry, first).Length();
            shape->Cumulative.push_back(total);
        }
    }
    return shape;
}

void Validate(const ParticleValidationContext& context)
{
    const auto& parameters = *static_cast<const Parameters*>(context.Parameters);
    const auto* geometry = context.Geometry;
    const bool mesh = parameters.Shape == ParticleShapeKind::MeshVertices || parameters.Shape == ParticleShapeKind::MeshSurface;
    if (mesh && (!geometry || geometry->Vertices.empty()))
        context.Error("A mesh shape needs vertices");
    if (parameters.Shape == ParticleShapeKind::MeshSurface && geometry &&
        (geometry->Indices.empty() || geometry->Indices.size() % 3 != 0))
        context.Error("A mesh surface needs triangle indices");
    if (parameters.InnerRadius > parameters.Radius && UsesInnerRadius(context.Parameters))
        context.Error("The inner radius is larger than the radius");
}

float ShapeRandom(uint32 seed, uint32 particle, uint32 processor, uint32 stream)
{
    return ParticleRandom(seed, particle, processor, kShapeStream + stream);
}

// Radius of a uniform sample of a disc or ring between `inner` and `outer`.
float RingRadius(float inner, float outer, float u)
{
    return std::sqrt(Math::Lerp(inner * inner, outer * outer, u));
}

// A position and outward normal on the shape for one particle, local to the shape. False for an
// empty or degenerate mesh.
bool SampleShape(const Parameters& settings, const CompiledShape& shape, uint32 seed, uint32 particle, uint32 processor,
                 Vector3& position, Vector3& normal)
{
    const float u = ShapeRandom(seed, particle, processor, 0);
    const float v = ShapeRandom(seed, particle, processor, 1);
    const float w = ShapeRandom(seed, particle, processor, 2);
    const float azimuth = 2.0f * kPi * v;
    Vector3 p{};
    Vector3 n{0.0f, 1.0f, 0.0f};
    switch (settings.Shape)
    {
    case ParticleShapeKind::Sphere:
    case ParticleShapeKind::Hemisphere:
    {
        n.y = settings.Shape == ParticleShapeKind::Hemisphere ? u : 2.0f * u - 1.0f;
        const float ring = std::sqrt(std::max(0.0f, 1.0f - n.y * n.y));
        n.x = ring * std::cos(azimuth);
        n.z = ring * std::sin(azimuth);
        p = n * (settings.Radius * (settings.Surface ? 1.0f : std::cbrt(w)));
        break;
    }
    case ParticleShapeKind::Box:
    {
        for (size_t axis = 0; axis < 3; ++axis)
            p[axis] = (2.0f * ShapeRandom(seed, particle, processor, static_cast<uint32>(axis)) - 1.0f) * settings.Extents[axis];
        if (settings.Surface)
        {
            const float areas[3] = {settings.Extents.y * settings.Extents.z, settings.Extents.x * settings.Extents.z, settings.Extents.x * settings.Extents.y};
            float pick = ShapeRandom(seed, particle, processor, 3) * (areas[0] + areas[1] + areas[2]);
            size_t axis = 0;
            while (axis < 2 && pick >= areas[axis])
                pick -= areas[axis++];
            n = {};
            n[axis] = ShapeRandom(seed, particle, processor, 4) < 0.5f ? -1.0f : 1.0f;
            p[axis] = n[axis] * settings.Extents[axis];
        }
        break;
    }
    case ParticleShapeKind::Line:
        p.y = (u - 0.5f) * settings.Length;
        break;
    case ParticleShapeKind::Circle:
    case ParticleShapeKind::Cylinder:
    {
        float radius = settings.Surface ? settings.Radius : RingRadius(std::min(settings.InnerRadius, settings.Radius), settings.Radius, u);
        if (settings.Shape == ParticleShapeKind::Cylinder)
        {
            p.y = (w - 0.5f) * settings.Length;
            n = {std::cos(azimuth), 0.0f, std::sin(azimuth)};
            // A closed cylinder's caps take their share of the surface by area.
            if (settings.Surface && ShapeRandom(seed, particle, processor, 3) * (settings.Length + settings.Radius) < settings.Radius)
            {
                radius = RingRadius(std::min(settings.InnerRadius, settings.Radius), settings.Radius, u);
                p.y = (ShapeRandom(seed, particle, processor, 4) < 0.5f ? -0.5f : 0.5f) * settings.Length;
                n = {0.0f, p.y < 0.0f ? -1.0f : 1.0f, 0.0f};
            }
        }
        p.x = radius * std::cos(azimuth);
        p.z = radius * std::sin(azimuth);
        break;
    }
    case ParticleShapeKind::Cone:
    {
        // Apex at the origin; the side area or the volume is sampled by inverse CDF.
        const float angle = settings.Angle * kPi / 180.0f;
        p.y = settings.Length * (settings.Surface ? std::sqrt(w) : std::cbrt(w));
        const float radius = p.y * std::tan(angle) * (settings.Surface ? 1.0f : std::sqrt(u));
        n = {std::cos(azimuth) * std::cos(angle), -std::sin(angle), std::sin(azimuth) * std::cos(angle)};
        p.x = radius * std::cos(azimuth);
        p.z = radius * std::sin(azimuth);
        break;
    }
    case ParticleShapeKind::MeshVertices:
    {
        if (!shape.Geometry || shape.Geometry->Vertices.empty())
            return false;
        const auto& geometry = *shape.Geometry;
        const size_t index = std::min(static_cast<size_t>(u * geometry.Vertices.size()), geometry.Vertices.size() - 1);
        p = geometry.Vertices[index];
        break;
    }
    case ParticleShapeKind::MeshSurface:
    {
        if (!shape.Geometry || shape.Cumulative.empty())
            return false;
        const auto& geometry = *shape.Geometry;
        const float total = shape.Cumulative.back();
        if (total <= kMinimumArea || !std::isfinite(total))
            return false;
        const auto found = std::upper_bound(shape.Cumulative.begin(), shape.Cumulative.end(), u * total);
        const size_t triangle = std::min(static_cast<size_t>(found - shape.Cumulative.begin()),
                                         shape.Cumulative.size() - 1);
        const size_t first = triangle * 3;
        n = TriangleCross(geometry, first);
        const float area = n.Length();
        if (area <= kMinimumArea)
            return false;
        n = n / area;
        const float root = std::sqrt(v);
        const float weights[3] = {1.0f - root, root * (1.0f - w), root * w};
        for (size_t corner = 0; corner < 3; ++corner)
            p = p + geometry.Vertices[geometry.Indices[first + corner]] * weights[corner];
        break;
    }
    default:
        return false;
    }
    position = p + settings.Offset;
    normal = n;
    return true;
}

void Execute(ParticleProcessorContext& context)
{
    const auto& parameters = context.Params<Parameters>();
    const auto& shape = *static_cast<const CompiledShape*>(context.Compiled);
    auto positions = context.Channels.Positions();
    auto velocities = context.Channels.Velocities();
    auto lifetimes = context.Channels.Lifetimes();
    const auto spawns = context.Channels.SpawnIndices();
    // Shape points are emitter-local. A particle simulated in world space starts at the emitter
    // origin, so only the linear part of the emitter transform applies to the offset.
    const auto& frame = context.Emitter;
    for (const uint32 i : context.Particles)
    {
        Vector3 point, normal;
        if (!SampleShape(parameters, shape, frame.Seed, spawns[i], context.ProcessorId, point, normal))
        {
            lifetimes[i] = 0.0f;
            continue;
        }
        // A 2D emitter keeps its particles on its XY plane.
        if (frame.Planar)
        {
            point.z = 0.0f;
            normal.z = 0.0f;
        }
        positions[i] = positions[i] + (frame.LocalSpace ? point : frame.LocalToWorldVector(point));
        if (parameters.AlignToNormal)
        {
            const Vector3 direction = frame.LocalSpace ? normal : frame.LocalToWorldVector(normal);
            const float length = direction.Length();
            if (length > 1e-6f)
                velocities[i] = direction * (velocities[i].Length() / length);
        }
    }
}
} // namespace

ParticleProcessorDescriptor MakeShapeProcessorDescriptor()
{
    ParticleProcessorDescriptor descriptor;
    descriptor.Id = "shape";
    descriptor.DisplayName = "Shape";
    descriptor.Description = "Places newborn particles on or inside a shape around the emitter";
    descriptor.IconClass = "particle-choice-shape";
    descriptor.Category = "Spawn";
    descriptor.Stages = StageBit(ParticleStage::Birth);
    descriptor.DefaultStage = ParticleStage::Birth;
    descriptor.Reads = ChannelBits(ParticleChannel::Position, ParticleChannel::Velocity, ParticleChannel::SpawnIndex);
    descriptor.Writes = ChannelBits(ParticleChannel::Position, ParticleChannel::Velocity, ParticleChannel::Lifetime);
    descriptor.UsesGeometry = true;
    descriptor.Validate = Validate;
    descriptor.Compile = Compile;
    descriptor.Execute = Execute;
    BindParticleParameters<Parameters>(descriptor, kFields);
    return descriptor;
}

const ParticleProcessorDescriptor& ParticleShapeProcessor()
{
    return FindBuiltInParticleProcessor("shape");
}

} // namespace GameEngine::Particles
