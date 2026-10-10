#pragma once

// Cross-framework ECS benchmark — shared component layouts, timing helpers,
// and result plumbing. Modeled on the case shapes of
// https://github.com/abeimler/ecs_benchmark (not a build of that repo).
//
// Fairness rules (see VERSIONS.md):
//  - identical component layouts across frameworks
//  - single-threaded documented idiom per framework
//  - std::chrono::steady_clock, median of >=5 timed runs after >=2 warmups
//  - deterministic seeds

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace bench {

// ---------------------------------------------------------------------------
// Component layouts — identical for every framework.
// ---------------------------------------------------------------------------
struct Position {
    float x = 0.0f;
    float y = 0.0f;
};

struct Velocity {
    float x = 0.0f;
    float y = 0.0f;
};

struct DataComp {
    int32_t thingy = 0;
    float dingy = 0.0f;
};

struct Health {
    int32_t hp = 100;
};

struct Damage {
    int32_t amount = 10;
};

inline constexpr float kDeltaTime = 1.0f / 60.0f;
inline constexpr uint32_t kShuffleSeed = 0x5EEDu;
// ComplexSystemsUpdate: fraction of entities whose Damage component is
// added/removed each frame (1/16, rotating window — deterministic).
inline constexpr std::size_t kChurnDivisor = 16;
// Ours-extra Changed<> case: fraction of entities dirtied per frame (1%).
inline constexpr std::size_t kDirtyDivisor = 100;

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
using Clock = std::chrono::steady_clock;

inline double MsBetween(Clock::time_point t0, Clock::time_point t1)
{
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

double MedianOf(std::vector<double> samples); // defined in Main.cpp

// Iteration passes at small N complete in a few hundred ns — below/near
// steady_clock's ~100ns tick on Windows. Repeat the pass inside one timed
// sample so every sample is comfortably above clock resolution, then divide.
inline int RepsFor(std::size_t n)
{
    return n >= 65'536 ? 1 : static_cast<int>(65'536 / n);
}

// Anti-DCE sink (defined in Main.cpp).
void KeepAlive(double value);

// ---------------------------------------------------------------------------
// Results
// ---------------------------------------------------------------------------
struct CaseResult {
    std::string Framework;   // "ours" | "entt" | "flecs" | "gaia" | "ours-extra"
    std::string Case;        // e.g. "CreateEntities"
    std::size_t N = 0;
    double TotalMsMedian = 0.0;  // median wall time of one timed run
    double NsPerEntity = 0.0;    // TotalMsMedian * 1e6 / N
    std::vector<double> SamplesMs;
    std::string Note;        // methodology footnote for this row, if any

    void Finalize()
    {
        TotalMsMedian = MedianOf(SamplesMs);
        NsPerEntity = (N > 0) ? (TotalMsMedian * 1.0e6) / static_cast<double>(N) : 0.0;
    }
};

struct BenchConfig {
    std::vector<std::size_t> Sizes = {1'000, 16'384, 65'536, 262'144, 1'048'576};
    int StructuralWarmups = 2;  // create/destroy/add-remove/random-get
    int StructuralRuns = 5;
    int IterateWarmups = 3;     // per-frame iteration cases
    int IterateRuns = 9;
};

// Shared sampling loop for per-frame iteration cases: warmups, then timed
// samples of `reps` back-to-back passes each (see RepsFor), normalized to one
// pass.
template <typename PassFn>
void SampleIteratePasses(CaseResult& r, const BenchConfig& cfg, std::size_t n, PassFn&& pass)
{
    const int reps = RepsFor(n);
    for (int run = -cfg.IterateWarmups; run < cfg.IterateRuns; ++run)
    {
        const auto t0 = Clock::now();
        for (int rep = 0; rep < reps; ++rep)
            pass();
        const auto t1 = Clock::now();
        if (run >= 0)
            r.SamplesMs.push_back(MsBetween(t0, t1) / reps);
    }
}

// One entry point per framework (each in its own TU to isolate headers).
std::vector<CaseResult> RunOursBenches(const BenchConfig& cfg);
std::vector<CaseResult> RunEnttBenches(const BenchConfig& cfg);
std::vector<CaseResult> RunFlecsBenches(const BenchConfig& cfg);
std::vector<CaseResult> RunGaiaBenches(const BenchConfig& cfg);

} // namespace bench
