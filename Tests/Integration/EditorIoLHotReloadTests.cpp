// Editor InitializeOnLoad + HotReload end-to-end tests.
//
// - WindowsEditorHotReload: runs the native Editor.exe, stages TestInit.cs
//   with v1/v2 markers, and verifies that InitializeOnLoad Boot() runs on
//   initial load and after hot reload.
// - LinuxEditorIoL_Docker: on Windows, optionally invokes the existing
//   Docker-based Linux Editor IoL harness and surfaces its result via gtest.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <cstdlib>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace
{

#ifdef _WIN32

std::filesystem::path GetExeDirectory()
{
    wchar_t buffer[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (len == 0 || len == MAX_PATH)
    {
        return {};
    }
    return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path GetBuildDirectoryFromExe()
{
    std::filesystem::path exeDir = GetExeDirectory();
    if (exeDir.empty())
    {
        return {};
    }

    // Contract: return the CMake binary dir the exe was built into, derived
    // exe-anchored from the canonical output layout
    //   <binary-dir>/bin/<Config>[/Tests]/EditorIoLHotReloadTests.exe
    // where <binary-dir> is build/<preset> for preset configures (or any
    // -B dir). Validated below via CMakeCache.txt so a relocated exe returns
    // empty instead of a nonsense ancestor; dev-tree discovery on top of this
    // is FindDevTreeRootAbove's marker walk.
    std::filesystem::path configDir;
    if (exeDir.filename() == "Tests")
    {
        // exeDir = .../bin/<config>/Tests -> configDir = .../bin/<config>
        configDir = exeDir.parent_path();
    }
    else
    {
        // exeDir = .../bin/<config>
        configDir = exeDir;
    }

    std::filesystem::path binDir = configDir.parent_path(); // .../bin
    std::filesystem::path buildDir = binDir.parent_path();  // <binary-dir>
    std::error_code ec;
    if (!std::filesystem::exists(buildDir / "CMakeCache.txt", ec))
    {
        return {}; // relocated exe: no build tree above it
    }
    return buildDir;
}

// DEV-TREE EXEMPTION (deliberate, visible): this suite drives the repo-dev
// Editor and reads repo paths (Tools/Scripts harnesses, Logs/). It is the
// sanctioned exception to the "never climb out of the staged build output"
// rule — the climb validates repo markers and every caller SKIPS when they
// are absent (relocated or shipped build outputs are not dev trees).
std::filesystem::path FindDevTreeRootAbove(std::filesystem::path dir)
{
    std::error_code ec;
    for (int up = 0; up < 3 && !dir.empty(); ++up)
    {
        dir = dir.parent_path();
        if (std::filesystem::exists(dir / "CMakePresets.json", ec) &&
            std::filesystem::exists(dir / "Apps" / "Editor", ec))
        {
            return dir;
        }
    }
    return {};
}

std::filesystem::path GetRepoRootFromExe()
{
    return FindDevTreeRootAbove(GetBuildDirectoryFromExe());
}

void LogTestMessage(const std::string& message)
{
	// Simple helper so all high-level test logs share a consistent prefix.
	// These messages are primarily surfaced when the test fails (ctest
	// --output-on-failure), or when the test binary is run directly.
	std::cout << "[EditorIoL] " << message << std::endl;
}

	// Helper to log wide Windows command lines as UTF-8 without relying on
	// lossy wchar_t -> char narrowing conversions that trigger warnings.
	std::string WideToUtf8(const std::wstring& value)
	{
		if (value.empty())
		{
			return {};
		}

		int sizeNeeded = WideCharToMultiByte(CP_UTF8,
		                                    0,
		                                    value.data(),
		                                    static_cast<int>(value.size()),
		                                    nullptr,
		                                    0,
		                                    nullptr,
		                                    nullptr);
		if (sizeNeeded <= 0)
		{
			return {};
		}

		std::string result(static_cast<size_t>(sizeNeeded), '\0');
		WideCharToMultiByte(CP_UTF8,
		                   0,
		                   value.data(),
		                   static_cast<int>(value.size()),
		                   result.data(),
		                   sizeNeeded,
		                   nullptr,
		                   nullptr);
		return result;
	}

bool WaitForMarkerInFile(const std::filesystem::path& logPath,
                         const std::string& marker,
                         int timeoutSeconds,
                         HANDLE processHandle)
{
    using namespace std::chrono;

    auto deadline = steady_clock::now() + seconds(timeoutSeconds);

    while (steady_clock::now() < deadline)
    {
        if (processHandle != nullptr)
        {
            DWORD exitCode = 0;
            if (GetExitCodeProcess(processHandle, &exitCode) && exitCode != STILL_ACTIVE)
            {
                ADD_FAILURE() << "Editor process exited early with code " << exitCode
                              << " while waiting for marker '" << marker << "'";
                return false;
            }
        }

	        if (std::filesystem::exists(logPath))
	        {
	            std::ifstream in(logPath, std::ios::in | std::ios::binary);
	            std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	            if (content.find(marker) != std::string::npos)
	            {
	                LogTestMessage("Found marker '" + marker + "' in " + logPath.string());
	                return true;
	            }
	        }

        std::this_thread::sleep_for(seconds(1));
    }

    ADD_FAILURE() << "Timed out after " << timeoutSeconds << " seconds waiting for marker '"
                  << marker << "' in " << logPath.string();
    return false;
}

struct ScopedTestInitRestore
{
    std::filesystem::path Path;
    std::string OriginalContent;
    bool HasOriginal{false};

    ~ScopedTestInitRestore()
    {
        if (Path.empty())
        {
            return;
        }

        std::error_code ec;
        if (HasOriginal)
        {
            std::ofstream out(Path, std::ios::out | std::ios::binary | std::ios::trunc);
            if (out.is_open())
            {
                out.write(OriginalContent.data(), static_cast<std::streamsize>(OriginalContent.size()));
            }
        }
        else
        {
            std::filesystem::remove(Path, ec);
        }
    }
};

struct ScopedProcess
{
    HANDLE Handle{nullptr};

    ~ScopedProcess()
    {
        if (Handle != nullptr)
        {
            DWORD exitCode = 0;
            if (GetExitCodeProcess(Handle, &exitCode) && exitCode == STILL_ACTIVE)
            {
                // Best-effort termination; Vulkan validation noise is tolerated elsewhere.
                TerminateProcess(Handle, 1);
                WaitForSingleObject(Handle, 10000);
            }
            CloseHandle(Handle);
        }
    }
};

constexpr const char* kTestInitBeforeSource = R"CSHARP(using System;
	using System.IO;
	using GameEngine.Scripting;
	
	namespace GameScripts
	{
	    public static class SampleInit
	    {
	        [InitializeOnLoad]
	        public static void Boot()
	        {
	            string tracePath = Environment.GetEnvironmentVariable("GE_HOTRELOAD_IOL_TRACE_FILE") ?? "<null>";
	            string marker = $"[Test] InitializeOnLoad Boot() called v1 (GE_HOTRELOAD_IOL_TRACE_FILE='{tracePath}')";
	            Console.WriteLine(marker);
	
	            if (!string.IsNullOrEmpty(tracePath) && !string.Equals(tracePath, "<null>", StringComparison.Ordinal))
	            {
	                try
	                {
	                    Directory.CreateDirectory(Path.GetDirectoryName(tracePath)!);
	                    File.AppendAllText(tracePath, marker + Environment.NewLine);
	                }
	                catch
	                {
	                }
	            }
	        }
	    }
	}
	)CSHARP";
	
constexpr const char* kTestInitAfterSource = R"CSHARP(using System;
	using System.IO;
	using GameEngine.Scripting;
	
	namespace GameScripts
	{
	    public static class SampleInit
	    {
	        [InitializeOnLoad]
	        public static void Boot()
	        {
	            string tracePath = Environment.GetEnvironmentVariable("GE_HOTRELOAD_IOL_TRACE_FILE") ?? "<null>";
	            string marker = $"[Test] InitializeOnLoad Boot() called v2 (GE_HOTRELOAD_IOL_TRACE_FILE='{tracePath}')";
	            Console.WriteLine(marker);
	
	            if (!string.IsNullOrEmpty(tracePath) && !string.Equals(tracePath, "<null>", StringComparison.Ordinal))
	            {
	                try
	                {
	                    Directory.CreateDirectory(Path.GetDirectoryName(tracePath)!);
	                    File.AppendAllText(tracePath, marker + Environment.NewLine);
	                }
	                catch
	                {
	                }
	            }
	        }
	    }
	}
	)CSHARP";
	 
#endif // _WIN32

} // namespace


