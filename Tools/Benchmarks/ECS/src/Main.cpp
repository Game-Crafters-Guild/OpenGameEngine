// ECSBenchCompare — cross-framework ECS micro-benchmark driver.
// Usage: ECSBenchCompare [--json <path>] [--sizes 1000,16384,...] [--skip-gaia]
//        [--contaminated]

#include "BenchCommon.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace bench {

double MedianOf(std::vector<double> samples)
{
    if (samples.empty())
        return 0.0;
    std::sort(samples.begin(), samples.end());
    const std::size_t mid = samples.size() / 2;
    if (samples.size() % 2 == 1)
        return samples[mid];
    return 0.5 * (samples[mid - 1] + samples[mid]);
}

namespace {
volatile double g_Sink = 0.0;
}

void KeepAlive(double value)
{
    g_Sink = g_Sink + value;
}

} // namespace bench

namespace {

using bench::CaseResult;

const char* kParityCases[] = {
    "CreateEntities",     "DestroyEntities",       "AddRemoveComponent",
    "RandomAccessGet",    "IterateOneComponent",   "IterateTwoComponents",
    "IterateThreeComponents", "ComplexSystemsUpdate",
};

// Cases reported as ms totals; everything else reports ns/entity.
bool ReportsMsTotal(const std::string& c)
{
    return c == "CreateEntities" || c == "DestroyEntities" || c == "AddRemoveComponent" ||
           c == "CreateEntities_Batch";
}

std::string JsonEscape(const std::string& s)
{
    std::string out;
    for (char ch : s)
    {
        if (ch == '"' || ch == '\\')
            out.push_back('\\');
        out.push_back(ch);
    }
    return out;
}

void WriteJson(const std::string& path, const std::vector<CaseResult>& results,
               const std::vector<std::string>& frameworks, bool contaminated)
{
    std::ofstream f(path, std::ios::trunc);
    if (!f)
    {
        std::fprintf(stderr, "ERROR: cannot open %s for writing\n", path.c_str());
        return;
    }

    char timeBuf[64] = {};
    const std::time_t now = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%dT%H:%M:%S", &tmv);

    const char* cpu = std::getenv("PROCESSOR_IDENTIFIER");

    f << "{\n  \"meta\": {\n";
    f << "    \"date\": \"" << timeBuf << "\",\n";
    f << "    \"config\": \"Release (/O2, /arch:AVX2 for all frameworks)\",\n";
    f << "    \"cpu\": \"" << JsonEscape(cpu ? cpu : "unknown") << "\",\n";
    f << "    \"clock\": \"std::chrono::steady_clock\",\n";
    f << "    \"contaminated\": " << (contaminated ? "true" : "false") << ",\n";
    f << "    \"versions\": { \"ours\": \"GameEngine ECS (this tree)\", \"entt\": \"v3.16.0\", "
         "\"flecs\": \"v4.1.6\", \"gaia\": \"v0.9.2\" },\n";
    f << "    \"frameworks\": [";
    for (std::size_t i = 0; i < frameworks.size(); ++i)
        f << (i ? ", " : "") << '"' << frameworks[i] << '"';
    f << "],\n";
    f << "    \"methodology\": \"median of timed runs after warmups; structural cases 2 warmups + "
         "5 runs, iteration cases 3 warmups + 9 runs; single-threaded documented idiom per "
         "framework; identical component layouts; rows with framework ours-extra are OUR "
         "differentiating features, not parity comparisons\"\n";
    f << "  },\n  \"results\": [\n";
    for (std::size_t i = 0; i < results.size(); ++i)
    {
        const auto& r = results[i];
        f << "    { \"framework\": \"" << r.Framework << "\", \"case\": \"" << r.Case
          << "\", \"n\": " << r.N << ", \"total_ms_median\": " << r.TotalMsMedian
          << ", \"ns_per_entity\": " << r.NsPerEntity << ", \"samples_ms\": [";
        for (std::size_t s = 0; s < r.SamplesMs.size(); ++s)
            f << (s ? ", " : "") << r.SamplesMs[s];
        f << "]";
        if (!r.Note.empty())
            f << ", \"note\": \"" << JsonEscape(r.Note) << "\"";
        f << " }" << (i + 1 < results.size() ? "," : "") << "\n";
    }
    f << "  ]\n}\n";
    std::printf("\nResults written to %s\n", path.c_str());
}

void PrintTables(const std::vector<CaseResult>& results, const std::vector<std::string>& frameworks,
                 const std::vector<std::size_t>& sizes)
{
    // (case, n) -> framework -> result
    std::map<std::string, std::map<std::size_t, std::map<std::string, const CaseResult*>>> grid;
    std::vector<std::string> extraCases;
    for (const auto& r : results)
    {
        grid[r.Case][r.N][r.Framework] = &r;
        if (r.Framework == "ours-extra" &&
            std::find(extraCases.begin(), extraCases.end(), r.Case) == extraCases.end())
            extraCases.push_back(r.Case);
    }

    std::printf("\n=== PARITY COMPARISON (same case shape, documented idiom per framework) ===\n");
    for (const char* caseName : kParityCases)
    {
        auto it = grid.find(caseName);
        if (it == grid.end())
            continue;
        const bool msTotal = ReportsMsTotal(caseName);
        std::printf("\n%-24s %s\n", caseName, msTotal ? "(ms total)" : "(ns/entity)");
        std::printf("%12s", "N");
        for (const auto& fw : frameworks)
            std::printf(" %12s", fw.c_str());
        std::printf("\n");
        for (const std::size_t n : sizes)
        {
            auto nIt = it->second.find(n);
            if (nIt == it->second.end())
                continue;
            std::printf("%12zu", n);
            for (const auto& fw : frameworks)
            {
                auto fIt = nIt->second.find(fw);
                if (fIt == nIt->second.end())
                    std::printf(" %12s", "-");
                else
                    std::printf(" %12.3f", msTotal ? fIt->second->TotalMsMedian
                                                   : fIt->second->NsPerEntity);
            }
            std::printf("\n");
        }
    }

    std::printf("\n=== OUR EXTRA ROWS (differentiating features, NOT parity comparisons) ===\n");
    for (const auto& caseName : extraCases)
    {
        const bool msTotal = ReportsMsTotal(caseName);
        std::printf("\n%-40s %s\n", caseName.c_str(), msTotal ? "(ms total)" : "(ns/entity)");
        std::printf("%12s %12s\n", "N", "ours-extra");
        for (const std::size_t n : sizes)
        {
            const auto* r = grid[caseName].count(n) ? grid[caseName][n]["ours-extra"] : nullptr;
            if (!r)
                continue;
            std::printf("%12zu %12.3f", n, msTotal ? r->TotalMsMedian : r->NsPerEntity);
            if (!r->Note.empty())
                std::printf("   %s", r->Note.c_str());
            std::printf("\n");
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    bench::BenchConfig cfg;
    std::string jsonPath = "results/ecs_bench_results.json";
    bool skipGaia = false;
    bool contaminated = false;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--json") == 0 && i + 1 < argc)
        {
            jsonPath = argv[++i];
        }
        else if (std::strcmp(argv[i], "--sizes") == 0 && i + 1 < argc)
        {
            cfg.Sizes.clear();
            const char* p = argv[++i];
            while (*p)
            {
                cfg.Sizes.push_back(static_cast<std::size_t>(std::strtoull(p, nullptr, 10)));
                const char* comma = std::strchr(p, ',');
                if (!comma)
                    break;
                p = comma + 1;
            }
        }
        else if (std::strcmp(argv[i], "--skip-gaia") == 0)
        {
            skipGaia = true;
        }
        else if (std::strcmp(argv[i], "--contaminated") == 0)
        {
            contaminated = true;
        }
        else
        {
            std::fprintf(stderr,
                         "Usage: %s [--json <path>] [--sizes n1,n2,...] [--skip-gaia] "
                         "[--contaminated]\n",
                         argv[0]);
            return 1;
        }
    }

    std::vector<CaseResult> all;
    std::vector<std::string> frameworks;

    std::printf("Running GameEngine ECS lane...\n");
    auto ours = bench::RunOursBenches(cfg);
    all.insert(all.end(), ours.begin(), ours.end());
    frameworks.push_back("ours");

    std::printf("Running EnTT lane...\n");
    auto entt = bench::RunEnttBenches(cfg);
    all.insert(all.end(), entt.begin(), entt.end());
    frameworks.push_back("entt");

    std::printf("Running flecs lane...\n");
    auto flecs = bench::RunFlecsBenches(cfg);
    all.insert(all.end(), flecs.begin(), flecs.end());
    frameworks.push_back("flecs");

    if (!skipGaia)
    {
        std::printf("Running gaia-ecs lane...\n");
        auto gaia = bench::RunGaiaBenches(cfg);
        all.insert(all.end(), gaia.begin(), gaia.end());
        frameworks.push_back("gaia");
    }

    PrintTables(all, frameworks, cfg.Sizes);
    WriteJson(jsonPath, all, frameworks, contaminated);
    return 0;
}
