#nullable enable

using System.Collections.Immutable;
using System.Numerics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Xunit;

namespace GameEngine.SourceGenerators.Tests;

public class EntitySystemGeneratorTests
{
    /// <summary>
    /// Stub source that provides the minimal types the generator resolves by metadata name:
    /// IEntitySystem, IComponent, WithoutAttribute,
    /// GameSystemRunner, Ecs, ChunkQueryNative, WorldHandle.
    /// These are not functional — just enough to satisfy Roslyn symbol resolution.
    /// </summary>
    private const string StubSource = """
        using System;
        using System.Runtime.InteropServices;

        namespace GameEngine.Scripting
        {
            public interface IEntitySystem
            {
                int Order => 0;
                bool Enabled => true;
            }

            public interface IComponent { }

            [AttributeUsage(AttributeTargets.Struct, AllowMultiple = false, Inherited = false)]
            public sealed class BuiltInComponentAttribute : Attribute
            {
                public string NativeName { get; }
                public BuiltInComponentAttribute(string nativeName)
                {
                    NativeName = nativeName;
                }
            }

            [AttributeUsage(AttributeTargets.Struct, AllowMultiple = false, Inherited = false)]
            public sealed class WithoutAttribute : Attribute
            {
                public Type[] ExcludedTypes { get; }
                public WithoutAttribute(params Type[] excludedTypes)
                {
                    ExcludedTypes = excludedTypes ?? Array.Empty<Type>();
                }
            }

            public readonly struct ReadOnlyOptional<T> where T : unmanaged
            {
                private readonly T _value;
                private readonly bool _hasValue;
                public bool HasValue => _hasValue;
                public T Value => _hasValue ? _value : throw new InvalidOperationException();
                public T GetValueOrDefault() => _value;
                public T GetValueOrDefault(T defaultValue) => _hasValue ? _value : defaultValue;
                internal ReadOnlyOptional(T value) { _value = value; _hasValue = true; }
                public static ReadOnlyOptional<T> None => default;
            }

            [AttributeUsage(AttributeTargets.Struct | AttributeTargets.Class, AllowMultiple = true)]
            public sealed class AfterAttribute : Attribute
            {
                public Type SystemType { get; }
                public AfterAttribute(Type systemType) { SystemType = systemType; }
            }

            [AttributeUsage(AttributeTargets.Struct | AttributeTargets.Class, AllowMultiple = true)]
            public sealed class BeforeAttribute : Attribute
            {
                public Type SystemType { get; }
                public BeforeAttribute(Type systemType) { SystemType = systemType; }
            }
        }

        namespace GameEngine.Scripting
        {
            public abstract class GameSystem
            {
                public virtual int Order => 0;
                public virtual void OnUpdate(float deltaTime) { }
            }
        }

        namespace GameEngine.Scripting.Runtime
        {
            public static class GameSystemRunner
            {
                public static void RegisterEntitySystem(string name, int order,
                    Action<ulong, float> execute, Action destroy,
                    string[]? runAfter = null, string[]? runBefore = null) { }
                public static void RegisterGameSystem(string name,
                    Func<GameEngine.Scripting.GameSystem> factory, int order,
                    string[]? runAfter = null, string[]? runBefore = null) { }
            }
        }

        namespace GameEngine.ECS
        {
            public static class Ecs
            {
                public static ulong RegisterBlobComponent(string name, uint sizeBytes) => 0;
                public static ulong GetComponentTypeIdByName(string nativeName) => 0;
                public static ulong RegisterComponentSchema(string name, uint sizeBytes, ComponentFieldDesc[] fields) => 0;
            }

            public enum ComponentFieldType : ushort
            {
                Unknown = 0, Bool = 1, Int8 = 2, Int16 = 3, Int32 = 4, Int64 = 5,
                UInt8 = 6, UInt16 = 7, UInt32 = 8, UInt64 = 9, Float = 10, Double = 11,
                Vec2 = 12, Vec3 = 13, Vec4 = 14, Quat = 15, Mat4 = 16, Color = 17,
                AssetGuid = 18, EntityHandle = 19, String = 20, Bytes = 21,
            }

            public readonly struct ComponentFieldDesc
            {
                public readonly string Name;
                public readonly uint Offset;
                public readonly uint Size;
                public readonly ComponentFieldType Type;
                public ComponentFieldDesc(string name, uint offset, uint size, ComponentFieldType type)
                {
                    Name = name; Offset = offset; Size = size; Type = type;
                }
            }
        }

        namespace GameEngine.ECS.Internal
        {
            public static class ChunkQueryNative
            {
                public static ulong CreateCachedQuery(ulong worldHandle,
                    ReadOnlySpan<ulong> required, ReadOnlySpan<ulong> excluded) => 0;
                public static void DestroyCachedQuery(ulong queryHandle) { }
                public static void ResetQuery(ulong queryHandle, out int archetypeCount)
                    { archetypeCount = 0; }
                public static void GetArchetypeInfo(ulong queryHandle, int archetypeIndex,
                    out int entityCount, out int chunkCount)
                    { entityCount = 0; chunkCount = 0; }
                public static Span<T> GetChunkSpan<T>(ulong queryHandle,
                    int archetypeIndex, int chunkIndex, ulong componentTypeId) where T : unmanaged
                    => Span<T>.Empty;
                public static ReadOnlySpan<T> GetChunkReadOnlySpan<T>(ulong queryHandle,
                    int archetypeIndex, int chunkIndex, ulong componentTypeId) where T : unmanaged
                    => ReadOnlySpan<T>.Empty;
                public static ReadOnlySpan<uint> GetChunkEntityIds(ulong queryHandle,
                    int archetypeIndex, int chunkIndex)
                    => ReadOnlySpan<uint>.Empty;
                public static bool ArchetypeHasComponent(ulong queryHandle,
                    int archetypeIndex, ulong componentTypeId) => false;
                public static unsafe uint CreateEntityRaw(ulong worldHandle) => 0;
                public static unsafe void DeferCommand(ulong worldHandle, uint commandType,
                    uint entity, ulong componentTypeId, void* data, uint dataLen) { }
                public static Span<T> GetComponentSlice<T>(ulong queryHandle,
                    int archetypeIndex, ulong componentTypeId, int entityOffset) where T : unmanaged
                    => Span<T>.Empty;
                public static ReadOnlySpan<T> GetComponentSliceReadOnly<T>(ulong queryHandle,
                    int archetypeIndex, ulong componentTypeId, int entityOffset) where T : unmanaged
                    => ReadOnlySpan<T>.Empty;
                public static ReadOnlySpan<uint> GetEntityIdSlice(ulong queryHandle,
                    int archetypeIndex, int entityOffset, int maxCount)
                    => ReadOnlySpan<uint>.Empty;
            }
        }

        namespace GameEngine.ECS
        {
            public readonly struct ComponentType<T> where T : unmanaged
            {
                public static ulong CachedId => 0;
            }
        }

        namespace GameEngine.Scripting
        {
            public unsafe struct EntityCommands
            {
                internal ulong WorldHandle;
                public uint CreateEntity() => 0;
                public void DestroyEntity(uint entityId) { }
                public void AddComponent<T>(uint entityId, in T component) where T : unmanaged, IComponent { }
                public void RemoveComponent<T>(uint entityId) where T : unmanaged, IComponent { }
                public void SetComponent<T>(uint entityId, in T component) where T : unmanaged, IComponent { }
            }
        }
        """;

    /// <summary>
    /// Creates a Roslyn compilation containing the stub types and the supplied user source.
    /// </summary>
    private static CSharpCompilation CreateCompilation(string userSource)
    {
        var syntaxTrees = new[]
        {
            CSharpSyntaxTree.ParseText(StubSource, path: "Stubs.cs"),
            CSharpSyntaxTree.ParseText(userSource, path: "UserCode.cs"),
        };

        // Gather the minimal set of runtime references needed for compilation.
        var references = new List<MetadataReference>();

        // System.Runtime (for Object, Attribute, Span<T>, etc.)
        var trustedAssemblies = ((string?)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES") ?? "")
            .Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries);

        var neededAssemblies = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        {
            "System.Runtime",
            "System.Runtime.InteropServices",
            "netstandard",
            "System.Private.CoreLib",
            "System.Memory",
            "System.Numerics.Vectors", // schema tests use System.Numerics.Vector2/3/4 + Quaternion
        };

        foreach (var path in trustedAssemblies)
        {
            var fileName = Path.GetFileNameWithoutExtension(path);
            if (neededAssemblies.Contains(fileName))
                references.Add(MetadataReference.CreateFromFile(path));
        }

        return CSharpCompilation.Create(
            assemblyName: "TestAssembly",
            syntaxTrees: syntaxTrees,
            references: references,
            options: new CSharpCompilationOptions(
                OutputKind.DynamicallyLinkedLibrary,
                allowUnsafe: true));
    }

    /// <summary>
    /// Runs the EntitySystemGenerator against the given user source and returns the
    /// output compilation, generated syntax trees, and diagnostics.
    /// </summary>
    private static (Compilation OutputCompilation, ImmutableArray<SyntaxTree> GeneratedTrees,
        ImmutableArray<Diagnostic> Diagnostics)
        RunGenerator(string userSource)
    {
        var compilation = CreateCompilation(userSource);
        var generator = new EntitySystemGenerator();
        GeneratorDriver driver = CSharpGeneratorDriver.Create(generator);
        driver = driver.RunGeneratorsAndUpdateCompilation(
            compilation, out var outputCompilation, out var diagnostics);

        // The per-assembly component schema hub (__ComponentSchemas.g.cs) is emitted for
        // any compilation containing IComponent structs. It is asserted by the dedicated
        // schema tests (via RunGeneratorSchemaHub); the system/game-system tests here
        // assert the per-system trees, so filter the hub out of their view.
        var generatedTrees = outputCompilation.SyntaxTrees
            .Except(compilation.SyntaxTrees)
            .Where(t => !t.FilePath.EndsWith("__ComponentSchemas.g.cs", StringComparison.Ordinal))
            .ToImmutableArray();

        return (outputCompilation, generatedTrees, diagnostics);
    }

