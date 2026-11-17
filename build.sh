#!/bin/bash
# panicast — top-level compile entry (LIF-006).
#   Thin orchestrator: all real work goes through make (Makefile → CMake/Ninja).
#   Never installs system packages (that is setup.sh's job).
#
# Usage:
#   ./build.sh            release build (default)
#   ./build.sh --debug    debug build (symbols, no optimization)
#   ./build.sh --clean    clean output first, then build
#
# Artifacts always land in ./bin/panicast. First-time environment prep:
#   ./setup.sh            (deps + JS runtime; never compiles)
set -e
cd "$(dirname "$0")"

say()  { echo -e "\033[0;32m✓\033[0m $1"; }
warn() { echo -e "\033[1;33m⚠\033[0m $1"; }
info() { echo -e "\033[0;34m…\033[0m $1"; }
die()  { echo -e "\033[0;31m✗\033[0m $1" >&2; exit 1; }

# ── Memory-aware job count ────────────────────────────────────────────────────
# ~1 GiB per compile job (cc1plus on heavy files can exceed 800 MiB), ~2 GiB
# headroom for the OS, capped at nproc. Prevents OOM kills on low-RAM hosts
# (e.g. 4 GiB → 2 jobs instead of $(nproc)=16). Override: PANICAST_BUILD_JOBS=N.
if [ -z "${PANICAST_BUILD_JOBS:-}" ]; then
    _NJ=$(nproc 2>/dev/null || echo 4)
    _GB=$(awk '/MemTotal/{printf "%d", $2/1024/1024}' /proc/meminfo 2>/dev/null || echo 8)
    JOBS=$(( _GB > 2 ? _GB - 2 : 1 ))
    [ "$JOBS" -gt "$_NJ" ] && JOBS=$_NJ
else
    JOBS="$PANICAST_BUILD_JOBS"
fi
export JOBS

# ── Build-cache freshness guard ───────────────────────────────────────────────
# Risk: after git checkout / commit switching, source mtimes can be OLDER than
# the .o files → ninja concludes "nothing to rebuild" → an outdated binary gets
# shipped. Guard: stamp the working tree's content state (tree sha via
# `git stash create` — snapshots the dirty tree without touching anything; a
# clean tree uses HEAD's) after every successful build. Next build: when the
# state changed but a ninja dry-run shows no work, mtimes are untrustworthy →
# touch all sources to force a full rebuild. Normal edit iterations are
# unaffected (ninja's own incremental logic applies).
build_source_state() {
    local stash_tree
    stash_tree="$(git stash create 2>/dev/null)"
    if [ -n "$stash_tree" ]; then
        git rev-parse "$stash_tree^{tree}" 2>/dev/null
    else
        git rev-parse "HEAD^{tree}" 2>/dev/null
    fi
}

build_freshness_guard() {
    [ -f bin/panicast ] || [ -f build/build.ninja ] || return 0  # nothing built yet
    command -v git >/dev/null || return 0    # non-git tree — mtime is all we have
    local stamp_file=build/.build-source-stamp
    local now_state
    now_state="$(build_source_state)"
    if [ -f "$stamp_file" ] && [ "$(cat "$stamp_file")" = "$now_state" ]; then
        return 0
    fi
    # Git state changed since the last successful build — would ninja no-op?
    #   (ninja -n prints "ninja: no work to do." even when idle — filter it out)
    local dry
    dry="$(cmake --build build --parallel "$JOBS" -- -n 2>/dev/null || true)"
    if [ -n "$(printf '%s' "$dry" | grep -v '^ninja: no work to do\.$')" ]; then
        return 0   # ninja already sees real work — let mtime incremental proceed
    fi
    warn "git state changed but the build cache sees no work (mtime regression) — forcing full rebuild"
    find src include -name '*.cpp' -o -name '*.h' -o -name '*.hpp' 2>/dev/null | xargs -r touch
}

# ── Args ──────────────────────────────────────────────────────────────────────
TYPE=Release
DO_CLEAN=0
for arg in "$@"; do
    case "$arg" in
        --debug) TYPE=Debug ;;
        --clean) DO_CLEAN=1 ;;
        *) echo "unknown argument: $arg"; echo "usage: $0 [--debug] [--clean]"; exit 1 ;;
    esac
done

[ "$DO_CLEAN" = 1 ] && make clean

info "building ($TYPE, ${JOBS} jobs) via make"
build_freshness_guard
make build BUILD_TYPE="$TYPE" JOBS="$JOBS"
[ -x bin/panicast ] || die "bin/panicast missing — build failed"
build_source_state > build/.build-source-stamp
say "built: bin/panicast"

# Y01: libqrencode is optional. If absent, QR login falls back to text.
if ! pkg-config --exists libqrencode 2>/dev/null && [ ! -f /usr/include/qrencode.h ]; then
    warn "libqrencode not found: Y-mode QR login falls back to plain-text user_code."
    warn "  install: apt install libqrencode-dev  /  pacman -S qrencode"
fi

echo
echo "  Binary:  bin/panicast"
echo "  Install: sudo make install          (file layer → /usr/local/bin)"
echo "  Service: panicast service install   (unit registration, sudo-free)"
