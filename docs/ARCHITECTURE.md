# 架构总览（真实状态）

> 本文档描述 panicast **当前**的真实架构，作为开发与重构的唯一架构事实来源。
> 决策记录见 `DECISIONS_LOG.md`（N0x/Y24.x 等条目，体例：Context/Approach/Verification/Followups）；
> 缺陷清单见 `AUDIT_REPORT.md`（P1/P2/DB/FX 等）。

## 1. 技术栈

C++17 · ncurses（TUI，强制宽字符 libncursesw）· libmpv（client API，非 IPC）· libcurl · libxml2 · sqlite3 · fmt · nlohmann_json · OpenSSL/libcrypto（Bilibili WBI + token 加密）· libqrencode（OAuth 登录 QR，可选）。
构建：CMake（单 `panicast` 可执行）+ Ninja + vcpkg（`vcpkg.json`）。

## 2. 目录约定

```
include/panicast/<module>/*.h   声明
src/<module>/*.cpp              实现
```
模块（`src/` 1:1 对应）：
| 模块 | 职责 |
|---|---|
| `core` | 基础设施：types/constants/logger/event_log/thread_pool/crypto/paths/terminal/safe_tmp/platform/utils |
| `config` | INI 配置：`ini_config.h`（304 行，纯声明 + 默认值文档）+ `ini_config.cpp`（D24-D31 起 72 个方法 out-of-line，含核心存取簇 load/save/get_int/set、static helper、create_default 配置模板）。仅 get/get_float/get_bool/resolve_cookies_path（多行签名简单访问器，留 inline 合理）+ trivial ctor 仍 inline |
| `net` | 网络：HTTP（`network.cpp` 代理入口）、URL 分类、yt-dlp 运行、Google OAuth、Bilibili API、远程控制（server/session/ws/command_bus/protocol）、TikTok 区域 |
| `parsers` | feed 解析器（`IFeedParser` + `ParserRegistry` 自注册：rss/opml/youtube_channel）+ 非feed解析器（bilibili API/itunes 搜索/m3u/tiktok/transcript） |
| `playback` | libmpv 封装（`mpv_controller` 生命周期核心：event_loop 纯派发骨架 + D36 codec-info 块 + D41 END_FILE 分支(handle_end_file_/handle_playback_error_) 已 Extract Method + `mpv_commands` D18 控制 wrapper + `mpv_metadata` D19 静态诊断 + `mpv_iptv` D20 IPTV 检测 + `mpv_play` D34 单条播放派发（play_audio/play_video/play）+ D48 play_list 簇（play_list/play_list_from））、睡眠定时 |
| `storage` | 持久化：database + 各 repo（history/tree/feed_cache/account/player_state/accounts/cache/youtube_cache）+ `persistence` 抽象 |
| `subtitle` | 字幕：`subtitle_parser`（`ISubtitleParser` + Registry）、`subtitle_manager`、`transcription_engine`（ASR/whisper） |
| `theme` | 颜色/主题/字符对 |
| `ui` | ncurses 渲染：ui（D22 后仅 `draw` + 渲染辅助，原 638→263；lifecycle 终端/信号+init/cleanup/handle_resize 抽至 `ui_lifecycle`）/ui_help（D17 抽自 ui.cpp 的 draw_help）/ui_toggles（D21 视图态 setter/toggle）/popups/lyric_renderer/tree_renderer/status_bar/info_panel/layout/border/icons/art/qr |
| `app` | 应用层：`app_run`（主循环：D35 抽 startup/shutdown bookend、D37 抽 tree-locked 显示构建 phase、D38 引入 FrameCtx + 抽 prepare_frame()（Method Object）、D39 抽 draw_frame(FrameCtx&)，run() loop 收敛为 flat 骨架：信号/定时退出检查→drain→prepare_frame→视图滚动→draw_frame→input→watchdog）、`app_persistence`（D23）、`app_feeds`（D32）、`app_flatten`（D33）、`app_input`（键派发）、`download_service`（D43 抽下载**执行引擎**：pending 队列 + pump/start_one + yt-dlp 核心 + curl 路径 + 校验簇；App 仅留 download_node 薄编排器）、`app_playback/download/search/subscriptions/navigation/...`、modes/ |

