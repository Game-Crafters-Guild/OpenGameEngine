// OS shell integration on web. A page cannot spawn processes, hand a path to a
// system file manager, or browse the user's filesystem in place, so most entry
// points here report failure loudly rather than pretending to have acted.
//
// Two are real. OpenUrl is window.open, which is what callers mean; it can
// still be refused by a popup blocker outside a user gesture. SelectFolder is
// the File System Access directory picker, which imports what the user chose
// into the origin's storage — see the note on the import below.

#include "Platform/Shell.h"

#include "Platform/WebPersistentStorage.h"
#include "WebUnavailable.h"

#include <emscripten/em_asm.h>
#include <emscripten/em_js.h>
#include <emscripten/emscripten.h>

#include <cstdlib>
#include <string_view>

// Show the File System Access directory picker and copy what the user chose
// into the origin's persistent storage, returning the name it was stored under
// (null on cancel or when the browser has no such picker).
//
// A page cannot open a project in place on the user's disk: the handle the
// picker returns is not a path, and it does not survive a reload. So the folder
// is imported. The copy is handle-to-handle in JS -- both the picked directory
// and OPFS are FileSystemDirectoryHandle trees -- so nothing passes through
// linear memory and a large project need not fit in it. The destination is the
// same OPFS root WasmFS mounts, so the files are visible to the engine as soon
// as this returns.
//
// At file scope because EM_ASYNC_JS defines a JS-side function keyed by symbol
// name. Blocking is ASYNCIFY unwinding the caller while the browser runs the
// picker and the copy, which is why the editor's ASYNCIFY link is not optional.
EM_ASYNC_JS(char*, GeWebPickDirectoryIntoOpfs, (), {
    if (typeof globalThis.showDirectoryPicker !== 'function') {
        return 0;
    }
    let picked;
    try {
        picked = await globalThis.showDirectoryPicker({ mode: 'read' });
    } catch (e) {
        return 0; // AbortError is the user cancelling, which is not a failure.
    }

    const copyInto = async (from, to) => {
        for await (const [name, child] of from.entries()) {
            if (child.kind === 'directory') {
                await copyInto(child, await to.getDirectoryHandle(name, { create: true }));
            } else {
                const target = await to.getFileHandle(name, { create: true });
                const writable = await target.createWritable();
                await (await child.getFile()).stream().pipeTo(writable);
            }
        }
    };

    try {
        const opfsRoot = await navigator.storage.getDirectory();
        // A second import of the same folder replaces it rather than merging
        // into a half-updated tree.
        try {
            await opfsRoot.removeEntry(picked.name, { recursive: true });
        } catch (e) {}
        await copyInto(picked, await opfsRoot.getDirectoryHandle(picked.name, { create: true }));
    } catch (e) {
        console.error('GeWebPickDirectoryIntoOpfs: ' + ((e && e.message) || e));
        return 0;
    }
    return stringToNewUTF8(picked.name);
});

