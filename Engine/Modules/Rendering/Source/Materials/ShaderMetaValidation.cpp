#include "Rendering/Materials/ShaderMetaValidation.h"
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace GameEngine { namespace Rendering {

static void AddIssue(ValidationReport& r, IssueSeverity s, const char* code, const std::string& msg) {
    r.Issues.push_back(ValidationIssue{ s, code, msg });
}

// Bitmask of the four 32-bit components a stage-IO variable occupies within its
// location. Scalars and vectors pack (`layout(component = N)`), so several
// narrow variables legally share one location while their spans stay disjoint —
// the interface slot is (location, component), not location alone. Matrices,
// structs and arrays straddle locations in ways this meta does not model, so
// they claim the whole location, which is the pre-component behaviour for them.
static uint32_t StageIOComponentMask(const StageIO& io) {
    constexpr uint32_t kAllComponents = 0xFu;
    if (!io.Type.ArrayDims.empty())
        return kAllComponents;
    if (io.Type.Kind != TypeKind::Scalar && io.Type.Kind != TypeKind::Vector)
        return kAllComponents;
    const uint32_t width = io.Type.Kind == TypeKind::Vector ? io.Type.VecSize : 1u;
    if (width == 0u || io.Component >= 4u || io.Component + width > 4u)
        return kAllComponents;
    return ((1u << width) - 1u) << io.Component;
}

// Reports every variable whose component span overlaps one already claimed at
// the same location. Returns the issue text, or an empty string when the slot
// is free; `claimed` accumulates across the direction being checked.
static std::string ClaimStageIOSlot(std::unordered_map<uint32_t, uint32_t>& claimed,
                                    const StageIO& io) {
    const uint32_t mask = StageIOComponentMask(io);
    uint32_t& occupied = claimed[io.Location];
    if ((occupied & mask) == 0u) {
        occupied |= mask;
        return {};
    }
    std::ostringstream oss;
    oss << "location " << io.Location << " component " << io.Component;
    return oss.str();
}

ValidationReport ValidateShaderMeta(const ShaderMeta& meta, uint32_t pushConstantMaxBytes) {
    ValidationReport report{};

    // Push-constant size policy
    uint32_t totalPc = 0;
    for (const auto& pc : meta.PushConstants) totalPc += pc.Size;
    if (pushConstantMaxBytes > 0 && totalPc > pushConstantMaxBytes) {
        std::ostringstream oss; oss << "Total push-constant bytes " << totalPc << " exceeds limit " << pushConstantMaxBytes;
        AddIssue(report, IssueSeverity::Error, "PushConstantsTotalBytesExceedLimit", oss.str());
    }

    // Spec constants unique ids
    {
        std::unordered_set<uint32_t> seen;
        for (const auto& sc : meta.SpecConstants) {
            if (!seen.insert(sc.Id).second) {
                std::ostringstream oss; oss << "Duplicate specialization constant id: " << sc.Id;
                AddIssue(report, IssueSeverity::Error, "DuplicateSpecConstantId", oss.str());
            }
        }
    }

    // Stage IO: no overlapping (location, component) spans within each direction
    // per stage. Component-packed variables sharing a location are legal.
    for (const auto& [stageName, stage] : meta.Stages) {
        std::unordered_map<uint32_t, uint32_t> inSlots, outSlots;
        for (const auto& i : stage.Inputs) {
            const std::string clash = ClaimStageIOSlot(inSlots, i);
            if (!clash.empty()) {
                std::ostringstream oss; oss << stageName << ": duplicate input " << clash;
                AddIssue(report, IssueSeverity::Error, "DuplicateStageIOLocation", oss.str());
            }
        }
        for (const auto& o : stage.Outputs) {
            const std::string clash = ClaimStageIOSlot(outSlots, o);
            if (!clash.empty()) {
                std::ostringstream oss; oss << stageName << ": duplicate output " << clash;
                AddIssue(report, IssueSeverity::Error, "DuplicateStageIOLocation", oss.str());
            }
        }
    }

    // Compute local size sanity
    if (auto it = meta.Stages.find("cs"); it != meta.Stages.end()) {
        const auto& s = it->second;
        if (s.ComputeLocalSize) {
            if (s.ComputeLocalSize->X == 0 || s.ComputeLocalSize->Y == 0 || s.ComputeLocalSize->Z == 0) {
                AddIssue(report, IssueSeverity::Error, "InvalidComputeLocalSize", "Compute local size has zero dimension");
            }
        }
    }

    // Propagate conflicts recorded during merge as errors
    for (const auto& req : meta.Requirements) {
        if (req.rfind("conflict:", 0) == 0) {
            AddIssue(report, IssueSeverity::Error, "DescriptorConflict", req);
        }
    }

    return report;
}

}} // namespace GameEngine::Rendering