入口：`src/main.cpp` → `App::run()`（`src/app/app_run.cpp`）。

### 2.1 层间依赖规则（UI 解耦不变量 · D11-4 确立）

**核心判据（稳定依赖原则）**：某层能否依赖 X，看 X 是否随业务变化——稳定基础设施可依赖，易变业务/运行时状态不可依赖。

UI（`src/ui/`）是纯呈现层，依赖规则：
- **允许（横切基础设施，性质等同标准库）**：`Utils::*`（文本/显示工具）、`LOG`/`EVENT_LOG`（后者是 UI 右侧日志环形缓冲，见 §3）、`get_emoji_width`（终端度量）、`URLClassifier::classify`/`is_local_file`/`is_youtube`（**无状态**纯函数：URL 串→`URLType` 枚举 / 本地文件判定、表驱动、无 I/O 无 `instance()`——物理在 `net/` 但性质同 Utils，§3 亦列为基础设施）。同类软件（mpv / cmus / Qt / LLVM / Chromium）的界面层都直接用日志与工具函数——日志是**横切关注点**（cross-cutting concern），不服从"只能往下调"的字面分层。
- **禁止（Core 业务 / 运行时状态）**：`Paths`（文件系统）、`crypto`、`ThreadPool`、`EventBus`、`process_utils`、`safe_tmp`，以及 `storage`/`net`/`playback`/`app` 的**业务/运行时状态查询**（即带 `instance()` 的有状态 singleton——如 `SleepTimer`/`OnlineState`/`TikTokRegion`）。UI 不得直查运行时状态，应经视图模型 / 事件获得数据。**D12-1 已解耦**这 3 个运行时 singleton → App 每帧构建 `DisplayContext`（睡眠定时 + 区域名）推进 `ui.draw()`。

> 由 `scripts/check.sh` §4 grep 门固化：`src/ui/` 出现 Core 业务符号即报警（咨询、不阻断）。Utils / LOG / URLClassifier 不剥离——剥离横切基础设施只给文件改名，不消除任何真实耦合（`DECISIONS_LOG` D11-4）。当前门**绿**（0 命中）。D12-1 后 `src/ui/` 的运行时 singleton 查询（`SleepTimer`/`OnlineState`/`TikTokRegion`）已**归零**——改由 `DisplayContext` 视图模型推入；`URLClassifier`（纯函数）保留。
>
> **ncurses 边界（D12-3a · M1 验收）**：ncurses 收敛在 `ui/`（渲染）+ `theme/`（呈现层）——二者本就是 ncurses 的归属层。`core/` 与 `config/` **零 ncurses 依赖**（`core/win_raii.h` 已归位 `ui/`；`config/ini_config.h` 颜色映射改原生 int）。core 基础设施对终端的操作走原生 termios/ANSI 转义直写 `/dev/tty`、绕过 ncurses（utils/process_utils/terminal 仅注释提及 ncurses）。

> **前端契约（D12-3b · IFrontend）**：`include/panicast/ui/frontend.h` 定义 `class IFrontend`——ncurses-free 抽象契约（26 纯虚：渲染入口 `draw`、输入/弹窗、每帧状态推入 `set_transcript`/`set_lyric_bar_active`/`update_lyric_history`、lyric/scroll/tree 开关与查询、几何），ncurses UI `class UI : public IFrontend` 实现之。契约拥有它所说的 ncurses-free 视图模型类型（`DisplayItem`/`DisplayContext`/`LyricManual`），依赖方向为 `frontend.h`（无 ncurses）← `ui.h`（ncurses）。字幕 Application Service 的 `poll(IFrontend&)` 与 `library_service.h`（App 层 DisplayItem）已改经契约、不再 `#include ui.h` → **modules/ 与 App 层不为前端契约拖 ncurses**。UI 私有渲染辅助（`draw_line`/`draw_status`/`draw_lyric_*`，带 `WINDOW*`）与静态 `is_input_cancelled`（ncurses 输入标记契约）留 UI 具体、不进 `IFrontend`。D12-3c 已让 App 经 `unique_ptr<IFrontend>` 持有 UI（构造点 `make_unique<UI>()` 具名具体实现、余皆经契约）→ **UI 可换（Qt 可后接同一契约）· M1 达成**；App 名具体 UI 仅剩构造点 + 12 处静态 `UI::is_input_cancelled`（输入契约残留，不阻可换）。

