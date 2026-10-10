using System.Text;
using System.Text.RegularExpressions;

namespace GameEngine.ComponentScanner;

// Lightweight scanner that finds C++ ECS component structs in
// namespace GameEngine::Components and emits self-registering reflection blocks.
//
// For each reflected struct it emits the cap-free GE_REGISTER_COMPONENT_BEGIN /
// GE_REGISTER_COMPONENT_FIELD / GE_REGISTER_COMPONENT_END form (one FIELD line per
// member) rather than the variadic GE_REGISTER_COMPONENT(Type, f1, ...), so a
// component may have any number of reflected fields. The C++ side deduces each
// field's type at compile time via FieldTypeIdOf<decltype(Type::field)>, so the
// registration names fields only; the scanner conservatively skips structs whose
// fields look non-POD (pointers, references, templates, std::, virtual) or whose
// fields are hidden behind an unexpanded field-injection macro.
//
// The same parse also feeds the web library's types: --emit-dts writes a TypeScript
// declaration file and --emit-json the same model as JSON (WebComponentModel), with
// each field's kind resolved from its type text by FieldKindResolver.
//
// Multiple --in roots may be passed (e.g. the core Engine/Include/Components plus
// each Engine/Modules/<X>/Include/Components); include paths are computed relative
// to each root's "Include" dir, and structs already seen from an earlier root are
// de-duplicated by fully-qualified name.

internal static class Program
{
    private const string kComponentNamespace = "GameEngine::Components";

    // CRTP base for per-entity systems (ECS::EntitySystem<Self>). Its derivation of SystemBase is
    // invisible to the scanner (it only sees the direct base), so it's matched by name alongside
    // the configured --detect-system-base.
    private const string kEntitySystemBase = "EntitySystem";

    internal static int Main(string[] args)
    {
        var inDirs = new List<string>();
        string? outFile = null;
        string? dtsFile = null;
        string? jsonFile = null;
        bool dryRun = false;
        // When set, a struct is a component if it inherits a base with this simple name
        // (e.g. "ComponentBase" for `: ECS::ComponentBase`), regardless of namespace. When
        // null, detection falls back to the engine rule: structs in GameEngine::Components.
        string? detectBase = null;
        // When set (e.g. "SystemBase" for `: ECS::SystemBase`), the scanner runs in SYSTEM mode:
        // it detects system structs by that base name and emits a UserSystemRegistration TU
        // instead of component reflection. Mutually exclusive with the component scan (a separate
        // invocation per kind). No field extraction / POD-skip applies — a system isn't a POD.
        string? systemBase = null;

        for (int i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--in":
                    inDirs.Add(RequireValue(args, ref i, "--in"));
                    break;
                case "--out":
                    outFile = RequireValue(args, ref i, "--out");
                    break;
                case "--emit-dts":
                    dtsFile = RequireValue(args, ref i, "--emit-dts");
                    break;
                case "--emit-json":
                    jsonFile = RequireValue(args, ref i, "--emit-json");
                    break;
                case "--detect-base":
                    detectBase = RequireValue(args, ref i, "--detect-base");
                    break;
                case "--detect-system-base":
                    systemBase = RequireValue(args, ref i, "--detect-system-base");
                    break;
                case "--dry-run":
                    dryRun = true;
                    break;
                default:
                    Console.Error.WriteLine($"Unknown argument: {args[i]}");
                    return 2;
            }
        }

        if (inDirs.Count == 0)
            inDirs.Add("Engine/Include/Components");

        bool emitWeb = dtsFile != null || jsonFile != null;
        if (emitWeb && systemBase != null)
        {
            Console.Error.WriteLine("--emit-dts and --emit-json describe components; they do not apply with --detect-system-base.");
            return 2;
        }

        var reflected = new List<ReflectedStruct>();
        var systems = new List<ReflectedSystem>();
        var skipped = new List<SkippedStruct>();
        var headersWithComponents = new SortedSet<string>(StringComparer.Ordinal);
        var headersWithSystems = new SortedSet<string>(StringComparer.Ordinal);
        // De-dupe structs reflected from more than one root (by fully-qualified name).
        var seenQualified = new HashSet<string>(StringComparer.Ordinal);
        var includeRoots = new List<string>();

        // Enum declarations discovered across ALL scanned headers, keyed by simple (unqualified)
        // name, so a reflected field typed as an enum can be matched to its enumerators at emit time
        // (a component may use an enum declared in a different header). Two enums sharing a simple
        // name but with different members are marked ambiguous and dropped — their fields fall back
        // to integer serialization rather than risk a wrong name table.
        var enumsByName = new Dictionary<string, EnumDecl>(StringComparer.Ordinal);
        var ambiguousEnums = new HashSet<string>(StringComparer.Ordinal);

