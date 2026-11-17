// panicast service lifecycle CLI (LIF-001). Unified entry:
//   panicast service <subcmd> [--json] [--user|--system]
// Subcommands: install / uninstall / start / stop / restart / enable / disable /
//   status. The bare forms (`panicast start`, `panicast status`, …) remain as
//   aliases and dispatch identically.
//
// The daemon is a USER-space systemd unit (~/.config/systemd/user/panicast.service,
// ExecStart=<this binary> --daemon) — every verb goes through `systemctl --user`,
// so NO sudo/polkit is involved. `--system` is accepted for interface uniformity
// but refused with exit code 4 (user-scope-only by design, N10.3).
//
// Exit codes (LIF-001): 0 success / 1 invalid arguments / 2 insufficient
// privileges / 3 service operation failed / 4 unsupported scope.
//
// Status truth source = the init system (`systemctl --user show`); the pidfile is
// only a bare-run clue — when both exist and disagree, status flags the anomaly.
// Log viewing is NOT a subcommand (LIF-001 rev3): use
//   tail -F ~/.local/share/panicast/panicast-$(date +%Y%m%d).log
#pragma once

namespace panicast
{

// Install (or refresh) the USER-space systemd unit. Idempotent: identical content
//   is a no-op; changed content rewrites the file + daemon-reload. Never starts
//   or enables anything (LIF-001: registration layer only).
void ensure_user_unit();

// N10.2: stop the daemon for the TUI session. systemd truth (`systemctl --user
//   stop`) with the pidfile SIGTERM fallback for a BARE-run daemon (pidfile = the
//   bare-run clue). Returns true when a daemon was stopped.
bool service_handover_takeover();

// Restart the daemon after the TUI exits (N10.1 semantics: the service is running
//   before AND after any TUI session).
void service_handover_restore();

// Returns a process exit code when the command ran, -1 when argv does not name a
// service subcommand (caller continues normal startup).
int run_cli_command(int argc, char *argv[]);

} // namespace panicast
