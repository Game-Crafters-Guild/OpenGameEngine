import { EventEmitter } from "events";
import * as net from "net";
import { ipcConfig } from "./config.js";

interface IpcRequest {
  id: string;
  method: string;
  params: Record<string, unknown>;
}

interface IpcResponse {
  id: string;
  ok: boolean;
  result?: unknown;
  error?: string;
  /** Diagnostic fields a handler refused with (candidates, blocking state), ok:false only. */
  details?: Record<string, unknown>;
}

/** The message an ok:false response rejects with: the reason, then any details it carried. */
function refusalMessage(resp: IpcResponse): string {
  const reason = resp.error ?? "Unknown error";
  return resp.details ? `${reason}\n${JSON.stringify(resp.details)}` : reason;
}

/**
 * The editor did not bind this server's connection to its assistant session (an
 * unknown token, another editor on the port, a closed conversation, a dropped
 * connection). Sticky: once thrown, the client sends nothing more to the port.
 */
export class AssistantBindRefused extends Error {
  constructor(reason: string) {
    super(`This editor did not accept the assistant's session (${reason}): ` +
      `start a new turn from the AI Assistant panel`);
    this.name = "AssistantBindRefused";
  }
}

/**
 * `host:port` for display. An IPv6 host is bracketed, because `::1:9987` is
 * unparseable as an endpoint and reads as an address ending in 9987.
 */
export function formatEndpoint(host: string, port: number): string {
  return host.includes(":") ? `[${host}]:${port}` : `${host}:${port}`;
}

export class IpcClient extends EventEmitter {
  private socket: net.Socket | null = null;
  private connected = false;
  private connecting = false;
  private buffer = "";
  private pending = new Map<string, { resolve: (r: IpcResponse) => void; reject: (e: Error) => void }>();
  private nextId = 0;
  private answered = 0;
  private lastAnswerEndpoint: string | null = null;
  private bindRefusal: AssistantBindRefused | null = null;

  async connect(): Promise<void> {
    if (this.bindRefusal) throw this.bindRefusal;
    if (this.connected) return;
    if (this.connecting) {
      // Wait for the in-flight connect to finish.
      return new Promise((resolve, reject) => {
        const onConnected = () => { cleanup(); resolve(); };
        const onError = (err: Error) => { cleanup(); reject(err); };
        const cleanup = () => {
          this.removeListener("connected", onConnected);
          this.removeListener("connectFailed", onError);
        };
        this.once("connected", onConnected);
        this.once("connectFailed", onError);
      });
    }

    this.connecting = true;
    return new Promise((resolve, reject) => {
      // Destroy any leftover socket before creating a new one.
      if (this.socket) {
        this.socket.removeAllListeners();
        this.socket.destroy();
        this.socket = null;
      }

      this.socket = new net.Socket();
      this.socket.setNoDelay(true);

      this.socket.on("connect", () => {
        this.buffer = "";
        const token = ipcConfig().assistantToken;
        if (!token) {
          this.markConnected();
          resolve();
          return;
        }
        // Bound before anything else is sent on this connection, reconnects included;
        // requests issued meanwhile wait in connect() for the outcome.
        this.bind(token).then(
          () => { this.markConnected(); resolve(); },
          (err: Error) => {
            this.bindRefusal = new AssistantBindRefused(err.message);
            this.connecting = false;
            this.emit("connectFailed", this.bindRefusal);
            if (this.socket) { this.socket.removeAllListeners(); this.socket.destroy(); this.socket = null; }
            reject(this.bindRefusal);
          });
      });

      this.socket.on("data", (data) => this.onData(data));

      this.socket.on("close", () => {
        this.connected = false;
        this.connecting = false;
        this.rejectAllPending("Connection closed");
        this.emit("disconnected");
        // No auto-reconnect — request() connects on demand.
      });

      this.socket.on("error", (err) => {
        if (!this.connected) {
          this.connecting = false;
          this.emit("connectFailed", err);
          reject(err);
        }
      });

      // Read the config here, not at construction: a CLI --port/--host override
      // is applied after this module is imported.
      const { host, port } = ipcConfig();
      this.socket.connect(port, host);
    });
  }