        // `using` aliases across all scanned headers, for the web emitters' kind resolution; an
        // alias declared twice with different targets is dropped.
        var aliases = new Dictionary<string, string>(StringComparer.Ordinal);
        // Simple names of every struct declared in the scanned headers, in any namespace: a field of
        // such a type with no reflected kind is Unknown in the registry too (WebOmissionReason).
        var knownStructs = new HashSet<string>(StringComparer.Ordinal);
        var ambiguousAliases = new HashSet<string>(StringComparer.Ordinal);

        foreach (string inDir in inDirs)
        {
            if (!Directory.Exists(inDir))
            {
                Console.Error.WriteLine($"Input directory does not exist: {inDir}");
                return 2;
            }

            // Engine mode resolves includes relative to Engine/Include (yielding e.g.
            // "Components/Transform.h"). User (--detect-base) mode has only the scanned
            // SourceDir on the generated DLL's include path, so emit includes relative to
            // it ("Foo.h", "Combat/Foo.h") — FindIncludeRoot's Engine/Components heuristic
            // would otherwise pick the parent of a SourceDir named "Components".
            string includeRoot = detectBase != null ? Path.GetFullPath(inDir) : FindIncludeRoot(inDir);
            includeRoots.Add(includeRoot);

            string[] headers = Directory
                .EnumerateFiles(inDir, "*.*", SearchOption.AllDirectories)
                .Where(f => IsHeaderExtension(Path.GetExtension(f)))
                .ToArray();
            Array.Sort(headers, StringComparer.OrdinalIgnoreCase);

            foreach (string header in headers)
            {
                string raw;
                try
                {
                    raw = File.ReadAllText(header);
                }
                catch (Exception ex)
                {
                    Console.Error.WriteLine($"Failed to read {header}: {ex.Message}");
                    continue;
                }

                string cleaned = CommentStripper.Strip(raw);
                var structs = StructParser.Parse(cleaned);
                foreach (ParsedStruct parsed in structs)
                    knownStructs.Add(parsed.Name);

                // Collect enum declarations for name-based serialization of enum fields (component
                // modes only — a system isn't reflected). Last unique definition wins; a conflicting
                // redefinition of the same simple name is flagged ambiguous.
                if (systemBase == null)
                {
                    foreach (EnumDecl ed in EnumScanner.Scan(cleaned))
                    {
                        if (enumsByName.TryGetValue(ed.Name, out EnumDecl? existing))
                        {
                            if (!SameMembers(existing, ed))
                                ambiguousEnums.Add(ed.Name);
                        }
                        else
                        {
                            enumsByName[ed.Name] = ed;
                        }
                    }

                    foreach ((string name, string target) in TypeAliasScanner.Scan(cleaned))
                    {
                        if (aliases.TryGetValue(name, out string? existing) && existing != target)
                            ambiguousAliases.Add(name);
                        aliases[name] = target;
                    }
                }

                // Marker comments ("// @ge-no-add", "// @ge-editor-only", "// @ge-readonly",
                // "// @ge-hidden", "// @ge-tooltip ...")
                // are captured from the RAW text because the comment stripper has already
                // removed them from `cleaned`. Split the raw text into lines once here and
                // reuse it for both marker scanning and per-struct/field attachment.
                string[] lines = raw.Replace("\r\n", "\n").Split('\n');
                MarkerLines markers = MarkerScanner.Scan(lines);

                string includePath = ToIncludePath(header, includeRoot);
                bool fileHadReflected = false;

                foreach (ParsedStruct s in structs)
                {
                    // System mode (--detect-system-base SystemBase): collect structs inheriting
                    // `: ... SystemBase` (global systems) OR `: ... EntitySystem<Self>` (per-entity
                    // systems — the CRTP base derives SystemBase, but the scanner only sees the
                    // direct base name, so match it explicitly). Both register identically: the
                    // generated UserSystemAdapter<T> ticks T::OnUpdate, which for a per-entity system
                    // is EntitySystem's inherited OnUpdate. No field extraction — a system holds
                    // behavior, not reflectable data.
                    if (systemBase != null)
                    {
                        if (!InheritsBase(s.Bases, systemBase) && !InheritsBase(s.Bases, kEntitySystemBase))
                            continue;
                        string sysQualified = string.IsNullOrEmpty(s.Namespace) ? s.Name : $"{s.Namespace}::{s.Name}";
                        if (!seenQualified.Add(sysQualified))
                            continue; // already seen from an earlier root
                        systems.Add(new ReflectedSystem(s.Name, s.Namespace, includePath));
                        headersWithSystems.Add(includePath);
                        continue;
                    }

                    // Engine mode (default): structs declared directly in GameEngine::Components.
                    // User mode (--detect-base ComponentBase): structs inheriting
                    // `: ... ComponentBase`, in any namespace.
                    bool isComponent = detectBase != null
                        ? InheritsBase(s.Bases, detectBase)
                        : s.Namespace == kComponentNamespace;
                    if (!isComponent)
                        continue;

                    string? skipReason = EvaluateSkip(s);
                    if (skipReason != null)
                    {
                        skipped.Add(new SkippedStruct(s.Name, includePath, skipReason));
                        continue;
                    }

                    string qualified = string.IsNullOrEmpty(s.Namespace) ? s.Name : $"{s.Namespace}::{s.Name}";
                    if (!seenQualified.Add(qualified))
                        continue; // already reflected from an earlier root

                    bool noAdd = MarkerAttachment.StructMarkerAttached(lines, s.DeclLine, markers.NoAdd);
                    bool nonSerializable = MarkerAttachment.StructMarkerAttached(lines, s.DeclLine, markers.DoNotSerialize);
                    bool editorOnly = MarkerAttachment.StructMarkerAttached(lines, s.DeclLine, markers.EditorOnly);

                    var fields = new List<ReflectedField>();
                    foreach (FieldDecl f in s.Fields)
                        fields.Add(ReadField(lines, f, markers));

                    reflected.Add(new ReflectedStruct(s.Name, s.Namespace, fields, includePath, noAdd, nonSerializable, editorOnly));
                    fileHadReflected = true;
                }

                if (fileHadReflected)
                    headersWithComponents.Add(includePath);
            }
        }

