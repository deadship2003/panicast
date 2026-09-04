// panicast CLI service subcommands (N09/S1 → N10.3): status / start / stop / restart /
//   enable / disable / log. Dispatched from main() before the TUI path. The daemon is a
//   USER-space systemd unit (~/.config/systemd/user/panicast.service, ExecStart=<this
//   binary> --daemon) — every verb goes through `systemctl --user`, so NO sudo/polkit
//   is involved anywhere. `panicast log` tails today's log and FOLLOWS it
//   (journalctl -fu semantics; -n N sets the history depth).
#pragma once

namespace panicast
{

// N10.3: install/refresh the USER-space systemd unit (~/.config/systemd/user/
//   panicast.service, ExecStart=<this binary> --daemon). Auto-called on the first TUI
//   run and by `panicast start` — user units need no sudo anywhere.
void ensure_user_unit();

// N10.2: stop the daemon for the TUI session (N10.3: `systemctl --user stop`; a
//   pre-N10.3 SYSTEM unit and a manually started daemon are stopped too — the latter
//   by pidfile SIGTERM). The daemon's clean exit persists player state, which the
//   TUI then restores. Returns true when a daemon was stopped.
bool service_handover_takeover();

// Restart the daemon after the TUI exits (N10.1 semantics: the service is running
//   before AND after any TUI session).
void service_handover_restore();

// Returns 0 when the command ran (and a process exit code is appropriate), -1 when
//   argv does not name a service subcommand (caller continues normal startup).
int run_cli_command(int argc, char *argv[]);

} // namespace panicast
