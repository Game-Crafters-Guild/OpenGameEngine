using GameEngine.ComponentScanner;
using Xunit;

namespace GameEngine.ComponentScanner.Tests;

// The web emitters (--emit-dts, --emit-json) run end to end through the scanner's entry point over
// a fixture header, beside the C++ registration output of the same parse.
public sealed class WebEmitterTests : IDisposable
{
    private readonly string m_Root = Path.Combine(Path.GetTempPath(), "ComponentScannerWeb_" + Guid.NewGuid().ToString("N"));

    private string ComponentsDir => Path.Combine(m_Root, "Include", "Components");
    private string CppPath => Path.Combine(m_Root, "out", "Reflection.gen.cpp");
    private string DtsPath => Path.Combine(m_Root, "out", "components.d.ts");
    private string JsonPath => Path.Combine(m_Root, "out", "components.json");

    public void Dispose()
    {
        if (Directory.Exists(m_Root))
            Directory.Delete(m_Root, recursive: true);
    }

    private int Scan(string header)
    {
        Directory.CreateDirectory(ComponentsDir);
        File.WriteAllText(Path.Combine(ComponentsDir, "Fixture.h"), header);
        return Program.Main(new[] { "--in", ComponentsDir, "--out", CppPath, "--emit-dts", DtsPath, "--emit-json", JsonPath });
    }

    // Every kind the resolver produces, every marker, an alias, an enum and an editor-only component.
    private const string kEveryKindHeader = @"#pragma once
namespace GameEngine::Components {
using MaterialRef = AssetRef<AssetType::Material>;
enum class Falloff : uint8 { Linear = 0, Smooth = 1, Sharp = 4 };
struct Probe
{
    bool Enabled = true;
    int8 A = 0;
    int16 B = 0;
    int32 C = 0;
    int64 D = 0;
    uint8 E = 0;
    uint16 F = 0;
    uint32 G = 0;
    std::uint64_t H = 0;
    float I = 0.0f;
    double J = 0.0;
    Mathematics::Vector2 Uv;
    Mathematics::Vector3 Position;
    Mathematics::Vector4 Plane;
    Mathematics::Quaternion Rotation;
    ColorLinear Tint;
    MaterialRef Material;
    ECS::EntityHandle Target;
    char Label[16] = {};
    // @ge-tooltip Brightness in lux */ of the probe.
    // @ge-range 0 10
    float Intensity = 1.0f;
    // @ge-range 0.5
    float Radius = 1.0f;
    float Weights[kWeightCount] = {};
    uint8 Bytes[4] = {};
    float Coefficients[3] = {};
    float Samples[5] = {};
    float FovY = 60.0f;
    float IBLScale = 1.0f;
    Falloff Mode = Falloff::Linear;
    Falloff Modes[2] = {};
    uint32 Version = 0; // @ge-readonly
    uint32 CacheSlot = 0; // @ge-hidden
    uint32 LiveBodyId = 0; // [DoNotSerialize]
};
// @ge-editor-only
struct EditorNote
{
    float Size = 1.0f;
};
// @ge-no-add
struct Anchor
{
    Mathematics::Vector3 Offset;
};
}
";

    [Fact]
    public void EmitsOneInterfacePerComponentWithEveryKindAndMarker()
    {
        Assert.Equal(0, Scan(kEveryKindHeader));
        const string expected = @"// Generated from the engine's component headers by ComponentScanner --emit-dts. Do not edit.
// One interface and one ComponentType value per reflected component; & NoAdd marks the ones a page
// cannot add or remove. Each field's type follows its reflected kind.

import type { AssetRef, Color, ComponentType, Entity, NoAdd, Quat, Vec2, Vec3, Vec4 } from './opengine';

export type Falloff = 'Linear' | 'Smooth' | 'Sharp';

export interface Anchor {
    offset: Vec3;
}
export declare const Anchor: ComponentType<Anchor> & NoAdd;

export interface Probe {
    enabled: boolean;
    a: number;
    b: number;
    c: number;
    d: bigint;
    e: number;
    f: number;
    g: number;
    h: bigint;
    i: number;
    j: number;
    uv: Vec2;
    position: Vec3;
    plane: Vec4;
    rotation: Quat;
    tint: Color;
    material: AssetRef;
    target: Entity | null;
    label: string;
    /**
     * Brightness in lux *\/ of the probe.
     * Range: 0 to 10.
     */
    intensity: number;
    /** Minimum: 0.5. */
    radius: number;
    weights: number[];
    bytes: number[];
    coefficients: [number, number, number];
    samples: number[];
    fovY: number;
    iblScale: number;
    mode: Falloff;
    modes: Falloff[];
    readonly version: number;
}
export declare const Probe: ComponentType<Probe>;
";
        Assert.Equal(expected.ReplaceLineEndings("\n"), File.ReadAllText(DtsPath));
    }

