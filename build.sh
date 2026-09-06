#!/bin/bash
# ╔═══════════════════════════════════════════════════════════════════════════╗
# ║          panicast — build + install (single script, native-only)         ║
# ╚═══════════════════════════════════════════════════════════════════════════╝
# Usage:
#   ./build.sh                     # compile for the current host CPU (auto-detected)
#   ./build.sh install             # bootstrap: JS runtime + build deps + build + install
#   ./build.sh install --no-deps   #   (skip the system build-deps step)
#   ./build.sh clean               # remove build/
#
# Native build only — no cross-compilation. Each machine compiles for its own CPU
#   (uname -m auto-detected), so run the script on the target arch directly.
# Everything installs to /usr/local/bin (system PATH — no ~/.local/bin, no PATH edit).
# Writing to /usr/local/bin requires sudo (install mode, and build mode auto-relocating a JS runtime).

set -e
cd "$(dirname "$0")"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'
say()  { echo -e "${GREEN}✓${NC} $1"; }
warn() { echo -e "${YELLOW}⚠${NC} $1"; }
info() { echo -e "${BLUE}…${NC} $1"; }
die()  { echo -e "${RED}✗${NC} $1" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

# Memory-aware parallel job count: ~1 GiB per compile job (cc1plus on heavy files can exceed
#   800 MiB), leave 1 GiB headroom for the OS, cap at nproc. Prevents OOM kills on low-RAM hosts
#   (e.g. 4 GiB → 2 jobs instead of $(nproc)=16). Override with PANICAST_BUILD_JOBS.
if [ -z "${PANICAST_BUILD_JOBS:-}" ]; then
    _NJ=$(nproc 2>/dev/null || echo 4)
    _GB=$(awk '/MemTotal/{printf "%d", $2/1024/1024}' /proc/meminfo 2>/dev/null || echo 8)
    # ~1 GiB per job, leave ~2 GiB headroom (OS + editor + running panicast). min 1.
    JOBS=$(( _GB > 2 ? _GB - 2 : 1 ))
    [ "$JOBS" -gt "$_NJ" ] && JOBS=$_NJ
else
    JOBS="$PANICAST_BUILD_JOBS"
fi
export JOBS

# Auto-detect host arch (uname -m → canonical target name).
case "$(uname -m)" in
    x86_64|amd64)   HOST_ARCH=amd64 ;;
    aarch64|arm64)  HOST_ARCH=arm64 ;;
    *)              HOST_ARCH=amd64 ;;  # unknown → assume amd64
esac

# ── Arg parsing ───────────────────────────────────────────────────────────────
MODE=build        # build | install | clean
NO_DEPS=0
for arg in "$@"; do
    case "$arg" in
        install|setup) MODE=install ;;
        clean)         MODE=clean ;;
        --no-deps)     NO_DEPS=1 ;;
        *) echo "未知参数: $arg"; echo "用法: $0 [install [--no-deps]] [clean]"; exit 1 ;;
    esac
done

# ── Install: everything → /usr/local/bin (system dirs, sudo required) ─────────
# First line of `--version` output (build-invariant version string), or empty if unsupported.
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

# yt-dlp 2026.07+ needs a JS runtime to solve YouTube's nsig "n challenge".
# quickjs-ng (~2MB, ~10× faster cold-start than deno) is the default; deno (~106MB) is the fallback.
# Both come from the vendor/ bundle and install to /usr/local/bin (no ~, no PATH edit).
install_js_runtime() {
    if [ -x vendor/quickjs/qjs ]; then
        install_to_system_bin vendor/quickjs/qjs qjs
        if have yt-dlp && ! python3 -c "import yt_dlp_ejs" 2>/dev/null && ! yt-dlp --version 2>/dev/null | grep -qi ejs; then
            warn "quickjs needs the EJS solver: pip install -U \"yt-dlp[default]\"  (or set [youtube] js_runtime = deno)"
        fi
    elif [ -x vendor/deno/deno ]; then
        install_to_system_bin vendor/deno/deno deno
    else
        warn "no bundled JS runtime (vendor/quickjs/qjs or vendor/deno/deno) — yt-dlp nsig solving will fail"
    fi
}

