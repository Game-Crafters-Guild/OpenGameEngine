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
    [Generator(LanguageNames.CSharp)]
    public sealed class EntitySystemGenerator : IIncrementalGenerator
    {
        // Fully-qualified metadata names for marker types.
        private const string IEntitySystemFqn = "GameEngine.Scripting.IEntitySystem";
        private const string IComponentFqn = "GameEngine.Scripting.IComponent";
        private const string WithoutAttributeFqn = "GameEngine.Scripting.WithoutAttribute";
        private const string StructLayoutAttributeFqn =
            "System.Runtime.InteropServices.StructLayoutAttribute";
        private const string GameSystemFqn = "GameEngine.Scripting.GameSystem";
        private const string AfterAttributeFqn = "GameEngine.Scripting.AfterAttribute";
        private const string BeforeAttributeFqn = "GameEngine.Scripting.BeforeAttribute";
        private const string ReadOnlyOptionalFqn = "GameEngine.Scripting.ReadOnlyOptional";
        private const string EntityCommandsFqn = "GameEngine.Scripting.EntityCommands";
        private const string BuiltInComponentAttributeFqn = "GameEngine.Scripting.BuiltInComponentAttribute";

        public void Initialize(IncrementalGeneratorInitializationContext context)
        {
            // Pipeline 1: Find all struct declarations that implement IEntitySystem.
            var entitySystemCandidates = context.SyntaxProvider.CreateSyntaxProvider(
                predicate: IsEntitySystemCandidate,
                transform: ExtractModel)
                .Where(static m => m != null);

            context.RegisterSourceOutput(entitySystemCandidates, EmitSourceAndDiagnostics!);

            // Pipeline 2: Find all class declarations that extend GameSystem.
            var gameSystemCandidates = context.SyntaxProvider.CreateSyntaxProvider(
                predicate: IsGameSystemCandidate,
                transform: ExtractGameSystemModel)
                .Where(static m => m != null);

            context.RegisterSourceOutput(gameSystemCandidates, EmitGameSystemSourceAndDiagnostics!);

            // Pipeline 3: Export a field schema for every IComponent struct into one
            // per-assembly registration hub (collected — one emission per compilation).
            var componentSchemaCandidates = context.SyntaxProvider.CreateSyntaxProvider(
                predicate: ComponentSchemaPipeline.IsComponentCandidate,
                transform: ComponentSchemaPipeline.ExtractComponentModel)
                .Where(static m => m != null)
                .Collect();

            context.RegisterSourceOutput(componentSchemaCandidates, ComponentSchemaPipeline.EmitComponentSchemas);
        }

        /// <summary>
        /// Fast syntactic filter: struct declarations with at least one base type.
        /// </summary>
        private static bool IsEntitySystemCandidate(SyntaxNode node, CancellationToken ct)
        {
            return node is StructDeclarationSyntax sds
                && sds.BaseList != null
                && sds.BaseList.Types.Count > 0;
        }

        /// <summary>
        /// Semantic extraction: validate the struct implements IEntitySystem and build the model.
        /// </summary>
        private static EntitySystemModel? ExtractModel(
            GeneratorSyntaxContext ctx, CancellationToken ct)
        {
            var structSyntax = (StructDeclarationSyntax)ctx.Node;
            var structSymbol = ctx.SemanticModel.GetDeclaredSymbol(structSyntax, ct);
            if (structSymbol == null)
                return null;

            // Check if the struct implements IEntitySystem.
            var entitySystemInterface = ctx.SemanticModel.Compilation
                .GetTypeByMetadataName(IEntitySystemFqn);
            if (entitySystemInterface == null)
                return null;

            bool implementsInterface = false;
            foreach (var iface in structSymbol.AllInterfaces)
            {
                if (SymbolEqualityComparer.Default.Equals(iface, entitySystemInterface))
                {
                    implementsInterface = true;
                    break;
                }
            }
            if (!implementsInterface)
                return null;

            var componentInterface = ctx.SemanticModel.Compilation
                .GetTypeByMetadataName(IComponentFqn);

            var model = new EntitySystemModel();
            model.StructName = structSymbol.Name;
            model.Namespace = structSymbol.ContainingNamespace.IsGlobalNamespace
                ? null
                : structSymbol.ContainingNamespace.ToDisplayString();
            model.FullName = structSymbol.ToDisplayString();
            model.StructLocation = structSyntax.Identifier.GetLocation();
            model.IsPartial = structSyntax.Modifiers.Any(SyntaxKind.PartialKeyword);
            model.IsNested = structSymbol.ContainingType != null;

            // Check for instance fields (GE0016).
            foreach (var member in structSymbol.GetMembers())
            {
                if (member is IFieldSymbol field && !field.IsStatic && !field.IsImplicitlyDeclared)
                {
                    model.HasInstanceFields = true;
                    model.InstanceFieldLocation = field.Locations.FirstOrDefault();
                    break;
                }
            }

            // Extract Order property override.
            model.Order = 0;
            foreach (var member in structSymbol.GetMembers())
            {
                if (member is IPropertySymbol prop && prop.Name == "Order" && !prop.IsStatic)
                {
                    // Try to extract the constant value from the getter.
                    // This is best-effort for simple `int Order => N;` patterns.
                    var propSyntaxRefs = prop.DeclaringSyntaxReferences;
                    foreach (var syntaxRef in propSyntaxRefs)
                    {
                        var propSyntax = syntaxRef.GetSyntax(ct);
                        if (propSyntax is PropertyDeclarationSyntax pds
                            && pds.ExpressionBody != null)
                        {
                            var constValue = ctx.SemanticModel
                                .GetConstantValue(pds.ExpressionBody.Expression, ct);
                            if (constValue.HasValue && constValue.Value is int orderVal)
                            {
                                model.Order = orderVal;
                            }
                        }
                    }
                    break;
                }
            }

            // Extract [Without] attribute.
            model.ExcludedComponents = new List<ComponentTypeInfo>();
            foreach (var attr in structSymbol.GetAttributes())
            {
                if (attr.AttributeClass?.ToDisplayString() == WithoutAttributeFqn)
                {
                    foreach (var arg in attr.ConstructorArguments)
                    {
                        if (arg.Kind == TypedConstantKind.Array)
                        {
                            foreach (var element in arg.Values)
                            {
                                if (element.Value is INamedTypeSymbol excludedType)
                                {
                                    string? builtInName = null;
                                    foreach (var excAttr in excludedType.GetAttributes())
                                    {
                                        if (excAttr.AttributeClass?.ToDisplayString()
                                            == BuiltInComponentAttributeFqn
                                            && excAttr.ConstructorArguments.Length > 0)
                                        {
                                            builtInName =
                                                excAttr.ConstructorArguments[0].Value as string;
                                            break;
                                        }
                                    }
                                    model.ExcludedComponents.Add(new ComponentTypeInfo
                                    {
                                        TypeName = excludedType.Name,
                                        TypeNamespace = excludedType.ContainingNamespace
                                            .IsGlobalNamespace
                                            ? null
                                            : excludedType.ContainingNamespace.ToDisplayString(),
                                        FullName = excludedType.ToDisplayString(),
                                        BuiltInNativeName = builtInName,
                                    });
                                }
                            }
                        }
                    }
                }
            }

            // Extract [After] and [Before] attributes.
            model.RunAfter = new List<string>();
            model.RunBefore = new List<string>();
            foreach (var attr in structSymbol.GetAttributes())
            {
                var attrName = attr.AttributeClass?.ToDisplayString();
                if (attrName == AfterAttributeFqn)
                {
                    if (attr.ConstructorArguments.Length > 0
                        && attr.ConstructorArguments[0].Value is INamedTypeSymbol afterType)
                    {
                        model.RunAfter.Add(afterType.ToDisplayString());
                    }
                }
                else if (attrName == BeforeAttributeFqn)
                {
                    if (attr.ConstructorArguments.Length > 0
                        && attr.ConstructorArguments[0].Value is INamedTypeSymbol beforeType)
                    {
                        model.RunBefore.Add(beforeType.ToDisplayString());
                    }
                }
            }

            // Find Execute methods.
            var executeMethods = new List<IMethodSymbol>();
            foreach (var member in structSymbol.GetMembers())
            {
                if (member is IMethodSymbol method && method.Name == "Execute")
                {
                    executeMethods.Add(method);
                }
            }

            model.ExecuteMethodCount = executeMethods.Count;

            if (executeMethods.Count == 1)
            {
                var executeMethod = executeMethods[0];
                model.ExecuteLocation = executeMethod.Locations.FirstOrDefault();
                model.ExecuteIsStatic = executeMethod.IsStatic;
                model.ExecuteIsGeneric = executeMethod.IsGenericMethod;
                model.ExecuteIsAsync = executeMethod.IsAsync;

                // Extract parameters.
                model.Parameters = new List<ExecuteParameter>();
                foreach (var param in executeMethod.Parameters)
                {
                    var ep = new ExecuteParameter();
                    ep.Name = param.Name;
                    ep.TypeName = param.Type.Name;
                    ep.TypeNamespace = param.Type.ContainingNamespace?.IsGlobalNamespace == true
                        ? null
                        : param.Type.ContainingNamespace?.ToDisplayString();
                    ep.FullTypeName = param.Type.ToDisplayString();
                    ep.RefKind = param.RefKind;
                    ep.Location = param.Locations.FirstOrDefault();

                    // Determine parameter role.
                    if (param.Type.SpecialType == SpecialType.System_Single
                        && param.Name == "deltaTime")
                    {
                        ep.Role = ParameterRole.DeltaTime;
                    }
                    else if (param.Type.SpecialType == SpecialType.System_UInt32
                        && param.Name == "entityId")
                    {
                        ep.Role = ParameterRole.EntityId;
                    }
                    else if (param.Type.ToDisplayString() == EntityCommandsFqn)
                    {
                        ep.Role = ParameterRole.EntityCommands;
                    }
                    else if (param.Type is INamedTypeSymbol namedOpt
                        && namedOpt.IsGenericType
                        && namedOpt.OriginalDefinition.ToDisplayString()
                            == ReadOnlyOptionalFqn + "<T>")
                    {
                        // ReadOnlyOptional<T> parameter — extract inner type T.
                        ep.Role = ParameterRole.OptionalReadOnlyComponent;
                        var innerType = namedOpt.TypeArguments[0];
                        ep.InnerFullTypeName = innerType.ToDisplayString();
                        ep.RefKind = param.RefKind;

                        bool isComponent = false;
                        if (componentInterface != null)
                        {
                            foreach (var iface in innerType.AllInterfaces)
                            {
                                if (SymbolEqualityComparer.Default.Equals(
                                    iface, componentInterface))
                                {
                                    isComponent = true;
                                    break;
                                }
                            }
                        }
                        ep.ImplementsIComponent = isComponent;
                        ep.IsUnmanaged = innerType.IsUnmanagedType;

                        ep.HasStructLayout = false;
                        if (innerType is INamedTypeSymbol innerNamed)
                        {
                            foreach (var attr in innerNamed.GetAttributes())
                            {
                                if (attr.AttributeClass?.ToDisplayString()
                                    == StructLayoutAttributeFqn)
                                {
                                    ep.HasStructLayout = true;
                                }
                                else if (attr.AttributeClass?.ToDisplayString()
                                    == BuiltInComponentAttributeFqn
                                    && attr.ConstructorArguments.Length > 0)
                                {
                                    ep.InnerBuiltInNativeName =
                                        attr.ConstructorArguments[0].Value as string;
                                }
                            }
                            ep.HasBoolFields = HasBoolFields(innerNamed);
                        }
                    }
                    else
                    {
                        // Check if it implements IComponent.
                        bool isComponent = false;
                        if (componentInterface != null)
                        {
                            foreach (var iface in param.Type.AllInterfaces)
                            {
                                if (SymbolEqualityComparer.Default.Equals(
                                    iface, componentInterface))
                                {
                                    isComponent = true;
                                    break;
                                }
                            }
                        }
                        ep.ImplementsIComponent = isComponent;
                        ep.IsUnmanaged = param.Type.IsUnmanagedType;
                        ep.Role = ParameterRole.Component;

                        // Check for [StructLayout] on the component type.
                        ep.HasStructLayout = false;
                        if (param.Type is INamedTypeSymbol namedType)
                        {
                            foreach (var attr in namedType.GetAttributes())
                            {
                                if (attr.AttributeClass?.ToDisplayString()
                                    == StructLayoutAttributeFqn)
                                {
                                    ep.HasStructLayout = true;
                                }
                                else if (attr.AttributeClass?.ToDisplayString()
                                    == BuiltInComponentAttributeFqn
                                    && attr.ConstructorArguments.Length > 0)
                                {
                                    ep.BuiltInNativeName =
                                        attr.ConstructorArguments[0].Value as string;
                                }
                            }

                            // Check for bool fields (GE0014).
                            ep.HasBoolFields = HasBoolFields(namedType);
                        }
                    }

                    model.Parameters.Add(ep);
                }
            }
            else if (executeMethods.Count > 1)
            {
                model.ExecuteLocation = executeMethods[0].Locations.FirstOrDefault();
            }

            return model;
        }

        // -----------------------------------------------------------------------
        // GameSystem pipeline
        // -----------------------------------------------------------------------

        /// <summary>
        /// Fast syntactic filter: class declarations with at least one base type.
        /// </summary>
        private static bool IsGameSystemCandidate(SyntaxNode node, CancellationToken ct)
        {
            return node is ClassDeclarationSyntax cds
                && cds.BaseList != null
                && cds.BaseList.Types.Count > 0;
        }

        /// <summary>
        /// Semantic extraction: validate the class extends GameSystem and build the model.
        /// </summary>
        private static GameSystemModel? ExtractGameSystemModel(
            GeneratorSyntaxContext ctx, CancellationToken ct)
        {
            var classSyntax = (ClassDeclarationSyntax)ctx.Node;
            var classSymbol = ctx.SemanticModel.GetDeclaredSymbol(classSyntax, ct);
            if (classSymbol == null)
                return null;

            // Must not be abstract — we only generate for concrete subclasses.
            if (classSymbol.IsAbstract)
                return null;

            // Walk the BaseType chain to check if it eventually reaches GameSystem.
            var gameSystemType = ctx.SemanticModel.Compilation
                .GetTypeByMetadataName(GameSystemFqn);
            if (gameSystemType == null)
                return null;

            bool extendsGameSystem = false;
            var current = classSymbol.BaseType;
            while (current != null)
            {
                if (SymbolEqualityComparer.Default.Equals(current, gameSystemType))
                {
                    extendsGameSystem = true;
                    break;
                }
                current = current.BaseType;
            }
            if (!extendsGameSystem)
                return null;

            var model = new GameSystemModel();
            model.ClassName = classSymbol.Name;
            model.Namespace = classSymbol.ContainingNamespace.IsGlobalNamespace
                ? null
                : classSymbol.ContainingNamespace.ToDisplayString();
            model.FullName = classSymbol.ToDisplayString();
            model.ClassLocation = classSyntax.Identifier.GetLocation();
            model.IsPartial = classSyntax.Modifiers.Any(SyntaxKind.PartialKeyword);
            model.IsNested = classSymbol.ContainingType != null;

            // Check for a public parameterless constructor.
            // If no constructors are declared, the compiler provides a default public one.
            bool hasExplicitConstructors = false;
            bool hasPublicParameterless = false;
            foreach (var member in classSymbol.InstanceConstructors)
            {
                if (member.IsImplicitlyDeclared)
                    continue;
                hasExplicitConstructors = true;
                if (member.DeclaredAccessibility == Accessibility.Public
                    && member.Parameters.Length == 0)
                {
                    hasPublicParameterless = true;
                }
            }
            model.HasPublicParameterlessConstructor = !hasExplicitConstructors || hasPublicParameterless;

            // Extract Order property override (same best-effort as IEntitySystem).
            model.Order = 0;
            foreach (var member in classSymbol.GetMembers())
            {
                if (member is IPropertySymbol prop && prop.Name == "Order" && !prop.IsStatic)
                {
                    var propSyntaxRefs = prop.DeclaringSyntaxReferences;
                    foreach (var syntaxRef in propSyntaxRefs)
                    {
                        var propSyntax = syntaxRef.GetSyntax(ct);
                        if (propSyntax is PropertyDeclarationSyntax pds
                            && pds.ExpressionBody != null)
                        {
                            var constValue = ctx.SemanticModel
                                .GetConstantValue(pds.ExpressionBody.Expression, ct);
                            if (constValue.HasValue && constValue.Value is int orderVal)
                            {
                                model.Order = orderVal;
                            }
                        }
                    }
                    break;
                }
            }

            // Extract [After] and [Before] attributes.
            model.RunAfter = new List<string>();
            model.RunBefore = new List<string>();
            foreach (var attr in classSymbol.GetAttributes())
            {
                var attrName = attr.AttributeClass?.ToDisplayString();
                if (attrName == AfterAttributeFqn)
                {
                    if (attr.ConstructorArguments.Length > 0
                        && attr.ConstructorArguments[0].Value is INamedTypeSymbol afterType)
                    {
                        model.RunAfter.Add(afterType.ToDisplayString());
                    }
                }
                else if (attrName == BeforeAttributeFqn)
                {
                    if (attr.ConstructorArguments.Length > 0
                        && attr.ConstructorArguments[0].Value is INamedTypeSymbol beforeType)
                    {
                        model.RunBefore.Add(beforeType.ToDisplayString());
                    }
                }
            }

            return model;
        }

        /// <summary>
        /// Emit source code and diagnostics for a single GameSystem subclass.
        /// </summary>
        private static void EmitGameSystemSourceAndDiagnostics(
            SourceProductionContext ctx, GameSystemModel model)
        {
            var diagnostics = new List<Diagnostic>();
            // Every diagnostic below blocks code generation, whether it is reported as an
            // error (GE0018 — the type is unusable) or a warning (GE0017/GE0019 — the type
            // still runs through reflection discovery, just unregistered).
            bool cannotGenerate = false;

            // GE0017: must be partial class.
            if (!model.IsPartial)
            {
                diagnostics.Add(Diagnostic.Create(
                    Diagnostics.GE0017_GameSystemMustBePartialClass,
                    model.ClassLocation,
                    model.ClassName));
                cannotGenerate = true;
            }

            // GE0019: must not be a nested type.
            if (model.IsNested)
            {
                diagnostics.Add(Diagnostic.Create(
                    Diagnostics.GE0019_GameSystemMustNotBeNested,
                    model.ClassLocation,
                    model.ClassName));
                cannotGenerate = true;
            }

            // GE0018: must have a public parameterless constructor.
            if (!model.HasPublicParameterlessConstructor)
            {
                diagnostics.Add(Diagnostic.Create(
                    Diagnostics.GE0018_GameSystemNoParameterlessConstructor,
                    model.ClassLocation,
                    model.ClassName));
                cannotGenerate = true;
            }

            foreach (var diag in diagnostics)
            {
                ctx.ReportDiagnostic(diag);
            }

            if (cannotGenerate)
                return;

            var source = GenerateGameSystemSource(model);
            var hintName = model.Namespace != null
                ? $"{model.Namespace}.{model.ClassName}.GameSystem.g.cs"
                : $"{model.ClassName}.GameSystem.g.cs";
            ctx.AddSource(hintName, source);
        }

        /// <summary>
        /// Generates the companion partial class source code for a GameSystem subclass.
        /// </summary>
        private static string GenerateGameSystemSource(GameSystemModel model)
        {
            var sb = new StringBuilder(512);

            sb.AppendLine("// <auto-generated/>");
            sb.AppendLine("using System;");
            sb.AppendLine("using System.Runtime.CompilerServices;");
            sb.AppendLine();

            bool hasNamespace = model.Namespace != null;
            if (hasNamespace)
            {
                sb.Append("namespace ");
                sb.AppendLine(model.Namespace);
                sb.AppendLine("{");
            }

            string indent = hasNamespace ? "    " : "";
            string indent2 = indent + "    ";
            string indent3 = indent2 + "    ";

            sb.Append(indent);
            sb.Append("partial class ");
            sb.AppendLine(model.ClassName);
            sb.Append(indent);
            sb.AppendLine("{");

            sb.Append(indent2);
            sb.AppendLine("[ModuleInitializer]");
            sb.Append(indent2);
            sb.AppendLine("internal static void __RegisterGameSystem()");
            sb.Append(indent2);
            sb.AppendLine("{");

            sb.Append(indent3);
            sb.AppendLine("GameEngine.Scripting.Runtime.GameSystemRunner.RegisterGameSystem(");
            sb.Append(indent3);
            sb.Append("    \"");
            sb.Append(model.FullName);
            sb.AppendLine("\",");
            sb.Append(indent3);
            sb.Append("    () => new ");
            sb.Append(model.FullName);
            sb.AppendLine("(),");
            sb.Append(indent3);
            sb.Append("    ");
            sb.Append(model.Order.ToString());

            bool hasRunAfter = model.RunAfter != null && model.RunAfter.Count > 0;
            bool hasRunBefore = model.RunBefore != null && model.RunBefore.Count > 0;

            if (hasRunAfter || hasRunBefore)
            {
                sb.AppendLine(",");
                sb.Append(indent3);
                if (hasRunAfter)
                {
                    sb.Append("    runAfter: new[] { ");
                    for (int ra = 0; ra < model.RunAfter!.Count; ra++)
                    {
                        if (ra > 0) sb.Append(", ");
                        sb.Append("\"");
                        sb.Append(model.RunAfter[ra]);
                        sb.Append("\"");
                    }
                    sb.Append(" }");
                }
                else
                {
                    sb.Append("    runAfter: null");
                }

                if (hasRunBefore)
                {
                    sb.AppendLine(",");
                    sb.Append(indent3);
                    sb.Append("    runBefore: new[] { ");
                    for (int rb = 0; rb < model.RunBefore!.Count; rb++)
                    {
                        if (rb > 0) sb.Append(", ");
                        sb.Append("\"");
                        sb.Append(model.RunBefore[rb]);
                        sb.Append("\"");
                    }
                    sb.Append(" }");
                }
            }

            sb.AppendLine(");");

            sb.Append(indent2);
            sb.AppendLine("}");

            sb.Append(indent);
            sb.AppendLine("}");

            if (hasNamespace)
            {
                sb.AppendLine("}");
            }

            return sb.ToString();
        }

        /// <summary>
        /// Converts a fully-qualified type name to a valid C# identifier for use in
        /// generated field/variable names. Replaces dots and plus signs with underscores.
        /// E.g., "MyGame.Physics.Position" → "MyGame_Physics_Position"
        /// </summary>
        private static string MangledName(string fullTypeName)
        {
            // Replace characters that are invalid in C# identifiers.
            // Handles namespaces (.), nested types (+), and generics (<, >, ,).
            var sb = new StringBuilder(fullTypeName.Length);
            foreach (char c in fullTypeName)
            {
                if (char.IsLetterOrDigit(c) || c == '_')
                    sb.Append(c);
                else
                    sb.Append('_');
            }
            return sb.ToString();
        }

        private static bool HasBoolFields(INamedTypeSymbol type)
        {
            foreach (var member in type.GetMembers())
            {
                if (member is IFieldSymbol field
                    && !field.IsStatic
                    && field.Type.SpecialType == SpecialType.System_Boolean)
                {
                    return true;
                }
            }
            return false;
        }

        /// <summary>
        /// Emit source code and diagnostics for a single IEntitySystem struct.
        /// </summary>
        private static void EmitSourceAndDiagnostics(
            SourceProductionContext ctx, EntitySystemModel model)
        {
            var diagnostics = new List<Diagnostic>();
            bool hasErrors = false;

            // GE0001: must be partial struct.
            if (!model.IsPartial)
            {
                diagnostics.Add(Diagnostic.Create(
                    Diagnostics.GE0001_MustBePartialStruct,
                    model.StructLocation,
                    model.StructName));
                hasErrors = true;
            }

            // GE0013: must not be nested.
            if (model.IsNested)
            {
                diagnostics.Add(Diagnostic.Create(
                    Diagnostics.GE0013_MustNotBeNested,
                    model.StructLocation,
                    model.StructName));
                hasErrors = true;
            }

            // GE0002: no Execute method.
            if (model.ExecuteMethodCount == 0)
            {
                diagnostics.Add(Diagnostic.Create(
                    Diagnostics.GE0002_NoExecuteMethod,
                    model.StructLocation,
                    model.StructName));
                hasErrors = true;
            }

            // GE0008: multiple Execute methods.
            if (model.ExecuteMethodCount > 1)
            {
                diagnostics.Add(Diagnostic.Create(
                    Diagnostics.GE0008_MultipleExecuteMethods,
                    model.ExecuteLocation ?? model.StructLocation,
                    model.StructName));
                hasErrors = true;
            }

            if (model.ExecuteMethodCount == 1)
            {
                // GE0010: Execute must not be static.
                if (model.ExecuteIsStatic)
                {
                    diagnostics.Add(Diagnostic.Create(
                        Diagnostics.GE0010_ExecuteMustNotBeStatic,
                        model.ExecuteLocation ?? model.StructLocation,
                        model.StructName));
                    hasErrors = true;
                }

                // GE0009: Execute must not be generic.
                if (model.ExecuteIsGeneric)
                {
                    diagnostics.Add(Diagnostic.Create(
                        Diagnostics.GE0009_ExecuteMustNotBeGeneric,
                        model.ExecuteLocation ?? model.StructLocation,
                        model.StructName));
                    hasErrors = true;
                }

                // GE0011: Execute must not be async.
                if (model.ExecuteIsAsync)
                {
                    diagnostics.Add(Diagnostic.Create(
                        Diagnostics.GE0011_ExecuteMustNotBeAsync,
                        model.ExecuteLocation ?? model.StructLocation,
                        model.StructName));
                    hasErrors = true;
                }

                // Validate parameters.
                var componentParams = new List<ExecuteParameter>();
                var optionalParams = new List<ExecuteParameter>();
                bool hasDeltaTime = false;
                bool hasEntityId = false;
                bool hasEntityCommands = false;
                var seenComponentTypes = new HashSet<string>();

                if (model.Parameters != null)
                {
                    foreach (var param in model.Parameters)
                    {
                        if (param.Role == ParameterRole.DeltaTime)
                        {
                            hasDeltaTime = true;
                            continue;
                        }
                        if (param.Role == ParameterRole.EntityId)
                        {
                            hasEntityId = true;
                            continue;
                        }

                        if (param.Role == ParameterRole.EntityCommands)
                        {
                            // GE0023: EntityCommands must not be passed by ref or in.
                            if (param.RefKind == RefKind.Ref || param.RefKind == RefKind.In)
                            {
                                diagnostics.Add(Diagnostic.Create(
                                    Diagnostics.GE0023_EntityCommandsByRef,
                                    param.Location ?? model.ExecuteLocation ?? model.StructLocation));
                                hasErrors = true;
                                continue;
                            }

                            // GE0024: Multiple EntityCommands parameters.
                            if (hasEntityCommands)
                            {
                                diagnostics.Add(Diagnostic.Create(
                                    Diagnostics.GE0024_MultipleEntityCommands,
                                    param.Location ?? model.ExecuteLocation ?? model.StructLocation,
                                    model.StructName));
                                hasErrors = true;
                                continue;
                            }

                            hasEntityCommands = true;
                            continue;
                        }

                        if (param.Role == ParameterRole.OptionalReadOnlyComponent)
                        {
                            string innerTypeName = param.InnerFullTypeName ?? param.FullTypeName;

                            // GE0022: ReadOnlyOptional<T> must not be passed by ref or in.
                            if (param.RefKind == RefKind.Ref || param.RefKind == RefKind.In)
                            {
                                diagnostics.Add(Diagnostic.Create(
                                    Diagnostics.GE0022_OptionalByRef,
                                    param.Location ?? model.ExecuteLocation ?? model.StructLocation,
                                    innerTypeName));
                                hasErrors = true;
                                continue;
                            }

                            // GE0020: inner type must implement IComponent.
                            if (!param.ImplementsIComponent)
                            {
                                diagnostics.Add(Diagnostic.Create(
                                    Diagnostics.GE0020_OptionalNotIComponent,
                                    param.Location ?? model.ExecuteLocation ?? model.StructLocation,
                                    innerTypeName));
                                hasErrors = true;
                                continue;
                            }

                            // GE0003: inner type must be unmanaged.
                            if (!param.IsUnmanaged)
                            {
                                diagnostics.Add(Diagnostic.Create(
                                    Diagnostics.GE0003_NotUnmanaged,
                                    param.Location ?? model.ExecuteLocation ?? model.StructLocation,
                                    innerTypeName));
                                hasErrors = true;
                                continue;
                            }

                            // GE0021: same type as both required and optional.
                            if (seenComponentTypes.Contains(innerTypeName))
                            {
                                diagnostics.Add(Diagnostic.Create(
                                    Diagnostics.GE0021_DuplicateRequiredOptional,
                                    param.Location ?? model.ExecuteLocation ?? model.StructLocation,
                                    innerTypeName));
                                hasErrors = true;
                                continue;
                            }
                            seenComponentTypes.Add(innerTypeName);

                            // GE0005: missing [StructLayout].
                            // Skip for [BuiltInComponent] types — the C# compiler elides
                            // [StructLayout(Sequential)] for structs that are already sequential,
                            // so GetAttributes() won't find it on types from external assemblies.
                            if (!param.HasStructLayout && param.InnerBuiltInNativeName == null)
                            {
                                diagnostics.Add(Diagnostic.Create(
                                    Diagnostics.GE0005_MissingStructLayout,
                                    param.Location ?? model.ExecuteLocation ?? model.StructLocation,
                                    innerTypeName));
                                hasErrors = true;
                            }

                            // GE0014: bool fields (warning).
                            if (param.HasBoolFields)
                            {
                                diagnostics.Add(Diagnostic.Create(
                                    Diagnostics.GE0014_BoolField,
                                    param.Location ?? model.ExecuteLocation ?? model.StructLocation,
                                    innerTypeName));
                            }

                            optionalParams.Add(param);
                            continue;
                        }

                        // Component parameter validation.
                        // GE0004: must implement IComponent.
                        if (!param.ImplementsIComponent)
                        {
                            diagnostics.Add(Diagnostic.Create(
                                Diagnostics.GE0004_NotIComponent,
                                param.Location ?? model.ExecuteLocation ?? model.StructLocation,
                                param.FullTypeName));
                            hasErrors = true;
                            continue;
                        }

                        // GE0003: must be unmanaged.
                        if (!param.IsUnmanaged)
                        {
                            diagnostics.Add(Diagnostic.Create(
                                Diagnostics.GE0003_NotUnmanaged,
                                param.Location ?? model.ExecuteLocation ?? model.StructLocation,
                                param.FullTypeName));
                            hasErrors = true;
                            continue;
                        }

                        // GE0012: component passed by value (not ref/in).
                        if (param.RefKind != RefKind.Ref && param.RefKind != RefKind.In)
                        {
                            diagnostics.Add(Diagnostic.Create(
                                Diagnostics.GE0012_ComponentByValue,
                                param.Location ?? model.ExecuteLocation ?? model.StructLocation,
                                param.Name,
                                param.FullTypeName));
                            hasErrors = true;
                            continue;
                        }

                        // GE0015: duplicate component type.
                        if (!seenComponentTypes.Add(param.FullTypeName))
                        {
                            diagnostics.Add(Diagnostic.Create(
                                Diagnostics.GE0015_DuplicateComponent,
                                param.Location ?? model.ExecuteLocation ?? model.StructLocation,
                                param.FullTypeName));
                            hasErrors = true;
                            continue;
                        }

                        // GE0005: missing [StructLayout] (error — risks silent data corruption).
                        // Skip for [BuiltInComponent] types — the C# compiler elides
                        // [StructLayout(Sequential)] for default-sequential structs.
                        if (!param.HasStructLayout && param.BuiltInNativeName == null)
                        {
                            diagnostics.Add(Diagnostic.Create(
                                Diagnostics.GE0005_MissingStructLayout,
                                param.Location ?? model.ExecuteLocation ?? model.StructLocation,
                                param.FullTypeName));
                            hasErrors = true;
                        }

                        // GE0014: bool fields (warning).
                        if (param.HasBoolFields)
                        {
                            diagnostics.Add(Diagnostic.Create(
                                Diagnostics.GE0014_BoolField,
                                param.Location ?? model.ExecuteLocation ?? model.StructLocation,
                                param.FullTypeName));
                        }

                        componentParams.Add(param);
                    }
                }

                // GE0006: no required component parameters (optional-only not allowed).
                if (componentParams.Count == 0 && !hasErrors)
                {
                    diagnostics.Add(Diagnostic.Create(
                        Diagnostics.GE0006_NoComponentParameters,
                        model.ExecuteLocation ?? model.StructLocation,
                        model.StructName));
                    hasErrors = true;
                }

                // GE0007: no deltaTime (warning).
                if (!hasDeltaTime)
                {
                    diagnostics.Add(Diagnostic.Create(
                        Diagnostics.GE0007_NoDeltaTime,
                        model.ExecuteLocation ?? model.StructLocation,
                        model.StructName));
                }

                // Store validated component params and flags for code gen.
                model.ValidComponentParameters = componentParams;
                model.OptionalComponentParameters = optionalParams;
                model.HasDeltaTime = hasDeltaTime;
                model.HasEntityId = hasEntityId;
                model.HasEntityCommands = hasEntityCommands;
            }

            // GE0016: struct has instance fields (warning).
            if (model.HasInstanceFields)
            {
                diagnostics.Add(Diagnostic.Create(
                    Diagnostics.GE0016_InstanceFields,
                    model.InstanceFieldLocation ?? model.StructLocation,
                    model.StructName));
            }

            // Report all diagnostics.
            foreach (var diag in diagnostics)
            {
                ctx.ReportDiagnostic(diag);
            }

            // Only generate code if there are no errors and we have a valid model.
            if (hasErrors
                || !model.IsPartial
                || model.IsNested
                || model.ExecuteMethodCount != 1
                || model.ValidComponentParameters == null
                || model.ValidComponentParameters.Count == 0)
            {
                return;
            }

            var source = GenerateSource(model);
            var hintName = model.Namespace != null
                ? $"{model.Namespace}.{model.StructName}.g.cs"
                : $"{model.StructName}.g.cs";
            ctx.AddSource(hintName, source);
        }

        /// <summary>
        /// Generates the companion partial struct source code.
        /// </summary>
        private static string GenerateSource(EntitySystemModel model)
        {
            var sb = new StringBuilder(2048);
            var components = model.ValidComponentParameters!;
            var optionals = model.OptionalComponentParameters ?? new List<ExecuteParameter>();
            var excluded = model.ExcludedComponents;

            sb.AppendLine("// <auto-generated/>");
            sb.AppendLine("#pragma warning disable CS0162 // Unreachable code");
            sb.AppendLine("#pragma warning disable CS0219 // Variable assigned but never used");
            sb.AppendLine("using System;");
            sb.AppendLine("using System.Runtime.CompilerServices;");
            sb.AppendLine("using GameEngine.ECS;");
            sb.AppendLine("using GameEngine.ECS.Internal;");
            sb.AppendLine("using GameEngine.Scripting;");
            sb.AppendLine("using GameEngine.Scripting.Runtime;");
            sb.AppendLine();

            bool hasNamespace = model.Namespace != null;
            if (hasNamespace)
            {
                sb.Append("namespace ");
                sb.AppendLine(model.Namespace);
                sb.AppendLine("{");
            }

            string indent = hasNamespace ? "    " : "";
            string indent2 = indent + "    ";
            string indent3 = indent2 + "    ";
            string indent4 = indent3 + "    ";
            string indent5 = indent4 + "    ";
            string indent6 = indent5 + "    ";

            sb.Append(indent);
            sb.Append("partial struct ");
            sb.AppendLine(model.StructName);
            sb.Append(indent);
            sb.AppendLine("{");

            // Static type ID fields for each required component.
            foreach (var comp in components)
            {
                sb.Append(indent2);
                sb.Append("private static ulong __typeId_");
                sb.Append(MangledName(comp.FullTypeName));
                sb.AppendLine(";");
            }

            // Static type ID fields for each excluded component.
            if (excluded != null)
            {
                foreach (var exc in excluded)
                {
                    sb.Append(indent2);
                    sb.Append("private static ulong __typeId_Excluded_");
                    sb.Append(MangledName(exc.FullName));
                    sb.AppendLine(";");
                }
            }

            // Static type ID fields for each optional component.
            foreach (var opt in optionals)
            {
                string innerName = opt.InnerFullTypeName!;
                sb.Append(indent2);
                sb.Append("private static ulong __typeId_");
                sb.Append(MangledName(innerName));
                sb.AppendLine(";");
            }

            // Cached query handle and type-ID-resolved flag.
            sb.Append(indent2);
            sb.AppendLine("private static ulong __queryHandle;");
            sb.Append(indent2);
            sb.AppendLine("private static bool __typeIdsResolved;");
            sb.AppendLine();

            // __RegisterSystem method — only registers with GameSystemRunner.
            // Type IDs are resolved lazily in __EnsureTypeIds (called from __Execute)
            // because [ModuleInitializer] runs before engine components are registered.
            sb.Append(indent2);
            sb.AppendLine("[System.Runtime.CompilerServices.ModuleInitializer]");
            sb.Append(indent2);
            sb.AppendLine("internal static void __RegisterSystem()");
            sb.Append(indent2);
            sb.AppendLine("{");

            sb.AppendLine();
            sb.Append(indent3);
            sb.AppendLine("GameSystemRunner.RegisterEntitySystem(");
            sb.Append(indent4);
            sb.Append("\"");
            sb.Append(model.FullName);
            sb.AppendLine("\",");
            sb.Append(indent4);
            sb.Append(model.Order.ToString());
            sb.AppendLine(",");
            sb.Append(indent4);
            sb.AppendLine("__Execute,");
            sb.Append(indent4);
            sb.Append("__Destroy");

            bool hasRunAfter = model.RunAfter != null && model.RunAfter.Count > 0;
            bool hasRunBefore = model.RunBefore != null && model.RunBefore.Count > 0;

            if (hasRunAfter || hasRunBefore)
            {
                sb.AppendLine(",");
                // runAfter parameter
                sb.Append(indent4);
                if (hasRunAfter)
                {
                    sb.Append("runAfter: new[] { ");
                    for (int ra = 0; ra < model.RunAfter!.Count; ra++)
                    {
                        if (ra > 0) sb.Append(", ");
                        sb.Append("\"");
                        sb.Append(model.RunAfter[ra]);
                        sb.Append("\"");
                    }
                    sb.Append(" }");
                }
                else
                {
                    sb.Append("runAfter: null");
                }

                if (hasRunBefore)
                {
                    sb.AppendLine(",");
                    sb.Append(indent4);
                    sb.Append("runBefore: new[] { ");
                    for (int rb = 0; rb < model.RunBefore!.Count; rb++)
                    {
                        if (rb > 0) sb.Append(", ");
                        sb.Append("\"");
                        sb.Append(model.RunBefore[rb]);
                        sb.Append("\"");
                    }
                    sb.Append(" }");
                }
            }

            sb.AppendLine(");");

            sb.Append(indent2);
            sb.AppendLine("}");
            sb.AppendLine();

            // __EnsureTypeIds — lazily resolves component type IDs on first Execute call.
            // Deferred because [ModuleInitializer] runs before engine components are registered.
            sb.Append(indent2);
            sb.AppendLine("private static void __EnsureTypeIds()");
            sb.Append(indent2);
            sb.AppendLine("{");
            sb.Append(indent3);
            sb.AppendLine("if (__typeIdsResolved) return;");

            // Register/resolve each required component type.
            foreach (var comp in components)
            {
                sb.Append(indent3);
                sb.Append("__typeId_");
                sb.Append(MangledName(comp.FullTypeName));
                if (comp.BuiltInNativeName != null)
                {
                    sb.Append(" = Ecs.GetComponentTypeIdByName(\"");
                    sb.Append(comp.BuiltInNativeName);
                    sb.AppendLine("\");");
                }
                else
                {
                    sb.Append(" = Ecs.RegisterBlobComponent(");
                    sb.AppendLine();
                    sb.Append(indent4);
                    sb.Append("\"");
                    sb.Append(comp.FullTypeName);
                    sb.Append("\", (uint)Unsafe.SizeOf<");
                    sb.Append(comp.FullTypeName);
                    sb.AppendLine(">());");
                }
            }

            // Resolve excluded component type IDs.
            if (excluded != null)
            {
                foreach (var exc in excluded)
                {
                    sb.Append(indent3);
                    sb.Append("__typeId_Excluded_");
                    sb.Append(MangledName(exc.FullName));
                    if (exc.BuiltInNativeName != null)
                    {
                        sb.Append(" = Ecs.GetComponentTypeIdByName(\"");
                        sb.Append(exc.BuiltInNativeName);
                        sb.AppendLine("\");");
                    }
                    else
                    {
                        sb.Append(" = Ecs.RegisterBlobComponent(");
                        sb.AppendLine();
                        sb.Append(indent4);
                        sb.Append("\"");
                        sb.Append(exc.FullName);
                        sb.Append("\", (uint)Unsafe.SizeOf<");
                        sb.Append(exc.FullName);
                        sb.AppendLine(">());");
                    }
                }
            }

            // Resolve optional component type IDs.
            foreach (var opt in optionals)
            {
                string innerName = opt.InnerFullTypeName!;
                sb.Append(indent3);
                sb.Append("__typeId_");
                sb.Append(MangledName(innerName));
                if (opt.InnerBuiltInNativeName != null)
                {
                    sb.Append(" = Ecs.GetComponentTypeIdByName(\"");
                    sb.Append(opt.InnerBuiltInNativeName);
                    sb.AppendLine("\");");
                }
                else
                {
                    sb.Append(" = Ecs.RegisterBlobComponent(");
                    sb.AppendLine();
                    sb.Append(indent4);
                    sb.Append("\"");
                    sb.Append(innerName);
                    sb.Append("\", (uint)Unsafe.SizeOf<");
                    sb.Append(innerName);
                    sb.AppendLine(">());");
                }
            }

            sb.Append(indent3);
            sb.AppendLine("__typeIdsResolved = true;");
            sb.Append(indent2);
            sb.AppendLine("}");
            sb.AppendLine();

            // __Execute method.
            sb.Append(indent2);
            sb.AppendLine("internal static void __Execute(ulong worldHandle, float deltaTime)");
            sb.Append(indent2);
            sb.AppendLine("{");

            // Lazily resolve type IDs and create query on first call.
            sb.Append(indent3);
            sb.AppendLine("if (__queryHandle == 0)");
            sb.Append(indent3);
            sb.AppendLine("{");
            sb.Append(indent4);
            sb.AppendLine("__EnsureTypeIds();");

            // Required type IDs array.
            sb.Append(indent4);
            sb.Append("Span<ulong> __req = stackalloc ulong[] { ");
            for (int i = 0; i < components.Count; i++)
            {
                if (i > 0) sb.Append(", ");
                sb.Append("__typeId_");
                sb.Append(MangledName(components[i].FullTypeName));
            }
            sb.AppendLine(" };");

            // Excluded type IDs array.
            if (excluded != null && excluded.Count > 0)
            {
                sb.Append(indent4);
                sb.Append("Span<ulong> __exc = stackalloc ulong[] { ");
                for (int i = 0; i < excluded.Count; i++)
                {
                    if (i > 0) sb.Append(", ");
                    sb.Append("__typeId_Excluded_");
                    sb.Append(MangledName(excluded[i].FullName));
                }
                sb.AppendLine(" };");

                sb.Append(indent4);
                sb.AppendLine(
                    "__queryHandle = ChunkQueryNative.CreateCachedQuery(worldHandle, __req, __exc);");
            }
            else
            {
                sb.Append(indent4);
                sb.AppendLine(
                    "__queryHandle = ChunkQueryNative.CreateCachedQuery(worldHandle, __req, default);");
            }

            sb.Append(indent3);
            sb.AppendLine("}");
            sb.AppendLine();

            // Reset query and iterate.
            sb.Append(indent3);
            sb.AppendLine("ChunkQueryNative.ResetQuery(__queryHandle, out int __archetypeCount);");
            sb.AppendLine();
            sb.Append(indent3);
            sb.Append("var __instance = default(");
            sb.Append(model.StructName);
            sb.AppendLine(");");

            if (model.HasEntityCommands)
            {
                sb.Append(indent3);
                sb.AppendLine("var __commands = new GameEngine.Scripting.EntityCommands { WorldHandle = worldHandle };");
            }

            sb.Append(indent3);
            sb.AppendLine("for (int __a = 0; __a < __archetypeCount; __a++)");
            sb.Append(indent3);
            sb.AppendLine("{");

            sb.Append(indent4);
            sb.AppendLine("ChunkQueryNative.GetArchetypeInfo(__queryHandle, __a,");
            sb.Append(indent5);
            sb.AppendLine("out _, out int __chunkCount);");

            // Per-archetype: check if optional components are present.
            foreach (var opt in optionals)
            {
                string innerName = opt.InnerFullTypeName!;
                string mangledInner = MangledName(innerName);
                sb.AppendLine();
                sb.Append(indent4);
                sb.Append("bool __has_");
                sb.Append(mangledInner);
                sb.Append(" = ChunkQueryNative.ArchetypeHasComponent(__queryHandle, __a, __typeId_");
                sb.Append(mangledInner);
                sb.AppendLine(");");
            }

            sb.AppendLine();

            // Chunk-cursor iteration: storage is colocated (one ArchetypeTable per
            // archetype, all component columns share the same chunk boundaries), so
            // per-chunk spans are fetched in O(1) by chunk index. This replaced the
            // entity-offset slice walk, whose native lookup re-scanned chunks from 0
            // on every call (O(chunks^2 * components) per archetype).
            sb.Append(indent4);
            sb.AppendLine("for (int __c = 0; __c < __chunkCount; __c++)");
            sb.Append(indent4);
            sb.AppendLine("{");

            // Get spans for each required component.
            foreach (var comp in components)
            {
                string mangledComp = MangledName(comp.FullTypeName);
                sb.Append(indent5);
                if (comp.RefKind == RefKind.Ref)
                {
                    sb.Append("var __span_");
                    sb.Append(mangledComp);
                    sb.Append(" = ChunkQueryNative.GetChunkSpan<");
                    sb.Append(comp.FullTypeName);
                    sb.AppendLine(">(");
                    sb.Append(indent6);
                    sb.Append("__queryHandle, __a, __c, __typeId_");
                    sb.Append(mangledComp);
                    sb.AppendLine(");");
                }
                else // RefKind.In
                {
                    sb.Append("var __span_");
                    sb.Append(mangledComp);
                    sb.Append(" = ChunkQueryNative.GetChunkReadOnlySpan<");
                    sb.Append(comp.FullTypeName);
                    sb.AppendLine(">(");
                    sb.Append(indent6);
                    sb.Append("__queryHandle, __a, __c, __typeId_");
                    sb.Append(mangledComp);
                    sb.AppendLine(");");
                }
            }

            // Get conditional read-only spans for optional components.
            foreach (var opt in optionals)
            {
                string innerName = opt.InnerFullTypeName!;
                string mangledInner = MangledName(innerName);
                sb.Append(indent5);
                sb.Append("ReadOnlySpan<");
                sb.Append(innerName);
                sb.Append("> __span_");
                sb.Append(mangledInner);
                sb.Append(" = __has_");
                sb.Append(mangledInner);
                sb.AppendLine();
                sb.Append(indent6);
                sb.Append("? ChunkQueryNative.GetChunkReadOnlySpan<");
                sb.Append(innerName);
                sb.Append(">(__queryHandle, __a, __c, __typeId_");
                sb.Append(mangledInner);
                sb.AppendLine(")");
                sb.Append(indent6);
                sb.AppendLine(": default;");
            }

            // Compute entity count: minimum across all present component spans.
            sb.Append(indent5);
            sb.Append("int __count = __span_");
            sb.Append(MangledName(components[0].FullTypeName));
            sb.AppendLine(".Length;");
            for (int i = 1; i < components.Count; i++)
            {
                sb.Append(indent5);
                sb.Append("__count = Math.Min(__count, __span_");
                sb.Append(MangledName(components[i].FullTypeName));
                sb.AppendLine(".Length);");
            }
            foreach (var opt in optionals)
            {
                string mangledInner = MangledName(opt.InnerFullTypeName!);
                sb.Append(indent5);
                sb.Append("if (__has_");
                sb.Append(mangledInner);
                sb.Append(") __count = Math.Min(__count, __span_");
                sb.Append(mangledInner);
                sb.AppendLine(".Length);");
            }

            // Trailing chunks can be empty; skip them.
            sb.Append(indent5);
            sb.AppendLine("if (__count == 0) continue;");
            sb.AppendLine();

            // Get entity IDs if needed (clamped so __entityIds[__i] can never overrun).
            if (model.HasEntityId)
            {
                sb.Append(indent5);
                sb.AppendLine(
                    "var __entityIds = ChunkQueryNative.GetChunkEntityIds(__queryHandle, __a, __c);");
                sb.Append(indent5);
                sb.AppendLine("__count = Math.Min(__count, __entityIds.Length);");
            }

            // Inner entity loop.
            sb.Append(indent5);
            sb.AppendLine("for (int __i = 0; __i < __count; __i++)");
            sb.Append(indent5);
            sb.AppendLine("{");

            // Construct optional values per entity.
            foreach (var opt in optionals)
            {
                string innerName = opt.InnerFullTypeName!;
                string mangledInner = MangledName(innerName);
                sb.Append(indent6);
                sb.Append("var __opt_");
                sb.Append(mangledInner);
                sb.Append(" = __has_");
                sb.Append(mangledInner);
                sb.AppendLine();
                sb.Append(indent6);
                sb.Append("    ? new ReadOnlyOptional<");
                sb.Append(innerName);
                sb.Append(">(__span_");
                sb.Append(mangledInner);
                sb.AppendLine("[__i])");
                sb.Append(indent6);
                sb.Append("    : ReadOnlyOptional<");
                sb.Append(innerName);
                sb.AppendLine(">.None;");
            }

            // Build the Execute call.
            sb.Append(indent6);
            sb.Append("__instance.Execute(");
            sb.AppendLine();

            var allParams = model.Parameters!;
            for (int i = 0; i < allParams.Count; i++)
            {
                var param = allParams[i];
                sb.Append(indent6);
                sb.Append("    ");

                if (param.Role == ParameterRole.DeltaTime)
                {
                    sb.Append("deltaTime");
                }
                else if (param.Role == ParameterRole.EntityId)
                {
                    sb.Append("__entityIds[__i]");
                }
                else if (param.Role == ParameterRole.Component)
                {
                    string mangledParam = MangledName(param.FullTypeName);
                    if (param.RefKind == RefKind.Ref)
                    {
                        sb.Append("ref __span_");
                        sb.Append(mangledParam);
                        sb.Append("[__i]");
                    }
                    else // RefKind.In
                    {
                        sb.Append("in __span_");
                        sb.Append(mangledParam);
                        sb.Append("[__i]");
                    }
                }
                else if (param.Role == ParameterRole.EntityCommands)
                {
                    sb.Append("__commands");
                }
                else if (param.Role == ParameterRole.OptionalReadOnlyComponent)
                {
                    string mangledInner = MangledName(param.InnerFullTypeName!);
                    sb.Append("__opt_");
                    sb.Append(mangledInner);
                }

                if (i < allParams.Count - 1)
                    sb.Append(",");
                sb.AppendLine();
            }

            sb.Append(indent6);
            sb.AppendLine(");");

            sb.Append(indent5);
            sb.AppendLine("}"); // end entity loop

            sb.Append(indent4);
            sb.AppendLine("}"); // end chunk loop

            sb.Append(indent3);
            sb.AppendLine("}"); // end archetype loop

            sb.Append(indent2);
            sb.AppendLine("}"); // end __Execute
            sb.AppendLine();

            // __Destroy method.
            sb.Append(indent2);
            sb.AppendLine("internal static void __Destroy()");
            sb.Append(indent2);
            sb.AppendLine("{");
            sb.Append(indent3);
            sb.AppendLine("if (__queryHandle != 0)");
            sb.Append(indent3);
            sb.AppendLine("{");
            sb.Append(indent4);
            sb.AppendLine("ChunkQueryNative.DestroyCachedQuery(__queryHandle);");
            sb.Append(indent4);
            sb.AppendLine("__queryHandle = 0;");
            sb.Append(indent3);
            sb.AppendLine("}");
            sb.Append(indent2);
            sb.AppendLine("}"); // end __Destroy

            sb.Append(indent);
            sb.AppendLine("}"); // end struct

            if (hasNamespace)
            {
                sb.AppendLine("}"); // end namespace
            }

            return sb.ToString();
        }
    }

    // -----------------------------------------------------------------------
    // Data model classes
    // -----------------------------------------------------------------------

    internal sealed class EntitySystemModel : IEquatable<EntitySystemModel>
    {
        public string StructName = "";
        public string? Namespace;
        public string FullName = "";
        public Location? StructLocation;
        public bool IsPartial;
        public bool IsNested;
        public bool HasInstanceFields;
        public Location? InstanceFieldLocation;
        public int Order;
        public int ExecuteMethodCount;
        public Location? ExecuteLocation;
        public bool ExecuteIsStatic;
        public bool ExecuteIsGeneric;
        public bool ExecuteIsAsync;
        public List<ExecuteParameter>? Parameters;
        public List<ComponentTypeInfo>? ExcludedComponents;
        public List<string>? RunAfter;
        public List<string>? RunBefore;

        // Set after validation.
        public List<ExecuteParameter>? ValidComponentParameters;
        public List<ExecuteParameter>? OptionalComponentParameters;
        public bool HasDeltaTime;
        public bool HasEntityId;
        public bool HasEntityCommands;

        public bool Equals(EntitySystemModel? other)
        {
            if (other is null) return false;
            if (ReferenceEquals(this, other)) return true;
            return StructName == other.StructName
                && Namespace == other.Namespace
                && FullName == other.FullName
                && IsPartial == other.IsPartial
                && IsNested == other.IsNested
                && HasInstanceFields == other.HasInstanceFields
                && Order == other.Order
                && ExecuteMethodCount == other.ExecuteMethodCount
                && ExecuteIsStatic == other.ExecuteIsStatic
                && ExecuteIsGeneric == other.ExecuteIsGeneric
                && ExecuteIsAsync == other.ExecuteIsAsync
                && HasDeltaTime == other.HasDeltaTime
                && HasEntityId == other.HasEntityId
                && HasEntityCommands == other.HasEntityCommands
                && SequenceEqual(Parameters, other.Parameters)
                && SequenceEqual(ExcludedComponents, other.ExcludedComponents)
                && StringListEqual(RunAfter, other.RunAfter)
                && StringListEqual(RunBefore, other.RunBefore);
        }

        public override bool Equals(object? obj) => Equals(obj as EntitySystemModel);

        public override int GetHashCode()
        {
            // Hash based on identity-defining fields only.
            int hash = FullName.GetHashCode();
            hash = hash * 31 + Order;
            hash = hash * 31 + ExecuteMethodCount;
            hash = hash * 31 + (Parameters?.Count ?? 0);
            hash = hash * 31 + (HasEntityCommands ? 1 : 0);
            hash = hash * 31 + (RunAfter?.Count ?? 0);
            hash = hash * 31 + (RunBefore?.Count ?? 0);
            return hash;
        }

        private static bool SequenceEqual<T>(List<T>? a, List<T>? b) where T : IEquatable<T>
        {
            if (a is null && b is null) return true;
            if (a is null || b is null) return false;
            if (a.Count != b.Count) return false;
            for (int i = 0; i < a.Count; i++)
                if (!a[i].Equals(b[i])) return false;
            return true;
        }

        private static bool StringListEqual(List<string>? a, List<string>? b)
        {
            if (a is null && b is null) return true;
            if (a is null || b is null) return false;
            if (a.Count != b.Count) return false;
            for (int i = 0; i < a.Count; i++)
                if (a[i] != b[i]) return false;
            return true;
        }
    }

    internal sealed class ExecuteParameter : IEquatable<ExecuteParameter>
    {
        public string Name = "";
        public string TypeName = "";
        public string? TypeNamespace;
        public string FullTypeName = "";
        public RefKind RefKind;
        public ParameterRole Role;
        public bool ImplementsIComponent;
        public bool IsUnmanaged;
        public bool HasStructLayout;
        public bool HasBoolFields;
        public string? InnerFullTypeName;
        public Location? Location;
        /// <summary>
        /// Non-null if the component type has [BuiltInComponent("...")].
        /// Contains the native C++ name for GetComponentTypeIdByName lookup.
        /// </summary>
        public string? BuiltInNativeName;
        /// <summary>
        /// Non-null for optional parameters whose inner type has [BuiltInComponent].
        /// </summary>
        public string? InnerBuiltInNativeName;

        public bool Equals(ExecuteParameter? other)
        {
            if (other is null) return false;
            return Name == other.Name
                && FullTypeName == other.FullTypeName
                && RefKind == other.RefKind
                && Role == other.Role
                && ImplementsIComponent == other.ImplementsIComponent
                && IsUnmanaged == other.IsUnmanaged
                && HasStructLayout == other.HasStructLayout
                && HasBoolFields == other.HasBoolFields
                && InnerFullTypeName == other.InnerFullTypeName
                && BuiltInNativeName == other.BuiltInNativeName
                && InnerBuiltInNativeName == other.InnerBuiltInNativeName;
        }

        public override bool Equals(object? obj) => Equals(obj as ExecuteParameter);
        public override int GetHashCode() => FullTypeName.GetHashCode();
    }

    internal sealed class ComponentTypeInfo : IEquatable<ComponentTypeInfo>
    {
        public string TypeName = "";
        public string? TypeNamespace;
        public string FullName = "";
        public string? BuiltInNativeName;

        public bool Equals(ComponentTypeInfo? other)
        {
            if (other is null) return false;
            return FullName == other.FullName
                && BuiltInNativeName == other.BuiltInNativeName;
        }

        public override bool Equals(object? obj) => Equals(obj as ComponentTypeInfo);
        public override int GetHashCode() => FullName.GetHashCode();
    }

    internal sealed class GameSystemModel : IEquatable<GameSystemModel>
    {
        public string ClassName = "";
        public string? Namespace;
        public string FullName = "";
        public Location? ClassLocation;
        public bool IsPartial;
        public bool IsNested;
        public bool HasPublicParameterlessConstructor;
        public int Order;
        public List<string>? RunAfter;
        public List<string>? RunBefore;

        public bool Equals(GameSystemModel? other)
        {
            if (other is null) return false;
            if (ReferenceEquals(this, other)) return true;
            return ClassName == other.ClassName
                && Namespace == other.Namespace
                && FullName == other.FullName
                && IsPartial == other.IsPartial
                && IsNested == other.IsNested
                && HasPublicParameterlessConstructor == other.HasPublicParameterlessConstructor
                && Order == other.Order
                && StringListEqual(RunAfter, other.RunAfter)
                && StringListEqual(RunBefore, other.RunBefore);
        }

        public override bool Equals(object? obj) => Equals(obj as GameSystemModel);

        public override int GetHashCode()
        {
            int hash = FullName.GetHashCode();
            hash = hash * 31 + Order;
            hash = hash * 31 + (RunAfter?.Count ?? 0);
            hash = hash * 31 + (RunBefore?.Count ?? 0);
            return hash;
        }

        private static bool StringListEqual(List<string>? a, List<string>? b)
        {
            if (a is null && b is null) return true;
            if (a is null || b is null) return false;
            if (a.Count != b.Count) return false;
            for (int i = 0; i < a.Count; i++)
                if (a[i] != b[i]) return false;
            return true;
        }
    }

    internal enum ParameterRole
    {
        Component,
        DeltaTime,
        EntityId,
        OptionalReadOnlyComponent,
        EntityCommands,
    }

    // -----------------------------------------------------------------------
    // Diagnostic descriptors
    // -----------------------------------------------------------------------

    internal static class Diagnostics
    {
        private const string Category = "GameEngine.ECS";

        public static readonly DiagnosticDescriptor GE0001_MustBePartialStruct = new(
            id: "GE0001",
            title: "IEntitySystem must be a partial struct",
            messageFormat: "'{0}' implements IEntitySystem but is not declared as 'partial struct'",
            category: Category,
            defaultSeverity: DiagnosticSeverity.Error,
            isEnabledByDefault: true);

        public static readonly DiagnosticDescriptor GE0002_NoExecuteMethod = new(
            id: "GE0002",
            title: "Execute method not found",
            messageFormat:
                "'{0}' implements IEntitySystem but does not declare an Execute method",
            category: Category,
            defaultSeverity: DiagnosticSeverity.Error,
            isEnabledByDefault: true);

        public static readonly DiagnosticDescriptor GE0003_NotUnmanaged = new(
            id: "GE0003",
            title: "Component parameter type is not unmanaged",
            messageFormat: "Component type '{0}' is not an unmanaged type",
            category: Category,
            defaultSeverity: DiagnosticSeverity.Error,
            isEnabledByDefault: true);

        public static readonly DiagnosticDescriptor GE0004_NotIComponent = new(
            id: "GE0004",
            title: "Component parameter type does not implement IComponent",
            messageFormat:
                "Parameter type '{0}' does not implement IComponent",
            category: Category,
            defaultSeverity: DiagnosticSeverity.Error,
            isEnabledByDefault: true);

        public static readonly DiagnosticDescriptor GE0005_MissingStructLayout = new(
            id: "GE0005",
            title: "Component struct missing [StructLayout]",
            messageFormat:
                "Component type '{0}' must have [StructLayout(LayoutKind.Sequential)] to ensure correct C#/C++ memory layout",
            category: Category,
            defaultSeverity: DiagnosticSeverity.Error,
            isEnabledByDefault: true);

        public static readonly DiagnosticDescriptor GE0006_NoComponentParameters = new(
            id: "GE0006",
            title: "Execute must have at least one required component parameter",
            messageFormat:
                "'{0}'.Execute must have at least one required component parameter (ref T or in T where T : IComponent)",
            category: Category,
            defaultSeverity: DiagnosticSeverity.Error,
            isEnabledByDefault: true);

        public static readonly DiagnosticDescriptor GE0007_NoDeltaTime = new(
            id: "GE0007",
            title: "No deltaTime parameter",
            messageFormat:
                "'{0}'.Execute has no 'float deltaTime' parameter (unusual for a per-frame system)",
            category: Category,
            defaultSeverity: DiagnosticSeverity.Warning,
            isEnabledByDefault: true);

        public static readonly DiagnosticDescriptor GE0008_MultipleExecuteMethods = new(
            id: "GE0008",
            title: "Multiple Execute methods found",
            messageFormat:
                "'{0}' declares multiple Execute methods; IEntitySystem requires exactly one",
            category: Category,
            defaultSeverity: DiagnosticSeverity.Error,
            isEnabledByDefault: true);

        public static readonly DiagnosticDescriptor GE0009_ExecuteMustNotBeGeneric = new(
            id: "GE0009",
            title: "Execute must not be generic",
            messageFormat: "'{0}'.Execute must not be a generic method",
            category: Category,
            defaultSeverity: DiagnosticSeverity.Error,
            isEnabledByDefault: true);

        public static readonly DiagnosticDescriptor GE0010_ExecuteMustNotBeStatic = new(
            id: "GE0010",
            title: "Execute must not be static",
            messageFormat: "'{0}'.Execute must be an instance method, not static",
            category: Category,
            defaultSeverity: DiagnosticSeverity.Error,
            isEnabledByDefault: true);

        public static readonly DiagnosticDescriptor GE0011_ExecuteMustNotBeAsync = new(
            id: "GE0011",
            title: "Execute must not be async",
            messageFormat: "'{0}'.Execute must not be async",
            category: Category,
            defaultSeverity: DiagnosticSeverity.Error,
            isEnabledByDefault: true);

        public static readonly DiagnosticDescriptor GE0012_ComponentByValue = new(
            id: "GE0012",
            title: "Component parameter passed by value",
            messageFormat:
                "Parameter '{0}' of type '{1}' is passed by value; use 'ref' for read-write or 'in' for read-only access",
            category: Category,
            defaultSeverity: DiagnosticSeverity.Error,
            isEnabledByDefault: true);

        public static readonly DiagnosticDescriptor GE0013_MustNotBeNested = new(
            id: "GE0013",
            title: "IEntitySystem must not be a nested type",
            messageFormat: "'{0}' implements IEntitySystem but is a nested type",
            category: Category,
            defaultSeverity: DiagnosticSeverity.Error,
            isEnabledByDefault: true);

        public static readonly DiagnosticDescriptor GE0014_BoolField = new(
            id: "GE0014",
            title: "Component struct contains bool field",
            messageFormat:
                "Component type '{0}' contains a bool field; use byte instead for C#/C++ interop safety",
            category: Category,
            defaultSeverity: DiagnosticSeverity.Warning,
            isEnabledByDefault: true);

        public static readonly DiagnosticDescriptor GE0015_DuplicateComponent = new(
            id: "GE0015",
            title: "Duplicate component type in Execute parameters",
            messageFormat:
                "Component type '{0}' appears multiple times in Execute parameters",
            category: Category,
            defaultSeverity: DiagnosticSeverity.Error,
            isEnabledByDefault: true);

        public static readonly DiagnosticDescriptor GE0016_InstanceFields = new(
            id: "GE0016",
            title: "IEntitySystem struct declares instance fields",
            messageFormat:
                "'{0}' declares instance fields which will be zero-initialized each frame; consider using static fields instead",
            category: Category,
            defaultSeverity: DiagnosticSeverity.Warning,
            isEnabledByDefault: true);

        // GE0017/GE0019 are CODEGEN constraints, not runtime ones: the generated
        // registration is a `partial class` at file scope, so a non-partial or nested
        // subclass simply cannot receive one. Such a system still runs — GameSystemRunner's
        // reflection discovery finds it and stamps its ALC — but a discovered entry is
        // built with null runAfter/runBefore, so [After]/[Before] are silently dropped.
        // Warning, therefore, not error: erroring would fail the build for a shape the
        // runtime fully supports. GE0018 stays an error because neither path can construct
        // the type at all.
        public static readonly DiagnosticDescriptor GE0017_GameSystemMustBePartialClass = new(
            id: "GE0017",
            title: "GameSystem subclass is not registered at startup (not partial)",
            messageFormat:
                "'{0}' extends GameSystem but is not declared 'partial', so no registration is generated: "
                + "it is found by reflection discovery instead and its [After]/[Before] attributes are ignored. "
                + "Declare it 'partial class' to register it.",
            category: Category,
            defaultSeverity: DiagnosticSeverity.Warning,
            isEnabledByDefault: true);

        public static readonly DiagnosticDescriptor GE0018_GameSystemNoParameterlessConstructor = new(
            id: "GE0018",
            title: "GameSystem subclass must have a public parameterless constructor",
            messageFormat:
                "'{0}' extends GameSystem but does not have a public parameterless constructor",
            category: Category,
            defaultSeverity: DiagnosticSeverity.Error,
            isEnabledByDefault: true);

        public static readonly DiagnosticDescriptor GE0019_GameSystemMustNotBeNested = new(
            id: "GE0019",
            title: "GameSystem subclass is not registered at startup (nested type)",
            messageFormat:
                "'{0}' extends GameSystem but is declared inside another type, so no registration is generated: "
                + "it is found by reflection discovery instead and its [After]/[Before] attributes are ignored. "
                + "Move it to file scope to register it.",
            category: Category,
            defaultSeverity: DiagnosticSeverity.Warning,
            isEnabledByDefault: true);

        public static readonly DiagnosticDescriptor GE0020_OptionalNotIComponent = new(
            id: "GE0020",
            title: "ReadOnlyOptional inner type does not implement IComponent",
            messageFormat:
                "ReadOnlyOptional inner type '{0}' does not implement IComponent",
            category: Category,
            defaultSeverity: DiagnosticSeverity.Error,
            isEnabledByDefault: true);

        public static readonly DiagnosticDescriptor GE0021_DuplicateRequiredOptional = new(
            id: "GE0021",
            title: "Component type appears as both required and optional",
            messageFormat:
                "Component type '{0}' appears as both a required and optional parameter",
            category: Category,
            defaultSeverity: DiagnosticSeverity.Error,
            isEnabledByDefault: true);

        public static readonly DiagnosticDescriptor GE0022_OptionalByRef = new(
            id: "GE0022",
            title: "ReadOnlyOptional must not be passed by ref or in",
            messageFormat:
                "ReadOnlyOptional<{0}> must be passed by value, not by ref or in",
            category: Category,
            defaultSeverity: DiagnosticSeverity.Error,
            isEnabledByDefault: true);

        public static readonly DiagnosticDescriptor GE0023_EntityCommandsByRef = new(
            id: "GE0023",
            title: "EntityCommands must not be passed by ref or in",
            messageFormat:
                "EntityCommands must be passed by value, not by ref or in",
            category: Category,
            defaultSeverity: DiagnosticSeverity.Error,
            isEnabledByDefault: true);

        public static readonly DiagnosticDescriptor GE0024_MultipleEntityCommands = new(
            id: "GE0024",
            title: "Multiple EntityCommands parameters",
            messageFormat:
                "'{0}'.Execute has multiple EntityCommands parameters; only one is allowed",
            category: Category,
            defaultSeverity: DiagnosticSeverity.Error,
            isEnabledByDefault: true);

        public static readonly DiagnosticDescriptor GE0025_ComponentFieldSkipped = new(
            id: "GE0025",
            title: "Component field excluded from the exported schema",
            messageFormat:
                "Component '{0}' field '{1}' of type '{2}' has no schema mapping and is excluded "
                + "from the inspector, scene serialization, and hot-reload migration (its bytes "
                + "still exist but read as an opaque region)",
            category: Category,
            defaultSeverity: DiagnosticSeverity.Warning,
            isEnabledByDefault: true);

        public static readonly DiagnosticDescriptor GE0026_ComponentSchemaUnavailable = new(
            id: "GE0026",
            title: "Component has no exportable field schema",
            messageFormat:
                "Component '{0}' has no exportable field schema (generic, not unmanaged, or "
                + "LayoutKind.Auto); it will not appear with editable fields in the inspector "
                + "and will not persist to .scene files",
            category: Category,
            defaultSeverity: DiagnosticSeverity.Warning,
            isEnabledByDefault: true);
    }
}
