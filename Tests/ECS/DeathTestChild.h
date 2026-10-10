#pragma once
// Setup for the child process of a gtest death test that is expected to die on a CRT
// assert. Only Windows can pop a modal dialog that would hang the child; elsewhere an
// aborting child just dies and gtest reads its stderr.
#if defined(_WIN32)
#include <cstdlib>
#endif

namespace GameEngine::ECS::test
{
inline void SuppressCrtDialogsInDeathTestChild()
{
#if defined(_WIN32)
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG);
#endif
}
} // namespace GameEngine::ECS::test