// Stage picked files into MEMFS so the caller can stream them into the
// project mount. showOpenFilePicker is Chromium; <input type=file> covers
// Firefox/Safari. ASYNCIFY unwinds the caller while the browser runs the
// picker, same as SelectFolder. The staging root is released through
// ReleaseTransientFiles once the caller has copied the files out.
EM_ASYNC_JS(char*, GeWebPickFilesIntoMemfs, (), {
    const files = [];
    if (typeof globalThis.showOpenFilePicker === 'function')
    {
        try
        {
            const handles = await globalThis.showOpenFilePicker({ multiple: true });
            for (const handle of handles)
                files.push(await handle.getFile());
        }
        catch (e)
        {
            return 0;
        }
    }
    else
    {
        const picked = await new Promise((resolve) => {
            const input = document.createElement('input');
            input.type = 'file';
            input.multiple = true;
            let settled = false;
            const finish = (list) => {
                if (settled)
                    return;
                settled = true;
                input.remove();
                resolve(list);
            };
            input.addEventListener('change', () => finish(Array.from(input.files || [])));
            input.addEventListener('cancel', () => finish([]));
            document.body.appendChild(input);
            input.click();
        });
        for (const file of picked)
            files.push(file);
    }
    if (files.length === 0)
        return 0;

    const fs = (typeof FS === 'object' && FS) ? FS : null;
    if (!fs || typeof fs.mkdirTree !== 'function' || typeof fs.writeFile !== 'function')
    {
        console.error('GeWebPickFilesIntoMemfs: MEMFS is not available');
        return 0;
    }

    const root = '/.web_picked_files/' + Date.now().toString();
    fs.mkdirTree(root);
    const used = {};
    for (const file of files)
    {
        let name = (file && file.name) ? String(file.name) : 'file';
        name = name.split('/').pop().split('\\\\').pop();
        if (!name)
            name = 'file';
        let dest = name;
        let n = 1;
        while (used[dest])
        {
            const dot = name.lastIndexOf('.');
            dest = (dot <= 0) ? (name + ' (' + n + ')')
                              : (name.slice(0, dot) + ' (' + n + ')' + name.slice(dot));
            n += 1;
        }
        used[dest] = 1;
        const data = new Uint8Array(await file.arrayBuffer());
        fs.writeFile(root + '/' + dest, data);
    }
    return stringToNewUTF8(root);
});

