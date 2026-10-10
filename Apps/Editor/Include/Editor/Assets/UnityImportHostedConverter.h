#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// Part of the EditorSDK surface (compiled into EditorSDK.dll) rather than the
// unity-import package module: the implementation includes the scripting-host
// headers, whose types are layout-sensitive to NETHOST_AVAILABLE — a define
// the staged-SDK module build shape does not carry. This header is
// deliberately std-only so package modules consume the seam ABI-safely.

namespace GameEngine {

// Render a path for use in an args list. Converter argv strings are treated
// as UTF-8 end to end ('\0'-separated block decoded by the managed side);
// path::string() would narrow through the ANSI code page on Windows and
// mangle non-ASCII path components (usernames, project names). Generic
// (forward-slash) form — the converter accepts it on every platform. Inline
// and header-only so package modules linking only Engine + EditorSDK get the
// definition without an Editor.exe-resident symbol.
inline std::string PathArgUtf8(const std::filesystem::path& path)
{
    const std::u8string utf8 = path.generic_u8string();
    return std::string(utf8.begin(), utf8.end());
}

// Result of a hosted converter run.
struct ConverterRunResult {
    bool Spawned = false;   // false if the hosted entry could not be invoked
    int ExitCode = -1;      // converter exit code (valid when Spawned)
    std::string StderrTail; // bounded tail of converter stderr (diagnostics)
};

// Exit code the managed side returns when a run unwinds because the editor
// requested cancellation. Keep in sync with kExitCancelled in
// dotnet/UnityConverter/EditorHost.cs of the OpenEngine-Unity-Scene-Converter
// repository, which builds the vendored assembly.
inline constexpr int kConverterCancelExitCode = 130;

// Run the C# converter (UnityConverter.dll) in-process on the engine-hosted
// CoreCLR: resolves the EditorHost.Run [UnmanagedCallersOnly] entry via the
// scripting host and executes the conversion on the CALLING thread — run this
// on a worker thread and marshal UI updates back via UIElement::PostAction.
// Converter output streams back through a write callback: stdout is split
// into complete lines for onStdoutLine (same contract as the retired
// subprocess path), stderr accumulates into the bounded StderrTail.
// cancelRequested is checked at every converter output line; once set the
// managed side unwinds and the call returns kConverterCancelExitCode. The
// bound is line-granular: a silent compute stretch (e.g. a large texture
// transcode) is an uncancellable window of that stretch's duration — still
// strictly better than the retired subprocess path, which had no cancel and
// waited out the entire conversion. The flag must stay set once raised
// (sticky) — see the cancellation contract note in EditorHost.cs.
// Spawned=false (with a reason in StderrTail) when the DLL is missing or the
// scripting runtime is unavailable.
ConverterRunResult RunConverterHosted(
    const std::filesystem::path& converterDll,
    const std::vector<std::string>& args,
    const std::function<void(const std::string&)>& onStdoutLine,
    const std::atomic<bool>& cancelRequested);

} // namespace GameEngine
