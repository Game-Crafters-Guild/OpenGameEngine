// Persistence gate for the browser build's project storage (web platform plan,
// Phase 7): does a write to the OPFS mount survive the page?
//
// The claim being tested is not "the write returned success" — a MEMFS write
// does that too and is gone on reload. So this smoke keeps a counter file on
// the mount and reports the value it found there. A reload re-instantiates the
// module with fresh linear memory, so run=2 on the second load can only have
// come off the browser's storage.
//
// Serve build/<preset>/bin and load WebStorageBrowserSmoke.html, or drive it
// with Tools/Web/browser_gate.py --page WebStorageBrowserSmoke.html --reloads 1.

#include "Platform/WebPersistentStorage.h"

#include <emscripten/emscripten.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace
{

// The mount the editor uses for project content, so this gate exercises the
// same path and not a private one.
constexpr const char kMountPoint[] = "/project";
constexpr const char kCounterFile[] = "/project/persistence-probe.txt";

// A directory under the mount: the counter alone would pass even if only the
// mount root were writable, and a project is a tree.
constexpr const char kNestedDir[] = "/project/Assets/Scenes";
constexpr const char kNestedFile[] = "/project/Assets/Scenes/probe.scene";

int ReadCounter()
{
    std::ifstream in(kCounterFile);
    if (!in)
    {
        return 0;
    }
    int value = 0;
    in >> value;
    return in.fail() ? -1 : value;
}

bool WriteCounter(int value)
{
    std::ofstream out(kCounterFile, std::ios::trunc);
    if (!out)
    {
        return false;
    }
    out << value << "\n";
    out.close();
    return !out.fail();
}

// Enumeration probes. std::filesystem::directory_iterator and raw opendir take
// different syscall paths, so a failure in one and not the other names the
// layer at fault instead of just "listing does not work".
void Probe(const char* label, const char* path)
{
    DIR* dir = opendir(path);
    if (dir == nullptr)
    {
        std::printf("OPFSSMOKE: %s opendir failed (%s)\n", label, std::strerror(errno));
        return;
    }
    int count = 0;
    std::string names;
    errno = 0;
    while (const dirent* entry = readdir(dir))
    {
        ++count;
        names += " ";
        names += entry->d_name;
    }
    const int readdirErrno = errno;
    closedir(dir);
    std::printf("OPFSSMOKE: %s opendir ok count=%d errno=%s names=%s\n", label, count,
                readdirErrno == 0 ? "0" : std::strerror(readdirErrno), names.c_str());
}

void Iterate(const char* label, const char* path)
{
    std::error_code ec;
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(path, ec))
    {
        (void)entry;
        ++count;
    }
    std::printf("OPFSSMOKE: %s directory_iterator count=%d ec=%s\n", label, count,
                ec ? ec.message().c_str() : "0");
}

} // namespace

int main()
{
    std::printf("OPFSSMOKE: start\n");

    if (!GameEngine::Platform::Web::MountPersistentStorage(kMountPoint))
    {
        std::printf("OPFSSMOKE: FAIL mount\n");
        return 1;
    }
    std::printf("OPFSSMOKE: mounted %s\n", kMountPoint);

    const int previous = ReadCounter();
    if (previous < 0)
    {
        std::printf("OPFSSMOKE: FAIL counter file is unreadable\n");
        return 1;
    }
    const int run = previous + 1;
    if (!WriteCounter(run))
    {
        std::printf("OPFSSMOKE: FAIL write counter\n");
        return 1;
    }

    // std::filesystem against the mount, not just fopen: the editor resolves
    // project paths through it.
    std::error_code ec;
    std::filesystem::create_directories(kNestedDir, ec);
    if (ec)
    {
        std::printf("OPFSSMOKE: FAIL create_directories %s (%s)\n", kNestedDir,
                    ec.message().c_str());
        return 1;
    }
    {
        std::ofstream out(kNestedFile, std::ios::trunc);
        out << "run " << run << "\n";
    }
    const bool nestedExists = std::filesystem::exists(kNestedFile, ec) && !ec;
    const auto nestedSize = nestedExists ? std::filesystem::file_size(kNestedFile, ec) : 0;

    std::printf("OPFSSMOKE: run=%d nested=%d nestedBytes=%llu\n", run,
                nestedExists ? 1 : 0, static_cast<unsigned long long>(nestedSize));

    // Directory enumeration is how the picker will find projects on the mount,
    // and how the asset registry scans it. Probed from both threads on purpose:
    // the OPFS backend proxies its work to a dedicated worker, and the browser's
    // main thread reaches that worker by a different route (ASYNCIFY unwinding)
    // than a pthread does (a blocking proxy call). A failure on one and not the
    // other names the route, not the filesystem.
    Probe("main-opendir-root", kMountPoint);
    Probe("main-opendir-nested", kNestedDir);
    Iterate("main-iterator-root", kMountPoint);
    Iterate("main-iterator-nested", kNestedDir);

    std::thread worker([] {
        Probe("thread-opendir-root", kMountPoint);
        Probe("thread-opendir-nested", kNestedDir);
        Iterate("thread-iterator-root", kMountPoint);
        Iterate("thread-iterator-nested", kNestedDir);
    });
    worker.join();
    std::printf("OPFSSMOKE: PASS run=%d\n", run);
    return 0;
}