## 3. 已有核心抽象（重构须复用，勿另起炉灶）

- **Provider 模式（自注册）**：
  - `IFeedParser` + `ParserRegistry` + `REGISTER_PARSER(XxxParser)` 宏（`include/panicast/parsers/feed_parser.h`）。新增解析器 = 实现接口 + 末尾一行宏，零改 switch。**M2 审计（D13）确认**：feed 形态的解析器已 Provider 化——`RSSParser`(RSS_PODCAST)/`OpmlParser`(OPML)/`YouTubeChannelParser`(YOUTUBE_CHANNEL) 自注册；以下**刻意不经** `IFeedParser`（各自非 feed 形态）：`BilibiliParser`（WBI 签名 arc API + SESSDATA 凭证，静态方法）、`ITunesSearch`（搜索 API 单例）、`parse_m3u`（IPTV 频道表加载器，多 URL + group-title 分组，在 `app_iptv`）、TikTok（yt-dlp flat-playlist，在 `app_run`）、`transcript_parser`（字幕关注点 → `SubtitleParserRegistry`）。`ParserRegistry::create()` 对未注册类型返回 nullptr（派发契约测试见 `tests/test_units.cpp`）；`app_run` 的 `default` 分支直调 `RSSParser::parse` 是刻意的"未知类型尽力猜 RSS"回退。
  - `ISubtitleParser` + `SubtitleParserRegistry`（`include/panicast/subtitle/subtitle_parser.h`），按格式（json/srt/vtt/lrc/twt）注册。
- **远程控制解耦**：`RemoteControlInterface`（`include/panicast/net/remote_protocol.h`）—— App 实现该接口，`RemoteServer`/`RemoteSession` 依赖接口而非 App。可组合、可测。
- **持久化抽象**：`include/panicast/storage/persistence.h` + 各 `*_repo`。
- **基础设施**：`thread_pool`（worker 池）、`EventLog`（UI 右侧日志环形缓冲，`EVENT_LOG` 宏；**不是**发布/订阅总线）、`URLClassifier`、`crypto`（EtM ChaCha20+HMAC token 加密）。

## 4. 领域模型