#ifdef _WIN32

TEST(EditorIoL, WindowsEditorHotReload)
{
    std::filesystem::path buildDir = GetBuildDirectoryFromExe();
    if (buildDir.empty())
        GTEST_SKIP() << "Dev-tree-only integration test: executable is not inside a CMake build tree.";

    std::filesystem::path exeDir = GetExeDirectory();
    // exeDir is expected to be <build>/bin/<config>
    std::filesystem::path configDir = exeDir;
    std::string configName = configDir.filename().string();
	LogTestMessage("Resolved build directory: " + buildDir.string());
	LogTestMessage("Resolved test executable directory: " + exeDir.string());
	LogTestMessage("Configuration name inferred from exe directory: " + configName);

    std::filesystem::path editorPath = buildDir / "bin" / configName / "Editor.exe";
    if (!std::filesystem::exists(editorPath))
    {
	    LogTestMessage("Editor.exe not found at: " + editorPath.string() + "; skipping test.");
	    GTEST_SKIP() << "Editor.exe not found at " << editorPath.string()
	                 << "; build the Editor before running this test.";
    }

    std::filesystem::path repoRoot = GetRepoRootFromExe();
    if (repoRoot.empty())
        GTEST_SKIP() << "Dev-tree-only integration test: no engine dev tree found above the build output (repo markers absent).";
	LogTestMessage("Repo root resolved to: " + repoRoot.string());

    std::filesystem::path logsDir = repoRoot / "Logs";
    std::error_code ec;
    std::filesystem::create_directories(logsDir, ec);

    std::filesystem::path editorLog = logsDir / "Editor-Windows-IoL-HotReload.log";
    std::filesystem::path iolTrace = logsDir / "Editor-Windows-IoL-HotReload-ioltrace.log";
	LogTestMessage("Editor logfile path: " + editorLog.string());
	LogTestMessage("IoL trace file path: " + iolTrace.string());

    std::filesystem::remove(editorLog, ec);
    std::filesystem::remove(iolTrace, ec);

    std::filesystem::path runtimeAssetsDir = buildDir / "bin" / configName / "Assets";
    std::filesystem::create_directories(runtimeAssetsDir, ec);
    std::filesystem::path runtimeTestInitPath = runtimeAssetsDir / "TestInit.cs";
	LogTestMessage("Runtime Assets directory: " + runtimeAssetsDir.string());
	LogTestMessage("Staging TestInit.cs at: " + runtimeTestInitPath.string());

    // Backup original TestInit.cs content (if any)
    ScopedTestInitRestore restore;
    restore.Path = runtimeTestInitPath;
    if (std::filesystem::exists(runtimeTestInitPath))
    {
        std::ifstream in(runtimeTestInitPath, std::ios::in | std::ios::binary);
        restore.OriginalContent.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        restore.HasOriginal = true;
	    LogTestMessage("Backed up existing TestInit.cs before test.");
    }

    // Ensure scripts assembly will be rebuilt from staged sources
    std::filesystem::path scriptsDir = buildDir / "bin" / configName / "ScriptAssemblies";
    std::filesystem::path scriptsDllPath = scriptsDir / "GameEngine.Scripts.dll";
    std::filesystem::path scriptsPdbPath = scriptsDir / "GameEngine.Scripts.pdb";
    std::filesystem::remove(scriptsDllPath, ec);
    std::filesystem::remove(scriptsPdbPath, ec);
	LogTestMessage("Cleared any existing script assemblies under: " + scriptsDir.string());

    // Stage initial (v1) version into the runtime Assets folder
    {
        std::ofstream out(runtimeTestInitPath, std::ios::out | std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open()) << "Failed to open " << runtimeTestInitPath.string() << " for writing.";
        out.write(kTestInitBeforeSource, static_cast<std::streamsize>(std::strlen(kTestInitBeforeSource)));
    }
	LogTestMessage("Staged v1 TestInit.cs into Assets (version with 'Boot() called v1' marker).");

    // Configure IoL/HotReload environment for the Editor process
    const std::wstring iolTraceWide = iolTrace.wstring();
    _wputenv_s(L"GE_HOTRELOAD_IOL_TRACE_FILE", iolTraceWide.c_str());
    _wputenv_s(L"GE_HOTRELOAD_VERBOSE", L"1");
	LogTestMessage("Set GE_HOTRELOAD_IOL_TRACE_FILE to: " + iolTrace.string());
	LogTestMessage("Set GE_HOTRELOAD_VERBOSE=1 for detailed HotReload logs.");

    // Launch Editor
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

		std::wstring cmdLine = L"\"" + editorPath.wstring() + L"\" -logfile \"" + editorLog.wstring() + L"\"";
		LogTestMessage("Launching Editor process: " + WideToUtf8(cmdLine));

    BOOL created = CreateProcessW(
        nullptr,
        cmdLine.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        editorPath.parent_path().wstring().c_str(),
        &si,
        &pi);

	ASSERT_TRUE(created != FALSE) << "Failed to start Editor.exe, GetLastError=" << GetLastError();
	LogTestMessage("Editor process started successfully; waiting for v1 InitializeOnLoad Boot() marker...");

    ScopedProcess process;
    process.Handle = pi.hProcess;
    CloseHandle(pi.hThread);

	// Initial IoL Boot (v1)
	ASSERT_TRUE(WaitForMarkerInFile(iolTrace,
	                                "InitializeOnLoad Boot() called v1",
	                                120,
	                                process.Handle));
	LogTestMessage("Observed v1 InitializeOnLoad Boot() marker; applying TestInit.cs hot-reload change.");

    // Apply hot-reload change (v2)
    {
        std::ofstream out(runtimeTestInitPath, std::ios::out | std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.is_open()) << "Failed to open " << runtimeTestInitPath.string() << " for writing (v2).";
        out.write(kTestInitAfterSource, static_cast<std::streamsize>(std::strlen(kTestInitAfterSource)));
    }
	LogTestMessage("Staged v2 TestInit.cs into Assets (version with 'Boot() called v2' marker).");

    // Hot-reload IoL Boot (v2)
    ASSERT_TRUE(WaitForMarkerInFile(iolTrace,
                                    "InitializeOnLoad Boot() called v2",
                                    240,
                                    process.Handle));
	LogTestMessage("Observed v2 InitializeOnLoad Boot() marker; IoL + HotReload sequence completed.");
}