    /// <summary>
    /// Runs the generator and returns the component schema hub's source text (empty
    /// string when no hub was emitted) plus the full compilation and diagnostics.
    /// </summary>
    private static (Compilation OutputCompilation, string HubText,
        ImmutableArray<Diagnostic> Diagnostics)
        RunGeneratorSchemaHub(string userSource)
    {
        var compilation = CreateCompilation(userSource);
        var generator = new EntitySystemGenerator();
        GeneratorDriver driver = CSharpGeneratorDriver.Create(generator);
        driver = driver.RunGeneratorsAndUpdateCompilation(
            compilation, out var outputCompilation, out var diagnostics);

        var hub = outputCompilation.SyntaxTrees
            .Except(compilation.SyntaxTrees)
            .FirstOrDefault(t => t.FilePath.EndsWith("__ComponentSchemas.g.cs", StringComparison.Ordinal));

        return (outputCompilation, hub?.GetText().ToString() ?? string.Empty, diagnostics);
    }

    /// <summary>
    /// Helper: asserts a specific diagnostic ID is present with the expected severity.
    /// </summary>
    private static void AssertDiagnostic(
        ImmutableArray<Diagnostic> diagnostics, string id, DiagnosticSeverity severity)
    {
        var match = diagnostics.FirstOrDefault(d => d.Id == id);
        Assert.NotNull(match);
        Assert.Equal(severity, match!.Severity);
    }

    /// <summary>
    /// Helper: asserts no diagnostic with the given ID is present.
    /// </summary>
    private static void AssertNoDiagnostic(ImmutableArray<Diagnostic> diagnostics, string id)
    {
        var match = diagnostics.FirstOrDefault(d => d.Id == id);
        Assert.Null(match);
    }

    // ------------------------------------------------------------------
    // 1. SingleComponentSystem
    // ------------------------------------------------------------------
    [Fact]
    public void SingleComponentSystem_GeneratesRegisterExecuteDestroy()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent
                {
                    public float Value;
                }

