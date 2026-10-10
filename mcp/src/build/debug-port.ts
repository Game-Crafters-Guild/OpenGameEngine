import { ipcConfig } from "../config.js";
import { ipc } from "../ipc-singleton.js";
import { findPidListeningOnPort, isEditorProcess, isPortListening, isProcessAlive, killPid, pollUntil } from "./process.js";

// Free the IPC debug port: graceful IPC shutdown of the editor holding it,
// then kill that PID if it lingers. Only the port's own holder is ever
// touched — editors on OTHER ports (other worktrees, other sessions) are left
// running, and a non-Editor port holder is never killed. This is the single
// shutdown path: shutdown_editor is this, scoped to the configured port.
export async function freeDebugPort(): Promise<{ steps: string[]; blocked?: string }> {
  // Read at call time — a CLI --port override must be visible here.
  const port = ipcConfig().port;
  const steps: string[] = [];
  if (!(await isPortListening(port))) {
    steps.push(`Port ${port} is free.`);
    return { steps };
  }

  // Identify the holder BEFORE speaking any protocol to it or waiting on it.
  const pid = await findPidListeningOnPort(port);
  if (pid !== null && !(await isEditorProcess(pid))) {
    return { steps, blocked: `Port ${port} is held by PID ${pid}, which is not an Editor process — refusing to kill it. Set GE_EDITOR_DEBUG_PORT to use a different port.` };
  }

  const client = ipc();
  try {
    if (!client.isConnected) await client.connect();
    // Short timeout: a hung editor should fall through to the kill path, not
    // stall the launch for the full deferred-readback budget.
    await client.request("shutdown", {}, 3000);
    steps.push(`Sent IPC shutdown to editor on port ${port}${pid !== null ? ` (PID ${pid})` : ""}.`);
  } catch {
    steps.push(`Editor on port ${port} did not accept IPC shutdown.`);
  }
  client.disconnect();

  let released = await pollUntil(async () => !(await isPortListening(port)), 5000);

  if (!released) {
    // Re-resolve the holder — the one we sampled may have exited by now and
    // the port been re-bound; never kill a stale PID.
    const holder = await findPidListeningOnPort(port);
    if (holder === null) {
      return { steps, blocked: `Port ${port} is still busy but its owner could not be identified (netstat/lsof unavailable or unparseable). Free it manually.` };
    }
    if (!(await isEditorProcess(holder))) {
      return { steps, blocked: `Port ${port} is held by PID ${holder}, which is not an Editor process — refusing to kill it. Set GE_EDITOR_DEBUG_PORT to use a different port.` };
    }
    try {
      await killPid(holder);
      steps.push(`Force-killed unresponsive editor PID ${holder}.`);
    } catch {
      steps.push(`Failed to kill PID ${holder} (already exited?).`);
    }
    released = await pollUntil(async () => !(await isPortListening(port)), 3000);
    if (!released) {
      return { steps, blocked: `Port ${port} is still busy after killing its holder. Free it manually.` };
    }
  } else {
    steps.push("Previous editor released the port.");
  }

  // The port drops before the old process finishes teardown (it still flushes
  // logs/AssetDatabase in the shared exeDir project) — wait for actual exit so
  // the new editor doesn't race it on those files.
  if (pid !== null && isProcessAlive(pid)) {
    const exited = await pollUntil(async () => !isProcessAlive(pid), 5000);
    steps.push(exited ? `Previous editor (PID ${pid}) exited.` : `Previous editor (PID ${pid}) is still shutting down — proceeding anyway.`);
  }

  return { steps };
}
