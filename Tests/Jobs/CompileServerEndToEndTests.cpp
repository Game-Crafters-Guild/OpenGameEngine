#include <gtest/gtest.h>
#include "CompileServerTestHost.h"
#include "Jobs/CompileServerClient.h"
#include "Jobs/PipeTransport.h"
#include "Jobs/WorkspaceId.h"
#include "Core/Application.h" // PathUtils
#include "Scripting/CoreCLRHost.h"
#include "Scripting/PackageScriptCompile.h"
#include "Scripting/PathResolver.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

using namespace GameEngine;

static std::filesystem::path ResolveRuntimeConfigForCoreClr()
{
    // Optional override for CI / ad-hoc runs.
    if (const char* env = std::getenv("GE_SCRIPTS_RUNTIMECONFIG"))
    {
        std::filesystem::path p(env);
        if (!p.empty() && std::filesystem::exists(p))
            return p;
    }

    // Preferred: resolve relative to the running test executable (and staged outputs).
    std::filesystem::path p = ScriptingPaths::ResolveScriptsRuntimeConfig();
    if (!p.empty() && std::filesystem::exists(p))
        return p;

    // Fallback: accept a runtimeconfig in the current working directory.
    p = std::filesystem::current_path() / "GameEngine.HotReload.runtimeconfig.json";
    if (std::filesystem::exists(p))
        return p;

    return {};
}

static std::string WriteTempCs(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir);
    auto p = dir / "WarningSample.cs";
    std::ofstream f(p);
    f << "using System;\n";
    f << "class X { public int Y() { int unused = 0; return 1; } }\n"; // CS0219 warning only
    f.close();
    return p.string();
}

static std::string WriteBadTempCs(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir);
    auto p = dir / "BadSample.cs";
    std::ofstream f(p);
    f << "using System;\n";
    f << "class Broken { public void M() { Console.WriteLine(1) } }\n"; // missing semicolon -> error
    f.close();
    return p.string();
}

static void BuildAndExpect(const std::string& pipeName, const std::filesystem::path& ws, const std::vector<std::string>& allFiles,
                           bool expectSuccess, bool expectAsm) {
#ifdef _WIN32
    // Ensure server is started explicitly to avoid path issues in CI
    ASSERT_TRUE(CompileServerTestHost::StartTestCompileServer(pipeName));
#endif

    // Use generic (forward-slash) paths to avoid JSON escaping issues on Windows
    std::string wsStr = std::filesystem::path(ws).generic_string();
    std::vector<std::string> filesNorm; filesNorm.reserve(allFiles.size());
    for (auto& p : allFiles) filesNorm.push_back(std::filesystem::path(p).generic_string());

    std::ostringstream json;
    json << "{";
    json << "\"ProjectRoot\":\"" << wsStr << "\",";
    json << "\"ChangedFiles\":[],\"AffectedFiles\":[],\"AllFiles\":[";
    for (size_t i=0;i<filesNorm.size();++i) { if (i) json << ","; json << "\"" << filesNorm[i] << "\""; }
    json << "],\"PreferredStrategy\":\"Incremental\",\"ForceFull\":false}";

    CompileServerClient client(std::unique_ptr<IHotReloadTransport>{}, pipeName);
    CompileServerResponse resp;
    bool ok = client.Compile(json.str(), resp);
    // Compile() returns true only when the managed compile succeeded; for a
    // Success:false response it returns false but still fills the diagnostics.
    ASSERT_EQ(ok, expectSuccess);
    if (resp.Success != expectSuccess) {
        std::ostringstream oss;
        oss << "Errors:";
        for (auto& e : resp.Errors) oss << "\n  " << e.FileUtf8 << ":" << e.Line << "," << e.Column << " " << e.Code << ": " << e.MessageUtf8;
        oss << "\nWarnings:";
        for (auto& w : resp.Warnings) oss << "\n  " << w.FileUtf8 << ":" << w.Line << "," << w.Column << " " << w.Code << ": " << w.MessageUtf8;
        GTEST_FAIL() << "Unexpected success flag. " << oss.str();
    }
    if (expectSuccess) {
        EXPECT_EQ(!resp.AssemblyBytes.empty(), expectAsm);
    } else {
        EXPECT_FALSE(resp.Errors.empty());
        EXPECT_TRUE(resp.AssemblyBytes.empty());
    }
}

