# panicast 功能清单与缺口跟踪

> **生成日期**：2026-09-01 · 盘点人：Claude Code
> **用途**：功能全量盘点 + 缺口持续跟踪。每个缺口有稳定 ID，可在「状态 / 反馈」列持续更新。
>
> **状态取值**：`待确认`（我没读全/需你确认）· `待实现` · `进行中` · `已完成` · `不采用`
> **优先级**：`P0`（高价值低成本，建议立即）· `P1`（重要）· `P2`（增强/可选）· `P3`（低/待定）· `债`（技术债，非功能）

---

## 一、项目概览

- **定位**：基于 ncurses + libmpv 的终端播客/电台播放器（C++17）。
- **线程模型**：UI（主）/ worker 池（10）/ mpv 事件线程，三处跨线程穿越点（RemoteCommandBus / RemoteStateSnapshot / pending_select_）。
- **架构状态**：已完成 M1（UI 解耦，`IFrontend` 契约，UI 可换）、M2（Media 收敛 + Provider 化）；god-object 已抽取 4 个 Application Service（Playback/Subtitle/Search/Library/Download）+ 多处机械拆分。仍有 App / DatabaseManager / app_input.cpp 等残余 god-object（见技术债）。
- **平台**：Linux（推荐）/ macOS。

---

## 二、功能清单（按域）

### 2.1 媒体源 · 9 种模式（`AppMode`）

| 模式 | 键 | 内容 |
|---|---|---|
| 电台 | `R` | TuneIn 电台目录 + 自定义流 URL |
| 播客 | `P` | RSS/Atom 订阅、OPML 导入、YouTube 频道（cookies）、Bilibili UP 主 |
| 收藏夹 | `F` | 收藏节点 + 本地文件夹（`a` 递归扫入）+ `online_root` LINK |
| 历史 | `H` | 播放历史（按 orig_url 键去重） |
| 在线 | `O` | iTunes Podcasts 搜索（`/`），`b` 切搜索区域 |
| 帐号 | `Y` | Google YouTube：OAuth 扫码多帐号，订阅/观看历史/搜索 |
| B 站 | `B` | Bilibili：QR 扫码登录，WBI 签名 API（关注/UP 视频/历史/搜索） |
| TikTok | `T` | TikTok/抖音（13 区域，`b` 循环），`a` 加 @user/视频，`/` 开 @user/#tag/URL |
| IPTV | `I` | iptv-org m3u 目录（All/Region/Country/Category/Language/Custom）+ off-air 诊断 |

媒体格式：~70 种扩展（`is_media_extension`），含 DSD/AC3/DTS/FLAC 等。

### 2.2 播放控制

- 播放/暂停、音量 ±5、速度 0.2–3.0（`[`/`]`/`\`）、断点续播（progress + player_state）
- 播放模式 repeat/shuffle/cycle（`:` 命令或全文前缀，持久化 `[playback] mode`）
- 隐式播放列表 = 当前节目 peers（兄弟节点），`C` 清空
- `:` 命令窗口 → mpv 热键：缩放/全屏/宽高比/反交错；字幕（F/G 大小、z/Z 同步、r/R 位置、v 显隐、j/J 换轨）；音轨 `#`、静音 `m`；OSD `o/O`、统计 `i/I`；A-B 循环 `l`；截图 `s/S`；视频 EQ 1–8（对比/亮度/伽马/饱和度）
- 睡眠定时器（CLI `-t` / remote `sleep`；状态栏显示剩余）
- 本地文件 mpv 直放，缓存项续播

### 2.3 UI / 主题

- 左树 + 右 INFO/LOG 三栏 + 底部 LYRIC 栏；状态栏含播放/网络/缓冲
- 15 套主题（`Ctrl+L`，`[display] theme_index` 持久化）
- 图标 ASCII/Emoji（`U`）、树线条（`T`）、滚动模式（`S`）、排序（`o`）
- 状态栏配色 5 模式（RAINBOW/RANDOM/TIME_BRIGHTNESS/FIXED/CUSTOM）
- 鼠标（左键选择/展开、滚轮翻页）、帮助（`?`）、`N` 跳当前播放、`Ctrl+Y` 复制 URL

### 2.4 字幕 / 转写

- 在线：RSS `<podcast:transcript>`（📜）、YouTube 软字幕、B 站 CC
- 本地：SRT 优先（统一 `find_local_subtitle` 查下载目录+本地旁）
- whisper.cpp 离线转写（F 模式 `L` → `<文件名>.srt`，CPU 感知并发）
- 实时 ASR（建设中，`:asr` 强制绕过）
- 解析器：json/srt/vtt/lrc/twt（`ISubtitleParser` + Registry）
- `L` 键统一「本地优先」源解析（embedded > local SRT > online > ASR）

