# vendor/quickjs — bundled quickjs-ng JS runtime (Y03, lightweight default)

yt-dlp 2026.07+ requires a JavaScript runtime to solve YouTube's nsig "n challenge".
Without it, YouTube playback fails with `Requested format is not available` /
`n challenge solving failed`.

**quickjs-ng (`qjs`) is the recommended lightweight runtime**, replacing the 106MB `deno`
binary in earlier Y03 builds:

| runtime | binary size | cold-start | yt-dlp priority | EJS solver source |
|---------|-------------|------------|-----------------|-------------------|
| **quickjs-ng** | **~2 MB** | **~10–30 ms** | 850 (`--js-runtimes quickjs`) | `yt-dlp[default]` (yt-dlp-ejs pkg) or `--remote-components ejs:github` |
| deno | ~106–180 MB | ~100–300 ms | 1000 (yt-dlp default) | auto-fetched from npm (`ejs:npm`) |
| node ≥22 | ~120 MB | ~100 ms | 900 | same as deno |
| bun | ~95 MB | — | 800 (DEPRECATED) | — |

## Why quickjs over deno

1. **~50× smaller** (2 MB vs 106 MB) — the "light" build drops the bundled deno entirely.
2. **~10× faster cold-start** — directly removes the "deno initialization lag" the user saw
   when first playing a YouTube video (yt-dlp forks the JS runtime once per video to solve
   nsig; deno's V8 cold-start was the bottleneck).
3. yt-dlp-native support (no patching): `--js-runtimes quickjs`.

## Install / bundle

Drop the official `qjs` binary here as `vendor/quickjs/qjs`:

```bash
# linux x86_64 (match your target arch):
curl -L https://github.com/quickjs-ng/quickjs/releases/latest/download/qjs-linux-x86_64 -o vendor/quickjs/qjs
chmod +x vendor/quickjs/qjs
# verify version ≥ 0.12.0 (older builds solve nsig in *minutes*):
./vendor/quickjs/qjs --version   # (or: qjs -e 'console.log(std.getenv?)'  — version printed on -h)
```

`../setup.sh` installs it into `/usr/local/bin/qjs` (system-wide, needs sudo).

## ⚠ EJS solver dependency (the one catch)

Unlike deno (which auto-fetches the EJS solver scripts from npm), **quickjs cannot fetch
from npm**. The EJS solver must already be available on the system:

```bash
pip install -U "yt-dlp[default]"   # brings the yt-dlp-ejs package
```

or, without installing the package, tell yt-dlp to fetch EJS from GitHub by adding to the
yt-dlp args: `--remote-components ejs:github`. (Podradio's `js_runtime=quickjs` does NOT
add this automatically — install `yt-dlp[default]` instead, the simpler path.)

If you can't install yt-dlp-ejs, keep using deno: set `[youtube] js_runtime = deno` in
`podradio.ini` and install deno manually: `sudo cp vendor/deno/deno /usr/local/bin/deno`.

## Verify end-to-end

```bash
yt-dlp --js-runtimes quickjs -f best -g "<youtube video url>"   # should print a stream URL
```