        // User-mode diagnostic: a macro-free `: <detectBase>` component authored in a .cpp
        // (not a header) is never registered — the scanner only reflects headers (the
        // generated TU #includes them by path) and there is no macro to self-register it.
        // Warn so the silent no-op becomes actionable ("my component never shows up").
        int sourceFileWarnings = 0;
        if (detectBase != null)
        {
            foreach (string inDir in inDirs)
            {
                foreach (string src in Directory.EnumerateFiles(inDir, "*.*", SearchOption.AllDirectories))
                {
                    if (!IsNonHeaderCppSource(Path.GetExtension(src)))
                        continue;
                    string cleaned;
                    try { cleaned = CommentStripper.Strip(File.ReadAllText(src)); }
                    catch { continue; }
                    foreach (string name in InheritingStructNames(cleaned, detectBase))
                    {
                        Console.Error.WriteLine(
                            $"warning: '{name}' inherits {detectBase} in {src} but the scanner only registers " +
                            "components declared in a header (.h/.hpp); move it to a header or use GE_REGISTER_COMPONENT.");
                        sourceFileWarnings++;
                    }
                }
            }
        }

        string generated;

        if (systemBase != null)
        {
            // System mode: emit one RegisterUserSystem line per detected ECS::SystemBase struct.
            generated = EmitSystemRegistrationCpp(systems, headersWithSystems.ToList());

            Console.WriteLine("=== ComponentScanner (system mode) ===");
            for (int i = 0; i < inDirs.Count; i++)
                Console.WriteLine($"Scanned root: {Path.GetFullPath(inDirs[i])}");
            Console.WriteLine($"Systems     : {systems.Count}");
            Console.WriteLine();
            Console.WriteLine("--- SYSTEMS ---");
            foreach (ReflectedSystem s in systems.OrderBy(s => s.Name, StringComparer.Ordinal))
                Console.WriteLine($"  {(string.IsNullOrEmpty(s.Namespace) ? s.Name : s.Namespace + "::" + s.Name)} [{s.Header}]");
            Console.WriteLine();
            Console.WriteLine($"ComponentScanner.summary systems={systems.Count}");
        }
        else
        {
            generated = EmitGeneratedCpp(reflected, OrderIncludes(reflected, headersWithComponents),
                detectBase != null, enumsByName, ambiguousEnums);

            int enumFieldCount = reflected.Sum(r => r.Fields.Count(f => MatchEnum(f.Type, enumsByName, ambiguousEnums) != null));

            // Report ------------------------------------------------------------
            Console.WriteLine("=== ComponentScanner ===");
            for (int i = 0; i < inDirs.Count; i++)
                Console.WriteLine($"Scanned root: {Path.GetFullPath(inDirs[i])}  (include root: {Path.GetFullPath(includeRoots[i])})");
            Console.WriteLine($"Reflected   : {reflected.Count}");
            Console.WriteLine($"Skipped     : {skipped.Count}");
            Console.WriteLine($"Enum fields : {enumFieldCount} (name-serialized)");
            Console.WriteLine();

            Console.WriteLine("--- REFLECTED ---");
            foreach (ReflectedStruct r in reflected.OrderBy(r => r.Name, StringComparer.Ordinal))
            {
                string flagged = string.Join(", ", r.Fields.Where(f => FieldFlagExpr(f) != null).Select(f => f.Name));
                string suffix = flagged.Length > 0 ? $"  [flags: {flagged}]" : "";
                Console.WriteLine($"  {r.Name} ({r.Fields.Count}){(r.NoAdd ? " NO_ADD" : "")}{(r.EditorOnly ? " EDITOR_ONLY" : "")}{suffix}");
            }
            Console.WriteLine();

            Console.WriteLine("--- SKIPPED ---");
            foreach (SkippedStruct sk in skipped.OrderBy(s => s.Name, StringComparer.Ordinal))
                Console.WriteLine($"  {sk.Name} [{sk.Header}] -- {sk.Reason}");
            Console.WriteLine();

            // Stable, locale-independent summary the native-build pipeline parses to surface
            // scan warnings (0 reflected / skipped structs / .cpp components) on the console.
            Console.WriteLine($"ComponentScanner.summary reflected={reflected.Count} skipped={skipped.Count} sourcewarnings={sourceFileWarnings}");
        }

