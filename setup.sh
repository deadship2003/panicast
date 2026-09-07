#!/bin/bash
# panicast — environment & dependency setup (LIF-006).
#   FIRST-TIME SETUP only; everyday builds use ./build.sh or make.
#   This script NEVER compiles: it prepares the environment (system packages,
#   JS runtime, legacy-artifact cleanup) and verifies readiness.
#
# Usage:
#   ./setup.sh             install build deps + JS runtime (idempotent, sudo where needed)
#   ./setup.sh --check     preflight: verify environment, install nothing (CI mode)
set -e
cd "$(dirname "$0")"

# Colors
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
say()  { echo -e "${GREEN}✓${NC} $1"; }
warn() { echo -e "${YELLOW}⚠${NC} $1"; }
info() { echo -e "${BLUE}…${NC} $1"; }
die()  { echo -e "${RED}✗${NC} $1" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

CHECK_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --check) CHECK_ONLY=1 ;;
        *) echo "unknown argument: $arg"; echo "usage: $0 [--check]"; exit 1 ;;
    esac
done

# ── System build deps (per package manager) ───────────────────────────────────
# ncurses note: Debian 13+ / current Ubuntu ship the unified `libncurses-dev`
# (wide + narrow); the old split names (libncurses5-dev/libncursesw5-dev) only
# exist on older releases. The list uses the unified name and pkg_installed()
# accepts any of the three as satisfying the requirement.
pkg_lists() {
    if have apt-get; then
        echo "mpv libmpv-dev libncurses-dev libcurl4-openssl-dev libsqlite3-dev libxml2-dev libfmt-dev nlohmann-json3-dev libqrencode-dev cmake ninja-build g++"
    elif have pacman; then
        echo "mpv ncurses curl libxml2 sqlite fmt nlohmann-json qrencode cmake ninja gcc"
    elif have dnf; then
        echo "mpv mpv-devel ncurses-devel libcurl-devel sqlite-devel libxml2-devel fmt-devel nlohmann-json-devel qrencode-devel cmake ninja-build gcc-c++"
    fi
}

pkg_installed() {  # $1 = package name
    local mgr
    if have dpkg; then mgr=dpkg
    elif have pacman; then mgr=pacman
    elif have rpm; then mgr=rpm
    else return 1; fi
    # ncurses aliasing (see the note above pkg_lists).
    case "$1" in
        libncurses-dev|libncurses5-dev|libncursesw5-dev)
            if [ "$mgr" = dpkg ]; then
                local n
                for n in libncurses-dev libncurses5-dev libncursesw5-dev; do
                    dpkg -s "$n" >/dev/null 2>&1 && return 0
                done
                return 1
            fi
            ;;
    esac
    if [ "$mgr" = dpkg ]; then dpkg -s "$1" >/dev/null 2>&1
    elif [ "$mgr" = pacman ]; then pacman -Q "$1" >/dev/null 2>&1
    else rpm -q "$1" >/dev/null 2>&1; fi
}

deps_missing() {
    local list; list="$(pkg_lists)"
    [ -n "$list" ] || { warn "no apt/pacman/dnf detected — cannot verify system deps"; return 1; }
    local missing=0 p
    for p in $list; do
        if ! pkg_installed "$p"; then
            echo "$p"
            missing=1
        fi
    done
    [ "$missing" = 1 ]
}

install_deps() {
    if have apt-get; then
        info "Installing build deps via apt (sudo)..."
        sudo apt-get update -y
        sudo apt-get install -y \
            mpv libmpv-dev libncurses-dev \
            libcurl4-openssl-dev libsqlite3-dev libxml2-dev libfmt-dev \
            nlohmann-json3-dev libqrencode-dev cmake ninja-build g++
        say "build deps installed (apt)"
    elif have pacman; then
        info "Installing build deps via pacman (sudo)..."
        sudo pacman -S --needed --noconfirm \
            mpv ncurses curl libxml2 sqlite fmt nlohmann-json qrencode cmake ninja gcc
        say "build deps installed (pacman)"
    elif have dnf; then
        info "Installing build deps via dnf (sudo)..."
        sudo dnf install -y \
            mpv mpv-devel ncurses-devel libcurl-devel sqlite-devel libxml2-devel fmt-devel \
            nlohmann-json-devel qrencode-devel cmake ninja-build gcc-c++
        say "build deps installed (dnf)"
    else
        die "no apt/pacman/dnf detected — install the deps manually (see README), then re-run"
    fi
}

