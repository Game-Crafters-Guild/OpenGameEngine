#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine {
namespace Platform {

// Open a URL in the default web browser.
// Returns true on success, false on failure.
bool OpenUrl(const std::string& url);

// Open a file or directory with the OS default handler.
// For directories, this typically opens the folder in the system file manager.
// Returns true on success, false on failure.
bool OpenPath(const std::filesystem::path& path);

// Reveal a path in the OS file manager.
// For files, this attempts to open the containing folder and select the file
// when the platform supports it. For directories, it opens the directory
// itself.
bool ShowInFileManager(const std::filesystem::path& path);

// Launch an executable with arguments and return immediately.
// Arguments are passed without shell interpretation.
bool LaunchDetached(const std::filesystem::path& executable,
                    const std::vector<std::string>& arguments = {},
                    const std::filesystem::path& workingDirectory = {});

// Launch an executable and return its OS process ID on platforms that support
// it (macOS/Linux: posix_spawn pid_t cast to int). Returns 0 on failure or on
// platforms without PID tracking (Windows).
int LaunchDetachedGetPid(const std::filesystem::path& executable,
                         const std::vector<std::string>& arguments = {},
                         const std::filesystem::path& workingDirectory = {});

// Returns true if the process with the given PID is still alive.
// Always returns false for pid <= 0 or on platforms without support (web).
bool IsProcessRunning(int pid);

	// Open a C# script file in the user's preferred external editor together with a
	// companion project file (typically an auto-generated .csproj) that provides
	// proper project context (IntelliSense, references, etc.).
	//
	// Behaviour:
	//  - On Windows, we inspect the OS file associations for .csproj/.sln/.cs to
	//    discover the default editor (Visual Studio, VS Code, Rider). For known
	//    editors we launch them explicitly with both the project and script paths
	//    as arguments so the file opens inside the correct project context.
	//  - On other platforms, or when we cannot determine/recognise the editor,
	//    this function gracefully falls back to opening the script with OpenPath
	//    and ignores the projectPath parameter.
	//
	// Returns true when an editor process was successfully started (or when the
	// fallback OpenPath(scriptPath) succeeds), false otherwise.
	bool OpenScriptWithProject(const std::filesystem::path& scriptPath,
	                          const std::filesystem::path& projectPath);

	// Open a native C/C++ source file in the user's IDE. Like OpenScriptWithProject, but with
	// C++-appropriate launch arguments: Visual Studio uses /Edit so the file opens in the running
	// instance and is focused (a CMake project has no .sln to attach the file to, so passing a
	// project path would just focus that instead and spawn a second window); VS Code reuses its
	// window, opens the project folder, and jumps to the file; Rider opens the file in its running
	// instance. projectDir is the native source root (folder + working dir). Falls back to OpenPath.
	bool OpenSourceWithProject(const std::filesystem::path& sourcePath,
	                          const std::filesystem::path& projectDir);

	// Whether SelectFolder can present a picker at all. Always true on desktop.
	// On web it requires the File System Access API (showDirectoryPicker):
	// present in Chrome/Edge, absent in Firefox and Safari, and disabled by
	// default in Brave. UI offers a browse affordance only when this is true —
	// SelectFolder's empty return cannot distinguish "cancelled" from
	// "impossible".
	bool SupportsFolderPicker();

	// Show a native folder selection dialog and return the selected folder path.
	// Returns an empty path if the user cancelled or an error occurred.
	// The initialPath parameter is optional and can be used to set the initial
	// directory shown in the dialog.
	std::filesystem::path SelectFolder(const std::filesystem::path& initialPath = {});

    // Show a native file selection dialog and return the selected file path.
    // Returns an empty path if the user cancelled or an error occurred.
    //
    // filterName/filterPattern are optional and are best-effort; on Windows they map
    // to COMDLG_FILTERSPEC (example: "Material Assets", "*.material").
    std::filesystem::path SelectFile(const std::filesystem::path& initialPath = {},
                                     const char* filterName = nullptr,
                                     const char* filterPattern = nullptr);

    // Show a native file selection dialog that allows one or more files.
    // Returns an empty vector if the user cancelled or an error occurred.
    // filterName/filterPattern follow the same best-effort rules as SelectFile.
    std::vector<std::filesystem::path> SelectFiles(const std::filesystem::path& initialPath = {},
                                                   const char* filterName = nullptr,
                                                   const char* filterPattern = nullptr);

    // Show a native save file dialog and return the selected file path.
    // Returns an empty path if the user cancelled or an error occurred.
    //
    // filterName/filterPattern are optional and are best-effort; on Windows they map
    // to COMDLG_FILTERSPEC (example: "Navigation Grid", "*.navgrid").
    std::filesystem::path SaveFile(const std::filesystem::path& initialPath = {},
                                   const char* filterName = nullptr,
                                   const char* filterPattern = nullptr);

    // Show a native, OS-level modal dialog with a title, a message, and 1..4
    // custom-labeled buttons; returns the 0-based index of the pressed button, or
    // `defaultButton` if the dialog could not be shown or was dismissed. BLOCKS the
    // calling thread (must be the UI/main thread) and does NOT touch the GPU — this is
    // the terminal device-loss surface, drawn when the engine's own GPU-rendered UI
    // cannot render (a lost/failed graphics device). Windows uses TaskDialog for custom
    // labels with a MessageBox fallback; other platforms log and return defaultButton.
    int ShowNativeChoiceDialog(const std::string& title, const std::string& message,
                               const std::vector<std::string>& buttons, int defaultButton = 0);

    // Absolute path to the currently running executable (Win32 GetModuleFileNameW /
    // Linux /proc/self/exe / macOS _NSGetExecutablePath). Empty on failure. Used to
    // relaunch the app after an unrecoverable device loss (Option-3: real TDR needs a
    // process restart).
    std::filesystem::path GetExecutablePath();

    // Move a file or folder to the OS trash / Recycle Bin so the user can recover it
    // outside the editor. Returns true if the item was removed from its original
    // location via the trash. False means the caller should fall back to a normal
    // delete (or keep a staging copy for undo).
    bool MoveToTrash(const std::filesystem::path& path);

    // Release files a picker or an OS drop handed the application through
    // transient staging, once the application has copied what it needs. On
    // desktop the paths are the user's own files and nothing is released. On
    // web, a drop or a file pick is copied into in-memory staging first, and
    // that staging leaks unless it is removed here.
    void ReleaseTransientFiles(const std::vector<std::filesystem::path>& paths);

} // namespace Platform
} // namespace GameEngine