        string? dts = null;
        string? json = null;
        if (emitWeb)
        {
            foreach (string name in ambiguousAliases)
                aliases.Remove(name);
            var errors = new List<string>();
            WebComponentModel? model = WebComponentModel.Build(reflected, aliases, enumsByName, ambiguousEnums,
                knownStructs, errors);
            if (model == null)
            {
                foreach (string error in errors)
                    Console.Error.WriteLine($"error: {error}");
                Console.Error.WriteLine("The web types cannot be generated; nothing was written.");
                return 1;
            }
            foreach (WebOmission o in model.Omitted)
                Console.Error.WriteLine($"warning: {o.Component}.{o.Field} (type '{o.Type}') is left out of the web types: " +
                    OmissionAdvice(o.Reason));
            dts = DtsEmitter.Emit(model);
            json = JsonEmitter.Emit(model);
            Console.WriteLine($"ComponentScanner.web components={model.Components.Count} " +
                $"fields={model.Components.Sum(c => c.Fields.Count)} enums={model.Enums.Count} omitted={model.Omitted.Count}");
        }

        if (dryRun)
        {
            Console.WriteLine("--- GENERATED (dry-run, not written) ---");
            Console.WriteLine(generated);
            return 0;
        }

        if (outFile == null)
        {
            Console.Error.WriteLine("No --out specified and not --dry-run; nothing to write.");
            return 2;
        }