# ── JS runtime (yt-dlp nsig solver, RUNTIME dependency — no compilation here) ──
# yt-dlp 2026.07+ needs a JS runtime to solve YouTube's nsig "n challenge".
# quickjs-ng (~2MB, ~10× faster cold-start than deno) is the default; deno
# (~106MB) is the fallback. Deployed to /usr/local/bin from the vendor/ bundle
# or a non-system PATH entry. See vendor/quickjs/README.md for fetching qjs.
# First line of `--version` output (build-invariant version string), or empty.
bin_version() { "$1" --version 2>/dev/null | head -1; }

install_to_system_bin() {  # $1=src  $2=name
    local dst="/usr/local/bin/$2"
    local new_v old_v
    new_v=$(bin_version "$1"); old_v=$(bin_version "$dst")
    if [ -x "$dst" ] && [ -n "$new_v" ] && [ "$new_v" = "$old_v" ]; then
        say "$2 already installed (same version: ${new_v}) (skip)"
        return 0
    fi
    info "installing $2 to $dst (system-wide — sudo required, may prompt for password)"
    sudo cp -f "$1" "$dst"
    sudo chmod +x "$dst"
    say "$2 -> $dst"
}

# Interactive Y/N prompt with a 30s timeout defaulting to Y.
confirm_yes() {  # $1 = prompt (may include color codes)
    local ans
    printf "%b" "$1"
    if read -r -t 30 ans; then
        case "$ans" in
            [Nn]|[Nn][Oo]) return 1 ;;
        esac
    else
        echo
    fi
    return 0
}

js_runtime_ok() {
    local c
    for c in /usr/local/bin/qjs /usr/local/bin/qjsng /usr/local/bin/deno; do
        [ -x "$c" ] && return 0
    done
    return 1
}

