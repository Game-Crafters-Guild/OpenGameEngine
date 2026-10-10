import { IpcClient } from "./ipc-client.js";

// One connection per process, shared by every tool. Built lazily so importing a
// tool module never opens a socket, and so a CLI --port override applied during
// argument parsing is in effect by the time the first request goes out.
let client: IpcClient | null = null;

export function ipc(): IpcClient {
  if (!client) client = new IpcClient();
  return client;
}