namespace GameEngine
{
namespace Platform
{

namespace
{
// Staging roots that a pick or an OS drop copies into. Emscripten's GLFW
// stages drops under /.glfw_dropped_files; picks go under /.web_picked_files.
bool IsTransientStagingPath(const std::filesystem::path& path)
{
    const std::string s = path.generic_string();
    for (const char* root : {"/.glfw_dropped_files", "/.web_picked_files"})
    {
        const std::string_view r(root);
        if (s == r || (s.size() > r.size() && s.compare(0, r.size(), r) == 0 && s[r.size()] == '/'))
            return true;
    }
    return false;
}
} // namespace

bool OpenUrl(const std::string& url)
{
    if (url.empty())
    {
        return false;
    }
    // Popup blockers refuse this outside a user gesture; the browser reports
    // nothing back, so a true here means "handed to the browser", not "shown".
    EM_ASM({ window.open(UTF8ToString($0), '_blank', 'noopener'); }, url.c_str());
    return true;
}

bool OpenPath(const std::filesystem::path& /*path*/)
{
    GE_PLATFORM_WEB_UNAVAILABLE("opening a path with the OS default handler");
    return false;
}

bool ShowInFileManager(const std::filesystem::path& /*path*/)
{
    GE_PLATFORM_WEB_UNAVAILABLE("revealing a path in the file manager");
    return false;
}

bool MoveToTrash(const std::filesystem::path& /*path*/)
{
    // The browser mounts (MEMFS/OPFS) have no trash. False, not a fake success:
    // callers state they fall back to a permanent remove and say so in the log.
    return false;
}

bool LaunchDetached(const std::filesystem::path& /*executable*/,
                    const std::vector<std::string>& /*arguments*/,
                    const std::filesystem::path& /*workingDirectory*/)
{
    GE_PLATFORM_WEB_UNAVAILABLE("launching a process");
    return false;
}

int LaunchDetachedGetPid(const std::filesystem::path& /*executable*/,
                         const std::vector<std::string>& /*arguments*/,
                         const std::filesystem::path& /*workingDirectory*/)
{
    GE_PLATFORM_WEB_UNAVAILABLE("launching a process");
    return 0;
}

bool IsProcessRunning(int /*pid*/)
{
    return false;
}

bool OpenScriptWithProject(const std::filesystem::path& /*scriptPath*/,
                           const std::filesystem::path& /*projectPath*/)
{
    GE_PLATFORM_WEB_UNAVAILABLE("opening a script in an external editor");
    return false;
}

bool OpenSourceWithProject(const std::filesystem::path& /*sourcePath*/,
                           const std::filesystem::path& /*projectDir*/)
{
    GE_PLATFORM_WEB_UNAVAILABLE("opening a source file in an external IDE");
    return false;
}

bool SupportsFolderPicker()
{
    // Chrome/Edge ship showDirectoryPicker; Firefox and Safari do not, and
    // Brave hides it behind brave://flags/#file-system-access-api.
    return EM_ASM_INT({ return typeof globalThis.showDirectoryPicker === 'function' ? 1 : 0; }) != 0;
}

std::filesystem::path SelectFolder(const std::filesystem::path& /*initialPath*/)
{
    // initialPath is meaningless here: the browser chooses where its picker
    // opens, and a page is not allowed to point it at a path.
    char* name = GeWebPickDirectoryIntoOpfs();
    if (name == nullptr)
    {
        // Cancel and "no such API" are indistinguishable at this seam, so the
        // message has to cover the one the user can act on.
        GE_PLATFORM_WEB_UNAVAILABLE(
            "the folder picker (it needs a Chromium-based browser: Firefox and Safari implement "
            "no showDirectoryPicker, so a project can only be opened from browser storage there)");
        return {};
    }
    std::filesystem::path imported = std::filesystem::path(Web::kProjectsMount) / name;
    std::free(name);
    LOG_INFO("Platform: imported the picked folder into persistent storage at '{}'.",
             imported.string());
    return imported;
}

std::filesystem::path SelectFile(const std::filesystem::path& /*initialPath*/,
                                 const char* /*filterName*/,
                                 const char* /*filterPattern*/)
{
    GE_PLATFORM_WEB_UNAVAILABLE("the native file picker");
    return {};
}

std::vector<std::filesystem::path> SelectFiles(const std::filesystem::path& /*initialPath*/,
                                               const char* /*filterName*/,
                                               const char* /*filterPattern*/)
{
    // initialPath and the filter are meaningless here: the browser chooses
    // where its picker opens and what it lists.
    char* root = GeWebPickFilesIntoMemfs();
    if (root == nullptr)
        return {};
    const std::filesystem::path stagingDir = root;
    std::free(root);

    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(stagingDir, ec);
         !ec && it != std::filesystem::directory_iterator(); ++it)
    {
        if (it->is_regular_file(ec))
            files.push_back(it->path());
    }
    return files;
}

void ReleaseTransientFiles(const std::vector<std::filesystem::path>& paths)
{
    for (const auto& path : paths)
    {
        if (!IsTransientStagingPath(path))
            continue;
        // A pick stages every file under one dated directory; remove the
        // directory so the whole pick goes, not one file of it.
        const std::filesystem::path victim =
            path.generic_string().rfind("/.web_picked_files/", 0) == 0 ? path.parent_path() : path;
        std::error_code ec;
        std::filesystem::remove_all(victim, ec);
    }
}

std::filesystem::path SaveFile(const std::filesystem::path& /*initialPath*/,
                               const char* /*filterName*/,
                               const char* /*filterPattern*/)
{
    GE_PLATFORM_WEB_UNAVAILABLE("the native save dialog");
    return {};
}

int ShowNativeChoiceDialog(const std::string& title, const std::string& message,
                           const std::vector<std::string>& /*buttons*/, int defaultButton)
{
    // The terminal device-loss surface: the engine's own UI cannot draw, so
    // this must still reach the user. window.alert is the only synchronous
    // browser dialog, and it carries no custom buttons — the caller's default
    // is what gets chosen.
    const std::string text = title + "\n\n" + message;
    EM_ASM({ window.alert(UTF8ToString($0)); }, text.c_str());
    return defaultButton;
}

std::filesystem::path GetExecutablePath()
{
    // There is no executable file. Path resolution anchors on the MEMFS root
    // the dist unpacked into, which the caller already knows.
    return {};
}

} // namespace Platform
} // namespace GameEngine