- `TreeNode`（`include/panicast/core/types.h`）—— **事实上的 Media 对象**：订阅树/收藏/播放列表的统一载体，80+ 字段（类型/URL/缓存/字幕/账号/链接…）。D14 起为收敛目标（逐步以窄 `Media` 替代跨模块传整节点）。
- `TreeNodePtr = std::shared_ptr<TreeNode>` —— **事实上的 MediaID**（运行期），模块间传指针身份；随节点/进程消亡，不跨 DB/网络。
- `MediaID` / `Media`（`include/panicast/domain/media.h`，D4 建、**D14-1 重定 identity**）—— 真正的领域身份：`MediaID` = **逻辑身份（真实绝对源 URL）**，`operator==` 即 URL 相等（跨内存/DB/H/F/远程一致、跨节点销毁与进程重启存活）；`Media{id,title,art_url,is_video}` 为 now-playing 视图。D14-1 完成身份模型（逻辑绝对源 URL）。**D14-2 ✅ 接线**：`PlaybackService::now_playing()` 暴露 canonical now-playing Media（从 `playback_node_` 派生），remote 快照为首消费者（替 `current_url` 播放路径）；`PlaybackTrackChanged` 保持 {node,mode,has_video}（字幕订阅者需 node，独立通道）。**D14-3 ✅ 读侧收敛**：TUI 3 个 `current_url`（播放路径）读点（tree_renderer 高亮 / status_bar / info_panel）改读 canonical 源 URL（经 `DisplayContext.now_playing_url`，App 每帧从 `now_playing()` 灌入）；修缓存项树高亮 bug（`node->url`=源 URL ≠ `current_url`=本地路径，此前永不点亮）；lyric 逐轨触发键保留 current_url（变化检测器、契约方法、行为不变）。**D14-4 ✅ 持久侧收敛**：progress + player_state 键由播放路径（mpv current_url）改为 canonical 源 URL（save 经 `now_playing().id.url()`、read 经 orig_url），与 history（早 orig_url 键）对齐；SCHEMA 47→48 一次性无损迁移（media_cache 反向 map re-key path→source，仅仍缓存项可迁）。**D14-4b ✅ save 守卫对齐 read**：save_progress 加 `classify!=RADIO_STREAM` 守卫——电台/播客/本地音频（read 本就不续播）不再存死数据，零续播回归。**D14-5 ✅ Favourites LINK 收敛**：sync_link_node_status 的 LINK→target 匹配由指针相等改为 URL 身份（`link_target_url`，OR 指针快路径无回归）——LINK 机制最后一块指针身份残留；至此 expand(按 URL 解析)/sync(按 URL 匹配)/persistence(存 URL) 统一 URL 身份，`linked_node` 纯运行期 weak_ptr 缓存（AUDIT shared_ptr 所有权债早已修）。**D14 收官**：now-playing 身份统一以真实绝对源 URL（MediaID）为准，跨内存/读侧/持久侧/收藏 LINK 一致。**D15 ✅ 渲染契约去冗余通道**（M1 UI 解耦收尾，非 D14）：D14-3 把 now-playing url 收进 DisplayContext 后 info_panel 仍另持 `playback_node` 域指针取 title/url（同一身份两通道）；D15 补 `now_playing_title` 入 dctx + 拔 `IFrontend::draw` 的 `playback_node` 形参，渲染契约对 now-playing 身份只走视图模型一通道。语义等价（`now_playing()`≡`playback_node` 经 `media_from_node`）。契约仍经 `selected_node`/`DisplayItem.node` 持 TreeNode（非完整脱耦）。
- 枚举：`NodeType`、`AppMode`（RADIO/PODCAST/FAVOURITE/HISTORY/ONLINE/ACCOUNT/BILIBILI/TIKTOK/IPTV）、`PlayMode`、`MediaType`（9 类显示分类）、`URLType`、`TranscriptStatus`、`AppState`。

## 5. 线程模型（三线程）

| 线程 | 职责 |
|---|---|
| **UI**（主） | ncurses 单线程，非线程安全；所有 App/渲染/输入在此 |
| **worker 池**（`thread_pool`，10） | 网络/解析/下载等异步任务 |
| **mpv event** | libmpv 事件回调（播放状态/结束） |

**三个跨线程穿越点**（见 `DECISIONS_LOG` N01/N02）：
1. `RemoteCommandBus`（写路径）—— server 线程 `push` 命令，UI 帧每轮 `drain` 派发。点对点队列。
2. `RemoteStateSnapshot`（读路径）—— UI 帧在 `remote_state_mtx_` 下构建快照，server 线程读副本。**刻意避免** server 线程交叉锁 `tree_mutex`/`playlist_mutex_`。
3. `pending_select_` + 散落回调（worker→UI 的异步通知）—— **竞态根因**（AUDIT P1-4/P1-5/P1-8）。这是后续 `EventBus` 要替换的目标（重构计划 Tier 2 / R1.1–R1.2）。

> 注：`EventBus` 目前**尚不存在**；`RemoteCommandBus` 是点对点，`EventLog` 是日志缓冲，二者都不是通用 pub/sub。

## 6. 已知技术债（重构输入，详见 AUDIT_REPORT）

