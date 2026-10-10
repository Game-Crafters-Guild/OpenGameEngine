import { ipcConfig } from "./config.js";
import { AssistantBindRefused } from "./ipc-client.js";
import { ipc } from "./ipc-singleton.js";
import { ToolError } from "./registry.js";

// Which editor answered, and how many have. Several editors run on one machine
// — a lane's own, the developer's on the default port — and a call answered by
// the wrong one looks exactly like a call answered by the right one unless the
// response says whose answer it is. A front-end compares the count across a
// tool invocation: unchanged means the tool reached no editor (build, launch,
// codegen) and may claim none.
//
// The numbers come from the transport, which is the path every registry tool
// takes — `ge batch` is the exception and reports its own endpoint. Process-wide
// rather than per-call: one IpcClient serves the process, so there is only ever
// one endpoint and the reported value cannot name the wrong editor. Concurrent
// calls can over-attribute the count — a build tool finishing while an editor
// call is in flight may be credited with reaching one — which puts a correct
// endpoint on a response that did not use it.

/**
 * How many editor round-trips have been answered in this process. A refusal
 * counts: the editor answered, it just said no.
 */
export function editorCallCount(): number {
  return ipc().answeredCount;
}

/** `host:port` of the editor that answered the most recent call, if any. */
export function lastEditorEndpoint(): string | null {
  return ipc().lastAnsweredEndpoint;
}

/**
 * Proxy a call to the C++ editor debug server, connecting on demand.
 *
 * Transport failures are classified here so both front-ends can act on the
 * distinction: "no editor" is a setup problem the caller can fix, while a
 * timeout means the editor is alive but not answering.
 */
export async function editorCall(method: string, params: Record<string, unknown> = {}): Promise<unknown> {
  try {
    return await ipc().request(method, params);
  } catch (err: any) {
    if (err instanceof AssistantBindRefused) throw new ToolError(err.message);
    const msg = err?.message ?? String(err);
    if (msg.includes("ECONNREFUSED") || msg.includes("ETIMEDOUT") || msg.includes("EHOSTUNREACH") || msg.includes("ENETUNREACH") || msg.includes("Not connected") || msg.includes("Connection closed")) {
      throw new ToolError(
        `Editor is not running or not responding on port ${ipcConfig().port}. Launch it with launch_editor first. (${msg})`,
        "EDITOR_UNREACHABLE");
    }
    if (msg.includes("timed out after")) throw new ToolError(msg, "EDITOR_TIMEOUT");
    throw err;
  }
}
