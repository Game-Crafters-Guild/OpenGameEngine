#pragma once

#include "Animation/EvaluationContext.h"

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
namespace Animation
{

class HumanoidRig;
class RetargetMap;
struct AnimationPose;

// Per-op runtime context. Carries the source pose snapshot, the source rig
// metadata (used for canonical bone lookup), and the target rig metadata
// (used for parent walks and attachment resolution). The base
// EvaluationContext piggybacks; OpExecuteContext extends it with rig refs.
struct OpExecuteContext
{
    const EvaluationContext* Eval = nullptr;
    const HumanoidRig*       SourceRig = nullptr;
    const HumanoidRig*       TargetRig = nullptr;
    const RetargetMap*       Map       = nullptr;
    const AnimationPose*     SourcePose = nullptr; // post-Stage-0 source local pose
};

// Op base. Each op subclass parses its JSON params at construction time and
// applies a deterministic mutation to outPose during Execute().
//
// Phase 5 v1 set: AttachmentPassthroughOp, LookAtOp, FootLockOp.
// All registrations happen in C++ (no scriptable ops in v1 per plan §10).
//
// Lifetime: an OpStackNode is built once per RetargetMap load via the factory
// registry (CreateFromName). It outlives a single Execute call, so per-op
// state (e.g., FootLock previous-frame foot positions on the CPU path) lives
// inside the subclass.
class OpStackNode
{
public:
    virtual ~OpStackNode() = default;

    // Apply this op's mutation to outPose (the post-IK target local pose).
    // Must be deterministic and allocation-free after the first Execute.
    virtual void Execute(const OpExecuteContext& ctx, AnimationPose& outPose) = 0;

    // Stable string used for canonical-order classification + diagnostics.
    virtual const char* Name() const = 0;

    // Factory: parse a JSON params blob into a concrete op instance. Returns
    // nullptr when opName is unknown; the caller (RetargetOpStackPass) logs
    // once and skips. Thread-safe for read-only lookup; registration must
    // happen during static init only.
    using FactoryFn = std::function<std::unique_ptr<OpStackNode>(const nlohmann::json& params)>;
    static std::unique_ptr<OpStackNode> CreateFromName(const std::string& opName,
                                                       const nlohmann::json& params);
    static void RegisterOpFactory(const std::string& opName, FactoryFn factory);

    // Returns 0..N for known canonical positions, or kUnknownOpOrder for
    // unrecognized names. RetargetOpStackPass uses this to detect ops written
    // out of canonical order and to drive the default order when the
    // RetargetMap.OpStack is empty.
    //
    // Canonical order (plan §3 Stage 6.1):
    //   0: AttachmentPassthrough
    //   1: LookAt
    //   2: BodyIntersect (deferred to v2)
    //   3: FootLock
    static int CanonicalOrderForName(const std::string& opName);
    static constexpr int kUnknownOpOrder = -1;

private:
    static std::unordered_map<std::string, FactoryFn>& Registry();
};

// Helper: list of op names in plan-canonical order. Used by the orchestrator
// when the RetargetMap.OpStack is empty (Phase 5 default).
const std::vector<std::string>& CanonicalOpOrder();

// Force-link the v1 op factories. Call once before resolving any op by
// name (RetargetOpStackPass calls this on first dispatch; tests call it in
// SetUp). Linkers strip TUs that only register via static init when no
// downstream symbol references them; this gives us a hook.
void EnsureOpStackRegistrations();

} // namespace Animation
} // namespace GameEngine
