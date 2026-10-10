#!/usr/bin/env python3
"""Defer the pthread mailbox pump while ASYNCIFY is mid-unwind/rewind.

Classic asyncify takes the wasm stack apart (unwind) and reassembles it
(rewind) around every suspension point. Emscripten's checkMailbox can fire in
that window — the field stacks read `_emscripten_check_mailbox <-
callUserCallback <- doRewind` — and executing wasm on a half-restored stack
traps with `memory access out of bounds`. Headed browsers align the timing
(vsync cadence, input-driven mailbox traffic); headless practically never
does, which is why the trap lived exclusively on real desktops.

JSPI (ASYNCIFY=2) would remove the window entirely, but it cannot suspend
inside WasmFS's OPFS proxying on the pinned emscripten (SuspendError at
OPFSBackend::createDirectory). Until the pin moves, this post-link patch
makes checkMailbox re-schedule itself when Asyncify is not idle.

Idempotent; fails loudly if the expected minified text is missing so an
emsdk bump cannot silently ship an unpatched runtime.
"""
import shutil
import subprocess
import sys
from pathlib import Path

# Entering wasm is safe only when this thread has NO asyncify continuation in
# flight and no wasm frame already on the stack:
#
#   state !== 0             an unwind or rewind is actively running;
#   currData !== null       a continuation is SUSPENDED, waiting for its wakeup.
#                           maybeStopUnwind() sets state back to Normal as soon
#                           as the last frame pops, so for the whole suspended
#                           window state reads 0 while the continuation is very
#                           much alive. Classic asyncify holds exactly one
#                           continuation per thread, so a pump that enters wasm
#                           here and suspends overwrites currData — the original
#                           wakeup then rewinds against foreign data and faults
#                           with "memory access out of bounds", and the clobbered
#                           continuation (typically the frame loop) never resumes.
#   exportCallStack.length  wasm is running right now. setDataRewindFunc records
#                           exportCallStack[0], so a suspension raised from a
#                           re-entrant pump would resume the OUTER export.
#
# The pump also always re-dispatches through a fresh macrotask, so it can never
# run synchronously inside doRewind's completion chain.
kBusy = ('(typeof Asyncify!="undefined"&&(Asyncify.state!==0||Asyncify.currData'
         '||Asyncify.exportCallStack.length))')

# Deferring is the only safe response to a busy thread, but a continuation that
# never gets its wakeup would defer the pump forever — silently, which is the
# failure class this patch exists to end. Say so instead. Declared (not
# assigned) so hoisting makes it callable from both entries regardless of the
# order emscripten emits them in.
kNoteDefer = ('function __geNoteDefer(){var n=Date.now();'
              'if(!__geDeferSince){__geDeferSince=n;return}'
              'if(n-__geDeferSince>5000){__geDeferSince=n;'
              'console.error("[emscripten] mailbox pump deferred >5s: an asyncify "'
              '+"continuation is outstanding and never woke (state="+Asyncify.state'
              '+" currData="+Asyncify.currData+" depth="+Asyncify.exportCallStack.length'
              '+"). This thread cannot make progress.")}}'
              'var __geDeferSince=0;')

# Anchored on the whole declaration, not the bare assignment: the injected
# helper is a statement, and splicing a statement in after `var` does not parse.
kAnchor = "var checkMailbox=()=>{"
kGuard = (kNoteDefer +
          'var checkMailbox=()=>setTimeout(__geRealCheckMailbox,0);'
          'var __geRealCheckMailbox=()=>{'
          f'if{kBusy}'
          '{__geNoteDefer();setTimeout(__geRealCheckMailbox,0);return}'
          '__geDeferSince=0;')


# The second pump entry holds the pthread identity captured by checkMailbox.
# Revalidate it after deferral: the pthread may have exited and its browser
# worker may already host another pthread. Registering a wait on the old
# pointer would touch released thread storage and can strand the new worker.
kAwaitAnchor = ("callUserCallback(()=>{__emscripten_thread_mailbox_await(pthread_ptr);"
                "__emscripten_check_mailbox()})")
kAwaitGuard = ("callUserCallback(()=>{var __gePump=()=>{"
               f'if{kBusy}'
               "{__geNoteDefer();setTimeout(__gePump,0);return}"
               "if(pthread_ptr!==_pthread_self())return;"
               "__geDeferSince=0;"
               "__emscripten_thread_mailbox_await(pthread_ptr);"
               "__emscripten_check_mailbox()};setTimeout(__gePump,0)})")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: patch_check_mailbox.py <WebEditor.js>", file=sys.stderr)
        return 2
    path = Path(sys.argv[1])
    text = path.read_text()
    if "__gePump" in text:
        print("patch_check_mailbox: already patched")
        return 0
    for anchor, name in ((kAnchor, "checkMailbox"), (kAwaitAnchor, "mailbox-await handler")):
        if text.count(anchor) != 1:
            print(f"patch_check_mailbox: expected exactly one '{name}' anchor in {path}; "
                  f"found {text.count(anchor)} — emscripten output changed, update the patch",
                  file=sys.stderr)
            return 1
    original = text
    text = text.replace(kAnchor, kGuard, 1)
    text = text.replace(kAwaitAnchor, kAwaitGuard, 1)
    path.write_text(text)

    # Splicing statements into minified output can produce a file that does not
    # parse, and the runtime's only symptom is a blank page with a SyntaxError —
    # indistinguishable at a glance from the wedge this patch fixes. Reject it
    # here, with the original restored, so the build fails instead of the page.
    if shutil.which("node"):
        check = subprocess.run(["node", "--check", str(path)],
                               capture_output=True, text=True)
        if check.returncode != 0:
            path.write_text(original)
            print(f"patch_check_mailbox: patched {path.name} does not parse — reverted.\n"
                  f"{check.stderr.strip()[:600]}", file=sys.stderr)
            return 1
    else:
        print("patch_check_mailbox: node not found — patched output not syntax-checked",
              file=sys.stderr)
    print("patch_check_mailbox: applied asyncify guards to both mailbox pump entries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