TEST(CompileServerEndToEnd, CompilesOneFileWithWarning) {
    std::filesystem::path ws = std::filesystem::temp_directory_path() / "GE_EndToEnd_Workspace";
    std::error_code ec; std::filesystem::create_directories(ws, ec);
    std::string pipeName = CompileServerTestHost::UniquePipeName("Warn");

    std::string file = WriteTempCs(ws);
    BuildAndExpect(pipeName, ws, {file}, /*expectSuccess*/true, /*expectAsm*/true);
}

TEST(CompileServerEndToEnd, FailsOnInvalidSourceAndReturnsErrors) {
    std::filesystem::path ws = std::filesystem::temp_directory_path() / "GE_EndToEnd_Workspace_Bad";
    std::error_code ec; std::filesystem::create_directories(ws, ec);
    std::string pipeName = CompileServerTestHost::UniquePipeName("Bad");

    std::string file = WriteBadTempCs(ws);
    BuildAndExpect(pipeName, ws, {file}, /*expectSuccess*/false, /*expectAsm*/false);
}

TEST(CompileServerEndToEnd, AssemblyLoadSmoke) {
#ifdef _WIN32
    // Arrange: workspace and simple source
    std::filesystem::path ws = std::filesystem::temp_directory_path() / "GE_EndToEnd_Workspace_Load";
    std::error_code ec; std::filesystem::create_directories(ws, ec);
    std::string pipeName = CompileServerTestHost::UniquePipeName("Load");

    std::ofstream f(ws / "Foo.cs");
    f << "public static class Foo {";
    f << " public static int Add(int a,int b){return a+b;}";
    f << " public static int Smoke(){ return 42; }";
    f << " public static int Ping(){ return 7; }";
    f << "}";
    f.close();

    // Create request
    std::string wsStr = ws.generic_string();
    std::string fileStr = (ws / "Foo.cs").generic_string();
    std::ostringstream json;
    json << "{";
    json << "\"ProjectRoot\":\"" << wsStr << "\",";
    json << "\"ChangedFiles\":[],\"AffectedFiles\":[],\"AllFiles\":[\"" << fileStr << "\"],";
    json << "\"PreferredStrategy\":\"Full\",\"ForceFull\":true}";

    // Act: compile
    ASSERT_TRUE(CompileServerTestHost::StartTestCompileServer(pipeName));
    CompileServerClient client(std::unique_ptr<IHotReloadTransport>{}, pipeName);
    CompileServerResponse resp;
    ASSERT_TRUE(client.Compile(json.str(), resp));

    if (!resp.Success) {
        GTEST_SKIP() << "Managed compiler failed in environment: cannot perform load smoke";
    }

    // Write bytes to disk in a temp bin dir
    std::filesystem::path outDir = ws / "bin" / "GE_Temp";
    std::filesystem::create_directories(outDir, ec);
    auto dllPath = outDir / "GameEngine.Scripts.dll";
    std::ofstream df(dllPath, std::ios::binary);
    df.write(reinterpret_cast<const char*>(resp.AssemblyBytes.data()), (std::streamsize)resp.AssemblyBytes.size());
    df.close();
    if (!resp.PdbBytes.empty()) {
        std::ofstream pf(outDir / "GameEngine.Scripts.pdb", std::ios::binary);
        pf.write(reinterpret_cast<const char*>(resp.PdbBytes.data()), (std::streamsize)resp.PdbBytes.size());
    }

    // Init CoreCLR and try to load via HotReloadManager
    CoreCLRHost clr;
    std::filesystem::path runtimeConfig = ResolveRuntimeConfigForCoreClr();

    if (!std::filesystem::exists(runtimeConfig)) {
        GTEST_SKIP() << "CoreCLR runtime config not found; skipping assembly load smoke";
    }

    bool initOk = clr.Initialize(runtimeConfig);
    if (!initOk) {
        GTEST_SKIP() << "CoreCLRHost initialization failed; skipping";
    }

    // Load via the modern preload+swap path (the legacy HotReloadManager load
    // entry points were removed from CoreBridge and return -404).
    bool loadOk = clr.PreloadAndSwapFromPath(dllPath);
    ASSERT_TRUE(loadOk) << "PreloadAndSwapFromPath failed to load compiled assembly (managed error code="
                        << clr.GetLastManagedErrorCode() << ")";

    // Validate actual execution by invoking methods in the loaded assembly via command layer
    int32 execSmoke = clr.CallUserScriptsMethod("Smoke");
    EXPECT_EQ(execSmoke, 42) << "Unexpected Foo.Smoke result";
    int32 execPing = clr.CallUserScriptsMethod("Ping");
    EXPECT_EQ(execPing, 7) << "Unexpected Foo.Ping result";

#else
    GTEST_SKIP() << "Assembly load smoke only runs on Windows in this environment";
#endif
}

