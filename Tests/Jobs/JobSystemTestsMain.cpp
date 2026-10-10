// JobSystemTests entry point.
//
// Debug CRT asserts must never open a modal dialog in a test executable: on
// an unattended box (CI, agent-driven runs) the dialog wedges the whole
// suite silently — observed as the ~12-minute "wedge" in the participating-
// wait triage (PR #342 thread), where a deadline-tripped test destructed a
// live JobCounter and the dtor's Debug assert parked the run behind an
// invisible message box. Route CRT assert/error reports to stderr instead;
// a genuine assert then fails loudly and terminates like on any POSIX box.

#include <gtest/gtest.h>

#if defined(_WIN32) && defined(_DEBUG)
#include <crtdbg.h>
#endif

int main(int argc, char** argv)
{
#if defined(_WIN32) && defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
