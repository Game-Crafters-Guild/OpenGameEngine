#nullable enable

using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.Linq;
using System.Text;
using System.Threading;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace GameEngine.SourceGenerators
{
    /// <summary>
    /// Pipeline 3 of the EntitySystemGenerator: exports a field schema for every
    /// IComponent struct in the compilation into one per-assembly registration hub.
    ///
    /// The hub's [ModuleInitializer] calls Ecs.RegisterComponentSchema(name, size,
    /// fields) at assembly load, which registers the component AND its reflected
    /// field table in the native ComponentFieldRegistry — the same registry native
    /// components fill via GE_REGISTER_COMPONENT. Scene save/load, the inspector,
    /// and hot-reload layout migration then treat C# components exactly like
    /// native ones.
    ///
    /// Offset contract: emitted offsets describe the struct's MANAGED
    /// (LayoutKind.Sequential) layout — the same bytes chunk spans and
    /// Set/GetComponentBytes touch. That is Unsafe.ByteOffset ground truth, NOT
    /// Marshal.OffsetOf (whose marshaled layout widens bool to 4 bytes). The walk
    /// below reproduces the CoreCLR sequential rules for unmanaged structs:
    /// fields in declaration order, each aligned to min(Pack, natural alignment),
    /// total size rounded up to the struct alignment (minimum 1). A DEBUG-only
    /// block in the hub asserts every emitted offset against Unsafe.ByteOffset at
    /// runtime, and the registration passes Unsafe.SizeOf&lt;T&gt;() as the
    /// authoritative size so a walk bug can never corrupt chunk strides.
    /// </summary>
    internal static class ComponentSchemaPipeline
    {
        private const string IComponentFqn = "GameEngine.Scripting.IComponent";
        private const string BuiltInComponentAttributeFqn = "GameEngine.Scripting.BuiltInComponentAttribute";
        private const string StructLayoutAttributeFqn = "System.Runtime.InteropServices.StructLayoutAttribute";
        private const string FieldOffsetAttributeFqn = "System.Runtime.InteropServices.FieldOffsetAttribute";

        // ---------------------------------------------------------------
        // Extraction
        // ---------------------------------------------------------------

        public static bool IsComponentCandidate(SyntaxNode node, CancellationToken ct)
        {
            return node is StructDeclarationSyntax sds
                && sds.BaseList != null
                && sds.BaseList.Types.Count > 0;
        }

        public static ComponentSchemaModel? ExtractComponentModel(
            GeneratorSyntaxContext ctx, CancellationToken ct)
        {
            var structSyntax = (StructDeclarationSyntax)ctx.Node;
            if (ctx.SemanticModel.GetDeclaredSymbol(structSyntax, ct) is not INamedTypeSymbol symbol)
                return null;

            var componentInterface = ctx.SemanticModel.Compilation.GetTypeByMetadataName(IComponentFqn);
            if (componentInterface == null)
                return null;

            bool isComponent = symbol.AllInterfaces.Any(
                i => SymbolEqualityComparer.Default.Equals(i, componentInterface));
            if (!isComponent)
                return null;

            // Built-in components resolve to native reflection by native name; the native
            // side owns their field tables. Generic components cannot be registered.
            foreach (var attr in symbol.GetAttributes())
            {
                if (attr.AttributeClass?.ToDisplayString() == BuiltInComponentAttributeFqn)
                    return null;
            }

            var model = new ComponentSchemaModel
            {
                FullName = symbol.ToDisplayString(),
                Location = structSyntax.Identifier.GetLocation(),
            };

            if (symbol.TypeParameters.Length > 0 || !symbol.IsUnmanagedType)
            {
                model.SchemaUnavailable = true; // GE0026 — registered name+size only (if at all)
                return model;
            }

            var layout = ReadStructLayout(symbol);
            if (layout.Kind == LayoutKindModel.Auto)
            {
                model.SchemaUnavailable = true; // layout unpredictable — never describe it
                return model;
            }

            var walk = ComputeLayout(symbol, layout);
            if (!walk.Valid)
            {
                model.SchemaUnavailable = true;
                return model;
            }

            model.ComputedSize = walk.Size;
            model.Fields = walk.SchemaFields;
            model.SkippedFields = walk.SkippedFields;
            return model;
        }

        // ---------------------------------------------------------------
        // Managed layout walk
        // ---------------------------------------------------------------

        private enum LayoutKindModel { Sequential, Explicit, Auto }

        private readonly struct StructLayoutInfo
        {
            public readonly LayoutKindModel Kind;
            public readonly uint Pack;         // 0 = default (natural alignment)
            public readonly uint ExplicitSize; // 0 = none

            public StructLayoutInfo(LayoutKindModel kind, uint pack, uint explicitSize)
            {
                Kind = kind;
                Pack = pack;
                ExplicitSize = explicitSize;
            }
        }

        private static StructLayoutInfo ReadStructLayout(INamedTypeSymbol symbol)
        {
            // C# structs without the attribute are Sequential in metadata.
            var kind = LayoutKindModel.Sequential;
            uint pack = 0;
            uint size = 0;
            foreach (var attr in symbol.GetAttributes())
            {
                if (attr.AttributeClass?.ToDisplayString() != StructLayoutAttributeFqn)
                    continue;
                if (attr.ConstructorArguments.Length > 0)
                {
                    var v = attr.ConstructorArguments[0].Value;
                    int layoutValue = v is short s ? s : v is int i ? i : 0;
                    kind = layoutValue switch
                    {
                        2 => LayoutKindModel.Explicit,   // LayoutKind.Explicit
                        3 => LayoutKindModel.Auto,       // LayoutKind.Auto
                        _ => LayoutKindModel.Sequential, // LayoutKind.Sequential == 0
                    };
                }
                foreach (var named in attr.NamedArguments)
                {
                    if (named.Key == "Pack" && named.Value.Value is int p && p > 0)
                        pack = (uint)p;
                    else if (named.Key == "Size" && named.Value.Value is int sz && sz > 0)
                        size = (uint)sz;
                }
            }
            return new StructLayoutInfo(kind, pack, size);
        }

        private readonly struct TypeLayout
        {
            public readonly uint Size;
            public readonly uint Alignment;
            public readonly ushort FieldType; // ComponentFieldType value; 0 = not schema-representable
            public readonly bool Valid;

            public TypeLayout(uint size, uint alignment, ushort fieldType)
            {
                Size = size;
                Alignment = alignment;
                FieldType = fieldType;
                Valid = true;
            }

            public static readonly TypeLayout Invalid = default;
        }

        // ComponentFieldType values (mirror of the native ECS FieldTypeId taxonomy).
        private const ushort kBool = 1, kInt8 = 2, kInt16 = 3, kInt32 = 4, kInt64 = 5;
        private const ushort kUInt8 = 6, kUInt16 = 7, kUInt32 = 8, kUInt64 = 9;
        private const ushort kFloat = 10, kDouble = 11;
        private const ushort kVec2 = 12, kVec3 = 13, kVec4 = 14, kQuat = 15, kColor = 17;

        private static TypeLayout LayoutOfType(ITypeSymbol type)
        {
            if (type is IPointerTypeSymbol || type is IFunctionPointerTypeSymbol)
                return new TypeLayout(8, 8, 0); // occupies space; never schema-representable

            if (type.TypeKind == TypeKind.Enum && type is INamedTypeSymbol enumType)
            {
                var underlying = enumType.EnumUnderlyingType;
                return underlying != null ? LayoutOfType(underlying) : TypeLayout.Invalid;
            }

            switch (type.SpecialType)
            {
                case SpecialType.System_Boolean: return new TypeLayout(1, 1, kBool);
                case SpecialType.System_SByte:   return new TypeLayout(1, 1, kInt8);
                case SpecialType.System_Byte:    return new TypeLayout(1, 1, kUInt8);
                case SpecialType.System_Int16:   return new TypeLayout(2, 2, kInt16);
                case SpecialType.System_UInt16:  return new TypeLayout(2, 2, kUInt16);
                case SpecialType.System_Char:    return new TypeLayout(2, 2, kUInt16); // UTF-16 code unit
                case SpecialType.System_Int32:   return new TypeLayout(4, 4, kInt32);
                case SpecialType.System_UInt32:  return new TypeLayout(4, 4, kUInt32);
                case SpecialType.System_Int64:   return new TypeLayout(8, 8, kInt64);
                case SpecialType.System_UInt64:  return new TypeLayout(8, 8, kUInt64);
                case SpecialType.System_Single:  return new TypeLayout(4, 4, kFloat);
                case SpecialType.System_Double:  return new TypeLayout(8, 8, kDouble);
                case SpecialType.System_IntPtr:
                case SpecialType.System_UIntPtr: return new TypeLayout(8, 8, 0); // size is platform-defined; not portable schema data
            }

            if (type is INamedTypeSymbol named && named.TypeKind == TypeKind.Struct)
            {
                var nested = ComputeLayout(named, ReadStructLayout(named));
                if (!nested.Valid)
                    return TypeLayout.Invalid;
                ushort fieldType = named.ToDisplayString() switch
                {
                    "System.Numerics.Vector2" => kVec2,
                    "System.Numerics.Vector3" => kVec3,
                    "System.Numerics.Vector4" => kVec4,
                    "System.Numerics.Quaternion" => kQuat,
                    _ => (ushort)0, // unknown composite: occupies space, not schema-representable
                };
                return new TypeLayout(nested.Size, nested.Alignment, fieldType);
            }

            return TypeLayout.Invalid;
        }

        private readonly struct WalkResult
        {
            public readonly uint Size;
            public readonly uint Alignment;
            public readonly bool Valid;
            public readonly List<ComponentFieldModel> SchemaFields;
            public readonly List<SkippedFieldModel> SkippedFields;

            public WalkResult(uint size, uint alignment, List<ComponentFieldModel> schemaFields,
                              List<SkippedFieldModel> skippedFields)
            {
                Size = size;
                Alignment = alignment;
                Valid = true;
                SchemaFields = schemaFields;
                SkippedFields = skippedFields;
            }
        }

        private static uint AlignUp(uint value, uint alignment)
        {
            return alignment == 0 ? value : (value + alignment - 1) / alignment * alignment;
        }

        private static WalkResult ComputeLayout(INamedTypeSymbol symbol, StructLayoutInfo layout)
        {
            var schemaFields = new List<ComponentFieldModel>();
            var skippedFields = new List<SkippedFieldModel>();

            uint offset = 0;
            uint maxAlign = 1;
            uint maxEnd = 0;

            foreach (var member in symbol.GetMembers())
            {
                if (member is not IFieldSymbol field || field.IsStatic || field.IsConst)
                    continue;

                TypeLayout fieldLayout;
                if (field.IsFixedSizeBuffer)
                {
                    // fixed T buf[N]: element type behind the pointer; occupies N elements.
                    var element = (field.Type as IPointerTypeSymbol)?.PointedAtType;
                    var elemLayout = element != null ? LayoutOfType(element) : TypeLayout.Invalid;
                    if (!elemLayout.Valid)
                        return default;
                    fieldLayout = new TypeLayout(elemLayout.Size * (uint)field.FixedSize,
                                                 elemLayout.Alignment, 0);
                }
                else
                {
                    fieldLayout = LayoutOfType(field.Type);
                    if (!fieldLayout.Valid)
                        return default; // a field we cannot size: the whole layout is unknowable
                }

                uint align = fieldLayout.Alignment;
                if (layout.Pack != 0 && align > layout.Pack)
                    align = layout.Pack;
                if (align > maxAlign)
                    maxAlign = align;

                uint fieldOffset;
                if (layout.Kind == LayoutKindModel.Explicit)
                {
                    if (!TryGetExplicitOffset(field, out fieldOffset))
                        return default; // Explicit layout requires [FieldOffset] on every field
                }
                else
                {
                    fieldOffset = AlignUp(offset, align);
                    offset = fieldOffset + fieldLayout.Size;
                }
                if (fieldOffset + fieldLayout.Size > maxEnd)
                    maxEnd = fieldOffset + fieldLayout.Size;

                // Schema surface: PUBLIC plain fields only. Non-public fields and property
                // backing fields still occupy layout (walked above) but are not editable /
                // persisted state. Public fields of non-representable types are reported
                // (GE0025) so nothing is skipped silently.
                bool isBackingField = field.IsImplicitlyDeclared || field.AssociatedSymbol != null;
                bool isPublicPlainField = field.DeclaredAccessibility == Accessibility.Public && !isBackingField;
                if (!isPublicPlainField)
                    continue;

                if (fieldLayout.FieldType == 0 || field.IsFixedSizeBuffer)
                {
                    skippedFields.Add(new SkippedFieldModel
                    {
                        FieldName = field.Name,
                        FieldTypeName = field.IsFixedSizeBuffer
                            ? "fixed " + ((field.Type as IPointerTypeSymbol)?.PointedAtType.ToDisplayString() ?? "?") + "[" + field.FixedSize + "]"
                            : field.Type.ToDisplayString(),
                    });
                    continue;
                }

                schemaFields.Add(new ComponentFieldModel
                {
                    Name = field.Name,
                    Offset = fieldOffset,
                    Size = fieldLayout.Size,
                    FieldType = fieldLayout.FieldType,
                    // Fully-qualified ("global::..."; keywords for primitives) — this name is
                    // pasted into the DEBUG offset asserts, where a bare name could bind to
                    // the wrong namespace.
                    FieldTypeName = field.Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
                });
            }

            uint size = layout.Kind == LayoutKindModel.Explicit ? maxEnd : offset;
            size = AlignUp(size, maxAlign);
            if (size == 0)
                size = 1; // the CLR gives empty structs one byte
            if (layout.ExplicitSize > size)
                size = layout.ExplicitSize;

            return new WalkResult(size, maxAlign, schemaFields, skippedFields);
        }

        private static bool TryGetExplicitOffset(IFieldSymbol field, out uint offset)
        {
            offset = 0;
            foreach (var attr in field.GetAttributes())
            {
                if (attr.AttributeClass?.ToDisplayString() == FieldOffsetAttributeFqn
                    && attr.ConstructorArguments.Length > 0
                    && attr.ConstructorArguments[0].Value is int v && v >= 0)
                {
                    offset = (uint)v;
                    return true;
                }
            }
            return false;
        }

        // ---------------------------------------------------------------
        // Emission
        // ---------------------------------------------------------------

        public static void EmitComponentSchemas(
            SourceProductionContext ctx, ImmutableArray<ComponentSchemaModel?> models)
        {
            // Dedupe (partial structs contribute one model per declaration).
            var unique = new Dictionary<string, ComponentSchemaModel>();
            foreach (var model in models)
            {
                if (model != null && !unique.ContainsKey(model.FullName))
                    unique.Add(model.FullName, model);
            }
            if (unique.Count == 0)
                return;

            var components = unique.Values.OrderBy(m => m.FullName, StringComparer.Ordinal).ToList();

            foreach (var model in components)
            {
                if (model.SchemaUnavailable)
                {
                    ctx.ReportDiagnostic(Diagnostic.Create(
                        Diagnostics.GE0026_ComponentSchemaUnavailable, model.Location, model.FullName));
                    continue;
                }
                foreach (var skipped in model.SkippedFields)
                {
                    ctx.ReportDiagnostic(Diagnostic.Create(
                        Diagnostics.GE0025_ComponentFieldSkipped, model.Location,
                        model.FullName, skipped.FieldName, skipped.FieldTypeName));
                }
            }

            var emittable = components.Where(m => !m.SchemaUnavailable).ToList();
            if (emittable.Count == 0)
                return;

            ctx.AddSource("__ComponentSchemas.g.cs", GenerateHubSource(emittable));
        }

        private static string GenerateHubSource(List<ComponentSchemaModel> components)
        {
            var sb = new StringBuilder(2048);
            sb.AppendLine("// <auto-generated/>");
            sb.AppendLine("// Per-assembly component schema registration hub. Runs once per assembly");
            sb.AppendLine("// load (including every hot-reload domain swap), so the native engine's");
            sb.AppendLine("// ComponentFieldRegistry always describes the layout this assembly compiled");
            sb.AppendLine("// with; a changed layout migrates placed instances at registration.");
            sb.AppendLine("using System.Runtime.CompilerServices;");
            sb.AppendLine();
            sb.AppendLine("namespace GameEngine.Generated");
            sb.AppendLine("{");
            sb.AppendLine("    internal static class __ComponentSchemaRegistration");
            sb.AppendLine("    {");
            sb.AppendLine("        [ModuleInitializer]");
            sb.AppendLine("        internal static void __RegisterComponentSchemas()");
            sb.AppendLine("        {");

            foreach (var model in components)
            {
                sb.Append("            GameEngine.ECS.Ecs.RegisterComponentSchema(\"");
                sb.Append(model.FullName);
                sb.AppendLine("\",");
                sb.Append("                (uint)Unsafe.SizeOf<global::");
                sb.Append(model.FullName);
                sb.AppendLine(">(),");
                if (model.Fields.Count == 0)
                {
                    sb.AppendLine("                System.Array.Empty<GameEngine.ECS.ComponentFieldDesc>());");
                }
                else
                {
                    sb.AppendLine("                new GameEngine.ECS.ComponentFieldDesc[]");
                    sb.AppendLine("                {");
                    foreach (var field in model.Fields)
                    {
                        sb.Append("                    new GameEngine.ECS.ComponentFieldDesc(\"");
                        sb.Append(field.Name);
                        sb.Append("\", ");
                        sb.Append(field.Offset);
                        sb.Append(", ");
                        sb.Append(field.Size);
                        sb.Append(", (GameEngine.ECS.ComponentFieldType)");
                        sb.Append(field.FieldType);
                        sb.AppendLine("),");
                    }
                    sb.AppendLine("                });");
                }
            }

            sb.AppendLine("#if DEBUG");
            sb.AppendLine("            __ValidateSchemaOffsets();");
            sb.AppendLine("#endif");
            sb.AppendLine("        }");
            sb.AppendLine();
            sb.AppendLine("#if DEBUG");
            sb.AppendLine("        // Runtime cross-check of the generator's layout walk: every emitted offset");
            sb.AppendLine("        // and the computed size must match the CLR's actual managed layout");
            sb.AppendLine("        // (Unsafe.ByteOffset / Unsafe.SizeOf — NOT Marshal.OffsetOf, whose");
            sb.AppendLine("        // marshaled layout widens bool fields).");
            sb.AppendLine("        private static void __ValidateSchemaOffsets()");
            sb.AppendLine("        {");
            foreach (var model in components)
            {
                sb.Append("            {");
                sb.AppendLine();
                sb.Append("                var __probe = default(global::");
                sb.Append(model.FullName);
                sb.AppendLine(");");
                sb.Append("                System.Diagnostics.Debug.Assert(");
                sb.Append(model.ComputedSize);
                sb.Append(" == Unsafe.SizeOf<global::");
                sb.Append(model.FullName);
                sb.Append(">(), \"schema size mismatch: ");
                sb.Append(model.FullName);
                sb.AppendLine("\");");
                foreach (var field in model.Fields)
                {
                    sb.Append("                System.Diagnostics.Debug.Assert(");
                    sb.Append(field.Offset);
                    sb.Append(" == (int)Unsafe.ByteOffset(ref Unsafe.As<global::");
                    sb.Append(model.FullName);
                    sb.Append(", byte>(ref __probe), ref Unsafe.As<");
                    sb.Append(field.FieldTypeName);
                    sb.Append(", byte>(ref Unsafe.AsRef(in __probe.");
                    sb.Append(field.Name);
                    sb.Append("))), \"schema offset mismatch: ");
                    sb.Append(model.FullName);
                    sb.Append('.');
                    sb.Append(field.Name);
                    sb.AppendLine("\");");
                }
                sb.AppendLine("            }");
            }
            sb.AppendLine("        }");
            sb.AppendLine("#endif");
            sb.AppendLine("    }");
            sb.AppendLine("}");
            return sb.ToString();
        }
    }

    // -----------------------------------------------------------------------
    // Data model
    // -----------------------------------------------------------------------

    internal sealed class ComponentSchemaModel : IEquatable<ComponentSchemaModel>
    {
        public string FullName = "";
        public Location? Location;
        public bool SchemaUnavailable;
        public uint ComputedSize;
        public List<ComponentFieldModel> Fields = new();
        public List<SkippedFieldModel> SkippedFields = new();

        public bool Equals(ComponentSchemaModel? other)
        {
            if (other is null) return false;
            if (ReferenceEquals(this, other)) return true;
            if (FullName != other.FullName
                || SchemaUnavailable != other.SchemaUnavailable
                || ComputedSize != other.ComputedSize
                || Fields.Count != other.Fields.Count
                || SkippedFields.Count != other.SkippedFields.Count)
                return false;
            for (int i = 0; i < Fields.Count; i++)
                if (!Fields[i].Equals(other.Fields[i])) return false;
            for (int i = 0; i < SkippedFields.Count; i++)
                if (!SkippedFields[i].Equals(other.SkippedFields[i])) return false;
            return true;
        }

        public override bool Equals(object? obj) => Equals(obj as ComponentSchemaModel);

        public override int GetHashCode()
        {
            int hash = FullName.GetHashCode();
            hash = hash * 31 + (int)ComputedSize;
            hash = hash * 31 + Fields.Count;
            return hash;
        }
    }

    internal sealed class ComponentFieldModel : IEquatable<ComponentFieldModel>
    {
        public string Name = "";
        public uint Offset;
        public uint Size;
        public ushort FieldType;
        public string FieldTypeName = "";

        public bool Equals(ComponentFieldModel? other)
        {
            if (other is null) return false;
            return Name == other.Name && Offset == other.Offset && Size == other.Size
                && FieldType == other.FieldType && FieldTypeName == other.FieldTypeName;
        }

        public override bool Equals(object? obj) => Equals(obj as ComponentFieldModel);
        public override int GetHashCode() => Name.GetHashCode() * 31 + (int)Offset;
    }

    internal sealed class SkippedFieldModel : IEquatable<SkippedFieldModel>
    {
        public string FieldName = "";
        public string FieldTypeName = "";

        public bool Equals(SkippedFieldModel? other)
        {
            if (other is null) return false;
            return FieldName == other.FieldName && FieldTypeName == other.FieldTypeName;
        }

        public override bool Equals(object? obj) => Equals(obj as SkippedFieldModel);
        public override int GetHashCode() => FieldName.GetHashCode();
    }
}
