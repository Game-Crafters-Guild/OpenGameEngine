#pragma once

#include "Jobs/HotReloadTestHooks.h"

namespace GameEngine::Tests
{

// Clears the process-global CompileServer test seams on every test exit path.
//
// The seams (SetOnCompileServerJsonBuiltForTests / the transport factory) are
// consumed by pool worker threads during later compiles, so a hook that
// captures test locals by reference MUST be unregistered before the test
// returns. Trailing Set*(nullptr) calls don't survive GTEST_SKIP / fatal
// ASSERT exits — that exact leak let a skipped test's dangling &lastRequest
// hook fire three tests later inside CompileServerCompiler::compile and AV
// the whole HotReloadPipelineTests run.
//
// Declare an instance at the top of any test that registers either seam:
// destruction happens after the test's pool has joined its workers (reverse
// declaration order), so no worker can observe the seams mid-teardown.
struct ScopedHotReloadTestHooks
{
    ScopedHotReloadTestHooks() = default;
    ScopedHotReloadTestHooks(const ScopedHotReloadTestHooks&) = delete;
    ScopedHotReloadTestHooks& operator=(const ScopedHotReloadTestHooks&) = delete;

    ~ScopedHotReloadTestHooks()
    {
        SetOnCompileServerJsonBuiltForTests(nullptr);
        SetCompileServerTransportFactoryForTests(nullptr);
    }
};

} // namespace GameEngine::Tests
