#include "Core/NvtxRange.h"

// GE_ENABLE_NVTX is PRIVATE to the Engine target (Engine/CMakeLists.txt), which
// is the only place the NVTX include path is set. Any other target that ever
// compiles this file gets the no-op bodies rather than a missing-header error.
#ifndef GE_ENABLE_NVTX
#define GE_ENABLE_NVTX 0
#endif

#if GE_ENABLE_NVTX
#include <nvtx3/nvToolsExt.h>

#include <cstdlib>
#include <cstring>
#endif

namespace GameEngine::Profiling
{

#if GE_ENABLE_NVTX
namespace
{
// Written once by InitializeNvtx before any worker thread starts; read-only
// afterwards, so no synchronization is needed on the range hot path.
bool s_NvtxEnabled = false;
// Never destroyed. The domain must outlive every range on every thread, and
// nvtxDomainDestroy during static teardown would race threads still unwinding
// scopes. A single process-lifetime handle reclaimed by process exit is the
// intended trade, not an oversight.
nvtxDomainHandle_t s_Domain = nullptr;
} // namespace
#endif

void InitializeNvtx()
{
#if GE_ENABLE_NVTX
    const char* env = std::getenv("GE_NVTX");
    // Truthy unless explicitly "0": tracing is on by default when compiled in,
    // because the cost of an unattached NVTX range is a predicate on a
    // process-local function pointer.
    s_NvtxEnabled = !(env && std::strcmp(env, "0") == 0);
    if (s_NvtxEnabled && !s_Domain)
        s_Domain = nvtxDomainCreateA("GameEngine");
#endif
}

ScopedNvtxRange::ScopedNvtxRange(const char* name)
    : m_Active(false)
{
#if GE_ENABLE_NVTX
    if (!s_NvtxEnabled || !s_Domain || !name)
        return;

    nvtxEventAttributes_t attributes{};
    attributes.version = NVTX_VERSION;
    attributes.size = NVTX_EVENT_ATTRIB_STRUCT_SIZE;
    attributes.messageType = NVTX_MESSAGE_TYPE_ASCII;
    attributes.message.ascii = name;
    nvtxDomainRangePushEx(s_Domain, &attributes);
    m_Active = true;
#else
    (void)name;
#endif
}

ScopedNvtxRange::~ScopedNvtxRange()
{
    if (!m_Active)
        return;
#if GE_ENABLE_NVTX
    nvtxDomainRangePop(s_Domain);
#endif
}

} // namespace GameEngine::Profiling