// Verifies that a constant value in a C# script is observable after hot-reload.
// Stages a [InitializeOnLoad] script with SPEED=5, waits for it to emit "speed=5"
// to the IoL trace file, then overwrites the script with SPEED=50 and waits for
// "speed=50" to appear — proving the compile server produced a new assembly and
// HotReloadManager swapped it in.
TEST(EditorIoL, WindowsEditorHotReload_ConstantChange)
{
    std::filesystem::path buildDir = GetBuildDirectoryFromExe();
    if (buildDir.empty())
        GTEST_SKIP() << "Dev-tree-only integration test: executable is not inside a CMake build tree.";

    std::filesystem::path exeDir = GetExeDirectory();
    std::filesystem::path configDir = exeDir;
    std::string configName = configDir.filename().string();

    std::filesystem::path editorPath = buildDir / "bin" / configName / "Editor.exe";
    if (!std::filesystem::exists(editorPath))
    {
        LogTestMessage("Editor.exe not found at: " + editorPath.string() + "; skipping test.");
        GTEST_SKIP() << "Editor.exe not found at " << editorPath.string();
    }

    std::filesystem::path repoRoot = GetRepoRootFromExe();
    if (repoRoot.empty())
        GTEST_SKIP() << "Dev-tree-only integration test: no engine dev tree found above the build output (repo markers absent).";

    // Use a dedicated test project directory to avoid F.20 overlap issues.
    std::filesystem::path testProjectDir = buildDir / "bin" / configName / "TestProjects" / "ConstantChange";
    std::filesystem::path testAssetsDir = testProjectDir / "Assets";
    std::error_code ec;
    std::filesystem::create_directories(testAssetsDir, ec);

    std::filesystem::path logsDir = repoRoot / "Logs";
    std::filesystem::create_directories(logsDir, ec);

    std::filesystem::path editorLog = logsDir / "Editor-Windows-IoL-ConstantChange.log";
    std::filesystem::path iolTrace = logsDir / "Editor-Windows-IoL-ConstantChange-ioltrace.log";
    std::filesystem::remove(editorLog, ec);
    std::filesystem::remove(iolTrace, ec);

    // Clean any pre-existing script assemblies so the editor does a fresh compile.
    std::filesystem::path scriptsDir = testProjectDir / "ScriptAssemblies";
    std::filesystem::remove_all(scriptsDir, ec);

    // Clean up the test project directory when the test exits (success or failure).
    struct ScopedDirCleanup
    {
        std::filesystem::path Path;
        ~ScopedDirCleanup()
        {
            if (!Path.empty())
            {
                std::error_code ec;
                std::filesystem::remove_all(Path, ec);
            }
        }
    };
    ScopedDirCleanup projectCleanup;
    projectCleanup.Path = testProjectDir;

    // Stage v1: SPEED=5
    std::filesystem::path scriptPath = testAssetsDir / "SpeedTest.cs";
    auto writeSpeedScript = [&scriptPath](const char* speedValue) -> bool
    {
        std::string src = std::string(R"(using System;
using System.IO;
using GameEngine.Scripting;
namespace SpeedTest {
    public static class SpeedInit {
        private const float SPEED = )") + speedValue + R"(f;
        [InitializeOnLoad]
        public static void Boot() {
            string tracePath = Environment.GetEnvironmentVariable("GE_HOTRELOAD_IOL_TRACE_FILE") ?? "";
            string marker = $"[SpeedTest] speed={SPEED}";
            Console.WriteLine(marker);
            if (!string.IsNullOrEmpty(tracePath)) {
                try {
                    Directory.CreateDirectory(Path.GetDirectoryName(tracePath)!);
                    File.AppendAllText(tracePath, marker + Environment.NewLine);
                } catch { }
            }
        }
    }
})";
        std::ofstream out(scriptPath, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out.write(src.data(), static_cast<std::streamsize>(src.size()));
        return true;
    };

    ASSERT_TRUE(writeSpeedScript("5")) << "Failed to write " << scriptPath.string();
    LogTestMessage("Staged SpeedTest.cs with SPEED=5");

    // Configure environment
    const std::wstring iolTraceWide = iolTrace.wstring();
    _wputenv_s(L"GE_HOTRELOAD_IOL_TRACE_FILE", iolTraceWide.c_str());
    _wputenv_s(L"GE_HOTRELOAD_VERBOSE", L"1");

    // Launch Editor with --project pointing to the test project
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::wstring cmdLine = L"\"" + editorPath.wstring()
        + L"\" --project \"" + testProjectDir.wstring()
        + L"\" -logfile \"" + editorLog.wstring() + L"\"";
    LogTestMessage("Launching: " + WideToUtf8(cmdLine));

    BOOL created = CreateProcessW(
        nullptr, cmdLine.data(), nullptr, nullptr, FALSE, 0, nullptr,
        editorPath.parent_path().wstring().c_str(), &si, &pi);
    ASSERT_TRUE(created != FALSE) << "Failed to start Editor.exe";

    ScopedProcess process;
    process.Handle = pi.hProcess;
    CloseHandle(pi.hThread);

    // Wait for v1 marker: "speed=5"
    ASSERT_TRUE(WaitForMarkerInFile(iolTrace, "[SpeedTest] speed=5", 120, process.Handle))
        << "Timed out waiting for initial speed=5 marker";
    LogTestMessage("Observed speed=5 marker; applying hot-reload change to speed=50");

    // Apply hot-reload change: SPEED=50
    ASSERT_TRUE(writeSpeedScript("50")) << "Failed to write " << scriptPath.string();
    LogTestMessage("Staged SpeedTest.cs with SPEED=50");

    // Wait for v2 marker: "speed=50"
    ASSERT_TRUE(WaitForMarkerInFile(iolTrace, "[SpeedTest] speed=50", 240, process.Handle))
        << "Timed out waiting for hot-reloaded speed=50 marker";
    LogTestMessage("Observed speed=50 marker; hot-reload constant change verified.");
}


TEST(EditorIoL, LinuxEditorIoL_Docker)
{
    // This test is intentionally Windows-only: it shells out to the existing
    // Docker-based Linux Editor IoL harness. If Docker is not available or
    // not running, we skip the test rather than failing the suite.

    std::filesystem::path repoRoot = GetRepoRootFromExe();
    if (repoRoot.empty())
        GTEST_SKIP() << "Dev-tree-only integration test: no engine dev tree found above the build output (repo markers absent).";

    std::filesystem::path scriptPath = repoRoot / "Tools" / "Scripts" / "run-linux-editor-iol-test.ps1";
    if (!std::filesystem::exists(scriptPath))
    {
        GTEST_SKIP() << "run-linux-editor-iol-test.ps1 not found at " << scriptPath.string()
                     << "; skipping Linux Editor IoL Docker test.";
    }

    // Quick availability check: docker info should succeed when Docker CLI and
    // daemon are both available.
    int dockerInfo = _wsystem(L"docker info >nul 2>&1");
    if (dockerInfo != 0)
    {
        GTEST_SKIP() << "Docker does not appear to be available or running (docker info exit code "
                     << dockerInfo << "); skipping Linux Editor IoL Docker test.";
    }

    std::wstring cmd = L"powershell -NoLogo -NoProfile -ExecutionPolicy Bypass -File \"" + scriptPath.wstring() + L"\"";
	int result = _wsystem(cmd.c_str());

	ASSERT_EQ(result, 0)
	    << "run-linux-editor-iol-test.ps1 failed with exit code " << result
	    << "; see build-linux-docker/linux_editor_iol.log for details.";
}
 
#endif // _WIN32

#if defined(__APPLE__)

namespace
{

std::filesystem::path GetExeDirectory_Mac()
{
	uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	if (size == 0)
	{
		return {};
	}

	std::vector<char> buffer(size);
	if (_NSGetExecutablePath(buffer.data(), &size) != 0)
	{
		return {};
	}

	std::error_code ec;
	std::filesystem::path exePath = std::filesystem::weakly_canonical(std::filesystem::path(buffer.data()), ec);
	if (ec)
	{
		return {};
	}

	return exePath.parent_path();
}

std::filesystem::path GetBuildDirectoryFromExe_Mac()
{
	std::filesystem::path exeDir = GetExeDirectory_Mac();
	if (exeDir.empty())
	{
		return {};
	}

	// Mirror the Windows layout assumptions:
	//   - <repo>/build-macos/bin/<config>/EditorIoLHotReloadTests
	//   - <repo>/build-macos/bin/<config>/Tests/EditorIoLHotReloadTests
	std::filesystem::path configDir;
	if (exeDir.filename() == "Tests")
	{
		configDir = exeDir.parent_path();
	}
	else
	{
		configDir = exeDir;
	}

	std::filesystem::path binDir = configDir.parent_path();  // .../bin
	std::filesystem::path buildDir = binDir.parent_path();   // <binary-dir>
	std::error_code ec;
	if (!std::filesystem::exists(buildDir / "CMakeCache.txt", ec))
	{
		return {}; // relocated exe: no build tree above it
	}
	return buildDir;
}

std::filesystem::path GetRepoRootFromExe_Mac()
{
	// Same dev-tree exemption + marker validation as GetRepoRootFromExe.
	return FindDevTreeRootAbove(GetBuildDirectoryFromExe_Mac());
}

} // namespace


TEST(EditorIoL, MacEditorIoL_HotReloadShellHarness)
{
	namespace fs = std::filesystem;

	fs::path repoRoot = GetRepoRootFromExe_Mac();
	if (repoRoot.empty())
		GTEST_SKIP() << "Dev-tree-only integration test: no engine dev tree found above the build output (repo markers absent).";

	fs::path scriptPath = repoRoot / "Tools" / "Scripts" / "run-macos-editor-hotreload-iol-test.sh";
	if (!fs::exists(scriptPath))
	{
		GTEST_SKIP() << "run-macos-editor-hotreload-iol-test.sh not found at " << scriptPath.string()
		             << "; skipping macOS Editor IoL hot-reload shell harness test.";
	}

	std::string cmd = "bash \"" + scriptPath.string() + "\"";
	int result = std::system(cmd.c_str());

	ASSERT_EQ(result, 0)
	    << "run-macos-editor-hotreload-iol-test.sh failed with exit code " << result
	    << "; see Logs/Editor-macOS-IoL-HotReload.log and Logs/Editor-macOS-IoL-HotReload-ioltrace.log for details.";
}

#endif // defined(__APPLE__)
