// Argument-fidelity fixture for the RunProcessCaptured and VCSCommandExecutor
// tests: prints every argument it receives, one per line, wrapped in brackets so
// an empty argument and a dropped argument are distinguishable in the transcript.
//
// A shell-free spawn can only be proven from the child's side — the parent's
// command line says what was sent, not what arrived.

#include <cstdio>

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        std::printf("[%s]\n", argv[i]);
    return 0;
}
