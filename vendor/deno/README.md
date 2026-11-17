# vendor/deno — bundled JavaScript runtime (Y03, FALLBACK)

> **Y03-light prefers quickjs-ng (`vendor/quickjs/qjs`, ~2 MB) over this 106 MB deno binary.**
> See `../quickjs/README.md`. This directory is kept as a fallback for environments where
> quickjs's EJS solver can't be installed (quickjs can't fetch EJS from npm; deno can).

yt-dlp 2026.07+ requires a JavaScript runtime to solve YouTube's nsig "n challenge".
Without it, YouTube playback fails with `Requested format is not available` / `n challenge
solving failed` — only storyboard images are returned, no audio/video.

**yt-dlp defaults to `deno` only.** Node 20 (Debian/Ubuntu `apt install nodejs`) is detected
but marked `unsupported` by yt-dlp's bundled EJS solver and does **not** work. Deno is the
simplest working runtime and is auto-used by yt-dlp when `deno` is in `PATH` (no extra flags).

This directory ships a prebuilt `deno` so Y03 is self-contained:

- `deno` — Deno 2.9.3, official linux-x86_64 build (statically-linked-ish single binary).

`../build.sh install` installs it into `/usr/local/bin/deno` (system-wide, needs sudo).
Manual alternative: `sudo cp deno /usr/local/bin/deno`.

> Only the x86_64 linux build is bundled. On ARM64 / macOS / Windows, run
> `curl -fsSL https://deno.land/install.sh | sh` instead (see README "依赖" section).
