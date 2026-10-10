/**
 * Resolved connection settings for the editor debug server.
 *
 * Environment supplies the defaults; the CLI may override them per invocation
 * (`--port`, `--host`, `--timeout`). Nothing may capture these at module-load
 * time — an override applied after import must still be visible, so read them
 * through `ipcConfig()` at call time rather than destructuring into a const.
 */
export interface IpcConfig {
  host: string;
  port: number;
  timeoutMs: number;
  /**
   * The AI Assistant conversation this server serves (GE_ASSISTANT_TOKEN), or null
   * for an ordinary server. With a token, every connection binds with
   * `assistant_bind` before any other request, and a failed bind refuses every call.
   */
  assistantToken: string | null;
}

export const kDefaultIpcPort = 9999;

function readPortEnv(): number {
  const raw = process.env.GE_EDITOR_DEBUG_PORT ?? process.env.GE_IPC_PORT;
  if (!raw) return kDefaultIpcPort;

  const parsed = Number(raw);
  return Number.isInteger(parsed) && parsed > 0 && parsed <= 65535 ? parsed : kDefaultIpcPort;
}

function readTimeoutEnv(): number {
  // GE_MCP_IPC_TIMEOUT_MS wins; IPC_TIMEOUT_MS is the older knob, so one export
  // covers every client in the tree.
  const parsed = Number(process.env.GE_MCP_IPC_TIMEOUT_MS ?? process.env.IPC_TIMEOUT_MS);
  // 30s default: deferred GPU readbacks (screenshots/captures) legitimately
  // take multiple frames on heavy scenes, and 10s proved too tight there.
  return Number.isFinite(parsed) && parsed > 0 ? parsed : 30_000;
}

const config: IpcConfig = {
  host: process.env.GE_EDITOR_DEBUG_HOST ?? "127.0.0.1",
  port: readPortEnv(),
  timeoutMs: readTimeoutEnv(),
  assistantToken: process.env.GE_ASSISTANT_TOKEN || null,
};

export function ipcConfig(): Readonly<IpcConfig> {
  return config;
}

/**
 * Apply explicit overrides (CLI flags). Must run before the first request:
 * the client reads the config on connect, and an established socket keeps the
 * host/port it was opened with.
 */
export function overrideIpcConfig(overrides: Partial<IpcConfig>): void {
  if (overrides.host !== undefined) config.host = overrides.host;
  if (overrides.port !== undefined) config.port = overrides.port;
  if (overrides.timeoutMs !== undefined) config.timeoutMs = overrides.timeoutMs;
}