### 2.5 数据管理

- 收藏（批量 `f`）、历史（容量/天数可配）、搜索缓存
- OPML 导入/导出（`-i`/`-e`）
- 下载：yt-dlp + curl 双路径（retry/resume/416/range），10 并发槽，校验簇
- SQLite 持久化（WAL + 幂等迁移，SCHEMA 48）

### 2.6 网络 / 远程控制

- 代理（`Ctrl+N`，实时生效；`IProxyManager` 规则链 platform→domain→global→direct；bilibili→直连种子）
- Cookies（`Ctrl+B` 上下文感知：YouTube/Bilibili/TikTok/抖音）
- 远程控制：TCP PRP + WebSocket + UDP 发现 + Android APK（Kotlin/Compose）
  - 命令面：播放/暂停/seek/音量/速度/播放模式/睡眠/切模式/导航/搜索/标记/收藏/编辑/下载/刷新/字幕/ASR
  - PIN 配对（动态 + 通用 6696），状态快照
- `:pin`、`:secret`（OAuth client_secret 导入）

### 2.7 配置系统（15 段 INI）

`[display] [colors] [network] [storage] [playback] [mpv] [youtube] [bilibili] [tiktok] [transcription] [statusbar_color] [search] [remote] [keys]`（+ `[youtube]` 的 cookies/player_client/js_runtime/play_format 等）。热键可重绑（`[keys]`，13 个无状态+模式键）。

### 2.8 CLI / 跨平台

`-a/-i/-e/-t/--purge/--quiet/--vid/--vo/--ao/-h/-v`；Linux（推荐）/ macOS。

---

## 三、缺口清单（跟踪表）

> 反馈列留给后续追踪。**优先级**见下；**状态**请你在沟通中更新。

### 3.1 播放体验

| ID | 缺口 | 优先级 | 状态 | 反馈/备注 |
|---|---|---|---|---|
| G01 | TUI 无快进/快退（seek）：`handle_input` 未处理 `←/→`，`:` 也未映射 seek；只能 remote 或手敲 `:mpv seek` | P0 | 待实现 | |
| G02 | 睡眠定时器无 TUI 入口：只能 CLI `-t` 或 remote，TUI 内无设置/取消键 | P0 | 待实现 | |
| G03 | 无显式播放队列（「添加到队列/下一首」）：只有隐式 peers 列表 + `C` 清空 | P1 | 待实现 | |
| G04 | 无音频 EQ / ReplayGain / gapless / crossfade：`:` 只有视频 EQ（对比/亮度/伽马/饱和度） | P1 | 待实现 | |

### 3.2 内容 / 订阅

| ID | 缺口 | 优先级 | 状态 | 反馈/备注 |
|---|---|---|---|---|
| G05 | 无自动下载新集 / 定时下载：只能手动 `D` | P1 | 待实现 | |
| G06 | 无新集通知 / 未读计数（普通播客侧） | P2 | 待实现 | |
| G07 | 无收听统计（播放时长/Top/总时长）：`stats` 表已存在但只存 `active_google_account_id`，未用于统计 | P1 | 待实现 | |

### 3.3 本地媒体 / 字幕

| ID | 缺口 | 优先级 | 状态 | 反馈/备注 |
|---|---|---|---|---|
| G08 | 本地媒体库无递归自动扫描/监控：F 模式 `a` 是手动扫一个文件夹 | P2 | 待实现 | |
| G09 | 本地 `.lrc` 歌词自动匹配：解析器支持 lrc，但 `find_local_subtitle` 主要找 SRT，需确认 .lrc 是否自动加载 | P3 | 待确认 | |

### 3.4 配置 / 易用性

| ID | 缺口 | 优先级 | 状态 | 反馈/备注 |
|---|---|---|---|---|
| G10 | 无 TUI 内设置页：15 段 INI 全靠手改文件，TUI 内只有 proxy（`Ctrl+N`）和 cookies（`Ctrl+B`）两个对话框 | P2 | 待实现 | |
| G11 | 无 MPRIS / 系统媒体键集成（Linux 桌面播放/暂停/下一首键） | P2 | 待实现 | |

### 3.5 工程 / 健壮性

