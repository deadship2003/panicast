// N10.6 client-mode TUI — when the background service is running, `panicast` does
//   NOT take over the engine: it opens a local CONTROLLER over the daemon's mini-LMS
//   plane (same protocol Squeeze Client speaks, on loopback). The service process is
//   never touched — no takeover, no pidfile, no fd handover, nothing to restore; the
//   phone keeps its connections for the whole session by construction.
//
//   Bounded surface on purpose: browse list + cursor navigation + row activation,
//   mode switching, transport, volume/mute, seek, queue view. Keys follow the TUI's
//   conventions; `q` quits (the service just keeps running). When no daemon is
//   running, main() boots the standalone engine TUI as before.
#pragma once

namespace panicast
{

// Returns the process exit code. Fails fast with a readable message when the
//   daemon is not reachable (caller falls back / reports).
int run_client_tui();

} // namespace panicast