TEST(CompileServerEndToEnd, AssemblyReloadSmoke) {
#ifdef _WIN32
    std::filesystem::path ws = std::filesystem::temp_directory_path() / "GE_EndToEnd_Workspace_Reload";
    std::error_code ec; std::filesystem::create_directories(ws, ec);
    std::string pipeName = CompileServerTestHost::UniquePipeName("Reload");

    auto sourcePath = ws / "Foo.cs";

    // Prepare CoreCLRHost once for the whole test
    CoreCLRHost clr;
    std::filesystem::path runtimeConfig = ResolveRuntimeConfigForCoreClr();
    if (!std::filesystem::exists(runtimeConfig)) { GTEST_SKIP() << "HotReload runtimeconfig not found"; }
    ASSERT_TRUE(clr.Initialize(runtimeConfig));

    // v1: Smoke returns 42
    {
        std::ofstream f(sourcePath);
        f << "public static class Foo {";
        f << " public static int Smoke(){ return 42; }";
        f << "}";
        f.close();

        // Compile v1
        ASSERT_TRUE(CompileServerTestHost::StartTestCompileServer(pipeName));
        std::ostringstream json;
        json << "{";
        json << "\"ProjectRoot\":\"" << ws.generic_string() << "\",";
        json << "\"ChangedFiles\":[],\"AffectedFiles\":[],\"AllFiles\":[\"" << sourcePath.generic_string() << "\"],";
        json << "\"PreferredStrategy\":\"Full\",\"ForceFull\":true}";
        CompileServerClient client(std::unique_ptr<IHotReloadTransport>{}, pipeName);
        CompileServerResponse resp;
        ASSERT_TRUE(client.Compile(json.str(), resp));
        ASSERT_TRUE(resp.Success);

        // Write DLL to disk
        std::filesystem::path outDir = ws / "bin" / "GE_Temp";
        std::filesystem::create_directories(outDir, ec);
        auto dllPath = outDir / "GameEngine.Scripts.dll";
        std::ofstream df(dllPath, std::ios::binary);
        df.write(reinterpret_cast<const char*>(resp.AssemblyBytes.data()), (std::streamsize)resp.AssemblyBytes.size());
        df.close();

        // Load v1 via the modern preload+swap path
        ASSERT_TRUE(clr.PreloadAndSwapFromPath(dllPath));
        int32 v1 = clr.CallUserScriptsMethod("Smoke");
        EXPECT_GE(v1, 0);
    }

    // v2: Smoke returns 99
    {
        std::ofstream f(sourcePath);
        f << "public static class Foo {";
        f << " public static int Smoke(){ return 99; }";
        f << "}";
        f.close();

        // Compile v2
        std::ostringstream json2;
        json2 << "{";
        json2 << "\"ProjectRoot\":\"" << ws.generic_string() << "\",";
        json2 << "\"ChangedFiles\":[\"" << sourcePath.generic_string() << "\"],\"AffectedFiles\":[],\"AllFiles\":[\"" << sourcePath.generic_string() << "\"],";
        json2 << "\"PreferredStrategy\":\"Full\",\"ForceFull\":true}";
        CompileServerClient client2(std::unique_ptr<IHotReloadTransport>{}, pipeName);
        CompileServerResponse resp2;
        ASSERT_TRUE(client2.Compile(json2.str(), resp2));
        ASSERT_TRUE(resp2.Success);

        // Overwrite DLL on disk
        std::filesystem::path outDir = ws / "bin" / "GE_Temp";
        std::filesystem::create_directories(outDir, ec);
        auto dllPath = outDir / "GameEngine.Scripts.dll";
        std::ofstream df2(dllPath, std::ios::binary | std::ios::trunc);
        df2.write(reinterpret_cast<const char*>(resp2.AssemblyBytes.data()), (std::streamsize)resp2.AssemblyBytes.size());
        df2.close();

        // RELOAD: preload the new bytes into a fresh context and swap —
        // this is the production hot-reload path.
        bool reloaded = clr.PreloadAndSwapFromPath(dllPath);
        if (!reloaded) {
            int32 managedErr = clr.GetLastManagedErrorCode();
            ADD_FAILURE() << "Reload failed; managed error code=" << managedErr;
        }
        ASSERT_TRUE(reloaded);
        int32 v2 = clr.CallUserScriptsMethod("Smoke");
        EXPECT_EQ(v2, 99) << "Reload did not reflect updated method result";
    }
#else
    GTEST_SKIP() << "Assembly reload smoke only runs on Windows in this environment";
#endif
}