    [Fact]
    public void EmitsTheSameModelAsJson()
    {
        Assert.Equal(0, Scan(kEveryKindHeader));
        const string expected = @"{
  ""components"": [
    {
      ""name"": ""Anchor"",
      ""fields"": [
        {
          ""name"": ""Offset"",
          ""tsName"": ""offset"",
          ""kind"": ""Vec3""
        }
      ]
    },
    {
      ""name"": ""Probe"",
      ""fields"": [
        {
          ""name"": ""Enabled"",
          ""tsName"": ""enabled"",
          ""kind"": ""Bool""
        },
        {
          ""name"": ""A"",
          ""tsName"": ""a"",
          ""kind"": ""Int8""
        },
        {
          ""name"": ""B"",
          ""tsName"": ""b"",
          ""kind"": ""Int16""
        },
        {
          ""name"": ""C"",
          ""tsName"": ""c"",
          ""kind"": ""Int32""
        },
        {
          ""name"": ""D"",
          ""tsName"": ""d"",
          ""kind"": ""Int64""
        },
        {
          ""name"": ""E"",
          ""tsName"": ""e"",
          ""kind"": ""UInt8""
        },
        {
          ""name"": ""F"",
          ""tsName"": ""f"",
          ""kind"": ""UInt16""
        },
        {
          ""name"": ""G"",
          ""tsName"": ""g"",
          ""kind"": ""UInt32""
        },
        {
          ""name"": ""H"",
          ""tsName"": ""h"",
          ""kind"": ""UInt64""
        },
        {
          ""name"": ""I"",
          ""tsName"": ""i"",
          ""kind"": ""Float""
        },
        {
          ""name"": ""J"",
          ""tsName"": ""j"",
          ""kind"": ""Double""
        },
        {
          ""name"": ""Uv"",
          ""tsName"": ""uv"",
          ""kind"": ""Vec2""
        },
        {
          ""name"": ""Position"",
          ""tsName"": ""position"",
          ""kind"": ""Vec3""
        },
        {
          ""name"": ""Plane"",
          ""tsName"": ""plane"",
          ""kind"": ""Vec4""
        },
        {
          ""name"": ""Rotation"",
          ""tsName"": ""rotation"",
          ""kind"": ""Quat""
        },
        {
          ""name"": ""Tint"",
          ""tsName"": ""tint"",
          ""kind"": ""Color""
        },
        {
          ""name"": ""Material"",
          ""tsName"": ""material"",
          ""kind"": ""AssetGuid""
        },
        {
          ""name"": ""Target"",
          ""tsName"": ""target"",
          ""kind"": ""EntityHandle""
        },
        {
          ""name"": ""Label"",
          ""tsName"": ""label"",
          ""kind"": ""String"",
          ""count"": 16
        },
        {
          ""name"": ""Intensity"",
          ""tsName"": ""intensity"",
          ""kind"": ""Float"",
          ""tooltip"": ""Brightness in lux */ of the probe."",
          ""min"": 0,
          ""max"": 10
        },
        {
          ""name"": ""Radius"",
          ""tsName"": ""radius"",
          ""kind"": ""Float"",
          ""min"": 0.5
        },
        {
          ""name"": ""Weights"",
          ""tsName"": ""weights"",
          ""kind"": ""Float"",
          ""count"": ""kWeightCount""
        },
        {
          ""name"": ""Bytes"",
          ""tsName"": ""bytes"",
          ""kind"": ""UInt8"",
          ""count"": 4
        },
        {
          ""name"": ""Coefficients"",
          ""tsName"": ""coefficients"",
          ""kind"": ""Float"",
          ""count"": 3
        },
        {
          ""name"": ""Samples"",
          ""tsName"": ""samples"",
          ""kind"": ""Float"",
          ""count"": 5
        },
        {
          ""name"": ""FovY"",
          ""tsName"": ""fovY"",
          ""kind"": ""Float""
        },
        {
          ""name"": ""IBLScale"",
          ""tsName"": ""iblScale"",
          ""kind"": ""Float""
        },
        {
          ""name"": ""Mode"",
          ""tsName"": ""mode"",
          ""kind"": ""UInt8"",
          ""enum"": ""Falloff""
        },
        {
          ""name"": ""Modes"",
          ""tsName"": ""modes"",
          ""kind"": ""UInt8"",
          ""enum"": ""Falloff"",
          ""count"": 2
        },
        {
          ""name"": ""Version"",
          ""tsName"": ""version"",
          ""kind"": ""UInt32"",
          ""readonly"": true
        }
      ]
    }
  ],
  ""enums"": [
    {
      ""name"": ""Falloff"",
      ""values"": [
        {
          ""name"": ""Linear"",
          ""value"": 0
        },
        {
          ""name"": ""Smooth"",
          ""value"": 1
        },
        {
          ""name"": ""Sharp"",
          ""value"": 4
        }
      ]
    }
  ],
  ""omitted"": []
}
";
        Assert.Equal(expected.ReplaceLineEndings("\n"), File.ReadAllText(JsonPath));
    }

    [Fact]
    public void EditorOnlyComponentIsAbsentFromBothOutputsAndStillRegistered()
    {
        Assert.Equal(0, Scan(kEveryKindHeader));
        Assert.DoesNotContain("EditorNote", File.ReadAllText(DtsPath));
        Assert.DoesNotContain("EditorNote", File.ReadAllText(JsonPath));
        Assert.Contains("GE_REFLECT_COMPONENT_EDITORONLY(GameEngine::Components::EditorNote);", File.ReadAllText(CppPath));
    }

    [Fact]
    public void FieldWithoutAKindIsOmittedAndListedWithWhyTheScannerCannotTypeIt()
    {
        const string header = @"namespace GameEngine::Rendering {
struct DayKeys { float Values[4]; };
}
namespace GameEngine::Components {
struct Keyed
{
    float Speed = 1.0f;
    Rendering::DayKeys Keys;
    Math::Curve Shape;
    long Count = 0;
};
}
";
        (int exitCode, string error) = ScanCapturingErrors(header);

        Assert.Equal(0, exitCode);
        Assert.Contains("warning: Keyed.Keys (type 'Rendering::DayKeys') is left out of the web types: a struct with no reflected kind", error);
        Assert.Contains("warning: Keyed.Shape (type 'Math::Curve') is left out of the web types: the type is not declared in the scanned component headers", error);
        Assert.Contains("export interface Keyed {\n    speed: number;\n}\n", File.ReadAllText(DtsPath));
        Assert.Contains(@"""omitted"": [
    {
      ""component"": ""Keyed"",
      ""field"": ""Keys"",
      ""type"": ""Rendering::DayKeys"",
      ""reason"": ""noReflectedKind""
    },
    {
      ""component"": ""Keyed"",
      ""field"": ""Shape"",
      ""type"": ""Math::Curve"",
      ""reason"": ""unresolvedType""
    },
    {
      ""component"": ""Keyed"",
      ""field"": ""Count"",
      ""type"": ""long"",
      ""reason"": ""unresolvedType""
    }
  ]".ReplaceLineEndings("\n"), File.ReadAllText(JsonPath));
        Assert.Contains("GE_REGISTER_COMPONENT_FIELD(GameEngine::Components::Keyed, Shape)", File.ReadAllText(CppPath));
    }

    [Fact]
    public void EnumIsMatchedBeforeAnAliasOfTheSameName()
    {
        const string header = @"namespace GameEngine::Components {
enum class Mode : uint8 { Off, On };
struct Helper { using Mode = int32; };
struct Lamp { Mode State = Mode::Off; };
}
";
        Assert.Equal(0, Scan(header));
        Assert.Contains("    state: Mode;\n", File.ReadAllText(DtsPath));
        Assert.Contains("GE_REFLECT_ENUM_FIELD(GameEngine::Components::Lamp, State, Mode)", File.ReadAllText(CppPath));
    }

    [Fact]
    public void EnumNamedLikeAComponentIsRefusedAndNothingIsWritten()
    {
        const string header = @"namespace Other { enum class Probe : uint8 { A, B }; }
namespace GameEngine::Components {
struct Probe { float X = 0.0f; };
struct Sensor { Other::Probe Kind = Other::Probe::A; };
}
";
        (int exitCode, string error) = ScanCapturingErrors(header);

        Assert.Equal(1, exitCode);
        Assert.Contains("error: the enum Probe and the component Probe would share one name in components.d.ts", error);
        Assert.False(File.Exists(DtsPath));
        Assert.False(File.Exists(CppPath));
    }

    [Fact]
    public void MultiDimensionalArrayIsRefusedByNameAndNothingIsWritten()
    {
        const string header = @"namespace GameEngine::Components {
struct Grid { float M[4][4] = {}; };
}
";
        (int exitCode, string error) = ScanCapturingErrors(header);

        Assert.Equal(1, exitCode);
        Assert.Contains("error: Grid.M is a multi-dimensional array", error);
        Assert.False(File.Exists(DtsPath));
        Assert.False(File.Exists(CppPath));
    }

    [Fact]
    public void FieldKindMatchesTheEnginesFieldTypeId()
    {
        string? dir = AppContext.BaseDirectory;
        while (dir != null && !File.Exists(Path.Combine(dir, "Engine", "Modules", "ECS", "Include", "ECS", "Reflection.h")))
            dir = Path.GetDirectoryName(dir);
        Assert.NotNull(dir);
        string source = CommentStripper.Strip(File.ReadAllText(Path.Combine(dir!, "Engine", "Modules", "ECS", "Include", "ECS", "Reflection.h")));

        EnumDecl fieldTypeId = EnumScanner.Scan(source).Single(e => e.Name == "FieldTypeId");

        Assert.Equal(fieldTypeId.Members.Select(m => (m.Name, m.Value)),
            Enum.GetValues<FieldKind>().Select(k => (k.ToString(), (long)k)));
    }

    private (int ExitCode, string Error) ScanCapturingErrors(string header)
    {
        var error = new StringWriter();
        TextWriter previous = Console.Error;
        Console.SetError(error);
        try
        {
            return (Scan(header), error.ToString());
        }
        finally
        {
            Console.SetError(previous);
        }
    }
}
