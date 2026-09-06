<div align="center">

# 🎧 panicast

**Terminal Podcast/Radio Player**

*A powerful ncurses-based media player for the command line — TuneIn 电台 / RSS 播客 / YouTube（P 模式 cookies 解析 + Y 模式 Google OAuth）/ 本地媒体*

[![Version](https://img.shields.io/badge/version-panicast-V0.0.1-blue.svg)](https://github.com/deadship2003/panicast)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS-lightgrey.svg)](https://github.com/deadship2003/panicast)
[![C++](https://img.shields.io/badge/C++-17-orange.svg)](https://isocpp.org/)
[![JS runtime](https://img.shields.io/badge/yt--dlp%20nsig-quickjs--ng-2ea44f.svg)](https://github.com/quickjs-ng/quickjs)

**[Features](#-功能特性) • [Install](#-安装) • [P vs Y 模式](#p-模式-vs-y-模式独立工作) • [Usage](#-使用方法) • [Build](#-编译选项)**

<img src="https://img.shields.io/badge/TuneIn-📻-red?style=flat-square" alt="TuneIn"/>
<img src="https://img.shields.io/badge/Podcast-🎙️-purple?style=flat-square" alt="Podcast"/>
<img src="https://img.shields.io/badge/YouTube-📺-red?style=flat-square" alt="YouTube"/>
<img src="https://img.shields.io/badge/Local-🎵-blue?style=flat-square" alt="Local"/>
<img src="https://img.shields.io/badge/Google%20OAuth-🔓-yellow?style=flat-square" alt="OAuth"/>
<img src="https://img.shields.io/badge/Bilibili-📺-00a1d6?style=flat-square" alt="Bilibili"/>

</div>

---

## 🎵 功能特性

### 媒体支持
- **📻 电台流媒体** - TuneIn电台目录完整支持
- **🎙️ 播客订阅** - RSS/Atom播客订阅与播放
- **📺 YouTube** - YouTube频道、视频、播放列表
- **🎵 本地文件** - MP3, FLAC, OGG, M4A, WAV, MP4, MKV, WebM

### 播放控制
- **终端TUI界面** - 美观的ncurses交互界面
- **播放控制** - 播放/暂停、音量、速度调节
- **进度追踪** - 自动保存播放位置，断点续播
- **播放模式** - repeat（单曲循环）/ shuffle（随机）/ cycle（列表循环），用 `:` 命令切换；播放列表即当前节目的 peers（兄弟节点），INFO 区显示 3 历史 + 当前 + 3 预告
- **下载管理** - 节目下载到本地

### 数据管理
- **收藏夹** - 收藏喜欢的电台和播客
- **播放历史** - 完整的播放历史记录
- **搜索功能** - 全局内容搜索
- **OPML导入导出** - 订阅备份与恢复

---

## 📥 安装

### 🚀 一键部署

发布包自带 **quickjs-ng** JS 运行时（`vendor/quickjs/`，~2MB）与部署脚本，解压后一条命令搞定依赖+运行时+编译+安装：

```bash
tar xzf panicast-V0.0.1.tar.gz
cd panicast-V0.0.1
./setup.sh install      # JS 运行时 + 构建依赖 + 编译 + 安装 panicast（全部装到 /usr/local/bin，需 sudo）
# 可选参数:
#   install --no-deps   跳过系统构建依赖安装（已装好时）
#   clean               清理 build/
```

`setup.sh` 把 `qjs`/`deno`/`panicast` 都装到 `/usr/local/bin`（系统 PATH 内，**无需改 PATH**）。`./setup.sh` 按当前机器 CPU 自动检测并原生编译（无交叉编译，各平台在本机各自编译）。详见 [`vendor/quickjs/README.md`](vendor/quickjs/README.md)。

> **为什么需要 JS 运行时**：yt-dlp 2026.07+ 求解 YouTube nsig「n 挑战」必须有 JS 运行时。**推荐 quickjs-ng**（~2MB，冷启动比 deno 快约 10×，可消除首次播放 YouTube 的初始卡顿）；deno（~106MB）为回退方案。apt 的 nodejs(20) 被 yt-dlp 标记 unsupported 不生效。详见下文「运行时依赖（JS 运行时）」。
>
> ⚠ **quickjs 需 EJS solver**：quickjs 不能从 npm 拉 EJS（deno 可以），需先 `pip install -U "yt-dlp[default]"`（带入 yt-dlp-ejs）。装不了就回退 deno（`[youtube] js_runtime = deno`）。

### Linux (Arch Linux) ⭐ 推荐

```bash
# 安装依赖
sudo pacman -S --needed mpv ncurses curl libxml2 sqlite fmt nlohmann-json qrencode cmake ninja gcc

# 克隆并编译
git clone https://github.com/deadship2003/panicast.git
cd panicast
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel $(nproc)
sudo cmake --install build

# 运行
panicast
```

### Linux (Debian/Ubuntu)

```bash
# 安装依赖
sudo apt-get update
sudo apt-get install -y mpv libmpv-dev libncurses5-dev libncursesw5-dev \
    libcurl4-openssl-dev libsqlite3-dev libxml2-dev libfmt-dev \
    nlohmann-json3-dev libqrencode-dev cmake ninja-build g++

# 编译
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel $(nproc)
sudo cmake --install build
```

> **依赖 `libqrencode`**（用于 Y 模式 Google 帐号扫码登录渲染二维码）。
> 未安装时构建仍可成功，Y 模式登录回退为纯文本 user_code（无 QR 图片）。
> apt: `libqrencode-dev` · pacman: `qrencode` · dnf: `qrencode-devel` · brew: `qrencode` · vcpkg: `qrencode`.

> **运行时依赖：JS 运行时**（YouTube 播放/下载必需）。yt-dlp 2026.07+ 需要一个 JavaScript 运行时来求解 YouTube 的 nsig「n 挑战」，否则只能取到缩略图、音视频格式全部缺失（报错 `Requested format is not available` / `n challenge solving failed`）。
> **推荐 `quickjs-ng`（`qjs`，~2MB）**：冷启动比 deno 快约 10×，可消除首次播放 YouTube 的初始卡顿。在 `config.ini（~/.config/panicast/）` 设 `[youtube] js_runtime = quickjs`（默认），并确保 `qjs` 在 `PATH`。
> ⚠️ **`node` 不可用**：yt-dlp 内置的 EJS 求解器把 Node 20（Debian/Ubuntu `apt install nodejs` 所装版本）标记为 `unsupported`（需 Node ≥22）。Node ≥22 + `--js-runtimes node` 也可用，但二进制 ~120MB，不如 quickjs 轻。
> **quickjs 安装**：从 [quickjs-ng releases](https://github.com/quickjs-ng/quickjs/releases) 取 ≥0.12.0 的 `qjs` 放到 `PATH`，并 `pip install -U "yt-dlp[default]"`（带入 yt-dlp-ejs；quickjs 不能从 npm 拉 EJS）。
> **deno 回退**：装不了 yt-dlp-ejs 时，`[youtube] js_runtime = deno` 并 `curl -fsSL https://deno.land/install.sh | sh`（~106MB，deno 自动从 npm 拉 EJS）。这是**运行时**依赖（yt-dlp 播放时调用），不影响编译，但未安装时 YouTube 无法播放。

### Linux (Fedora)

```bash
# 安装依赖
sudo dnf install -y mpv mpv-devel ncurses-devel libcurl-devel sqlite-devel \
    libxml2-devel fmt-devel nlohmann-json-devel qrencode-devel cmake ninja-build gcc-c++

# 编译
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel $(nproc)
sudo cmake --install build
```

### macOS

```bash
# 安装依赖 (Homebrew)
brew install mpv ncurses curl sqlite libxml2 fmt nlohmann-json qrencode cmake ninja deno

# 编译
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel $(sysctl -n hw.ncpu)
sudo cmake --install build
```

---

## P 模式 vs Y 模式（独立工作）

panicast 的 YouTube 支持有两种**互相独立**的模式，可单独使用，也可同时用：

| | P 模式（PODCAST/ONLINE） | Y 模式（ACCOUNT） |
|---|---|---|
| 鉴权 | **cookies.txt**（`Ctrl+B`） | **Google OAuth 扫码登录** |
| 订阅/解析频道 | yt-dlp + cookies 解析频道 TAB | Data API 拉订阅列表 + `fetch_channel_videos` 取节目 |
| 节目列表 | yt-dlp `--flat-playlist`（需 cookies+代理+JS 运行时） | Data API（OAuth token，不需 cookies/yt-dlp） |
| 播放 | yt-dlp `-g` + cookies + quickjs（DASH 1080p） | 同 P 模式（OAuth 不能用于播放） |
| 需要 Google 登录？ | ❌ 不需要 | ✅ 需要 |
| 需要 cookies？ | ✅ 播放/解析需要 | 仅播放需要（列表不需要） |

**关键：两者共享底层（`ytdlp_youtube_args_parse` 的 cookies/player_client/js_runtime 参数 + `episode_cache` 表），但功能互不依赖：**
- **只用 P**：放 `youtube_cookie.txt`（`Ctrl+B`），订阅 YouTube 频道，不登录 Google 也能解析+播放。
- **只用 Y**：扫码登录 Google，看订阅/搜索/节目列表（Data API）；播放仍需 cookies（OAuth 不能播放）。
- **两者都用**：cookies 管播放，OAuth 管列表/订阅。

> 所以修 Y 模式的共享件（cookies 单一路径、quickjs、`player_client=tv_downgraded,web`、`episode_cache.is_youtube` 列）会连带让 P 模式 YouTube 订阅也正常——它们走同一套 yt-dlp 参数和缓存表。

---

## 📺 B 模式（Bilibili）

B 模式支持 Bilibili 视频的登录、浏览、搜索、播放和下载。**不需要 quickjs/deno**（B 站无 nsig 挑战），比 Y 模式更轻量。

### 登录与 Cookie

**扫码 = 获取 Cookie**（与 Y 模式 OAuth 扫码 = 获取 Token 同理）：

1. 按 `a` → 显示 QR 码 → 用 B 站 App 扫码 → 自动获取 SESSDATA cookie → 写入 `bilibili_cookie.txt`。
2. `A` = 登录另一个 B 站账号（QR 扫码，同 `a`）。
3. `Ctrl+B`（B 模式下）= 设 cookie 文件路径（如 QR 失败，可浏览器导出 cookies.txt 后设路径）。

**Cookie 的作用**：
- **关注列表**（B 模式展开账号）：需要 SESSDATA（用户私有数据）。
- **UP 主视频列表**（B 模式 + P 模式订阅）：不需要 SESSDATA（公开视频，WBI 签名即可）。
- **播放高清**：需要 SESSDATA（未登录 360p/480p，登录后 720p/1080p+）。

**B 模式与 P 模式共享 cookie**：B 模式扫码 → `bilibili_cookie.txt` → P 模式播放 B 站视频时自动读取 → 高清。

### 浏览

- 展开账号 → **关注列表**（WBI 签名 API `/x/relation/followings`）→ 展开 UP 主 → **视频列表**（WBI 签名 API `/x/space/wbi/arc/search`，真实标题）。
- `/` 搜索 Bilibili 视频（yt-dlp `bilisearch:`）。

### 播放 / 下载

- yt-dlp `--cookies bilibili_cookie.txt` 解析流地址 → mpv 播放（有 cookie = 高清）。
- 下载同 YouTube（yt-dlp + cookies）。

### 配置

```ini
[bilibili]
cookies_file = bilibili_cookie.txt   # 默认 <数据目录>/bilibili_cookie.txt
```

### 与 Y 模式对比

| | Y 模式 (YouTube) | B 模式 (Bilibili) |
|---|---|---|
| 登录 | OAuth2 device flow → access_token | **QR 扫码 → SESSDATA cookie** |
| 认证凭证 | OAuth token (Data API) + cookies (播放) | **SESSDATA cookie**（API + 播放统一） |
| nsig/JS 运行时 | ✅ 需要 quickjs | ❌ **不需要** |
| Data API 配额 | 10000 units/天 | **无配额**（WBI 签名，无限制） |
| P 模式订阅 | YouTube 频道（yt-dlp） | Bilibili UP 主（WBI API，真实标题） |
| P 模式播放 | yt-dlp --cookies youtube_cookie.txt | yt-dlp --cookies bilibili_cookie.txt |
| 字幕 | ✅ sub-text 歌词面板 | ✅ B 站 CC 字幕 |
| 扫码/Cookie 关系 | 扫码=获取 OAuth token | 扫码=获取 SESSDATA cookie |


## 🎵 T 模式（TikTok / 抖音）

终端播客播放器对短视频平台的支持。**匿名，无登录**。区域显示在状态栏边框（`🎵 TIKTOK [US]` / `🎵 抖音 [CN]`），根节点标签静态。

### 平台差异（重要）
- **TikTok**：`tiktok:user` 提取器可用 → 可订阅创作者、展开列其全部视频（匿名）。
- **抖音**：yt-dlp **无 DouyinUserIE**，列不出用户视频 → 仅支持**单个视频播放**（option A）。抖音用户 URL 会被拒绝并提示。

### 操作
| 键 | 作用 |
|---|---|
| `T` | 进入 T 模式 |
| `a` | 添加：`@user` / TikTok 视频 URL / 抖音视频 URL。TikTok 视频 URL 自动订阅其 @user（含 @user），展开列全部视频；抖音视频 URL 存为可播放叶节点 |
| `/` | 打开：`@user` / `#tag` / URL。**无匿名关键词搜索**（搜索引擎不索引 TikTok/抖音内容页，yt-dlp 无搜索提取器）。`#tag` 走 yt-dlp（当前 `tiktok:tag` 上游禁用，休眠中，上游修复后自动可用） |
| `b` | 循环区域：US/JP/GB/DE/FR/KR/ID/TH/VN/MY/BR/MX/**CN**。CN=抖音（douyin.com），其余 12 区=TikTok（tiktok.com + `--geo-bypass-country`） |
| `l`/`Enter` | 展开 TikTok 创作者 → 视频列表（yt-dlp）/ 播放视频 |
| `d` | 删除光标项 |
| `Ctrl+B` | 导入 cookie（CN→`[tiktok] douyin_cookies_file`，否则 `[tiktok] cookies_file`），Netscape cookies.txt |

### Cookie / 网络
- **TikTok**：匿名即可（`[tiktok] cookies_file` 可选，登录态用）。
- **抖音**：需 cookie（**未登录的新鲜 cookie 即可**）+ **中国大陆出口**（TPClash 路由 douyin.com 走 CN）。`[tiktok] douyin_cookies_file` 默认 `<data_dir>/douyin_cookie.txt`。
- 播放走 mpv ytdl_hook（mpv 内置 yt-dlp）。

### 已知限制
- 抖音"展开用户全部视频"不可用（yt-dlp 无 DouyinUserIE）；抖音仅单视频播放。
- `#tag` 标签浏览依赖 yt-dlp `tiktok:tag`（上游 `_WORKING=False`，待修复）。
- 匿名无关键词搜索；用 `a` 直接输入 `@user` / 视频 URL。


## 🔑 OAuth 客户端（Y 模式登录）三种配置方式

Y 模式扫码登录需要一个 Google OAuth "Desktop app / TV and Limited Input" 客户端。三种方式（按优先级，前者覆盖后者）：

1. **运行时文件（免编译，推荐）**：把自己的 `client_secret*.json` 放到 `~/.local/share/panicast/`（数据目录）。程序运行时优先读它，覆盖内置客户端。
2. **编译时内置（bake）**：把 `client_secret.json` 放到源码树 `secrets/` 目录再编译——CMake `configure` 时自动检测，将其 `client_id`/`client_secret` 烘焙为内置默认（方便分发自己编译的版本）。模板见 `secrets/client_secret.json.example`。
3. **不配置**：用项目内置的兜底客户端（开箱即用，无需任何文件）。

> 安全：`secrets/client_secret.json` 已在 `.gitignore`，勿提交真实 secret。Desktop-app 客户端的 secret 按 Google 安装型应用模型本就随二进制分发，非真正机密。

---

## 💬 YouTube 字幕（软字幕）

YouTube 视频的字幕默认**不加载**（`resolve_youtube_url` 用 `yt-dlp -g` 只取视频+音频流，不取字幕）。要启用软字幕（可缩放/移位/居中），在 `config.ini（~/.config/panicast/）` 设：

```ini
[youtube]
sub_lang = en        # 字幕语言码；空=不加载（默认）。例：en / zh-Hans / ja
sub_auto = true      # 无手动字幕时用自动生成字幕
```

**处理流程（编译者/使用者都需了解）：**
1. 播放 YouTube 视频时，`resolve_youtube_url` 先用 `yt-dlp -f <fmt> -g` 取视频+音频流 URL（DASH 双流）。
2. 若 `sub_lang` 非空，**追加一次** `yt-dlp --write-subs --write-auto-sub --sub-langs <lang> --sub-format vtt --skip-download -o <tmp_dir>/pod_sub <url>`，把 `.vtt` 字幕写到临时目录，路径作为第三个返回值。
3. `play_video` 用 mpv `loadfile` 的 per-file options 同时传 `audio-file=<音频流>` 和 `sub-file=<字幕路径>`（逗号分隔）。
4. mpv 加载 vtt 软字幕 → **默认底部居中**渲染；`sub-ass-override=auto` 确保 ASS 字幕也响应缩放。
5. 于是 `:` 命令的 **F/G 字幕大小、r/R 位置、z/Z 同步、v 显隐** 全部生效。

**注意：**
- **硬字幕（burned-in，烧录在画面里的字幕）mpv 无法处理**——它是视频像素的一部分，不能缩放/移位/居中。多数 YouTube 视频用关闭字幕（closed captions），加载软字幕即可；少数硬字幕视频只能换源。
- 字幕文件写到 `Paths::get_tmp_dir()/pod_sub.<lang>.vtt`，每次播放覆盖。
- 依赖 yt-dlp（同播放）+ cookies（同播放）。取字幕会增加 ~1-2s（异步，不阻塞 UI）。

---

## 🎤 转写字幕（whisper.cpp）

无字幕的节目可用 whisper.cpp 离线转写生成 SRT 字幕（回放自动加载）。**实时转写（建设中）**。

### 安装依赖（用户自装）
**Arch**：
```bash
sudo pacman -S whisper-cpp          # 提供 whisper-cli
# 模型（按 CPU 选；AMD 7735HS 用 small.en-q5_1，弱 CPU 用 tiny.en）：
mkdir -p ~/.local/share/panicast/models
wget -O ~/.local/share/panicast/models/ggml-small.en-q5_1.bin \
  https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.en-q5_1.bin
```
**Debian**（Trixie+ 有包；Bookworm 源码编译）：
```bash
sudo apt install -y whisper.cpp     # 或：git clone https://github.com/ggml-org/whisper.cpp && make
./models/download-ggml-model.sh small.en-q5_1   # 源码方式下模型
```

### 配置（`config.ini（~/.config/panicast/）`）
```ini
[transcription]
whisper_bin = whisper-cli                              # 或源码编译的绝对路径
model = ~/.local/share/panicast/models/ggml-small.en-q5_1.bin
max_concurrent = 3                                     # 离线并发上限，[1,3] 内动态 CPU 感知
```

### 用法
- **F 模式**：光标在本地音视频文件（或 `v` 多选若干）→ 按 `L` → 入队离线转写 → 完成生成 `<文件名>.srt`（同路径）→ 回放自动加载字幕。
- 进度在 LOG 区（`Transcribe: queued N` / `Transcribe done: <文件> → <file>.srt`）。
- 并发：调度器按 CPU 负载（getloadavg）在 [1, max_concurrent] 内动态调整，不抢占系统。

### 模型选型（实测）
| CPU | 首选 | 备注 |
|---|---|---|
| AMD 7735HS（本机 WSL2） | small.en-q5_1 / large-v3-turbo-q5_0 | 实时可行 |
| i3-5010U 等弱 CPU | tiny.en | small.en 8x 慢不可用；离线 ~1.5x |

> 仓库：GitHub `ggml-org/whisper.cpp`；模型镜像 `huggingface.co/ggerganov/whisper.cpp`（同项目）。

---

## 🎨 主题（15 套）

15 套 GitHub 流行终端配色，全深色 + **软前景**（无纯白 #ffffff，不刺眼）。`Ctrl+L` 循环切换，`[display] theme_index` 持久化（默认 0）：

`Solarized Dark`(默认) · `Gruvbox Dark` · `Nord` · `Dracula` · `Catppuccin Mocha` · `Tokyo Night` · `Rose Pine` · `One Dark` · `Everforest` · `Kanagawa` · `Ayu Mirage` · `Amber Terminal` · `Cobalt2` · `Horizon Dark` · `Material Ocean`

**配色表单独一个文件**：`src/theme/themes.cpp`（声明在 `include/panicast/theme/themes.h`）。调整配色只改这个文件（`rgb[8][3]`：`[0]=背景 [1-6]=红绿黄蓝品红青 [7]=前景`，RGB 0-1000），不动 UI 逻辑。CMakeLists 已包含。

---

## 📋 编译选项

### CMake 配置参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `CMAKE_BUILD_TYPE` | `Release` | 构建类型: Debug/Release |
| `CMAKE_INSTALL_PREFIX` | `/usr/local` | 安装路径 |
| `PANICAST_REMOTE_LMS` | `ON` | mini-LMS 模块（Squeezer 遥控协议，Bayeux/JSON-RPC 控制面）。OFF 可裁剪精简体积 |

### 常用编译命令

```bash
# Debug 构建 (包含调试符号)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel $(nproc)

# Release 构建 (优化编译)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel $(nproc)

# 指定安装路径
cmake -B build -G Ninja -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build --parallel $(nproc)
sudo cmake --install build

# 清理构建目录
rm -rf build && cmake -B build -G Ninja
```

### CMake 预设

```bash
# Linux Release
cmake --preset linux-release
cmake --build --preset linux-release

# macOS Release
cmake --preset macos-release
cmake --build --preset macos-release

```

### 功能模块裁剪 — PANICAST_REMOTE_LMS（默认 ON）

mini-LMS 模块让 Android 的 **Squeezer**（Logitech Media Server 控制器）直接遥控 panicast：内嵌 LMS JSON-RPC（cometd/Bayeux）控制面，注册单虚拟 player，传输/音量/now-playing 全映射。默认编译内置。

```bash
# 关闭编译（裁剪模块、精简程序体积；CMake 会打印开启指引）
cmake -DPANICAST_REMOTE_LMS=OFF ..

# 重新开启
cmake -DPANICAST_REMOTE_LMS=ON ..
```

**双层控制**：编译内置 ≠ 自动运行。运行层由配置文件控制，**默认开启**（`lms_enable = true`，`bind = 0.0.0.0`），并有两道接入防线——源 IP 白名单 + 可选登录鉴权：

```ini
# ~/.config/panicast/config.ini
[remote]
# mini-LMS（Squeezer 遥控）运行开关 — 默认 true
lms_enable = true
# mini-LMS 监听端口（LMS CLI 惯例 9090；Squeezer 手工填 IP:9090）
lms_port = 9090
# 允许接入的源 IP（CIDR 逗号分隔；裸 IP = /32；留空 = 允许所有，不建议）
# 默认：家庭网段 + 本机回环
lms_allow = 192.168.0.0/16,127.0.0.0/8
# 登录鉴权：与 Squeezer 设置里的 Username/Password 一致才可控制
# 出厂默认 panicast/panicast（装好即用，建议改成自己的）；lms_pass 显式留空 = 关闭鉴权
# 密码明文存放，建议 chmod 600 本文件
lms_user = panicast
lms_pass = panicast
```

> 范围说明：本模块在同一端口（9090）上**自动识别两种协议**——新 Squeezer 的 HTTP cometd/JSON-RPC（含 Basic 鉴权）与传统 CLI 行协议（`login` 鉴权）；队列浏览为阶段二。SlimProto 音频协议（squeezelite 作独立音频输出）为后续可选扩展，不受本开关管理。
> 开发路线详见 [CHANGELOG.md](CHANGELOG.md) 的 N08 / N08.1 / N08.2 / N08.3 条目。

---

## 🚀 使用方法

### 命令行参数

```bash
panicast                    # 启动TUI界面
panicast -a <url>           # 添加订阅源
panicast -i <file>          # 导入OPML
panicast -e <file>          # 导出OPML
panicast -t <time>          # 睡眠定时器 (格式: 5h/30m/1:25:15)
panicast --purge            # 清除缓存
panicast -h                 # 显示帮助
panicast -v                 # 显示版本
```

### 快捷键

| 按键 | 功能 |
|------|------|
| `j`/`↓` | 向下移动 |
| `k`/`↑` | 向上移动 |
| `l`/`Enter` | 播放/展开 |
| `h` | 返回/折叠 |
| `Space`/`p` | 暂停/继续 |
| `+`/`-` | 音量增减 |
| `[`/`]` | 速度增减 |
| `Ctrl+Y` | 复制当前播放码流 URL 到剪贴板（未播放则复制光标节点 URL） |
| `B` | Bilibili 模式 |
| `b` | 切换搜索区域（ONLINE 模式） |
| `Ctrl+L` | 循环 15 套主题（Solarized Dark/Gruvbox/Nord/Dracula/...） |
| `a` | 添加订阅 |
| `d` | 删除 |
| `f` | 收藏 |
| `/` | 搜索 |
| `?` | 帮助 |
| `q` | 退出 |

### `:` 命令窗口 — mpv 热键

播放时按 `:` 打开命令窗口，输入一个字符：`r`/`s`/`c` 切播放模式（repeat/shuffle/cycle），其余字符转发给 mpv。键值对齐 mpv 原生 `input.conf`（原生绑定标注 ✅）：

| 类别 | 键 | mpv 命令 | 作用 |
|---|---|---|---|
| **画面/缩放** | `+` / `-` / `=` | `add video-zoom ±0.1` / `set 0` | 缩放 放大/缩小/复位（无原生单键；mpv 原生是 Alt++/Alt+-） |
| | `f` ✅ | `fullscreen` | 全屏 |
| | `A` ✅ | `cycle video-aspect` | 切宽高比 |
| | `d` ✅ | `cycle deinterlace` | 反交错 |
| **字幕** | `F` / `G` ✅ | `add sub-scale ∓0.1` | 字幕 缩小/放大 |
| | `z` / `Z` ✅ | `add sub-delay ∓0.1` | 字幕 提前/延后（同步） |
| | `r` / `R` ✅ | `add sub-pos ∓1` | 字幕 上/下移 |
| | `v` ✅ | `cycle sub-visibility` | 字幕 显/隐 |
| | `j` / `J` ✅ | `cycle sub` / `cycle sub down` | 切下一/上一字幕轨 |
| **音频** | `#` ✅ | `cycle audio` | 切音轨 |
| | `m` ✅ | `cycle mute` | 静音 |
| **OSD/统计** | `o` / `O` ✅ | `show-progress` / `cycle osd-level` | OSD 进度/等级 |
| | `i` / `I` ✅ | `stats/display-stats`(/-toggle) | 统计 瞬时/常驻 |
| **循环/截图** | `l` ✅ | `ab-loop` | A-B 循环 |
| | `s` / `S` ✅ | `screenshot` / `screenshot video` | 截图（含/不含字幕） |
| **视频 EQ** | `1`/`2` `3`/`4` `5`/`6` `7`/`8` ✅ | contrast/brightness/gamma/saturation ∓1 | 对比度/亮度/伽马/饱和度 |

> INFO 区还显示一行 `Network: <下载速率> | Buffering: <缓冲秒数或%>`（默认 100ms）刷新。

### 模式切换

| 按键 | 模式 |
|------|------|
| `R` | 电台模式 |
| `P` | 播客模式 |
| `F` | 收藏夹 |
| `H` | 历史记录 |
| `O` | 在线搜索 |
| `Y` | 帐号模式（Google 帐号） |
| `M` | 循环切换模式 |

### Y 模式（Google 帐号）

Y 模式用于管理多个 Google 帐号，每个帐号的 YouTube 订阅与观看记录互相独立。登录走 SmartTube
同款方案（Google OAuth 设备授权 + 终端二维码扫码）。所有数据仍存在同一个 `panicast.db`，OAuth
token 用本机密钥加密存储；现有 podcast/电台/收藏/历史等数据保持全局，不受影响。

左侧树：每个 Google 帐号是一个节点，其下有 `播放历史` 与 `订阅列表` 两个子节点（订阅频道再展开为视频列表）。

| 按键 | 作用 |
|------|------|
| `a` | 登录一个 Google 帐号（显示二维码，手机扫码授权） |
| `A` | 登录**另外一个** Google 帐号（追加帐号） |
| `j` / `k` | 上下选择帐号 |
| `l` / `Enter` | 激活帐号 / 展开 `播放历史`·`订阅列表` / 进入频道 / 播放视频 |
| `r` | 重新同步该帐号的 YouTube 订阅 + 观看记录 |
| `d` | 删除所选帐号（及其 YouTube 数据，需确认） |

激活的 Google 帐号的 OAuth 授权会**自动**应用到所有 YouTube 播放/下载路径（P 模式 `a` 订阅的
YouTube 频道、F 模式、Y 模式频道浏览），即 yt-dlp/mpv 拿到登录态，绕过机器人验证、可播会员/年龄限制内容，无需手动操作。

> **已知限制**：YouTube 观看记录**拉取**走非官方 InnerTube（Data API v3 不提供观看记录）；
> YouTube 无"标记已看/回写续播进度"的官方 API，故本地播放仅记录到该帐号的 `播放历史`（本地侧），
> 订阅的增删可双向（Data API）。yt-dlp oauth2 cache 文件名/格式按默认实现，若 yt-dlp 版本不匹配则
> 回退其自身 device flow，cookie 作为无帐号时的兜底。

---

## 📁 项目结构

```
panicast/
├── CMakeLists.txt              # CMake 配置
├── CMakePresets.json           # CMake 预设
├── vcpkg.json                  # vcpkg 依赖管理
├── src/
│   └── panicast.cpp            # 主程序源码 (14,043 行)
├── man/
│   └── panicast.1              # man 手册页
├── .github/
│   └── workflows/
│       └── build.yml           # GitHub Actions CI/CD
├── README.md                   # 项目说明
├── LICENSE                     # MIT 许可证
└── setup.sh                    # 构建脚本
```

---

## 📋 依赖库

| 库 | 版本要求 | 用途 | Arch 包名 | Debian 包名 |
|---|---------|------|-----------|-------------|
| libmpv | 0.34+ | 媒体播放 | `mpv` | `libmpv-dev` |
| ncurses | 6.0+ | 终端UI | `ncurses` | `libncurses5-dev` |
| libcurl | 7.80+ | 网络请求 | `curl` | `libcurl4-openssl-dev` |
| libxml2 | 2.9+ | XML/RSS解析 | `libxml2` | `libxml2-dev` |
| SQLite3 | 3.40+ | 数据存储 | `sqlite` | `libsqlite3-dev` |
| fmt | 10.0+ | 格式化输出 | `fmt` | `libfmt-dev` |
| nlohmann_json | 3.11+ | JSON处理 | `nlohmann-json` | `nlohmann-json3-dev` |
| yt-dlp | 可选 | YouTube支持 | `yt-dlp` | `yt-dlp` |

---

## 📂 数据存储

所有用户数据存储在 `~/.local/share/panicast/`:

| 文件/目录 | 说明 |
|----------|------|
| `panicast.db` | SQLite数据库 (订阅、历史、缓存) |
| `downloads/` | 下载的媒体文件 |
| `cache/` | 缓存的媒体和元数据 |

配置文件位置: `~/.config/panicast/config.ini`

---

## 📄 许可证

MIT License - 详见 [LICENSE](LICENSE)

---

## 🤝 贡献

欢迎提交 Issue 和 Pull Request！

---

## 📮 联系方式

- **作者**: Panic
- **邮箱**: Deadship2003@gmail.com
- **Issues**: [GitHub Issues](https://github.com/deadship2003/panicast/issues)

---

<p align="center">
  Made with ❤️ by <a href="mailto:Deadship2003@gmail.com">Panic</a>
</p>
