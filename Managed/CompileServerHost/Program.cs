using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Pipes;
using System.Linq;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Runtime.Versioning;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Collections.Immutable;
using System.Reflection;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

[assembly: SupportedOSPlatform("windows")]
[assembly: SupportedOSPlatform("linux")]
[assembly: SupportedOSPlatform("macOS")]
namespace GameEngine.CompileServerHost;

// References: extra absolute reference paths beyond the TPA + engine set (P1 packages:
// a package assembly referencing its dependency packages' assemblies). Included in the
// reference-cache key. Defines: preprocessor symbols for this workspace's parse options
// (package manifest `defines` + propagated dependency defines). AllowUnsafe,
// ImplicitUsings, Nullable: the generated csproj's AllowUnsafeBlocks, ImplicitUsings
// and Nullable, so the live compile matches `dotnet build` of that csproj; absent
// means false, which is how a request from before these fields compiles.
internal record BuildRequest(string ProjectRoot, string[] ChangedFiles, string[] AffectedFiles, string[] AllFiles, string PreferredStrategy, bool ForceFull, string? Config = null, string? Tfm = null, string? EngineBinDir = null, string? AssemblyName = null, string[]? References = null, string[]? Defines = null, bool AllowUnsafe = false, bool ImplicitUsings = false, bool Nullable = false);
internal record Diagnostic(string Severity, string Code, string FileUtf8, int Line, int Column, string MessageUtf8);
internal record BuildResponse(bool Success, Diagnostic[] Warnings, Diagnostic[] Errors, byte[]? AssemblyBytes, byte[]? PdbBytes);

internal class Program
{
	// Shared parse options instance — must be the same object across all syntax trees
	// and generator driver invocations to avoid Roslyn's "Inconsistent language versions" error.
	private static readonly CSharpParseOptions s_ParseOptions = new(LanguageVersion.Preview);
	private static string GetLogPath()
	{
		var editorLog = Environment.GetEnvironmentVariable("GE_LOGFILE");
		var logDirectory = string.IsNullOrWhiteSpace(editorLog)
			? Path.Combine(Path.GetTempPath(), "GameEngine")
			: Path.GetDirectoryName(editorLog);
		if (string.IsNullOrWhiteSpace(logDirectory))
			logDirectory = Path.Combine(Path.GetTempPath(), "GameEngine");
		try { Directory.CreateDirectory(logDirectory); } catch { }
		return Path.Combine(logDirectory, "CompileServerHost.log");
	}

	private sealed class WorkspaceState
	{
		public Dictionary<string, SyntaxTree> Trees = new(StringComparer.OrdinalIgnoreCase);
		public GeneratorDriver? Driver;
		// Per-workspace parse options: s_ParseOptions plus the request's preprocessor
		// symbols. ALL trees and the generator driver of a workspace must share one
		// options instance (Roslyn rejects mixed language versions), so a defines
		// change resets the tree cache and the driver.
		public CSharpParseOptions ParseOptions = s_ParseOptions;
		public string DefinesKey = "";
		// The implicit global usings parsed with ParseOptions; reset with it.
		public SyntaxTree? ImplicitUsingsTree;
	}

	// The global usings `dotnet build` adds for <ImplicitUsings>enable</ImplicitUsings>
	// under Microsoft.NET.Sdk (Microsoft.NET.Sdk.CSharp.props), written out by hand.
	// `__status__` reports the list; PackageCompileE2E.GeneratedCsprojAndServerCompileTheSameModule
	// compares it with the GlobalUsings.g.cs the installed SDK generates, both ways.
	private static readonly string[] s_ImplicitUsings =
	{
		"System",
		"System.Collections.Generic",
		"System.IO",
		"System.Linq",
		"System.Net.Http",
		"System.Threading",
		"System.Threading.Tasks",
	};

	private static SyntaxTree GetImplicitUsingsTree(WorkspaceState ws)
	{
		if (ws.ImplicitUsingsTree is null)
		{
			var text = string.Concat(s_ImplicitUsings.Select(ns => $"global using global::{ns};\n"));
			ws.ImplicitUsingsTree = CSharpSyntaxTree.ParseText(text, ws.ParseOptions, "GameEngine.ImplicitUsings.g.cs", Encoding.UTF8);
		}
		return ws.ImplicitUsingsTree;
	}

	// Returns the workspace's parse options for this request, resetting cached trees
	// and the generator driver when the request's defines differ from what the cache
	// was parsed with.
	private static CSharpParseOptions ApplyWorkspaceDefines(WorkspaceState ws, BuildRequest request)
	{
		var defines = request.Defines ?? Array.Empty<string>();
		var definesKey = string.Join(";", defines);
		if (!string.Equals(ws.DefinesKey, definesKey, StringComparison.Ordinal))
		{
			ws.DefinesKey = definesKey;
			ws.ParseOptions = defines.Length > 0 ? s_ParseOptions.WithPreprocessorSymbols(defines) : s_ParseOptions;
			ws.Trees.Clear();
			ws.Driver = null;
			ws.ImplicitUsingsTree = null;
		}
		return ws.ParseOptions;
	}

	// Keyed on the engine bin dir the probe list is derived from, and populated only by
	// a discovery that found something. The host is resident and its pipe is keyed on
	// the workspace alone, so one process serves every EngineBinDir that opens the same
	// project (a Debug editor and a DebugFast one, say). A single process-global slot
	// would let the first request's discovery answer for all of them, so a request whose
	// bin dir stages no generator would pin "no generators" for the host's lifetime —
	// which leaves the generator-error gate nothing to gate: a driver that never runs
	// produces no generator diagnostics, and every later build reports success without
	// them.
	private static readonly Dictionary<string, IIncrementalGenerator[]> s_GeneratorsByBinDir =
		new(StringComparer.Ordinal);

	/// <summary>
	/// Discovers and loads IIncrementalGenerator instances from the SourceGenerators
	/// subdirectory next to the engine binaries or this process.
	/// </summary>
	private static IIncrementalGenerator[] TryLoadGenerators(string? engineBinDir)
	{
		string cacheKey = engineBinDir ?? "<none>";
		if (s_GeneratorsByBinDir.TryGetValue(cacheKey, out var cached))
			return cached;

		var generators = new List<IIncrementalGenerator>();

		string[] probePaths = new[]
		{
			engineBinDir is not null ? Path.Combine(engineBinDir, "SourceGenerators") : "",
			Path.Combine(AppContext.BaseDirectory, "SourceGenerators"),
		};

		foreach (var dir in probePaths)
		{
			if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir))
				continue;

			foreach (var dll in Directory.EnumerateFiles(dir, "*.dll"))
			{
				try
				{
					// Load from bytes to avoid locking the DLL file, which would
					// prevent recompiling the generator while CompileServerHost is running.
					var asmBytes = File.ReadAllBytes(dll);
					var asm = Assembly.Load(asmBytes);
					foreach (var type in asm.GetTypes())
					{
						if (type.IsAbstract || type.IsInterface)
							continue;
						if (typeof(IIncrementalGenerator).IsAssignableFrom(type))
						{
							var instance = (IIncrementalGenerator)Activator.CreateInstance(type)!;
							generators.Add(instance);
						}
					}
				}
				catch
				{
					// Skip assemblies that fail to load.
				}
			}

