#include "Animation/OpStackNode.h"

#include "Animation/Ops/AttachmentPassthroughOp.h"
#include "Animation/Ops/FootLockOp.h"
#include "Animation/Ops/LookAtOp.h"

namespace GameEngine
{
namespace Animation
{

namespace
{

// Force-registration of the v1 op set on first call. Linkers may strip
// translation units that have only static-init side effects when nothing
// else references them; routing through an explicit symbol that op-stack
// orchestration calls keeps the registrations live without relying on
// init-order tricks across linker boundaries.
struct OpRegistrar
{
    OpRegistrar()
    {
        OpStackNode::RegisterOpFactory("AttachmentPassthrough",
                                       &AttachmentPassthroughOp::Create);
        OpStackNode::RegisterOpFactory("LookAt",   &LookAtOp::Create);
        OpStackNode::RegisterOpFactory("FootLock", &FootLockOp::Create);
    }
};

OpRegistrar g_OpRegistrar;

} // namespace

// Public no-op symbol; calling this from another TU forces this object file
// to be linked, which in turn pulls in g_OpRegistrar's static init. The
// orchestrator (RetargetOpStackPass) calls EnsureOpStackRegistrations() at
// startup so the v1 ops are always reachable via CreateFromName.
void EnsureOpStackRegistrations()
{
    static_cast<void>(&g_OpRegistrar);
}

} // namespace Animation
} // namespace GameEngine