TEST(CompileServerEndToEnd, NegativeCompileReportsDiagnostics) {
#ifdef _WIN32
    // Prepare a synthetic project with an intentional compile error
    std::filesystem::path ws = std::filesystem::temp_directory_path() / "GE_EndToEnd_Workspace_Err";
    std::filesystem::create_directories(ws);
    auto sourcePath = ws / "Bad.cs";
    {
        std::ofstream f(sourcePath);
        f << "public static class Bad { public static int Oops() { return notDefined; } }"; // undefined symbol
        f.close();
    }

    std::string pipeName = CompileServerTestHost::UniquePipeName("Err");
    ASSERT_TRUE(CompileServerTestHost::StartTestCompileServer(pipeName));

    std::ostringstream json;
    json << "{";
    json << "\"ProjectRoot\":\"" << ws.generic_string() << "\",";
    json << "\"ChangedFiles\":[],\"AffectedFiles\":[],\"AllFiles\":[\"" << sourcePath.generic_string() << "\"],";
    json << "\"PreferredStrategy\":\"Full\",\"ForceFull\":true}";

    CompileServerClient client(std::unique_ptr<IHotReloadTransport>{}, pipeName);
    CompileServerResponse resp;
    bool ok = client.Compile(json.str(), resp);
    EXPECT_FALSE(ok);
    EXPECT_FALSE(resp.Success);
    EXPECT_FALSE(resp.Errors.empty());
#else
    GTEST_SKIP() << "Negative compile test is Windows-only in this environment";
#endif
}






TEST(CompileServerEndToEnd, ReloadSoakTenCycles) {
#ifdef _WIN32
    CoreCLRHost clr;
    std::filesystem::path runtimeConfig = ResolveRuntimeConfigForCoreClr();
    if (!std::filesystem::exists(runtimeConfig)) { GTEST_SKIP() << "HotReload runtimeconfig not found"; }
    ASSERT_TRUE(clr.Initialize(runtimeConfig));

    // Prepare script that returns a counter value we update
    std::filesystem::path ws = std::filesystem::temp_directory_path() / "GE_EndToEnd_Workspace_Soak";
    std::filesystem::create_directories(ws);
    auto sourcePath = ws / "Foo.cs";

    auto writeSource = [&](int v){
        std::ofstream f(sourcePath);
        f << "public static class Foo { public static int Val(){ return " << v << "; } }";
        f.close(); };

    std::string pipeName = CompileServerTestHost::UniquePipeName("Soak");
    ASSERT_TRUE(CompileServerTestHost::StartTestCompileServer(pipeName));

    auto buildOnce = [&](){
        std::ostringstream json;
        json << "{";
        json << "\"ProjectRoot\":\"" << ws.generic_string() << "\",";
        json << "\"ChangedFiles\":[],\"AffectedFiles\":[],\"AllFiles\":[\"" << sourcePath.generic_string() << "\"],";
        json << "\"PreferredStrategy\":\"Full\",\"ForceFull\":true}";
        CompileServerClient client(std::unique_ptr<IHotReloadTransport>{}, pipeName);
        CompileServerResponse resp; bool ok = client.Compile(json.str(), resp);
        if (!ok || !resp.Success) { return std::filesystem::path(); }
        auto outDir = ws / "bin" / "GE_Temp"; std::filesystem::create_directories(outDir);
        auto dllPath = outDir / "GameEngine.Scripts.dll";
        std::ofstream df(dllPath, std::ios::binary);
        df.write(reinterpret_cast<const char*>(resp.AssemblyBytes.data()), (std::streamsize)resp.AssemblyBytes.size()); df.close();
        return dllPath; };

    writeSource(1); auto dll = buildOnce(); ASSERT_TRUE(!dll.empty()); ASSERT_TRUE(clr.PreloadAndSwapFromPath(dll)); EXPECT_EQ(clr.CallUserScriptsMethod("Val"), 1);

    for (int i=2;i<=11;++i){
        writeSource(i); dll = buildOnce();
        ASSERT_TRUE(!dll.empty());
        // Reload via the production preload+swap path and assert new behavior
        bool ok = clr.PreloadAndSwapFromPath(dll);
        if (!ok) {
            int err = clr.GetLastManagedErrorCode();
            ADD_FAILURE() << "Reload failed at iteration " << i << ", managed error=" << err;
            break;
        }
        EXPECT_EQ(clr.CallUserScriptsMethod("Val"), i);
    }
#else
    GTEST_SKIP() << "Reload soak test is Windows-only in this environment";
#endif
}