# Interactive Y/N prompt with a 30s timeout defaulting to Y (empty input or timeout → Y).
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

# Build-mode JS runtime check. If the runtime is missing from /usr/local/bin but a candidate
# source exists (the vendor/ bundle, or a binary on a non-system PATH entry like ~/.local/bin),
# ask Y/N before installing it to /usr/local/bin (30s default Y). Never fails the build.
build_check_js_runtime() {
    local _c _src="" _name="" _p _n
    for _c in /usr/local/bin/qjs /usr/local/bin/qjsng /usr/local/bin/deno; do
        if [ -x "$_c" ]; then
            say "JS 运行时: $_c (yt-dlp nsig solver)"
            return 0
        fi
    done

    if [ -x vendor/quickjs/qjs ]; then
        _src=vendor/quickjs/qjs; _name=qjs
    elif [ -x vendor/deno/deno ]; then
        _src=vendor/deno/deno; _name=deno
    else
        for _n in qjs qjsng deno; do
            if command -v "$_n" >/dev/null 2>&1; then
                _p=$(command -v "$_n")
                case "$_p" in /usr/local/bin/*) continue ;; esac
                _src="$_p"; _name="$_n"; break
            fi
        done
    fi

    if [ -n "$_src" ]; then
        if confirm_yes "${YELLOW}  未在 /usr/local/bin 检测到 JS 运行时，是否把 $_name 安装到 /usr/local/bin/$_name ？[Y/n]（30s 无输入默认 Y）${NC} "; then
            if sudo mv -f "$_src" "/usr/local/bin/$_name" && sudo chmod +x "/usr/local/bin/$_name"; then
                say "JS 运行时 -> /usr/local/bin/$_name"
            else
                warn "安装失败（sudo 不可用或被拒绝）：请手动  sudo mv $_src /usr/local/bin/$_name"
            fi
        else
            info "跳过 JS 运行时安装（YouTube 播放/下载将不可用，除非手动装到 /usr/local/bin）"
        fi
        return 0
    fi

    warn "未在 /usr/local/bin 下检测到 qjs/qjsng/deno：YouTube 播放/下载将失败（yt-dlp 求解 n 挑战需要 JS 运行时）"
    echo -e "${YELLOW}  推荐 quickjs-ng(2MB，二进制名 qjs)：${NC}"
    echo -e "${YELLOW}    Arch: paru -S quickjs-ng (AUR)  /  Debian: apt install quickjs 或 pip install quickjs-ng${NC}"
    echo -e "${YELLOW}    或从 https://github.com/quickjs-ng/quickjs/releases 取 ≥0.12.0 放到 /usr/local/bin(命名为 qjs)${NC}"
    echo -e "${YELLOW}  或 deno(106MB)：curl -fsSL https://deno.land/install.sh | sh（装完同样需在系统路径）${NC}"
    echo -e "${YELLOW}  注: quickjs 需 EJS solver——pip install -U \"yt-dlp[default]\"；deno 可自动从 npm 拉 EJS${NC}"
}

# 0 if every build dep for the active package manager is already installed.
deps_installed() {
    if have apt-get; then
        for p in mpv libmpv-dev libncurses5-dev libncursesw5-dev libcurl4-openssl-dev \
                 libsqlite3-dev libxml2-dev libfmt-dev nlohmann-json3-dev libqrencode-dev \
                 cmake ninja-build g++; do
            dpkg -s "$p" >/dev/null 2>&1 || return 1
        done
        return 0
    elif have pacman; then
        for p in mpv ncurses curl libxml2 sqlite fmt nlohmann-json qrencode cmake ninja gcc; do
            pacman -Q "$p" >/dev/null 2>&1 || return 1
        done
        return 0
    elif have dnf; then
        for p in mpv mpv-devel ncurses-devel libcurl-devel sqlite-devel libxml2-devel \
                 fmt-devel nlohmann-json-devel qrencode-devel cmake ninja-build gcc-c++; do
            rpm -q "$p" >/dev/null 2>&1 || return 1
        done
        return 0
    fi
    return 1  # unknown package manager → assume not installed
}

install_deps() {
    if deps_installed; then
        say "build deps already installed (skip)"
        return 0
    fi
    if have apt-get; then
        info "Installing build deps via apt (sudo)..."
        sudo apt-get update -y
        sudo apt-get install -y \
            mpv libmpv-dev libncurses5-dev libncursesw5-dev \
            libcurl4-openssl-dev libsqlite3-dev libxml2-dev libfmt-dev \
            nlohmann-json3-dev libqrencode-dev cmake ninja-build g++
        say "build deps installed"
    elif have pacman; then
        info "Installing build deps via pacman (sudo)..."
        sudo pacman -S --needed --noconfirm \
            mpv ncurses curl libxml2 sqlite fmt nlohmann-json qrencode cmake ninja gcc
    elif have dnf; then
        info "Installing build deps via dnf (sudo)..."
        sudo dnf install -y \
            mpv mpv-devel ncurses-devel libcurl-devel sqlite-devel libxml2-devel fmt-devel \
            nlohmann-json-devel qrencode-devel cmake ninja-build gcc-c++
    else
        warn "No apt/pacman/dnf detected — skipping build-deps. Install them manually (see README)."
    fi
}

install_panicast() {
    info "installing panicast to /usr/local/bin (system-wide — sudo required, may prompt for password)"
    if sudo cmake --install build 2>/dev/null; then
        say "installed -> /usr/local/bin/panicast"
    else
        warn "cmake --install failed, trying direct copy"
        [ -x build/panicast ] || die "build/panicast 不存在 — 已中止安装"
        sudo cp -f build/panicast /usr/local/bin/panicast
        say "installed -> /usr/local/bin/panicast"
    fi
    # N10: the separate panicastd binary is gone — one binary, two frontends (`panicast`
    #   TUI / `panicast -d` daemon). Remove a stale installed copy so it can never be
    #   started again (it would race the merged daemon over :9090 and the DB).
    if [ -x /usr/local/bin/panicastd ]; then
        sudo systemctl stop panicastd.service 2>/dev/null
        sudo systemctl disable panicastd.service 2>/dev/null
        sudo rm -f /usr/local/bin/panicastd
        say "removed legacy binary -> /usr/local/bin/panicastd"
    fi
    install_daemon_units
}

# N10.3: the daemon is a USER-space unit (~/.config/systemd/user/panicast.service) —
#   the binary installs/refreshes it itself on first run (ensure_user_unit), so this
#   hook only MIGRATES away the pre-N10.3 system-level unit + polkit rule (needs the
#   one-time sudo this script already uses for /usr/local/bin).
install_daemon_units() {
    # N10 → N10.3 migration: retire the system-level panicast(-d) units and the polkit
    #   rule; the replacement user unit appears on the next `panicast` run.
    if [ -f /etc/systemd/system/panicast.service ]; then
        sudo systemctl stop panicast.service 2>/dev/null
        sudo systemctl disable panicast.service 2>/dev/null
        sudo rm -f /etc/systemd/system/panicast.service
        say "removed system-level unit -> panicast.service (replaced by a user unit)"
    fi
    if [ -f /etc/systemd/system/panicastd.service ]; then
        sudo systemctl stop panicastd.service 2>/dev/null
        sudo systemctl disable panicastd.service 2>/dev/null
        sudo rm -f /etc/systemd/system/panicastd.service
        say "removed legacy unit -> panicastd.service"
    fi
    sudo rm -f /etc/polkit-1/rules.d/49-panicast.rules /etc/polkit-1/rules.d/49-panicastd.rules
    echo "The background service is now a USER systemd unit — it installs itself on"
    echo "the next panicast run (sudo-free). Enable autostart with: panicast enable"
}

do_install() {
    have sudo || die "sudo is required — the install path writes to /usr/local/bin"
    install_js_runtime
    [ "$NO_DEPS" = "0" ] && install_deps
    build_native   # install always builds for THIS machine (host arch)
    install_panicast
    echo
    echo -e "${GREEN}══════════════════════════════════════════════════════${NC}"
    echo -e "${GREEN} panicast ready.${NC}"
    echo -e " Run: ${BLUE}panicast${NC}"
    echo -e "${GREEN}══════════════════════════════════════════════════════${NC}"
}

# ── Build ─────────────────────────────────────────────────────────────────────
# ── 编译缓存新鲜度防护 ────────────────────────────────────────────────────────
#   风险:git checkout/切换提交后,源文件 mtime 可能比 .o 旧 → ninja 判定"无需重建"
#   → `install` 把过期二进制装进 /usr/local/bin。防护:成功构建时记录 git 状态戳
#   (HEAD + 是否 dirty);下次构建若状态变了、而 ninja 干跑显示无事可做,则时间戳
#   不可信 → touch 全部源码强制全量重建。正常编辑迭代不受影响(ninja 自有工作)。
# Content-addressed source state: the WORKING TREE's tree sha (git stash create
#   snapshots the dirty tree without touching anything; a clean tree uses HEAD's).
#   Same content → same stamp (committing already-built sources does NOT force a
#   rebuild); any content change → different stamp → freshness guard engages.
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
    [ -f build/panicast ] || return 0        # no binary yet — normal full build
    command -v git >/dev/null || return 0    # non-git tree — mtime is all we have
    local stamp_file=build/.build-source-stamp
    local now_state
    now_state="$(build_source_state)"
    if [ -f "$stamp_file" ] && [ "$(cat "$stamp_file")" = "$now_state" ]; then
        return 0
    fi
    # git state changed since the last successful build — is ninja about to no-op?
    #   (ninja -n prints "ninja: no work to do." even when idle — filter it out)
    local dry
    dry="$(cmake --build build --parallel "$JOBS" -- -n 2>/dev/null || true)"
    if [ -n "$(printf '%s' "$dry" | grep -v '^ninja: no work to do\.$')" ]; then
        return 0   # ninja already sees real work — let mtime incremental proceed
    fi
    warn "git 状态变化但编译缓存判定无需重建(时间戳回退)— 强制全量重建"
    find src include -name '*.cpp' -o -name '*.h' -o -name '*.hpp' 2>/dev/null | xargs -r touch
}

build_native() {  # native build for the host arch
    echo -e "\n${BLUE}[${HOST_ARCH}] 编译中...${NC}"
    build_freshness_guard
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || die "CMake 配置失败 — 已中止(绝不安装过期二进制)"
    cmake --build build --parallel "$JOBS" || die "编译失败 — 已中止(build/panicast 是旧文件,绝不安装)"
    [ -x build/panicast ] || die "build/panicast 不存在 — 已中止安装"
    build_source_state > build/.build-source-stamp
    echo -e "${GREEN}✓ 完成: build/panicast${NC}"
    # Y01: libqrencode is optional. If absent, cmake warns and Y-mode QR login falls back to text.
    if ! pkg-config --exists libqrencode 2>/dev/null && [ ! -f /usr/include/qrencode.h ]; then
        echo -e "${YELLOW}⚠ 未检测到 libqrencode：Y 模式扫码登录将回退为纯文本 user_code（无 QR 图片）。${NC}"
        echo -e "${YELLOW}  安装：apt install libqrencode-dev  /  pacman -S qrencode  /  vcpkg install qrencode${NC}"
    fi
    # Y05: a JavaScript runtime is a RUNTIME dependency for YouTube playback (yt-dlp 2026.07+ needs
    #   it to solve YouTube's nsig "n challenge"). build_check_js_runtime() detects it and, if it's
    #   missing from /usr/local/bin but a candidate source exists (vendor/ bundle or a non-system
    #   PATH entry like ~/.local/bin), asks Y/N before installing (30s default Y). See its comment.
    build_check_js_runtime
}

# ── Dispatch ──────────────────────────────────────────────────────────────────
case "$MODE" in
    clean)   rm -rf build; say "cleaned build/" ;;
    install) do_install ;;
    build)   build_native ;;
esac
