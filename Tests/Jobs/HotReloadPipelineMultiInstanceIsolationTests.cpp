#include <gtest/gtest.h>
#include "Jobs/HotReloadTasks.h"
#include "Jobs/HotReloadTestHooks.h"
#include "Jobs/IHotReloadTransport.h"
#include "Jobs/WorkspaceId.h"
#include "JobSystem/JobChannel.h"
#include "JobSystem/WorkStealingThreadPool.h"
#include "Jobs/CompileServerVersion.h"
#include "ScopedHotReloadTestHooks.h"
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>
#include <utility>
#include <nlohmann/json.hpp>

using namespace GameEngine;

namespace {
class CapturingTransport : public IHotReloadTransport {
public:
    CapturingTransport(std::string pipe, std::vector<std::pair<std::string,std::string>>* outVec, std::mutex* mtx)
        : m_Pipe(std::move(pipe)), m_Out(outVec), m_Mtx(mtx) {}
    bool Connect() override { return true; }
    bool SendRequest(const std::string& req, std::string& outResponse) override {
        // Handle protocol-level control messages without capturing
        if (req == "__version__") {
            outResponse = std::string("{\"Version\":\"") + kExpectedCompileServerVersion + "\"}";
            return true;
        }
        if (req == "__shutdown__") {
            outResponse = std::string("{}");
            return true;
        }

        {
            std::lock_guard<std::mutex> lock(*m_Mtx);
            // Only capture actual compile JSON requests
            if (!req.empty() && req[0] == '{') {
                m_Out->emplace_back(m_Pipe, req);
            }
        }
        // Return a fast negative so pipeline returns quickly/deterministically
        outResponse = std::string("{\"Success\":false,\"Warnings\":[],\"Errors\":[]}");
        return true;
    }
    void Close() override {}
private:
    std::string m_Pipe;
    std::vector<std::pair<std::string,std::string>>* m_Out;
    std::mutex* m_Mtx;
};
}