install_js_runtime() {
    if js_runtime_ok; then
        say "JS runtime present: $(command -v qjs || command -v qjsng || command -v deno) (yt-dlp nsig solver)"
    elif [ -x vendor/quickjs/qjs ]; then
        install_to_system_bin vendor/quickjs/qjs qjs
    elif [ -x vendor/deno/deno ]; then
        install_to_system_bin vendor/deno/deno deno
    else
        # A candidate on a non-system PATH entry (e.g. ~/.local/bin) can be promoted.
        local _n _p _src="" _name=""
        for _n in qjs qjsng deno; do
            if command -v "$_n" >/dev/null 2>&1; then
                _p=$(command -v "$_n")
                case "$_p" in /usr/local/bin/*) continue ;; esac
                _src="$_p"; _name="$_n"; break
            fi
        done
        if [ -n "$_src" ]; then
            if confirm_yes "${YELLOW}  No JS runtime in /usr/local/bin — promote ${_name} to /usr/local/bin/${_name}? [Y/n] (30s timeout = Y)${NC} "; then
                if sudo cp -f "$_src" "/usr/local/bin/$_name" && sudo chmod +x "/usr/local/bin/$_name"; then
                    say "JS runtime -> /usr/local/bin/$_name"
                else
                    warn "install failed (sudo unavailable or declined): sudo cp $_src /usr/local/bin/$_name"
                fi
            else
                info "skipped JS runtime install (YouTube playback/downloads will fail without one in /usr/local/bin)"
            fi
        else
            warn "no qjs/qjsng/deno found in /usr/local/bin: YouTube playback/download will fail (yt-dlp needs a JS runtime for the n challenge)"
            echo -e "${YELLOW}  Recommended quickjs-ng (~2MB, binary name qjs):${NC}"
            echo -e "${YELLOW}    Arch: paru -S quickjs-ng (AUR)  /  Debian: apt install quickjs  /  pip install quickjs-ng${NC}"
            echo -e "${YELLOW}    or fetch ≥0.12.0 from https://github.com/quickjs-ng/quickjs/releases into vendor/quickjs/qjs and re-run${NC}"
            echo -e "${YELLOW}  or deno (~106MB): curl -fsSL https://deno.land/install.sh | sh (must end up on the system PATH)${NC}"
            echo -e "${YELLOW}  note: quickjs needs the EJS solver — pip install -U \"yt-dlp[default]\"; deno auto-fetches EJS from npm${NC}"
        fi
    fi
    # EJS solver hint (quickjs cannot fetch it from npm by itself).
    if [ -x /usr/local/bin/qjs ] || [ -x /usr/local/bin/qjsng ]; then
        if have yt-dlp && ! python3 -c "import yt_dlp_ejs" 2>/dev/null && ! yt-dlp --version 2>/dev/null | grep -qi ejs; then
            warn "quickjs needs the EJS solver: pip install -U \"yt-dlp[default]\"  (or set [youtube] js_runtime = deno)"
        fi
    fi
}

# ── Legacy-artifact cleanup (one-time migration hygiene, needs sudo) ──────────
# N10: the separate panicastd binary is gone — one binary, two frontends
# (`panicast` TUI / `panicast --daemon` daemon). Remove stale copies so they can
# never be started again (they would race the merged daemon over :9090 and the
# DB). Also retire the pre-N10.3 system-level units + polkit rule; the
# replacement user unit installs itself on the next panicast run (sudo-free).
cleanup_legacy() {
    if [ -x /usr/local/bin/panicastd ]; then
        sudo systemctl stop panicastd.service 2>/dev/null || true
        sudo systemctl disable panicastd.service 2>/dev/null || true
        sudo rm -f /usr/local/bin/panicastd
        say "removed legacy binary -> /usr/local/bin/panicastd"
    fi
    if [ -f /etc/systemd/system/panicast.service ]; then
        sudo systemctl stop panicast.service 2>/dev/null || true
        sudo systemctl disable panicast.service 2>/dev/null || true
        sudo rm -f /etc/systemd/system/panicast.service
        say "removed system-level unit -> panicast.service (replaced by a user unit)"
    fi
    if [ -f /etc/systemd/system/panicastd.service ]; then
        sudo systemctl stop panicastd.service 2>/dev/null || true
        sudo systemctl disable panicastd.service 2>/dev/null || true
        sudo rm -f /etc/systemd/system/panicastd.service
        say "removed legacy unit -> panicastd.service"
    fi
    if [ -f /etc/polkit-1/rules.d/49-panicast.rules ] || [ -f /etc/polkit-1/rules.d/49-panicastd.rules ]; then
        sudo rm -f /etc/polkit-1/rules.d/49-panicast.rules /etc/polkit-1/rules.d/49-panicastd.rules
        say "removed legacy polkit rules"
    fi
}

# ── Preflight (--check): verify only, never install ───────────────────────────
do_check() {
    local fail=0 missing
    info "toolchain"
    for t in cmake ninja c++; do
        if have $t; then say "  $t: $($t --version 2>/dev/null | head -1)"
        else echo -e "${RED}✗${NC}   $t: MISSING"; fail=1; fi
    done
    info "system build deps ($( have apt-get && echo apt || { have pacman && echo pacman || echo dnf; } ))"
    missing="$(deps_missing || true)"
    if [ -n "$missing" ]; then
        echo "$missing" | sed 's/^/  MISSING: /'
        fail=1
    else
        say "  all packages present"
    fi
    info "JS runtime (yt-dlp nsig solver)"
    if js_runtime_ok; then
        say "  $( [ -x /usr/local/bin/qjs ] && echo /usr/local/bin/qjs || { [ -x /usr/local/bin/qjsng ] && echo /usr/local/bin/qjsng || echo /usr/local/bin/deno; } )"
    else
        echo -e "${YELLOW}⚠${NC}   none in /usr/local/bin — YouTube playback will fail (fix: run ./setup.sh)"
        fail=1
    fi
    echo
    if [ "$fail" = 1 ]; then
        die "environment NOT ready — run ./setup.sh to fix the items above"
    fi
    say "environment ready — build with: ./build.sh"
}

# ── Dispatch ──────────────────────────────────────────────────────────────────
if [ "$CHECK_ONLY" = 1 ]; then
    do_check
else
    install_deps
    install_js_runtime
    cleanup_legacy
    echo
    echo -e "${GREEN}Environment ready. Next:${NC}"
    echo "  ./build.sh                 build → bin/panicast"
    echo "  sudo make install          deploy → /usr/local/bin"
fi
