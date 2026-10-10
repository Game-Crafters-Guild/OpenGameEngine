import * as fs from "fs";
import * as path from "path";
import { repoRoot } from "../paths.js";

export function normalizePreset(preset: string): string {
  const aliases: Record<string, string> = {
    "build-xcode": "__macos_xcode__",
    "build-macos": "__macos_ninja__",
    "build-linux": "__linux_ninja__",
  };
  return aliases[preset] ?? preset;
}

export function detectArch(): "x64" | "arm64" {
  return process.arch === "arm64" ? "arm64" : "x64";
}

// Detect the highest available Visual Studio version by checking for build dirs
// or known VS installation paths.
function detectVsYear(): "2026" | "2022" {
  const cwd = repoRoot();
  const buildDir = path.join(cwd, "build");
  // Prefer existing build dirs (user already configured).
  try {
    const entries = fs.readdirSync(buildDir);
    if (entries.some(d => d.startsWith("vs2026"))) return "2026";
    if (entries.some(d => d.startsWith("vs2022"))) return "2022";
  } catch { /* build dir doesn't exist yet */ }
  // Fall back to checking VS install paths.
  const progFiles = process.env["ProgramFiles"] ?? "C:\\Program Files";
  if (fs.existsSync(path.join(progFiles, "Microsoft Visual Studio", "18"))) return "2026";
  if (fs.existsSync(path.join(progFiles, "Microsoft Visual Studio", "2022"))) return "2022";
  return "2026";
}

// Detect the best CMake build preset for the current platform + arch.
// Priority: existing build dir > platform-specific default.
export function detectPreset(config: string = "DebugFast"): string {
  const cwd = repoRoot();
  const platform = process.platform; // win32, darwin, linux

  if (platform === "darwin") {
    // macOS: prefer configured Ninja builds before legacy Xcode build dirs.
    const arch = detectArch();
    const candidates = [
      `macos-${arch}-ninja`,
      `macos-${arch}-local`,
      ...(arch === "arm64" ? ["macos-x64-ninja", "macos-x64-local"] : []),
      "ninja-x64-sccache-unity",
      "ninja-x64-sccache",
    ];
    for (const preset of candidates) {
      if (fs.existsSync(path.join(cwd, "build", preset))) return preset;
    }
    if (fs.existsSync(path.join(cwd, "build-xcode"))) return "__macos_xcode__";
    if (fs.existsSync(path.join(cwd, "build-macos"))) return "__macos_ninja__";
    return "__macos_ninja__";
  }

  if (platform === "linux") {
    // Linux: check for Ninja sccache preset first, then custom dirs.
    for (const preset of ["ninja-x64-sccache-unity", "ninja-x64-sccache"]) {
      if (fs.existsSync(path.join(cwd, "build", preset))) return preset;
    }
    if (fs.existsSync(path.join(cwd, "build-linux"))) return "__linux_ninja__";
    return "__linux_ninja__";
  }

  // Windows: Visual Studio presets.
  const arch = detectArch();
  const vsYear = detectVsYear();

  // Candidates in preference order. The non-unity `-local` preset is the
  // documented daily driver (see CLAUDE.md: fast incremental, single-file
  // changes recompile only that file), so it comes FIRST. The `-unity` preset
  // is for cold/CI rebuilds and is frequently stale relative to `-local`.
  const candidates = [
    `vs${vsYear}-${arch}-local`,
    `vs${vsYear}-${arch}-local-unity`,
    // Cross-arch fallback on Windows (e.g., ARM64 machine with x64 build).
    ...(arch === "arm64" ? [`vs${vsYear}-x64-local`, `vs${vsYear}-x64-local-unity`] : []),
  ];

  // Among presets that have an actually-built Editor.exe, pick the one whose
  // executable was built most recently. This guarantees we never launch a
  // stale `-unity` build when a fresher `-local` build exists. The freshest
  // pick stats the SAME config the caller will launch (threaded in), so a
  // Release launch isn't decided by Debug-exe mtimes.
  const built = candidates
    .map(preset => {
      const exe = editorExePath(preset, config);
      try {
        return { preset, mtimeMs: fs.statSync(exe).mtimeMs };
      } catch {
        return null; // No built Editor.exe for this preset.
      }
    })
    .filter((x): x is { preset: string; mtimeMs: number } => x !== null);

  if (built.length > 0) {
    built.sort((a, b) => b.mtimeMs - a.mtimeMs); // Newest-built first.
    return built[0].preset;
  }

  // No built editor anywhere: fall back to the first existing build *directory*
  // (configured but not yet built), else the daily-driver preset as best guess.
  for (const preset of candidates) {
    if (fs.existsSync(path.join(cwd, "build", preset))) return preset;
  }
  return candidates[0];
}

// Check if a preset is a non-preset (macOS/Linux custom build dir).
export function isCustomBuild(preset: string): boolean {
  return preset.startsWith("__");
}

// Get the cmake build arguments for a given preset or custom build.
export function buildArgs(preset: string, config: string, target: string, jobs: string): string[] {
  if (preset === "__macos_ninja__") {
    return ["--build", "build-macos", "--target", target, "-j", jobs];
  }
  if (preset === "__macos_xcode__") {
    return ["--build", "build-xcode", "--config", config, "--target", target, "-j", jobs];
  }
  if (preset === "__linux_ninja__") {
    return ["--build", "build-linux", "--target", target, "-j", jobs];
  }
  return ["--build", `build/${preset}`, "--config", config, "--target", target, "-j", jobs];
}

// Get the CTest arguments for a given preset or custom build.
export function ctestArgs(preset: string, config: string, testFilter?: string): string[] {
  if (preset === "__macos_ninja__") {
    const args = ["--test-dir", "build-macos", "--output-on-failure"];
    if (testFilter) args.push("-R", testFilter);
    return args;
  }
  if (preset === "__macos_xcode__") {
    const args = ["--test-dir", "build-xcode", "--output-on-failure", "-C", config];
    if (testFilter) args.push("-R", testFilter);
    return args;
  }
  if (preset === "__linux_ninja__") {
    const args = ["--test-dir", "build-linux", "--output-on-failure"];
    if (testFilter) args.push("-R", testFilter);
    return args;
  }
  const args = ["--test-dir", `build/${preset}`, "--output-on-failure", "-C", config];
  if (testFilter) args.push("-R", testFilter);
  return args;
}

// Detect the editor executable path for a given preset/config.
export function editorExePath(preset: string, config: string): string {
  const cwd = repoRoot();
  const exeName = process.platform === "win32" ? "Editor.exe" : "Editor";

  if (preset === "__macos_ninja__") return path.join(cwd, "build-macos", "bin", "Apps", "Editor", exeName);
  if (preset === "__macos_xcode__") {
    const macExe = path.join(cwd, "build-xcode", "bin", config, "Apps", "Editor", "Editor.app", "Contents", "MacOS", "Editor");
    if (fs.existsSync(macExe)) return macExe;
    return path.join(cwd, "build-xcode", "bin", config, "Apps", "Editor", exeName);
  }
  if (preset === "__linux_ninja__") return path.join(cwd, "build-linux", "bin", "Apps", "Editor", exeName);

  const editorDir = path.join(cwd, "build", preset, "bin", config, "Apps", "Editor");
  if (process.platform === "darwin") {
    const macExe = path.join(editorDir, "Editor.app", "Contents", "MacOS", "Editor");
    if (fs.existsSync(macExe)) return macExe;
  }
  return path.join(editorDir, exeName);
}