			// Stop after first successful directory.
			if (generators.Count > 0)
				break;
		}

		var loaded = generators.ToArray();
		// An empty result is a discovery that found nothing, not an answer about this
		// bin dir: the generator may simply not be staged yet. Re-probing costs two
		// Directory.Exists calls per compile, so leave it uncached and retry.
		if (loaded.Length > 0)
			s_GeneratorsByBinDir[cacheKey] = loaded;
		return loaded;
	}
	private static readonly Dictionary<string, WorkspaceState> s_Workspaces = new(StringComparer.OrdinalIgnoreCase);

	private static string s_ServerVersion = GameEngine.CompileServerHost.CompileServerBuildInfo.Version;
	private const string kShutdownMessage = "__shutdown__";
	// Followed by an assembly path: the host exits only when it runs from that path.
	private const string kShutdownRunningFromMessage = "__shutdown_running_from__ ";
	private static readonly DateTime s_StartTimeUtc = DateTime.UtcNow;
	private static DateTime s_LastActivityUtc = s_StartTimeUtc;

	/// <summary>
	/// Decide whether a given C# source file should be included in the compilation.
	///
	/// We explicitly drop MSBuild-generated sources that add assembly-level attributes
	/// or global using directives, since they are the root cause of CS0579 duplicate
	/// attribute errors when multiple obj/ directories leak into the AllFiles list
	/// provided by native clients.
	///
	/// NOTE: We intentionally do *not* blanket-filter paths containing "/bin/" here.
	/// Our native Editor currently runs from a bin directory (e.g. build/bin/Debug),
	/// and script assets live under that tree (build/bin/Debug/Assets).  Filtering
	/// on "/bin/" would therefore discard all user-authored scripts before Roslyn
	/// ever sees them, producing tiny stub assemblies with no user types and
	/// breaking InitializeOnLoad/IoL behavior.  Native clients are responsible for
	/// excluding their own MSBuild output trees (obj/bin) when they construct the
	/// AllFiles list.
	/// </summary>
	private static bool ShouldIncludeSource(string? path)
	{
		if (string.IsNullOrEmpty(path))
			return false;

		// Only compile .cs files
		if (!path.EndsWith(".cs", StringComparison.OrdinalIgnoreCase))
			return false;

		var fileName = Path.GetFileName(path);
		if (string.Equals(fileName, "AssemblyInfo.cs", StringComparison.OrdinalIgnoreCase))
			return false;
		if (fileName.EndsWith(".g.cs", StringComparison.OrdinalIgnoreCase) ||
			fileName.EndsWith(".g.i.cs", StringComparison.OrdinalIgnoreCase) ||
			string.Equals(fileName, "GameEngine.Scripts.GlobalUsings.g.cs", StringComparison.OrdinalIgnoreCase))
			return false;

		// Normalize separators and drop any obj/.NETCoreApp flavored paths.
		var norm = path.Replace('\\', '/');
		if (norm.IndexOf("/obj/", StringComparison.OrdinalIgnoreCase) >= 0)
			return false;
		if (norm.IndexOf(".NETCoreApp,Version=", StringComparison.OrdinalIgnoreCase) >= 0)
			return false;

		return true;
	}

	// ------------------------------------------------------------------
	// Metadata reference caching (A4)
	//
	// Building the reference list used to re-enumerate the TPA set and call
	// MetadataReference.CreateFromFile for every BCL + engine DLL on every
	// request (hundreds of PE re-opens per keystroke compile) while syntax
	// trees were cached. References are now cached keyed by the TPA hash
	// (process-constant) plus the engine bin dir and per-DLL size/mtime
	// stamps, so an engine rebuild that swaps DLLs invalidates the cache.
	// ------------------------------------------------------------------

	private sealed class ReferenceCacheEntry
	{
		public string Key = "";
		public List<MetadataReference> References = new();
	}

	// Keyed multi-entry cache: different workspaces legitimately use different extra
	// reference sets (P1: one per package module + the project), so a single slot
	// would thrash on alternating requests. Small and reset at the cap — rebuilds are
	// correct, just slower.
	private static readonly Dictionary<string, ReferenceCacheEntry> s_ReferenceCaches = new(StringComparer.Ordinal);
	private const int kMaxReferenceCacheEntries = 8;
	private static string? s_TpaHash;

	private static string GetTpaPaths() =>
		(AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES") as string) ?? string.Empty;

	private static string GetTpaHash()
	{
		if (s_TpaHash is not null)
			return s_TpaHash;
		var bytes = System.Security.Cryptography.SHA256.HashData(Encoding.UTF8.GetBytes(GetTpaPaths()));
		s_TpaHash = Convert.ToHexString(bytes);
		return s_TpaHash;
	}

	// ------------------------------------------------------------------
	// Targeting-pack BCL references (P4a)
	//
	// The request's Tfm names the framework the output must be consumable as
	// (the generated csprojs say net10.0). Compiling against this process's
	// TRUSTED_PLATFORM_ASSEMBLIES binds the output to whatever runtime the
	// server happens to run on (e.g. an 11.0 CoreLib when the server runs on
	// a newer major) — a net10.0 `dotnet build` consuming such a DLL as a
	// <Reference> then fails CS0012/CS1705 on the very code the editor
	// accepted, and a shipped game whose runtime matches the declared TFM
	// cannot load it. Resolve the Microsoft.NETCore.App.Ref targeting pack
	// for the requested TFM and compile against its reference assemblies;
	// only when no matching pack is installed fall back to the TPA set
	// (previous behavior), loudly in the log.
	// ------------------------------------------------------------------

	private static readonly Dictionary<string, List<string>?> s_TargetingPackFiles = new(StringComparer.OrdinalIgnoreCase);

	private static IEnumerable<string> EnumerateDotnetRootCandidates()
	{
		var fromEnv = Environment.GetEnvironmentVariable("DOTNET_ROOT");
		if (!string.IsNullOrEmpty(fromEnv))
			yield return fromEnv;

		// <root>/shared/Microsoft.NETCore.App/<version>/ -> <root>
		string runtimeDir = "";
		try { runtimeDir = RuntimeEnvironment.GetRuntimeDirectory(); } catch { }
		if (!string.IsNullOrEmpty(runtimeDir))
		{
			var root = Path.GetFullPath(Path.Combine(runtimeDir, "..", "..", ".."));
			yield return root;
		}
	}

	/// <summary>
	/// Reference-assembly set of the newest installed Microsoft.NETCore.App.Ref
	/// pack matching the TFM's major version, or null when none is installed.
	/// Cached per TFM (packs are immutable once installed).
	/// </summary>
	private static List<string>? GetTargetingPackFiles(string tfm)
	{
		lock (s_TargetingPackFiles)
		{
			if (s_TargetingPackFiles.TryGetValue(tfm, out var cached))
				return cached;
		}

		// "net10.0" -> "10.0"; anything unparseable skips the pack lookup.
		List<string>? files = null;
		if (tfm.StartsWith("net", StringComparison.OrdinalIgnoreCase) &&
			Version.TryParse(tfm.Substring(3), out var tfmVersion))
		{
			foreach (var dotnetRoot in EnumerateDotnetRootCandidates())
			{
				try
				{
					var packRoot = Path.Combine(dotnetRoot, "packs", "Microsoft.NETCore.App.Ref");
					if (!Directory.Exists(packRoot))
						continue;

					Version? bestVersion = null;
					string? bestDir = null;
					foreach (var versionDir in Directory.EnumerateDirectories(packRoot))
					{
						if (!Version.TryParse(Path.GetFileName(versionDir), out var v))
							continue;
						if (v.Major != tfmVersion.Major)
							continue;
						var refDir = Path.Combine(versionDir, "ref", tfm);
						if (!Directory.Exists(refDir))
							continue;
						if (bestVersion is null || v > bestVersion)
						{
							bestVersion = v;
							bestDir = refDir;
						}
					}

					if (bestDir is not null)
					{
						files = Directory.EnumerateFiles(bestDir, "*.dll", SearchOption.TopDirectoryOnly)
							.OrderBy(p => p, StringComparer.OrdinalIgnoreCase)
							.ToList();
						if (files.Count > 0)
							break;
						files = null;
					}
				}
				catch { /* malformed install; try the next root */ }
			}
		}

		lock (s_TargetingPackFiles)
		{
			s_TargetingPackFiles[tfm] = files;
		}
		return files;
	}

	/// <summary>
	/// Engine-managed DLLs referenced in addition to the TPA set, in reference order:
	/// ABI modules (sorted), then the core engine-managed assemblies.
	/// </summary>
	private static List<string> EnumerateEngineReferenceCandidates(string engineBinDir)
	{
		var candidates = new List<string>();
		try
		{
			candidates.AddRange(Directory.EnumerateFiles(engineBinDir, "GameEngine.*.ABI.dll", SearchOption.TopDirectoryOnly)
				.OrderBy(p => p, StringComparer.OrdinalIgnoreCase));
		}
		catch { /* engine bin dir may vanish mid-request; stamps below capture the difference */ }
		candidates.Add(Path.Combine(engineBinDir, "GameEngine.CoreBridge.dll"));
		candidates.Add(Path.Combine(engineBinDir, "GameEngine.HotReload.dll"));
		candidates.Add(Path.Combine(engineBinDir, "GameEngine.Scripting.Runtime.dll"));
		candidates.Add(Path.Combine(engineBinDir, "GameEngine.Editor.dll"));
		return candidates;
	}

	private static string BuildReferenceCacheKey(string bclKey, string? engineBinDir, List<string>? engineCandidates, List<string>? extraReferences)
	{
		var sb = new StringBuilder(512);
		sb.Append(bclKey).Append('\n');
		sb.Append(engineBinDir ?? "<none>").Append('\n');
		void AppendStamps(List<string>? paths)
		{
			if (paths is null)
				return;
			foreach (var path in paths)
			{
				try
				{
					var fi = new FileInfo(path);
					if (!fi.Exists)
						continue;
					sb.Append(fi.FullName).Append('|').Append(fi.LastWriteTimeUtc.Ticks).Append('|').Append(fi.Length).Append('\n');
				}
				catch
				{
					sb.Append(path).Append("|stat-failed").Append('\n');
				}
			}
		}
		AppendStamps(engineCandidates);
		if (extraReferences is not null && extraReferences.Count > 0)
		{
			sb.Append("--refs--\n");
			AppendStamps(extraReferences);
		}
		return sb.ToString();
	}

	/// <summary>
	/// Resolve the engine binaries directory for a request: explicit EngineBinDir,
	/// then derived from ProjectRoot, then this process's base directory.
	/// </summary>
	private static string? ResolveEngineBinDir(BuildRequest request)
	{
		if (!string.IsNullOrEmpty(request.EngineBinDir) && Directory.Exists(request.EngineBinDir))
			return request.EngineBinDir;

		if (!string.IsNullOrEmpty(request.ProjectRoot))
		{
			try
			{
				// e.g. .../ScriptAssemblies/Debug/net10.0 -> bin dir is ..\..\..
				var candidate = Path.GetFullPath(Path.Combine(request.ProjectRoot, "..", "..", ".."));
				if (File.Exists(Path.Combine(candidate, "GameEngine.CoreBridge.dll")) ||
					File.Exists(Path.Combine(candidate, "GameEngine.Scripting.ABI.dll")))
				{
					return candidate;
				}
			}
			catch { /* ProjectRoot may be malformed; fall through to base dir probe */ }
		}

		try
		{
			var candidate = AppContext.BaseDirectory;
			if (File.Exists(Path.Combine(candidate, "GameEngine.CoreBridge.dll")) ||
				File.Exists(Path.Combine(candidate, "GameEngine.Scripting.ABI.dll")))
			{
				return candidate;
			}
		}
		catch { }

		return null;
	}

	/// <summary>
	/// Return the metadata reference list for a request, cached across requests.
	/// Cache hits skip all PE re-opens; any engine DLL change (path set, size,
	/// or last-write-time) or TPA change rebuilds the list — correctness over speed.
	/// </summary>
	private static List<MetadataReference> GetOrBuildReferences(BuildRequest request, string logPath)
	{
		var sw = System.Diagnostics.Stopwatch.StartNew();
		string? engineBinDir = ResolveEngineBinDir(request);
		var engineCandidates = engineBinDir is not null ? EnumerateEngineReferenceCandidates(engineBinDir) : null;
		var extraReferences = (request.References ?? Array.Empty<string>())
			.Where(p => !string.IsNullOrEmpty(p))
			.ToList();
		string tfm = string.IsNullOrEmpty(request.Tfm) ? "net10.0" : request.Tfm!;
		var targetingPack = GetTargetingPackFiles(tfm);
		// Pack files are immutable; the ref directory identifies the BCL set.
		string bclKey = targetingPack is not null
			? "refpack:" + (Path.GetDirectoryName(targetingPack[0]) ?? tfm)
			: "tpa:" + GetTpaHash();
		string key = BuildReferenceCacheKey(bclKey, engineBinDir, engineCandidates, extraReferences);

		if (s_ReferenceCaches.TryGetValue(key, out var cached))
		{
			sw.Stop();
			try { File.AppendAllText(logPath, $"[{DateTime.Now:O}] References: cache HIT ({cached.References.Count} refs) in {sw.Elapsed.TotalMilliseconds:F2}ms{Environment.NewLine}"); } catch { }
			return cached.References;
		}

		int skippedTpa = 0;
		var refs = new List<MetadataReference>();

		var addedRefs = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
		void AddRefIfExists(string path)
		{
			try
			{
				if (string.IsNullOrEmpty(path) || !File.Exists(path))
					return;
				var full = Path.GetFullPath(path);
				if (!addedRefs.Add(full))
					return;
				refs.Add(MetadataReference.CreateFromFile(full));
			}
			catch { skippedTpa++; }
		}

		if (targetingPack is not null)
		{
			// BCL = the requested TFM's reference assemblies, nothing from the live
			// runtime (mixing in the running CoreLib would leak newer-than-TFM API
			// surface right back in).
			foreach (var p in targetingPack)
				AddRefIfExists(p);
		}
		else
		{
			try
			{
				File.AppendAllText(logPath,
					$"[{DateTime.Now:O}] References: no Microsoft.NETCore.App.Ref pack for '{tfm}' — " +
					$"falling back to the server runtime's TPA (output binds to the live runtime's BCL){Environment.NewLine}");
			}
			catch { }

			// Resolve references from Trusted Platform Assemblies (managed set)
			foreach (var p in GetTpaPaths().Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
			{
				if (!p.EndsWith(".dll", StringComparison.OrdinalIgnoreCase)) continue;
				try { refs.Add(MetadataReference.CreateFromFile(p)); }
				catch { skippedTpa++; /* native or invalid PE; summarized below */ }
			}

			// Ensure core references are present explicitly
			AddRefIfExists(typeof(object).Assembly.Location); // System.Private.CoreLib
			AddRefIfExists(typeof(Console).Assembly.Location); // System.Console
			AddRefIfExists(typeof(Enumerable).Assembly.Location); // System.Linq
		}

		// Engine-managed assemblies (ABI modules + CoreBridge/HotReload/Scripting.Runtime/Editor)
		// so user scripts compile against the modular GameEngine.*.ABI surfaces.
		if (engineCandidates is not null)
		{
			foreach (var candidate in engineCandidates)
				AddRefIfExists(candidate);
		}

		// Request-supplied extra references (P1: dependency package assemblies).
		foreach (var extra in extraReferences)
			AddRefIfExists(extra);

		if (s_ReferenceCaches.Count >= kMaxReferenceCacheEntries)
			s_ReferenceCaches.Clear();
		s_ReferenceCaches[key] = new ReferenceCacheEntry { Key = key, References = refs };
		sw.Stop();
		try
		{
			File.AppendAllText(logPath,
				$"[{DateTime.Now:O}] References: cache MISS, rebuilt {refs.Count} refs in {sw.Elapsed.TotalMilliseconds:F2}ms " +
				$"(engineBinDir='{engineBinDir ?? "<none>"}', skipped {skippedTpa} non-PE/unreadable entries){Environment.NewLine}");
		}
		catch { }
		return refs;
	}

	public static int Main(string[] args)
	{
		try
		{
			Console.InputEncoding = Encoding.UTF8;
			Console.OutputEncoding = Encoding.UTF8;

			// Control mode: maintenance helpers
			if (args.Length > 0 && args[0].StartsWith("--", StringComparison.Ordinal))
			{
				var cmd = args[0];
				if (string.Equals(cmd, "--shutdown", StringComparison.OrdinalIgnoreCase))
				{
					var targetPipe = args.Length > 1 ? args[1] : "GE_CompileServer_default";
					return SendShutdown(targetPipe, kShutdownMessage) ? 0 : 2;
				}
				if (string.Equals(cmd, "--shutdown-running-from", StringComparison.OrdinalIgnoreCase))
				{
					if (args.Length < 2)
					{
						Console.Error.WriteLine("[CompileServerHost] --shutdown-running-from needs the path of a GameEngine.CompileServerHost.dll");
						return 2;
					}
					ShutdownHostsRunningFrom(args[1]);
					return 0;
				}
				if (string.Equals(cmd, "--status", StringComparison.OrdinalIgnoreCase))
				{
					var targetPipe = args.Length > 1 ? args[1] : "GE_CompileServer_default";
					return PrintStatus(targetPipe) ? 0 : 2;
				}
				if (string.Equals(cmd, "--shutdown-if-idle", StringComparison.OrdinalIgnoreCase))
				{
					var targetPipe = args.Length > 1 ? args[1] : "GE_CompileServer_default";
					return SendShutdownIfIdle(targetPipe) ? 0 : 2;
				}
			}

			var pipeName = args.Length > 0 ? args[0] : "GE_CompileServer";
			var logPath = GetLogPath();
			try { File.AppendAllText(logPath, $"[{DateTime.Now:O}] Start server pipe='{pipeName}'\n"); } catch { }
			Console.Error.WriteLine($"[CompileServerHost] Starting named pipe server '{pipeName}'...");


			// Optional test-only override for reported version (allows C++ client to test recycle path)
			try
			{
				for (int i = 1; i < args.Length; i++)
				{
					var a = args[i];
					if (a.StartsWith("--spoof-version", StringComparison.Ordinal))
					{
						string? val = null;
						var eq = a.IndexOf('=');
						if (eq >= 0 && eq + 1 < a.Length) val = a.Substring(eq + 1);
						else if (i + 1 < args.Length && !args[i + 1].StartsWith("--", StringComparison.Ordinal)) { val = args[i + 1]; i++; }
						if (!string.IsNullOrEmpty(val)) s_ServerVersion = val!;
					}
				}
			}
			catch { }

			// PID file <Temp>/GE_CompileServer/<pipe>.pid: line 1 the process id, line 2 the full
			// path of the assembly this host runs from. The host build reads line 2 to stop only
			// the host running from its own output; the client prunes files whose pid is gone.
			// Written to a temporary name and moved so a reader never sees a partial file.
			string pidDir = Path.Combine(Path.GetTempPath(), "GE_CompileServer");
			try { Directory.CreateDirectory(pidDir); } catch { }
			string pidPath = Path.Combine(pidDir, pipeName + ".pid");
			string pidTempPath = pidPath + "." + Environment.ProcessId + ".tmp";
			try
			{
				File.WriteAllText(pidTempPath, Environment.ProcessId + "\n" + typeof(Program).Assembly.Location + "\n");
				File.Move(pidTempPath, pidPath, overwrite: true);
			}
			catch
			{
				try { File.Delete(pidTempPath); } catch { }
			}

			// Optional idle timeout (seconds). Default: 120s. Set to 0 to disable.
			int idleSeconds = 120;
			try
			{
				var s = Environment.GetEnvironmentVariable("GE_COMPILE_SERVER_IDLE_SECONDS");
				if (!string.IsNullOrEmpty(s))
					idleSeconds = Math.Max(0, int.Parse(s));
			}
			catch { }

			int exitCode;
			if (OperatingSystem.IsWindows())
			{
				exitCode = RunNamedPipeServer(pipeName, logPath, pidPath, idleSeconds);
			}
			else
			{
				exitCode = RunUnixSocketServer(pipeName, logPath, pidPath, idleSeconds);
			}

			return exitCode;
		}
		catch (Exception ex)
		{
			try { File.AppendAllText(GetLogPath(), $"[{DateTime.Now:O}] Fatal: {ex}\n"); } catch { }
			Console.Error.WriteLine($"[CompileServerHost] Fatal: {ex}");
			return -1;
		}
	}

	private static int RunNamedPipeServer(string pipeName, string logPath, string pidPath, int idleSeconds)
	{
		for (; ; )
		{
			NamedPipeServerStream? server = null;
			try
			{
				server = new NamedPipeServerStream(
					pipeName,
					PipeDirection.InOut,
					NamedPipeServerStream.MaxAllowedServerInstances,
					PipeTransmissionMode.Byte,
					PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
			}
			catch (IOException)
			{
				// Another server instance may hold the pipe. Wait briefly and retry.
				Thread.Sleep(200);
				continue;
			}

			try { File.AppendAllText(logPath, $"[{DateTime.Now:O}] Waiting for connection...{Environment.NewLine}"); } catch { }

			try
			{
				if (idleSeconds > 0)
				{
					using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(idleSeconds));
					server.WaitForConnectionAsync(cts.Token).GetAwaiter().GetResult();
				}
				else
				{
					server.WaitForConnection();
				}
			}
			catch (OperationCanceledException)
			{
				try { File.AppendAllText(logPath, $"[{DateTime.Now:O}] Idle timeout -> exiting{Environment.NewLine}"); } catch { }
				server.Dispose();
				break;
			}
			catch (IOException)
			{
				// Pipe was closed externally (e.g., another server instance took over).
				// Dispose and retry — the next iteration creates a fresh pipe.
				server.Dispose();
				continue;
			}

			try { File.AppendAllText(logPath, $"[{DateTime.Now:O}] Client connected{Environment.NewLine}"); } catch { }

			bool shouldExit;
			HandleClientConnection(server, pipeName, logPath, pidPath, idleSeconds, isNamedPipe: true, out shouldExit);

			// Disconnect and dispose after handling the client.
			try { if (server.IsConnected) server.Disconnect(); } catch { }
			server.Dispose();

			if (shouldExit)
			{
				try { File.Delete(pidPath); } catch { }
				return 0;
			}
		}

		try { File.Delete(pidPath); } catch { }
		return 0;
	}

	private static int RunUnixSocketServer(string pipeName, string logPath, string pidPath, int idleSeconds)
	{
		var socketPath = GetUnixSocketPath(pipeName);

		try { if (File.Exists(socketPath)) File.Delete(socketPath); } catch { }

		Socket? listenSocket = null;
		try
		{
			listenSocket = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
			var endPoint = new UnixDomainSocketEndPoint(socketPath);
			listenSocket.Bind(endPoint);
			listenSocket.Listen(10);

			try { File.AppendAllText(logPath, $"[{DateTime.Now:O}] Listening on unix socket '{socketPath}'{Environment.NewLine}"); } catch { }

			for (; ; )
			{
				Socket? client = null;
				try
				{
					if (idleSeconds > 0)
					{
						int timeoutUs = Math.Max(1, idleSeconds * 1_000_000);
						bool ready = listenSocket.Poll(timeoutUs, SelectMode.SelectRead);
						if (!ready)
						{
							try { File.AppendAllText(logPath, $"[{DateTime.Now:O}] Idle timeout -> exiting{Environment.NewLine}"); } catch { }
							break;
						}
					}

					client = listenSocket.Accept();
				}
				catch (SocketException ex)
				{
					try { File.AppendAllText(logPath, $"[{DateTime.Now:O}] Socket accept failed: {ex}{Environment.NewLine}"); } catch { }
					break;
				}

				if (client == null)
				{
					break;
				}

				try { File.AppendAllText(logPath, $"[{DateTime.Now:O}] Client connected (unix socket){Environment.NewLine}"); } catch { }

				using (client)
				using (var stream = new NetworkStream(client, ownsSocket: false))
				{
					bool shouldExit;
					HandleClientConnection(stream, pipeName, logPath, pidPath, idleSeconds, isNamedPipe: false, out shouldExit);
					if (shouldExit)
					{
						return 0;
					}
				}
			}
		}
		finally
		{
			try { listenSocket?.Dispose(); } catch { }
			try { File.Delete(socketPath); } catch { }
			try { File.Delete(pidPath); } catch { }
		}

		return 0;
	}

	private static void HandleClientConnection(Stream stream, string pipeName, string logPath, string pidPath, int idleSeconds, bool isNamedPipe, out bool shouldExit)
	{
		shouldExit = false;

		try
		{
			using var reader = new StreamReader(stream, Encoding.UTF8, detectEncodingFromByteOrderMarks: false, leaveOpen: true);
			using var writer = new StreamWriter(stream, new UTF8Encoding(false), bufferSize: 1024, leaveOpen: true) { AutoFlush = true };

			// Multi-message loop: process messages until the client disconnects.
			// This allows the client to send __version__ and a compile request on the
			// same connection without reconnecting (eliminates pipe recycling latency).
			// Old clients that disconnect after __version__ are handled naturally —
			// ReadLine returns null and the loop exits.
			string? line;
			while ((line = reader.ReadLine()) != null)
			{
			// Control message: shutdown
			if (string.Equals(line, kShutdownMessage, StringComparison.Ordinal))
			{
				try { writer.WriteLine("ok"); TryPipeDrain(stream, isNamedPipe); } catch { }
				try { File.Delete(pidPath); } catch { }
				shouldExit = true;
				return;
			}

			// Control message: shutdown only if this host runs from the given assembly path
			if (line.StartsWith(kShutdownRunningFromMessage, StringComparison.Ordinal))
			{
				if (!IsRunningFrom(line.Substring(kShutdownRunningFromMessage.Length)))
				{
					try { writer.WriteLine("other"); TryPipeDrain(stream, isNamedPipe); } catch { }
					continue; // stay on connection for next message
				}
				try { writer.WriteLine("ok"); TryPipeDrain(stream, isNamedPipe); } catch { }
				try { File.Delete(pidPath); } catch { }
				shouldExit = true;
				return;
			}

			// Control message: version
			if (string.Equals(line, "__version__", StringComparison.Ordinal))
			{
				var jsonVersion = JsonSerializer.Serialize(new { Version = s_ServerVersion, ProcessId = Environment.ProcessId });
				try { writer.WriteLine(jsonVersion); TryPipeDrain(stream, isNamedPipe); } catch { }
				continue; // stay on connection for next message
			}

			// Control message: status
			if (string.Equals(line, "__status__", StringComparison.Ordinal))
			{
				var now = DateTime.UtcNow;
				var status = new
				{
					Pipe = pipeName,
					ProcessId = Environment.ProcessId,
					UptimeSeconds = (now - s_StartTimeUtc).TotalSeconds,
					LastActivityAgoSeconds = (now - s_LastActivityUtc).TotalSeconds,
					IdleSeconds = idleSeconds,
					Workspaces = s_Workspaces.Count,
					ImplicitUsings = s_ImplicitUsings,
				};
				var jsonStatus = JsonSerializer.Serialize(status);
				try { writer.WriteLine(jsonStatus); TryPipeDrain(stream, isNamedPipe); } catch { }
				continue; // stay on connection for next message
			}

			// Control message: shutdown if idle (server decides)
			if (string.Equals(line, "__shutdown_if_idle__", StringComparison.Ordinal))
			{
				var now = DateTime.UtcNow;
				if (idleSeconds > 0 && (now - s_LastActivityUtc).TotalSeconds >= idleSeconds)
				{
					try { writer.WriteLine("ok"); TryPipeDrain(stream, isNamedPipe); } catch { }
					try { File.Delete(pidPath); } catch { }
					shouldExit = true;
					return;
				}

				try { writer.WriteLine("busy"); TryPipeDrain(stream, isNamedPipe); } catch { }
				continue; // stay on connection for next message
			}

			// Compile request path
			try
			{
				var request = JsonSerializer.Deserialize<BuildRequest>(line) ?? throw new InvalidOperationException("Invalid request");

				// Lightweight diagnostics: how many files are we actually compiling?
				try
				{
					var all = request.AllFiles ?? Array.Empty<string>();
					int included = 0;
					foreach (var p in all)
					{
						if (!ShouldIncludeSource(p))
							continue;
						if (File.Exists(p))
							included++;
					}
					File.AppendAllText(logPath,
						$"[{DateTime.Now:O}] Compile request: root='{request.ProjectRoot}' allFiles={all.Length} includedFiles={included} forceFull={request.ForceFull} strategy='{request.PreferredStrategy}'{Environment.NewLine}");
				}
				catch { }

				// Update last-activity ONLY for real compile requests (not control messages)
				s_LastActivityUtc = DateTime.UtcNow;

				// Maintain per-workspace state for incremental builds
				var workspaceKey = request.ProjectRoot ?? string.Empty;
				if (!s_Workspaces.TryGetValue(workspaceKey, out var ws))
				{
					ws = new WorkspaceState();
					s_Workspaces[workspaceKey] = ws;
				}

				// Per-workspace parse options (defines); resets cached trees on change.
				var parseOptions = ApplyWorkspaceDefines(ws, request);

				// Build list of syntax trees based on PreferredStrategy
				var trees = new List<SyntaxTree>();
				if (!request.ForceFull && string.Equals(request.PreferredStrategy, "Incremental", StringComparison.OrdinalIgnoreCase))
				{
					// Update changed files in cache
					foreach (var path in request.ChangedFiles ?? Array.Empty<string>())
					{
						if (!ShouldIncludeSource(path))
							continue;
						if (File.Exists(path))
						{
							var text = File.ReadAllText(path, Encoding.UTF8);
							ws.Trees[path] = CSharpSyntaxTree.ParseText(text, parseOptions, path, Encoding.UTF8);
						}
						else
						{
							ws.Trees.Remove(path);
						}
					}

					// Affected files: reparse to ensure semantic correctness
					foreach (var path in request.AffectedFiles ?? Array.Empty<string>())
					{
						if (!ShouldIncludeSource(path))
							continue;
						if (File.Exists(path))
						{
							var text = File.ReadAllText(path, Encoding.UTF8);
							ws.Trees[path] = CSharpSyntaxTree.ParseText(text, parseOptions, path, Encoding.UTF8);
						}
					}

					// All current trees come from cache; ensure all AllFiles are represented
					foreach (var path in request.AllFiles ?? Array.Empty<string>())
					{
						if (!ShouldIncludeSource(path))
							continue;
						if (!ws.Trees.ContainsKey(path) && File.Exists(path))
						{
							var text = File.ReadAllText(path, Encoding.UTF8);
							ws.Trees[path] = CSharpSyntaxTree.ParseText(text, parseOptions, path, Encoding.UTF8);
						}
					}
					trees.AddRange(ws.Trees.Values);
				}
				else
				{
					// Full compile: reparse everything
					foreach (var path in request.AllFiles ?? Array.Empty<string>())
					{
						if (!ShouldIncludeSource(path))
							continue;
						if (File.Exists(path))
						{
							var text = File.ReadAllText(path, Encoding.UTF8);
							trees.Add(CSharpSyntaxTree.ParseText(text, parseOptions, path, Encoding.UTF8));
						}
					}
					// reset cache
					ws.Trees.Clear();
					ws.Driver = null; // Force generator driver re-creation on full rebuild
					foreach (var t in trees)
					{
						var filePath = t.FilePath;
						if (!string.IsNullOrEmpty(filePath)) ws.Trees[filePath] = t;
					}
				}

				// A snapshot taken mid-write (rapid-edit storms) can see the user's
				// source files missing: the request names sources but every tree
				// filters out, and Roslyn would happily emit a ~4KB stub assembly
				// with zero types that the client then swaps in as a "successful"
				// build that runs nothing. Reject exactly that case. A request that
				// arrives with an EMPTY source list is different — it is the
				// by-design "no scripts here" compile (e.g. editor-scripts for a
				// project without editor-side C#) and must keep emitting its empty
				// assembly.
				int requestedSources = (request.AllFiles ?? Array.Empty<string>()).Length;
				if (trees.Count == 0 && requestedSources > 0)
				{
					try
					{
						File.AppendAllText(logPath,
							$"[{DateTime.Now:O}] Compile rejected: request named {requestedSources} source(s) but none were parseable (root='{request.ProjectRoot}') — files missing or filtered at snapshot time{Environment.NewLine}");
					}
					catch { }
					var noSources = new BuildResponse(false, Array.Empty<Diagnostic>(),
						new[] { new Diagnostic("Error", "GE0002", request.ProjectRoot ?? string.Empty, 0, 0,
							$"Request named {requestedSources} source file(s) but none were parseable (missing at snapshot time — mid-write race?)") },
						null, null);
					var noSourcesJson = JsonSerializer.Serialize(noSources);
					try { writer.WriteLine(noSourcesJson); TryPipeDrain(stream, isNamedPipe); } catch (IOException) { }
					continue;
				}

				// References: cached across requests, invalidated on any engine DLL change (A4)
				var refs = GetOrBuildReferences(request, logPath);

				// Added after the source-count gate: the synthetic usings tree is not a user source.
				if (request.ImplicitUsings)
					trees.Add(GetImplicitUsingsTree(ws));

				var compilation = CSharpCompilation.Create(
					assemblyName: string.IsNullOrWhiteSpace(request.AssemblyName) ? "GameEngine.DynamicAssembly" : request.AssemblyName,
					syntaxTrees: trees,
					references: refs,
					options: new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, optimizationLevel: OptimizationLevel.Debug, concurrentBuild: true, warningLevel: 4,
						allowUnsafe: request.AllowUnsafe,
						nullableContextOptions: request.Nullable ? NullableContextOptions.Enable : NullableContextOptions.Disable)
				);

				// Run source generators (EntitySystemGenerator, etc.)
				// Resolve engineBinDir for generator discovery (may already be computed above).
				ImmutableArray<Microsoft.CodeAnalysis.Diagnostic> generatorDiagnostics = ImmutableArray<Microsoft.CodeAnalysis.Diagnostic>.Empty;
				{
					string? genBinDir = request.EngineBinDir;
					if (string.IsNullOrEmpty(genBinDir))
					{
						try
						{
							var candidate = AppContext.BaseDirectory;
							if (Directory.Exists(Path.Combine(candidate, "SourceGenerators")))
								genBinDir = candidate;
						}
						catch { }
					}

					if (ws.Driver is null)
					{
						var generators = TryLoadGenerators(genBinDir);
						if (generators.Length > 0)
							ws.Driver = CSharpGeneratorDriver.Create(
								generators.Select(g => g.AsSourceGenerator()).ToArray(),
								parseOptions: parseOptions);
					}
					if (ws.Driver is not null)
					{
						ws.Driver = ws.Driver.RunGeneratorsAndUpdateCompilation(
							compilation, out var updatedCompilation, out generatorDiagnostics);
						compilation = (CSharpCompilation)updatedCompilation;
					}
				}

				using var peStream = new MemoryStream();
				using var pdbStream = new MemoryStream();
				var emitResult = compilation.Emit(peStream, pdbStream);

				// Merge generator diagnostics with emit diagnostics
				var allDiagnostics = emitResult.Diagnostics;
				if (!generatorDiagnostics.IsDefaultOrEmpty)
					allDiagnostics = allDiagnostics.AddRange(generatorDiagnostics);
				var diags = allDiagnostics.Where(d => d.Severity != DiagnosticSeverity.Hidden).ToArray();
				var warns = diags.Where(d => d.Severity == DiagnosticSeverity.Warning)
					.Select(d => new Diagnostic("Warning", d.Id, d.Location.SourceTree?.FilePath ?? request.ProjectRoot ?? string.Empty, d.Location.GetLineSpan().StartLinePosition.Line + 1, d.Location.GetLineSpan().StartLinePosition.Character + 1, d.GetMessage()))
					.ToArray();
				var errs = diags.Where(d => d.Severity == DiagnosticSeverity.Error)
					.Select(d => new Diagnostic("Error", d.Id, d.Location.SourceTree?.FilePath ?? request.ProjectRoot ?? string.Empty, d.Location.GetLineSpan().StartLinePosition.Line + 1, d.Location.GetLineSpan().StartLinePosition.Character + 1, d.GetMessage()))
					.ToArray();

				// When duplicate-attribute errors occur (CS0579), log the full set of input
				// files for diagnostics so the native side can see exactly what was compiled.
				if (errs.Any(d => string.Equals(d.Code, "CS0579", StringComparison.Ordinal)))
				{
					try
					{
						var sb = new StringBuilder();
						sb.AppendLine($"[{DateTime.Now:O}] CS0579 detected. ProjectRoot='{request.ProjectRoot}' AllFiles:");
						foreach (var path in request.AllFiles ?? Array.Empty<string>())
						{
							sb.AppendLine($"  {path}");
						}
						File.AppendAllText(logPath, sb.ToString());
					}
					catch { /* best-effort logging only */ }
				}

				// A source generator reports its failures through generatorDiagnostics, never
				// through emitResult: Roslyn emits a perfectly valid assembly for a compilation
				// whose generator errored — just missing whatever that generator would have
				// contributed. Gating only on emitResult.Success would ship a "successful" build
				// with the [ModuleInitializer] registrations absent, so a generator error fails
				// the compile exactly like a compiler error does. Warnings stay warnings.
				bool success = emitResult.Success && errs.Length == 0;

				byte[]? peBytes = null, pdbBytes = null;
				if (success)
				{
					peBytes = peStream.ToArray();
					pdbBytes = pdbStream.ToArray();
				}

				var response = new BuildResponse(success, warns, errs, peBytes, pdbBytes);
				var json = JsonSerializer.Serialize(response);
				try { writer.WriteLine(json); TryPipeDrain(stream, isNamedPipe); } catch (IOException) { }
			}
			catch (Exception ex)
			{
				var error = new BuildResponse(false, Array.Empty<Diagnostic>(), new[] { new Diagnostic("Error", "GE0001", "", 0, 0, ex.Message) }, null, null);
				var json = JsonSerializer.Serialize(error);
				try { writer.WriteLine(json); TryPipeDrain(stream, isNamedPipe); } catch (IOException) { }
			}

			} // end while (multi-message loop)
		}
		catch (IOException)
		{
			// Broken pipe or similar I/O issue talking to a single client should not
			// tear down the entire server process. Swallow and allow the server loop
			// to continue accepting new connections.
		}
	}

	private static void TryPipeDrain(Stream stream, bool isNamedPipe)
	{
		if (!isNamedPipe)
			return;

		if (stream is NamedPipeServerStream pipe)
		{
			try { pipe.WaitForPipeDrain(); } catch { }
		}
	}

	private static string GetUnixBaseDirectory()
	{
		var overrideDir = Environment.GetEnvironmentVariable("GE_PIPE_PATH");
		if (!string.IsNullOrEmpty(overrideDir))
			return overrideDir;

		if (OperatingSystem.IsLinux())
		{
			var xdg = Environment.GetEnvironmentVariable("XDG_RUNTIME_DIR");
			if (!string.IsNullOrEmpty(xdg))
				return Path.Combine(xdg, "gameengine-compile-server");

			var uid = GetUidSafe();
			return Path.Combine("/tmp", $"gameengine-{uid}", "compile_server");
		}

		if (OperatingSystem.IsMacOS())
		{
			// NOTE: macOS enforces a strict ~104 byte limit on Unix domain socket paths.
			// TMPDIR is often a very long per-user path (e.g. /var/folders/...) which,
			// when combined with our pipe name, easily exceeds this limit and causes
			// UnixDomainSocketEndPoint to throw ArgumentOutOfRangeException.
			//
			// To avoid this, always prefer a short, stable base directory under /tmp.
			// Callers can still override this via GE_PIPE_PATH if desired.
			return Path.Combine("/tmp", "gameengine-compile-server");
		}

		return "/tmp/gameengine-compile-server";
	}

	private static string GetUnixSocketPath(string pipeName)
	{
		var dir = GetUnixBaseDirectory();
		try { Directory.CreateDirectory(dir); } catch { }
		return Path.Combine(dir, pipeName + ".sock");
	}

	[DllImport("libc")]
	private static extern uint getuid();

	private static long GetUidSafe()
	{
		try { return getuid(); }
		catch { return 0; }
	}

	private static bool SendShutdown(string pipeName, string message)
	{
		return OperatingSystem.IsWindows()
			? SendShutdownNamedPipe(pipeName, message)
			: SendShutdownUnixSocket(pipeName, message);
	}

	private static bool PathsEqual(string a, string b)
	{
		var comparison = OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
		try { return string.Equals(Path.GetFullPath(a), Path.GetFullPath(b), comparison); }
		catch { return false; }
	}

	private static bool IsRunningFrom(string assemblyPath)
	{
		return PathsEqual(assemblyPath, typeof(Program).Assembly.Location);
	}

	// Asks every host whose pid file names assemblyPath to exit, and waits briefly for each to
	// go. The host checks the path itself and refuses when it runs from another copy, so a
	// stale pid file or a reused pid never stops anything. A host that does not answer (busy
	// in a long compile, or hung) is left running.
	private static void ShutdownHostsRunningFrom(string assemblyPath)
	{
		var pidDir = Path.Combine(Path.GetTempPath(), "GE_CompileServer");
		string[] pidFiles;
		try { pidFiles = Directory.GetFiles(pidDir, "*.pid"); }
		catch { return; }
		foreach (var pidFile in pidFiles)
		{
			string[] lines;
			try { lines = File.ReadAllLines(pidFile); }
			catch { continue; }
			if (lines.Length < 2 || !int.TryParse(lines[0], out var hostId) || !PathsEqual(lines[1], assemblyPath))
				continue;
			if (!SendShutdown(Path.GetFileNameWithoutExtension(pidFile), kShutdownRunningFromMessage + assemblyPath))
				continue;
			try
			{
				using var process = System.Diagnostics.Process.GetProcessById(hostId);
				process.WaitForExit(5000);
			}
			catch { }
		}
	}

	private static bool PrintStatus(string pipeName)
	{
		return OperatingSystem.IsWindows()
			? PrintStatusNamedPipe(pipeName)
			: PrintStatusUnixSocket(pipeName);
	}

	private static bool SendShutdownIfIdle(string pipeName)
	{
		return OperatingSystem.IsWindows()
			? SendShutdownIfIdleNamedPipe(pipeName)
			: SendShutdownIfIdleUnixSocket(pipeName);
	}

	private static bool SendShutdownNamedPipe(string pipeName, string message)
	{
		try
		{
			using var client = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
			var sw = System.Diagnostics.Stopwatch.StartNew();
			while (!client.IsConnected && sw.Elapsed < TimeSpan.FromMilliseconds(400))
			{
				try { client.Connect(100); } catch { }
			}
			if (!client.IsConnected) return false;
			using var writer = new StreamWriter(client, new UTF8Encoding(false), leaveOpen: true) { AutoFlush = true };
			using var reader = new StreamReader(client, Encoding.UTF8, detectEncodingFromByteOrderMarks: false, leaveOpen: true);
			writer.WriteLine(message);
			var response = reader.ReadLine();
			return string.Equals(response, "ok", StringComparison.Ordinal);
		}
		catch { return false; }
	}

	private static bool PrintStatusNamedPipe(string pipeName)
	{
		try
		{
			using var client = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
			var sw = System.Diagnostics.Stopwatch.StartNew();
			while (!client.IsConnected && sw.Elapsed < TimeSpan.FromMilliseconds(400))
			{
				try { client.Connect(100); } catch { }
			}
			if (!client.IsConnected) return false;
			using var writer = new StreamWriter(client, new UTF8Encoding(false), leaveOpen: true) { AutoFlush = true };
			using var reader = new StreamReader(client, Encoding.UTF8, detectEncodingFromByteOrderMarks: false, leaveOpen: true);
			writer.WriteLine("__status__");
			var response = reader.ReadLine();
			if (string.IsNullOrEmpty(response)) return false;
			Console.WriteLine(response);
			return true;
		}
		catch { return false; }
	}

	private static bool SendShutdownIfIdleNamedPipe(string pipeName)
	{
		try
		{
			using var client = new NamedPipeClientStream(".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
			var sw = System.Diagnostics.Stopwatch.StartNew();
			while (!client.IsConnected && sw.Elapsed < TimeSpan.FromMilliseconds(400))
			{
				try { client.Connect(100); } catch { }
			}
			if (!client.IsConnected) return false;
			using var writer = new StreamWriter(client, new UTF8Encoding(false), leaveOpen: true) { AutoFlush = true };
			using var reader = new StreamReader(client, Encoding.UTF8, detectEncodingFromByteOrderMarks: false, leaveOpen: true);
			writer.WriteLine("__shutdown_if_idle__");
			var response = reader.ReadLine();
			return string.Equals(response, "ok", StringComparison.Ordinal);
		}
		catch { return false; }
	}

	private static bool SendShutdownUnixSocket(string pipeName, string message)
	{
		try
		{
			var socketPath = GetUnixSocketPath(pipeName);
			using var socket = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
			socket.ReceiveTimeout = 5000; // a host that never answers does not hold the caller
			var endPoint = new UnixDomainSocketEndPoint(socketPath);

			var sw = System.Diagnostics.Stopwatch.StartNew();
			while (!socket.Connected && sw.Elapsed < TimeSpan.FromMilliseconds(400))
			{
				try { socket.Connect(endPoint); }
				catch (SocketException) { Thread.Sleep(50); }
			}
			if (!socket.Connected) return false;

			using var stream = new NetworkStream(socket, ownsSocket: false);
			using var writer = new StreamWriter(stream, new UTF8Encoding(false), leaveOpen: true) { AutoFlush = true };
			using var reader = new StreamReader(stream, Encoding.UTF8, detectEncodingFromByteOrderMarks: false, leaveOpen: true);
			writer.WriteLine(message);
			var response = reader.ReadLine();
			return string.Equals(response, "ok", StringComparison.Ordinal);
		}
		catch { return false; }
	}

	private static bool PrintStatusUnixSocket(string pipeName)
	{
		try
		{
			var socketPath = GetUnixSocketPath(pipeName);
			using var socket = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
			var endPoint = new UnixDomainSocketEndPoint(socketPath);

			var sw = System.Diagnostics.Stopwatch.StartNew();
			while (!socket.Connected && sw.Elapsed < TimeSpan.FromMilliseconds(400))
			{
				try { socket.Connect(endPoint); }
				catch (SocketException) { Thread.Sleep(50); }
			}
			if (!socket.Connected) return false;

			using var stream = new NetworkStream(socket, ownsSocket: false);
			using var writer = new StreamWriter(stream, new UTF8Encoding(false), leaveOpen: true) { AutoFlush = true };
			using var reader = new StreamReader(stream, Encoding.UTF8, detectEncodingFromByteOrderMarks: false, leaveOpen: true);
			writer.WriteLine("__status__");
			var response = reader.ReadLine();
			if (string.IsNullOrEmpty(response)) return false;
			Console.WriteLine(response);
			return true;
		}
		catch { return false; }
	}

	private static bool SendShutdownIfIdleUnixSocket(string pipeName)
	{
		try
		{
			var socketPath = GetUnixSocketPath(pipeName);
			using var socket = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
			var endPoint = new UnixDomainSocketEndPoint(socketPath);

			var sw = System.Diagnostics.Stopwatch.StartNew();
			while (!socket.Connected && sw.Elapsed < TimeSpan.FromMilliseconds(400))
			{
				try { socket.Connect(endPoint); }
				catch (SocketException) { Thread.Sleep(50); }
			}
			if (!socket.Connected) return false;

			using var stream = new NetworkStream(socket, ownsSocket: false);
			using var writer = new StreamWriter(stream, new UTF8Encoding(false), leaveOpen: true) { AutoFlush = true };
			using var reader = new StreamReader(stream, Encoding.UTF8, detectEncodingFromByteOrderMarks: false, leaveOpen: true);
			writer.WriteLine("__shutdown_if_idle__");
			var response = reader.ReadLine();
			return string.Equals(response, "ok", StringComparison.Ordinal);
		}
		catch { return false; }
	}

}