- **上帝对象/文件**：`App`（`app.h` 553 行声明；D43 抽下载执行引擎 → `DownloadService`：pending_downloads_/start_one/ytdlp/pump + 校验簇 capture_exec/probe/verify 离 App，690→553；剩 pool_/remote_/frontend_/player 等成员簇 + 余下 god-object 方法待续抽）、`app_run.cpp`(589，run() loop 已收敛为 flat 编排骨架：D35 startup/shutdown bookend、D37 tree-locked 显示构建、D38 FrameCtx+prepare_frame（Method Object）、D39 draw_frame、D40 check_exit_requests/drain_frame_events；仅剩 resize/scroll/input/watchdog 小内联段)、`mpv_controller.cpp`(687，D18-D20+D34 机械抽 wrapper/metadata/iptv/单条播放；D36 设计层抽 event_loop codec-info 块、D41 抽 END_FILE 分支 handle_end_file_/handle_playback_error_，event_loop 回归纯派发骨架；D46 抽 update_state IPTV 运行诊断 → mpv_iptv.cpp detect_iptv_states_、D47 抽 initialize() [mpv] 选项簇 → 新 mpv_init.cpp apply_mpv_options_、D48 抽 play_list/play_list_from → mpv_play.cpp（play_* 派发簇收官，918→687）；initialize/update_state 主体为单关注点 setup/polling，再抽即过度分解)、`app_input.cpp`(882，硬编码 `switch(ch)`；D7 迁无状态键 play/pause/volume/nav、D42 迁 mode-switch 键簇 R/P/F/H/O/Y/B/I → SwitchModeAction，复杂有状态流 play/搜索/下载/标记留 switch)。~~`ini_config.h`~~（原 1087 行 god-header，D24-D31 已迁 72 个方法出体，现 304 行纯声明头，**基本驯服**）。
- **竞态**：`pending_select_` + 跨线程裸回调（P1-4 向量竞态、P1-5 两锁一树、P1-8 `~App` 缺失致 UAF）。
- **SQL 卫生**：多处忽略 `exec_sql` 返回、Bilibili 一处 `fmt::format` 拼接（注入面）、`atoll` 解 TEXT 时间戳恒为 0、`StmtRAII` 预编译缓存造好但零使用。
- **热键**：无状态命令 + 模式切换键（play/pause/volume/nav + 8 mode-switch）经 `Keymap`（D7）→ 可经 INI `[keys]` 重绑（D44，D7「热键可自定义」完整目标）；复杂有状态流（play 'l'/搜索/下载/标记/M/N/b/T）深依赖 mode+选中上下文，仍留 `app_input.cpp handle_input` 的 `switch(ch)`（D42 边界）。
- **代理**：`apply_network_proxy(CURL*, url, platform)`（`net/network.cpp`）是 `IProxyManager::resolveProxy`（规则链 platform→domain→global→direct，D2）的 curl 包装——所有 curl 调用点经它；yt-dlp 经 `YtdlpRunner::run(source_url)`→`resolveProxy(source_url)`（D45，替旧的空 URL）；mpv 播放仍直连。生产期种子规则：`*.bilibili.com→direct`（INI `[network] bilibili_direct` 门控，D45——bilibili 国内站点、设全局代理时仍直连）；youtube/googlevideo 走全局 fall-through。
- **Windows 构建**：posix_spawn/HOME 路径致坏（P2-X）。

## 7. 构建/版本约束

- `CMakeLists.txt` 是**显式源列表**（非 glob）：新增 `.cpp` 必须手动加入 `add_executable(panicast ...)`（`:242` 起）。
- **版本单一事实来源**：5 处须同步——`CMakeLists.txt` / `vcpkg.json` / `include/panicast/core/constants.h` / `man/panicast.1` / `README.md`。
- 数据迁移：sqlite3 WAL + 幂等 `add_column_if_missing` + 自有事务 + `sqlite3_close_v2`；`SCHEMA_VERSION` 驱动（现 47）。用户数据跨版本必须无损。
