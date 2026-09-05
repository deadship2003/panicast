// panicast --daemon mode (N10): one binary, two frontends. `panicast -d` runs the SAME
//   engine as the TUI with a NullFrontend instead of ncurses — playback, queue,
//   subscriptions, downloads, transcription and the LMS/PRP remote-control servers all
//   run without a terminal; Squeezer (or the web remote) is the UI. Managed by systemd
//   (panicast.service, ExecStart=panicast -d) or run directly in the foreground for
//   debugging. Replaces the former separate panicastd binary (N09/S1).
//
// Shared helpers: the pidfile path + liveness probe are also used by the service
//   subcommands (`panicast status`) and the TUI session handover, so there is exactly
//   one definition of "is the daemon running" in the codebase.
#pragma once

#include <string>

namespace panicast
{

// <data_dir>/panicast-daemon.pid (N10 rename; run_daemon() removes a stale N09
//   panicastd.pid left behind by an older install).
std::string daemon_pidfile_path();

// True when the pidfile names a live process. Stale files (dead pid) read as false.
bool daemon_pid_alive(int *out_pid = nullptr);

// ── TUI session ownership (single-instance, N10.2) ─────────────────────────────
//   The engine (mpv + queue + DB) has exactly ONE owner at a time. The TUI writes
//   its own pidfile so the daemon path (and a second TUI) can refuse instead of
//   racing it — mirroring what daemon_pid_alive() does for the daemon side.
std::string tui_pidfile_path();

// True when a TUI session owns the engine. Stale files (dead pid) read as false.
bool tui_pid_alive(int *out_pid = nullptr);

// Write/remove the TUI pidfile. write creates the data dir if needed.
void write_tui_pidfile();
void remove_tui_pidfile();

// True when a live process holds the mini-LMS listen port ([remote] lms_port) while
//   no daemon pidfile names a live daemon — i.e. an orphan/older-binary panicast
//   session owns the engine without a pidfile. Starting beside it would produce a
//   zombie daemon (LMS bind fails, everything else runs).
bool lms_port_in_use();

// Foreground headless mode behind `panicast -d / --daemon`. Returns the process exit
//   code. Refuses to start when another daemon instance — or a TUI session — is
//   already alive (double-run would fight over :9090, the mpv instance and the
//   SQLite writes).
int run_daemon();

} // namespace panicast
