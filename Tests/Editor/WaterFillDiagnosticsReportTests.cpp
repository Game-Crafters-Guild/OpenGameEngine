// What the water fill's refusal warnings SAY.
//
// The field's own tests pin the numbers; the thing only this side can check is
// that the author-facing line CARRIES them. A position threaded into the
// diagnostics and then dropped by the reporter reads exactly like the defect it
// was added to fix: a count, a magnitude, and no way to find the sample.

#include <gtest/gtest.h>

#include "Placement/SplineFillRebuild.h"

#include "Logger/CallbackSink.h"
#include "Logger/Logger.h"

#include <mutex>
#include <string>
#include <vector>

using GameEngine::Editor::ReportWaterFillDiagnostics;
using GameEngine::Editor::WaterFillOutcome;
using GameEngine::Editor::WaterFillRun;

namespace SG = GameEngine::SplineGeometry;

namespace
{

// Every SplineExtrude line the reporter logs. The sink outlives the capture
// (the logger owns it); only the callback is unregistered, so nothing dangles.
class ReportCapture
{
  public:
    ReportCapture()
    {
        // The logger's effective level is Off until it is configured, and an
        // unconfigured logger would make every assertion below vacuous.
        Logger::Log::Initialize({Logger::LogLevel::Debug, false});
        auto sink = Logger::MakeUnique<Logger::CallbackSink>();
        m_Sink = sink.get();
        Logger::Log::AddSink(std::move(sink));
        m_CallbackId = m_Sink->RegisterCallback(
            [this](const Logger::LogMessage& message)
            {
                if (message.Message.find("SplineExtrude:") == std::string::npos)
                    return;
                std::lock_guard<std::mutex> lock(m_Mutex);
                m_Lines.push_back(message.Message);
            });
    }

    ~ReportCapture() { m_Sink->UnregisterCallback(m_CallbackId); }

    // The captured line containing `needle`, or empty if none does.
    std::string LineContaining(const char* needle) const
    {
        Logger::Log::Flush();
        std::lock_guard<std::mutex> lock(m_Mutex);
        for (const std::string& line : m_Lines)
            if (line.find(needle) != std::string::npos)
                return line;
        return {};
    }

  private:
    Logger::CallbackSink* m_Sink = nullptr;
    Logger::uint64 m_CallbackId = 0;
    mutable std::mutex m_Mutex;
    std::vector<std::string> m_Lines;
};

// A run that refuses on both counts, with two DIFFERENT worst samples so a
// line that quotes the wrong one is a failure rather than a coincidence. Every
// coordinate is exact in binary and prints without rounding at the precision
// the messages use, so the expected substrings are the format's own output.
WaterFillRun RefusingRun()
{
    WaterFillRun run;
    run.Outcome = WaterFillOutcome::Built;
    run.Diagnostics.Seeds = 240;
    run.Diagnostics.WetCorners = 8100;
    run.Diagnostics.OverBankCorners = 37;
    run.Diagnostics.MaxOverBankMetres = 0.25f;
    run.Diagnostics.WorstOverBank = {-46.25f, 13.75f, 61.5f};
    run.Diagnostics.UnseenBankCorners = 12;
    run.Diagnostics.WorstUnseenBank = {-51.5f, 24.0f, 78.5f};
    return run;
}

// The reporter is throttled on the counters it last emitted; a zeroed record
// and a mismatched outcome are what "this is new" looks like to it.
void ReportOnce(const WaterFillRun& run)
{
    SG::SplineFillDiagnostics lastReported{};
    WaterFillOutcome lastOutcome = WaterFillOutcome::NoTerrain;
    ReportWaterFillDiagnostics(run, 77u, lastReported, lastOutcome);
}

} // namespace

TEST(WaterFillDiagnosticsReport, TheOverBankWarningNamesTheWorstSample)
{
    ReportCapture capture;
    ReportOnce(RefusingRun());

    const std::string line = capture.LineContaining("standing over its own banks");
    ASSERT_FALSE(line.empty()) << "the over-bank refusal was not reported at all";
    EXPECT_NE(line.find("(-46.25, 13.75)"), std::string::npos) << line;
    EXPECT_NE(line.find("61.5 m along the run"), std::string::npos) << line;
    EXPECT_NE(line.find("0.25 m"), std::string::npos) << line;
}

TEST(WaterFillDiagnosticsReport, TheUnseenBankWarningNamesItsOwnWorstSample)
{
    ReportCapture capture;
    ReportOnce(RefusingRun());

    const std::string line = capture.LineContaining("could not see a bank");
    ASSERT_FALSE(line.empty()) << "the unseen-bank refusal was not reported at all";
    EXPECT_NE(line.find("(-51.50, 24.00)"), std::string::npos) << line;
    EXPECT_NE(line.find("78.5 m along the run"), std::string::npos) << line;
    EXPECT_EQ(line.find("13.75"), std::string::npos)
        << "this warning is quoting the over-bank sample, not its own: " << line;
}
