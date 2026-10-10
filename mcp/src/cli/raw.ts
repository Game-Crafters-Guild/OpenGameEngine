import * as net from "net";
import * as readline from "readline";
import { ipcConfig } from "../config.js";
import { editorCall, editorCallCount, lastEditorEndpoint } from "../editor-call.js";
import { formatEndpoint } from "../ipc-client.js";
import { ToolError } from "../registry.js";
import {
  kExitOk, kExitTimeout, kExitUnreachable, kExitUsage,
  renderError, renderOutput, stripPngFromResult, type RenderOptions,
} from "./output.js";

/**
 * Escape hatch for the debug-server methods that have no registry tool. The
 * method name and parameters are forwarded unvalidated, which is deliberate:
 * `raw` must keep working for a handler added to the C++ side today, before
 * anyone writes a schema for it.
 */
export async function runRaw(argv: string[], opts: RenderOptions & { dryRun?: boolean }): Promise<number> {
  const method = argv[0];
  if (!method) {
    console.error("Usage: ge raw <method> ['{\"key\":\"value\"}']");
    console.error("Lists every method and its coverage: node Tests/Mcp/ipc-method-drift.mjs --list");
    return kExitUsage;
  }
  if (argv.length > 2) {
    console.error("Too many arguments: parameters must be a single quoted JSON object.");
    console.error(`Got: ${argv.slice(1).map((a) => `"${a}"`).join(" ")}`);
    return kExitUsage;
  }

  let params: Record<string, unknown> = {};
  if (argv[1] !== undefined) {
    try {
      const parsed = JSON.parse(argv[1]);
      if (parsed === null || typeof parsed !== "object" || Array.isArray(parsed)) {
        console.error(`Parameters must be a JSON object, got: ${argv[1]}`);
        return kExitUsage;
      }
      params = parsed;
    } catch (e: any) {
      console.error(`Parameters are not valid JSON: ${e.message}`);
      console.error(`Got: ${argv[1]}`);
      return kExitUsage;
    }
  }

  // --dry-run is a global flag; raw honors it like the tool path does. There is
  // no schema here, so the resolved parameters are the parsed JSON verbatim.
  if (opts.dryRun) {
    console.log(JSON.stringify({ method, params }, null, 2));
    return kExitOk;
  }

  const callsBefore = editorCallCount();
  const answeredEndpoint = () => (editorCallCount() > callsBefore ? lastEditorEndpoint() : null);
  try {
    const result = await editorCall(method, params);
    renderOutput(method, { data: stripPngFromResult(result) }, opts, answeredEndpoint());
    return kExitOk;
  } catch (err) {
    // A method the editor rejects is the commonest failure here — a wrong name
    // or bad params — and a rejection is an answer, from an editor worth naming.
    return renderError(method, err, opts, answeredEndpoint());
  }
}

/**
 * One connection, one request per stdin line, one response line per request.
 *
 * Requests are numbered "1".."N" in stdin order — pair responses by `id`, NOT
 * by line order: deferred operations (take_screenshot, capture_resource)
 * complete frames later and interleave after faster requests. Exits when stdin
 * closes and every request has been answered.
 *
 * Raw protocol only. Host-side tools (build_engine, launch_editor, …) are not
 * debug-server methods and have no place on this wire.
 *
 * This is the one path that does not go through IpcClient — it owns its socket
 * so responses can stream out unbuffered — so it reports its own endpoint. That
 * goes to stderr, once, on connect: stdout here is a wire protocol whose lines
 * consumers parse by id, and adding a field to it would break them.
 */
export function runBatch(opts: RenderOptions): Promise<number> {
  const { host, port, timeoutMs } = ipcConfig();

  return new Promise((resolve) => {
    let requestsSent = 0;
    let responsesReceived = 0;
    let stdinDone = false;
    let settled = false;
    let watchdog: NodeJS.Timeout | null = null;
    let rl: readline.Interface | null = null;

    // Let buffered stdout drain rather than truncating large piped output.
    // Closing the readline and unrefing stdin matters as much as destroying
    // the socket: an open stdin keeps the event loop alive, so without it a
    // fired watchdog prints its message and the process still never exits —
    // the driver holding stdin open then hangs right alongside it.
    const finish = (code: number) => {
      if (settled) return;
      settled = true;
      if (watchdog) { clearTimeout(watchdog); watchdog = null; }
      rl?.close();
      process.stdin.unref();
      socket.destroy();
      resolve(code);
    };

    // The watchdog covers time spent WAITING on the editor: armed while
    // requests are outstanding, disarmed once answered — a batch driver may
    // idle between requests indefinitely.
    const updateWatchdog = () => {
      if (watchdog) { clearTimeout(watchdog); watchdog = null; }
      if (responsesReceived >= requestsSent) return;
      watchdog = setTimeout(() => {
        console.error(`Timeout after ${timeoutMs}ms (${responsesReceived}/${requestsSent} responses received) from ${formatEndpoint(host, port)}.`);
        console.error("A native popup/modal nested loop stalls the debug server; context menus are drivable via open_context_menu instead.");
        finish(kExitTimeout);
      }, timeoutMs);
    };

    const socket = net.createConnection({ port, host }, () => {
      if (!opts.quiet) {
        console.error(`[editor ${socket.remoteAddress !== undefined && socket.remotePort !== undefined
          ? formatEndpoint(socket.remoteAddress, socket.remotePort)
          : formatEndpoint(host, port)}]`);
      }
      rl = readline.createInterface({ input: process.stdin, terminal: false });
      rl.on("line", (line) => {
        const trimmed = line.trim();
        if (!trimmed) return;
        let req: { method?: string; params?: Record<string, unknown> };
        try {
          req = JSON.parse(trimmed);
        } catch (e: any) {
          console.error(`Skipping malformed request line: ${e.message}`);
          return;
        }
        requestsSent++;
        socket.write(JSON.stringify({ id: String(requestsSent), method: req.method, params: req.params ?? {} }) + "\n");
        updateWatchdog();
      });
      rl.on("close", () => {
        stdinDone = true;
        if (responsesReceived >= requestsSent) finish(kExitOk);
      });
    });

    let buf = "";
    socket.on("data", (d) => {
      buf += d.toString();
      const lines = buf.split("\n");
      buf = lines.pop() ?? "";
      for (const line of lines) {
        if (!line.trim()) continue;
        // Full response including id — consumers pair by id, since deferred
        // responses arrive out of request order.
        try {
          const resp = JSON.parse(line);
          if (resp.ok && resp.result) resp.result = stripPngFromResult(resp.result);
          console.log(JSON.stringify(resp));
        } catch {
          console.log(line);
        }
        responsesReceived++;
        updateWatchdog();
        if (stdinDone && responsesReceived >= requestsSent) { finish(kExitOk); return; }
      }
    });

    socket.on("error", (e: any) => {
      if (settled) return;
      renderError("batch", new ToolError(`Connection error: ${e.code} (${host}:${port}).`, "EDITOR_UNREACHABLE"), opts);
      finish(kExitUnreachable);
    });

    // Editor closed the socket without answering (shutdown, server reset) —
    // fail fast instead of hanging until the watchdog fires.
    socket.on("close", () => {
      if (settled) return;
      if (responsesReceived < requestsSent || !stdinDone) {
        // Name the editor that dropped us: on a machine running several, "the
        // editor closed the connection" is not enough to know whose did.
        console.error(`Connection closed by ${formatEndpoint(host, port)} after ${responsesReceived}/${requestsSent} responses`);
        finish(kExitUnreachable);
      }
    });
  });
}