        WriteOutput(outFile, generated);
        if (dtsFile != null)
            WriteOutput(dtsFile, dts!);
        if (jsonFile != null)
            WriteOutput(jsonFile, json!);
        return 0;
    }

    private static string OmissionAdvice(WebOmissionReason reason) => reason switch
    {
        WebOmissionReason.NoReflectedKind =>
            "a struct with no reflected kind. Give it a FieldTypeTraits specialization the scanner knows, or mark " +
            "the field // @ge-hidden if a page never reads it.",
        _ =>
            "the type is not declared in the scanned component headers, so the scanner cannot resolve its kind. " +
            "An enum must be declared under a scanned Components root (#3082); a platform-sized integer (long, " +
            "size_t) should be a fixed-width type; a struct needs a FieldTypeTraits specialization.",
    };

    private static void WriteOutput(string path, string text)
    {
        string? outDir = Path.GetDirectoryName(Path.GetFullPath(path));
        if (!string.IsNullOrEmpty(outDir))
            Directory.CreateDirectory(outDir);
        File.WriteAllText(path, text);
        Console.WriteLine($"Wrote {Path.GetFullPath(path)}");
    }

    private static string RequireValue(string[] args, ref int i, string flag)
    {
        if (i + 1 >= args.Length)
        {
            Console.Error.WriteLine($"Missing value for {flag}");
            Environment.Exit(2);
        }
        return args[++i];
    }

    // Read one field's markers: "// @ge-hidden", "// @ge-readonly", a field-level
    // "// [DoNotSerialize]" (FieldFlags::Transient: the scene serializer skips the field),
    // "// @ge-tooltip Text" and "// @ge-range Min [Max]". Attachment (which line belongs to
    // which field) is handled by MarkerAttachment.
    private static ReflectedField ReadField(string[] lines, FieldDecl field, MarkerLines markers)
    {
        bool hidden = MarkerAttachment.FieldMarkerAttached(lines, field.DeclLine, field.EndLine, markers.Hidden);
        bool readOnly = MarkerAttachment.FieldMarkerAttached(lines, field.DeclLine, field.EndLine, markers.ReadOnly);
        bool transient = MarkerAttachment.FieldMarkerAttached(lines, field.DeclLine, field.EndLine, markers.DoNotSerialize);
        string? tooltip = MarkerAttachment.FieldValueMarkerAttached(lines, field.DeclLine, field.EndLine, markers.Tooltips);
        FieldRange? range = MarkerAttachment.FieldValueMarkerAttached(lines, field.DeclLine, field.EndLine, markers.Ranges);
        return new ReflectedField(field.Name, field.Type, field.ArrayExtent, hidden, readOnly, transient,
            string.IsNullOrWhiteSpace(tooltip) ? null : tooltip, range);
    }

    // The C++ FieldFlags expression to emit via GE_REFLECT_FIELD_FLAGS, or null when the
    // field carries no policy marker.
    private static string? FieldFlagExpr(ReflectedField field)
    {
        var flags = new List<string>();
        if (field.Hidden)
            flags.Add("::GameEngine::ECS::FieldFlags::Hidden");
        if (field.ReadOnly)
            flags.Add("::GameEngine::ECS::FieldFlags::ReadOnly");
        if (field.Transient)
            flags.Add("::GameEngine::ECS::FieldFlags::Transient");
        return flags.Count == 0 ? null : string.Join(" | ", flags);
    }

    // The GE_REFLECT_FIELD_RANGE argument list ("Min, Max"); an omitted Max binds an open
    // maximum.
    private static string FieldRangeArgs(FieldRange range)
    {
        string max = range.Max.HasValue ? CppFloatLiteral(range.Max.Value) : "::std::numeric_limits<float>::infinity()";
        return $"{CppFloatLiteral(range.Min)}, {max}";
    }

    // "0" -> "0.0f", "1.5" -> "1.5f", "1E-05" -> "1E-05f": a float literal needs a
    // point or an exponent before the suffix.
    private static string CppFloatLiteral(double value)
    {
        string text = value.ToString("R", System.Globalization.CultureInfo.InvariantCulture);
        if (text.IndexOfAny(new[] { '.', 'e', 'E' }) < 0)
            text += ".0";
        return text + "f";
    }

    // Decide whether a struct should be skipped. Returns null to reflect it,
    // or a human-readable reason string to skip it. No field-count cap: the
    // emitted cap-free GE_REGISTER_COMPONENT_BEGIN/FIELD/END form has no arg limit.
    private static string? EvaluateSkip(ParsedStruct s)
    {
        if (s.HasVirtual)
            return "has a virtual member (non-POD)";
        if (s.UnexpandedMacro != null)
            return $"body uses unexpanded field-injection macro '{s.UnexpandedMacro}' (fields not visible without the preprocessor)";
        if (s.Fields.Count == 0)
            return "0 extractable fields";

        foreach (FieldDecl f in s.Fields)
        {
            string t = f.Type;
            if (t.Contains('*'))
                return $"field '{f.Name}' type '{t.Trim()}' contains a pointer (*)";
            if (t.Contains('&'))
                return $"field '{f.Name}' type '{t.Trim()}' contains a reference (&)";
            if (t.Contains('<'))
                return $"field '{f.Name}' type '{t.Trim()}' is templated (<...>)";
            // std:: is conservative against non-POD containers (std::vector, std::string,
            // std::unique_ptr, ...). Plain fixed-width integer/byte aliases (std::int32_t,
            // std::uint64_t, std::size_t, std::byte) are POD and reflect fine, so allow them.
            if (t.Contains("std::") && !IsPodStdAlias(t))
                return $"field '{f.Name}' type '{t.Trim()}' uses std:: (likely non-POD container)";
        }

        return null;
    }

    // True if every std:: token in the type is a known POD fixed-width / byte / size alias.
    private static bool IsPodStdAlias(string type)
    {
        var stdTokens = Regex.Matches(type, @"std::([A-Za-z_][A-Za-z0-9_]*)");
        if (stdTokens.Count == 0)
            return false;

        foreach (Match m in stdTokens)
        {
            string name = m.Groups[1].Value;
            bool ok =
                name is "int8_t" or "int16_t" or "int32_t" or "int64_t" or
                        "uint8_t" or "uint16_t" or "uint32_t" or "uint64_t" or
                        "size_t" or "ptrdiff_t" or "intptr_t" or "uintptr_t" or "byte";
            if (!ok)
                return false;
        }
        return true;
    }

    // True if the base-clause inherits a base whose simple (unqualified) name matches
    // baseName — e.g. InheritsBase("public ECS::ComponentBase", "ComponentBase") == true. Strips
    // access / virtual specifiers, namespace qualifiers, and any template arguments.
    // internal (not private) so the scanner unit tests can exercise it directly.
    internal static bool InheritsBase(string bases, string baseName)
    {
        if (string.IsNullOrWhiteSpace(bases))
            return false;
        foreach (string entry in bases.Split(','))
        {
            string[] tokens = entry.Split(new[] { ' ', '\t', '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
            string? type = null;
            foreach (string tok in tokens)
            {
                if (tok is "public" or "private" or "protected" or "virtual")
                    continue;
                type = tok; // specifiers stripped; the trailing token is the base type
            }
            if (type == null)
                continue;
            int lt = type.IndexOf('<');
            if (lt >= 0)
                type = type.Substring(0, lt);
            int ci = type.LastIndexOf("::", StringComparison.Ordinal);
            string simple = ci >= 0 ? type.Substring(ci + 2) : type;
            if (simple == baseName)
                return true;
        }
        return false;
    }

    // Header extensions the scanner reflects from: any of these can be #include'd by the
    // generated registration TU. The file watcher (NativeScriptManager) watches .h/.hpp,
    // so .hpp components must be detected too — not just .h.
    private static bool IsHeaderExtension(string ext) =>
        ext.Equals(".h", StringComparison.OrdinalIgnoreCase) ||
        ext.Equals(".hpp", StringComparison.OrdinalIgnoreCase) ||
        ext.Equals(".hxx", StringComparison.OrdinalIgnoreCase) ||
        ext.Equals(".hh", StringComparison.OrdinalIgnoreCase);

    // Non-header C++ translation units. A macro-free component here can't be reflected
    // (the generated TU #includes by header path), so it is diagnosed, not registered.
    private static bool IsNonHeaderCppSource(string ext) =>
        ext.Equals(".cpp", StringComparison.OrdinalIgnoreCase) ||
        ext.Equals(".cc", StringComparison.OrdinalIgnoreCase) ||
        ext.Equals(".cxx", StringComparison.OrdinalIgnoreCase);

    // Names of structs in `cleanedSource` whose base-clause inherits baseName. Used to warn
    // when a macro-free component is authored in a non-header file the scanner won't register.
    // internal so the scanner unit tests can exercise it directly.
    internal static List<string> InheritingStructNames(string cleanedSource, string baseName)
    {
        var names = new List<string>();
        foreach (ParsedStruct s in StructParser.Parse(cleanedSource))
            if (InheritsBase(s.Bases, baseName))
                names.Add(s.Name);
        return names;
    }

    // Order includes to mirror the hand-written ComponentReflection.cpp: keep the
    // canonical leading order for the well-known headers, then any remaining ones
    // alphabetically. This keeps a stable, readable generated file.
    private static List<string> OrderIncludes(List<ReflectedStruct> reflected, SortedSet<string> headers)
    {
        string[] preferred =
        {
            "Components/Transform.h",
            "Components/Hierarchy.h",
            "Components/Name.h",
            "Components/Animation/AnimatorRef.h",
            "Components/Rendering/RenderLayer.h",
            "Components/Rendering/Camera.h",
            "Components/Rendering/Light.h",
        };

        var ordered = new List<string>();
        foreach (string p in preferred)
            if (headers.Contains(p))
                ordered.Add(p);

        foreach (string h in headers)
            if (!ordered.Contains(h))
                ordered.Add(h);

        return ordered;
    }

    private static string EmitGeneratedCpp(List<ReflectedStruct> reflected, List<string> includes, bool userMode,
        Dictionary<string, EnumDecl> enums, HashSet<string> ambiguousEnums)
    {
        var byHeader = reflected
            .GroupBy(r => r.Header)
            .ToDictionary(g => g.Key, g => g.ToList());

        var sb = new StringBuilder();
        sb.AppendLine("// AUTO-GENERATED by ComponentScanner -- do not edit by hand.");
        sb.AppendLine("// Regenerate by running the ComponentScanner tool over the component headers.");
        sb.AppendLine("// One cap-free GE_REGISTER_COMPONENT_BEGIN/_FIELD/_END block per reflected");
        sb.AppendLine("// component struct (+ GE_REFLECT_FIELD_FLAGS / GE_REFLECT_FIELD_TOOLTIP metadata).");
        sb.AppendLine();
        // User DLLs use the PCH-light registration header: the same
        // GE_REGISTER_COMPONENT_* macros, but the factory adds via type-erased bytes
        // (no World::AddComponentImmediate<T> template instantiation), so a user .cpp
        // never pulls the heavy World template surface. The engine's own generated TU
        // keeps the typed path.
        sb.AppendLine(userMode
            ? "#include \"Components/ComponentRegistrationLite.h\"  // GE_REGISTER_COMPONENT_BEGIN/_FIELD/_END (type-erased)"
            : "#include \"Components/ComponentRegistration.h\"  // GE_REGISTER_COMPONENT_BEGIN/_FIELD/_END");
        sb.AppendLine();

        foreach (string inc in includes)
        {
            if (byHeader.TryGetValue(inc, out var structs) && structs.Count > 0)
                sb.AppendLine($"#include \"{inc}\"  // {string.Join(", ", structs.Select(r => r.Name))}");
            else
                sb.AppendLine($"#include \"{inc}\"");
        }

        sb.AppendLine();

        // Enum tables: one constexpr name<->value table per enum actually used by a reflected field,
        // emitted before the component blocks (the GE_REFLECT_ENUM_FIELD lines below reference
        // GeEnumTables::<EnumName>). Sorted by name for a stable generated file.
        var usedEnums = new SortedDictionary<string, EnumDecl>(StringComparer.Ordinal);
        foreach (ReflectedStruct r in reflected)
            foreach (ReflectedField f in r.Fields)
            {
                EnumDecl? ed = MatchEnum(f.Type, enums, ambiguousEnums);
                if (ed != null)
                    usedEnums[ed.Name] = ed;
            }

        if (usedEnums.Count > 0)
        {
            sb.AppendLine("// --- enum value-name tables (constexpr; bound to fields below) ---");
            foreach (EnumDecl ed in usedEnums.Values)
            {
                sb.AppendLine($"GE_REFLECT_ENUM_TABLE_BEGIN({ed.Name})");
                foreach (EnumMember m in ed.Members)
                    sb.AppendLine($"    GE_REFLECT_ENUM_TABLE_VALUE(\"{m.Name}\", {m.Value})");
                sb.AppendLine("GE_REFLECT_ENUM_TABLE_END()");
            }
            sb.AppendLine();
        }

        foreach (string inc in includes)
        {
            if (!byHeader.TryGetValue(inc, out var structs))
                continue;

            foreach (ReflectedStruct r in structs)
            {
                string qualified = string.IsNullOrEmpty(r.Namespace) ? r.Name : $"{r.Namespace}::{r.Name}";
                sb.Append(FormatRegisterBlock(qualified, r, enums, ambiguousEnums));
                sb.AppendLine();
            }
        }

        // Trim the trailing blank line for a clean file.
        return sb.ToString().TrimEnd() + "\n";
    }

    // Emit one cap-free registration block: a BEGIN, one FIELD per member, an END
    // (addable) or END_NO_ADD (kept out of the Add Component menu), followed by a
    // GE_REFLECT_FIELD_FLAGS / GE_REFLECT_FIELD_TOOLTIP lines for fields carrying metadata markers.
    private static string FormatRegisterBlock(string qualified, ReflectedStruct r,
        Dictionary<string, EnumDecl> enums, HashSet<string> ambiguousEnums)
    {
        string endMacro = r.NoAdd ? "GE_REGISTER_COMPONENT_END_NO_ADD" : "GE_REGISTER_COMPONENT_END";

        var sb = new StringBuilder();
        sb.AppendLine($"GE_REGISTER_COMPONENT_BEGIN({qualified})");
        foreach (ReflectedField f in r.Fields)
            sb.AppendLine($"    GE_REGISTER_COMPONENT_FIELD({qualified}, {f.Name})");
        sb.AppendLine($"{endMacro}({qualified})");

        foreach (ReflectedField f in r.Fields)
            if (FieldFlagExpr(f) is string flagExpr)
                sb.AppendLine($"GE_REFLECT_FIELD_FLAGS({qualified}, {f.Name}, {flagExpr});");

        foreach (ReflectedField f in r.Fields)
            if (f.Tooltip != null)
                sb.AppendLine($"GE_REFLECT_FIELD_TOOLTIP({qualified}, {f.Name}, {CppStringLiteral(f.Tooltip)});");

        foreach (ReflectedField f in r.Fields)
            if (f.Range != null)
                sb.AppendLine($"GE_REFLECT_FIELD_RANGE({qualified}, {f.Name}, {FieldRangeArgs(f.Range)});");

        if (r.DoNotSerialize)
            sb.AppendLine($"GE_REFLECT_COMPONENT_DONOTSERIALIZE({qualified});");

        if (r.EditorOnly)
            sb.AppendLine($"GE_REFLECT_COMPONENT_EDITORONLY({qualified});");

        // Bind enum-typed fields to their value-name table (emitted above) for name serialization.
        foreach (ReflectedField f in r.Fields)
        {
            EnumDecl? ed = MatchEnum(f.Type, enums, ambiguousEnums);
            if (ed != null)
                sb.AppendLine($"GE_REFLECT_ENUM_FIELD({qualified}, {f.Name}, {ed.Name})");
        }

        return sb.ToString();
    }

    private static string CppStringLiteral(string value)
    {
        var sb = new StringBuilder();
        sb.Append('"');
        foreach (char c in value)
        {
            switch (c)
            {
                case '\\':
                    sb.Append(@"\\");
                    break;
                case '"':
                    sb.Append("\\\"");
                    break;
                case '\n':
                    sb.Append(@"\n");
                    break;
                case '\r':
                    sb.Append(@"\r");
                    break;
                case '\t':
                    sb.Append(@"\t");
                    break;
                default:
                    if (char.IsControl(c))
                        sb.AppendFormat("\\{0:000}", (int)c);
                    else
                        sb.Append(c);
                    break;
            }
        }
        sb.Append('"');
        return sb.ToString();
    }

    // Resolve a field's C++ type string to a scanned enum declaration, or null if it isn't a known
    // (non-ambiguous) enum. Strips qualifiers/namespace: the last whitespace-separated token, then
    // its last "::" segment — so "const Foo::Bar" matches enum "Bar".
    internal static EnumDecl? MatchEnum(string fieldType, Dictionary<string, EnumDecl> enums, HashSet<string> ambiguousEnums)
    {
        if (string.IsNullOrWhiteSpace(fieldType))
            return null;
        string[] toks = fieldType.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
        string last = toks.Length > 0 ? toks[^1] : fieldType.Trim();
        int ci = last.LastIndexOf("::", StringComparison.Ordinal);
        string simple = ci >= 0 ? last.Substring(ci + 2) : last;
        if (ambiguousEnums.Contains(simple))
            return null;
        return enums.TryGetValue(simple, out EnumDecl? ed) ? ed : null;
    }

    // Two enum declarations are the same if their enumerators match in order, name, and value.
    private static bool SameMembers(EnumDecl a, EnumDecl b)
    {
        if (a.Members.Count != b.Members.Count)
            return false;
        for (int i = 0; i < a.Members.Count; i++)
            if (a.Members[i].Name != b.Members[i].Name || a.Members[i].Value != b.Members[i].Value)
                return false;
        return true;
    }

    // Emit the user-system registration TU: include <GameSDK/System.h> + each header declaring a
    // system, then a file-scope static per system that wraps it in a UserSystemAdapter<T> and
    // registers it at module load. Registration is non-owning (the adapter leaks with the
    // never-unloaded user DLL); the engine's bridge ticks the registry during play mode.
    private static string EmitSystemRegistrationCpp(List<ReflectedSystem> systems, List<string> includes)
    {
        var byHeader = systems.GroupBy(s => s.Header).ToDictionary(g => g.Key, g => g.ToList());

        var sb = new StringBuilder();
        sb.AppendLine("// AUTO-GENERATED by ComponentScanner --detect-system-base -- do not edit by hand.");
        sb.AppendLine("// One RegisterUserSystem(new UserSystemAdapter<T>) per ECS::SystemBase-derived user");
        sb.AppendLine("// system; the adapter forwards the system's optional OnStart/OnUpdate/OnDestroy.");
        sb.AppendLine();
        sb.AppendLine("#include \"GameSDK/System.h\"  // UserSystemAdapter + RegisterUserSystem + the typed Query API");
        sb.AppendLine();

        foreach (string inc in includes)
            sb.AppendLine($"#include \"{inc}\"  // {string.Join(", ", byHeader[inc].Select(s => s.Name))}");
        sb.AppendLine();

        sb.AppendLine("namespace {");
        int n = 0;
        foreach (string inc in includes)
        {
            foreach (ReflectedSystem s in byHeader[inc])
            {
                string q = string.IsNullOrEmpty(s.Namespace) ? s.Name : $"{s.Namespace}::{s.Name}";
                sb.AppendLine($"[[maybe_unused]] const bool ge_user_system_{n++} =");
                sb.AppendLine($"    ::GameEngine::NativeScripting::RegisterUserSystem(");
                sb.AppendLine($"        new ::GameEngine::ECS::UserSystemAdapter<{q}>(\"{q}\"));");
            }
        }
        sb.AppendLine("} // namespace");

        return sb.ToString().TrimEnd() + "\n";
    }

    // Locate "Engine/Include" by walking up from the scanned directory so include
    // paths can be made relative to it (e.g. "Components/Rendering/Camera.h"). For a
    // module dir (e.g. Modules/HeightFog/Include/Components) the "Include whose parent
    // is Engine" check fails; the fallback returns the parent of the nearest
    // "Components" dir, which is that module's Include root.
    private static string FindIncludeRoot(string scanDir)
    {
        var dir = new DirectoryInfo(Path.GetFullPath(scanDir));
        while (dir != null)
        {
            if (string.Equals(dir.Name, "Include", StringComparison.OrdinalIgnoreCase) &&
                dir.Parent != null &&
                string.Equals(dir.Parent.Name, "Engine", StringComparison.OrdinalIgnoreCase))
            {
                return dir.FullName;
            }
            dir = dir.Parent;
        }
        // Fallback: the parent of "Components" if present, else the scan dir itself.
        var d = new DirectoryInfo(Path.GetFullPath(scanDir));
        while (d != null)
        {
            if (string.Equals(d.Name, "Components", StringComparison.OrdinalIgnoreCase) && d.Parent != null)
                return d.Parent.FullName;
            d = d.Parent;
        }
        return Path.GetFullPath(scanDir);
    }

    private static string ToIncludePath(string headerFullPath, string includeRoot)
    {
        string rel = Path.GetRelativePath(includeRoot, headerFullPath);
        return rel.Replace('\\', '/');
    }
}

// One reflected field: its member name, its C++ type string (used to match enum fields to a
// scanned enum declaration and to resolve the web kind), its array extent text (null when not an
// array), and its markers: the FieldFlags policy (Hidden / ReadOnly / Transient), the tooltip and
// the inspector range (null when unmarked).
internal sealed record ReflectedField(string Name, string Type, string? ArrayExtent, bool Hidden, bool ReadOnly,
    bool Transient, string? Tooltip, FieldRange? Range);

internal sealed record ReflectedStruct(string Name, string Namespace, List<ReflectedField> Fields, string Header, bool NoAdd, bool DoNotSerialize, bool EditorOnly);

internal sealed record SkippedStruct(string Name, string Header, string Reason);

// One detected user system: an ECS::SystemBase-derived struct. Unlike a component it carries no
// reflected fields (a system isn't a POD — it may hold arbitrary members); only the type name +
// header are needed to emit its RegisterUserSystem line.
internal sealed record ReflectedSystem(string Name, string Namespace, string Header);