| ID | 缺口 | 优先级 | 状态 | 反馈/备注 |
|---|---|---|---|---|
| G12 | 测试覆盖薄：50 个单测集中在纯函数，下载/播放/远程/字幕/ASR 靠手动冒烟；缺端到端自动化 | P1 | 待实现 | |

### 3.6 数据库 / 存储专项

| ID | 缺口 | 优先级 | 状态 | 反馈/备注 |
|---|---|---|---|---|
| G14 | 无数据库备份/导出/恢复：OPML 只备份订阅，历史/收藏/进度/账号/下载状态无法备份迁移 | P1 | 待实现 | |
| G15 | 无 DB 维护/空间管理：无 VACUUM、无下载目录清理、media_cache 无容量上限 | P2 | 待实现 | |
| G16 | SQL 卫生债：多处忽略 `exec_sql` 返回值；Bilibili 一处 `fmt::format` 拼接（注入面）；`atoll` 解 TEXT 时间戳恒 0 | 债 | 待实现 | |
| G17 | Repository 化不彻底：`DatabaseManager` 仍是 god-singleton，raw SQL 散落；`StmtRAII` 预编译包装器已定义但**零使用**（死代码） | 债 | 待实现 | |

---

## 四、数据库 / 存储层盘点

### 4.1 表清单（19 张，SCHEMA_VERSION 48）

| 表 | 用途 | 备注 |
|---|---|---|
| `tree_nodes` | 订阅树（radio + podcast roots，递归 parent_id） | F38 起统一 |
| `progress` | 续播位置 | D14-4 键改为 canonical 源 URL |
| `history` | 播放历史（url UNIQUE） | 容量/天数清理 |
| `stats` | 键值存储 | **仅存 `active_google_account_id`**，未用于统计 |
| `media_cache` | 下载缓存状态（0/1/2） | single source of truth |
| `search_cache` | 搜索缓存（itunes/youtube/bilibili，per-account） | |
| `player_state` | 播放器状态快照（音量/速度/位置/模式） | |
| `podcast_cache` | 播客元数据缓存 | |
| `episode_cache` | 集列表缓存（含 has_subtitle/subtitle_url/has_asr_srt/asr_srt_path） | |
| `favourites` | 收藏 | |
| `youtube_cache` | YouTube 频道缓存（per-account） | |
| `bilibili_accounts` | B 站账号（SESSDATA 等） | |
| `bilibili_up_cache` | B 站 UP 主头像缓存 | |
| `removed_defaults` | 已删除的内置默认播客 | |
| `tiktok_accounts` | TikTok/Douyin 订阅 | |
| `accounts` | Google 账号（token **加密存储**） | crypto EtM ChaCha20+HMAC |
| `youtube_subscriptions` | YouTube 订阅列表 | |
| `youtube_history` | YouTube 观看历史（source 区分 pulled/local） | |
| `account_sync_state` | 同步游标 | |

### 4.2 存储层健壮性现状

- ✅ WAL + `synchronous=NORMAL` + `busy_timeout=5000` + `foreign_keys=ON`；DB 文件 `chmod 0600`
- ✅ 幂等迁移：`add_column_if_missing` + `PRAGMA user_version` 驱动；历史迁移含去重/建唯一索引/backfill/re-key
- ✅ 单连接（`DatabaseManager` 共享，避免 F38 前的双连接竞态）
- ⚠️ Repository 抽象未落实：DESIGN/ARCHITECTURE 目标「业务不碰裸 sqlite」，但实际 `DatabaseManager` 是巨型单例，各 `*_repo.cpp`（history_repo/tree_repo/feed_cache_repo/account_repo/player_state_repo）是部分迁移产物

---

## 五、已文档化的已知限制（**不算**遗漏）

| 项 | 说明 |
|---|---|
| TikTok 搜索 | 无匿名关键词搜索、抖音无「展开用户全部视频」（yt-dlp 上游无提取器） |
| 硬字幕 | burned-in 字幕无法缩放/移位 |
| 封面渲染 | `art_url` 不渲染（终端无法内联图片，留待 sixel/kitty 或 Android 远程） |
| YouTube 观看记录 | 无法回写官方 API（InnerTube 非官方，本地侧记录） |
| 实时 ASR | 标「建设中」 |

---

## 六、跟踪约定

- 每个缺口 ID 稳定不变；后续在「反馈/备注」列或直接告诉我「Gxx 已做 / 改优先级 / 拆成两个」。
- 完成时更新状态 → `已完成`，并在 CHANGELOG / ROADMAP 里同步。
- 新发现的缺口追加到对应分节，编号顺延。