TEST(CompileServerEndToEnd, ManagedExceptionPropagatesAsNegative) {
#ifdef _WIN32
    CoreCLRHost clr;
    std::filesystem::path runtimeConfig = ResolveRuntimeConfigForCoreClr();
    if (!std::filesystem::exists(runtimeConfig)) { GTEST_SKIP() << "HotReload runtimeconfig not found"; }
    ASSERT_TRUE(clr.Initialize(runtimeConfig));

    // Script method throws
    std::filesystem::path ws = std::filesystem::temp_directory_path() / "GE_EndToEnd_Workspace_ErrThrow";
    std::filesystem::create_directories(ws);
    auto sourcePath = ws / "Thrower.cs";
    {
        std::ofstream f(sourcePath);
        f << "using System; public static class Thrower { public static int Boom(){ throw new InvalidOperationException(\"boom\"); } }";
        f.close();
    }

    std::string pipeName = CompileServerTestHost::UniquePipeName("Throw");
    ASSERT_TRUE(CompileServerTestHost::StartTestCompileServer(pipeName));

    std::ostringstream json;
    json << "{";
    json << "\"ProjectRoot\":\"" << ws.generic_string() << "\",";
    json << "\"ChangedFiles\":[],\"AffectedFiles\":[],\"AllFiles\":[\"" << sourcePath.generic_string() << "\"],";
    json << "\"PreferredStrategy\":\"Full\",\"ForceFull\":true}";

    CompileServerClient client(std::unique_ptr<IHotReloadTransport>{}, pipeName);
    CompileServerResponse resp;
    ASSERT_TRUE(client.Compile(json.str(), resp));
    ASSERT_TRUE(resp.Success);
    auto outDir = ws / "bin" / "GE_Temp"; std::filesystem::create_directories(outDir);
    auto dllPath = outDir / "GameEngine.Scripts.dll";
    std::ofstream df(dllPath, std::ios::binary);
    df.write(reinterpret_cast<const char*>(resp.AssemblyBytes.data()), (std::streamsize)resp.AssemblyBytes.size()); df.close();

    ASSERT_TRUE(clr.PreloadAndSwapFromPath(dllPath));
    int32 rc = clr.CallUserScriptsMethod("Boom");
    EXPECT_LT(rc, 0);
#else
    GTEST_SKIP() << "Managed exception propagation test is Windows-only in this environment";
#endif
}


