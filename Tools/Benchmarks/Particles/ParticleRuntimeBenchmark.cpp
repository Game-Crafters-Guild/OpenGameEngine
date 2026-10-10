// Diagnostic executable, not a test: CPU time of the particle runtime per frame for a fixed stack
// at several particle counts and emitter splits. Numbers mean something only from a Release build
// on a quiet machine; the unit suites carry no timing thresholds.

#include "Particles/CompiledParticleStack.h"
#include "Particles/ParticleRuntime.h"
#include "Particles/ParticleStackDocument.h"
#include "Particles/Processors/ParticleAccelerationProcessor.h"
#include "Particles/Processors/ParticleCollisionProcessor.h"
#include "Particles/Processors/ParticleDragProcessor.h"
#include "Particles/Processors/ParticleEmitRateProcessor.h"
#include "Particles/Processors/ParticleNoiseProcessor.h"
#include "Particles/Processors/ParticlePropertyProcessor.h"
#include "Particles/Processors/ParticleShapeProcessor.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace GameEngine;
using namespace GameEngine::Particles;

namespace
{
constexpr double kFrame = 1.0 / 60.0;
constexpr int kWarmUpFrames = 180;
constexpr int kMeasuredFrames = 600;
constexpr float kLifetime = 2.0f;

void Append(StackDocument& document, ParticleProcessorInstance processor)
{
    uint32 id = 0;
    std::string error;
    AddProcessor(document, 1, std::move(processor), id, error);
}

template <typename T>
ParticleProcessorInstance Processor(const ParticleProcessorDescriptor& descriptor, const T& parameters,
                                    ParticleStage stage)
{
    auto processor = MakeProcessorInstance(descriptor);
    processor.Params<T>() = parameters;
    processor.Stage = stage;
    return processor;
}

// A steady population of `particles` per emitter: emission from a sphere, a random upward
// velocity, gravity, drag, noise, size and alpha over life, and a ground plane.
StackDocument BenchmarkStack(uint32 particles)
{
    StackDocument document;
    document.Lifetime = kLifetime;
    document.Phases.emplace_back();
    Append(document, Processor(ParticleEmitRateProcessor(),
                               ParticleEmitRateParameters{.Rate = static_cast<float>(particles) / kLifetime},
                               ParticleStage::Emission));
    Append(document, Processor(ParticleShapeProcessor(), ParticleShapeParameters{.Radius = 0.5f}, ParticleStage::Birth));
    ParticlePropertyParameters velocity{.Target = ParticleAttribute::Velocity, .Operation = ParticleOperation::Set};
    velocity.Value[0] = {-1.0f, 1.0f};
    velocity.Value[1] = {4.0f, 6.0f};
    velocity.Value[2] = {-1.0f, 1.0f};
    Append(document, Processor(ParticlePropertyProcessor(), velocity, ParticleStage::Birth));
    Append(document, Processor(ParticleAccelerationProcessor(), ParticleAccelerationParameters{}, ParticleStage::Update));
    Append(document, Processor(ParticleDragProcessor(), ParticleDragParameters{.Drag = 0.3f}, ParticleStage::Update));
    Append(document, Processor(ParticleNoiseProcessor(), ParticleNoiseParameters{.Strength = 0.5f}, ParticleStage::Update));
    ParticlePropertyParameters size{.Target = ParticleAttribute::Size, .Operation = ParticleOperation::Multiply};
    size.Value[0] = ParticleValue::MakeCurve({{0.0f, 0.5f}, {1.0f, 2.0f}});
    Append(document, Processor(ParticlePropertyProcessor(), size, ParticleStage::Update));
    ParticlePropertyParameters alpha{.Target = ParticleAttribute::Alpha, .Operation = ParticleOperation::Set};
    alpha.Value[0] = ParticleValue::MakeCurve({{0.0f, 1.0f}, {1.0f, 0.0f}});
    Append(document, Processor(ParticlePropertyProcessor(), alpha, ParticleStage::Update));
    Append(document, Processor(ParticleCollisionProcessor(), ParticleCollisionParameters{.Bounce = 0.4f},
                               ParticleStage::Update));
    return document;
}

void AdvanceAll(std::vector<ParticleRuntime>& runtimes)
{
    ParticleSimulationInput input;
    input.FixedFps = 60;
    for (uint32 index = 0; index < runtimes.size(); ++index)
    {
        input.Seed = index + 1;
        runtimes[index].Advance(input, kFrame);
    }
}

double Percentile(std::vector<double> samples, double fraction)
{
    std::sort(samples.begin(), samples.end());
    return samples[static_cast<size_t>(fraction * static_cast<double>(samples.size() - 1))];
}

void Measure(uint32 emitters, uint32 particlesPerEmitter)
{
    using Clock = std::chrono::steady_clock;
    std::vector<StackDiagnostic> diagnostics;
    const auto stack =
        CompileParticleStack(std::make_shared<const StackDocument>(BenchmarkStack(particlesPerEmitter)), diagnostics);
    if (!stack)
    {
        std::printf("stack failed to compile: %s\n", diagnostics.empty() ? "" : diagnostics.front().Message.c_str());
        return;
    }
    std::vector<ParticleRuntime> runtimes(emitters);
    for (auto& runtime : runtimes)
        runtime.Bind(stack, particlesPerEmitter);
    for (int frame = 0; frame < kWarmUpFrames; ++frame)
        AdvanceAll(runtimes);
    std::vector<double> milliseconds;
    milliseconds.reserve(kMeasuredFrames);
    uint32 live = 0;
    for (int frame = 0; frame < kMeasuredFrames; ++frame)
    {
        const auto begin = Clock::now();
        AdvanceAll(runtimes);
        milliseconds.push_back(std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
    }
    for (const auto& runtime : runtimes)
        live += runtime.Count();
    const double median = Percentile(milliseconds, 0.5);
    std::printf("emitters=%u particles_per_emitter=%u live=%u frame_ms_p50=%.4f frame_ms_p95=%.4f "
                "ms_per_10000_particles_p50=%.4f\n",
                emitters, particlesPerEmitter, live, median, Percentile(milliseconds, 0.95),
                live > 0 ? median * 10000.0 / live : 0.0);
}
} // namespace

int main()
{
    Measure(10, 1000);
    Measure(100, 100);
    Measure(3, 4096);
    Measure(1000, 10);
    return 0;
}
