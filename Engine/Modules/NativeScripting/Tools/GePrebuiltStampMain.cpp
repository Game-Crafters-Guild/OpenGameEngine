// GePrebuiltStamp — engine-build helper for staging prebuilt engine-package
// module binaries (PrebuiltModuleBinaries.h layout). Three modes, each writing
// its single-line result to <outfile> (a file, not stdout, so Logger output
// can never contaminate the value a CMake script reads back):
//
//   GePrebuiltStamp platformdir <outfile>
//       The prebuilt platform dir name for THIS build's toolchain, e.g.
//       "windows-x64-1a2b3c4d". This binary compiles with the same global
//       flags as the engine, so its HostToolchainFingerprint matches the
//       editor that will probe the staged dir.
//
//   GePrebuiltStamp modulefile <moduleName> <outfile>
//       The file name the loader probes for <moduleName> in that dir, e.g.
//       "Eztree.dylib" (PrebuiltModuleFileName). The build stages its MODULE
//       library output under this name whatever the toolchain called it.
//
//   GePrebuiltStamp abidigest <sdkRoot> editor|runtime <outfile> [define ...]
//       The engine-ABI digest a module built against the STAGED SDK at
//       <sdkRoot> carries — the same LoadSdkManifest + AppendPackageDefines +
//       ComputeEngineAbiDigest pipeline the editor's package-module wiring
//       runs, so the engine_abi marker written from this value matches what
//       the loader recomputes at project open. Runtime modules clear the
//       EditorSDK interface exactly like the editor wiring does.

#include "NativeScripting/BuildCacheRecord.h"
#include "NativeScripting/PrebuiltModuleBinaries.h"
#include "NativeScripting/SdkManifest.h"
#include "NativeScripting/ToolchainFingerprint.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace
{

int WriteResult(const char* outfile, const std::string& value)
{
    std::ofstream out(outfile, std::ios::trunc);
    if (!out)
    {
        std::fprintf(stderr, "GePrebuiltStamp: cannot write '%s'\n", outfile);
        return 1;
    }
    out << value << '\n';
    return out ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    using namespace GameEngine::NativeScripting;

    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "platformdir" && argc == 3)
        return WriteResult(argv[2], PrebuiltPlatformDirName(HostToolchainFingerprint()));

    if (mode == "modulefile" && argc == 4)
        return WriteResult(argv[3], PrebuiltModuleFileName(argv[2]));

    if (mode == "abidigest" && argc >= 5)
    {
        NativeBuildConfig config;
        std::string error;
        if (!LoadSdkManifest(argv[2], config, error))
        {
            std::fprintf(stderr, "GePrebuiltStamp: %s\n", error.c_str());
            return 1;
        }
        const std::string kind = argv[3];
        if (kind == "runtime")
        {
            // Runtime modules never fold the editor surface into their digest
            // (mirrors the editor's package-module wiring).
            config.EditorImportLib.clear();
            config.EditorIncludeDirs.clear();
        }
        else if (kind != "editor")
        {
            std::fprintf(stderr, "GePrebuiltStamp: module kind must be editor|runtime, got '%s'\n",
                         kind.c_str());
            return 2;
        }
        if (kind == "editor" && config.EditorImportLib.empty())
        {
            std::fprintf(stderr,
                         "GePrebuiltStamp: SDK at '%s' has no EditorSDK interface — cannot digest "
                         "an editor-kind module against it\n",
                         argv[2]);
            return 1;
        }
        std::vector<std::string> defines;
        for (int i = 5; i < argc; ++i)
            defines.emplace_back(argv[i]);
        AppendPackageDefines(config, defines);
        return WriteResult(argv[4], ComputeEngineAbiDigest(config));
    }

    std::fprintf(stderr, "usage: GePrebuiltStamp platformdir <outfile>\n"
                         "       GePrebuiltStamp modulefile <moduleName> <outfile>\n"
                         "       GePrebuiltStamp abidigest <sdkRoot> editor|runtime <outfile> "
                         "[define ...]\n");
    return 2;
}