TEST(CompileServerEndToEnd, CallWithArgumentsAddsTwoInts) {
#ifdef _WIN32
    CoreCLRHost clr;
    std::filesystem::path runtimeConfig = ResolveRuntimeConfigForCoreClr();
    if (!std::filesystem::exists(runtimeConfig)) { GTEST_SKIP() << "HotReload runtimeconfig not found"; }
    ASSERT_TRUE(clr.Initialize(runtimeConfig));

    std::filesystem::path ws = std::filesystem::temp_directory_path() / "GE_EndToEnd_Workspace_CallArgs";
    std::filesystem::create_directories(ws);
    auto sourcePath = ws / "FooArgs.cs";
    {
        std::ofstream f(sourcePath);
        f << "namespace GameEngine.Scripts { public static class Foo { public static int Add(int a,int b){ return a+b; } } }";
        f.close();
    }

    std::string pipeName = CompileServerTestHost::UniquePipeName("CallArgs");
    ASSERT_TRUE(CompileServerTestHost::StartTestCompileServer(pipeName));

    std::ostringstream json;
    json << "{";
    json << "\"ProjectRoot\":\"" << ws.generic_string() << "\",";
    json << "\"ChangedFiles\":[],\"AffectedFiles\":[],\"AllFiles\":[\"" << sourcePath.generic_string() << "\"],";
    json << "\"PreferredStrategy\":\"Full\",\"ForceFull\":true}";

    CompileServerClient client(std::unique_ptr<IHotReloadTransport>{}, pipeName);
    CompileServerResponse resp; ASSERT_TRUE(client.Compile(json.str(), resp)); ASSERT_TRUE(resp.Success);

    auto outDir = ws / "bin" / "GE_Temp"; std::filesystem::create_directories(outDir);
    auto dllPath = outDir / "GameEngine.Scripts.dll";
    std::ofstream df(dllPath, std::ios::binary); df.write(reinterpret_cast<const char*>(resp.AssemblyBytes.data()), (std::streamsize)resp.AssemblyBytes.size()); df.close();

    ASSERT_TRUE(clr.PreloadAndSwapFromPath(dllPath));
    // Dump assembly info to help debug method discovery and assert positive type count
    // INFO command removed; directly invoke methods
    int32 rc = clr.CallUserScriptsMethod("GameEngine.Scripts.Foo.Add?i32=3&i32=4");
    EXPECT_EQ(rc, 7);
#else
    GTEST_SKIP() << "Argument passing test is Windows-only in this environment";
#endif
}

// ---------------------------------------------------------------------------
// Source-generator diagnostics reach the response AND decide its success flag.
//
// A generator reports its failures through the driver's out-parameter, not
// through emitResult: Roslyn still emits a valid assembly for a compilation
// whose generator errored, just missing what that generator would have added.
// So a server that gates only on emitResult.Success answers "Success" with a
// registration-less assembly, and the user's GameSystem/IEntitySystem silently
// stops being scheduled.
//
// Both legs run against a REAL host with EngineBinDir pointed at this exe's
// directory, where the build stages SourceGenerators/EntitySystemGenerator.dll
// and GameEngine.Scripting.ABI.dll.
// ---------------------------------------------------------------------------
#ifdef _WIN32
namespace {

bool HasDiagnostic(const std::vector<CompileServerDiagnostic>& diags, const char* code)
{
    return std::any_of(diags.begin(), diags.end(),
                       [code](const CompileServerDiagnostic& d) { return d.Code == code; });
}

std::string DumpDiagnostics(const CompileServerResponse& resp)
{
    std::ostringstream oss;
    oss << "Errors:";
    for (const auto& e : resp.Errors) oss << "\n  " << e.Code << ": " << e.MessageUtf8;
    oss << "\nWarnings:";
    for (const auto& w : resp.Warnings) oss << "\n  " << w.Code << ": " << w.MessageUtf8;
    return oss.str();
}

CompileServerRequestDesc MakeGeneratorRequest(const std::filesystem::path& workspace,
                                              const std::string& assemblyName)
{
    const std::filesystem::path exeDir = PathUtils::GetExecutableDirectory();
    CompileServerRequestDesc desc;
    desc.ProjectRoot = workspace;
    desc.AssemblyName = assemblyName;
    desc.EngineBinDir = exeDir;
    desc.SourceRoots = {workspace};
    desc.References = {exeDir / "GameEngine.Scripting.ABI.dll"};
    return desc;
}

} // namespace

