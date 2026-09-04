// panicast CLI service subcommands (N09/S1, unit renamed in N10): status / start /
//   stop / restart / enable / disable / log [-f]. Dispatched from main() before the TUI
//   path. The daemon is `panicast -d` (systemd unit panicast.service, ExecStart=panicast
//   -d); start/stop/restart go through systemctl directly (the S1-3 polkit rule allows
//   the owning user passwordless), while enable/disable deliberately require the user's
//   sudo (explicit opt-in to autostart).
#pragma once

namespace panicast
{

// N10.2: stop the daemon for the TUI session (systemctl via the S1-3 polkit rule;
//   SIGTERM by pidfile when a manually started `panicast -d` survives systemctl —
//   e.g. unit not installed). The daemon's clean exit persists player state, which
//   the TUI then restores. Returns true when a daemon was stopped.
bool service_handover_takeover();

// Restart the daemon after the TUI exits (N10.1 semantics: the service is running
//   before AND after any TUI session).
void service_handover_restore();

// Returns 0 when the command ran (and a process exit code is appropriate), -1 when
//   argv does not name a service subcommand (caller continues normal startup).
int run_cli_command(int argc, char *argv[]);

} // namespace panicast