TEST(HotReloadPipeline_MultiInstanceIsolation, ConcurrentPipelinesUseDistinctPipesAndRoots)
{
#ifndef _WIN32
    GTEST_SKIP() << "Windows-only in this environment";
#else
    using nlohmann::json;
    // Arrange two isolated workspaces
    std::filesystem::path wsA = std::filesystem::temp_directory_path() / "GE_WS_Isolation_A";
    std::filesystem::path wsB = std::filesystem::temp_directory_path() / "GE_WS_Isolation_B";
    std::error_code ec;
    std::filesystem::create_directories(wsA, ec);
    std::filesystem::create_directories(wsB, ec);
    auto aCs = wsA / "A.cs"; { std::ofstream f(aCs); f << "public static class A{public static int V()=>1;}"; }
    auto bCs = wsB / "B.cs"; { std::ofstream f(bCs); f << "public static class B{public static int V()=>2;}"; }

    auto asmA = wsA / "bin" / "GameEngine.Scripts.dll"; std::filesystem::create_directories(asmA.parent_path(), ec); { std::ofstream f(asmA); f << ""; }
    auto asmB = wsB / "bin" / "GameEngine.Scripts.dll"; std::filesystem::create_directories(asmB.parent_path(), ec); { std::ofstream f(asmB); f << ""; }

    // Force server selection
    _putenv_s("GE_DISABLE_COMPILE_SERVER", "0");

    // Capture pipeName and JSON per request
    static std::vector<std::pair<std::string,std::string>> captured; captured.clear();
    static std::mutex capMtx;
    GameEngine::Tests::ScopedHotReloadTestHooks hookGuard;
    SetCompileServerTransportFactoryForTests([&](const std::string& pipe){
        return std::unique_ptr<IHotReloadTransport>(new CapturingTransport(pipe, &captured, &capMtx));
    });

    auto pool = std::make_shared<JobSystem::WorkStealingThreadPool>(4);
    JobSystem::JobChannel pipeACompiles(*pool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
    HotReloadPipeline pipeA(*pool, pipeACompiles);
    JobSystem::JobChannel pipeBCompiles(*pool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
    HotReloadPipeline pipeB(*pool, pipeBCompiles);

    // Execute concurrently
    auto futA = pipeA.ExecuteAsync(asmA.string());
    auto futB = pipeB.ExecuteAsync(asmB.string());

    ASSERT_EQ(futA.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    ASSERT_EQ(futB.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    (void)futA.get();
    (void)futB.get();

    // Verify two requests captured with distinct pipes
    {
        std::lock_guard<std::mutex> lock(capMtx);
        ASSERT_EQ(captured.size(), 2u) << "Expected two compile requests captured";
        auto p0 = captured[0].first;
        auto p1 = captured[1].first;
        EXPECT_NE(p0, p1) << "Pipes should be distinct per workspace";

        // Validate pipe names match computed WorkspaceId
        auto expectA = std::string("GE_CompileServer_") + WorkspaceId::Compute(wsA.string());
        auto expectB = std::string("GE_CompileServer_") + WorkspaceId::Compute(wsB.string());
        EXPECT_TRUE(p0 == expectA || p1 == expectA);
        EXPECT_TRUE(p0 == expectB || p1 == expectB);

        // Parse JSONs and validate ProjectRoot and AllFiles are rooted correctly
        json j0 = json::parse(captured[0].second);
        json j1 = json::parse(captured[1].second);
        ASSERT_TRUE(j0.contains("ProjectRoot"));
        ASSERT_TRUE(j1.contains("ProjectRoot"));
        std::string r0 = j0["ProjectRoot"].get<std::string>();
        std::string r1 = j1["ProjectRoot"].get<std::string>();
        // Each JSON must correspond to exactly one of the roots
        auto norm = [](const std::filesystem::path& p){ return p.generic_string(); };
        auto ra = norm(wsA);
        auto rb = norm(wsB);
        EXPECT_TRUE((r0 == ra && r1 == rb) || (r0 == rb && r1 == ra));

        // Ensure no cross-talk: AllFiles arrays should not include the other's .cs path
        auto hasPath = [](const json& arr, const std::string& p){ if(!arr.is_array()) return false; for (auto& v:arr){ if (v.is_string() && v.get<std::string>()==p) return true; } return false; };
        ASSERT_TRUE(j0.contains("AllFiles"));
        ASSERT_TRUE(j1.contains("AllFiles"));
        auto aCsStr = aCs.generic_string();
        auto bCsStr = bCs.generic_string();
        if (r0 == ra) {
            EXPECT_TRUE(hasPath(j0["AllFiles"], aCsStr));
            EXPECT_FALSE(hasPath(j0["AllFiles"], bCsStr));
            EXPECT_TRUE(hasPath(j1["AllFiles"], bCsStr));
            EXPECT_FALSE(hasPath(j1["AllFiles"], aCsStr));
        } else {
            EXPECT_TRUE(hasPath(j0["AllFiles"], bCsStr));
            EXPECT_FALSE(hasPath(j0["AllFiles"], aCsStr));
            EXPECT_TRUE(hasPath(j1["AllFiles"], aCsStr));
            EXPECT_FALSE(hasPath(j1["AllFiles"], bCsStr));
        }
    }
#endif
}



TEST(HotReloadPipeline_MultiInstanceIsolation, AffectsInferenceIsolatedAcrossWorkspaces)
{
#ifndef _WIN32
    GTEST_SKIP() << "Windows-only in this environment";
#else
    using nlohmann::json;
    std::error_code ec;
    // Ensure CompileServer is enabled (default unless explicitly disabled)
    _putenv_s("GE_DISABLE_COMPILE_SERVER", "0");
    _putenv_s("GE_AFFECTS_INFERENCE", "1");

    // Workspace A
    std::filesystem::path wsA = std::filesystem::temp_directory_path() / "GE_WS_Infer_A";
    std::filesystem::create_directories(wsA, ec);
    auto aCs = wsA / "Foo.cs"; { std::ofstream f(aCs); f << "public static class Foo{public static int V()=>1;}"; }
    auto aGen = wsA / "Foo.g.cs"; { std::ofstream f(aGen); f << "// gen"; }
    auto aProj = wsA / "Foo.csproj"; { std::ofstream f(aProj); f << "<Project Sdk=\"Microsoft.NET.Sdk\"/>"; }
    auto asmA = wsA / "bin" / "GameEngine.Scripts.dll"; std::filesystem::create_directories(asmA.parent_path(), ec); { std::ofstream f(asmA); f << ""; }

    // Workspace B
    std::filesystem::path wsB = std::filesystem::temp_directory_path() / "GE_WS_Infer_B";
    std::filesystem::create_directories(wsB, ec);
    auto bCs = wsB / "Bar.cs"; { std::ofstream f(bCs); f << "public static class Bar{public static int V()=>2;}"; }
    auto bGen = wsB / "Bar.generated.cs"; { std::ofstream f(bGen); f << "// gen2"; }
    auto bProj = wsB / "Bar.csproj"; { std::ofstream f(bProj); f << "<Project Sdk=\"Microsoft.NET.Sdk\"/>"; }
    auto asmB = wsB / "bin" / "GameEngine.Scripts.dll"; std::filesystem::create_directories(asmB.parent_path(), ec); { std::ofstream f(asmB); f << ""; }

    static std::vector<std::pair<std::string,std::string>> captured; captured.clear();
    static std::mutex capMtx;
    GameEngine::Tests::ScopedHotReloadTestHooks hookGuard;
    SetCompileServerTransportFactoryForTests([&](const std::string& pipe){
        return std::unique_ptr<IHotReloadTransport>(new CapturingTransport(pipe, &captured, &capMtx));
    });

    auto pool = std::make_shared<JobSystem::WorkStealingThreadPool>(4);

    // Run A with changed .cs
    captured.clear();
    SetChangedFilesForNextCompile({ aCs.generic_string() });
    {
        JobSystem::JobChannel pACompiles(*pool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
        HotReloadPipeline pA(*pool, pACompiles);
        auto fA = pA.ExecuteAsync(asmA.string());
        ASSERT_EQ(fA.wait_for(std::chrono::seconds(10)), std::future_status::ready);
        (void)fA.get();
    }
    {
        std::lock_guard<std::mutex> lock(capMtx);
        ASSERT_EQ(captured.size(), 1u);
        json j = json::parse(captured[0].second);
        ASSERT_TRUE(j.contains("AffectedFiles"));
        auto has = [](const json& arr, const std::string& p){ if(!arr.is_array()) return false; for (auto& v:arr){ if(v.is_string() && v.get<std::string>()==p) return true; } return false; };
        EXPECT_TRUE(has(j["AffectedFiles"], aCs.generic_string()));
        EXPECT_TRUE(has(j["AffectedFiles"], aGen.generic_string()));
        EXPECT_TRUE(has(j["AffectedFiles"], aProj.generic_string()));
        EXPECT_TRUE(j.contains("ForceFull") && !j["ForceFull"].get<bool>());
        EXPECT_EQ(j["PreferredStrategy"].get<std::string>(), "Incremental");
    }

    // Run B with changed .cs
    captured.clear();
    SetChangedFilesForNextCompile({ bCs.generic_string() });
    {
        JobSystem::JobChannel pBCompiles(*pool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
        HotReloadPipeline pB(*pool, pBCompiles);
        auto fB = pB.ExecuteAsync(asmB.string());
        ASSERT_EQ(fB.wait_for(std::chrono::seconds(10)), std::future_status::ready);
        (void)fB.get();
    }
    {
        std::lock_guard<std::mutex> lock(capMtx);
        ASSERT_EQ(captured.size(), 1u);
        json j = json::parse(captured[0].second);
        ASSERT_TRUE(j.contains("AffectedFiles"));
        auto has = [](const json& arr, const std::string& p){ if(!arr.is_array()) return false; for (auto& v:arr){ if(v.is_string() && v.get<std::string>()==p) return true; } return false; };
        EXPECT_TRUE(has(j["AffectedFiles"], bCs.generic_string()));
        EXPECT_TRUE(has(j["AffectedFiles"], bGen.generic_string()));
        EXPECT_TRUE(has(j["AffectedFiles"], bProj.generic_string()));
        EXPECT_TRUE(j.contains("ForceFull") && !j["ForceFull"].get<bool>());
        EXPECT_EQ(j["PreferredStrategy"].get<std::string>(), "Incremental");
    }

    // Cleanup
    _putenv_s("GE_AFFECTS_INFERENCE", "0");
#endif
}

TEST(HotReloadPipeline_MultiInstanceIsolation, MixedStrategiesIsolatedAcrossWorkspaces)
{
#ifndef _WIN32
    GTEST_SKIP() << "Windows-only in this environment";
#else
    using nlohmann::json;
    std::error_code ec;
    // CompileServer is default; ensure it remains enabled and inference is on for this test
    _putenv_s("GE_DISABLE_COMPILE_SERVER", "0");
    _putenv_s("GE_AFFECTS_INFERENCE", "1");

    // Workspace C (Full via .csproj)
    std::filesystem::path wsC = std::filesystem::temp_directory_path() / "GE_WS_Mixed_C";
    std::filesystem::create_directories(wsC, ec);
    auto cCs = wsC / "C.cs"; { std::ofstream f(cCs); f << "public static class C{public static int V()=>3;}"; }
    auto cProj = wsC / "C.csproj"; { std::ofstream f(cProj); f << "<Project Sdk=\"Microsoft.NET.Sdk\"/>"; }
    auto asmC = wsC / "bin" / "GameEngine.Scripts.dll"; std::filesystem::create_directories(asmC.parent_path(), ec); { std::ofstream f(asmC); f << ""; }

    // Workspace D (Incremental via .cs)
    std::filesystem::path wsD = std::filesystem::temp_directory_path() / "GE_WS_Mixed_D";
    std::filesystem::create_directories(wsD, ec);
    auto dCs = wsD / "D.cs"; { std::ofstream f(dCs); f << "public static class D{public static int V()=>4;}"; }
    auto dProj = wsD / "D.csproj"; { std::ofstream f(dProj); f << "<Project Sdk=\"Microsoft.NET.Sdk\"/>"; }
    auto asmD = wsD / "bin" / "GameEngine.Scripts.dll"; std::filesystem::create_directories(asmD.parent_path(), ec); { std::ofstream f(asmD); f << ""; }

    static std::vector<std::pair<std::string,std::string>> captured; captured.clear();
    static std::mutex capMtx;
    GameEngine::Tests::ScopedHotReloadTestHooks hookGuard;
    SetCompileServerTransportFactoryForTests([&](const std::string& pipe){
        return std::unique_ptr<IHotReloadTransport>(new CapturingTransport(pipe, &captured, &capMtx));
    });

    auto pool = std::make_shared<JobSystem::WorkStealingThreadPool>(4);

    // Run C with changed .csproj (expect Full)
    captured.clear();
    SetChangedFilesForNextCompile({ cProj.generic_string() });
    {
        JobSystem::JobChannel pCCompiles(*pool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
        HotReloadPipeline pC(*pool, pCCompiles);
        auto fC = pC.ExecuteAsync(asmC.string());
        ASSERT_EQ(fC.wait_for(std::chrono::seconds(10)), std::future_status::ready);
        (void)fC.get();
    }
    std::string jsonC;
    {
        std::lock_guard<std::mutex> lock(capMtx);
        ASSERT_EQ(captured.size(), 1u);
        jsonC = captured[0].second;
    }

    // Run D with changed .cs (expect Incremental)
    captured.clear();
    SetChangedFilesForNextCompile({ dCs.generic_string() });
    {
        JobSystem::JobChannel pDCompiles(*pool, JobSystem::JobChannelDesc{.Name = "Script compiles", .MaxRunning = 1});
        HotReloadPipeline pD(*pool, pDCompiles);
        auto fD = pD.ExecuteAsync(asmD.string());
        ASSERT_EQ(fD.wait_for(std::chrono::seconds(10)), std::future_status::ready);
        (void)fD.get();
    }
    std::string jsonD;
    {
        std::lock_guard<std::mutex> lock(capMtx);
        ASSERT_EQ(captured.size(), 1u);
        jsonD = captured[0].second;
    }

    // Validate strategies independent
    {
        json jC = json::parse(jsonC);
        ASSERT_TRUE(jC.contains("ForceFull"));
        EXPECT_TRUE(jC["ForceFull"].get<bool>()) << "csproj change should force full";
        EXPECT_EQ(jC["PreferredStrategy"].get<std::string>(), "Full");
        // ChangedFiles should contain the csproj
        bool found=false; for (auto& v : jC["ChangedFiles"]) if (v.is_string() && v.get<std::string>()==cProj.generic_string()) { found=true; break; }
        EXPECT_TRUE(found);
    }
    {
        json jD = json::parse(jsonD);
        ASSERT_TRUE(jD.contains("ForceFull"));
        EXPECT_FALSE(jD["ForceFull"].get<bool>()) << ".cs change should be incremental";
        EXPECT_EQ(jD["PreferredStrategy"].get<std::string>(), "Incremental");
        bool found=false; for (auto& v : jD["ChangedFiles"]) if (v.is_string() && v.get<std::string>()==dCs.generic_string()) { found=true; break; }
        EXPECT_TRUE(found);
    }

    _putenv_s("GE_AFFECTS_INFERENCE", "0");
#endif
}
