#!/usr/bin/env bash
# panicast local preflight (aligned with CI): clang-format advisory check + build + ctest.
# Usage: ./scripts/check.sh   (limit parallelism with PANICAST_BUILD_JOBS=N)
# Exit code: non-zero on configure/build failure; format/test issues are advisory
# (reported, non-blocking).
set -uo pipefail
cd "$(dirname "$0")/.."

say()  { printf '\033[32m✓\033[0m %s\n' "$1"; }
warn() { printf '\033[33m⚠\033[0m %s\n' "$1"; }
info() { printf '\033[34m…\033[0m %s\n' "$1"; }
fail() { printf '\033[31m✗\033[0m %s\n' "$1"; }

# ── 1. clang-format (advisory; changed .cpp/.h vs HEAD only) ────────────────
info "clang-format advisory check (changed C++ files only)"
if ! command -v clang-format >/dev/null 2>&1; then
  warn "clang-format not installed — skipped (apt install clang-format)"
else
  mapfile -t FILES < <(git diff --name-only HEAD -- '*.cpp' '*.h' '*.hpp' 2>/dev/null || true)
  if [ "${#FILES[@]}" -eq 0 ]; then
    say "no changed C++ files — format check skipped"
  else
    bad=0
    for f in "${FILES[@]}"; do
      [ -f "$f" ] || continue
      if ! diff -q <(clang-format "$f" 2>/dev/null) "$f" >/dev/null 2>&1; then
        warn "  format drift: $f"; bad=1
      fi
    done
    if [ "$bad" -eq 0 ]; then say "changed files format OK"
    else warn "run: clang-format -i <file>  (advisory, non-blocking)"; fi
  fi
fi
echo

# ── 2. Build (must pass) ────────────────────────────────────────────────────
info "build (Ninja Release + BUILD_TESTING=ON)"
if ! cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON; then
  fail "configure failed"; exit 1
fi
if ! cmake --build build --parallel "${PANICAST_BUILD_JOBS:-$(nproc 2>/dev/null || echo 4)}"; then
  fail "build failed"; exit 1
fi
say "bin/panicast OK"
echo

# ── 3. Tests (best-effort) ──────────────────────────────────────────────────
info "ctest (best-effort)"
if ctest --test-dir build --output-on-failure; then
  say "tests passed"
else
  warn "ctest failed or no tests (tests are disabled when GTest is missing: apt install libgtest-dev)"
fi
echo

# ── 4. Layer-boundary advisory gate (D11-4 acceptance) ──────────────────────
#   The UI layer must be a pure presentation layer. Dependency invariant
#   (docs/ARCHITECTURE.md §2.1 / stable-dependency principle):
#     Allowed — cross-cutting infrastructure: Utils::* text/display helpers,
#           LOG/EVENT_LOG (the UI's own log panel, see ARCHITECTURE §3),
#           get_emoji_width terminal metrics. Same nature as the standard
#           library — mpv/cmus/Qt/LLVM/Chromium UI layers all use logging
#           directly (cross-cutting concern).
#     Forbidden — Core business: Paths (filesystem) / crypto / ThreadPool /
#           EventBus / process_utils / safe_tmp.
#   (Singleton reads in net/playback/app — URLClassifier/SleepTimer/
#    OnlineState/TikTokRegion — are the D12 IFrontend frontier: the UI
#    self-queries state instead of receiving view models; recorded here but
#    not gated yet; see DECISIONS_LOG D11-4.)
info "layer-boundary advisory gate: zero Core-business calls in the UI layer"
viols=$(grep -rnE 'Paths::|machine_key|token_seal|token_open|Key32|ThreadPool|EventBus::|which_binary|safe_tmp|popen\(' src/ui/ 2>/dev/null || true)
if [ -z "$viols" ]; then
  say "UI layer clean of Core-business calls ✓"
else
  warn "Core-business calls found in src/ui/ (violates docs/ARCHITECTURE.md §2.1 layering):"
  printf '%s\n' "$viols"
  warn "  route through the App/service layer; see DECISIONS_LOG D11-4"
fi
echo
say "preflight done. Smoke test: ./bin/panicast --version"
