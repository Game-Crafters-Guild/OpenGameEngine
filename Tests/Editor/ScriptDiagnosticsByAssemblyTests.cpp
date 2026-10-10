#include <gtest/gtest.h>

#include "Panels/ScriptDiagnosticsByAssembly.h"

#include <string>
#include <vector>

using namespace GameEngine;

namespace
{

CompileServerDiagnostic Error(std::string file, int line)
{
    CompileServerDiagnostic diagnostic;
    diagnostic.Severity = "Error";
    diagnostic.Code = "CS1002";
    diagnostic.FileUtf8 = std::move(file);
    diagnostic.Line = line;
    diagnostic.Column = 1;
    diagnostic.MessageUtf8 = "; expected";
    return diagnostic;
}

std::vector<std::string> Files(const std::vector<CompileServerDiagnostic>& rows)
{
    std::vector<std::string> files;
    for (const CompileServerDiagnostic& row : rows)
        files.push_back(row.FileUtf8);
    return files;
}

} // namespace

// One reload compiles a package module, then the project scripts. The package's
// rows must survive the project request's start, and go only when the package's
// own next request starts.
TEST(ScriptDiagnosticsByAssembly, AStartedRequestReplacesOnlyItsOwnAssemblysRows)
{
    ScriptDiagnosticsByAssembly model;
    std::vector<CompileServerDiagnostic> rows;

    model.OnCompileStarted("WatchedPack");
    model.OnDiagnostics("WatchedPack", {Error("Packages/watched-pack/Pack.cs", 3)});
    model.OnCompileStarted("GameEngine.Scripts");
    model.OnDiagnostics("GameEngine.Scripts", {Error("Assets/Scripts/Player.cs", 7)});
    ASSERT_TRUE(model.TakeRowsIfChanged(rows));
    EXPECT_EQ(Files(rows), (std::vector<std::string>{"Assets/Scripts/Player.cs", "Packages/watched-pack/Pack.cs"}));

    model.OnCompileStarted("WatchedPack");
    ASSERT_TRUE(model.TakeRowsIfChanged(rows));
    EXPECT_EQ(Files(rows), (std::vector<std::string>{"Assets/Scripts/Player.cs"}))
        << "The package's restarted request clears its rows and keeps the project's";
    EXPECT_FALSE(model.TakeRowsIfChanged(rows)) << "Nothing changed since the last take";
}

// A removed or disabled package never starts another request, so leaving the
// compile set is what clears its rows.
TEST(ScriptDiagnosticsByAssembly, AnAssemblyLeavingTheCompileSetLosesItsRows)
{
    ScriptDiagnosticsByAssembly model;
    std::vector<CompileServerDiagnostic> rows;

    model.OnDiagnostics("RemovedPack", {Error("Packages/removed-pack/Pack.cs", 3)});
    model.OnDiagnostics("GameEngine.Scripts", {Error("Assets/Scripts/Player.cs", 7)});
    ASSERT_TRUE(model.TakeRowsIfChanged(rows));
    ASSERT_EQ(rows.size(), 2u);

    model.SetCompiledAssemblies({"KeptPack", "GameEngine.Editor", "GameEngine.Scripts"});
    ASSERT_TRUE(model.TakeRowsIfChanged(rows));
    EXPECT_EQ(Files(rows), (std::vector<std::string>{"Assets/Scripts/Player.cs"}));
}

// A reload that was already running when the package was removed still reports
// it afterwards; those late rows must not come back.
TEST(ScriptDiagnosticsByAssembly, ALateReportForADroppedAssemblyShowsNoRows)
{
    ScriptDiagnosticsByAssembly model;
    std::vector<CompileServerDiagnostic> rows;

    model.OnDiagnostics("RemovedPack", {Error("Packages/removed-pack/Pack.cs", 3)});
    model.SetCompiledAssemblies({"GameEngine.Editor", "GameEngine.Scripts"});
    ASSERT_TRUE(model.TakeRowsIfChanged(rows));
    ASSERT_TRUE(rows.empty());

    model.OnCompileStarted("RemovedPack");
    model.OnDiagnostics("RemovedPack", {Error("Packages/removed-pack/Pack.cs", 4)});
    model.OnDiagnostics("GameEngine.Scripts", {Error("Assets/Scripts/Player.cs", 7)});
    ASSERT_TRUE(model.TakeRowsIfChanged(rows));
    EXPECT_EQ(Files(rows), (std::vector<std::string>{"Assets/Scripts/Player.cs"}));
}