// A generator ERROR must fail the compile and suppress the assembly.
// GE0018: a GameSystem the generator cannot construct — neither the generated
// registration nor the runtime's reflection discovery can instantiate it.
TEST(CompileServerEndToEnd, GeneratorErrorFailsCompile) {
    const std::filesystem::path exeDir = PathUtils::GetExecutableDirectory();
    ASSERT_TRUE(std::filesystem::exists(exeDir / "SourceGenerators" / "EntitySystemGenerator.dll"))
        << "generator not staged next to the test exe — the assertions below would pass vacuously";
    ASSERT_TRUE(std::filesystem::exists(exeDir / "GameEngine.Scripting.ABI.dll"));

    const std::string pipeName = CompileServerTestHost::UniquePipeName("GenErr");
    ASSERT_TRUE(CompileServerTestHost::StartTestCompileServer(pipeName));

    const std::filesystem::path ws =
        std::filesystem::temp_directory_path() / "GE_EndToEnd_GenErr";
    std::error_code ec;
    std::filesystem::remove_all(ws, ec);
    std::filesystem::create_directories(ws, ec);
    {
        std::ofstream f(ws / "NoCtorSystem.cs", std::ios::trunc);
        f << "using GameEngine.Scripting;\n"
             "public partial class NoCtorSystem : GameSystem\n"
             "{\n"
             "    private NoCtorSystem(int unused) { }\n"
             "    public override void OnCreate() { }\n"
             "}\n";
    }

    CompileServerClient client(std::unique_ptr<IHotReloadTransport>{}, pipeName);
    CompileServerResponse resp;
    const bool ok = client.Compile(
        BuildCompileServerRequestJson(MakeGeneratorRequest(ws, "GenErrScripts")), resp);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(resp.Success) << DumpDiagnostics(resp);
    EXPECT_TRUE(HasDiagnostic(resp.Errors, "GE0018")) << DumpDiagnostics(resp);
    EXPECT_TRUE(resp.AssemblyBytes.empty())
        << "a build that failed on a generator error must not hand back an assembly";

    CompileServerTestHost::ReapHostsOnPipe(pipeName);
}

// A generator WARNING must not fail the compile. Doubles as the positive control
// for the test above: it proves the generator really loaded and ran, so a
// GE0018-shaped failure there is the diagnostic and not an ambient breakage.
// GE0017: a non-partial GameSystem gets no generated registration, but the
// runtime's reflection discovery still finds and runs it.
TEST(CompileServerEndToEnd, GeneratorWarningKeepsCompileSuccessful) {
    const std::filesystem::path exeDir = PathUtils::GetExecutableDirectory();
    ASSERT_TRUE(std::filesystem::exists(exeDir / "SourceGenerators" / "EntitySystemGenerator.dll"))
        << "generator not staged next to the test exe — the assertions below would pass vacuously";

    const std::string pipeName = CompileServerTestHost::UniquePipeName("GenWarn");
    ASSERT_TRUE(CompileServerTestHost::StartTestCompileServer(pipeName));

    const std::filesystem::path ws =
        std::filesystem::temp_directory_path() / "GE_EndToEnd_GenWarn";
    std::error_code ec;
    std::filesystem::remove_all(ws, ec);
    std::filesystem::create_directories(ws, ec);
    {
        std::ofstream f(ws / "NotPartialSystem.cs", std::ios::trunc);
        f << "using GameEngine.Scripting;\n"
             "public class NotPartialSystem : GameSystem\n"
             "{\n"
             "    public override void OnCreate() { }\n"
             "}\n";
    }

    CompileServerClient client(std::unique_ptr<IHotReloadTransport>{}, pipeName);
    CompileServerResponse resp;
    const bool ok = client.Compile(
        BuildCompileServerRequestJson(MakeGeneratorRequest(ws, "GenWarnScripts")), resp);

    EXPECT_TRUE(ok) << DumpDiagnostics(resp);
    EXPECT_TRUE(resp.Success) << DumpDiagnostics(resp);
    EXPECT_TRUE(HasDiagnostic(resp.Warnings, "GE0017")) << DumpDiagnostics(resp);
    EXPECT_FALSE(HasDiagnostic(resp.Errors, "GE0017")) << DumpDiagnostics(resp);
    EXPECT_FALSE(resp.AssemblyBytes.empty());

    CompileServerTestHost::ReapHostsOnPipe(pipeName);
}

