import * as fs from "fs";
import * as path from "path";
import { ipcConfig } from "../config.js";
import { formatEndpoint } from "../ipc-client.js";
import { ToolError, type ToolErrorCode, type ToolOutput } from "../registry.js";

// Rendering and exit codes for the CLI. Everything an operator or a script
// reads comes through here, so the messages that name the port and the
// modal-stall hint live here rather than at each call site.

export const kExitOk = 0;
export const kExitToolFailed = 1;
export const kExitUsage = 2;
export const kExitUnreachable = 3;
export const kExitTimeout = 4;

export function exitCodeFor(code: ToolErrorCode): number {
  switch (code) {
    case "BAD_ARGUMENT": return kExitUsage;
    case "EDITOR_UNREACHABLE": return kExitUnreachable;
    case "EDITOR_TIMEOUT": return kExitTimeout;
    default: return kExitToolFailed;
  }
}

export interface RenderOptions {
  json: boolean;
  quiet: boolean;
}

let captureSeq = 0;

/**
 * Write a capture's PNG to disk and report the path, rather than printing
 * megabytes of base64. The editor already saves the file it names in
 * `filePath`; writing it again costs nothing and covers responses that do not
 * name one.
 */
export function savePng(base64: string, filePath?: string): string {
  const target = filePath ?? path.resolve(`capture_${Date.now()}_${++captureSeq}.png`);
  fs.mkdirSync(path.dirname(target), { recursive: true });
  fs.writeFileSync(target, Buffer.from(base64, "base64"));
  return target;
}

/**
 * Strip an embedded PNG out of a raw debug-server result, writing it to disk
 * and replacing the field with `savedTo`. Applied to `raw` and `batch` as well
 * as to registry tools, so no path can flood stdout.
 */
export function stripPngFromResult(result: any): any {
  if (!result || typeof result !== "object" || !result.pngBase64) return result;
  const savedTo = savePng(result.pngBase64, result.filePath);
  const { pngBase64, ...rest } = result;
  return { ...rest, savedTo };
}

/**
 * `editor` is the `host:port` that answered, or null when the tool reached no
 * editor. Reported on success, not only on failure: a call answered by another
 * session's editor succeeds and reads as correct, and the endpoint is the only
 * part of the response that separates the two. Plain mode puts it on stderr so
 * a scripted caller's stdout is byte-for-byte what it was.
 */
export function renderOutput(tool: string, out: ToolOutput, opts: RenderOptions, editor: string | null = null): void {
  let savedTo: string | undefined;
  if (out.image) savedTo = savePng(out.image.base64, out.image.filePath);

  if (opts.json) {
    console.log(JSON.stringify({ ok: true, tool, data: out.data, summary: out.summary, savedTo, editor }));
    return;
  }

  if (out.summary) console.log(out.summary);
  else console.log(JSON.stringify(out.data, null, 2));

  if (savedTo && !out.summary?.includes(savedTo) && !opts.quiet) console.error(`Saved ${savedTo}`);
  if (editor && !opts.quiet) console.error(`[editor ${editor}]`);
}

/**
 * `editor` follows the same rule as on success: the endpoint that answered, or
 * null when nothing did. A refusal IS an answer, and "which editor refused" is
 * exactly the question a failing call raises — while an unreachable one must
 * still name nobody, or the message would invent an editor that never saw the
 * request.
 */
export function renderError(tool: string, err: unknown, opts: RenderOptions, editor: string | null = null): number {
  const code: ToolErrorCode = err instanceof ToolError ? err.code : "TOOL_FAILED";
  const message = (err as any)?.message ?? String(err);

  if (opts.json) {
    console.log(JSON.stringify({ ok: false, tool, code, error: message, editor }));
    return exitCodeFor(code);
  }

  console.error(message);
  if (editor) console.error(`[editor ${editor}]`);
  if (code === "EDITOR_UNREACHABLE") {
    const cfg = ipcConfig();
    console.error(`Is the editor running with its debug server on ${formatEndpoint(cfg.host, cfg.port)}?`);
    console.error("Set GE_EDITOR_DEBUG_PORT or pass --port for non-default editors.");
  }
  return exitCodeFor(code);
}