  /**
   * Try to connect if not connected, then send the request.
   * `timeoutMs` overrides the configured default — control-plane probes
   * (readiness checks, shutdown) should pass a short value so they fail fast
   * instead of inheriting the 30s budget sized for deferred GPU readbacks.
   */
  async request(method: string, params: Record<string, unknown> = {}, timeoutMs?: number): Promise<unknown> {
    if (this.bindRefusal) throw this.bindRefusal;
    if (!this.connected) {
      await this.connect();
    }

    if (!this.connected || !this.socket) {
      throw new Error("Not connected to editor");
    }

    const effectiveTimeout = timeoutMs ?? ipcConfig().timeoutMs;
    const id = String(++this.nextId);
    const msg: IpcRequest = { id, method, params };

    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        // The modal hint is load-bearing: a native popup's nested event loop
        // stalls the debug server's main-thread dispatch, and the symptom is
        // indistinguishable from a hang. Context menus are drivable
        // programmatically via open_context_menu.
        reject(new Error(
          `Request '${method}' timed out after ${effectiveTimeout}ms — the editor is running but did not respond ` +
          `(busy loading/compiling, hung, or stalled in a native popup/modal nested loop; ` +
          `context menus are drivable via open_context_menu instead)`));
      }, effectiveTimeout);

      this.pending.set(id, {
        resolve: (r) => { clearTimeout(timer); resolve(r.result); },
        reject: (e) => { clearTimeout(timer); reject(e); },
      });

      this.socket!.write(JSON.stringify(msg) + "\n");
    });
  }

  get isConnected(): boolean { return this.connected; }

  /**
   * `host:port` of the editor currently on the other end, or null when there is
   * no live socket.
   *
   * Read from the socket, not from the config: an established connection keeps
   * the endpoint it was opened with, so after an override the config names a
   * port this client is not talking to. Callers reporting "which editor
   * answered" need the socket's answer or they report a guess.
   */
  get remoteEndpoint(): string | null {
    const s = this.socket;
    if (!this.connected || !s || s.remoteAddress === undefined || s.remotePort === undefined) return null;
    return formatEndpoint(s.remoteAddress, s.remotePort);
  }

  /**
   * How many requests this client has had answered, and by which editor.
   * "Answered" means a response line arrived — a refusal counts.
   *
   * Recorded in this class because it is the only path to an editor that any
   * registry tool takes: the control-plane helpers (readiness polling, port
   * freeing, shutdown) drive this client directly to get their own timeouts, so
   * attribution added above them would miss exactly the tools whose job is to
   * say which editor you just attached to. `ge batch` is the deliberate
   * exception — it opens its own socket and reports its own endpoint. The
   * endpoint is captured when the answer lands, so it survives the disconnect
   * those helpers perform.
   */
  get answeredCount(): number { return this.answered; }
  get lastAnsweredEndpoint(): string | null { return this.lastAnswerEndpoint; }

  disconnect(): void {
    this.rejectAllPending("Disconnecting");
    if (this.socket) { this.socket.removeAllListeners(); this.socket.destroy(); this.socket = null; }
    this.connected = false;
    this.connecting = false;
  }

  // --- private ---

  private markConnected(): void {
    this.connected = true;
    this.connecting = false;
    this.emit("connected");
  }

  /** Sends `assistant_bind` on the fresh socket; rejects with the editor's reason. */
  private bind(token: string): Promise<void> {
    const id = String(++this.nextId);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error("the editor did not answer the binding"));
      }, ipcConfig().timeoutMs);
      this.pending.set(id, {
        resolve: () => { clearTimeout(timer); resolve(); },
        reject: (e) => { clearTimeout(timer); reject(e); },
      });
      const msg: IpcRequest = { id, method: "assistant_bind", params: { token } };
      this.socket!.write(JSON.stringify(msg) + "\n");
    });
  }

  private onData(data: Buffer): void {
    this.buffer += data.toString();
    const lines = this.buffer.split("\n");
    this.buffer = lines.pop() ?? "";
    for (const line of lines) {
      if (!line.trim()) continue;
      try {
        const resp: IpcResponse = JSON.parse(line);
        const p = this.pending.get(resp.id);
        if (p) {
          this.pending.delete(resp.id);
          // Attributed here rather than in the resolve closure: an ok:false
          // response is still an editor answering, and refusals are exactly
          // when a caller wants to know which editor refused. Sits outside
          // rejectAllPending, so a dropped connection still claims nobody.
          this.answered++;
          this.lastAnswerEndpoint = this.remoteEndpoint;
          if (resp.ok) p.resolve(resp);
          else p.reject(new Error(refusalMessage(resp)));
        }
      } catch { /* ignore malformed */ }
    }
  }

  private rejectAllPending(reason: string): void {
    for (const [, p] of this.pending) p.reject(new Error(reason));
    this.pending.clear();
  }
}
