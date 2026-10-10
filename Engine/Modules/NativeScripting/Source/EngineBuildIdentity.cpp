#include "NativeScripting/EngineBuildIdentity.h"

#include "NativeScripting/BuildCacheRecord.h" // Fnv1aHash / ToHexDigest

#include <cstdint>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#elif defined(__APPLE__)
#  include <dlfcn.h>
#  include <mach-o/loader.h>
#elif defined(__EMSCRIPTEN__)
// A wasm module has no ELF image and no dl_iterate_phdr; nothing to include.
#else
#  include <dlfcn.h>
#  include <elf.h>
#  include <link.h>
#endif

#if defined(_WIN32)
// The linker-provided base of the module this TU is linked into — a link-time
// constant, resolved with no call and no lock. Deliberately NOT GetModuleHandleEx,
// which takes the loader lock: this runs on the path that is about to LoadLibrary
// a user module, and loader-lock calls stay off that path.
extern "C" IMAGE_DOS_HEADER __ImageBase;
#endif

namespace GameEngine
{
namespace NativeScripting
{

namespace
{

#if !defined(_WIN32)
// Address inside THIS translation unit, i.e. inside the engine image. The POSIX
// queries below start by asking "which module contains this address", which is
// correct whether NativeScripting is spliced into the engine binary (it is today
// — an OBJECT library on Engine's source list) or ever lands in a library of its
// own: the answer is always the image whose ABI the guard cares about.
void IdentityAnchor() {}
#endif

#if defined(_WIN32)

// The linker writes a fresh CodeView GUID+Age on every link, so it is the exact
// "this build of this binary" stamp. Read from the MAPPED image: the debug
// directory's AddressOfRawData is an RVA, valid in memory (PointerToRawData is
// the file offset and would be wrong here).
std::string ReadImageStamp()
{
    const auto* base = reinterpret_cast<const unsigned char*>(&__ImageBase);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return {};
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return {};

    const IMAGE_DATA_DIRECTORY& dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (dir.VirtualAddress != 0 && dir.Size >= sizeof(IMAGE_DEBUG_DIRECTORY))
    {
        const auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + dir.VirtualAddress);
        for (DWORD i = 0; i < dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY); ++i)
        {
            if (entries[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW || entries[i].AddressOfRawData == 0)
                continue;
            // RSDS record: 4-byte signature, 16-byte GUID, 4-byte age, then the
            // PDB path. Only the GUID+age are the identity; the path is where the
            // symbols happened to be written and differs between worktrees that
            // produced byte-identical engines.
            constexpr DWORD kRsdsPrefix = 4 + 16 + 4;
            if (entries[i].SizeOfData < kRsdsPrefix)
                continue;
            const auto* cv = reinterpret_cast<const char*>(base + entries[i].AddressOfRawData);
            if (cv[0] != 'R' || cv[1] != 'S' || cv[2] != 'D' || cv[3] != 'S')
                continue;
            return std::string(cv + 4, kRsdsPrefix - 4);
        }
    }

    // No CodeView record (symbols stripped). The optional header always exists,
    // and these fields move together on a relink, so identity survives — it just
    // stops being guaranteed-unique. Both writer and reader use this same
    // fallback on such a build, so comparisons stay consistent.
    struct HeaderStamp
    {
        std::uint32_t TimeDateStamp;
        std::uint32_t SizeOfImage;
        std::uint32_t CheckSum;
        std::uint32_t EntryPoint;
    } stamp{nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage, nt->OptionalHeader.CheckSum,
            nt->OptionalHeader.AddressOfEntryPoint};
    return std::string(reinterpret_cast<const char*>(&stamp), sizeof(stamp));
}

#elif defined(__APPLE__)

// LC_UUID is the Mach-O equivalent of the CodeView GUID: ld stamps a fresh one
// per link.
std::string ReadImageStamp()
{
    Dl_info info{};
    if (!::dladdr(reinterpret_cast<const void*>(&IdentityAnchor), &info) || !info.dli_fbase)
        return {};
    const auto* header = reinterpret_cast<const mach_header_64*>(info.dli_fbase);
    if (header->magic != MH_MAGIC_64)
        return {};
    const auto* cmd = reinterpret_cast<const load_command*>(header + 1);
    for (std::uint32_t i = 0; i < header->ncmds; ++i)
    {
        if (cmd->cmd == LC_UUID)
        {
            const auto* uuidCmd = reinterpret_cast<const uuid_command*>(cmd);
            return std::string(reinterpret_cast<const char*>(uuidCmd->uuid), sizeof(uuidCmd->uuid));
        }
        cmd = reinterpret_cast<const load_command*>(reinterpret_cast<const unsigned char*>(cmd) +
                                                   cmd->cmdsize);
    }
    return {};
}

#elif defined(__EMSCRIPTEN__)

// No ELF image and no dl_iterate_phdr: a wasm module is one immutable artefact fetched by the
// page, not a set of shared objects that can be relinked under a running process. The identity
// this stamp exists to detect a change in cannot change without a new module, so an empty stamp
// is the honest answer rather than a fabricated one.
std::string ReadImageStamp()
{
    return {};
}

#else

// ELF: .note.gnu.build-id, present whenever the link ran with --build-id (the
// default for GCC/Clang on the distros this engine targets). Absent otherwise,
// which surfaces as "cannot verify" rather than a false mismatch.
struct BuildIdSearch
{
    const void* Anchor;
    std::string Result;
};

int VisitObject(struct dl_phdr_info* info, std::size_t, void* userData)
{
    auto& search = *static_cast<BuildIdSearch*>(userData);
    Dl_info anchorInfo{};
    if (!::dladdr(search.Anchor, &anchorInfo) || !anchorInfo.dli_fbase)
        return 1; // stop: nothing to match against
    if (reinterpret_cast<const void*>(info->dlpi_addr) != anchorInfo.dli_fbase)
        return 0; // keep looking

    for (int i = 0; i < info->dlpi_phnum; ++i)
    {
        const ElfW(Phdr)& phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_NOTE)
            continue;
        const auto* cursor = reinterpret_cast<const unsigned char*>(info->dlpi_addr + phdr.p_vaddr);
        const unsigned char* end = cursor + phdr.p_memsz;
        while (cursor + sizeof(ElfW(Nhdr)) <= end)
        {
            const auto* note = reinterpret_cast<const ElfW(Nhdr)*>(cursor);
            const auto aligned = [](ElfW(Word) size) { return (size + 3u) & ~3u; };
            const unsigned char* name = cursor + sizeof(ElfW(Nhdr));
            const unsigned char* desc = name + aligned(note->n_namesz);
            if (desc + note->n_descsz > end)
                break;
            if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 &&
                std::string(reinterpret_cast<const char*>(name), 3) == "GNU")
            {
                search.Result.assign(reinterpret_cast<const char*>(desc), note->n_descsz);
                return 1;
            }
            cursor = desc + aligned(note->n_descsz);
        }
    }
    return 1; // the anchor's object was found; done either way
}

std::string ReadImageStamp()
{
    BuildIdSearch search{reinterpret_cast<const void*>(&IdentityAnchor), {}};
    ::dl_iterate_phdr(&VisitObject, &search);
    return search.Result;
}

#endif

} // namespace

const std::string& EngineBuildIdentity()
{
    // The stamp is fixed for the process lifetime — the engine image cannot be
    // relinked underneath a running process.
    static const std::string identity = [] {
        const std::string stamp = ReadImageStamp();
        return stamp.empty() ? std::string{} : ToHexDigest(Fnv1aHash(stamp));
    }();
    return identity;
}

} // namespace NativeScripting
} // namespace GameEngine