                public partial struct HealthDecaySystem : IEntitySystem
                {
                    public void Execute(ref Health health, float deltaTime)
                    {
                        health.Value -= 1.0f * deltaTime;
                    }
                }
            }
            """;

        var (outputCompilation, generatedTrees, diagnostics) = RunGenerator(source);

        // Should produce exactly one generated file.
        Assert.Single(generatedTrees);

        var generatedText = generatedTrees[0].GetText().ToString();

        // Verify key generated members exist.
        Assert.Contains("__RegisterSystem", generatedText);
        Assert.Contains("__Execute", generatedText);
        Assert.Contains("__Destroy", generatedText);

        // Verify it registers the component type.
        Assert.Contains("Ecs.RegisterBlobComponent(", generatedText);
        Assert.Contains("\"TestGame.Health\"", generatedText);

        // Verify it calls GetChunkSpan (mutable, because ref).
        Assert.Contains("GetChunkSpan<TestGame.Health>", generatedText);

        // Verify it calls GameSystemRunner.RegisterEntitySystem.
        Assert.Contains("GameSystemRunner.RegisterEntitySystem(", generatedText);

        // Should have partial struct in correct namespace.
        Assert.Contains("namespace TestGame", generatedText);
        Assert.Contains("partial struct HealthDecaySystem", generatedText);

        // No errors from the generator.
        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));
    }

    // ------------------------------------------------------------------
    // 2. MultipleComponents — ref and in produce different spans
    // ------------------------------------------------------------------
    [Fact]
    public void MultipleComponents_ProducesCorrectSpanTypes()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Position : IComponent { public float X, Y, Z; }

                [StructLayout(LayoutKind.Sequential)]
                public struct Velocity : IComponent { public float X, Y, Z; }

                public partial struct MovementSystem : IEntitySystem
                {
                    public void Execute(ref Position pos, in Velocity vel, float deltaTime)
                    {
                        pos.X += vel.X * deltaTime;
                    }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        Assert.Single(generatedTrees);
        var text = generatedTrees[0].GetText().ToString();

        // Position is ref → mutable GetChunkSpan.
        Assert.Contains("GetChunkSpan<TestGame.Position>", text);
        // Velocity is in → GetChunkReadOnlySpan.
        Assert.Contains("GetChunkReadOnlySpan<TestGame.Velocity>", text);

        // Execute call uses ref for Position and in for Velocity.
        Assert.Contains("ref __span_TestGame_Position[__i]", text);
        Assert.Contains("in __span_TestGame_Velocity[__i]", text);

        // Both type IDs are registered.
        Assert.Contains("__typeId_TestGame_Position", text);
        Assert.Contains("__typeId_TestGame_Velocity", text);

        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));
    }

    // ------------------------------------------------------------------
    // 3. WithEntityId — includes GetEntityIdSlice in generated code
    // ------------------------------------------------------------------
    [Fact]
    public void WithEntityId_IncludesGetEntityIdSlice()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                public partial struct EntityIdSystem : IEntitySystem
                {
                    public void Execute(ref Health health, uint entityId, float deltaTime)
                    {
                    }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        Assert.Single(generatedTrees);
        var text = generatedTrees[0].GetText().ToString();

        // Should call GetChunkEntityIds.
        Assert.Contains("GetChunkEntityIds", text);
        // Should pass __entityIds[__i] to the Execute call.
        Assert.Contains("__entityIds[__i]", text);

        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));
    }

    // ------------------------------------------------------------------
    // 4. WithExcludedComponents — [Without] includes excluded type IDs
    // ------------------------------------------------------------------
    [Fact]
    public void WithExcludedComponents_IncludesExcludedTypeIds()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Position : IComponent { public float X, Y, Z; }

                [StructLayout(LayoutKind.Sequential)]
                public struct Static : IComponent { public byte Marker; }

                [Without(typeof(Static))]
                public partial struct MoveOnlyDynamic : IEntitySystem
                {
                    public void Execute(ref Position pos, float deltaTime)
                    {
                    }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        Assert.Single(generatedTrees);
        var text = generatedTrees[0].GetText().ToString();

        // Should register the excluded component type.
        Assert.Contains("__typeId_Excluded_TestGame_Static", text);
        // Should include excluded IDs in query creation.
        Assert.Contains("__exc", text);
        Assert.Contains("CreateCachedQuery(worldHandle, __req, __exc)", text);

        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));
    }

    // ------------------------------------------------------------------
    // 5. DiagnosticGE0001 — non-partial struct produces error
    // ------------------------------------------------------------------
    [Fact]
    public void DiagnosticGE0001_NotPartial()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                public struct NotPartialSystem : IEntitySystem
                {
                    public void Execute(ref Health health, float deltaTime) { }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        AssertDiagnostic(diagnostics, "GE0001", DiagnosticSeverity.Error);
        // No source should be generated when there's an error.
        Assert.Empty(generatedTrees);
    }

    // ------------------------------------------------------------------
    // 6. DiagnosticGE0002 — no Execute method
    // ------------------------------------------------------------------
    [Fact]
    public void DiagnosticGE0002_NoExecute()
    {
        const string source = """
            using GameEngine.Scripting;

            namespace TestGame
            {
                public partial struct EmptySystem : IEntitySystem
                {
                    // No Execute method at all.
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        AssertDiagnostic(diagnostics, "GE0002", DiagnosticSeverity.Error);
        Assert.Empty(generatedTrees);
    }

    // ------------------------------------------------------------------
    // 7. DiagnosticGE0012 — component passed by value
    // ------------------------------------------------------------------
    [Fact]
    public void DiagnosticGE0012_ComponentByValue()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                public partial struct BadSystem : IEntitySystem
                {
                    public void Execute(Health health, float deltaTime) { }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        AssertDiagnostic(diagnostics, "GE0012", DiagnosticSeverity.Error);
        Assert.Empty(generatedTrees);
    }

    // ------------------------------------------------------------------
    // 8. DiagnosticGE0008 — multiple Execute overloads
    // ------------------------------------------------------------------
    [Fact]
    public void DiagnosticGE0008_MultipleExecute()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                [StructLayout(LayoutKind.Sequential)]
                public struct Position : IComponent { public float X, Y, Z; }

                public partial struct OverloadedSystem : IEntitySystem
                {
                    public void Execute(ref Health health, float deltaTime) { }
                    public void Execute(ref Position pos, float deltaTime) { }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        AssertDiagnostic(diagnostics, "GE0008", DiagnosticSeverity.Error);
        Assert.Empty(generatedTrees);
    }

    // ------------------------------------------------------------------
    // 9. NamespaceCollisionHandled — same short name, different namespaces
    // ------------------------------------------------------------------
    [Fact]
    public void NamespaceCollisionHandled_ProducesUniqueFieldNames()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace Physics
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Position : IComponent { public float X, Y, Z; }
            }

            namespace Graphics
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Position : IComponent { public float X, Y, Z; }
            }

            namespace TestGame
            {
                public partial struct DualPositionSystem : IEntitySystem
                {
                    public void Execute(ref Physics.Position physPos, ref Graphics.Position gfxPos, float deltaTime)
                    {
                    }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        Assert.Single(generatedTrees);
        var text = generatedTrees[0].GetText().ToString();

        // Should have distinct mangled field names for each namespace.
        Assert.Contains("__typeId_Physics_Position", text);
        Assert.Contains("__typeId_Graphics_Position", text);

        // Both must appear as separate registrations.
        Assert.Contains("\"Physics.Position\"", text);
        Assert.Contains("\"Graphics.Position\"", text);

        // Both spans should be generated with distinct names.
        Assert.Contains("__span_Physics_Position", text);
        Assert.Contains("__span_Graphics_Position", text);

        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));
    }

    // ------------------------------------------------------------------
    // 10. AllReadOnlyComponents — only `in` params, count reads from ReadOnlySpan
    // ------------------------------------------------------------------
    [Fact]
    public void AllReadOnlyComponents_UsesReadOnlySpanForCount()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Position : IComponent { public float X, Y, Z; }

                [StructLayout(LayoutKind.Sequential)]
                public struct Velocity : IComponent { public float X, Y, Z; }

                public partial struct ReadOnlySystem : IEntitySystem
                {
                    public void Execute(in Position pos, in Velocity vel, float deltaTime)
                    {
                    }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        Assert.Single(generatedTrees);
        var text = generatedTrees[0].GetText().ToString();

        // Both components should use GetChunkReadOnlySpan (not mutable GetChunkSpan).
        Assert.Contains("GetChunkReadOnlySpan<TestGame.Position>", text);
        Assert.Contains("GetChunkReadOnlySpan<TestGame.Velocity>", text);
        Assert.DoesNotContain("GetChunkSpan<TestGame.Position>", text);
        Assert.DoesNotContain("GetChunkSpan<TestGame.Velocity>", text);

        // __count reads from a ReadOnlySpan which has .Length.
        Assert.Contains("__count = __span_TestGame_Position.Length;", text);

        // Execute call uses `in` for both.
        Assert.Contains("in __span_TestGame_Position[__i]", text);
        Assert.Contains("in __span_TestGame_Velocity[__i]", text);

        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));
    }

    // ------------------------------------------------------------------
    // 11. CustomOrder — Order property passes through to RegisterEntitySystem
    // ------------------------------------------------------------------
    [Fact]
    public void CustomOrder_PassesOrderToRegister()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                public partial struct OrderedSystem : IEntitySystem
                {
                    public int Order => 5;

                    public void Execute(ref Health health, float deltaTime)
                    {
                        health.Value -= deltaTime;
                    }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        Assert.Single(generatedTrees);
        var text = generatedTrees[0].GetText().ToString();

        // RegisterEntitySystem should receive 5 as the order argument.
        // The generated pattern is: "FullName",\n            5,
        Assert.Contains("5,", text);

        // Verify it's specifically in the RegisterEntitySystem call context.
        Assert.Contains("GameSystemRunner.RegisterEntitySystem(", text);

        // Double-check: should NOT contain "0," as the order line.
        // The order line comes right after the system name line.
        var lines = text.Split('\n');
        bool foundRegisterCall = false;
        for (int i = 0; i < lines.Length; i++)
        {
            if (lines[i].Contains("GameSystemRunner.RegisterEntitySystem("))
            {
                foundRegisterCall = true;
                // Next line is the name, line after is the order.
                Assert.Contains("5", lines[i + 2]);
                break;
            }
        }
        Assert.True(foundRegisterCall, "Should have found RegisterEntitySystem call");

        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));
    }

    // ------------------------------------------------------------------
    // 12. GlobalNamespaceSystem — no namespace block in generated code
    // ------------------------------------------------------------------
    [Fact]
    public void GlobalNamespaceSystem_NoNamespaceBlock()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            [StructLayout(LayoutKind.Sequential)]
            public struct Health : IComponent { public float Value; }

            public partial struct GlobalSystem : IEntitySystem
            {
                public void Execute(ref Health health, float deltaTime)
                {
                    health.Value -= deltaTime;
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        Assert.Single(generatedTrees);
        var text = generatedTrees[0].GetText().ToString();

        // Should NOT contain a namespace block.
        Assert.DoesNotContain("namespace ", text.Split('\n')
            .Where(l => !l.TrimStart().StartsWith("using "))
            .Aggregate("", (acc, l) => acc + l + "\n"));

        // Should still have the partial struct declaration at top level.
        Assert.Contains("partial struct GlobalSystem", text);
        Assert.Contains("__RegisterSystem", text);
        Assert.Contains("__Execute", text);
        Assert.Contains("__Destroy", text);

        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));
    }

    // ------------------------------------------------------------------
    // 13. DiagnosticGE0006 — Execute with only deltaTime, no component params
    // ------------------------------------------------------------------
    [Fact]
    public void DiagnosticGE0006_NoComponentParams()
    {
        const string source = """
            using GameEngine.Scripting;

            namespace TestGame
            {
                public partial struct NoComponentSystem : IEntitySystem
                {
                    public void Execute(float deltaTime) { }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        AssertDiagnostic(diagnostics, "GE0006", DiagnosticSeverity.Error);
        Assert.Empty(generatedTrees);
    }

    // ------------------------------------------------------------------
    // 14. DiagnosticGE0010 — static Execute method
    // ------------------------------------------------------------------
    [Fact]
    public void DiagnosticGE0010_StaticExecute()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                public partial struct StaticExecuteSystem : IEntitySystem
                {
                    public static void Execute(ref Health health, float deltaTime) { }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        AssertDiagnostic(diagnostics, "GE0010", DiagnosticSeverity.Error);
        Assert.Empty(generatedTrees);
    }

    // ------------------------------------------------------------------
    // 15. DiagnosticGE0013 — nested struct
    // ------------------------------------------------------------------
    [Fact]
    public void DiagnosticGE0013_NestedStruct()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                public class Outer
                {
                    public partial struct NestedSystem : IEntitySystem
                    {
                        public void Execute(ref Health health, float deltaTime) { }
                    }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        AssertDiagnostic(diagnostics, "GE0013", DiagnosticSeverity.Error);
        Assert.Empty(generatedTrees);
    }

    // ------------------------------------------------------------------
    // 16. GeneratedCodeCompiles — generated output has zero compilation errors
    // ------------------------------------------------------------------
    [Fact]
    public void GeneratedCodeCompiles_NoCompilationErrors()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Position : IComponent { public float X, Y, Z; }

                [StructLayout(LayoutKind.Sequential)]
                public struct Velocity : IComponent { public float X, Y, Z; }

                public partial struct PhysicsSystem : IEntitySystem
                {
                    public void Execute(ref Position pos, in Velocity vel, float deltaTime)
                    {
                        pos.X += vel.X * deltaTime;
                        pos.Y += vel.Y * deltaTime;
                        pos.Z += vel.Z * deltaTime;
                    }
                }
            }
            """;

        var (outputCompilation, generatedTrees, diagnostics) = RunGenerator(source);

        // Generator should not have reported errors.
        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));

        // Should have generated code.
        Assert.Single(generatedTrees);

        // The output compilation (user source + stubs + generated code) should compile
        // without any errors.
        var compilationErrors = outputCompilation.GetDiagnostics()
            .Where(d => d.Severity == DiagnosticSeverity.Error)
            .ToList();

        Assert.Empty(compilationErrors);
    }

    // ==================================================================
    // GameSystem source generation tests
    // ==================================================================

    // ------------------------------------------------------------------
    // 17. GameSystem_GeneratesRegistration
    // ------------------------------------------------------------------
    [Fact]
    public void GameSystem_GeneratesRegistration()
    {
        const string source = """
            using GameEngine.Scripting;

            namespace MyGame
            {
                public partial class WaveSpawner : GameSystem
                {
                    public override void OnUpdate(float deltaTime) { }
                }
            }
            """;

        var (outputCompilation, generatedTrees, diagnostics) = RunGenerator(source);

        // Should produce exactly one generated file for the GameSystem.
        Assert.Single(generatedTrees);

        var generatedText = generatedTrees[0].GetText().ToString();

        // Verify key generated members exist.
        Assert.Contains("__RegisterGameSystem", generatedText);
        Assert.Contains("RegisterGameSystem(", generatedText);
        Assert.Contains("\"MyGame.WaveSpawner\"", generatedText);
        Assert.Contains("new MyGame.WaveSpawner()", generatedText);

        // Should have partial class in correct namespace.
        Assert.Contains("namespace MyGame", generatedText);
        Assert.Contains("partial class WaveSpawner", generatedText);

        // Default order is 0.
        Assert.Contains("0)", generatedText);

        // No errors from the generator.
        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));

        // Generated code should compile without errors.
        var compilationErrors = outputCompilation.GetDiagnostics()
            .Where(d => d.Severity == DiagnosticSeverity.Error)
            .ToList();
        Assert.Empty(compilationErrors);
    }

    // ------------------------------------------------------------------
    // 18. GameSystem_CustomOrder
    // ------------------------------------------------------------------
    [Fact]
    public void GameSystem_CustomOrder()
    {
        const string source = """
            using GameEngine.Scripting;

            namespace MyGame
            {
                public partial class EarlySystem : GameSystem
                {
                    public override int Order => 5;
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        Assert.Single(generatedTrees);
        var text = generatedTrees[0].GetText().ToString();

        // Should pass 5 as the order argument.
        Assert.Contains("5)", text);
        Assert.Contains("RegisterGameSystem(", text);

        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));
    }

    // ------------------------------------------------------------------
    // 19. GameSystem_NotPartial_ProducesWarning
    // ------------------------------------------------------------------
    [Fact]
    public void GameSystem_NotPartial_ProducesWarning()
    {
        const string source = """
            using GameEngine.Scripting;

            namespace MyGame
            {
                public class NotPartialSystem : GameSystem
                {
                    public override void OnUpdate(float deltaTime) { }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        // Warning, not error: the system still runs through reflection discovery, it just
        // carries no generated registration (and therefore no [After]/[Before] ordering).
        AssertDiagnostic(diagnostics, "GE0017", DiagnosticSeverity.Warning);
        Assert.Empty(generatedTrees);
    }

    // ------------------------------------------------------------------
    // 20. GameSystem does not interfere with IEntitySystem generation
    // ------------------------------------------------------------------
    [Fact]
    public void GameSystem_DoesNotInterfereWithEntitySystem()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                public partial struct HealthSystem : IEntitySystem
                {
                    public void Execute(ref Health health, float deltaTime) { }
                }

                public partial class MyGameSystem : GameSystem
                {
                    public override void OnUpdate(float deltaTime) { }
                }
            }
            """;

        var (outputCompilation, generatedTrees, diagnostics) = RunGenerator(source);

        // Should produce TWO generated files: one for IEntitySystem, one for GameSystem.
        Assert.Equal(2, generatedTrees.Length);

        var allText = string.Join("\n", generatedTrees.Select(t => t.GetText().ToString()));

        // Verify IEntitySystem generation.
        Assert.Contains("partial struct HealthSystem", allText);
        Assert.Contains("__RegisterSystem", allText);
        Assert.Contains("__Execute", allText);

        // Verify GameSystem generation.
        Assert.Contains("partial class MyGameSystem", allText);
        Assert.Contains("__RegisterGameSystem", allText);
        Assert.Contains("RegisterGameSystem(", allText);

        // No errors.
        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));

        // Everything compiles.
        var compilationErrors = outputCompilation.GetDiagnostics()
            .Where(d => d.Severity == DiagnosticSeverity.Error)
            .ToList();
        Assert.Empty(compilationErrors);
    }

    [Fact]
    public void GameSystem_Nested_ProducesGE0019()
    {
        var source = StubSource + @"
class Outer
{
    public partial class InnerSystem : GameEngine.Scripting.GameSystem
    {
        public override void OnUpdate(float deltaTime) { }
    }
}
";
        var compilation = CreateCompilation(source);
        var generator = new EntitySystemGenerator();
        GeneratorDriver driver = CSharpGeneratorDriver.Create(generator);
        driver = driver.RunGeneratorsAndUpdateCompilation(
            compilation, out _, out var diagnostics);

        // Warning, not error: a nested subclass cannot receive a file-scope partial
        // registration, but reflection discovery still finds and runs it.
        Assert.Contains(diagnostics,
            d => d.Id == "GE0019" && d.Severity == DiagnosticSeverity.Warning);
    }

    // ==================================================================
    // Dependency attribute tests ([After] / [Before])
    // ==================================================================

    // ------------------------------------------------------------------
    // 22. DependencyAfter_PassesRunAfterToRegister
    // ------------------------------------------------------------------
    [Fact]
    public void DependencyAfter_PassesRunAfterToRegister()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                public partial struct PhysicsSystem : IEntitySystem
                {
                    public void Execute(ref Health health, float deltaTime) { }
                }

                [After(typeof(PhysicsSystem))]
                public partial struct RenderSystem : IEntitySystem
                {
                    public void Execute(ref Health health, float deltaTime) { }
                }
            }
            """;

        var (outputCompilation, generatedTrees, diagnostics) = RunGenerator(source);

        // Should produce two generated files.
        Assert.Equal(2, generatedTrees.Length);

        var allText = string.Join("\n", generatedTrees.Select(t => t.GetText().ToString()));

        // RenderSystem should have runAfter referencing PhysicsSystem.
        Assert.Contains("runAfter: new[] { \"TestGame.PhysicsSystem\" }", allText);

        // No errors from the generator.
        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));

        // Generated code should compile.
        var compilationErrors = outputCompilation.GetDiagnostics()
            .Where(d => d.Severity == DiagnosticSeverity.Error)
            .ToList();
        Assert.Empty(compilationErrors);
    }

    // ------------------------------------------------------------------
    // 23. DependencyBefore_PassesRunBeforeToRegister
    // ------------------------------------------------------------------
    [Fact]
    public void DependencyBefore_PassesRunBeforeToRegister()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                public partial struct RenderSystem : IEntitySystem
                {
                    public void Execute(ref Health health, float deltaTime) { }
                }

                [Before(typeof(RenderSystem))]
                public partial struct PhysicsSystem : IEntitySystem
                {
                    public void Execute(ref Health health, float deltaTime) { }
                }
            }
            """;

        var (outputCompilation, generatedTrees, diagnostics) = RunGenerator(source);

        Assert.Equal(2, generatedTrees.Length);

        var allText = string.Join("\n", generatedTrees.Select(t => t.GetText().ToString()));

        // PhysicsSystem should have runBefore referencing RenderSystem.
        Assert.Contains("runBefore: new[] { \"TestGame.RenderSystem\" }", allText);

        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));

        var compilationErrors = outputCompilation.GetDiagnostics()
            .Where(d => d.Severity == DiagnosticSeverity.Error)
            .ToList();
        Assert.Empty(compilationErrors);
    }

    // ------------------------------------------------------------------
    // 24. DependencyMultiple_PassesBothArrays
    // ------------------------------------------------------------------
    [Fact]
    public void DependencyMultiple_PassesBothArrays()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                public partial struct PhysicsSystem : IEntitySystem
                {
                    public void Execute(ref Health health, float deltaTime) { }
                }

                public partial struct RenderSystem : IEntitySystem
                {
                    public void Execute(ref Health health, float deltaTime) { }
                }

                [After(typeof(PhysicsSystem))]
                [Before(typeof(RenderSystem))]
                public partial struct AnimationSystem : IEntitySystem
                {
                    public void Execute(ref Health health, float deltaTime) { }
                }
            }
            """;

        var (outputCompilation, generatedTrees, diagnostics) = RunGenerator(source);

        Assert.Equal(3, generatedTrees.Length);

        var allText = string.Join("\n", generatedTrees.Select(t => t.GetText().ToString()));

        // AnimationSystem should have both runAfter and runBefore.
        Assert.Contains("runAfter: new[] { \"TestGame.PhysicsSystem\" }", allText);
        Assert.Contains("runBefore: new[] { \"TestGame.RenderSystem\" }", allText);

        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));

        var compilationErrors = outputCompilation.GetDiagnostics()
            .Where(d => d.Severity == DiagnosticSeverity.Error)
            .ToList();
        Assert.Empty(compilationErrors);
    }

    // ------------------------------------------------------------------
    // 25. GameSystem_WithDependencies
    // ------------------------------------------------------------------
    [Fact]
    public void GameSystem_WithDependencies()
    {
        const string source = """
            using GameEngine.Scripting;

            namespace MyGame
            {
                public partial class PhysicsGameSystem : GameSystem
                {
                    public override void OnUpdate(float deltaTime) { }
                }

                [After(typeof(PhysicsGameSystem))]
                public partial class RenderGameSystem : GameSystem
                {
                    public override void OnUpdate(float deltaTime) { }
                }
            }
            """;

        var (outputCompilation, generatedTrees, diagnostics) = RunGenerator(source);

        Assert.Equal(2, generatedTrees.Length);

        var allText = string.Join("\n", generatedTrees.Select(t => t.GetText().ToString()));

        // RenderGameSystem should have runAfter in its RegisterGameSystem call.
        Assert.Contains("runAfter: new[] { \"MyGame.PhysicsGameSystem\" }", allText);
        Assert.Contains("RegisterGameSystem(", allText);

        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));

        var compilationErrors = outputCompilation.GetDiagnostics()
            .Where(d => d.Severity == DiagnosticSeverity.Error)
            .ToList();
        Assert.Empty(compilationErrors);
    }

    // ==================================================================
    // Optional component (ReadOnlyOptional<T>) tests
    // ==================================================================

    // ------------------------------------------------------------------
    // OptionalComponent_GeneratesArchetypeCheck
    // ------------------------------------------------------------------
    [Fact]
    public void OptionalComponent_GeneratesArchetypeCheck()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                [StructLayout(LayoutKind.Sequential)]
                public struct Armor : IComponent { public float Value; }

                public partial struct DamageSystem : IEntitySystem
                {
                    public void Execute(ref Health health, ReadOnlyOptional<Armor> armor, float deltaTime)
                    {
                    }
                }
            }
            """;

        var (outputCompilation, generatedTrees, diagnostics) = RunGenerator(source);

        Assert.Single(generatedTrees);
        var text = generatedTrees[0].GetText().ToString();

        // Should call ArchetypeHasComponent for the optional component.
        Assert.Contains("ArchetypeHasComponent(__queryHandle, __a, __typeId_TestGame_Armor)", text);

        // Should have conditional span fetch.
        Assert.Contains("GetChunkReadOnlySpan<TestGame.Armor>", text);

        // Should construct ReadOnlyOptional per entity.
        Assert.Contains("new ReadOnlyOptional<TestGame.Armor>", text);
        Assert.Contains("ReadOnlyOptional<TestGame.Armor>.None", text);

        // Should pass __opt_ variable to Execute.
        Assert.Contains("__opt_TestGame_Armor", text);

        // Required component should still use GetChunkSpan (mutable).
        Assert.Contains("GetChunkSpan<TestGame.Health>", text);

        // No errors from the generator.
        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));

        // Generated code should compile.
        var compilationErrors = outputCompilation.GetDiagnostics()
            .Where(d => d.Severity == DiagnosticSeverity.Error)
            .ToList();
        Assert.Empty(compilationErrors);
    }

    // ------------------------------------------------------------------
    // OptionalComponent_NotInRequiredQuery
    // ------------------------------------------------------------------
    [Fact]
    public void OptionalComponent_NotInRequiredQuery()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                [StructLayout(LayoutKind.Sequential)]
                public struct Armor : IComponent { public float Value; }

                public partial struct DamageSystem : IEntitySystem
                {
                    public void Execute(ref Health health, ReadOnlyOptional<Armor> armor, float deltaTime)
                    {
                    }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        Assert.Single(generatedTrees);
        var text = generatedTrees[0].GetText().ToString();

        // The __req stackalloc should only contain the required type ID (Health),
        // NOT the optional type ID (Armor).
        // Find the __req line and verify it only has Health.
        var lines = text.Split('\n');
        var reqLine = lines.FirstOrDefault(l => l.Contains("stackalloc ulong[]"));
        Assert.NotNull(reqLine);
        Assert.Contains("__typeId_TestGame_Health", reqLine);
        Assert.DoesNotContain("__typeId_TestGame_Armor", reqLine);

        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));
    }

    // ------------------------------------------------------------------
    // OptionalComponent_DiagnosticGE0021_DuplicateRequired
    // ------------------------------------------------------------------
    [Fact]
    public void OptionalComponent_DiagnosticGE0021_DuplicateRequired()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                public partial struct BadSystem : IEntitySystem
                {
                    public void Execute(ref Health health, ReadOnlyOptional<Health> optHealth, float deltaTime) { }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        AssertDiagnostic(diagnostics, "GE0021", DiagnosticSeverity.Error);
        Assert.Empty(generatedTrees);
    }

    // ------------------------------------------------------------------
    // OptionalComponent_DiagnosticGE0022_PassedByRef
    // ------------------------------------------------------------------
    [Fact]
    public void OptionalComponent_DiagnosticGE0022_PassedByRef()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                [StructLayout(LayoutKind.Sequential)]
                public struct Armor : IComponent { public float Value; }

                public partial struct BadSystem : IEntitySystem
                {
                    public void Execute(ref Health health, ref ReadOnlyOptional<Armor> armor, float deltaTime) { }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        AssertDiagnostic(diagnostics, "GE0022", DiagnosticSeverity.Error);
        Assert.Empty(generatedTrees);
    }

    // ------------------------------------------------------------------
    // OptionalComponent_RequiresAtLeastOneRequiredParam
    // ------------------------------------------------------------------
    [Fact]
    public void OptionalComponent_RequiresAtLeastOneRequiredParam()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Armor : IComponent { public float Value; }

                public partial struct OptionalOnlySystem : IEntitySystem
                {
                    public void Execute(ReadOnlyOptional<Armor> armor, float deltaTime) { }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        AssertDiagnostic(diagnostics, "GE0006", DiagnosticSeverity.Error);
        Assert.Empty(generatedTrees);
    }

    // ==================================================================
    // EntityCommands tests
    // ==================================================================

    // ------------------------------------------------------------------
    // EntityCommands_GeneratesCommandsInjection
    // ------------------------------------------------------------------
    [Fact]
    public void EntityCommands_GeneratesCommandsInjection()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                public partial struct SpawnSystem : IEntitySystem
                {
                    public void Execute(ref Health health, EntityCommands commands, float deltaTime)
                    {
                    }
                }
            }
            """;

        var (outputCompilation, generatedTrees, diagnostics) = RunGenerator(source);

        Assert.Single(generatedTrees);
        var text = generatedTrees[0].GetText().ToString();

        // Should create __commands variable.
        Assert.Contains("var __commands = new GameEngine.Scripting.EntityCommands", text);
        Assert.Contains("WorldHandle = worldHandle", text);

        // Should pass __commands to Execute.
        Assert.Contains("__commands", text);

        // Should still have normal component handling.
        Assert.Contains("GetChunkSpan<TestGame.Health>", text);

        // No errors from the generator.
        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));

        // Generated code should compile.
        var compilationErrors = outputCompilation.GetDiagnostics()
            .Where(d => d.Severity == DiagnosticSeverity.Error)
            .ToList();
        Assert.Empty(compilationErrors);
    }

    // ------------------------------------------------------------------
    // EntityCommands_DiagnosticGE0023_PassedByRef
    // ------------------------------------------------------------------
    [Fact]
    public void EntityCommands_DiagnosticGE0023_PassedByRef()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                public partial struct BadSystem : IEntitySystem
                {
                    public void Execute(ref Health health, ref EntityCommands commands, float deltaTime) { }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        AssertDiagnostic(diagnostics, "GE0023", DiagnosticSeverity.Error);
        Assert.Empty(generatedTrees);
    }

    // ------------------------------------------------------------------
    // EntityCommands_DiagnosticGE0024_MultipleCommands
    // ------------------------------------------------------------------
    [Fact]
    public void EntityCommands_DiagnosticGE0024_MultipleCommands()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                public partial struct BadSystem : IEntitySystem
                {
                    public void Execute(ref Health health, EntityCommands a, EntityCommands b, float deltaTime) { }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        AssertDiagnostic(diagnostics, "GE0024", DiagnosticSeverity.Error);
        Assert.Empty(generatedTrees);
    }

    // ------------------------------------------------------------------
    // EntityCommands_WithOtherParams — all param types together
    // ------------------------------------------------------------------
    [Fact]
    public void EntityCommands_WithOtherParams()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Position : IComponent { public float X, Y, Z; }

                [StructLayout(LayoutKind.Sequential)]
                public struct Velocity : IComponent { public float X, Y, Z; }

                public partial struct FullSystem : IEntitySystem
                {
                    public void Execute(ref Position pos, in Velocity vel, uint entityId,
                        EntityCommands commands, float deltaTime)
                    {
                    }
                }
            }
            """;

        var (outputCompilation, generatedTrees, diagnostics) = RunGenerator(source);

        Assert.Single(generatedTrees);
        var text = generatedTrees[0].GetText().ToString();

        // All param types present.
        Assert.Contains("GetChunkSpan<TestGame.Position>", text);
        Assert.Contains("GetChunkReadOnlySpan<TestGame.Velocity>", text);
        Assert.Contains("GetChunkEntityIds", text);
        Assert.Contains("var __commands = new GameEngine.Scripting.EntityCommands", text);
        Assert.Contains("deltaTime", text);

        // Execute call includes all params.
        Assert.Contains("ref __span_TestGame_Position[__i]", text);
        Assert.Contains("in __span_TestGame_Velocity[__i]", text);
        Assert.Contains("__entityIds[__i]", text);
        Assert.Contains("__commands", text);

        // No errors from the generator.
        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));

        // Generated code should compile.
        var compilationErrors = outputCompilation.GetDiagnostics()
            .Where(d => d.Severity == DiagnosticSeverity.Error)
            .ToList();
        Assert.Empty(compilationErrors);
    }

    // ==================================================================
    // Priority 1: Untested Diagnostics
    // ==================================================================

    // ------------------------------------------------------------------
    // DiagnosticGE0003 — component type is not unmanaged (has string field)
    // ------------------------------------------------------------------
    [Fact]
    public void DiagnosticGE0003_NotUnmanaged()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct BadComponent : IComponent
                {
                    public string Name;
                }

                public partial struct BadSystem : IEntitySystem
                {
                    public void Execute(ref BadComponent comp, float deltaTime) { }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        AssertDiagnostic(diagnostics, "GE0003", DiagnosticSeverity.Error);
        Assert.Empty(generatedTrees);
    }

    // ------------------------------------------------------------------
    // DiagnosticGE0004 — parameter type does not implement IComponent
    // ------------------------------------------------------------------
    [Fact]
    public void DiagnosticGE0004_NotIComponent()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct PlainStruct
                {
                    public float Value;
                }

                public partial struct BadSystem : IEntitySystem
                {
                    public void Execute(ref PlainStruct data, float deltaTime) { }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        AssertDiagnostic(diagnostics, "GE0004", DiagnosticSeverity.Error);
        Assert.Empty(generatedTrees);
    }

    // ------------------------------------------------------------------
    // DiagnosticGE0005 — component missing [StructLayout]
    // ------------------------------------------------------------------
    [Fact]
    public void DiagnosticGE0005_MissingStructLayout()
    {
        const string source = """
            using GameEngine.Scripting;

            namespace TestGame
            {
                public struct Health : IComponent
                {
                    public float Value;
                }

                public partial struct BadSystem : IEntitySystem
                {
                    public void Execute(ref Health health, float deltaTime) { }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        AssertDiagnostic(diagnostics, "GE0005", DiagnosticSeverity.Error);
        Assert.Empty(generatedTrees);
    }

    // ------------------------------------------------------------------
    // DiagnosticGE0007 — no deltaTime parameter (warning, code still generated)
    // ------------------------------------------------------------------
    [Fact]
    public void DiagnosticGE0007_NoDeltaTime()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                public partial struct NoDeltaSystem : IEntitySystem
                {
                    public void Execute(ref Health health) { }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        // GE0007 fires as a warning.
        AssertDiagnostic(diagnostics, "GE0007", DiagnosticSeverity.Warning);

        // It is only a warning, so generated code should still be produced.
        Assert.Single(generatedTrees);
        var text = generatedTrees[0].GetText().ToString();
        Assert.Contains("__RegisterSystem", text);
        Assert.Contains("__Execute", text);

        // No errors from the generator.
        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));
    }

    // ------------------------------------------------------------------
    // DiagnosticGE0009 — generic Execute method
    // ------------------------------------------------------------------
    [Fact]
    public void DiagnosticGE0009_GenericExecute()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                public partial struct GenericSystem : IEntitySystem
                {
                    public void Execute<T>(ref Health health, float deltaTime) { }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        AssertDiagnostic(diagnostics, "GE0009", DiagnosticSeverity.Error);
        Assert.Empty(generatedTrees);
    }

    // ------------------------------------------------------------------
    // DiagnosticGE0011 — async Execute method
    // ------------------------------------------------------------------
    [Fact]
    public void DiagnosticGE0011_AsyncExecute()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                public partial struct AsyncSystem : IEntitySystem
                {
                    public async void Execute(ref Health health, float deltaTime) { }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        AssertDiagnostic(diagnostics, "GE0011", DiagnosticSeverity.Error);
        Assert.Empty(generatedTrees);
    }

    // ------------------------------------------------------------------
    // DiagnosticGE0015 — duplicate component type in Execute parameters
    // ------------------------------------------------------------------
    [Fact]
    public void DiagnosticGE0015_DuplicateComponent()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                public partial struct DuplicateSystem : IEntitySystem
                {
                    public void Execute(ref Health a, ref Health b, float deltaTime) { }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        AssertDiagnostic(diagnostics, "GE0015", DiagnosticSeverity.Error);
        Assert.Empty(generatedTrees);
    }

    // ------------------------------------------------------------------
    // DiagnosticGE0018 — GameSystem without public parameterless constructor
    // ------------------------------------------------------------------
    [Fact]
    public void DiagnosticGE0018_NoParameterlessConstructor()
    {
        const string source = """
            using GameEngine.Scripting;

            namespace TestGame
            {
                public partial class BadGameSystem : GameSystem
                {
                    public BadGameSystem(int x) { }
                    public override void OnUpdate(float deltaTime) { }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        AssertDiagnostic(diagnostics, "GE0018", DiagnosticSeverity.Error);
        Assert.Empty(generatedTrees);
    }

    // ------------------------------------------------------------------
    // DiagnosticGE0020 — ReadOnlyOptional inner type does not implement IComponent
    // ------------------------------------------------------------------
    [Fact]
    public void DiagnosticGE0020_OptionalNotIComponent()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                [StructLayout(LayoutKind.Sequential)]
                public struct PlainStruct
                {
                    public float Value;
                }

                public partial struct BadSystem : IEntitySystem
                {
                    public void Execute(ref Health health, ReadOnlyOptional<PlainStruct> opt, float deltaTime) { }
                }
            }
            """;

        var (_, generatedTrees, diagnostics) = RunGenerator(source);

        AssertDiagnostic(diagnostics, "GE0020", DiagnosticSeverity.Error);
        Assert.Empty(generatedTrees);
    }

    // ==================================================================
    // Priority 2: Critical Combinatorial Tests
    // ==================================================================

    // ------------------------------------------------------------------
    // AllParamTypesCombined — kitchen-sink test with all parameter kinds
    // ------------------------------------------------------------------
    [Fact]
    public void AllParamTypesCombined_GeneratesAndCompiles()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Position : IComponent { public float X, Y, Z; }

                [StructLayout(LayoutKind.Sequential)]
                public struct Velocity : IComponent { public float X, Y, Z; }

                [StructLayout(LayoutKind.Sequential)]
                public struct Armor : IComponent { public float Reduction; }

                [StructLayout(LayoutKind.Sequential)]
                public struct Static : IComponent { public byte Flag; }

                public partial struct OtherSystem : IEntitySystem
                {
                    void Execute(ref Position pos, float deltaTime) { }
                }

                [Without(typeof(Static))]
                [After(typeof(OtherSystem))]
                public partial struct KitchenSinkSystem : IEntitySystem
                {
                    public int Order => 5;

                    void Execute(ref Position pos, in Velocity vel, ReadOnlyOptional<Armor> armor,
                                 uint entityId, EntityCommands commands, float deltaTime)
                    {
                        pos.X += vel.X * deltaTime;
                    }
                }
            }
            """;

        var (outputCompilation, generatedTrees, diagnostics) = RunGenerator(source);

        // Should produce two generated files (OtherSystem + KitchenSinkSystem).
        Assert.Equal(2, generatedTrees.Length);

        var allText = string.Join("\n", generatedTrees.Select(t => t.GetText().ToString()));

        // Required component type IDs.
        Assert.Contains("__typeId_TestGame_Position", allText);
        Assert.Contains("__typeId_TestGame_Velocity", allText);

        // Optional component type ID (registered but NOT in __req).
        Assert.Contains("__typeId_TestGame_Armor", allText);

        // Excluded component type ID.
        Assert.Contains("__typeId_Excluded_TestGame_Static", allText);

        // ArchetypeHasComponent for optional Armor.
        Assert.Contains("ArchetypeHasComponent", allText);

        // GetChunkEntityIds for entityId parameter.
        Assert.Contains("GetChunkEntityIds", allText);

        // EntityCommands injection.
        Assert.Contains("var __commands = new GameEngine.Scripting.EntityCommands", allText);

        // [After] dependency.
        Assert.Contains("runAfter: new[]", allText);

        // Order = 5 passed to RegisterEntitySystem.
        // Find the KitchenSinkSystem registration and verify order 5.
        var kitchenSinkTree = generatedTrees
            .FirstOrDefault(t => t.GetText().ToString().Contains("KitchenSinkSystem"));
        Assert.NotNull(kitchenSinkTree);
        var kitchenText = kitchenSinkTree!.GetText().ToString();
        Assert.Contains("5,", kitchenText);

        // Verify optional Armor is NOT in __req stackalloc.
        var lines = kitchenText.Split('\n');
        var reqLine = lines.FirstOrDefault(l => l.Contains("stackalloc ulong[]"));
        Assert.NotNull(reqLine);
        Assert.DoesNotContain("__typeId_TestGame_Armor", reqLine);

        // No errors from the generator.
        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));

        // The output compilation should have zero errors.
        var compilationErrors = outputCompilation.GetDiagnostics()
            .Where(d => d.Severity == DiagnosticSeverity.Error)
            .ToList();
        Assert.Empty(compilationErrors);
    }

    // ------------------------------------------------------------------
    // MultipleOptionalComponents — two optionals generate two checks
    // ------------------------------------------------------------------
    [Fact]
    public void MultipleOptionalComponents_GeneratesMultipleChecks()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                [StructLayout(LayoutKind.Sequential)]
                public struct Armor : IComponent { public float Reduction; }

                [StructLayout(LayoutKind.Sequential)]
                public struct Shield : IComponent { public float Capacity; }

                public partial struct MultiOptSystem : IEntitySystem
                {
                    public void Execute(ref Health health, ReadOnlyOptional<Armor> armor,
                                        ReadOnlyOptional<Shield> shield, float deltaTime) { }
                }
            }
            """;

        var (outputCompilation, generatedTrees, diagnostics) = RunGenerator(source);

        Assert.Single(generatedTrees);
        var text = generatedTrees[0].GetText().ToString();

        // Two ArchetypeHasComponent calls, one per optional.
        Assert.Contains("ArchetypeHasComponent(__queryHandle, __a, __typeId_TestGame_Armor)", text);
        Assert.Contains("ArchetypeHasComponent(__queryHandle, __a, __typeId_TestGame_Shield)", text);

        // Both optional spans.
        Assert.Contains("GetChunkReadOnlySpan<TestGame.Armor>", text);
        Assert.Contains("GetChunkReadOnlySpan<TestGame.Shield>", text);

        // Both ReadOnlyOptional constructions.
        Assert.Contains("new ReadOnlyOptional<TestGame.Armor>", text);
        Assert.Contains("new ReadOnlyOptional<TestGame.Shield>", text);

        // No errors from the generator.
        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));

        // Generated code should compile.
        var compilationErrors = outputCompilation.GetDiagnostics()
            .Where(d => d.Severity == DiagnosticSeverity.Error)
            .ToList();
        Assert.Empty(compilationErrors);
    }

    // ------------------------------------------------------------------
    // GameSystem_AbstractBase_ConcreteSubclass — abstract skipped, concrete generated
    // ------------------------------------------------------------------
    [Fact]
    public void GameSystem_AbstractBase_ConcreteSubclass()
    {
        const string source = """
            using GameEngine.Scripting;

            namespace TestGame
            {
                public abstract class BaseSystem : GameSystem
                {
                }

                public partial class ConcreteSystem : BaseSystem
                {
                    public override void OnUpdate(float deltaTime) { }
                }
            }
            """;

        var (outputCompilation, generatedTrees, diagnostics) = RunGenerator(source);

        // Only ConcreteSystem should have generated code (abstract is skipped).
        Assert.Single(generatedTrees);

        var text = generatedTrees[0].GetText().ToString();

        // ConcreteSystem gets registration.
        Assert.Contains("partial class ConcreteSystem", text);
        Assert.Contains("__RegisterGameSystem", text);
        Assert.Contains("RegisterGameSystem(", text);
        Assert.Contains("new TestGame.ConcreteSystem()", text);

        // BaseSystem should NOT appear in generated code.
        Assert.DoesNotContain("partial class BaseSystem", text);

        // No errors from the generator.
        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));

        // Generated code should compile.
        var compilationErrors = outputCompilation.GetDiagnostics()
            .Where(d => d.Severity == DiagnosticSeverity.Error)
            .ToList();
        Assert.Empty(compilationErrors);
    }

    // ------------------------------------------------------------------
    // Built-in component tests (F.6)
    // ------------------------------------------------------------------

    [Fact]
    public void BuiltInComponent_UsesGetComponentTypeIdByName()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                [BuiltInComponent("GameEngine::components::Position")]
                public struct Position : IComponent { public float X, Y, Z; }

                [StructLayout(LayoutKind.Sequential)]
                [BuiltInComponent("GameEngine::components::Velocity")]
                public struct Velocity : IComponent { public float X, Y, Z; }

                public partial struct MoveSystem : IEntitySystem
                {
                    public void Execute(ref Position pos, in Velocity vel, float deltaTime)
                    {
                        pos.X += vel.X * deltaTime;
                    }
                }
            }
            """;

        var (outputCompilation, generatedTrees, diagnostics) = RunGenerator(source);

        Assert.Single(generatedTrees);
        var text = generatedTrees[0].GetText().ToString();

        // Should use GetComponentTypeIdByName for built-in components.
        Assert.Contains("Ecs.GetComponentTypeIdByName(\"GameEngine::components::Position\")", text);
        Assert.Contains("Ecs.GetComponentTypeIdByName(\"GameEngine::components::Velocity\")", text);

        // Should NOT use RegisterBlobComponent for these.
        Assert.DoesNotContain("Ecs.RegisterBlobComponent(", text);

        // Should use slice-based iteration.
        Assert.Contains("GetChunkSpan<TestGame.Position>", text);
        Assert.Contains("GetChunkReadOnlySpan<TestGame.Velocity>", text);
        Assert.Contains("for (int __c = 0; __c < __chunkCount; __c++)", text);

        // No errors.
        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));

        // Generated code should compile.
        var compilationErrors = outputCompilation.GetDiagnostics()
            .Where(d => d.Severity == DiagnosticSeverity.Error)
            .ToList();
        Assert.Empty(compilationErrors);
    }

    [Fact]
    public void MixedBuiltInAndUserComponents_BothRegistrationPaths()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                [BuiltInComponent("GameEngine::components::Position")]
                public struct Position : IComponent { public float X, Y, Z; }

                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                public partial struct DamageSystem : IEntitySystem
                {
                    public void Execute(ref Health health, in Position pos, float deltaTime)
                    {
                        health.Value -= 1.0f;
                    }
                }
            }
            """;

        var (outputCompilation, generatedTrees, diagnostics) = RunGenerator(source);

        Assert.Single(generatedTrees);
        var text = generatedTrees[0].GetText().ToString();

        // Health is a user component → RegisterBlobComponent.
        Assert.Contains("Ecs.RegisterBlobComponent(", text);
        Assert.Contains("\"TestGame.Health\"", text);

        // Position is built-in → GetComponentTypeIdByName.
        Assert.Contains("Ecs.GetComponentTypeIdByName(\"GameEngine::components::Position\")", text);

        // Slice-based iteration for both.
        Assert.Contains("GetChunkSpan<TestGame.Health>", text);
        Assert.Contains("GetChunkReadOnlySpan<TestGame.Position>", text);

        // No errors.
        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));

        var compilationErrors = outputCompilation.GetDiagnostics()
            .Where(d => d.Severity == DiagnosticSeverity.Error)
            .ToList();
        Assert.Empty(compilationErrors);
    }

    [Fact]
    public void BuiltInComponent_AsOptional_UsesGetComponentTypeIdByName()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                [StructLayout(LayoutKind.Sequential)]
                [BuiltInComponent("GameEngine::components::Velocity")]
                public struct Velocity : IComponent { public float X, Y, Z; }

                public partial struct OptionalVelSystem : IEntitySystem
                {
                    public void Execute(ref Health health, ReadOnlyOptional<Velocity> vel, float deltaTime)
                    {
                        if (vel.HasValue) health.Value += vel.Value.X;
                    }
                }
            }
            """;

        var (outputCompilation, generatedTrees, diagnostics) = RunGenerator(source);

        Assert.Single(generatedTrees);
        var text = generatedTrees[0].GetText().ToString();

        // Velocity is built-in and optional → GetComponentTypeIdByName.
        Assert.Contains("Ecs.GetComponentTypeIdByName(\"GameEngine::components::Velocity\")", text);

        // Health is user → RegisterBlobComponent.
        Assert.Contains("Ecs.RegisterBlobComponent(", text);

        // No errors.
        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));

        var compilationErrors = outputCompilation.GetDiagnostics()
            .Where(d => d.Severity == DiagnosticSeverity.Error)
            .ToList();
        Assert.Empty(compilationErrors);
    }

    [Fact]
    public void ChunkBasedIteration_GeneratesChunkLoopWithEmptySkip()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Health : IComponent { public float Value; }

                public partial struct SimpleSystem : IEntitySystem
                {
                    public void Execute(ref Health health, float deltaTime)
                    {
                        health.Value -= deltaTime;
                    }
                }
            }
            """;

        var (outputCompilation, generatedTrees, diagnostics) = RunGenerator(source);

        Assert.Single(generatedTrees);
        var text = generatedTrees[0].GetText().ToString();

        // Verify chunk-cursor iteration pattern.
        Assert.Contains("for (int __c = 0; __c < __chunkCount; __c++)", text);
        Assert.Contains("if (__count == 0) continue;", text);

        // The entity-offset slice walk (O(chunks^2) native lookups) must be gone.
        Assert.DoesNotContain("GetComponentSlice", text);
        Assert.DoesNotContain("while (__offset", text);

        Assert.DoesNotContain(diagnostics,
            d => d.Severity == DiagnosticSeverity.Error && d.Id.StartsWith("GE"));

        var compilationErrors = outputCompilation.GetDiagnostics()
            .Where(d => d.Severity == DiagnosticSeverity.Error)
            .ToList();
        Assert.Empty(compilationErrors);
    }

    // ==================================================================
    // Component schema pipeline (D1): the generator exports each IComponent
    // struct's field table into the per-assembly registration hub. The
    // battery below asserts every emitted offset/size against the CLR's real
    // managed layout (Unsafe.ByteOffset / Unsafe.SizeOf) on structs declared
    // identically in THIS assembly — the generator's build-time walk and the
    // runtime's layout must agree byte-for-byte or chunk reads corrupt.
    // ==================================================================

    // Ground-truth twins of the structs fed to the generator in SchemaBatterySource.
    private enum SchemaMode { Off = 0, On = 1 }

    [StructLayout(LayoutKind.Sequential)]
    private struct SchemaPlainFloats { public float A; public float B; }

    [StructLayout(LayoutKind.Sequential)]
    private struct SchemaMixedAlign { public byte A; public int B; public byte C; public double D; public float E; }

    [StructLayout(LayoutKind.Sequential)]
    private struct SchemaBoolPadding { public bool A; public float B; public bool C; public bool D; public int E; }

    [StructLayout(LayoutKind.Sequential)]
    private struct SchemaVectors { public Vector3 P; public float S; public Vector2 U; public Vector4 Q; public Quaternion R; }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    private struct SchemaPackedOne { public byte A; public int B; public short C; public double D; }

    [StructLayout(LayoutKind.Sequential)]
    private struct SchemaWithEnum { public SchemaMode M; public byte B; public long L; }

    private const string SchemaBatterySource = """
        using System.Numerics;
        using System.Runtime.InteropServices;
        using GameEngine.Scripting;

        namespace TestGame
        {
            public enum SchemaMode { Off = 0, On = 1 }

            [StructLayout(LayoutKind.Sequential)]
            public struct SchemaPlainFloats : IComponent { public float A; public float B; }

            [StructLayout(LayoutKind.Sequential)]
            public struct SchemaMixedAlign : IComponent { public byte A; public int B; public byte C; public double D; public float E; }

            [StructLayout(LayoutKind.Sequential)]
            public struct SchemaBoolPadding : IComponent { public bool A; public float B; public bool C; public bool D; public int E; }

            [StructLayout(LayoutKind.Sequential)]
            public struct SchemaVectors : IComponent { public Vector3 P; public float S; public Vector2 U; public Vector4 Q; public Quaternion R; }

            [StructLayout(LayoutKind.Sequential, Pack = 1)]
            public struct SchemaPackedOne : IComponent { public byte A; public int B; public short C; public double D; }

            [StructLayout(LayoutKind.Sequential)]
            public struct SchemaWithEnum : IComponent { public SchemaMode M; public byte B; public long L; }
        }
        """;

    private static int RuntimeOffset<TStruct, TField>(ref TStruct probe, ref TField field)
        where TStruct : unmanaged
        where TField : unmanaged
    {
        return (int)Unsafe.ByteOffset(
            ref Unsafe.As<TStruct, byte>(ref probe),
            ref Unsafe.As<TField, byte>(ref field));
    }

    /// <summary>
    /// Extracts (name -> offset/size/type) from the hub's RegisterComponentSchema call
    /// for one component.
    /// </summary>
    private static Dictionary<string, (uint Offset, uint Size, ushort Type)> ParseHubFields(
        string hubText, string componentFullName)
    {
        int start = hubText.IndexOf($"RegisterComponentSchema(\"{componentFullName}\"", StringComparison.Ordinal);
        Assert.True(start >= 0, $"hub has no registration for {componentFullName}:\n{hubText}");
        int end = hubText.IndexOf("RegisterComponentSchema(\"", start + 1, StringComparison.Ordinal);
        string block = end < 0 ? hubText.Substring(start) : hubText.Substring(start, end - start);

        var fields = new Dictionary<string, (uint, uint, ushort)>();
        foreach (Match m in Regex.Matches(block,
            @"new GameEngine\.ECS\.ComponentFieldDesc\(""(\w+)"", (\d+), (\d+), \(GameEngine\.ECS\.ComponentFieldType\)(\d+)\)"))
        {
            fields[m.Groups[1].Value] =
                (uint.Parse(m.Groups[2].Value), uint.Parse(m.Groups[3].Value), ushort.Parse(m.Groups[4].Value));
        }
        return fields;
    }

    private static void AssertHubField(
        Dictionary<string, (uint Offset, uint Size, ushort Type)> fields,
        string name, int runtimeOffset, uint size, ushort type)
    {
        Assert.True(fields.ContainsKey(name), $"schema is missing field '{name}'");
        var f = fields[name];
        Assert.Equal((uint)runtimeOffset, f.Offset);
        Assert.Equal(size, f.Size);
        Assert.Equal(type, f.Type);
    }

    [Fact]
    public void ComponentSchema_Offsets_MatchRuntimeManagedLayout()
    {
        var (outputCompilation, hubText, diagnostics) = RunGeneratorSchemaHub(SchemaBatterySource);
        Assert.False(string.IsNullOrEmpty(hubText), "schema hub was not emitted");
        Assert.DoesNotContain(diagnostics, d => d.Severity == DiagnosticSeverity.Error);

        // ComponentFieldType values (mirror of the native FieldTypeId taxonomy).
        const ushort kBool = 1, kUInt8 = 6, kInt16 = 3, kInt32 = 4, kInt64 = 5;
        const ushort kFloat = 10, kDouble = 11, kVec2 = 12, kVec3 = 13, kVec4 = 14, kQuat = 15;

        {
            var p = default(SchemaPlainFloats);
            var f = ParseHubFields(hubText, "TestGame.SchemaPlainFloats");
            Assert.Equal(2, f.Count);
            AssertHubField(f, "A", RuntimeOffset(ref p, ref p.A), 4, kFloat);
            AssertHubField(f, "B", RuntimeOffset(ref p, ref p.B), 4, kFloat);
            Assert.Contains($"{Unsafe.SizeOf<SchemaPlainFloats>()} == Unsafe.SizeOf<global::TestGame.SchemaPlainFloats>()", hubText);
        }
        {
            var p = default(SchemaMixedAlign);
            var f = ParseHubFields(hubText, "TestGame.SchemaMixedAlign");
            Assert.Equal(5, f.Count);
            AssertHubField(f, "A", RuntimeOffset(ref p, ref p.A), 1, kUInt8);
            AssertHubField(f, "B", RuntimeOffset(ref p, ref p.B), 4, kInt32);
            AssertHubField(f, "C", RuntimeOffset(ref p, ref p.C), 1, kUInt8);
            AssertHubField(f, "D", RuntimeOffset(ref p, ref p.D), 8, kDouble);
            AssertHubField(f, "E", RuntimeOffset(ref p, ref p.E), 4, kFloat);
            Assert.Contains($"{Unsafe.SizeOf<SchemaMixedAlign>()} == Unsafe.SizeOf<global::TestGame.SchemaMixedAlign>()", hubText);
        }
        {
            // bool is ONE byte in managed layout (Marshal.OffsetOf would report 4) — the
            // schema must describe the managed layout, because that's what chunk spans hold.
            var p = default(SchemaBoolPadding);
            var f = ParseHubFields(hubText, "TestGame.SchemaBoolPadding");
            Assert.Equal(5, f.Count);
            AssertHubField(f, "A", RuntimeOffset(ref p, ref p.A), 1, kBool);
            AssertHubField(f, "B", RuntimeOffset(ref p, ref p.B), 4, kFloat);
            AssertHubField(f, "C", RuntimeOffset(ref p, ref p.C), 1, kBool);
            AssertHubField(f, "D", RuntimeOffset(ref p, ref p.D), 1, kBool);
            AssertHubField(f, "E", RuntimeOffset(ref p, ref p.E), 4, kInt32);
            Assert.Contains($"{Unsafe.SizeOf<SchemaBoolPadding>()} == Unsafe.SizeOf<global::TestGame.SchemaBoolPadding>()", hubText);
        }
        {
            var p = default(SchemaVectors);
            var f = ParseHubFields(hubText, "TestGame.SchemaVectors");
            Assert.Equal(5, f.Count);
            AssertHubField(f, "P", RuntimeOffset(ref p, ref p.P), 12, kVec3);
            AssertHubField(f, "S", RuntimeOffset(ref p, ref p.S), 4, kFloat);
            AssertHubField(f, "U", RuntimeOffset(ref p, ref p.U), 8, kVec2);
            AssertHubField(f, "Q", RuntimeOffset(ref p, ref p.Q), 16, kVec4);
            AssertHubField(f, "R", RuntimeOffset(ref p, ref p.R), 16, kQuat);
            Assert.Contains($"{Unsafe.SizeOf<SchemaVectors>()} == Unsafe.SizeOf<global::TestGame.SchemaVectors>()", hubText);
        }
        {
            var p = default(SchemaPackedOne);
            var f = ParseHubFields(hubText, "TestGame.SchemaPackedOne");
            Assert.Equal(4, f.Count);
            AssertHubField(f, "A", RuntimeOffset(ref p, ref p.A), 1, kUInt8);
            AssertHubField(f, "B", RuntimeOffset(ref p, ref p.B), 4, kInt32);
            AssertHubField(f, "C", RuntimeOffset(ref p, ref p.C), 2, kInt16);
            AssertHubField(f, "D", RuntimeOffset(ref p, ref p.D), 8, kDouble);
            Assert.Contains($"{Unsafe.SizeOf<SchemaPackedOne>()} == Unsafe.SizeOf<global::TestGame.SchemaPackedOne>()", hubText);
        }
        {
            var p = default(SchemaWithEnum);
            var f = ParseHubFields(hubText, "TestGame.SchemaWithEnum");
            Assert.Equal(3, f.Count);
            AssertHubField(f, "M", RuntimeOffset(ref p, ref p.M), 4, kInt32); // enum -> underlying int
            AssertHubField(f, "B", RuntimeOffset(ref p, ref p.B), 1, kUInt8);
            AssertHubField(f, "L", RuntimeOffset(ref p, ref p.L), 8, kInt64);
            Assert.Contains($"{Unsafe.SizeOf<SchemaWithEnum>()} == Unsafe.SizeOf<global::TestGame.SchemaWithEnum>()", hubText);
        }

        // The hub (including nothing outside #if DEBUG here) must compile.
        Assert.Empty(outputCompilation.GetDiagnostics()
            .Where(d => d.Severity == DiagnosticSeverity.Error));
    }

    [Fact]
    public void ComponentSchema_DebugOffsetAsserts_Compile()
    {
        // Re-run the generator with DEBUG defined so the emitted
        // __ValidateSchemaOffsets block (Unsafe.ByteOffset asserts) is compiled.
        var parseOptions = new CSharpParseOptions(preprocessorSymbols: new[] { "DEBUG" });
        var compilation = CreateCompilation(SchemaBatterySource);
        GeneratorDriver driver = CSharpGeneratorDriver.Create(
            new ISourceGenerator[] { new EntitySystemGenerator().AsSourceGenerator() },
            parseOptions: parseOptions);
        driver.RunGeneratorsAndUpdateCompilation(compilation, out var outputCompilation, out _);

        var hub = outputCompilation.SyntaxTrees.FirstOrDefault(
            t => t.FilePath.EndsWith("__ComponentSchemas.g.cs", StringComparison.Ordinal));
        Assert.NotNull(hub);
        Assert.Contains("__ValidateSchemaOffsets", hub!.GetText().ToString());
        Assert.Empty(outputCompilation.GetDiagnostics()
            .Where(d => d.Severity == DiagnosticSeverity.Error));
    }

    [Fact]
    public void ComponentSchema_UnsupportedField_SkippedWithGE0025_OffsetsStillCorrect()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                public struct Inner { public float X; public float Y; }

                [StructLayout(LayoutKind.Sequential)]
                public struct Holder : IComponent
                {
                    public float Before;
                    public Inner Nested;   // unknown composite: skipped, still occupies space
                    public float After;
                }
            }
            """;

        var (_, hubText, diagnostics) = RunGeneratorSchemaHub(source);
        AssertDiagnostic(diagnostics, "GE0025", DiagnosticSeverity.Warning);

        var f = ParseHubFields(hubText, "TestGame.Holder");
        Assert.Equal(2, f.Count); // Nested is excluded
        Assert.False(f.ContainsKey("Nested"));
        Assert.Equal(0u, f["Before"].Offset);
        Assert.Equal(12u, f["After"].Offset); // the skipped 8-byte Inner still consumed layout
    }

    [Fact]
    public void ComponentSchema_AutoLayout_GE0026_NotExported()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Auto)]
                public struct AutoComp : IComponent { public float A; }
            }
            """;

        var (_, hubText, diagnostics) = RunGeneratorSchemaHub(source);
        AssertDiagnostic(diagnostics, "GE0026", DiagnosticSeverity.Warning);
        Assert.DoesNotContain("AutoComp", hubText);
    }

    [Fact]
    public void ComponentSchema_BuiltInComponent_NotExported()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [BuiltInComponent("GameEngine::Components::Transform")]
                [StructLayout(LayoutKind.Sequential)]
                public struct TransformMirror : IComponent { public float M00; }
            }
            """;

        var (_, hubText, diagnostics) = RunGeneratorSchemaHub(source);
        Assert.DoesNotContain("TransformMirror", hubText);
        AssertNoDiagnostic(diagnostics, "GE0026");
    }

    [Fact]
    public void ComponentSchema_PrivateAndBackingFields_ExcludedButConsumeLayout()
    {
        const string source = """
            using System.Runtime.InteropServices;
            using GameEngine.Scripting;

            namespace TestGame
            {
                [StructLayout(LayoutKind.Sequential)]
                public struct Mixed : IComponent
                {
                    public float Shown;
                    private float _hidden;
                    public float AlsoShown;
                }
            }
            """;

        var (_, hubText, diagnostics) = RunGeneratorSchemaHub(source);
        // Private fields are intentional hiding, not an unsupported type: no GE0025.
        AssertNoDiagnostic(diagnostics, "GE0025");

        var f = ParseHubFields(hubText, "TestGame.Mixed");
        Assert.Equal(2, f.Count);
        Assert.Equal(0u, f["Shown"].Offset);
        Assert.Equal(8u, f["AlsoShown"].Offset); // _hidden consumed 4 bytes at offset 4
    }
}
