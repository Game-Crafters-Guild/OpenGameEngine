// Environment fixture for VCSCommandExecutorTests: prints the value of each
// environment variable named on its command line as [NAME=value], or
// [NAME unset] when the child did not receive it. What a spawn hands the child
// can only be read from the child's side.

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
    {
        const char* value = std::getenv(argv[i]);
        if (value != nullptr)
            std::printf("[%s=%s]\n", argv[i], value);
        else
            std::printf("[%s unset]\n", argv[i]);
    }
    return 0;
}