// A request that discovers no generators must not answer for the requests after it.
//
// The host is resident — that is its point — and its pipe is keyed on the workspace
// alone, so one process serves every EngineBinDir that opens the project. A request
// carrying no EngineBinDir discovers nothing (the host's own directory stages no
// SourceGenerators), and a process-global generator cache filled by that discovery
// pins "no generators" for the host's lifetime. The generator-error gate then has
// nothing to gate: no driver runs, so no generator diagnostic can exist, and the
// GE0018 leg below reports Success with a registration-less assembly — pre-gate
// behavior, silently restored by request ordering.
TEST(CompileServerEndToEnd, GeneratorErrorStillFailsAfterAGeneratorLessRequest) {
    const std::filesystem::path exeDir = PathUtils::GetExecutableDirectory();
    ASSERT_TRUE(std::filesystem::exists(exeDir / "SourceGenerators" / "EntitySystemGenerator.dll"))
        << "generator not staged next to the test exe — the GE0018 assertion would pass vacuously";
    ASSERT_TRUE(std::filesystem::exists(exeDir / "GameEngine.Scripting.ABI.dll"));

    const std::string pipeName = CompileServerTestHost::UniquePipeName("GenAfterBare");
    ASSERT_TRUE(CompileServerTestHost::StartTestCompileServer(pipeName));

    // Both legs have to reach the SAME process, or the shared cache under test was
    // never shared and nothing below proves anything.
    //
    // StartTestCompileServer spawns the host without waiting for it to listen, and the
    // host tears its pipe instance down and recreates it between clients — so a single
    // probe can miss a perfectly healthy host either way. Retry, as the reaper does.
    const auto residentHostPid = [&pipeName]() -> uint32_t {
        for (int attempt = 0; attempt < 20; ++attempt)
        {
            const uint32_t pid = CompileServerTestHost::QueryHostPid(pipeName, 500);
            if (pid != 0)
                return pid;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        return 0;
    };

    const uint32_t hostPid = residentHostPid();
    ASSERT_NE(hostPid, 0u) << "no resident host answered on " << pipeName;

    std::error_code ec;

    // Leg 1: no EngineBinDir. It compiles cleanly; what matters is what it caches.
    {
        const std::filesystem::path ws =
            std::filesystem::temp_directory_path() / "GE_EndToEnd_GenAfterBare_NoBinDir";
        std::filesystem::remove_all(ws, ec);
        std::filesystem::create_directories(ws, ec);
        {
            std::ofstream f(ws / "Plain.cs", std::ios::trunc);
            f << "public static class Plain { public static int Value() { return 1; } }\n";
        }

        CompileServerRequestDesc desc;
        desc.ProjectRoot = ws;
        desc.AssemblyName = "GenAfterBareNoBinDirScripts";
        desc.SourceRoots = {ws};
        // EngineBinDir deliberately left empty — this is the request being reproduced.

        // An INJECTED transport is what keeps this test honest: a client constructed
        // with a null one owns the spawn path and starts a second host whenever its
        // connect lands in the gap while the resident host recreates its pipe
        // instance. Two hosts on the pipe mean two generator caches, and the legs
        // below would stop sharing the state under test.
        CompileServerClient client(std::make_unique<PipeTransport>(pipeName), pipeName);
        CompileServerResponse resp;
        ASSERT_TRUE(client.Compile(BuildCompileServerRequestJson(desc), resp)) << DumpDiagnostics(resp);
    }

    ASSERT_EQ(residentHostPid(), hostPid) << "a second host took over the pipe between legs";

    // Leg 2: same host, EngineBinDir supplied, generator error present.
    {
        const std::filesystem::path ws =
            std::filesystem::temp_directory_path() / "GE_EndToEnd_GenAfterBare_GenErr";
        std::filesystem::remove_all(ws, ec);
        std::filesystem::create_directories(ws, ec);
        {
            std::ofstream f(ws / "NoCtorSystem.cs", std::ios::trunc);
            f << "using GameEngine.Scripting;\n"
                 "public partial class NoCtorSystem : GameSystem\n"
                 "{\n"
                 "    private NoCtorSystem(int unused) { }\n"
                 "    public override void OnCreate() { }\n"
                 "}\n";
        }

        CompileServerClient client(std::make_unique<PipeTransport>(pipeName), pipeName);
        CompileServerResponse resp;
        const bool ok = client.Compile(
            BuildCompileServerRequestJson(MakeGeneratorRequest(ws, "GenAfterBareScripts")), resp);

        EXPECT_FALSE(ok);
        EXPECT_FALSE(resp.Success) << DumpDiagnostics(resp);
        EXPECT_TRUE(HasDiagnostic(resp.Errors, "GE0018")) << DumpDiagnostics(resp);
        EXPECT_TRUE(resp.AssemblyBytes.empty())
            << "a build that failed on a generator error must not hand back an assembly";
    }

    EXPECT_EQ(residentHostPid(), hostPid) << "the two legs were served by different processes";

    CompileServerTestHost::ReapHostsOnPipe(pipeName);
}
#endif
