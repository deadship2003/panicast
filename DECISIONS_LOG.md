
## S01 — TUI 启动时延：接管请求体畸形 JSON 修复 + 握手/停止时延收敛（2026-09-07）

**Context:** 用户报告 `panicast` 启动 5s 以上。日志实测分解（守护在跑场景，总计 ~10s）：LMS POST 后 drain 烧满 3s SO_RCVTIMEO（cometd 连接 keep-alive 永不关闭）；`poll(3000)` 又超时 3s；同步 `systemctl --user stop` 阻塞 3.7s。深挖发现 poll 超时的根因是**零掉线接管自始未生效**：`post_lms_handover` 拼的请求体第二个数组元素缺开括号（`}},"id":"999"}]`），守护端 nlohmann parse 静默丢弃（错误不记日志）→ handover 派发从未执行。停止 3.7s 的根因是 `jam_loop_` 每轮 `sleep_for(5s)`（threshold/10），`stop()` 的 join 只能等它睡醒。

**Decision:** 三处修复而非重构：① 请求体补全为合法 JSON（保持字段形状不变，只补 `{`）；② drain 改为按 `Content-Length` 读完整响应即关闭（250ms 兜底），poll 上限 3000→1000ms；③ jam 看门狗睡眠改条件变量定时等待、`stop()` notify 立即唤醒。**取舍：保留同步 `systemctl stop`**（曾评估 `--no-block` + 短等待以进一步压时延，拒绝——TUI 在旧守护进程未死时启动引擎违反 LIF-001 单实例所有权红线；jam cv 修复后同步停止仅 ~44ms，无需放松语义）。

**关键点:**
- 守护端可诊断性补齐：cometd 请求体 parse 失败记日志（此前静默 `[]`，正是畸形体隐藏一整个 N10.5 时代的原因）；`send_handover_fds` 的 `listen_fd_<0` 静默早退补日志。
- drain 的存在理由（"给传输线程一拍"）由 poll 承担，读响应只为不 RST；Content-Length 语义与守护响应实测一致（416 字节单包）。

**Verification:** ctest 50/50 绿；畸形体负测（守护回 `[]` + parse FAILED 日志 + 无传输）与修复体正测（POST→SCM_RIGHTS 2ms）双证；`systemctl --user stop` 0.044s/0.037s 且 "Player state saved" 落盘；启动估算 ~2.1s（原 ~10s）。

**Followups:** 本会话经 `./bin/panicast service install` 将 unit ExecStart 指向 `bin/panicast`（N10.3 设计：ExecStart=运行中二进制）——用户 `sudo make install` 后需从安装路径再跑一次 `service install` 以指回 `/usr/local/bin`；mpv AO=null 出现在用户真实终端日志（16:39/17:44/17:46 三轮），疑 WSLg pulse 链路问题，与本时延修复独立，待查。

---


## L01 — LIF 准则库合规精化：构建三件套 + 服务 CLI 标准化 + 守护探测收敛（2026-09-07）

**Context:** 用户引入全局 LIF 准则库（LIF-001 服务生命周期 CLI / LIF-002 --debug / LIF-006 构建三件套 / LIF-007 英文注释），要求 panicast 对齐。审计现状：裸动词服务 CLI 缺 install/uninstall/--json/scope/统一退出码；status 以 pidfile 为真相源（红线要求 init 为真相源）；`panicast log` 为内置聚合（rev3 已移出标准集）；build.sh 已被改名掉且无 Makefile；setup.sh 越权编译+安装；17 文件 64 行中文注释存量。

**Decision:** 五阶段落地（每阶段独立 commit），用户确认四项关键取舍：① `service` 前缀为规范入口、裸动词保留为别名；② `--system` 保留旗标但显式拒绝（退出码 4，用户域单一设计 N10.3 维持）；③ 移除 `log` 动词（用 `tail -F` 原生命令）；④ 中文注释专项批量全转。

**关键点:**
- **守护标准化（用户点名收敛，拒绝过度设计）**：迭代期的补偿层退役——`lms_port_in_use()` 预探测删除，改为无头守护 LMS bind 失败即致命（`headless_fatal_` → exit 1），systemd Restart 自愈；unit 注入 `StartLimitIntervalSec=60/Burst=5` 防永久占用端口时 3s 死循环。真相源 = `systemctl --user show`；pidfile 降级为裸跑线索（`--daemon` 裸跑互斥 + TUI 接管的 SIGTERM 兜底 + status 异常标注）；pre-N10.3 系统 unit 停止路径删除。
- **构建三件套**：Makefile 9 标准 target 封装 CMake/Ninja，产物统一 `./bin/`（`CMAKE_RUNTIME_OUTPUT_DIRECTORY`）；setup.sh 瘦身为纯环境（含 `--check` 预检 + Debian 13 `libncurses-dev` 包名统一）；build.sh 薄编排保留内存感知并行度 + git 新鲜度防护。install 两层分工落地：`make install`=文件层（cmake --install），`panicast service install`=注册层（ensure_user_unit 暴露为动词）。
- **服务 CLI**：`panicast service <subcmd> [--json] [--user|--system]`；status 固定 11 字段从 systemctl show 取数；退出码 0/1/2/3/4；start 幂等（已运行 → 提示 + exit 0）。
- **--debug（LIF-002）**：`run_daemon(debug)` 复用引擎路径——与托管实例互斥、从 unit 对齐 Workdir/Environment、Logger tee 到 stderr、SIGINT/SIGTERM 同一干净退出路径。
- **注释**：64 行中文注释全转英文（douyin 接口文档、「本地字幕文件优先」策略名统一译 "local subtitle first"）；豁免：INI 模板内注释、CI 表单/发布说明文案（用户向 UI 文案）。

**Verification:** `setup.sh --check` 绿；`make build/test` 绿（ctest 50/50、0-warning）；服务动词双输出 + 退出码实测（status/install/start 幂等/stop/restart/--system=4/未知参数=1）；`--debug` 实测（互斥拒绝 + TERM 干净退出 + pidfile 清理 + 控制台日志镜像）；守护重启换新 pid 正常。待用户端：真实终端 TUI↔daemon 交接冒烟、`sudo make install` 部署新二进制。

**Followups:** 本工具 shell 内 mpv AO=null（沙箱无 pulse 访问）为环境现象非代码缺陷，真实终端不受影响；`make fmt` 会重排全量文件（用户自行决定时机）；docs/BUILD.md、README、man 已同步三件套与新 CLI 面。

---

## D48 — play_list/play_list_from → mpv_play.cpp（#77 · god-object 抽取收官）

**Context:** #77 续。mpv_controller.cpp 的 play_* 派发簇：D34 已迁单条播放（play_audio/play_video/play）入 mpv_play.cpp；剩 play_list（m3u 临时文件 + keep-open + loadlist）与 play_list_from（+playlist-pos 起始）两块列表派发仍留 mpv_controller.cpp。ROADMAP 既标「play_* core」为候选。

**Decision:** 将 play_list/play_list_from 逐字迁入既有 mpv_play.cpp（播放派发关注点 TU）。

**关键点:**
- 归宿 mpv_play.cpp 非 mpv_controller.cpp：与 play_audio/play_video/play 同属「派发到 mpv」关注点 → play_* 簇聚一处。物理位移、行为逐字等价（仅 .cpp 改变，声明仍留 mpv_controller.h）。
- 无新依赖：mpv_play.cpp 既有 include 已含 `<fstream>`（std::ofstream）、safe_tmp（SafeTmpFile）、logger（LOG）、fmt；int64_t 经 mpv_controller.h→client.h 可用。两函数均不在 mpv_controller.cpp 内被调用（调用点在 App/PlaybackService 层），故删除零回归。
- 收官边界：initialize()/update_state() 主体为单一关注点（setup/polling 骨架），已非「成员组」可抽——再拆即过度分解。#77 mpv god-object 抽取就此告一段落。

**Verification:** 0-warning、ctest 46/46、pty 冒烟 exit 0 + clean endin。播放行为待用户端测。

**Followups:** mpv_controller.cpp 790→687（D46+D47+D48 累计 918→687，−231）。app.h 553 行仍待续（其他成员簇，独立任务）。

---

## D47 — initialize() [mpv] 选项簇 → mpv_init.cpp（#77 · god-object 抽取）

**Context:** #77 续。initialize() 202 行是文件最大函数；其 [mpv] 选项应用块（~85 行）是离散关注点「应用用户可配选项」，ROADMAP 既标 initialize 为目标。

**Decision:** 抽 `apply_mpv_options_()` 迁新 `mpv_init.cpp`（init 关注点兄弟 TU）。

**关键点:**
- 新 TU mpv_init.cpp：无既有 init 兄弟文件（mpv_commands=控制 wrapper、mpv_play=单条派发均非 init）；选项簇够大（~85 行）值得独立 TU。声明留 mpv_controller.h（成员函数，读 ctx_/cli_*_override_）。
- 单一关注点 + 无早返回：选项块是顺序 set_option、无控制流外逸 → 调一次于 mpv_initialize 前，行为与内联块逐字等价。
- initialize() 收敛为可读序列（create→探测 yt-dlp→固定 flag→apply_mpv_options_→initialize→post-init→启线程）。
- 边界：固定行为 flag（idle/geometry/ontop/terminal/input-bindings/ytdl）留 initialize（核心行为常量，非「用户可配选项」）；mpv_initialize 后的 post-init（log 订阅/VO-AO 日志/stderr 重定向/启线程）留 initialize（一次性初始化）。再细分即过度分解。

**Verification:** 0-warning、ctest 46/46、pty 冒烟 exit 0 + clean endin。

**Followups:** #77 mpv god-object 抽取告一段落——initialize()/update_state() 主体为单关注点（setup/polling），再抽过度分解。app.h 553 仍待续（其他成员簇）。#77 评估完成。

---

## D46 — update_state IPTV 运行诊断 → mpv_iptv.cpp（#77 · god-object 抽取）

**Context:** #77 评估 mpv_controller.cpp 剩余簇。update_state() 内 IPTV 诊断块（~50 行，off-air/audio-only/slow 三态判定）是 ARCHITECTURE 既标的「update_state IPTV 块」目标——离散关注点、含计时/one-shot 逻辑，适合 Extract Method。

**Decision:** 抽 `detect_iptv_states_(has_media_now,has_audio,has_video_track,idle,cache_speed,buf_pct,buf_dur)`，迁 mpv_iptv.cpp（与既有 IPTV 方法同处、读相同私有标志）。

**关键点:**
- 归宿 mpv_iptv.cpp 非 mpv_controller.cpp：与 classify_iptv_load_error_/iptv_message_for_error_/set_iptv_context/reset_iptv_detection_ 同属 IPTV 关注点、共享 one-shot 标志 → 内聚类聚一处（D20 模式）。
- 入参传派生值（bool×3 + idle + cache_speed + buf_pct + buf_dur）非重读属性：update_state 已读这些给 state_，detect 复用快照（避免重复 mpv_get_property）。
- 守卫内移：原 `if(iptv_context_&&...)` 块体迁入方法、首行 `if(!(...)) return;`——调用点无条件调一次，语义等价。
- 逐字搬迁：计时/阈值/one-shot 更新 1:1；event-loop 线程独占故成员无锁（原注释保留）。

**Verification:** 0-warning、ctest 46/46、pty 冒烟 exit 0 + clean endin。IPTV 诊断行为待用户端测。

**Followups:** initialize() 选项簇（D47）。

---

## D45 — yt-dlp 代理 URL 感知 + bilibili 直连种子规则（#75 · Connectivity 收尾）

**Context:** D2 立 IProxyManager 规则链、D3 让 curl 全经 `apply_network_proxy(url,platform)`，但留两缺口：① `YtdlpRunner::run` 内 `resolveProxy("")` 传空 URL → 域名规则永不命中（host 为空）；② 无生产期种子规则（youtube/bilibili/googlevideo 仅注释示例）。#75 收尾。

**Decision:** ① run() 加 `source_url` 形参 → `resolveProxy(source_url)`，6 调用点传源 URL；② network.cpp 全局源初始化注册 `*.bilibili.com→direct` 域名规则（INI `bilibili_direct` 门控默认 true）。

**关键点:**
- yt-dlp URL 感知：source_url 让域名规则按下载 host 命中（如 `*.bilibili.com→direct`）。默认 "" 保旧行为。6 调用点作用域内均有源 URL（download/tiktok/playback/channel_parser），机械传参、行为等价（无规则时 resolveProxy(url)≡resolveProxy("")≡全局）。
- bilibili 用域名规则非平台规则：一条 `*.bilibili.com` 同时覆盖 curl（api.bilibili.com；bilibili_api 已传 platform="bilibili"，platform 未命中后域名规则兜底）+ yt-dlp（www.bilibili.com）。比平台规则鲁棒（不依赖调用点传 platform）。
- youtube/googlevideo 不加规则：它们 fall-through 到全局代理（设了用、没设直连）。规则存固定 ProxyConfig、不能表达「用全局」，故加显式规则是冗余 no-op。仅 bilibili 需显式规则因其语义是「覆盖全局为直连」。
- 行为变更：设了全局代理的用户，bilibili 改走直连（修方向非回归——bilibili 经境外代理本多失败/变慢）。无代理用户零影响；INI `bilibili_direct = false` 可关。下载/解析代理路由不经冒烟（无网络），用户端测。

**Verification:** 0-warning、ctest 45/45（+1 `DomainRuleForcesDirectOverGlobal`）、pty 冒烟 exit 0 + clean endin。

**Followups:** #75 Connectivity 收尾。规则链 API（setPlatform/setDomain）+ IniConfig::get(section,key) 已就位，后续若需更多 per-platform 专用代理（如 youtube 专用代理）可在 network.cpp 扩展种子或加 INI `[proxy]` 段。

---

## D44 — INI [keys] 热键重绑（#71 收尾 · D7 完整目标）

**Context:** D7 立 Keymap/Action + D42 迁 mode-switch 键，但 Keymap 仍是代码默认绑定、未接 INI——「热键可自定义」(ROADMAP D7) 未达成，#71 剩最后一块。complex stateful 流已在 D42 判定留 switch，本步只接 Keymap 已有绑定的重绑通道。

**Decision:** build_keymap 改「默认表 + `IniConfig::get("keys", name, default_token)` 覆盖」；`Keymap::parse_token` 解析 token（键名/单字符/数字，多键逗号分隔）。

**关键点:**
- 默认与覆盖同路径：每项 token = `ini.get("keys", name, default_token)`；INI 无 [keys] → 用 default → 行为零变化（现有用户无感）。
- 可重绑范围 = Keymap 已有的 13 绑定（无状态 + mode-switch）。复杂有状态流留 switch（D42 边界）——它们深依赖上下文、包 Action 无重绑收益。
- case 敏感：parse_token 单字符原样返回（'r'≠'R'），匹配 wget_wch「返回即所按」。故默认 'R' 需 Shift+R、'p' 按 p 触发——与 D42 一致，未改既有大小写语义。
- hint 随 Action：重绑只换触发键，切模式后提示不变（提示描述模式、非键）——比 D42「键特定」更干净，且编程式 switch_mode 不经此路径故无冲突。
- parse_token 作 Keymap 静态方法：归属自然 + 可单测（inline 免额外链接）。3 gtest 锁契约。

**Verification:** 0-warning、ctest 44/44（+3 KeymapParseToken）、pty 冒烟 exit 0 + clean endin。重绑行为待用户端测。

**Followups:** #71 至此完整收尾（D7 完整目标达成）。复杂有状态流（play/搜索/下载/标记）若要可重绑，需先 Action 化（每流独立 Action 类型），属更大改动、暂不做。

---

## D43 — app.h 声明 god-object → DownloadService（#73）

**Context:** #73 = 拆 App god-object（app.h 690 行声明）。已抽 3 个 Application Service（PlaybackService D8/LibraryService D10-4/SubtitleService D10-1）。下载簇是自包含的执行单元（library_/pool_/subtitle_ + 单例，无 mode/frontend/running/player 回读），适合抽第四个 Service。但 download_node 依赖 App 共享 mark 方法。

**Decision:** 抽 `DownloadService` 持下载**执行引擎**（pending_downloads_/start_one/ytdlp/pump + 校验簇）。App 留 `download_node` 薄编排器。option B 边界（持协作者引用、不持 App& 反引），匹配已立 Service 模式。

**关键点:**
- 执行 vs 编排分离：download_node（gather marks → enqueue/pump → clear → save_cache）属 App 编排（用 App mark 方法）；start_one/ytdlp/pump 属执行，移 Service。mark 方法（collect_playable_marked_current/clear_marks_current）导航/input/订阅也用 → 留 App。
- 校验簇作文件局部自由函数：capture_exec/probe_media_duration/verify_downloaded_file/VerifyResult 是纯函数（无成员态）、仅下载引擎消费 → 不进 class 接口，放 download_service.cpp 匿名 namespace。capture_exec 之前 App static 内联、仅 probe 调，随唯一消费者迁出。
- 持引用不持 App&：DownloadService(library_, pool_, subtitle_) 反引会造 Service↔App 环依赖（气味），同已立 Service 一律持窄引用。
- body 逐字搬迁：`App::`→`DownloadService::`，成员名同（library_/pool_/subtitle_/pending_downloads_）→ 行为等价。lambda 捕 `[this,...]` 中 this 现=DownloadService，调 ytdlp_download/verify_downloaded_file 解析正确（成员 / 文件局部自由函数）。
- 风险与限：下载路径不经冒烟（无网络）；机械搬迁 + 编译/ctest/冒烟门绿，下载行为正确性以用户端测为准（"用户末尾统一测"）。

**Verification:** 0-warning、ctest 41/41、pty 冒烟 exit 0 + clean endin（prepare_frame 经 download_.pump()/pending_downloads() 路径；download_node 经 enqueue/pump 路径）。

**Followups:** app.h 仍 553 行（pool_/remote_/frontend_/player 等其他成员簇 + 余下 god-object 方法）；后续按簇继续抽（候选：mode/frontend 持有、persistence 簇）。capture_exec 为通用 exec+capture 工具，若他处复用可晋 core/utils。

---

## D42 — app_input mode-switch 键簇 → SwitchModeAction（Keymap/Action 迁移 · #71）

**Context:** D6/D7 立 Keymap/Action（key→Action→EventBus publish）+ 迁 6 个无状态键（play/pause/volume/nav）。#71 续作：handle_input 的 switch(ch) 仍有 30+ case。评估哪些键簇适合迁 Action。

**Decision:** 迁 8 个同质 mode-switch 键（R/P/F/H/O/Y/B/I）→ `SwitchModeAction{AppMode target; std::string hint}`。其余键按设计不迁。

**关键点:**
- 同质簇优先：8 键都是 `switch_mode(X)` + 可选提示，结构一致 → 一个 Action 类型覆盖。M（循环读当前 mode）、N（跳节点）、b（区域）、T 非纯 mode-switch，不迁。
- hint 键特定非模式特定：switch_mode() 被远程命令/M 循环/编程路径多处调用，那些不要提示；提示是「按某键」的属性，随键绑定（Action）走，不进 switch_mode。这避免了「编程式切模式也弹提示」的行为漂移。
- 边界裁定：复杂有状态流（play 'l'/搜索 '/'/下载 'd''D'/标记 'a''A'）深依赖 mode+选中节点上下文，包 Action 增间接层却不带来可重绑收益 → 留 switch。Keymap 持「无状态可重绑命令」、switch 持「复杂有状态流」——主流设计（如 vim 的 mode-dependent 命令也保留过程式派发）。
- 非过度设计：续已立的 D7 模式（一致性），非为新抽象而新抽象。可重绑性是 Keymap 的既定价值（INI [keys] 覆盖待接线，但 Keymap 已是绑定的单一事实源）。

**Verification:** ctest 41/41、0-warning、pty 冒烟 exit 0 + clean endin（8 mode 键经 keymap→publish→subscribe→switch_mode 完整路径）。

**Followups:** #71 剩余：INI [keys] 覆盖接线（让 Keymap 真正可重绑）；复杂流是否迁 Action 视可重绑需求定（目前留 switch 合理）。#71 无状态簇迁移告一段落。下一大目标 #73（app.h Service 抽取）。


## D41 — event_loop END_FILE 分支分解 → handle_end_file_() + handle_playback_error_()（#72 完成）

**Context:** D36 抽完 event_loop 的 codec-info 块后，event_loop 另一大内联块——MPV_EVENT_END_FILE 分支（~80 行，含 reason==4 错误路径 ~48 行）仍内联。这是 mpv_controller.cpp 设计层分解的剩余目标（#72）。

**Decision:** 分两刀 Extract Method。① END_FILE 分支整体 → `handle_end_file_(mpv_event_end_file *ef)`（1 参，调用方保留 null-data 守卫）；② reason==4 错误路径 → `handle_playback_error_(int error_code)`（从 handle_end_file_ 内抽出）。两块皆输入单一（ef / error_code）、余皆成员、纯副作用。

**关键点:**
- reason/error_code 在块内从 ef 派生（非外层局部）→ ef 作唯一参数，零外层局部缠绕，干净切点（同 D36 codec 块的判据）。
- handle_end_file_ 抽出后 reason 派发成 switch 样结构，reason==4 再抽 handle_playback_error_ 使两者各 ~30/47 行、单一职责。event_loop 的 END_FILE 分支缩为 6 行（null 守卫 + 一次调用）。
- 锁语义不变：cb_mtx_ 仅持于读 end_file_callback_/last_load_url_/vo_fallback_done_（短临界区），callback 调用与 vo-fallback 命令在锁外——与原内联 1:1。
- 抽取脚本须正确处理块的尾花括号：reason==4 体的内层 if(iptv_context_) 闭合 `}` 属于体本身，须随体迁入新方法（脚本初版误排除致缺一 `}`，已修）。

**Verification:** ctest 41/41、0-warning、pty 冒烟 exit 0 + clean endin。

**收尾:** #72 完成。mpv_controller.cpp 设计层分解告一段落（D36 codec + D41 END_FILE；剩 update_state IPTV 块等小目标按需）。下一个大目标：#71 app_input Keymap/Action 迁移，或 #73 app.h Service 抽取。


## D40 — run() loop 抽 exit-check + drain → check_exit_requests()/drain_frame_events()（设计层第六刀 · #70 收尾）

**Context:** D35-D39 抽完 run() loop 的 startup/shutdown bookend、tree-locked 显示构建、prepare 半（Method Object）、draw 半后，loop 顶部仍剩两块注释密集的内联 phase——exit-check（CTRL+C/终止信号/睡眠定时，~28 行）与 drain（remote/playback/state 三排空，~13 行）。评估收尾抽取。

**Decision:** 抽两块。exit-check → `bool check_exit_requests()`（命中退出条件时 `running=false` + `return true`）；drain → `void drain_frame_events()`。两块皆零外层局部读取（只读成员）。

**关键点:**
- break 不能跨方法 → exit-check 以返回值传「该 break 了」：方法内设 `running=false` 并 `return true`，调用方 `if (check_exit_requests()) break;`。退出序列（reset 标志→EVENT_LOG→running=false→跳出）与原内联 1:1。这是 Extract Method 处理「含 break/continue 的块」的标准手法（控制信号经返回值传出）。
- 终止信号（SIGHUP/SIGTERM/SIGQUIT）无弹窗立即退出的语义保持——可能已无终端可交互；pool_.shutdown 在 shutdown() 跑（避免「异常退出丢 YouTube 频道」bug），此逻辑未动、仅搬位置。
- D4 不变量（playback-ended 在 UI 线程 drain、不在 mpv 线程持 playlist_mutex_）保持——drain_frame_events 仍每帧在 UI 线程跑。
- 不再继续抽 resize/view-scroll/input/watchdog：皆小（6-9 行）且单一职责，再抽为命名方法属过度分解（用户排斥）——run() loop 现已可一眼读懂。

**Verification:** ctest 41/41、0-warning、pty 冒烟 exit 0 + clean endin。

**收尾:** #70（run() loop 分解收尾）完成。run() loop = `startup()` + while{ frame_start; check_exit_requests→break; resize; drain_frame_events; prepare_frame; scroll; draw_frame; input; watchdog } + `shutdown()`。下一个 god-方法目标：#72 event_loop END_FILE 分支（mpv_controller.cpp）或 #71 app_input Keymap。


## D39 — run() loop 抽 draw 块 → draw_frame(const FrameCtx&)（设计层第五刀 · prepare/draw 双半收尾）

**Context:** D38 引入 FrameCtx 并抽出 prepare 半后，run() loop 的「draw 半」（取锁 → 快照 current_index/next/cur_url → 节流 history 缓存 → 构建 DisplayContext → frontend_->draw，~85 行）仍内联。D38 把 draw 块的输入全改成 `f.*`，使其具备零额外参数抽出条件——评估是否此时抽出。

**Decision:** 抽出 draw 半 → `void App::draw_frame(const FrameCtx &f)`。该块产出的 current_index_snap/next_snap/cur_url_snap **仅被 draw 调用自消费**——input 读 `f.marked`、watchdog 读 `frame_start_`，都不触及这三个 → 它们降为方法内局部，FrameCtx 无需承载。配合 D38 已有的 FrameCtx 输入，draw_frame 签名仅 1 个 const 引用（而非 D38 前预估的 5 参裸值）。

**关键点:**
- 锁搬进方法内获取/释放：原为 while 体内 `{ lock_guard(playlist_mutex_); ... }` 作用域，整体进方法——方法入口取锁、出口释放，与原作用域进出完全同构（与 D37 build_frame_display 搬 tree_mutex 同型）。
- P1.2 不变量（Y23.5）保持：`playlist_mutex_` 仅持于 draw、draw_frame 返回即释放、其后 input（handle_input→play_current→lock playlist_mutex_）无锁区，不致死锁。行为等价（锁覆盖范围与原 scope 1:1）。
- 块多级嵌套（声明 8sp / 锁体 12sp / 内层 current_index 重取 16sp），按级 dedent：≥12 前导去 8、≥8 去 4（声明 8→4、锁体 12→4、内层 16→8），丢弃外层 lock-scope 大括号（方法体即该作用域），内层 current_index 重取 scope 保留。声明先于锁初始化的结构不变（原即如此）。
- Method Object 价值兑现：D38 的 FrameCtx 使 draw 半从「缠绕切点（5 参）」降为「1 参 const 引用」——这正是引入 FrameCtx 的回报。

**Verification:** ctest 41/41、0-warning、pty 冒烟 exit 0 + clean endin（prepare→draw→input 每帧路径完整）。

**Followups:** run() loop 仅剩 exit-check phase（SIGINT/睡眠定时/resize，~38 行）可抽 `bool check_exit_requests()`（返回 true 则 break）；drain 三行 + 视图滚动 + input + watchdog 短小可留内联。抽完 exit-check 即收尾 #70。


## D38 — run() loop 引入 FrameCtx + 抽 prepare_frame()（设计层第四刀 · Method Object）

**Context:** D35-D37 用「干净切点」判据（零外层局部读取）摘完了 run() loop 的干净果（startup/shutdown bookend、tree-locked 显示构建）。剩下的 phase 都**与外层帧局部缠绕**：state（取自 player.get_state）被状态计算 + draw 共读；app_state（计算产物）被 draw 读；marked/sel_node/downloads（快照产物）被 draw + input 共读。plain Extract Method 对 draw phase 会落到 5 参 `draw(state, app_state, marked, sel_node, downloads)`——缠绕切点，无法零参数。

**Decision:** 引入 **per-frame 渲染上下文结构 `FrameCtx`**（Fowler「Replace Method with Method Object」）承载这五个共享帧局部，把「prepare 半」（取状态 + 派生 AppState + build_frame_display loading 合并 + 选中/下载快照 + pump + pending 折叠）抽成 `FrameCtx App::prepare_frame()`。run() 改 `FrameCtx f = prepare_frame();`，残留内联 draw 块与 input 改读 `f.*`。FrameCtx 作 App 私有嵌套结构定义于 app.h（`MPVController::State` 用嵌套公开类型，经 mpv_controller.h 可见）。

**关键点:**
- 这是共享帧局部缠绕的标准解法：当 plain Extract Method 被跨段局部共享阻塞，Method Object 把这些临时量收进一个结构，各 phase 变成取 `FrameCtx&` 的方法（或读返回的 FrameCtx）。draw phase 即将（D39）抽为 `void draw_frame(const FrameCtx&)`，届时参数列表是 1 个 struct 引用而非 5 个裸值。
- 行为保持：prepare_frame 是纯块搬迁（原内联 compute+gather 整体迁入，逐行不变，仅局部名 state→f.state 等）；current_index「app-owned 不从 mpv 每帧同步」注释随 `current_index_snap` 归位到 draw 快照块（语义注释随其所注释的代码）。
- 锁边界不变量（P1.2）：draw 块仍持 `playlist_mutex_`、输入在其后无锁区——本刀未动锁结构（draw 仍内联）。frame_start_（watchdog 计时基）prepare 不触及，留 loop 顶层。
- 非过度设计：Method Object 仅在 plain Extract Method 确被共享局部阻塞时引入（此处正是），非为抽象而抽象。app.h 净增 FrameCtx 结构 + 1 方法声明（run loop 解耦的合理代价）。

**Verification:** ctest 41/41、0-warning（`-Wall -Wextra -Wpedantic`）、pty 冒烟 exit 0 + clean endin（prepare→draw→input 每帧路径完整）。

**Followups:** D39 抽 draw 块 → `draw_frame(const FrameCtx&)`；D40 抽 exit-check phase → `bool check_exit_requests()`（返回 true 则 break）。run() loop 收敛为 flat 骨架后收尾 #70。


## D37 — run() loop Extract Method：tree-locked 显示构建 phase → build_frame_display()（设计层第三刀）

**Context:** D35 抽 run() bookend、D36 抽 event_loop codec 块后，继续 run() 帧循环的 god-方法分解。loop 各 phase 多与外层局部缠绕（frame_start_/state/app_state/sel_node/downloads/current_index_snap 横跨多 phase）。评估哪个 phase 是干净切点。

**Decision:** 抽 tree-locked 显示构建 phase → `bool App::build_frame_display()`。该 phase（取 tree_mutex→清空+flatten 显示列表→扫 loading→消费 pending_select→字幕 poll→lyric history→lyric-bar 激活）**零外层局部读取**——只用成员(library_/player/subtitle_/frontend_/cur_items())，产出唯一 bool `is_loading`。→ 零参数、一个返回值的干净 Extract Method。锁在方法内获取/释放，语义不变。

**关键点:**
- "干净切点"判据复用 D36：块零外层局部读取 + 只读成员 = 零参数抽出。其余 phase（状态计算读 state、draw 读 app_state/sel_node/downloads/current_index_snap、输入读 marked）皆有外层局部缠绕，须传参/提成成员 → 缠绕切点，留后续。
- 块 12 空格→方法体 4 空格，dedent 8。块内一处多行续行（watchdog `duration_cast<...>(\n  ...)`，续行对齐在 `<...>` 内），逐行去 8 前导空格保相对对齐（非压平）。dedent 前逐行核查无续行对齐失真。
- 锁语义保持：原为 `while` 体内 `{ lock_guard(tree_mutex); ... }` 作用域，现整体进方法——方法入口取锁、出口释放，与原作用域进出完全同构。P1.2 不变量（tree_mutex 仅持锁于显示构建、不延及输入）保持——输入仍在其后无锁区。
- 风险：per-frame 路径，pty 冒烟渲染帧覆盖（gross breakage 可测）；抽取为纯块搬迁+返回值，行为保持。

**Verification:** ctest 41/41、0-warning、pty 冒烟 exit 0 + clean endin。run() loop tree-lock phase 抽出，loop 更可读。



## D36 — event_loop Extract Method：codec-info 块 → log_track_codec_info_()（设计层第二刀）

**Context:** D35 拆 run() bookend 后，继续 god-方法分解。event_loop(~254)/update_state(~60 经 D34 后) 是 mpv 事件/状态方法。event_loop 的 PLAYBACK_RESTART 分支有一块"每轨记一次 codec 信息"(~52 行)，评估可抽性。

**Decision:** 抽 codec-info 块 → `log_track_codec_info_()`。理据：块**完全自包含**——每个局部(vc/hw/ac/vcodec/hwdec/acodec/vw/vh/vbr/abr/asr/ach/samplerate/channels/vline/aline)都在块内声明、只读成员 `ctx_` → **零参数**抽出。每轨一次守卫(`if(!restart_info_logged_){restart_info_logged_=true;<块>}`)留调用方，只搬 `<块>`。这是干净的 Extract Method（vs update_state 的 IPTV 检测块，其与属性轮询局部缠绕需 ~8 参数 → 不抽）。

**关键点:**
- 块在 16 空格缩进（event_loop 体 4 → while 8 → else-if 12 → if 16），迁方法体(4 空格)须 **dedent 12**。块内全单行 `fmt::format`、无跨行续行/括号对齐 → 均匀 `line[12:]` 保相对嵌套（空白行保空白）。dedent 安全性前提：块内无续行对齐——抽取前已逐行核查。
- 设计层 Extract Method 的"零参数"判据：块内局部全自声明 + 只读成员 = 干净切点；若块读取外层局部 → 须传参，参数越多越接近"缠绕切点"（应留后续或重新设计）。codec 块零外层局部读取 → 干净。
- 续 D35：god-方法分解按"先摘干净果"推进——run() bookend(零参数)、event_loop codec 块(零参数) 已抽；剩余(run loop 各 phase、update_state IPTV 块)皆与外层局部缠绕，需传参/提成成员，留后续设计。

**Verification:** ctest 41/41、0-warning、pty 冒烟 exit 0 + clean endin。块 verbatim 搬迁 + dedent，行为保持。



## D35 — run() Extract Method：startup/shutdown bookend 抽出（设计层第一刀）

**Context:** 机械搬迁缝（D17-D34）已饱和——cohesive 方法簇都迁了 sibling TU，剩余膨胀在 god-**方法**（run ~470 / event_loop ~254 / update_state ~214）。run() 三段清晰：startup(~115，一次性初始化)/while loop(~286，每帧)/shutdown(~67，teardown+persist+_exit)。是否抽？

**Decision:** 抽两个 bookend（startup/shutdown），不抽 loop 体。理据：
1. **两 bookend 自包含**：无局部变量流入/出 loop——startup 经成员(frontend_/player/playback_/library_/pool_/remote_server_)建状态、调成员方法(load_persistent_data/restore_player_state/mark_cached_nodes/load_default_podcasts)；shutdown 的局部(player_state/np/canonical_url/current_title/completed)全在区内自用。→ 抽出方法**零参数**，纯块搬迁。
2. **loop 体不抽**：各 phase 共享大量局部（frame_start_/app_state/is_loading/sel_node/downloads/state/current_index_snap/next_snap/cur_url_snap/hist_titles/markd 等）横跨多 phase，抽单 phase 须传多参或提成成员 → 复杂度上升、行为风险高。bookend 是干净切点，loop 是缠绕切点——先摘干净果。
3. **安全护栏**：两 bookend 恰被 pty 冒烟覆盖（启动→loop→q/y 退出→shutdown→exit 0 全路径），行为回归有测。

**关键点（机械层 vs 设计层的本质差异）:**
- 机械搬迁（D17-D34）：方法簇迁 sibling TU → **减总行数**（大文件变小，多 TU）。验证靠编译等价（同 include 集）。
- Extract Method（D35）：长方法拆短方法 → **不减总行数**（多方法签名/注释），改善**结构可读性/可测性/可维护性**。验证靠行为保持（测试护栏）。
- 两者都守铁律（0-warning+ctest+冒烟），但价值维度不同：机械层"文件不再臃肿"，设计层"方法不再难懂"。进入设计层后，行数不再是进度指标，**方法可读性**才是。
- 主流判断：长方法的首选重构永远是 Extract Method（Fowler《重构》第一个手法、最高频）。run() 先抽 bookend（干净），loop 体留后续设计（缠绕，需更细致的 phase 边界与局部归类）。

**Verification:** ctest 41/41、0-warning、pty 冒烟 exit 0 + clean endin。run() god-方法第一步驯服：startup(115)/loop(286)/shutdown(67) 三具名方法。



## D34 — mpv_controller 单条播放簇 → mpv_play.cpp（3 方法、~153 行 verbatim 搬迁）

**Context:** D18/D19/D20 已从 mpv_controller.cpp 抽 wrapper(mpv_commands)/metadata(mpv_metadata)/iptv(mpv_iptv) 三组，剩 1050 行。评估剩余方法的机械可搬性。

**Decision:** 抽单条播放簇 play_audio/play_video/play（326-477，~153 行）。理据：
1. **cohesive**：3 方法都是单条播放派发（URL→mpv 命令：play_audio 走 audio-only、play_video 走 video+可选 audio_file、play 按 URLType 分派到 audio/video），与播放列表（play_list/play_list_from）是 separate cohesion。
2. **整文件无文件局部 helper**、3 方法连续成块、依赖全 mpv_controller.h 可见 → 同 D18/D19/D20 的 .cpp→.cpp verbatim 搬迁（同 include 集 = 编译等价，无签名改动 → 调用点零触及）。
3. 不碰 event_loop(254)/update_state(214)——大型单方法，同 app_run run()，需设计性分解（抽 sub-loop），非 verbatim 目标。

**关键点:** mpv_controller 的机械缝与 app_run 同构——cohesive 方法簇可机械搬，大型单方法需设计分解。D18-D20+D34 已抽 4 组 sibling（mpv_commands/metadata/iptv/play），mpv_controller.cpp 1379→897（−35%）。剩 initialize/stop(生命周期)/event_loop/update_state(待设计)/play_list*(播放列表)。

**Verification:** ctest 41/41、0-warning、pty 冒烟 exit 0 + clean endin。mpv_controller.cpp 1050→897。



## D33 — app_run tree-flatten 组 → app_flatten.cpp（4 方法、~52 行 verbatim 搬迁）

**Context:** D32 抽完 feed 簇后，app_run.cpp 剩 run()（~470 行主循环）+ ctor/dtor + flatten 簇（flatten/flatten_items/items_for_mode/cur_items，~52 行）。评估 flatten 簇是否可机械搬、run() 是否可搬。

**Decision:** flatten 簇——搬；run()——不搬。
1. **flatten 簇可搬**：4 方法 cohesive（树→显示列表扁平化 + 按模式取项），与主循环是不同关注点；依赖全 app.h 可见（library_/LibraryService、TreeNode/NodeType/AppMode、OnlineState），无文件局部 helper → 同 D20/D23/D32 的 .cpp→.cpp verbatim 搬迁（同 include 集 = 编译等价）。新 app_flatten.cpp 取 app_run.cpp 同 10 include（超集，保守；flatten 实际仅需 app.h，余沿用无害）。
2. **run() 不可机械搬**：~470 行单方法 = 主循环 + 中央状态机，deeply coupled（每帧 drain 命令/事件、调各 Service、驱动渲染、处理退出），整块搬走只是换文件名不减肥；真正的减肥需设计性分解（抽 sub-loop 为 helper 方法）——那是设计任务，非 verbatim 搬迁，风险等级不同。本刀不碰。

**关键点:** 区分"机械搬迁缝"与"设计分解缝"——前者是 cohesive 方法簇、零签名改动、同 include 即编译（D17-D33 共 17 刀已挖尽 app_run.cpp/ui.cpp/mpv_controller.cpp/ini_config.h 的机械缝）；后者需改控制流/签名/调用点，是更高的设计决策。app_run.cpp 机械缝至此收官（run() 待设计分解）。判断"还能不能机械搬"看耦合（helper/签名/inline 三规则），不看体量——但判断"搬了有没有减肥意义"看是否真 cohesive：flatten 簇虽仅 52 行，但它是独立关注点（显示列表构造），搬出后 app_run.cpp = 纯主循环，形合理。

**Verification:** ctest 41/41、0-warning、pty 冒烟 exit 0 + clean endin。app_run.cpp 591→538。



## D32 — app_run feed-loading 组 → app_feeds.cpp（6 方法、~739 行 verbatim 搬迁）

**Context:** D23 首抽 app_run.cpp 时选了持久化组（4 方法、83 行、cohesive），刻意把 feed-loading 组（spawn_load_radio/spawn_load_feed/parse_feed_by_type/cache_youtube_videos/commit_feed_result/load_default_podcasts）留后，理由是"~738 行，parse_feed_by_type/load_default_podcasts 大且缠绕"。本刀评估该留后项：是否真缠绕到不能纯机械 verbatim 搬？

**Decision:** 证伪"缠绕"顾虑——仍是 **.cpp→.cpp verbatim 搬迁**（与 D20 mpv_iptv / D23 app_persistence 同构），理由链：
1. **实查 app_run.cpp 无文件局部 helper**（无 `static` 内联工具函数、无匿名命名空间）→ 搬 6 方法无需带额外符号。
2. 6 方法**连续成块**（spawn_load_radio 到 flatten 前），无中间夹杂其他方法 → 切片边界干净。
3. 仅依赖 app.h 传递可见的符号（线程池 spawn / ParserRegistry::create / BilibiliAPI / YouTube 缓存 repo / EVENT_LOG / fmt）→ 新 TU 取**相同 include 集**即编译等价（include 超集保证任何已编译代码在新 TU 仍编译）。
4. 方法已 out-of-line `App::`（非 inline）→ 无声明/定义分离的 C++ 三规则坑（默认参数/static/Class::）。
5. 无签名改动 → 调用点（app_run 主循环 / 各 app_*.cpp）零触及。

**关键点:** "缠绕"（tangled）≠ 不可机械搬——D23 的顾虑是**体量大 + 内部逻辑复杂**（parse_feed_by_type 的多分支、load_default_podcasts 的多源默认），但 verbatim 搬迁**不改逻辑、只改定义所在 TU**，逻辑复杂度与搬迁可行性正交。真正的搬迁障碍是**跨方法耦合**（helper 依赖、签名变更、inline 三规则），这三项 feed 簇都不沾。判断"能否纯机械搬"应看耦合而非体量。

**Verification:** ctest 41/41、0-warning、pty 冒烟 exit 0 + clean endin。app_run.cpp 1330→591。



## D31 — ini_config create_default（raw-string 配置模板）inline→cpp（~408 行体）

**Context:** ini_config.h 最后一大块是 create_default——自动生成的默认配置模板，体为巨型 raw string `R"(...)"`。D30 脚本的逐行 `b[4:]` dedent 对含 raw string 的体有 corrupt 风险：若 raw-string 内容行有 ≥4 前导空格，会被误去缩进而损坏字符串。

**Decision:** 先排查——`awk` 验证 create_default 体内 raw-string 内容全在 0 列（无前导空格行，唯一前导空格行是代码 `}`）。故逐行 `b[4:]` 安全：0 列行走 `else: block+=b` 原样保留，仅代码行（≥4 空格）去缩进。D30 手法直接复用，无新逻辑。ini_config.h 713→304。

**关键验证（raw string 搬迁必做）:**
1. **byte 等价**：旧体（git HEAD header）抽 create_default 体去 4 空格 vs 新 cpp 体，归一化 diff 空（408 行）——raw-string 内容逐字保留。
2. **运行时**（pty 测不出，create_default 仅 config.ini 缺失时触发）：临时 HOME 跑 binary → 生成 402 行 config.ini + 未崩 → 功能正常。

**D24-D31 收官小结:** 72 个 ini_config 方法由 inline 迁 out-of-line；ini_config.h 1087→304（−72%）。god-header 驯服为纯声明头。剩余 inline：get/get_float/get_bool/resolve_cookies_path（多行签名简单泛型访问器，留 inline=合理设计：inline 访问器可内联优化、且这些是全文高频调用的底层访问器）+ `IniConfig(){}` trivial ctor。**ini_config 机械搬迁到此真正收尾。**

**Verification:** ctest 41/41、0-warning、pty 冒烟 exit 0 + clean endin、create_default 运行时实测生成配置。



**Context:** D29 抽完核心存取簇后，剩余单行签名方法（3 static + 6 逻辑）仍是纯机械 verbatim 搬迁目标。static helper（normalize_proxy/resolve_color/get_config_file）此前刻意留 inline 因带逻辑，但 verbatim 迁移不改逻辑、仅改定义位置，编译安全（cpp `#include` header = 依赖超集）。

**Decision:** D29 手法 + **static 剥离**：static 成员函数的 out-of-line 定义不重复 `static`（声明保留）。脚本 `re.sub(r"^static\s+","",nm)`。默认参数剥离复用 D29。多行签名访问器（get/get_float/get_bool/resolve_cookies_path）刻意留——续行重排收益低；raw-string `create_default` 留——逐行 dedent 有 corrupt 巨型 raw string 风险，须单独纯 verbatim 处理。ini_config.h 854→713。

**关键点:** static 成员 out-of-line 定义的三个 C++ 规则一并验证：①不重复 `static`；②默认参数仅声明处（D29）；③`IniConfig::` 限定。inline→cpp 搬迁 static 方法至此手法完备。

**D24-D30 阶段小结:** 71 个 ini_config 方法由 inline 迁 out-of-line；ini_config.h 1087→713（−374 行）。剩余 inline：get/get_float/get_bool/resolve_cookies_path（多行签名）、create_default（raw string）、`IniConfig(){}`（trivial ctor）——均为特例，非高性价比。

**Verification:** ctest 41/41、0-warning、pty 冒烟 exit 0 + clean endin。



**Context:** D28 抽完所有简单 getter 后，剩 inline 多为带逻辑方法。但**任何 inline 方法整块迁对应 cpp 都是编译安全的**（cpp `#include` header = 依赖超集；即便 load-bearing 核心亦然），故核心存取簇仍属纯机械 verbatim 搬迁。选 load/save/get_int/set（单行签名、高价值 ~115 行）；get/get_float/get_bool（多行签名 + 续行对齐）留 inline（搬迁须重排续行，收益低）。

**Decision:** D24-D28 手法 + 泛化 name 匹配（通用 sig_re `^    .*\b(\w+)\s*\(.*\)...{`、TARGETS 白名单、`len==` 断言）。体含深嵌套（while/for/if/try-catch），方法级闭括号在 4 空格处——脚本只匹配它，对嵌套稳健。ini_config.h 966→854。

**关键点（两处脚本缺陷本轮暴露并修正）:**
1. **嵌套体不可压平**：D24-D28 的 body 重构用 `"    "+b.strip()`（单行 getter 体 OK）会把 load/save 的 if/while 嵌套全压到 4 空格。改逐行 `b[4:]` 保相对缩进（去 4 空格、留嵌套层级、留 `\n`）。
2. **默认参数剥离**：`get_int(int default_val = 0)` 整签名搬进定义会触发 `-fpermissive` error（默认参数 C++ 只能在声明处给一次）。脚本加 `re.sub(r"\s*=\s*[^,);]+","",nm)` 剥定义处默认值（声明保留）。此为 inline→cpp 搬迁带默认参数方法的**通用坑**。

**D24-D29 阶段小结:** 62 个 ini_config 方法由 inline 迁 out-of-line；ini_config.h 1087→854（−233 行）。剩余 inline（get/get_float/get_bool 多行签名、get_statusbar_color_config/get_play_mode/get_proxy/color/get_node_color/is_url_safe/normalize_proxy/get_config_file 带逻辑或 static）非高性价比机械搬迁。

**Verification:** ctest 41/41、0-warning、pty 冒烟 exit 0 + clean endin（启动 load() 实测）。



**Context:** D27 小结误判"简单 getter 簇基本尽"——复查发现 display 配置组（log_height_ratio/log_compress_height/display_state_refresh_ms/display_lyric/display_lyric_lines/display_lyric_bar/display_lyric_bar_height）7 个单行 `return get_*` 委派 getter 仍是纯机械搬迁目标。

**Decision:** D24-D27 同手法（sig_re=`get_\w+...const {`、块边界严格断言、dedent 4、`IniConfig::` 限定）。文档注释（F36 终端高度阈值 / Y11 刷新间隔 / Y12 lyric / Y24 lyric bar 阈值说明）留 header 作 API 文档。ini_config.h 980→966。

**关键点:** D27 脚本两处缺陷（声明丢缩进；定义插到 `namespace panicast{}` 闭括号外 + 缩进倍增）暴露后修正：①header 重写须 `"    "+decl`；②cpp 须 `rfind("} // namespace panicast")` 在其前插入、0 缩进 sig / 4 缩进 body、2 空行+`// ── ... (Dxx: moved out-of-line) ──` 组头。脚本即回归基准。

**D24-D28 阶段小结:** 58 个 ini_config getter 由 inline 迁 out-of-line；ini_config.h 1087→966（−121 行）。display 简单 getter 簇尽。剩余 inline（load/save/get_statusbar_color_config/get_play_mode/get_proxy/color/get_node_color/is_url_safe/get_config_file/normalize_proxy/get_int）均带逻辑，非纯机械。

**Verification:** ctest 41/41、0-warning、pty 冒烟 exit 0 + clean endin。



**Context:** D24-D26 抽完 mpv/youtube/cookies-iptv-remote 四个前缀簇后，剩余简单 getter 散落（search/history/region/network/url_hyperlink 等，非同前缀）。

**Decision:** 在有界块（245-282）内用**通用 `get_\w+` sig 模式**抽 8 个 → ini_config.cpp。块边界严格断言，故通用模式安全（块内仅这 8 个 get_ 签名）。`get_statusbar_color_config`/`get_play_mode`/`get_proxy`/color 组结构稍复杂（循环/transform/路径解析）刻意留 inline，后续单独评估。ini_config.h 996→980。

**D24-D27 阶段小结:** 51 个 ini_config getter 由 inline 迁 out-of-line；ini_config.h 1087→980（−107 行）。剩余 inline 多为带逻辑的方法（load/save/color/proxy/statusbar），非纯机械，需更细评估——ini_config 的"简单 getter 簇"机械抽到此基本尽。

**Verification:** ctest 41/41、0-warning、pty 冒烟 exit 0 + clean endin。

## D26 — ini_config cookies/IPTV/remote getter 组 inline→cpp（14 个声明/定义分离）

**Context:** D24/D25 手法持续验证。继续抽 cookies(bilibili/tiktok) + IPTV + remote-control 三个相邻简单 getter 组（14 个，连续 416–475 行）。

**Decision:** 一次抽 14 个 → ini_config.cpp out-of-line。脚本泛化 name_re 为多前缀 `get_(bilibili|tiktok|iptv|remote)_\w+`。get_proxy（路径解析 if/else）与 color 组**刻意留 inline**（结构稍复杂，留后续单独评估）。ini_config.h 1024→996。

**Why 一次抽 14 个（跨 3 组）:** 同性质（全单 return/单层体、无嵌套大括号）且相邻 → 脚本一次扫描、每方法断言闭合，比拆三刀高效；仍是一增量（一次 build+test+commit）。

**Verification:** ctest 41/41、0-warning、pty 冒烟 exit 0 + clean endin。

## D25 — ini_config YouTube getter 组 inline→cpp（9 个 get_youtube_* 声明/定义分离）

**Context:** D24 手法验证可行（mpv getter 组）。继续抽 ini_config.h 下一 cohesive 簇——YouTube getter 组（9 个）。此刀起工作流变更为 **main 主线**（dev/m2 批次已 ff-merge 入 main）。

**Decision:** 抽 → ini_config.cpp out-of-line 定义，声明 + 文档注释留 header。ini_config.h 1043→1024。脚本复用 D24（split_youtube_getters.py，仅改 name 模式 + 边界 + 期望数 9）。

**Why 同手法增量:** D24 已证 inline→cpp 拆分对"简单 getter 簇"是可机械脚本化、可断言、零行为变化的。YouTube 组同性质（9 个全单 return/单层体、无嵌套大括号）。逐组增量抽（mpv→youtube→…）比一刀全切 119 方法安全。

**Verification:** ctest 41/41、0-warning、pty 冒烟 exit 0 + clean endin。

## D24 — ini_config mpv getter 组 inline→cpp（20 个 get_mpv_* 声明/定义分离）

**Context:** ini_config.h 1087 行 header god-object，方法全内联（`grep -c IniConfig::` = 0）。ini_config.cpp 仅是 `instance()` stub。任务 #69 = inline→cpp 抽取。

**Decision:** 选 **mpv-config getter 组**（get_mpv_vo…get_mpv_sub_lang，20 个）做声明/定义分离：声明 + 文档注释留 header，`IniConfig::` 限定定义迁 ini_config.cpp。ini_config.h 1087→1043。

**Why 选 mpv getter 组而非全量 119 方法:** 全量 inline→cpp 是 119 方法的逐个签名复制，typo 风险高、非纯 verbatim。mpv getter 组是 header 最大最纯的 cohesive 簇——20 个全为单 return / 单层体、**无嵌套大括号** → 脚本可按"签名行 → 同缩进首个闭合 `}`"机械切分（每方法断言方法名 + 闭合），可验证。其余组（YouTube/bilibili/color/save/load）留后续同手法增量抽，不一刀全切。

**Why 声明/定义分离而非整块 verbatim 搬:** inline→cpp 本质是 per-method 变换（header 去 body 留声明、cpp 加限定定义），非 D17-D23 的字节保持整块搬。但遵循标准约定（声明 + API 文档留 header、cpp 精简实现）使其仍是机械的——无需设计决策。header 仍含全部依赖 → cpp 经 ini_config.h 见私有成员与 get/get_int/get_bool，零新 include、零行为变化。

**Why 注释放 header:** 方法间注释（F40/Y14/Y24.12/Y24.43）是 API 文档（解释每个 getter 的语义/默认值）→ 留 header 服务消费者；cpp 定义不重复（实现即 `return get(...)`，自解释）。

**Verification:** ctest 41/41、0-warning、pty 冒烟 exit 0 + clean endin。5 刀批次（D20–D24）完成。

## D23 — app 持久化组 → app_persistence.cpp（load_data/load_persistent_data/save_persistent_data/restore_player_state）

**Context:** app_run.cpp 1414 行（最大 app 文件）。survey 后选**持久化组**作首抽：4 方法 cohesive（启动/退出 DB load/save/restore），体量适中（83 行），低于 feed-loading 组（~738 行，parse_feed_by_type/load_default_podcasts 大且缠绕）。

**Decision:** 抽 → `src/app/app_persistence.cpp`，逐字节 verbatim。app_run.cpp 1414→1330。

**Why 干净机械:** 实查 app_run.cpp **无文件局部 helper**（无 anon-namespace、无 static 自由函数）；4 方法只依赖 header 可见符号——`Persistence::`(persistence.h)、`DatabaseManager::`(database.h)、`OnlineState::`(online_state.h)、`EVENT_LOG`/`fmt`(ui.h→event_log.h / fmt) 全经 **app.h 传递可见**（app.h:78,81,86,98），加 `<iostream>`（std::cout）即最小正确集。`switch_mode` 是 App 成员（app.h 声明、sibling 定义），跨 TU 链接无碍。

**Why 选持久化组而非 feed 组:** feed-loading 组虽 cohesive 但 parse_feed_by_type(142 行)/load_default_podcasts(388 行) 大且缠绕、依赖更多——留作后续需更细 survey 的项。持久化组是 app_run.cpp 当前最干净的一刀，与既有 18 个 app_*.cpp sibling 同 idiom。

**Verification:** ctest 41/41、0-warning、pty 冒烟 exit 0 + clean endin。

## D22 — ui lifecycle 组 → ui_lifecycle.cpp（终端/信号簇 + init/cleanup/handle_resize + 3 文件局部全局连带搬）

**Context:** D21 后 ui.cpp 剩 init/cleanup/handle_resize/draw 四块。任务预设 lifecycle 组引用文件局部全局需特殊处理——实查（grep）推翻：`init`/`cleanup`/`handle_resize` **只引用 extern/header 符号**（emoji 全局在 `core/terminal.h:35-36` extern）；3 个文件局部全局（anon-namespace `g_original_termios`/`g_termios_saved`、`static g_term_sig_count`）**仅**被终端/信号自由函数簇引用。

**Decision:** 整组（终端/信号 6 自由函数 + 3 UI 成员方法）逐字节 verbatim 抽 → `src/ui/ui_lifecycle.cpp`。3 文件局部全局**连带搬**（D20 原则：随其唯一调用者簇搬，保持文件局部、零接口面变化）；5 个 ui.h-extern 全局的**定义**随簇迁此（单 TU 定义、声明留 header）。ui.cpp 638→263。

**Why 整组搬而非只搬成员方法:** 终端/信号簇是文件局部全局的唯一归宿（anon/static 无法 extern 化，只能随簇搬），与 init/cleanup/handle_resize 同属"生命周期"——合为一文件达成 ui.cpp 的"生命周期 vs 渲染"干净切割（ui.cpp 仅剩 draw + 渲染辅助 263 行）。include 集与 ui.cpp **逐字一致** → 搬迁代码编译等价（同经 ui.h 传递包含），无需猜测依赖。

**Why 编译等价成立:** 给新 TU 与源 TU 相同 include 集，则被搬代码的传递包含环境不变 → 必然 0-warning 编译（实测验证）。这是大块 verbatim 搬迁降险的关键手法。

**切割点验证:** `draw` 起的剩余 ui.cpp 经 grep 确认**零引用**任何被搬符号（含 5 extern 全局、3 自由函数）→ 无悬空引用。

**Verification:** ctest 41/41、0-warning、pty 冒烟 exit 0 + clean endin。

## D21 — ui setter/toggle 组 → ui_toggles.cpp

**Context:** D20 后回 ui.cpp 再抽一组。5 个视图态 setter/toggle 是纯成员写（+ EVENT_LOG/INI），无文件局部依赖。

**Decision:** 抽 → `src/ui/ui_toggles.cpp`，逐字节 verbatim。ui.cpp 668→638。

**Why:** 与既有 ui_*.cpp 同 idiom；单方法体虽小（5×~5 行）但 cohesive（视图态开关），抽出后 ui.cpp 剩 init/cleanup/handle_resize/draw 四块核心，结构更清。

**Verification:** ctest 41/41、0-warning、pty 冒烟 exit 0 + clean endin。

## D20 — mpv IPTV 检测组 → mpv_iptv.cpp（含 log_has 连带搬 + 修 reset 伪 inline）

**Context:** D19 后 mpv_controller.cpp 再抽 IPTV 诊断组。`log_has`(file-local static) 仅被 `classify_iptv_load_error_` 调用，故连带搬（不 extern、不改可见性）。

**Decision:** 抽 log_has + 4 方法 → `src/playback/mpv_iptv.cpp`，逐字节 verbatim。mpv_controller.cpp 1126→1050。

**搬迁暴露的既有 bug（重要）:** `reset_iptv_detection_` header 声明无 inline、cpp 定义带 `inline`。同 TU 时偶然能编；跨 TU 后 undefined reference。修正：定义去 inline 与声明对齐。**教训——搬迁是暴露"声明/定义不一致"类隐性 bug 的探针**；这类修正是机械的（让定义匹配既有声明），不属行为变更。

**Why 连带搬 log_has 而非 extern:** log_has 是 static、单调用者，连带搬保持其文件局部性、零接口面变化（最简）。若日后多 TU 需它再 extern 化。

**Verification:** ctest 41/41、0-warning、pty 冒烟 exit 0 + clean endin。

## D19 — god-object 拆分第三刀：mpv_controller 静态元数据组 → mpv_metadata.cpp（M3 · 拆 god-object）

**Context:** D18 抽 wrapper 组后，mpv_controller.cpp 再抽一组干净 static 方法。

**Decision:** 抽静态元数据/诊断组（`end_file_reason_str` / `mpv_error_str` / `set_cli_overrides`，原 36-109）入新 `src/playback/mpv_metadata.cpp`，逐字节 verbatim。mpv_controller.cpp 1203→1126。

**Why 干净:** 3 方法皆 static、无实例状态、无文件局部 helper 依赖（纯 switch+fmt / 静态成员写+LOG），依赖全 header 可见。静态成员定义（`cli_*_override_`）留 mpv_controller.cpp（单 TU），声明在 header 供 mpv_commands（读）+ mpv_metadata（写）共用。

**Why 到此为止（mpv_controller 干净机械抽尽）:** 剩余 IPTV 检测组（`classify_iptv_load_error_` 等）依赖文件局部 `static log_has`（搬它须连带搬 log_has，超纯机械）且与析构交织；initialize/event_loop/play_*/update_state 是缠绕核心。这些需设计决策，不再 quick cut。（D18 预测 IPTV 组为"下个安全候选"——D19 实查后修正：因 log_has 耦合不干净，归入"需设计"。）

**Verification:** ctest 41/41、0-warning、pty 冒烟 exit 0 + clean endin。

## D18 — god-object 拆分第二刀：mpv_controller wrapper 组 → mpv_commands.cpp（M3 · 拆 god-object）

**Context:** D17 立 ui.cpp 拆分轨道后，开第二个 god-object mpv_controller.cpp（1379 行）。ROADMAP M3 既定 split mpv_controller.cpp。

**Decision:** 抽 thin-wrapper 组（735-909，18 叶子方法：pause/volume/speed/loop/sub/osd/state-query/handle/callback/resume）入新 `src/playback/mpv_commands.cpp`，逐字节 verbatim；方法留成员、声明在 header、impl 落 sibling。mpv_controller.cpp 1379→1203，重核心（initialize/event_loop/play_*/IPTV 检测）留原文件。

**Why 依赖搬迁前预核（与 D17 同纪律）:** wrapper 组用的私有成员/类型/常量——`ctx_/mtx_/cb_mtx_/state_/enqueue_cmd_/cli_*_override_/end_file_callback_/pending_resume_*`、`EndFileCallback/State`、`MAX_VOLUME/DEFAULT_SPEED/SPEED_STEP/MIN_SPEED/MAX_SPEED`（constants.h，**非匿名命名空间**——关键：mpv_controller.h:19 已 include constants.h，故任何含该 header 的 sibling 即得常量）、`enqueue_cmd_`（header 154 已声明）——**全 header 可见，无需动 header**。搬迁前 grep + 逐行读 735-909 复核，断言式脚本抽取。

**Why wrapper 组作 playback 首刀（非 initialize/event_loop）:** 18 方法全是 `if(!ctx_)return`+单属性 set/get 的 leaf op，不涉事件循环/状态机控制流——最大**安全**可抽取组。initialize/event_loop/play_* 是缠绕核心（事件线程、VO 回退、resume、IPTV 时序），留最后。建立 `mpv_*.cpp` sibling 模式（与 ui_*.cpp 同 idiom）。

**Why 纯机械 verbatim:** ctest（41）不覆盖 mpv 运行时；逐字节搬迁保行为零变化，依赖人工 pty 冒烟 + 末尾统一实测。

**后续:** IPTV 检测组（128-194，cohesive 诊断）下个安全候选 → 再视情况碰核心。

**Verification:** ctest 41/41、0-warning、pty 冒烟 exit 0 + clean endin。播放控制待统一实测。

## D17 — god-object 拆分首刀：draw_help → ui_help.cpp（M3 · 拆 god-object 启动）

**Context:** ROADMAP M3 任务"拆 god-object：split app_run.cpp/ui.cpp/mpv_controller.cpp/ini_config.h"。剩余 hub 是缠绕的 run-loop/draw/mpv 生命周期核心——最难的拆分地带。用户指示"做里程碑级的拆！"，启动该轨道。

**Decision:** 首刀抽 `UI::draw_help`（帮助/热键覆盖窗渲染器，ui.cpp 668-945，~278 行，全文件最大单块）入新 `src/ui/ui_help.cpp`，逐字节 verbatim 搬迁；方法仍 UI 成员、声明留 ui.h、CMake 加源；ui.cpp 947→668。

**Why draw_help 作首刀（不是 mpv wrapper 组 / app_run）:**
1. **最大干净收益**：~278 行单块，是 ui.cpp 内最大的可抽取体。
2. **精确同构既有模式**：与 12 个已抽 `ui_*.cpp` 渲染器（info_panel/popups/status_bar/tree_renderer/lyric_renderer…）同一 idiom——渲染器落 sibling、方法留成员、声明在 header。零新 idiom。
3. **零依赖搬运**：三形参全 `(void)` 弃用；只用 `h/w` 私有成员 + constants.h + utils.h + fmt + ncurses(via ui.h)。常量/成员全 header 可见，**无需动 header**。（对比：mpv wrapper 组虽也已核查 header 可见，但 mpv 无既有 sibling 模式、是首建 `playback/` 拆分轨道，风险略高，留作后续刀。）

**Why 纯机械 verbatim:** ctest（41）不覆盖 ui 运行时；hub 拆分无自动测试护驾。逐字节搬迁（脚本抽取）保证行为零变化，依赖人工 pty 冒烟 + 末尾统一实测。

**后续链:** ui.cpp 再抽（生命周期组/setter 组/draw 留核心）→ mpv_controller wrapper 组（建 `mpv_*.cpp` 模式，依赖已核查全 header 可见）→ ini_config.h。

**Verification:** ctest 41/41、0-warning、pty 冒烟 exit 0 + clean endin。帮助窗（`?`）待统一实测。

## D16 — save 路径 current_title 收敛到 now_playing()（持久侧 now-playing 单通道）

**Context:** D15 把渲染侧 now-playing 身份统一到 DisplayContext 单通道（url+title）。但 app_run.cpp 的 player_state save 块仍分裂：`current_title = playback_.playback_node()->title`、`canonical_url = playback_.now_playing().id.url()`——同一 save 块、同一 now-playing 身份、两通道（节点指针 + now_playing()）。

**Decision:** 合并为单次 `const Media np = playback_.now_playing(); canonical_url = np.id.url(); current_title = np.title;`，删 `cur_node`。app_run.cpp 的 `playback_node()` 直调归零（now-playing 身份全经 now_playing()）。

**Why D15 的对偶、现在做:** D15 收口渲染侧、D16 收口持久侧——两路 now-playing 身份读取都从单一 now_playing() 通道。这是 D15 留下的对称 loose end（渲染侧拔了 playback_node，持久侧还有 cur_node）；补上即 app_run.cpp 全程单通道。

**Why 零续播风险:** `current_title` 仅存 player_state 供下次启动展示（"正在播放"标题），非 resume 键——resume 仍键于 `canonical_url`（D14-4 已收敛源 URL，本增量未动）。`now_playing().title≡playback_node_->title`（media_from_node 派生），值逐字不变。

**Verification:** ctest 41/41、0-warning、pty 冒烟 exit 0 + clean endin。

## D15 — 渲染契约 now-playing 去冗余通道：拔 playback_node 域指针（M1 UI 解耦收尾）

**Context:** D14-3 读侧收敛时把 now-playing **url** 收进 `DisplayContext.now_playing_url`，但 info_panel 仍用既有的 `TreeNodePtr playback_node` 形参取 title/url（D14-3 只换值来源 current_url→playback_node->url，未动契约形状）。结果同一 now-playing 身份在渲染契约里走两通道：dctx（url）+ 域指针（title/url）。

**Decision:** title 并入 `DisplayContext.now_playing_title`；`IFrontend::draw` 删 `TreeNodePtr playback_node` 形参；`draw_info` 形参 `playback_node`→`const DisplayContext&`，读 dctx。渲染契约对 now-playing 身份只走视图模型一通道。

**Why 现在做（D14 收官之后）:** 不是 D14 身份收敛（D14-5 已收官，身份早已统一）。这是 D14-3 留下的**契约形状补遗**——url 进了 dctx、title 还在指针上，是同一身份两通道的冗余。补 title 进 dctx + 拔指针 = 收口这层冗余，属 M1 UI 解耦（视图模型化）收尾。

**Why 行为零变化仍可做（对照 D14-3 lyric_renderer 原则）:** D14-3 对 lyric_renderer 保留 current_url 因"无行为收益不改契约方法"。本增量同样无行为收益，但改的是身份的**传递通道**（指针→dctx）非**值**（current_url↔源 URL）；`now_playing().title≡playback_node->title`（`media_from_node` 派生），值逐字不变。签名变更是机械的（编译挡位置错位）、唯一实现者是 UI（无 mock 要跟）；去冗余本身是收益（契约不再对同一身份持两通道）。

**Why 不做完整脱耦（坦注边界）:** 契约仍经 `selected_node`（渲染游标块正当需节点字段）+ `DisplayItem.node` 持 TreeNode。整契约从 TreeNode 解耦需 TreeNode 视图化（DisplayItem/selected 都换成视图模型）——更大的 M1 后续，超出"去 now-playing 冗余通道"范围，暂不做（避免过度设计）。本增量只去 now-playing 身份这一处冗余。

**Verification:** ctest 41/41、0-warning（`-Wall -Wextra -Wpedantic`，5 文件 11 处）、pty 冒烟 exit 0 + clean endin + q→y 退出对话框。INFO 面板 now-playing Title/Streaming URL 显示待人工验证（应与 D14-3 后同行为）。

## D12-2 — 游标事件化：reveal_node→SearchService + jump_to_match→pending_select（M1 · D12 收官）

**Context:** D11-3b 把搜索算法（search_recursive/collect_context_matches/cycle_match）搬进 SearchService，但 jump_to_match/reveal_node 留在 App——头注释明说"moving them needs the cursor event-driven, deferred"。D12-2 是这个延迟步骤。

**Decision:**
- **reveal_node → `SearchService::reveal_node(roots, node)`**（纯树展开，lock-free，调用方持 tree_mutex——同 collect_context_matches 先例）。它本就是纯树变异，与搜索算法同域、同锁约定，能干净搬入。
- **jump_to_match 内联 flatten+select+scroll → pending_select 延迟机制**（reveal + 设 pending_select → 主循环下帧 select+居中）。主循环每帧重建 display_list，jump_to_match 的内联 flatten 是冗余。

**Why 不让 SearchService 读视图态（偏离原 spec 字面）:** 原 task 描述"游标经访问器/事件让 SearchService 读视图态"。但 SearchService 读 selected_idx/view_start = 搜索层→视图层反向依赖，破坏 display-decoupling。改为让游标跳转**完全延迟到视图**（run loop 解析 pending_select），SearchService 只产"要跳到的节点"+展开祖先，视图自己 select+scroll。更彻底符合"搜索只负责搜索的事"。

**Why pending_select 而非新 EventBus 事件:** pending_select 已是项目既有的游标延迟机制（Y11），event_bus.h:6-9 注记它是 EventBus post/drain 的前身。搜索游标统一到它上面 = 三条路径（search/jump_to_playing/异步建节点）共用一个解析点，且为日后 pending_select→EventBus 统一迁移铺路。新增独立 SearchReveal 事件会再造一个并行机制。

**Why 补居中到共享消费点:** jump_to_match 原内联 `view_start = max(0, sel-(LINES-5)/2)` 居中。搬出后把居中也搬到 pending_select 消费点，三条路径一致居中。jump_to_playing 原只 select 不居中——现也居中（一致性改进，非回归）。

**Verification:** ctest 41/41、0-warning、pty 冒烟绿。搜索跳转/jump_to_playing 居中行为待人工验证（pty smoke 只覆盖启动/退出）。

## 测试镜像漂移 2/3+3/3 — parse_time 链入真实 / escape_sql 删除（M2 · 测试质量补遗）

**Context:** test_units.cpp 手抄了 classify/parse_time_string/escape_sql 三副本，测副本非真实 → 真实改动副本不变 = 假阳性。1/3（classify）已链入真实。本增量决 2/3、3/3（+ 用户要求睡眠时间非法时提示正确格式）。

**Decision:**
- **parse_time_string（2/3）= 链入真实 `SleepTimer::parse_time_string`**，非继续手抄。CMake 加 `sleep_timer.cpp`+`event_log.cpp`（timer 用 EVENT_LOG/LOG 5 处，需 EventLog 链入）。副本删除，调用改 `SleepTimer::parse_time_string(`。
- **escape_sql（3/3）= 删除（不拆分）**。核实：① 生产 **0 调用**（grep src/+include/ 零命中）；② 功能**已迁移**——AccountsManager 的 SQL 全走 `account_repo.cpp` prepared statement（`sqlite3_prepare_v2`+`sqlite3_bind_text/int`），这是防注入的正道，手工转义已被取代。删 database.cpp impl + database.h 声明 + test_units 副本 + 3 EscapeSql 测试。

**Why escape_sql 删而非拆纯逻辑:** 选项曾为"拆纯函数层"——但拆分前提是有人用它。已是死代码，拆出独立纯函数无消费者 = 为死代码动生产代码，不合理。迁移已发生（prepared statement），删除 = 收尾废弃路径。副本漂移（真实 strip NUL 防 c_str 截断 / 副本不 strip）本身是假阳性的另一证据——副本全过的"安全"从未反映真实行为。

**Why parse_time InvalidInput -1→0:** 真实无效输入返回 0（`clamp_ret(v<=0)→0`），非 -1。语义：0=无效/不设，调用方 `if(seconds>0)` 跳过。CLI（main.cpp）早已据此提示非法——本增量补正确格式示例（`30m / 1h / 90s / HH:MM:SS / <分钟>`）。

**Verification:** ctest 41/41（44 - 3 EscapeSql）、0-warning、panicast 链接无未定义符号（坐实 escape_sql 零生产调用）、pty 冒烟绿。

## D14-4b — save 守卫对齐 read（流式 resume gap 清理）（M2 · 持久侧补遗）

**Context:** D14-4 把 progress 键收敛到源 URL 后暴露的 gap：save 侧无类型守卫对所有项存 progress，但 read 侧续播条件含 `ut != RADIO_STREAM`（playback_service.cpp:488）——电台/在线播客/本地音频（classify 折叠进 RADIO_STREAM）存了却永不续播，库积累死行（电台最高频）。

**Decision: save_progress 加 `classify(canonical_url) != RADIO_STREAM` 守卫，对齐 read。** save 只存 read 会消费的类型；RADIO_STREAM 项不再存。

**Why 零回归:** 被剔除项（电台/播客/本地音频）此前也不续播（read `ut != RADIO_STREAM` 挡掉）；不存仅停止累积无用行，续播行为零变化。保留 VIDEO_FILE（本地视频 + 在线视频缓存，续播）/ YouTube / RSS（日后缓存可读）。本地音频文件因 classify 折叠进 RADIO_STREAM 也不存——它本就不续播（同后果 B，URLClassifier 无 LOCAL 类别）。

**Why 不清理旧死数据:** D14-4 迁移后已存的 RADIO_STREAM progress 行保留。清理需 C++ 端遍历 classify（SQL 不知 URLClassifier），且旧行无害。阻新不清旧，增量最小。

**Why 不动 player_state:** save_player_state 是"上次播放"指针（不涉 progress 续播），无需类型守卫。

**Verification:** ctest 44/44、0-warning、pty 冒烟绿。续播路径未动（read 守卫不变）。

**Followups（可选）:** 旧 RADIO_STREAM progress 死数据清理（单独增量，C++ 端）；流式视频续播扩展（问题二方案 B，功能增强非收敛，需人工验证 mpv 网络流 resume）。

## D14-3b — is_streaming 去重（URLClassifier::is_local_file）（M2 · D14 读侧补遗）

**Context:** D14-3 收敛 TUI 读侧时拆出的代码质量增量（不阻 D14 收官）。`url[0]=='/'||file://` 本地/流式判定在 ASR 路径（app_input `:asr`/L 键 is_streaming、F 模式 is_local、app_remote asr_start）复制多处。D14-5 收官后补做。

**Decision: 抽 `URLClassifier::is_local_file(url)`，放 net/url_classifier，非 Utils/core。** ① 本地/非本地本就是 URL 分类关注点；② `classify()` 内部首分支**早已**做此判定（`url.compare(0,7,"file://")==0 || url[0]=='/'`），只是落进 RADIO_STREAM/VIDEO_FILE 分支、无法表达"本地"为独立类别——暴露为公共方法 = 提升既有内部判定为接口，零新依赖（纯函数）；③ classify 内部也改调它（DRY，本地判定 5 处→1 真相源）。

**Why 不放 Utils/core:** Utils 是横切文本/显示工具；本地文件判定是 URL 语义，与 classify/type_name/is_video 同域，放分类器内聚。若放 Utils 反割裂"URL 判定"的单一归属。

**Verification:** ctest 44/44、0-warning、pty 冒烟绿。`rfind("file://",0)==0` ≡ `compare(0,7,"file://")==0`（均判前 7 字符）→ classify 行为零变化；4 处调用点语义等价。

**修正:** 立项称"4 处 is_streaming"，核实为 3 处 is_streaming + 1 处 is_local（F 模式，多 `!local_file.empty()||`），共享本地路径子表达式——抽 is_local_file 后两类均受益。

## D14-5 — Favourites LINK 收敛（sync 指针身份 → URL 身份）· D14 收官（M2 第 6 人日，D14 增量5）

**Context:** D14-4 收敛持久侧。Favourites LINK 机制（收藏树节点引用 RADIO/PODCAST/ONLINE 等目标节点）：D14-5 原框架"shared_ptr 跨树引用 → MediaID(URL) 解析重建"基于 AUDIT §5/197 + P1-2。审计当前态（代码自 monolithic app.cpp 拆分后演进，AUDIT 行号失效）。

**Decision: 审计修正——大改前提已过时；收敛 sync 的最后一块指针身份。** ① `linked_node` 已是 `weak_ptr`（types.h:95，运行期缓存、不持有所有权）——AUDIT 197 的 shared_ptr 所有权债**已修**。② `link_target_url` 已是持久 URL 身份（types.h:97），favourites DB round-trip（persistence.cpp save:68/load:112），收藏创建时设为目标源 URL（app_subscriptions:435/532）。③ `find_node_by_url` URL 解析重建已在 expand_link_node Step 1（target 过期则按 URL 重建）。④ P1-2 parent 不变式**已修**（shared children 不重设 parent，app_tree_expand.cpp:188/232/266 注释）。⑤ 真正剩余：sync_link_node_status:85 用指针相等 `linked.get()==target.get()` 匹配 LINK→target——target 被 Step 3 按 URL 重建后 weak_ptr 指向旧对象→指针失配→漏 sync。改 URL 身份 `link_target_url==target->url`（OR 指针快路径→严格无回归）+ 命中刷新 linked_node。

**Why OR 指针快路径而非纯 URL:** 零回归。指针相等时必命中（含 online_root 等合成键，target url 可能 ≠ link_target_url）；URL 身份补重建场景。OR 是原条件超集。online_root 经 load_search_history 另路、通常不进 sync，但 OR 兜底。

**Why 不抽 resolve_link_target helper / 不大改:** expand 的 URL 解析仅一处使用（find_node_by_url 单定义单使用）；抽 helper 是 used-once YAGNI churn，铁律禁过度设计。sync 指针→URL 是实质身份收敛（重建后漏 sync 的 bug 类）。

**Gotcha — AUDIT 行号失效:** AUDIT_REPORT 的 L10990/L13002 等是旧 monolithic app.cpp 行号；代码已拆入 app_tree_expand.cpp / app_subscriptions.cpp / persistence.cpp。D14-5 立项时的"工作量最大"判断基于过时 AUDIT——审计当前态后，大改已在前序（pre-D14）演进中完成，D14-5 实质工作仅 sync 一处。

**Acceptance:** ctest 44/44、0-warning（2 单元重链：app_tree_expand）、pty 冒烟 exit 0 + clean endwin。sync 重建场景 pty 测不出——审查确认 + OR 无回归。

**Followups（不阻 D14 完成）:** D14-3b is_streaming 去重；流式项 resume gap（键统一后可扩 resume 到流式项）；§5.2 测试镜像漂移。**D14 收官。**

## D14-4 — 持久侧收敛 progress/player_state 键 → canonical 源 URL + 无损迁移（M2 第 5 人日，D14 增量4 · 持久侧）

**Context:** D14-3 收敛 TUI 读侧。持久侧 now-playing 身份：history 键于 `orig_url`（源 URL），但 progress + player_state 键于 `player_state.current_url`（= mpv 播放路径；Y23.9 起缓存项=本地缓存路径）。审计：save_progress(app_run:488 current_url) / get_progress(playback_service:487 local_url) / save_player_state(app_run:479 current_url)。同一源流式（current_url=流 URL）与缓存（current_url=本地路径）产生双键——progress 取决于"怎么播"而非源身份。

**Decision: progress/player_state 键收敛到 canonical 源 URL + 一次性无损迁移。** ① save(app_run save 函数)：`canonical_url = playback_.now_playing().id.url()`（D14-2 访问器），替 `player_state.current_url` 作 save_player_state + save_progress 的键（守卫/日志同改）。② read(playback_service:487)：`get_progress(orig_url)`（替 local_url；orig_url 在作用域 line 436）。③ 迁移(database.cpp)：SCHEMA_VERSION 47→48，gated `stored_version<48`，progress/player_state 行 url 若是缓存路径则经 media_cache 反向 map（local_file→url）re-key 为源 URL。④ `set_resume_position(local_url)` 不动（mpv watch-later，键=实际播放文件）。

**Why progress/player_state 而非 history:** `record_play_history` 全 5 调用点（playback_service:261/272/284/498/515）均传 orig_url → history 早键于源 URL，无需收敛。原 ROADMAP "save_progress/history 以 MediaID 为键" 的 history 部分是误判——审计修正。

**Why 带迁移（非接受一次性丢失）:** 用户数据跨版本必须无损。迁移经 media_cache 反向 map re-key，仅对仍缓存的项可行（缓存已清→无文件→本不可续播）→ 对所有可恢复数据无损。player_state 单行也迁（首次重启续播按源 URL re-feed progress）。纯 SQL 关联 UPDATE（无 C++ map/新 include），幂等非破坏（仅 UPDATE url，不删行）。

**Why 不动 resume 守卫:** playback_service:486 `if(!local_url.empty())` 使 resume 仅缓存项触发（既有行为）。键源 local_url→orig_url 对缓存项结果等价；流式项 progress 存而不读是独立 gap（可选后续：键统一后可扩 resume 到流式项，radio 守卫已在）。

**Gotcha — 迁移隔离验证:** 迁移嵌入 `DatabaseManager::init`（非独立可测函数）；ctest 用 fresh DB（stored_version=0→gate 跳过）不覆盖。故隔离验证：临时 DB 三场景（缓存项 re-key+保 position / cache 已清保持 / 已是源 URL 不动）+ 幂等，全过；真实 DB user_version 47→48、progress 行源 URL 键确认。

**Acceptance:** ctest 44/44、0-warning（4 单元重链：database/playback_service/app_run）、pty 冒烟 exit 0 + clean endwin。迁移隔离验证通过 + 真实 DB 落地确认。

**Followups:** 流式项 resume gap（可选行为变更）；D14-5 Favourites LINK；D14-3b is_streaming 去重。

## D14-3 — TUI 读侧收敛 current_url → canonical now-playing 源 URL（M2 第 4 人日，D14 增量3 · 读侧）

**Context:** D14-2 让 PlaybackService 暴露 `now_playing()` canonical Media、remote 快照首消费。TUI 读侧仍直读 `MPVController::State::current_url`（= mpv 播放路径；Y23.9 起缓存项给 mpv 传**原始本地路径**非 file://，故 current_url 是本地缓存路径）。读点：tree_renderer:59 高亮比 `item.node->url == current_url`、status_bar:60、info_panel:392/398 "Streaming URL"、lyric_renderer:15 逐轨重取触发。

**Decision: canonical 身份经 DisplayContext 视图模型推入；3 显示/高亮读点收敛，lyric 触发保留。** ① `DisplayContext` += `now_playing_url`，App 每帧 `playback_.now_playing().id.url()` 灌入（复用 D12-1 dctx 推入链、与 D12-3c IFrontend 可换一致——UI 不持 PlaybackService 引用）。② tree_renderer 高亮：ui.cpp:601 传 `dctx.now_playing_url`（替 `state.current_url`）→ **修缓存项高亮 bug**（`node->url`=源 URL ≠ `current_url`=本地路径，此前永不点亮）。③ status_bar:60 → `dctx.now_playing_url`。④ info_panel → `playback_node->url`（已收该参数，fallback current_url 无节点边界）。⑤ lyric_renderer:15 **保留** current_url + 注明。

**Why 经 DisplayContext 而非 UI 直调 now_playing():** UI 是纯呈现层、经 IFrontend 抽象（D12-3c 可换 Qt）；canonical 身份属"App 每帧解析、UI 只渲染纯值"的视图模型（与 D12-1 解耦 SleepTimer/OnlineState/TikTokRegion 同模式——§2.1：UI 不得自查运行时状态）。status_bar 已收 dctx（零新增管线）；tree_renderer 经调用点传 `dctx.now_playing_url`（draw() 有 dctx 作用域）；info_panel 经已有 playback_node（== dctx 值同源）。

**Why lyric 不收敛:** `update_lyric_history` 是 IFrontend 契约方法（形参 `const State&`），current_url 仅作逐轨变化检测（非显示）。播放路径逐轨必变 → 触发语义保持；canonical 源 URL 零行为收益，改它须动契约签名。注明防后混淆。

**Gotcha — info_panel 不经 dctx 而经 playback_node:** draw_info 已收 playback_node 参数（F32 接的）；`playback_node->url` == `playback_.now_playing().id.url()` == `dctx.now_playing_url`（同源节点）。就近用已有参数，免给 draw_info 加 dctx 形参。两表达同值。

**Acceptance:** ctest 44/44、0-warning（6 单元重链：frontend.h + app_run/ui/status_bar/info_panel/lyric_renderer）、pty 冒烟 exit 0 + clean endwin + quit dialog。无新行为，仅身份源切换。`current_url` 在 src/ui 残留仅 lyric_renderer:15/17（有意保留，注明）。

**Followups:** D14-3b is_streaming 去重（ASR 路径，从本增量拆出）；D14-4 持久侧；D14-5 Favourites。

## D14-2 — PlaybackService canonical now-playing Media + remote 快照首消费（M2 第 3 人日，D14 增量2 · 首次接线）

**Context:** D14-1 建好逻辑身份 MediaID/Media（零生产接线）。本增量首次接线：PlaybackService 暴露 canonical now-playing Media，让读侧用 canonical 源 URL（node->url）替 `MPVController::State::current_url`（= 播放路径，缓存项为本地文件路径——非规范身份）。原 D14 计划写"PlaybackTrackChanged 携带 Media + SubtitleService 订阅取 Media"。

**Decision: 修正原计划——事件不变，canonical Media 供读侧；首个消费者=remote 快照。** ① 审计 `PlaybackTrackChanged`(playback_events.h:24，`{TreeNodePtr node; AppMode mode; bool has_video}`) 唯一订阅者：SubtitleService(subtitle_service.cpp:57) `begin_track(e.node, e.has_video)`——它**需 TreeNodePtr**（读 subtitle_url/has_subtitle/asr_srt_path/local 字幕解析）。Media 是窄视图（id/title/art_url/is_video，无 node），事件改携 Media 会切断字幕 setup。故事件保持 {node,mode,has_video}（字幕通道独立、正确）；canonical Media 是**另一条**给读侧/持久侧的身份。② PlaybackService 新增 `Media now_playing() const`（media_from_node(playback_node_) 派生 id/title/art_url + 设 is_video）+ `now_playing_is_video_`（TreeNode 无 is_video 字段，单独存）；play_current(440)/on_playback_ended(215) 随 playback_node_ 赋值时同步设。③ remote 快照(app_remote.cpp) title/url/has_video/art_url 从双源（ps + playback_node）统一到 now_playing()；修 s.url 本地路径 bug；ps 兜底无节点边界。

**Why now_playing() 派生而非存 Media 成员:** playback_node_ 已是权威源节点；即时派生避免 url/title 三份拷贝与 node 漂移；仅 is_video（非 TreeNode 字段）单独存。clear playback_node_ 自动使 now_playing() 失效，无额外同步态。UI 线程单写单读（与 playback_node_ 同），无需 mutex。

**Why 先 remote 而非 TUI:** remote 快照 now-playing 字段（title/url/has_video/art_url）正是 Media 形状 + 跨序列化边界（App→网络终端）——证明 Media 端到端、价值最直接。TUI 读侧经 DisplayContext 推入链路，留 D14-3。

**Why 修正原计划（事件不改携 Media）:** D14-1 立项时未察觉的约束。SubtitleService 需 node 是硬约束（字幕源解析依赖树节点字段）；强把事件改成 Media 会逼字幕另寻 node（回耦合 PlaybackService::playback_node()，撤销 D9/D10-3 的事件化解耦）。正确切分：事件=字幕通道（携 node），canonical Media=读侧/持久侧身份。两条独立、各得其所。

**Gotcha — has_video 取自 PlaylistItem.is_video:** play_current(427)/on_playback_ended(217) 在 playlist_mutex_ 锁内已 snapshot `current_playlist_[idx].is_video`（建队列时 URLClassifier 分类好）。now_playing_is_video_ 直接存此值，无需 now_playing() 时重算 URLClassifier（domain 层亦不可依赖 net/，D14-1 既定）。

**Gotcha — ps 兜底:** 直链播放等未经 PlaybackService::play_current 的路径不设 playback_node_ → now_playing() 返无效 Media → 快照回退 ps（current_url/title/has_video），保旧行为、无回归。

**Acceptance:** ctest 44/44、0-warning（18 单元重链——playback_service.h 加 media.h 触发）、pty 冒烟 exit 0 + clean endwin。PlaybackTrackChanged/SubtitleService 未改。now_playing() 调用点：app_remote.cpp 快照（首消费者）。

**Followups:** D14-3 TUI 读侧收敛（tree_renderer 高亮/status_bar/info_panel/lyric 经 DisplayContext 读 canonical Media）+ 4× is_streaming 去重（ASR 路径）。D14-4 持久侧。D14-5 Favourites。

## D14-1 — MediaID 逻辑身份重写（M2 第 2 人日，D14 增量1 · identity 模型）

**Context:** D14（Media 域从 TreeNode 收敛）的第一步是定 identity 模型。审计 now-playing 身份的真实载体：`MPVController::State::current_url`（`mpv_controller.cpp:1256` `state_.current_url = path`）——Y23.9 起给 mpv 传**原始本地路径**（非 file://），故缓存项的 current_url 是本地路径、`node->url` 是源 URL，二者不同形。该字符串身份被 15+ 处直读且无归一：持久化（`player_state_repo.cpp:40/67`、`app_run.cpp:485` save_progress）、显示（`tree_renderer.cpp:59` `url==current_url` 高亮、status_bar、info_panel、`lyric_renderer.cpp:15` 重取触发）、远程（`app_remote.cpp:140` `s.url=ps.current_url`）；本地/流式判定 `url[0]=='/'||file://` 在 `app_input.cpp:248/734/822`+`app_remote.cpp:525` **复制 4 份**。而 D4 的 `MediaID`（media.h）是**指针身份**（`TreeNodeWeakPtr`、`lock().get()==` 比较）——对持久化/跨会话/网络终端完全无效（指针随节点/进程消亡）。用户确认：身份以**真实绝对源 URL**为准。

**Decision: adapter 首次接线前把 identity 从指针改为逻辑身份（绝对源 URL）。** ① `MediaID` backing 由 `TreeNodeWeakPtr` 改 `std::string url_`（绝对源 URL）；`operator==`/`!=` 比 URL；`url()`/`valid()` 访问器；删 `lock()`/`node_`。② `Media{id,title,art_url,is_video}`——删冗余 `Media::url`（身份唯一源是 `MediaID::url()`，并存会双份漂移）。③ `media_from_node` 复制 url→id / title / art_url；新增 `media_id_from_url`（DB 行/线字段直接建身份）。④ is_video **不**在 media_from_node 派生（TreeNode 无此字段；派生需 URLClassifier=net/，domain/ 不可依赖 net/）→ 留 false，由 D14-2 的 PlaybackService 层填。⑤ 单测 3→5 例锁逻辑身份语义。

**Why 逻辑身份而非指针:** 用户描述的真实数据流（每次解析后结构化缓存入 DB、播放记录/状态联动 H/F/远程）要求身份跨 DB（持久）+ 网络（线）存活。指针随节点/进程消亡，对 history/remote 无意义；逻辑身份（绝对源 URL）正是 DB/history/remote 当前的 key——统一之即消三表示分裂。`RemoteStateSnapshot`(remote_protocol.h:31-33,46) 的 title/url/has_video/art_url **已是事实上的 Media**，收敛是显式化而非新增抽象。

**Why 现在改 identity 而非边接线边改:** D4 的 adapter 生产零采用（grep 验证 src/include 对 `MediaID`/`Media`/`media_from_node` 零引用、仅 `tests/test_units.cpp:15` 包含）。在首次接线前纠正 identity 模型成本最低（只动 header + 测试）、零行为风险；接线后再改要回溯每个采用点。strangler 铁律不阻止"纠正未部署的 adapter"——那正是其预留的演进空间。

**Why 不激进规范化 URL（小写 host/去 fragment/排序 query）:** 会改变现有 `node->url == current_url` 类比较的匹配语义、引入回归风险，且非 D14 目标。D14-1 身份 = 节点已携带的绝对源 URL、`operator==` 即精确串相等（与当前 url 比较语义一致、零行为变化）；规范化（如有需要）是独立关注点，按需后加。

**Gotcha — is_video 不是 TreeNode 字段:** types.h:59-149 的 TreeNode 无 is_video（它在 PlaylistItem:156）。has_video 当前由 PlaybackService 层 `is_youtube||URLClassifier::is_video(url)` 派生。故 media_from_node 不填 is_video——domain/ 不能依赖 net/ 的 URLClassifier。

**Acceptance:** ctest 44/44（+2 净增：`MediaID.IdentityIsUrl`/`EmptyIsInvalid`/`SurvivesNodeDestruction`/`Media.FromNodeCopiesFields`/`MediaID.FromUrlFactory`，替旧 3 例指针语义测试）、0-warning（仅 test_units 重链——主二进制零改动印证无生产接线）、pty 冒烟 exit 0 + clean endwin。src/include 对 `MediaID`/`Media`/`media_from_node` 零引用（grep 验证）。

**Followups:** D14-2 PlaybackService 持 canonical now_playing Media + PlaybackTrackChanged 携 Media + SubtitleService 订阅取 Media；播什么路径（缓存本地路径/源 URL）由 Media 派生（消除 4× is_streaming 复制）。D14-3 读侧、D14-4 持久侧、D14-5 Favourites LINK 收敛依次。

## D13 — Provider 化审计固化 + ParserRegistry 契约测试（M2 第 1 人日，M2 启动）

**Context:** roadmap M2 第一句"各 parser 确认 Provider 化（youtube/bilibili/itunes/rss/m3u/opml/tiktok）"读起来像"有 7 个 parser 待 Provider 化"。但 `IFeedParser` 契约（`feed_parser.h`）要求 `URLType supports()` + `TreeNodePtr parse(ParseInput{data,url})`——输入是"已抓取的 body"，输出是单个 feed 树。审计每个 parser 对此契约的真实形态，才能判断哪些该 Provider 化、哪些本就不该。

**Decision: 审计固化（文档）+ Registry 契约测试（代码）。** ① 审计结论：feed 形态 parser 已 Provider 化——`RSSParser`(RSS_PODCAST)/`OpmlParser`(OPML)/`YouTubeChannelParser`(YOUTUBE_CHANNEL) 经 `REGISTER_PARSER` 自注册，`app_run.cpp` 三处 `ParserRegistry::instance().create(cur_type)` 派发（YOUTUBE_CHANNEL/PLAYLIST、YOUTUBE_RSS/RSS_PODCAST、OPML）。以下**刻意不经** `IFeedParser`：`BilibiliParser`（WBI 签名 arc API + SESSDATA 凭证 → 静态方法 `parse_user_videos(sessdata,mid,url,title)`，输入非 body）、`ITunesSearch`（搜索 API 单例，头注释自承"not a feed parser"）、`parse_m3u`（IPTV 频道表加载器：在 `app_iptv` 动态抓 index/regions/countries .m3u + group-title 分组建树，非单一 feed）、TikTok（yt-dlp `--flat-playlist`，在 `app_run` `parse_tiktok_user_videos`）、`transcript_parser`（字幕关注点 → `SubtitleParserRegistry`）。`default` 分支直调 `RSSParser::parse` 是刻意的"未知类型尽力猜 RSS"回退（Registry 对未知返 nullptr，不能替代）。② 写进 `docs/ARCHITECTURE.md §2 表 + §3`。③ 新增 `ParserRegistry` 契约测试（`tests/test_units.cpp`，3 例）：`DummyFeedParser` 经公开 `reg()` 注册 → `create()` 派发 → 未注册类型 nullptr → 单例稳定。CMake test_units 加 `src/parsers/feed_parser.cpp`（依赖极轻）。

**Why 测试只锁契约、不锁注册清单:** 要断言"RSS_PODCAST 已注册"得把 rss_parser.cpp（libxml2）+ opml + youtube（network/yt-dlp）链进测试——重依赖、高风险，违背"小增量"。Registry 该回归守的是 reg/create/nullptr/单例**机制**，不是某 parser 是否注册（后者由生产代码的 `REGISTER_PARSER` 宏自证，且设计上可变）。故测试里真实 parser 不链入 → Registry 空、隔离验证机制。

**Why 不把 m3u/bilibili 强塞进 IFeedParser:** "确认 Provider 化"不是"把所有 parser 塞进同一个接口"。m3u 是频道表加载器（多 URL + 分组）、bilibili 是凭证 API——硬套 `parse(ParseInput{data,url})→TreeNodePtr` 是削足适履，制造虚假一致性。正确的是识别"哪些是 feed 形态"（已 Provider 化）+ 记录其余的正当理由。这正是"确认"（audit）的本意。

**Gotcha — default 分支不是 Provider 化缺口:** `app_run.cpp` default 分支 `RSSParser::parse(data,cur_url)` 直调 RSSParser，看似"该走 Registry 却没走"。但 default 捕获的是所有未识别 URLType（直链视频等），刻意向 RSS 猜测；改走 `create(cur_type)` 会因未知类型返 nullptr 而静默失败。故直调是**正确**的，非缺口。

**Acceptance:** ctest 42/42（+3 `ParserRegistry.*`）、0-warning、pty 冒烟 exit 0 + clean endwin。无生产行为改动（仅 +测试 + 文档）；§4 层间门绿。

**Followups:** M2 真正的工作 = **Media 域从 TreeNode 逐步收敛**（D14：D4 的 `MediaID`/`Media` adapter 让模块逐步传句柄而非裸 `TreeNodePtr`/URL——待选首个收敛起点）。D12-2 游标事件化仍 defer。

## D12-3c — App 经 IFrontend 持有 UI（UI 可换性就位 · M1 达成）（M1 第 23 人日，D12 增量3c）

**Context:** 3b 建好 `IFrontend` 契约、UI 实现它、字幕/App 层经契约说话，但 App 仍以具体类型 `UI ui;`(app.h:125) 作**值成员**——App 直接具名 UI 类型，UI 并不真正"可换"（换 Qt 要改 App 成员类型 + 所有 `ui.`）。M1 的终点是 App 经接口持有前端：构造时具名具体实现、使用时全经 `IFrontend`。

**Decision: App `UI ui;` → `std::unique_ptr<IFrontend> frontend_ = std::make_unique<UI>()`；src/app/ 的 `ui.`→`frontend_->` 机械重定向。** ① 成员改 `unique_ptr<IFrontend>`，构造点 `make_unique<UI>()` 是 App 唯一具名具体 UI 处（组合根 / 依赖注入的正确形态）。② `\bui\.` 词界 sed 58 处（`gui.`/`_ui.`/`UI.` 都不触——词界安全；src/app/ 中 `ui` 仅作成员访问）。③ `subtitle_.poll(ui, ..)` 的 bare-ui（按引用传、无 `.`）手动改 `poll(*frontend_, ..)`——src/app/ 中唯一的 bare-ui-按引用传成员（字幕服务 `poll` 形参 `IFrontend &ui` 是局部参数，非成员，sed 不触、保持转发）。

**Why 保留 UI::is_input_cancelled（12 处静态）:** 它检查 ncurses input_box 的 `INPUT_CANCELLED` 标记（`\x01CANCELLED\x01`）——是**输入契约**（input_box 如何表态取消），非渲染/状态关注点。Qt 后接时其 input_box 须返回同标记（或改契约为 std::optional）。把它搬上契约是独立关注点（输入契约清理），不属"App 经接口持有 UI"这一所有权切换；留作后续可选清理。故 App 名具体 UI 仅剩：构造点 + 这 12 处静态检查——二者皆非渲染/状态耦合，不阻 UI 可换。

**Why 不带 is_input_cancelled 一起搬（避免增量过大）:** 本增量的语义是"所有权切换"（App 经接口持 UI）。is_input_cancelled 搬迁改的是"输入取消契约"（input_box 返回值约定）——两个独立关注点，捆一起会把一个机械增量变成语义变更、放大风险。strangler 铁律：每增量一个关注点、绿。is_input_cancelled 上契约留作可选后续。

**Why bare-ui 只有 1 处需手动:** 预先审计 src/app/ 的 `\bui\b`（非 `ui.`）：除字幕服务局部形参外，唯一 bare-ui-按引用传成员 = `app_run.cpp:299 subtitle_.poll(ui, ..)`。该行同时含 `ui.is_lyric_bar_requested()`（会被 sed 改 `frontend_->`），故 bare-ui 部分需手动 `*frontend_`。无遗漏（grep `\bui[),]|[ ,]ui[),]` 仅此 + 字幕形参）。

**Gotcha — 析构序不变:** `frontend_`（line 125）声明早于 player_/playback_ 等，成员析构逆序 → frontend_ 晚于它们析构，与原 `UI ui;` 同位同序。App::~App 先 `player.stop()`/`pool_.shutdown()`/`subtitle_.shutdown()` 再析构成员的顺序不受影响（unique_ptr 析构调 `IFrontend` 虚析构 → UI 析构，等价原 UI 成员析构）。

**Acceptance:** ctest 39/39、0-warning（23/23）、pty 冒烟 exit 0 + clean endwin。App 具体 UI 引用 = 1 构造 + 12 静态 `is_input_cancelled`（grep 验证）；§4 层间门绿。**M1 达成（UI 解耦核心目标）**：UI 经 `IFrontend` 可换、ncurses 收敛 ui/+theme/、src/ui 零运行时状态查询。

**Followups:** M2（Provider 化 + Media 收敛）。可选：`is_input_cancelled`/`INPUT_CANCELLED` 迁 frontend.h（输入契约上契约，消除 12 处静态残留）；D12-2 游标事件化（defer，非 UI 可换必需）。

## D12-3b — IFrontend 抽象契约 + UI 实现 + 字幕层解耦（M1 第 22 人日，D12 增量3b · 前端可换契约就位）

**Context:** M1（UI 解耦）的终点是"UI 可换"——系统经一个 ncurses-free 抽象与前端说话，ncurses UI 是其实现，Qt/其它前端可后接同一契约。ncurses 已收敛（3a）之后，需要的就是这个契约。审计 App + 字幕 Application Service 对 UI 的**实际调用面**：src/app/ 的 `ui.` 25 个方法（input_box/dialog/confirm_box 输入设备 + draw + 一堆 lyric/theme/scroll/tree 开关与查询 + get_top_h/get_left_w 几何）+ 字幕 `set_transcript`（SubtitleManager::poll 经 `UI&` 调，TranscriptionEngine::poll 拿 `UI&` 但未用）= **26**。UI 是 App 直接成员 `UI ui;`(app.h:125)。

**Decision: 新增 `IFrontend`（ncurses-free，26 纯虚），`UI : public IFrontend` 实现之；字幕 `poll(UI&)`→`poll(IFrontend&)`。** ① `include/panicast/ui/frontend.h` 定义 `class IFrontend`（26 纯虚 = 实际调用面），并把契约所说的 ncurses-free 视图模型类型**迁入**：`DisplayItem`/`DisplayContext`（原 ui.h）+ `LyricManual`（原 UI 嵌套枚举）。契约的 include 全在 `ui/`+`theme/` 之外（types/mpv_controller/progress/subtitle_parser）→ ncurses-free；依赖方向 `frontend.h`（无 ncurses）← `ui.h`（ncurses）。② UI 26 方法加 `override`；私有渲染辅助（`draw_line`/`draw_status`/`draw_lyric_*`，带 `WINDOW*`）+ 静态 `is_input_cancelled` 留 UI 具体、不进契约。③ 字幕经契约说话：3 处 `poll(UI&)`→`poll(IFrontend&)`（头前向 + 实现签名），`.cpp` 的 `#include ui.h`→`frontend.h` → **modules/ 不再名具体 UI / 不再依赖 ncurses**。④ `library_service.h`（App 层 DisplayItem）include 由 `ui.h` 改 `frontend.h` → App 层不为视图模型拖 ncurses。

**Why 方法集 = 实际调用面（26），非 UI 全部 public:** 接口最小主义——只放跨层调用者真用的。`toggle_tree_lines`/`set_lyric_bar_requested`/`show_url_popup`/`current_theme_name`/`apply_theme` 等非跨层调用者（App 不经实例调、或纯 UI 内部）留 UI 具体。一个 Qt 前端要实现的只是这 26 个，契约越窄越易后接。

**Why 拆 3b（本增量）/ 3c（App unique_ptr）:** 3b 先证明"契约 + 继承 + 26 override + 字幕改经契约"编译 0-warning（最大不确定项），把 App 所有权切换（`UI ui;`→`unique_ptr<IFrontend>` + `ui.`→`frontend_->` 机械 sed + 1 处手动解引用）留给 3c 这步纯机械活。strangler 铁律：每增量绿；拆开各自更低风险。3b 已交付真实价值：契约存在 + modules/ 与 App 层不再为前端拖 ncurses。

**Why 把 DisplayItem/DisplayContext/LyricManual 迁入 frontend.h（而非前向声明）:** 契约以这些类型说话（`draw(const vector<DisplayItem>&)` 等）。它们本就 ncurses-free，放进 frontend.h 使契约自洽、不需 include ui.h（否则契约又拖 ncurses）。前向声明 + `vector<不完整类型>&` 在声明里属灰区/有风险；三个结构体极小，整体迁移最干净。`UI::LyricManual::X` 调用点（App 4 处）随之改 `LyricManual::X`。

**Why 默认参数基类+派生都留:** `draw`/`input_box`/`confirm_box`/`init` 有默认参数；具体 UI 调用点（如 App 的 `ui.draw(...)`）依赖这些默认。基类（IFrontend）与派生（UI override）保留**一致**默认值——经具体 UI 用 UI 默认、经契约用契约默认，合法（不同作用域非重定义）、0-warning。

**Why is_input_cancelled 不进 IFrontend:** 它检查 ncurses input_box 的 CANCELLED 标记（`\x01CANCELLED\x01`）——是 ncurses 输入契约的实现细节，Qt 前端自有其取消语义、不会用此标记。留 UI 静态。

**Acceptance:** ctest 39/39、0-warning（29/29）、pty 冒烟 exit 0 + clean endwin；IFrontend 全部 include 在 ui/+theme/ 之外（ncurses-free 契约）；§4 层间门绿。App 仍持具体 `UI ui;`（3c 切换）——故 M1"UI 可换性就位"待 3c。

**Followups:** D12-3c（App `unique_ptr<IFrontend>` 持有 UI，`ui.`→`frontend_->`，UI 可换性就位）→ **M1 达成**。D12-2（jump_to_match/reveal_node 搬 SearchService）defer。

## D12-3a — 收 ncurses：Core/config 零 ncurses 依赖（M1 第 21 人日，D12 增量3a · IFrontend 前置）

**Context:** D12-3（IFrontend）的 M1 验收明写"Core 不依赖 ncurses"。审计发现**此前并不成立**：`core/win_raii.h`（ncurses `WINDOW*` RAII：newwin/keypad/delwin）+ `config/ini_config.h`（`resolve_color` 颜色名→码映射用 `COLOR_BLACK`…`COLOR_WHITE` 宏）都 `#include <ncurses.h>`。core/ 另 4 处（utils.h / text_utils.cpp / process_utils.cpp / terminal.cpp）提及 ncurses 仅**注释**——它们用原生 termios/ANSI 转义直写 `/dev/tty` 绕过 ncurses（core 基础设施正确做法），零 ncurses API。故真依赖仅 win_raii + ini_config 两处。

**Decision: 把 ncurses 收敛进 ui/ + theme/（呈现层）；core/ 与 config/ 零 ncurses。** ① `core/win_raii.h` → `ui/win_raii.h`（`git mv`，内容不变）——ncurses WINDOW RAII 是纯 UI 关注点，归位 ui/（ui/ 可依赖 ncurses）；app.h include 路径更新。② `config/ini_config.h` 的 `COLOR_*` 宏换原生 int 字面量 0-7 + 删 `#include <ncurses.h>`——config 解析不再拉 ncurses。theme/colors.h 留（呈现层，非 Core，其 ncurses 可接受）。

**Why win_raii 搬非删:** WinRAII 当前无引用（grep 仅自引用；app.h include 它但未用——pre-existing 死 include）。但它是合法 UI 工具（ncurses 窗口 RAII），D12-3b 的 NcursesFrontend 可能用；搬去 ui/（它的本位）比删更可逆，core/ 同样达成 ncurses-free。死 include 清理留作后续。

**Why 原生 int 而非自定义命名常量:** `COLOR_BLACK`…`COLOR_WHITE` 是 ncurses 宏=0-7（curses API 规定）。换自定义常量要么与 ncurses 宏重名冲突（同 TU 含两者即重定义）、要么造新名再让 ncurses 侧映射=churn。原生 int 0-7 与 ncurses 值完全一致、零冲突，既有注释已明示 0-7=标准 ANSI。

**Acceptance:** core/ 零 ncurses API（grep 排除注释=空）；ncurses.h 仅 ui/+theme/ 引用；ctest 39/39、0-warning（48/48）、pty 冒烟 exit 0 + clean endwin。**M1 验收"Core 不依赖 ncurses"达成。**

**Followups:** D12-3b（抽 IFrontend 接口 ~25 方法，ncurses UI 实现，App 持 IFrontend&，UI 可换/Qt 可后接）→ M1 达成。D12-2（jump_to_match/reveal_node 搬 SearchService）defer——属显示编排/控制器职责，非 IFrontend 必需（D11-3b ADR 曾质疑其价值）。

## D12-1 — DisplayContext 视图模型：UI 不再自查 3 运行时 singleton（M1 第 20 人日，D12 增量1）

**Context:** D11-4 验收认定 UI 的"真耦合"是**自查运行时状态**（4 处 singleton 读：SleepTimer/OnlineState/TikTokRegion/URLClassifier），而非横切基础设施（Utils/LOG，已豁免）。D12 切第一刀。但审计 4 处时发现 URLClassifier 性质不同：它是**无状态纯函数**（表驱动 URL 串→枚举，无 `instance()`、无 I/O），与 Utils 同类——不是运行时状态查询。故真该解耦的是 3 个**有状态** singleton。

**Decision: 新增 `DisplayContext` 视图模型，App 每帧构建推进 `ui.draw()`，解耦 SleepTimer/OnlineState/TikTokRegion 三处；URLClassifier 作为纯函数保留。** DisplayContext 紧邻 `DisplayItem`（UI 既有视图模型），含 `sleep_active`/`sleep_remaining`/`online_region_name`（已解析名）/`tiktok_region`。`UI::draw`/`draw_status` 末尾加 `const DisplayContext &dctx = {}` 参数（默认值让单调用方可增量采纳）。§2.1 显式把 `URLClassifier::classify/is_youtube` 列入"允许的横切基础设施"白名单（§3 早把它列为基础设施，与之对齐）。

**Why URLClassifier 不解耦（纯函数判据）:** `static constexpr` 模式表 + `static URLType classify(const string&)`——无实例、无状态、无 I/O。UI 对 URL 字符串调它选图标，等价于对字符串调 `Utils::truncate_*`：横切基础设施，稳定依赖原则下 UI 可依赖。剥离它（pre-classify 进 TreeNode/DisplayItem）要给每个节点预算 `url_type`，徒增字段+缓存复杂度，零架构收益。

**Why online_region_name 在 App 解析（而非传 region 码让 UI 调 get_region_name）:** 若传码，UI 仍要调 `ITunesSearch::get_region_name`（parsers/ 静态方法）——把 net/parsers 调用留在 UI。故 App 一次性解析成名，UI 只渲染纯字符串。代价：每帧多一次 `get_region_name` 表查（仅 ONLINE 模式 title 原本才调，现每帧调）——纯查可忽略。

**Gotcha — sleep_remaining 镜像原守卫:** 原 status_bar 只在 `is_active()` 时读 `remaining_seconds()`（避免不活动时的语义）。App 构造 dctx 时同样 `sleep_remaining = sleep_active ? remaining_seconds() : 0`——UI 只在 `dctx.sleep_active` 时读 sleep_remaining，等价且安全（app_remote.cpp:164-165 早已用同一守卫模式）。

**Acceptance:** src/ui 零 `SleepTimer::|OnlineState::|TikTokRegion::|ITunesSearch::` 调用（grep 验证，仅剩注释散文）；ctest 39/39、0-warning（26/26）、pty 冒烟 exit 0 + clean endwin。

**Followups:** D12-2（游标事件化 → jump_to_match/reveal_node 无反向依赖搬 SearchService）→ D12-3（IFrontend 抽象）→ M1 达成（UI 解耦）。

## D11-4 — UI 层依赖不变量确立：基础设施豁免（M1 第 19 人日 / D11 收官）

**Context:** D11 标题原写"移除 UI 对 Core 的**全部**直接调用"。D11-4 grep 审计 UI→Core 实际调用面：仅 {`Utils::*` 文本/显示工具(~100处)、`LOG`/`EVENT_LOG`(16处)、`get_emoji_width`(2处)}——全是横切基础设施；Core **业务**直调（Paths/crypto/ThreadPool/EventBus/process_utils/safe_tmp）= 0。问题："全部"是否含 Utils/LOG？用户要求参照同类软件给出专业建议。

**Decision: 选 A（基础设施豁免）——LOG/Utils 不剥离，纠正 D11 标题为正确不变量。** 新不变量（稳定依赖原则）：**UI 只依赖稳定基础设施 + 视图模型，不直调 Core 业务/运行时状态。** 写进 `docs/ARCHITECTURE.md §2.1`；把"UI 零 Core 业务直调"固化进 `scripts/check.sh` §4（grep 门）。Utils/LOG/get_emoji_width 列入白名单（允许）。

**Why LOG 不剥离（同类软件佐证）:** 日志是教科书级**横切关注点**（AOSD）——所有主流项目界面层都直接用：mpv（`mp_msg` 在 OSD）、cmus（`ui_curses.c` 直接调日志+utils）、Qt（`qDebug`/`qCInfo` 在 View）、LLVM（`errs()`/`LLVM_DEBUG`）、Chromium（`LOG` 在 views/）。无一项目把日志对 UI 藏起来。剥离横切基础设施 = 给文件改名，**零耦合收益**（UI 仍在调同一函数）。且本项目 `EVENT_LOG` 本就是 UI 右侧日志环形缓冲（ARCHITECTURE §3）——它是 UI 自己的面板。

**Why 纠正标题而非照搬:** 旧标题与项目自身分层定义冲突——`docs/ARCHITECTURE.md` §2 明写 `core = 基础设施`，"基础设施"即供各层使用。照"移除全部 Core 直调"执行 = 把 printf 式工具赶出 UI = cargo-cult 分层。正确判据是**稳定依赖原则**（Robert Martin）：UI 可依赖不随业务变的稳定基础设施，不可依赖随业务变的运行时状态。Utils/LOG 稳定 → 可依赖；DB/网络/账号/播放 singleton 易变 → 不可依赖。

**Gotcha — 真正的耦合在别处（非 Core）:** UI 有 4 处 net/playback/app singleton 读（`SleepTimer::instance()` / `OnlineState::instance()` / `URLClassifier::classify` / `TikTokRegion::current`）——UI **自查**业务/运行时状态而非**收视图模型**。这才是 mpv/Qt/cmus 式该解耦的真耦合，但属 D12 `IFrontend` 范畴，不在 D11-4 的"Core 直调"判据内，本轮仅记录、不门控（grep 门只盯 Core 业务符号）。

**Acceptance:** UI 零 Core 业务直调（grep 门绿）；ctest 39/39、0-warning、冒烟绿（无 C++ 改动，沿用 D11-3c 15df9ab）。D11（1/2/3/4）收官。

**Followups:** D12（`IFrontend` 抽象 + 4 singleton→视图模型解耦 + 游标事件化，届时 jump_to_match/reveal_node 可无反向依赖搬 SearchService）→ **M1 达成（UI 解耦）**。

## D11-3c — 库 load_*_root + 帐号 mode handler 搬迁（M1 第 18 人日，D11 增量3c / Round 3）

**Context:** D11-3c 原任务"库 load_*_root + 帐号 mode handler 搬进 LibraryService"。LibraryService 已拥每模式树数据（D10-4）+ 视图态/tree_mutex（D11-2），但**构造这些 root 的方法仍散在 App**（app_run/account/bilibili/tiktok/iptv/search 各文件）——库域拥数据却不拥构造，分裂。用户在 6 个全搬（A）vs 分批（B）间选了 **A（6 个全搬，一提交）**。

**Decision: 6 个 load_*_root + 2 helper 一次性搬进 LibraryService，逐字搬 + 内部成员访问反转。** bodies 行为不动，只改访问：`library_.X_root()`→`X_root_`、`library_.tree_mutex()`→`tree_mutex_`、`library_.X_loaded()`→`X_loaded_`。调用点（17 处）机械重定向 `library_.X()`。

**Why 全搬（A）不分批（B）:** 6 个方法同质（都是"从数据源建一个 mode 的 root 列表，tree_mutex 保护"），同一领地同一模式，一提交完成让"库域拥数据+构造"一步到位、不留半搬中间态；分批只增 commit 数不降风险（每个都是逐字搬 + 机械重定向，pty 测不出树构建正确性，风险面相同）。

**Why load_bilibili_accounts 设 public（非 private helper）:** 它有 1 个"搬走"的调用方（load_bilibili_root）+ 3 个"留下"的调用方（App 的 expand_bili_followings / expand_bili_history / perform_bilibili_search 都要解密后的 SESSDATA）。若 private 则 3 个 App op 无法访问；故 public，App 经 `library_.load_bilibili_accounts()` 调。core/crypto.h（token_open/seal/machine_key）随它进 LibraryService——crypto 在可复用 core/，layering OK（service→core 向下）。

**Why make_search_history_child 搬、load_tiktok_accounts 内联:** make_search_history_child 有 2 个调用方（load_accounts_root 搬走 + expand_bilibili_account 留 App），纯节点构造，搬进 LibraryService 两边都经 `library_.make_search_history_child()`。load_tiktok_accounts 只 1 个调用方（load_tiktok_root，搬走）、且是一行 `DatabaseManager::list_tiktok_accounts()`——直接内联，不留单调用方的一行 static。

**Gotcha — 帐号 mode 无 AccountService:** D10-5 决定不为 Account 建 Service。load_accounts_root（Y-mode 树构建，UI-free）搬进 LibraryService（库域统一拥树构造）；UI 耦合的帐号 op（start_account_login QR / 激活 / delete_account_node）留 App——它们调 `library_.load_accounts_root()` 刷新树。

**Gotcha — 行为零变化靠 tree_mutex 是同一对象:** D11-2 已把 tree_mutex 搬进 LibraryService（搬非拷）。本增量方法体内的 `lock(tree_mutex_)` 与 App 调用点的 `lock(library_.tree_mutex())` 锁的是**同一把** recursive_mutex——锁序、锁范围、线程可见性全不变。构造/析构序不变（library_@133 析构晚于 pool_@160，锁存活过工作线程）。

**Followups:** D11-3（3a/3b/3c）全完 → D11-4（grep 验证 UI 层零 Core 直调）→ D12（IFrontend 抽象 + 游标事件化，届时 jump_to_match/reveal_node 可无反向依赖搬 SearchService）。手动验证（用户侧）：6 模式树构建（Y/B/T/I/H/R）正确性 pty 测不出。

## D11-3b — 搜索算法收口进 SearchService（M1 第 17 人日，D11 增量3b / Round 2 / model-view 分离）

**Context:** D11-3b 原任务"搜索 jump/reveal 搬入 SearchService"。但 jump_to_match/reveal_node 是**显示编排**（依赖 `mode`/`cur_items()`/`flatten_items()`/`LINES`，直接改写 display_list/selected_idx/view_start），D10-2 头注释明确要求**游标事件化后**才能整体搬——否则 SearchService 须引入 `LibraryService*` 反向依赖 + mode/flatten 回调（目前无服务持有跨服务 LibraryService 引用）。且 pty 冒烟测不出搜索正确性。用户经讨论选了"搜索只负责搜索，折叠/展开是另一回事"的原则。

**Decision: model/view 分离——只搬显示解耦的纯算法，显示编排留 App。** 对应主流 TUI（ranger/cmus/less）：搜索=产出匹配列表+游标（model），reveal/拍平/滚动=视图（view）。SearchService 加 3 个方法（`search_recursive`/`collect_context_matches`/`cycle_match`），全部以参数传树/游标、不持 LibraryService*、不碰 display_list/LINES。perform_search/jump_search 改调它们；jump_to_match/reveal_node/**Shift-N(jump_to_playing) 原样保留**。

**Why 不整体搬（拒绝字面搬迁）:** 整体搬 jump_to_match/reveal_node 须给 SearchService 加 `LibraryService*` + 当前 `mode` + flatten 回调 + LINES——引入新的跨服务反向依赖（破坏分层：服务不该往回依赖控制器/视图），且触及 pty 测不出的搜索正确性路径。在游标(selected_idx)仍由 App 直接改写、未事件化前，搬它得不偿失。

**Why collect_context_matches 逐字搬 + 加防御 clear:** F20 上下文优先匹配的 4 段顺序（同级同类型→同级异类型→兄弟子树→全局）+ 去重是搜索核心语义，必须逐字不动。collect 开头加 `search_matches_.clear()` 是防御性的（reset_search() 已清过，再清是 no-op）——行为等价，但让方法自洽（不依赖调用方先 reset）。total_matches_ 由 collect 内部设（原 perform_search 在锁块外 `set_total_matches`，现并入方法、仍在调用方锁保护下）。

**Gotcha — 游标(cursor)由 App 读、锁由 App 持:** cursor 来自 display_list[selected_idx]（视图态），必须 App 读（SearchService 不碰 display_list）；roots=cur_items()（mode 派发）也由 App 传。两者皆树态，collect_context_matches 在 App 的 tree_mutex 锁块内调用——SearchService 方法本身不加锁（调用方持锁），与原 perform_search 锁范围一致。

**Gotcha — jump_to_playing(Shift+N) 不动:** 它是"任意界面跳回播放节目的模式+节点"，非搜索逻辑（不改 search_ 状态），与本轮 model/view 分离无关，原样保留。

**Followups:** jump_to_match/reveal_node 搬迁待游标事件化（D12 IFrontend / cursor 事件）后做——届时 SearchService 经事件订阅视图态、无反向依赖。下一步 D11-3c（库 load_\*_root + 帐号 mode handler 搬迁）。

## D11-3a 实现 — 字幕/ASR 编排收口 + "本地字幕文件优先"（M1 第 16 人日，D11 增量3a / 敏感一刀 / Round 1）

**Context:** D11-3a 规划（见下）定了 4 处缺口收口方案。D11-2（视图态迁入 LibraryService）已解锁，L 键编排可经访问器读节点、搬进 SubtitleService。本增量实现该收口——ASR 只在没有更廉价字幕源时才跑。

**Decision: 两件新东西 + 三入口收口。** ① `SubtitleManager::find_local_subtitle(node)` 统一本地字幕查找器（下载目录 `<sanitize(title)>.srt`/缓存本地文件 `<base>.srt` 优先，回退 `find_sidecar`），取代 `probe_sidecar`/`load_async` 里只查本地文件旁的旧逻辑（修缺口②）；② `SubtitleService::resolve_subtitle_source(node)` 返回 `ResolvedSubtitle{None,Embedded,LocalSrt,Online}`（优先链单一真相源）；③ L 键删内联 `find_local_srt` lambda + 两路内联链改调 resolver；④ remote `asr_start` 加本地检查（Embedded/LocalSrt/Online→`begin_track`，None→ASR，修缺口①）；⑤ `:asr` 保留 force 直调、注释澄清（修缺口③）。

**Why `:asr` 不走 resolve_subtitle_source（force 入参的替代）:** 规划 Gotcha 提过 "解析器需 force 入参"。实现取更简方案——`:asr` **根本不调** resolver，直调 `start_realtime`。force 是"绕过所有本地源"的语义，与其在 resolver 加 `bool force` 让 L 键/remote 传 false、`:asr` 传 true，不如让 force 路径绕开解析器（它本就是"无视解析结果"）。resolver 保持纯查询语义，`:asr` 是唯一不走它的入口——更清晰。

**Why remote LocalSrt/Online 走 `begin_track` 而非手写 sub_add/load_async:** `begin_track` 是 track 字幕编排的标准入口（video→Method A，audio→Method B，内部已调统一的 `find_local_subtitle`）。remote 检测到本地/online 源后调它，与 track-load 走同一路径，不另起一份编排逻辑（避免再散一份）。Embedded 单独处理（mpv 已在渲，`begin_track`(video) 会 reset Method B 略浪费）——仅记日志、不动。

**Gotcha — resolve_subtitle_source 的 None 隐含 node 非空保证:** resolver 开头 `if (!node) return r;`（None），故 LocalSrt/Online/Embedded 三种非 None 皆隐含 `node != null`。L 键分支因此可对 LocalSrt/Online 安全 `pn->has_asr_srt = ...`，无需重复 null 守卫。唯 None 分支可能带 null pn 落到 `start_realtime(pn, ...)`——与搬前等价（原 `find_local_srt` 对 null 返 ""、online 守 `pn &&`，最终同样 start_realtime(null)）。

**Gotcha — LYRIC 路的 Embedded 边界微宽:** 原 LYRIC embedded 守 `pst.has_video && player.has_active_subtitle()`；resolver 仅查 `mpv_->has_active_subtitle()`（audio 无内嵌 sub，故等价）。理论边角（has_active_subtitle() 真但 has_video 假）实践不发生，接受。

**Followups:** D11-3b（搜索 jump/reveal 搬入 SearchService）/ D11-3c（库 load_\*_root + 帐号 handler）→ D11-4（grep 验证 UI 零 Core 直调）→ D12（IFrontend）。手动验证（用户侧）：remote asr_start 本地优先、track-load 下载目录 SRT、L 键各路、audio/video 字幕方法。

## D11-2 — 视图态 + tree_mutex 迁入 LibraryService（M1 第 15 人日，D11 增量2 / 干净一刀）

**Context:** D10-4 把树数据模型（8 root + 6 loaded）搬进 LibraryService 时，`tree_mutex` 故意留 App——它同时守视图态 `display_list`/`selected_idx`（D11 领地），锁不与数据同处。D11-2 是 D11 的"干净一刀"：把视图态 + handoff + 锁一并搬进 LibraryService，为 D11-3 各 Service 方法体搬迁解锁（方法体改经访问器读视图态、不再直探 App 成员）。

**Decision: 搬 5 个成员进 LibraryService（display_list/selected_idx/view_start/tree_mutex/pending_select_）+ 访问器机械重定向，行为零变化。** 锁随它保护的数据/视图一起搬（同处）。访问器复刻 D8b-1/D10-4：数据成员非/常引用双载，tree_mutex 仅非常引用（mutex 不在 const 上下文锁定），int 双载（非/常）。

**Why tree_mutex 定点替换 `(tree_mutex)` 而非 `\b` blanket:** tree_mutex 在 src/app/ 有 119 处，其中大量是**注释散文**（"under tree_mutex"/"requires holding tree_mutex"）。blanket `\b` 会把注释也改成 "under library_.tree_mutex()"——读着错。实测**所有代码** lock 站点都是 `(tree_mutex)`（lock_guard 单行）或 `tree_mutex);`（跨行 lock_guard，`(` 在上行尾）两种形式，注释散文永非这两种字面。故定点两条 sed 注释零误伤。

**Why 跨行 lock 调用需第二条 sed (`s/tree_mutex);/.../g`):** 第一条 `s/(tree_mutex)/.../g` 只匹配 `(tree_mutex)` 同行；`std::lock_guard<...> lock(\n  tree_mutex);` 的 `(` 在上行尾、`tree_mutex);` 独占下行，不匹配。第一次构建即报 5 处 "tree_mutex not declared"（app_subscriptions×4 + app_search×1）暴露此——补第二条 sed 收齐。这是 sed 按行处理的固有限制。

**Why scope 限 src/app/（不触 src/ui/、remote_protocol.h）:** `view_start` 在 `ui.cpp`/`ui.h` 是 draw 函数**形参**、`selected_idx` 在 `remote_protocol.h` 是 `RemoteStateSnapshot` **成员**——同名异义。blanket `\b` 跨文件会误伤。sed 只作用于 src/app/*.cpp（同名异义文件不在内）；app_remote.cpp:150 `s.selected_idx = selected_idx` 的 LHS（struct 成员）误伤手修还原。

**Gotcha — 构造/析构序验证:** tree_mutex 现在是 library_ 的子成员（library_@133），析构序 reverse-declaration：pool_@160 先析构（join 工作线程）、library_@133 后析构（含 tree_mutex_）→ 锁存活过所有线程 join，与搬前等价（搬前 tree_mutex@157 也在 pool_@160 之后析构）。搬动不改锁存活期。

**Gotcha — library_service.h 现含 ui.h:** DisplayItem 定义在 `panicast/ui/ui.h`，library_service.h 为 `vector<DisplayItem>` 须 include 之。ui.h 不含 app/library 头（验证无环），但传递引入 ncurses 等——LibraryService（app 层）现传递依赖 UI 头，层度略瑕。DisplayItem 本质是 dumb 数据结构，未来可挪到 core/types.h 清理（非本增量 scope）。

**Followups:** D11-3a/b/c（各 Service 方法体搬迁——现可经访问器读视图态）→ D11-4（grep 验证 UI 零 Core 直调）。注意 D11-3a（字幕/ASR 编排收口）现已解锁。

## D11-3a 规划 — ASR "本地字幕文件优先" 缺口分析 + 收口计划（M1 第 14 人日续，规划/延后）

**Context:** 用户提出 "ASR 流程缺失了本地字幕文件优先的逻辑，需要补回迭代计划，并在合适时机实现。" ASR（whisper.cpp，离线 F-mode 产 .srt sidecar + 实时 L 键渐进）昂贵；**原则**：有本地字幕源就不该跑 ASR。优先级应为 内嵌 > 本地 ASR SRT > online 📜 transcript > ASR。深挖全部 ASR 入口 + track-load + worker 后定位四处不一致。

**Finding（四处缺口，详 `transcription_engine.cpp:299/449` worker 的 skip-existing-SRT 已具备、不重复）:**
- **① remote `asr_start`（`app_remote.cpp:509`）无本地字幕检查**——仅守 `has_media && pn && !realtime_running()`，有本地 SRT/内嵌字幕也照跑 ASR。**主缺口。**
- **② track-load `find_sidecar`（`subtitle_manager.cpp:121`）只查 `local_file` 旁（.json/.srt/.vtt/.lrc/.transcript），不查下载目录 `<title>.srt`**——而 L 键的 `find_local_srt`（`app_input.cpp:734`）查两处。`local_file` 为空则 `find_sidecar` 返回 ""，漏掉 L 键能找到的 ASR SRT。**两解析器不一致。**
- **③ `:asr`（`app_input.cpp:241`）注释写 "force ASR (skip online transcript)"，实则连本地 SRT 也跳**——语义应明确为 "force bypass all local sources"（显式 force，合理但注释/行为边界要清）。
- **④ L 键优先链（`app_input.cpp:756-822` VO-open 与 LYRIC 两路）内联在 UI 输入处理器**——embedded > `find_local_srt` > online > ASR 的链子是对的（✅ 全链），但未收口、与 ①②③ 各自为政。

**Decision: 不当下打补丁，收口进 D11-3a（字幕/ASR 编排统一进 SubtitleController）。** 把 L 键字幕编排（embedded>本地SRT>online>ASR 链）从 UI 输入处理器搬进 SubtitleService 的单一 `resolve_subtitle_source(node)` 解析器；**统一 `find_local_srt`（下载目录+本地旁）与 `find_sidecar`（仅本地旁）为一个解析器**（修缺口②）；三个 ASR 入口（L 键/`:asr`/remote `asr_start`）+ track-load 全部经此解析器，"有本地字幕源就不跑 ASR" 统一生效（修缺口①③④）。`offset`（`:z`/`:Z` sub-delay）随迁。

**Why 不当下打补丁（最小改 app_remote 加 find_local_srt 检查）:** 缺口的根因是**编排逻辑散在 4 处、各查各的**——给 remote 加一行本地检查只是把第 4 份拷贝补齐，下次再增入口又漏；且 `find_local_srt`/`find_sidecar` 两套不一致的解析逻辑本身需合并。D11-3a 正是"字幕/ASR 编排收口"的领地（D11 视图态迁移后 UI 不再直探 tree 耦合态、可干净搬 L 键编排）。当下打补丁违反 strangler-fig "敏感一刀留到 UI 解耦后"的纪律，且在冒烟测不出字幕正确性的前提下增加风险面。

**Ordering:** D11-3a 待 **D11-2（视图态 display_list/selected_idx/view_start + tree_mutex + pending_select_ 迁入 LibraryService）** 解锁后做——L 键编排经访问器读节点而非直探 tree，搬迁才干净。

**Gotcha:** `:asr` 是**显式 force**（用户明知要跑 ASR），收口后须保留 "bypass all local sources" 的 force 语义，不能被 resolve_subtitle_source 的"有本地源就 return"吞掉——解析器需 force 入参。

**Followups:** D11-2 先做（解锁）→ D11-3a 收口（统一解析器 + 4 入口路由）→ 人工验字幕/ASR（remote 触发、track-load 自动加载、`:asr` force、L 键全链）。

## D11-1 — PlaybackTrackEnded：字幕最后残留直调改事件（M1 第 14 人日，D11 增量1）

**Context:** D10-3 Step 2 把字幕加载事件化后，PlaybackService 仅剩 2 处 `subtitle_svc_->stop_realtime()` 直接调用（on_playback_ended 入口 + play_current 入口）——"曲目结束但不进阶"（error/stop，reason≠0）路径不发 PlaybackTrackChanged，ASR 仍须停，故 Step 2 保留为残留直调、注明 D11 切。

**Decision: 新增 `PlaybackTrackEnded{}` 事件，两处改 publish，SubtitleService 订阅 → stop_realtime。彻底删除 subtitle_svc_ 指针 + attach 的 SubtitleService 参数 + 前向声明 + include。** PlaybackService 现零 SubtitleService 引用——播放域完全不认识字幕，播放↔字幕所有直接耦合切断、全走总线（Changed 加载 + Ended 停 ASR）。

**Why 双 publish（on_playback_ended + play_current）而非折进 begin_track:** play_current 入口的 stop_realtime 是"手动换曲时 prompt 杀旧 ASR"——mpv 的 END_FILE 要到下一帧才 drain，若依赖 on_playback_ended 则旧 ASR 多跑一帧；折进 begin_track 则杀时机从入口(416)移到 publish(442)、产生（虽无害的）时序微变。本增量取**最低风险**：双 publish 保 exact 时序、不折进、零行为微变（这是冒烟测不出的 ASR 路径）。stop_realtime 幂等（已验证），advance 路径 Ended + 后续无重复杀问题。

**Gotcha:** PlaybackTrackEnded 在 play_current 入口 publish（"开始新曲"却发"结束"事件）语义上是"前曲被替换/superseded"——文档注明即可；订阅方语义明确（stop ASR）。EventBus 同步派发，线程/时序与旧直调等价。

**Followups:** D11-2（视图态 display_list/selected_idx/view_start + tree_mutex + pending_select_ 迁入 LibraryService，为逻辑搬迁解锁）→ D11-3（各 Service 方法体搬迁）→ D11-4（grep 验证 UI 零 Core 直调）。

## D10-3 Step 2 — 字幕加载事件化：Option B（自动进阶统一走 A/B 分支）（M1 第 13 人日续）

**Context:** D10-3 Step 1 把字幕编排逻辑搬进 SubtitleService 后（行为等价、触发仍命令式直调），Step 2 要换触发方式为事件订阅、拆 PlaybackService→字幕 直调耦合。深挖发现两个设计岔口：(1) `PlaybackTrackChanged{node,mode}` 不区分手动播放（begin_track 完整 A/B）与自动进阶（load_transcript 仅 Method B）；(2) `on_playback_ended` 入口 `stop_realtime`（杀 ASR）在"曲目结束但不进阶"时也要跑，而该路径不发 PlaybackTrackChanged。

**Decision: Option B（统一）——自动进阶重新识别节目类型、走与手动播放相同的 A/B 分支。** 用户拍板："下一曲自动重新识别节目类型，按当前曲逻辑走分支判断。" 即：两条路都 publish `PlaybackTrackChanged{node, mode, has_video}`，SubtitleService 订阅 → `begin_track(node, has_video)`。视频自动进阶从 Method B 升级到 Method A（mpv 渲染，与手动播放一致——修了"视频进阶显 LYRIC 而非烧字幕"的潜在不一致）；音频进阶仍 Method B（仅多一行 log）。

**Why 事件带 has_video 而非订阅方读 node->is_video:** `PlaylistItem.is_video`（= 手动播放/进阶的 A/B 判据）= `node->is_youtube || URLClassifier::is_video(url)`，**与 `node->is_video` 不同**。让 PlaybackService 计算好入事件，订阅方直接用——精确保留"重新识别"语义、订阅方不需耦合 URLClassifier。

**Why stop_realtime 留 2 处直接调用（不进事件）:** `PlaybackTrackChanged` 只在进阶/播放时发；"曲目结束但不进阶"（error/stop，reason≠0）路径无该事件，但 ASR 仍须停（防 whisper-cli 进程泄漏/陈旧字幕）。故保留 `subtitle_svc_` 供 on_playback_ended 入口 + play_current 入口两处 `stop_realtime` 直接调用（直调从 4 处降到 2 处）。**strangler-fig 残留**——加 `PlaybackTrackEnded` 事件即可彻底拆，留 D11。

**Why 不把 stop_realtime 折进 begin_track（验证过、可选但未取）:** `stop_realtime` 幂等（realtime_active_ 为假即 return），折进 begin_track 安全、可把残留降到 1 处。但为最小化这一**冒烟测不出**的敏感步骤的行为微变（折进后 play_current 的 ASR 杀时机从入口移到 publish 点），取保守方案：begin_track 不动、两处 stop_realtime 原样直调。

**Gotcha:** EventBus 同步派发（发布者线程）——publish 在 UI 线程，订阅方 begin_track 同步在 play() 前跑，时序与旧直调等价（Step 1 已用此特性）。has_video 默认初始化故 `{node,mode}` 聚合初始化的单测仍编译。

**Followups:** ① 人工验字幕（音频/视频/L键ASR/进阶重识别）；② D11 加 `PlaybackTrackEnded` 事件切掉 2 处 stop_realtime 残留直调；③ 字幕 L 键编排 + offset 等 input-side 解耦（D11）。

## D10-3 — SubtitleController：字幕编排反应式化（提前 M3），strangler-fig 两步（M1 第 13 人日）

**Context:** D10 收官时把 D10-3（字幕事件化）判为"增量收益、并入 D11"，因为 poll 是每帧状态机驱动、lyric_active 每帧多源派生，事件不能替代它们。但用户要求深入分析字幕+ASR 机制对照主流方案后再定。分析结论：D10-3 的真正价值**不在替代 poll/lyric_active**，而在**拆掉 PlaybackService→字幕 的命令式内联耦合**——这才是主流（mpv/VLC/ExoPlayer 反应式）与现状（命令式反模式）的差距。ASR 作为"字幕源"（渐进喂 segments）本身设计正确，其生命周期（切歌停、L 键启）天然归字幕控制器。

**Decision: 做成 SubtitleController（= 把 ROADMAP M3 的 SubtitleController 中介者提前），strangler-fig 两步。** 不做"半 D10-3"，而是正经把字幕编排（Method A/B 判定 + sub_add + load_async + stop_realtime）从 PlaybackService 搬进 SubtitleService，最终让 PlaybackService 只发 PlaybackTrackChanged、不认识字幕（D9 reactor 通道首个真实消费者）。

**Approach（两步，每步绿、可回退）:**
- **Step 1（搬逻辑、保触发）**：字幕编排 verbatim 搬入 SubtitleService 三方法（stop_realtime/begin_track[完整 A/B 块]/load_transcript[advance Method B]）；is_mpv_sub_url/basename_of 随搬。PlaybackService attach 改收 SubtitleService&、删两引擎裸指针、4 调用点改走方法（触发仍命令式直调，仅搬家）。行为零变化（字幕对象同一：init 传入的 pool/mpv/subtitle_mgr 即 PlaybackService 原用者——一个 MPVController、一个 pool）。
- **Step 2（换触发、拆依赖）**：PlaybackService 直调 → 只 publish 事件；SubtitleService 订阅 PlaybackTrackChanged 触发；删 subtitle_svc_ 指针。**字幕正确性冒烟测不出 → 人工放带字幕内容验证。**

**Why 两步而非一刀:** 字幕加载跨 playback↔subtitle 边界、有 has_video 分支 + sub_add 时序，一刀搬+换触发风险叠加且无法冒烟验证。拆开：Step 1 纯搬家（行为等价、绿即证明搬迁无误），Step 2 才动触发方式（你人工验字幕）。可回退点清晰。

**Why begin_track/load_transcript 分两个方法（而非一个）:** play_current 的 begin_track 是完整 A/B（reset+探+sub_add/load_async）；on_playback_ended 的 advance 只做 Method B load_async（无 reset/无分支）。两者语义不同——advance 路径的简化是**既有行为**（auto-advance 假设同质 track），Step 1 严格保持、不"修"。分方法让差异显式、Step 2 事件触发时也能区分（或评估是否该统一）。

**Gotcha:** stop_realtime 在 play_current/on_playback_ended **入口**（锁前）调用；Step 2 若搬进事件回调（publish 在锁后），时序微移（锁前→锁后）——多半无害，Step 2 验证时留意。

**Followups:** Step 2（换触发、拆依赖、人工验字幕）；其后字幕 L 键编排（多源 orchestrator）+ offset 等 input-side 解耦仍属 D11。

## D10-5 + D10 收官 — Account 勘察无需切割 + D10 所有权切割收官（M1 第 12 人日，D10 增量5 / 里程碑）

**Context:** D10-1/2/4 抽完 Subtitle/Search/Library 后，按 ROADMAP 还有 AccountService。需先勘察帐号域在 App 层的自有状态以定切割范围。

**Finding: 帐号域在 App 层无自有状态可切。**
- `AccountsManager`（storage/accounts.h:66-68）= 单例 `static AccountsManager& instance()`；app_account.cpp 全部访问经单例（list_accounts/add_account/delete_account/update_tokens/set_active_account/touch_login/load_subscriptions/load_history/get_tokens…）。
- `GoogleOAuth` = 静态工具类（`request_device_code`/`poll_token`/`fetch_identity`/`fetch_channel_videos`），无实例成员。
- 帐号数据 = 数据库 + 树节点（树节点 D10-4 已归 LibraryService）。
- App 唯一沾边的帐号态 = `tiktok_region_`（一字符串，T 模式区域码，INI 持久化）。

**Decision: 不造 AccountService（以"无需切割"结案 D10-5）。** 把单例/静态调用再裹一层 Service = 纯 churn（多一层转发、零封装收益——状态本就不在 App，调者照样直探单例）。与 D10 整体"把 App 自有域态外迁"的目标不符（这里无 App 自有态可迁）。帐号**逻辑**（app_account.cpp 的 Y 模式 Google 登录/订阅同步/历史、app_bilibili/tiktok/iptv mode handler）操作单例+树+UI，全方法搬迁属 D11（UI 纯交互化）。

**D10 收官判定:** D10 目标"各域状态各归其 Service"对**持 App 自有状态**的域全部达成——Playback（D8/D9）、Subtitle（D10-1）、Search（D10-2）、Library（D10-4）。Account 无 App 自有状态（单例/静态/DB 天然解耦）。App god-object 的域数据块全部外迁。**D10 完成。**

**Why 其余 D10 项归 D11 而非现做（不强行凑数）:**
- D10-3（字幕事件化）= 真实但**敏感**：SubtitleService 订 PlaybackTrackChanged 自动加载字幕，需跨 playback↔subtitle 边界搬 `load_async`（has_video 分支 + player_.sub_add），且 poll 是每帧状态机驱动、lyric_active 每帧多源派生——事件**不能替代**它们，只能补"track 变化→触发加载"的解耦。属 D11 首步（首个 D9 reactor 真实消费者），需随 UI 解耦一起验证。
- 各 Service 方法体/逻辑搬迁、视图态+tree_mutex+pending_select_ 迁入——全部被 UI 耦合（tree_mutex/display_list/selected_idx）挡住，是 D11 的定义性工作。
- 强行现做 = 要么造空壳 AccountService（churn），要么硬啃敏感的字幕事件搬迁（无法用冒烟验证字幕正确性、风险高）。两者都违反铁律"干净一刀 + 敏感一刀分拆、每步可回退、不 churn"。故诚实收官、把 D11 边界划清。

**Followups:** **D11（UI 纯交互化）**——① 视图态 display_list/selected_idx/view_start + tree_mutex + pending_select_ 迁入 LibraryService（锁与数据/视图同处）；② 各 Service 方法体搬迁（搜索/字幕/库/帐号逻辑）；③ 字幕事件化（首个 reactor 消费者）；④ grep 验证 UI 层无 Core 直调。→ D12（IFrontend 抽象 + 验证可换 UI）→ M1 达成。

## D10-4 — LibraryService 所有权切割：树数据模型搬所有权（M1 第 11 人日，D10 增量4）

**Context:** D10-1/2 把字幕/搜索搬入 Service 后，App 剩余的最大域数据块是树数据模型——8 个模式根 item 列表（`radio/podcast/fav/history/account/bilibili/tiktok/iptv_root`，共 175 处访问）+ 6 个 loaded flag（9 处）。它们与 `tree_mutex`（recursive_mutex，~120 处、11 文件共用）、视图态 `display_list`/`selected_idx`/`view_start`（~208 处）、`pending_select_`（UI handoff）同处一个耦合簇。完整搬迁（数据+锁+视图+方法）会是一次 500+ 处的大改，违反"干净一刀 + 敏感一刀分拆、每步可回退"铁律，且视图态/方法的真正归位要等 D11（UI 纯交互化），现搬=与 D11 双 churn。

**Decision: D10-4 只搬树数据（8 root + 6 flag），锁/视图态/handoff/方法全留 App（各附 D11 归属理由）。** 与 D10-1/2 一致的"干净一刀"：把 App 不再应持有的域数据外迁到 LibraryService，App 各访问点经引用访问器，机械重定向、行为零变化。这是 D10 收尾策略（所有权切割、为 D11 腾边界）的第四刀，也是迄今最大的一刀。

**Approach（引用访问器 + `\b` 词界 sed）:**
- 8 root：`std::vector<TreeNodePtr>& xxx_root()` + const 重载（复刻 SearchService 的 `search_matches()`）。覆盖所有用法：push_back / clear / size / empty / 下标 / range-for（值或引用）/ 取地址传参 / 别名引用——引用访问器一律胜任。
- 6 flag：`bool& xxx_loaded()`（写）+ `bool xxx_loaded() const`（读）。引用写访问器统一 `xxx_loaded = true` → `library_.xxx_loaded() = true`，避免 getter/setter 在赋值语境的非对称替换。
- **重定向工具=GNU sed `\b…\b`**，仅作用于 `src/app/*.cpp`（app.h 手改、library_service.h 不在作用域）。词界 `\b` 是安全关键：`radio_root` 是 `load_radio_root()` 的子串、`account_root` 与 `load_accounts_root()`（复数）形似——`\b` 保证只命中独立 token。逐 token 转换后计数与 sed 前逐项吻合 → 零遗漏零误伤。

**Why 不连 tree_mutex 一起搬:** 锁同时守数据（现归 LibraryService）与视图态 `display_list`/`selected_idx`（留 App、D11 领地）。现把锁迁入 LibraryService，它仍要守 App 的视图态（锁跨对象，且方向相反）；而 D11 会把视图态也迁入——届时锁跟随视图态同处更自然。故锁现迁=双 churn 且无同处收益，留 D11 一次性归位最省。当前中间态：`lock_guard lock(tree_mutex); library_.podcast_root().push_back(...)`——锁对象身份不变、加锁顺序不变、无死锁/竞态引入（行为零变化的核心保证）。

**Gotcha（验证）:** 初次用 `grep -oE "\bxxx\b"` 验证时被 `-o` 坑（只输出匹配子串、丢 `library_.` 上下文，converted 也被当 unconverted 报）。改用"converted-form 计数逐 token 比对 sed 前总量"+ PCRE 负向后顾查"非 `.` 前导的裸访问"双保险，方确认全转。

**Followups:** D10-5（AccountService 所有权切割，按 Y/B/T/I 模式切片，最大域）；D11（UI 纯交互化：视图态 display_list/selected_idx/view_start + tree_mutex + pending_select_ + 各 load_*_root 方法体迁入对应 Service，grep 验证 UI 无 Core 直调）。D10-3（SubtitleService 事件化）评估为低增量收益（poll 是每帧状态机驱动、lyric_active 每帧多源派生，事件无法替代），并入 D11。

## D10-2 — SearchService 所有权切割：树内搜索状态搬所有权（M1 第 10 人日，D10 增量2）

**Context:** D10-1 把字幕/ASR 引擎搬入 SubtitleService 后，继续抽下一域。原计划 D10-2 = "字幕按键→SubtitleActions 上总线"，但 D10 四域深入勘察后**重排优先级**：① 字幕的 `L` 键是多源编排器（非 D8a 式纯 toggle），且字幕加载 `load_async` 与播放/视频态强耦合（`has_video` 分支 + `player_.sub_add`）——其**输入/逻辑全方法搬迁**被播放耦合挡住；② Search 的逻辑体同样直探 UI 导航态（`jump_to_match` 改写 `selected_idx`/`view_start` + 持 `tree_mutex`/`display_list`）。结论：四域（字幕/搜索/库/帐号）的干净全方法搬迁都需先解 UI 耦合 → 属 **D11（UI 纯交互化）**。故 D10 现实收尾策略改为**所有权切割**：逐域把 App 的域私有态外迁到各 Service，为 D11 腾边界。Search 4 成员最局部、最干净，选作 D10-2。

**Decision: D10-2 做 SearchService 所有权切割（搬 4 个搜索成员 + 访问器重定向），方法体留 App。** 与 D10-1/D8b-1 一致的"干净一刀"：状态归位到 Service，App 各读写点经访问器，机械改名、行为零变化。全方法搬迁（`start_search`/`jump_search`/`jump_to_match`/`search_recursive` 搬进服务并解 UI 耦合）留 D11。

**Approach（4 成员 → 访问器）:**
- `search_query_` / `search_matches_`（`std::string` / `std::vector<TreeNodePtr>`）：暴露非/常引用访问器（写用 `search_.search_query() = q;`、`search_.search_matches().push_back(...)`）。
- `current_match_idx_` / `total_matches_`（`int`）：getter + `set_(int)` setter（不能用 replace_all——二者是 `set_...` 的子串，会腐蚀；逐点定向改）。
- `reset()`：一次清四态；`reset_search()` 体塌缩为 `search_.reset()`。

**Why 访问器而非门面（per-operation）:** 与 D10-1/D8b-1 同理——访问器零逻辑、机械、与既有先例一致，且是过渡缝（D11 逻辑搬迁后访问器调用自然消解）。本域无 inter-object 接线，故无 `init()`（区别于 SubtitleService）。

**Gotcha:** `start_search` 命中首跳传 `jump_to_match(0)`（字面量 0，非 `current_match_idx`），重定向时勿误改。`jump_search` 的环形索引 `((idx + dir + total) % total)` 全部经访问器组合，等价。

**Followups:** D10-3（SubtitleService 订 PlaybackTrackChanged 自动加载字幕 + publish SubtitleStatusChanged，增量收益、可并入 D11）；D10-4（LibraryService 所有权切割：8 模式根 + 8 loaded flag + tree_mutex，11 caller 逐文件改访问器，最大切片）；D10-5（AccountService 按模式切片）；D11（UI 纯交互化，届时搜索/字幕/库/帐号的逻辑体方可干净搬入各 Service）。

## D10-1 — SubtitleService 干净一刀：字幕/ASR 引擎搬所有权 + 生命周期（M1 第 9 人日，D10 增量1）

**Context:** D9 收官后 PlaybackService 独占全部播放运行时状态，但字幕/ASR（`SubtitleManager` + `TranscriptionEngine`）仍是 App 裸成员，被 ~16 处触发点（app_run 构造/析构/主循环、app_input×10、app_remote×3、app_download×2）+ PlaybackService（经 attach 裸指针）直接调用。D10 要抽更多 Application Service（Library/Search/Subtitle/Account）。四域勘察：Library=tree_mutex 11 文件共用的脊椎（HARD）、Account=最大最交织（HARD）、Subtitle=逻辑已自封装于两对象但触发点散落（MEDIUM）、Search=4 成员状态最局部但 jump/reveal 改写 selected_idx/view_start UI 导航状态（MEDIUM）。

**Decision: 先抽 SubtitleService，且 D10-1 只做"干净一刀"（搬所有权 + 集中生命周期 + 访问器重定向），不动逻辑。** 选 Subtitle 而非 Search：① 复刻已验证的 D8a/D8b PlaybackService 模式（own 对象 + 后续接 Action/事件），风险最低、收益最直接；② 触发点虽散落 15 处，但每处是**单行调用重定向**、无共享可变状态穿越（Search 的 jump/reveal 要改写 UI 导航状态，搬方法得穿 selected_idx/view_start，耦合更深）；③ 直接为 M3 SubtitleController 打底。

**Approach（复刻 D8b-1 访问器模式）:**
- SubtitleService 私有持两对象；`init(pool,mpv)` 内部 `transcription_engine_.init(&subtitle_mgr_, &pool, &mpv)`——把"引擎↔Sub­title­Man­ag­er"inter-object 接线收进服务（原 App 构造期 `transcription_engine_.init(&subtitle_mgr_,…)` 外泄了这个内部依赖）；`shutdown()`/`poll()` 转发。
- 访问器 `subtitle_mgr()`/`transcription_engine()` 作 D10-1 重定向缝（与 D8b-1 队列状态访问器、D9-2 track 手柄访问器一致）——App 各触发点 `subtitle_.subtitle_mgr().foo()` / `subtitle_.transcription_engine().foo()`，机械改名、行为零变化。D10-2/3 用 Action/事件替代这些访问器调用。
- **PlaybackService 一行未动**：`attach()` 仍收 `SubtitleManager&`+`TranscriptionEngine&`，App 在调用点传访问器（`playback_.attach(pool_, subtitle_.subtitle_mgr(), subtitle_.transcription_engine())`）。最小爆炸半径——不引入跨 Service 头依赖、不改 PlaybackService 的裸指针用法。

**Why 访问器而非门面方法（per-operation facade）:** 门面（每操作一方法）要写 ~15 个转发方法、易签名出错；访问器零逻辑、机械、与既有 D8b-1/D9-2 先例一致，且本就是过渡缝（D10-2 起 Action 化、D10-3 起事件化会逐步消解）。唯一"非纯访问器"是 `init()`——因其封装了 inter-object 接线（真实改进，非纯转发），`shutdown()`/`poll()` 配对作生命周期入口。

**等价性:** 纯所有权搬移——SubtitleService 持有的两对象与原 App 成员构造/析构序等价（默认构造、init 接线时机同、析构前 shutdown 同），所有调用经访问器转达到同一实例。无新逻辑分支、无线程变化、D4 不变量无关（字幕不经 on_playback_ended 线程路径）。

**Verification:** ctest 39/39；构建 0-warning（-Wall -Wextra -Wpedantic，19 文件含新 subtitle_service.cpp）；pty 冒烟 exit 0 + clean endwin。

**Followups:** D10-2（字幕按键 LYRIC/offset/ASR → SubtitleActions 上总线，SubtitleService 订阅，复刻 D8a）；D10-3（订 PlaybackTrackChanged 自动加载新轨字幕、解耦 PlaybackService 直调；publish SubtitleStatusChanged 让 D9 保留的 reactor 通道有首个真实消费者）；D10-4… Search/Library/Account。

---

## D9-3 — BUFFERING 手柄 + 状态机迁入 PlaybackService（M1 第 8 人日，D9 增量2b · D9 收官）

**Context:** D9-2 把"在播"track 手柄（playback_node/mode_）私有化进服务后，只剩 BUFFERING 手柄 `playback_pending_(_start_)` 还在 App。它与前两个不同：不仅被服务（play_current/on_playback_ended 经事件）写，还被 **app_run 每帧状态机直写**（mpv 报 has_media→清、30s 超时→清），且 30s 超时 + 一次性 buffering 时长日志逻辑嵌在状态机里——是 D4 现场（on_playback_ended 线程亲和）附近的敏感区。ROADMAP 原设想只搬状态 + 加 `playback_pending()`/`playback_pending_since()`/`clear_playback_pending()` 访问器、状态机读写改走访问器（仍由 App 驱动清除）。

**Approach（搬状态 + 整段搬逻辑，非仅访问器）:**
- `playback_pending_(_start_)` 迁入服务私有；新增 **`advance_buffering(bool mpv_has_media)→bool`**——把状态机的 pending 生命周期**整段逻辑**（置位/has_media 清除/30s 超时/时长日志）搬进服务，逐字保留。app_run 状态机精简为调用 advance_buffering + 从 mpv 派生 PLAYING/PAUSED。
- 私有 `set_buffering_(bool)` 单漏斗：写成员 + publish `PlaybackBufferingChanged`。play_current/on_playback_ended 的 3 处 publish 改走它；advance_buffering 的清除也走它（保证事件反映真实状态、单点变更）。
- App 删 pending 成员 + `PlaybackBufferingChanged` 订阅。

**Why 搬逻辑而非仅访问器：** 仅访问器方案（ROADMAP 原案）下 App 仍调 `clear_buffering()` 决定何时清——仍"写"播放状态（只是经方法），且 30s/时长日志留 app_run、状态分散。搬逻辑方案把 buffering 状态机**整体**收进服务（它是纯播放关注点：何时在加载、何时加载完、何时放弃），App 只剩"从 mpv 派生 PLAYING/PAUSED"（mpv 状态、非 pending），边界更干净，真正满足"App 不再直接写任何播放手柄"。搬迁是机械的（逻辑自含、无新分支）、逐帧 5-case 等价，且 advance_buffering 跑在 app_run 主循环 = UI 线程，**D4 不变量不受影响**（on_playback_ended 线程亲和未动；advance_buffering 是新方法、本就在 UI 线程跑）。

**等价性：** 见 CHANGELOG 5-case 表——advance_buffering 的返值 + App 对 has_media 的复检，与原三分支状态机在每个 (has_media, pending, 超时) 组合下产出同一 app_state、同一日志。set_buffering_ 的 publish 无订阅方（App 已退订）→ no-op，行为零变化。

**Verification:** ctest 39/39；构建 0-warning（-Wall -Wextra -Wpedantic）；pty 冒烟 exit 0 + clean endwin。

**Followups:** D9 完成——PlaybackService 独占全部播放运行时状态。下一步 **D10**（抽 LibraryService/SearchService/SubtitleService/AccountService + UI/remote 直订 track/buffering 事件，让保留的事件通道有真实消费者）。

---

## D9-2 — "在播"手柄迁入 PlaybackService 私有化（M1 第 7 人日，D9 增量2a）

**Context:** D9-1 把 D8b-2 回调缝换成总线事件后，"在播曲目"运行时手柄（`playback_node`/`playback_mode_`）仍是 App 成员、由 `PlaybackTrackChanged` 订阅镜像——App 事实持有状态、服务只 publish。这与"PlaybackService 是播放 Application Service、应拥有其全部状态"的 M1 目标不符。ROADMAP D9-2 原设想一把迁全部 4 个手柄（含 `playback_pending_(_start_)`）+ UI 展示事件化。

**Approach（只私有化两个单写者 track 手柄；pending_ 留 D9-3）:**
- `playback_node_`/`playback_mode_` 迁入 PlaybackService 私有；在 `play_current`/`on_playback_ended` 直接写（替 publish 作写回）；新增只读访问器 `playback_node()`/`playback_mode()`。
- App 删两个镜像成员 + `PlaybackTrackChanged` 订阅；所有读取点（draw/`'N'`/remote/ASR/复制 URL）改 `playback_.playback_node()`/`playback_.playback_mode()`。`'L'` 字幕块缓存局部 `pn`（含 `find_local_srt` lambda 经 `[&]` 捕获）。
- **`PlaybackTrackChanged` 保留 publish**：意义转为"外部 reactor（remote 推送 / 未来 UI 直订）通道"，单测守护。即 own-state + notify 模式——访问器供帧驱动的 draw 同步查、事件供异步 reactor。

**Why-not 一把迁 4 个手柄：** `playback_pending_(_start_)` 还被 app_run 每帧状态机**直写**（mpv 报 has_media→清 pending、30s 超时→清 pending），且 30s 超时逻辑嵌在状态机里。一把迁需给服务加 `clear_playback_pending()`/`playback_pending_since()` 等、改状态机读写、可能还要把超时逻辑搬进服务——动 D4 现场、耦合大。两个 track 手柄是**单写者**（只服务写）、读取点纯机械改名，干净低风险。按 D8b-1/D8b-2 的"干净一刀 + 敏感一刀分拆"原则，pending_ + 状态机收尾单列 **D9-3**。**不删事件**：删了会在 D10 remote 需要时重加、且破坏 D9-1 单测；保留 + 注释"暂无生产订阅方、D10+ 直订"更稳。

**等价性：** 服务在 UI 线程（D4 不变量保持）直接写私有成员，与 D9-1 经同步事件在 UI 线程更新 App 镜像——线程/时机一致；读取点本就在 UI 线程、改访问器不引入跨线程访问。行为零变化。

**Verification:** ctest 39/39；构建 0-warning（-Wall -Wextra -Wpedantic）；pty 冒烟（exit 0 + clean endwin）。

**Followups:** D9-3——`playback_pending_(_start_)` 迁入服务（私有 + 清/读接口），app_run 状态机读写改走 service、评估收进 30s 超时逻辑，删 App 的 `PlaybackBufferingChanged` 订阅；随后 D10 抽更多 Services + UI/remote 直订事件。

---

## D9-1 — 播放事件上总线，替代 D8b-2 回调缝（M1 第 6 人日，D9 增量1）

**Context:** D8b-2 把播放逻辑迁入 PlaybackService，运行时手柄（playback_node/playback_pending_(_start_)/playback_mode_）经 4 个 `attach()` 回调写回 App——这是临时缝，文档标注"D9 事件层替代"。D9 起做输出侧事件层（Core→UI）。完整 D9（UI 展示全经事件、替代每帧 `player.get_state()` 轮询）很大；先做最自然的一刀：把 D8b-2 回调缝换成类型化总线事件，建立 service→App 事件通道。

**Approach（回调缝 → 类型化总线事件，行为零变化）:**
- 新 `playback_events.h` 三事件：`PlaybackTrackChanged{node,mode}`（合并 set_playback_node_+set_playback_mode_，二者总是一前一后）、`PlaybackBufferingChanged{pending}`（替 set_pending_）、`HistoryChanged{}`（替 on_history_changed_）。
- PlaybackService：`attach()` 去掉 4 个 std::function 形参，只留三指针注入；play_current/on_playback_ended/record_play_history 改 `EventBus::publish`。
- App：`run()` 加 3 个 `EventBus::subscribe`（token 入 action_subs_），订阅体即原回调体（更新 playback_node/playback_mode_、playback_pending_(_start_)、load_history_to_root）。
- **等价性靠总线的同步语义**：`publish` 在调用线程同步派发 → 线程/顺序与回调完全一致（play_current/on_playback_ended 在 UI 线程；record_play_history 的 HistoryChanged 在 pool 线程）。D4 不变量保持。无需 post/drain（那是跨线程→UI 投递才需要的，本刀订阅方就是 App 自身、跑在发布线程）。
- 加 `TEST(PlaybackEvents, DeliveredOnBus)` 守护事件类型可上总线 + delivery。

**Why-not 直接做 UI 展示事件化：** 那需先把手柄私有化进服务 + 改 UI/remote 读取点（app_run 每帧状态机、app_input 21×、draw 调用），耦合大、动 D4 现场。本刀只换"写回通道"（回调→事件），读取点零改动，低风险、可独立验证；事件类型与 publish/subscribe 框架正是 D9-2 要复用的。

**Verification:** ctest 39/39；构建 0-warning（含 test_units）；pty 冒烟（exit 0 + clean endwin）。

**Followups:** D9-2——把运行时手柄迁入 PlaybackService 私有化（事件让 UI 可经快照读、不再直读 App 成员），UI/remote 订阅事件更新展示、逐步替代 `player.get_state()` 轮询对应部分。

---

## D8b-2 — 播放/自动进阶逻辑迁入 PlaybackService，运行时手柄经回调缝（M1 第 5 人日，D8 增量2b）

**Context:** D8b-1 把队列状态所有权迁入 PlaybackService 后，D8b-2 本应把 `play_current`/`on_playback_ended` 及其同写的运行时手柄（`playback_node`/`playback_pending_(_start_)`/`playback_mode_`）一并迁入。原 ROADMAP 设想"手柄与 play_current 同写、须一起迁避免 App 反向依赖"。但摸底发现：这些手柄在迁移的方法里**只写不读**——读取全在 App（app_input 21× 读 playback_node、app_run 每帧状态机读 playback_pending_、nav/remote）。若把手柄物理迁入服务，需新增 ~50 个 `playback_.playback_node()` 之类访问器读取点改名，纯属 churn（D9 事件层本就要用事件替代这些直接读）。

**Approach（Option Y：迁逻辑、留手柄、回调缝搭桥）:**
- **迁逻辑**：`play_current`/`on_playback_ended`/`record_play_history`/`resolve_youtube_url` + 静态助手 `is_mpv_sub_url`/`basename_of` → PlaybackService。队列状态已是私有成员，直接访问。`build_peer_list`/`is_playable_node`/`play_episode`（树逻辑）留 App。
- **依赖后注入**：`pool_`/`subtitle_mgr_`/`transcription_engine_` 声明于 `playback_` 之后 → 无法构造期引用，PlaybackService 持指针、经 `attach()` 在 `App::run()`（`playback_.init()` 之后）注入。
- **回调缝**：迁移方法对运行时手柄只写 → 4 个 `std::function`（set_playback_node / set_pending[true 时盖 start] / set_playback_mode / on_history_changed[→ load_history_to_root]）写回 App。手柄物理位置不动、读取点零改动。
- **play_mode** 是设置（多点写），留 App，作形参传 `play_current(idx,mode,play_mode)`/`on_playback_ended(reason,mode,play_mode)`。**坑**：play_current 的 YouTube 异步 pool lambda 原隐式读 `this->play_mode`（成员），迁入后须**显式捕获** play_mode。
- **D4 不变量保持**：on_playback_ended 现为 PlaybackService 方法，但调用点（app_run 的 `pending_end_reason_` drain）未换线程 → 仍只在 UI 线程跑。

**Why-not 全迁手柄：** 全迁 = ~50 读取点改名 + 给服务加一堆 getter，零行为收益、还动 app_run 状态机（D4 现场附近，敏感）。回调缝仅 4 个 `std::function`，是临时桥——D9 事件层（PlaybackStateChanged 等）一上，UI 改订事件、手柄即可迁入私有化、回调删除。把 churn 推迟到它真正该发生的 D9。

**Verification:** ctest 38/38；构建 0-warning（-Wall -Wextra -Wpedantic）；pty 冒烟（启动→attach→主循环渲染→q/y 退出，exit 0 + clean endwin 序列）。

**Followups:** D9 事件层——PlaybackService 发 PlaybackStateChanged 等、UI 订阅；随后把 playback_node/playback_pending_(_start_)/playback_mode_ 迁入服务私有化、删 attach() 回调缝。

---

## D8b-1 — 播放队列状态迁入 PlaybackService（M1 第 4 人日，D8 增量2a）

**Context:** D8a 起了功能抽象层（PlaybackService 接管 Action）。D8b 要把播放状态/逻辑也迁入，但摸底发现耦合面很大：`play_current`/`on_playback_ended` 除队列状态外还触 mode/playback_mode_/playback_pending_(_start_)/transcription_engine_/subtitle_mgr_/pool_/record_play_history→load_history_to_root/collect_playable_items；`playback_node` 被 app_input 21× 读取（ASR/字幕）+ UI 读；`playback_pending_` 驱动 app_run 每帧状态机。一次性全迁 ≈100+ 引用、跨 11 文件（含 UI 与 remote_protocol），且 on_playback_ended 的 playlist_mutex_ 跨线程路径正是 D4 修的"暂停后 TUI 冻结"现场——盲迁有回退风险。

**Approach（分两刀，先迁所有权、再迁逻辑）:**
- **D8b-1（本刀）：只迁队列状态的所有权 + 纯队列逻辑。** `current_playlist`/`current_index`/`shuffle_queue_`/`playlist_mutex_` → PlaybackService 私有；`clear_playlist`/`refill_shuffle_queue`/`random_peer_index`（三者只依赖队列状态、无跨切依赖）→ PlaybackService。公开访问 API（`playlist_mutex()/playlist()/current_index()/set_current_index()/shuffle_queue()`）让仍留在 App 的 `play_current`/`on_playback_ended`/`build_peer_list` + app_run 绘制 + app_remote 快照经 `playback_` 访问。**锁语义零变化**（同一 mutex、同样 guard 点、同样"调用方持锁"契约——refill_shuffle_queue 不自锁）。
- **故意不迁** `play_mode`（全局设置，app_input/app_remote/app_run 多点写）、`playback_node`/`playback_pending_(_start_)`/`playback_mode_`（运行时手柄，读取点多且 app_run 状态机敏感）——这些在 D9（事件层替代直接读）时迁更自然；现在迁只是把 ~50 个读取点改名、不增价值。`play_current`/`on_playback_ended`/`build_peer_list` 留 D8b-2（需注入 pool/subtitle/transcription + mode/history 回调缝）。
- **D4 不变量保持**：on_playback_ended 仍是 App 方法，调用点 app_run 的 `pending_end_reason_` drain（UI 线程）未动→它绝不跑在 mpv 事件线程。

**Verification:** ctest 38/38；构建 0-warning；冒烟（pty 下 TUI 全渲染、无崩溃签名）。

**Followups:** D8b-2 迁 play_current/on_playback_ended（注入依赖/回调）；D9 事件层（PlaybackStateChanged 等）取代 app_run/app_input 对 playback_node/pending 的直接读，届时把这些运行时手柄也迁入。

---

## D8a — PlaybackService 接管播放 Action（第一个 Application Service）（M1 第 3 人日，D8 增量1）

**Context:** M1 UI 解耦第 3 步。D6/D7 把 UI 输入迁到总线；D8 起抽 Application Service（功能抽象层）——先做增量1：PlaybackService 接管播放类 Action（不碰复杂状态）。

**Approach:**
- `PlaybackService`（`playback_service.h/.cpp`）：持 `MPVController& player_`；`init()` 订阅 PlayPause/VolumeUp/VolumeDown → `on_play_pause/on_volume_up/down` → 调 player（已是 worker 异步）。
- App：`PlaybackService playback_{player};`（成员，player 之前声明→构造序正确）；`run()` 调 `playback_.init()` 取代 App 内联的 3 个订阅。
- pause/音量路径：UI→Keymap→Action→总线→**PlaybackService**→player。nav 仍在 App（非播放）。
- 故意不在此增量迁复杂状态（current_playlist/auto-advance/play_current/on_playback_ended 与 tree_mutex/pool/subtitle 深耦合）——留 D8b，分步降风险。

**Verification:** ctest 38/38；构建 0-warning；冒烟正常。

**Followups:** D8b 迁播放状态+逻辑入 PlaybackService（注入依赖）；D9 事件层；D10 更多 Services。

---

## D7 — Keymap + 迁移 pause/音量/导航到 Action（UI 解耦）（M1 第 2 人日）

**Context:** M1 UI 解耦第 2 步。D6 证明单点（pause）走总线；D7 引入 Keymap（键→Action）+ 迁移更多输入。

**Approach:**
- `actions.h` 重写为 `using Action = std::variant<PlayPause/VolumeUp/Down/NavUp/Down>` + `publish_action`（`std::visit` → `EventBus::publish` 活跃类型）。Action 是**数据**（可放 Keymap / 将来序列化进 `[keys]`），不是函数。
- `keymap.h`：`Keymap`（`unordered_map<int,Action>`）—— 键绑定单一来源、可重绑。
- `App::build_keymap()`：绑默认（space/p、+/-、k/j）。`handle_input` switch 前先 `keymap_.lookup` → 命中则 `publish_action`+return；移除 pause/音量/导航 旧 case。
- `App::run()`：`build_keymap()` + 订阅 VolumeUp/Down/NavUp/Down → handler（`player.set_volume` / `nav_up/down`）。PlayPause 订阅 D6 已有。
- 效果：5 个交互经 键→Keymap→Action→总线→handler；UI 不直调 Core。Keymap 集中化→`[keys]` INI 覆盖有单一落点（后续）。

**Verification:** ctest 38/38；构建 0-warning；冒烟正常；5 交互经总线工作。

**Followups:** page/seek/模式/复杂流程（搜索/账号/bilibili）迁 Action；`[keys]` INI 覆盖；D8 抽 PlaybackService 接管 handler。

---

## D6 — Action 类型 + 暂停走消息总线（UI 解耦输入侧种子）（M1 第 1 人日）

**Context:** M1（UI 解耦）启动。目标（`docs/DESIGN.md`）：UI 变纯交互层（只发 Action + 订阅事件），消息总线成 UI↔核心唯一通道。D6 是输入侧种子——证明 UI→Core 经总线。

**Approach:**
- `include/panicast/app/actions.h`：定义 Action 类型（`PlayPauseAction`）。复用 **EventBus** 承载 Action（输入方向与事件共用同一总线；Action 是 UI→Core，Event 是 Core→UI，都是 publish/subscribe）。
- `app_input.cpp`：Space/'p' 暂停键改为 `EventBus::publish(PlayPauseAction{})`，不再直调 `player.toggle_pause()`。
- `App::run()`：`EventBus::subscribe<PlayPauseAction>([this](auto&){ player.toggle_pause(); })`，token 存 `action_subs_`。player.toggle_pause 已是 worker 异步（前 robust 提交）。
- 效果：UI 暂停路径不再直接碰 player——首个 UI→Core 总线闭环。

**Verification:** ctest 38/38；构建 0-warning；冒烟正常；暂停经总线工作。

**Followups:** D7 迁移更多输入（play/seek/volume/导航/模式）到 Action + Keymap 可自定义；D8 抽 PlaybackService 接管这些 Action handler；D9 加事件层（输出侧）。

---

## robust — mpv 交互命令移到 worker 线程（TUI 永不被 mpv/PA 阻塞）

**Context:** 用户要求"不管 mpv 状态，TUI 都要响应交互"。前一条日志（看 IPTV/mp3 暂停）+ 诊断确认：WSLg PulseAudio 挂死 → 暂停触发 `pa_stream_cork` 超时 → mpv 阻塞 → UI 线程直接调 `mpv_set_property` 冻结。

**Approach:**
- `get_state()` 已是缓存读（`update_state` 在 event 线程、mpv 读取在 `mtx_` 锁外）→ 不阻塞 UI，无需改。
- 阻塞点在**命令侧**：`set_pause/set_volume/set_speed/...` 在调用线程（UI）直接 `mpv_set_property`。
- 修：MPVController 加**命令 worker 线程**（`cmd_thread_` + `cmd_queue_` + cv）。`toggle_pause/set_pause/set_volume/adjust_speed/reset_speed/set_speed` 改 `enqueue_cmd_` 投递；UI 立即返回（state_ 乐观更新：pause/volume/speed）。worker 顺序执行；mpv 卡死时只 worker 阻塞，UI 继续跑。
- 关停：`stop()` bounded-join（等 `cmd_done_` ≤1.2s）+ detach 回退（与 event 线程同模式，靠 `_exit` 退出）。
- 编译坑：按值捕获的 lambda 在 const operator() 里 `&capture` 是 const 指针 → mpv 的 `void*` 参数报 const void*→void*；三个 lambda 加 `mutable` 解决。

**Verification:** ctest 38/38；构建 0-warning；冒烟正常。

**Followups:** 其余 mpv 调用（play/seek/loop/keep_open/sub_add）同法迁 worker（按需）；治本修 WSL PA。

---

## fix — "看视频/暂停时 TUI 输入无响应" = mpv 视频窗口抢键盘焦点（非死锁）

**Context:** 用户报告"暂停后 TUI 不响应"，看 IPTV 时复现。D4（end_file 入队）+ D5（watchdog）按"死锁"方向未根治。用户提供 panicast.log。

**Root cause（日志确认）：**
- 日志 `input: No key binding found for key 'l'/'a'/SPACE/','/MBTN_LEFT'` —— mpv 的视频窗口（vo=gpu/wlshm）**抢了键盘焦点**，用户按键被该窗口接收、因无绑定被丢弃，ncurses TUI 收不到。看视频即发生，与暂停无关。
- 排除死锁：ncurses 只在 ui/app（无跨线程破坏）、`stop_realtime` 非阻塞、END_FILE 已 D4 迁 UI 线程；watchdog 也未触发慢帧 → 不是 freeze。

**Fix（卫生）：** `mpv_set_option_string(ctx, "input-default-bindings", "no")` —— TUI 拥有输入，mpv 窗口不解释键（消除吃键日志、防绑定冲突）。

**Limitation：** 视频窗口抢焦点是 WSLg/Wayland 窗口行为，应用内无法强制终端保持焦点。绕过：点终端窗口恢复 TUI 控制。根治需 WSLg/WM 层（focus-new-windows 规则）。

**Also（日志）：** WSL 音频坏（PulseAudio timeout / ALSA refused → AO init failed），独立问题。

---

## D5 — M0 收尾 + UI 帧时间 watchdog（新架构 M0 第 5 人日）

**Context:** M0 收尾。D1–D4 已落地 EventBus、IProxyManager（全消费者经 Connectivity）、Media/MediaID。用户报告"暂停后 TUI 输入无响应"在 D4 修复后仍存——静态分析已穷尽明显原因（D4 修了 END_FILE 跨线程 `playlist_mutex_` 争用；排除 ncurses 跨线程破坏[只在 ui/app]、ASR join 阻塞[`stop_realtime` 非阻塞]）。剩余卡点需运行时数据。

**Approach:**
- M0 端到端 = app 本身（EventLog→EventBus、所有网络→ProxyManager、Media 适配器），无需额外样例。
- 加 **UI 帧时间 watchdog**：每帧测 `tree_mutex` / `playlist_mutex_` 等待 + 整帧耗时；超阈值（锁等待 >80ms / 帧 >150ms）写 `panicast.log`。下次复现"暂停后无响应"时，日志直接显示卡点（等哪把锁 / 哪步慢）→ 定点修复。

**M0 达成：** 最小可演进系统（EventBus + Connectivity + Media + 工程基线）。M1+ 增量。

**Verification:** ctest 38/38；构建 0-warning；冒烟正常。

**Followups：** 据 watchdog 日志定点修"暂停后输入无响应"剩余卡点（疑似 ASR worker 暂停后仍转写整段占 CPU，待确认）；M1（Downloader 全覆盖 D6 + 热键 Keymap D7）。

---

## D4 — Media/MediaID 骨架 + 修复"暂停后 TUI 无响应"（新架构 M0 第 4 人日）

**Context:** D4 落地 Media 领域句柄；用户报告"TUI 暂停播放后过一段时间无响应"，要求在 D4 一并修复。

**Bug 根因分析：**
- `on_playback_ended`（`app_playback.cpp:66`）通过 `end_file_callback`（`app_run.cpp:73` 注册）在 **mpv 事件线程**执行。它锁 `playlist_mutex_`（:68）并做 `fs::exists`/`URLClassifier::classify`/`pool_.submit`/`player.play` 等一串工作。
- UI 主循环（`app_run.cpp:338`）draw 时也持有 `playlist_mutex_`。
- 暂停久了（尤其直播流），流空闲断开 → mpv 触发 END_FILE → mpv 事件线程在 `on_playback_ended` 持 `playlist_mutex_` → UI 线程到 :338 拿不到锁 → 卡在 draw、到不了 :398 读输入 → TUI 无响应。属 AUDIT P1-4/5/8 同源的"跨线程持 UI 锁"问题。

**修复：**
- mpv 事件线程的 end_file 回调改为只入队：`pending_end_reason_.store(reason)`（`app.h` 新增 `std::atomic<int> pending_end_reason_{-1}`）。
- UI 主循环每帧 drain（`app_run.cpp` 紧随 `drain_remote_commands()`）：`exchange(-1)` 取出 reason，非 -1 则在 **UI 线程**调 `on_playback_ended(reason)`。
- 效果：`on_playback_ended` 不再在 mpv 线程持 `playlist_mutex_`，消除与 UI draw 的争用。这正是 EventBus/命令总线把跨线程回调 marshal 到 UI 线程的模式（D1 EventBus 的延伸）。

**Media/MediaID：**
- `include/panicast/domain/media.h`（header-only）：`MediaID`（弱引用 TreeNode 身份，`==`/`!=`/`valid`/`lock`）+ `Media{id,url,title}` 只读视图 + `media_from_node` adapter。不改 TreeNode；后续逐步收敛。

**Verification:** ctest 35→38（+3 MediaID/Media）；增量构建 0-warning；冒烟正常。

**Followups：** 若个别场景仍偶发卡顿，继续把其它 mpv 线程→App 回调（property observers 等）也 marshal 到 UI 线程（EventBus）；Media 表面随 M2 收敛。

---

## D3 — 全部网络消费者接入 Connectivity（url-aware resolveProxy + Downloader）（新架构 M0 第 3 人日）

**Context:** D2 落地 IProxyManager 但 apply_network_proxy 只用 resolveProxy("")（无 url，仅全局）。D3 让所有网络消费者传真实 url + platform（启用域名/平台规则），Downloader（含 yt-dlp）也接入。

**Approach:**
- `apply_network_proxy(CURL*, const std::string &url, const std::string &platform = "")`：body 改 `resolveProxy(url, platform)`。
- 4 个 curl 调用点传值：`network.cpp` configure_curl 传其 `url`（通用 fetch，platform ""）；`bilibili_api.cpp` 传 `url` + "bilibili"；`itunes_search.cpp` 传 `url` + "podcast"；`app_download.cpp` 传 `url`（下载，platform ""）。
- `ytdlp_runner.cpp`：`--proxy` 从 `IniConfig::get_proxy()` 改为 `ProxyManager::instance().resolveProxy("").url`（经 Connectivity；yt-dlp 的 url/platform 感知路由留作精化）。
- 行为零变化：无平台/域名规则时 resolveProxy 返回全局 [network] proxy（与改动前一致）。

**Verification:** ctest 35/35；增量构建 0-warning；`build/panicast --version` 正常。

**Followups:** 按需填首批规则（youtube→代理、`*.googlevideo.com`→代理、bilibili→直连）；ytdlp_runner 传真实 url/platform（需 run() 加参数）。

---

## D2 — IProxyManager（Connectivity 层）+ apply_network_proxy 首个消费者（新架构 M0 第 2 人日）

**Context:** 落地"统一网络前端"——所有网络消费者（Parser/Downloader/字幕/AI 云端）经一个 IProxyManager 解析代理，mpv 播放直连。D2 做接口 + 规则链 + 首个消费者；Downloader 等调用点切换在 D3。

**Approach:**
- `IProxyManager` + `ProxyConfig{url}` + `ProxyManager`（`resolveProxy(url, platform)` 规则链：平台→域名→全局→直连；`host_of` + `domain_matches` 实现 `*.glob` 域名匹配；`mutable std::mutex` 线程安全）。
- **全局源可注入**（`setGlobalSource(std::function<ProxyConfig()>)`）而非直接读 IniConfig：使 `proxy_manager.cpp` 不依赖 config 系统 → 单测目标可干净链入 proxy_manager.cpp 而不拖入 ini_config 及其依赖。生产由 `network.cpp`（本就依赖 ini_config）在文件作用域静态初始化里把 `[network] proxy`（实时读，Ctrl+N 即时生效）注入。
- `apply_network_proxy(CURL*)` 改走 `ProxyManager::instance().resolveProxy("")`：首个真实消费者。无平台/域名规则时返回 [network] proxy → 行为零变化。

**Verification:** ctest 30→35（5 个 ProxyManager 用例全过：全局源/无源直连/平台覆盖/域名匹配/平台优先）；增量构建 0-warning；`build/panicast --version` 正常；代理路径经 ProxyManager（Ctrl+N 全局源实时生效）。

**Followups:** D3 切换 network.cpp/bilibili_api.cpp/itunes_search.cpp + ytdlp_runner 调用点到带 url 的 `resolveProxy`，并接入 Downloader；按需填平台/域名规则（如 youtube→代理、*.googlevideo.com→代理）。

---

## D1 — EventBus 核心 + EventLog 作为首个生产者（新架构 M0 第 1 人日）

**Context:** 新架构迁移第 1 步（开发计划 D1）。引入类型安全事件总线，作为后续替换 `pending_select_`+散落回调（AUDIT 竞态 P1-4/5/8 根因）的承重墙。

**Approach:**
- header-only `EventBus`（`include/panicast/core/event_bus.h`）：模板 `subscribe<E>/publish<E>`，`type_index` 分桶；订阅者锁内快照、锁外派发（handler 可递归 publish/unsubscribe，不自死锁）。`subscribe` 返回 token，`unsubscribe(token)` 移除。
- **先只上同步 `publish`**（caller 线程派发）；异步 `post/drain`（跨线程→UI）留到迁移 `pending_select_` 时再加（并发敏感，单列后续人日）。
- **首个真实生产者 = `EventLog::push`**（每条日志 `publish(LogEvent)`）。选它：①真实（379 处 EVENT_LOG 全上总线）；②安全（EventLog 本身线程安全、无订阅者即空操作、不依赖总线即可工作 → 零 init-order 风险、零行为变化）；③启用未来订阅者（远程日志推送/调试覆盖层）。

**Verification:** ctest 26→30（4 个 EventBus 用例全过）；增量构建 0-warning（event_log.h 改动触发其依赖重编，均通过）；`build/panicast --version` 正常，EventLog 行为不变。

**Followups:** `post/drain`（异步）+ 迁移 `pending_select_` 到总线（修竞态）；更多信号（playback 状态等）逐步上总线。

---

## N07 — titlebar-as-root data-model refactor (root nodes → vectors) + exit typeahead fix

**User goal:** eliminate the vestigial per-mode root NODE from the data model (option E, "最优雅"). Display redundancy (5-mode border + root row) was already fixed by Step 1 (display iterates items). N07 completes the data-model cleanup.

**Approach (E = vectors, not D's display-only):** the 8 per-mode roots (`radio_root`...`iptv_root`) were `TreeNodePtr` container nodes whose `->children` held the items. Converted to `std::vector<TreeNodePtr>` — items live directly in the vector, no container node. `current_root` (TreeNodePtr) eliminated → `items_for_mode(mode)`/`cur_items()`. Container functions (`clear_marks`/`collect_marked`/`count_marked_safe`/`remove_node`) gained `_current` helpers that loop `cur_items()`. `sort_target` reworked (top-level sort = sort `cur_items()`; reverse state in new `cur_sort_reversed` member, was on the root node). `flatten` simplified to pure recursion (removed `title=="Root"` magic). `Persistence::save_cache/load_cache` → vector signatures. `get_root_by_mode_string` → `vector*`. Parent pointers for top-level items → `reset()`. `*_root_loaded` (root node's `children_loaded`) → App member bools.

**`online_root` stays TreeNode (key finding):** audit revealed it's a cross-mode LINK TARGET — the favourited "Online Search" is a LINK node pointing to `online_root` (`fn->linked_node = online_root`, pointer-identity check, LINK expand resolves `"online_root"` URL). Vectorizing it would break the LINK feature. It's already invisible (display iterates its children). Conclusion: online_root is a functional LINK-target node, NOT a vestigial container → stays. So E applies to the 8 per-mode display containers; online_root is a documented exception.

**Exit typeahead fix (separate bug):** keys typed during shutdown (after the main loop stops reading) sat in the terminal input queue and carried to the shell (e.g. "kjkkj" at the prompt). Fix: `tcflush(STDIN, TCIFLUSH)` in `restore_terminal_state()` + `restore_termios_async()` drains the input queue on exit.

**Verification:** compiles 0-warning; 9-mode smoke (display+nav) + deeper regression (sort/expand/search/go_back-parent/mark/delete — DB history 62→59 confirms `remove_from_current`) all pass, no crash. Bilibili search now requires a logged-in account (no root to attach anonymous results to — aligns with Y mode).

**Followups:** download/play/remote need user's real-resource testing (same `_current` helper pattern, low risk). Bilibili anonymous search removed (was attaching to the now-gone root).

## N06 — MediaType: DB-stored display category (platform-specific before generic)

**User goal:** history/favourites icons must respect platform specificity — YouTube never "OnlineVideo", radio never "OnlineAudio", and m3u8 must be IPTV (not radio). Store a `media_type` column so the icon comes from the DB instead of being re-inferred from the URL every render.

**Key design correction to the initial proposal:** classifyMediaType is NOT a pure URLType→MediaType switch. URLClassifier IS platform-priority for platform-vs-generic (YouTube matched before .mp4), BUT URLType conflates two distinctions the user needs:
- `.m3u8` → URLType::RADIO_STREAM (so pure mapping would make m3u8 = Radio — wrong);
- local `file://.mp4` and online `.mp4` both → VIDEO_FILE; local non-video and online radio both → RADIO_STREAM (so URLType can't tell local from online).

Fix: classifyMediaType layers two pre-checks on top of classify(): (1) `file://`/absolute-path → LocalVideo/LocalAudio by extension (reuse classify()'s VIDEO_FILE split); (2) `iptv:` scheme or `.m3u`/`.m3u8` extension → Iptv. THEN switch on URLType for the rest.

**Confirmed decisions:** 9 categories + emoji; DOUYIN_* → Tiktok (CN counterpart, placeholder); m3u8 empirically = IPTV (user's DB: 39/39 m3u8 are IPTV channels, zero radio uses m3u8); emoji de-duped + all glibc wcwidth=2 (▶ U+25B6 is wcwidth=1 → use 📹 for YouTube; 🎶/🎥 for local audio/video to avoid clashing with Tiktok🎵/OnlineVideo🎬).

**Display scope:** only history + favourites DB-driven LEAF nodes use media_type_icon (gated on NodeType::PODCAST_EPISODE/RADIO_STREAM). Folder/feed/link favourites keep the folder icon; live tree (radio/podcast browsing) unchanged — its flag-based icons are already accurate.

**Schema:** SCHEMA_VERSION 46→47; history + favourites gain `media_type INTEGER`; one-time backfill (guarded by stored_version<47) recomputes from url. Write path computes media_type inside add_history/save_favourite (zero caller changes).

## N05 — 'r' key unified per-node refresh (Y/B/T fix)

**User goal:** Y mode's dedicated "resync" on 'r' was not a user-designed feature — diagnose & fix; extend the same mechanism to B and T; then package as N05.

**Diagnosis:** The resync *capability* is legitimate & required (account data only syncs at login + manual 'r'; lazy-expand reads local DB; no periodic sync — deleting it would freeze data). The *wiring* was wrong:
- Y: 'r' on ANY node triggered whole-account resync (heavy + collapsed the tree).
- B: 'r' was a silent no-op (man/help falsely said "Refresh node"; a comment claimed re-fetches, never implemented).
- T: 'r' couldn't refresh stale local cache at all.

**Fix (app_input.cpp 'r' dispatch unified by node type; account nodes disambiguated by AppMode — is_account is reused by T creators):**
- Y account → `resync_account_node`; Y Subs/History → new `refresh_account_subs/history` (subtree-only, preserves expansion).
- B account → `refresh_bilibili_account`; B followings/history → `refresh_bili_followings/history` (force re-fetch).
- T creator (FOLDER) → new `refresh_node` T branch → `spawn_load_feed`(TIKTOK_USER) online re-fetch → `commit_feed_result` replaces children + `episode_cache` (DEL+INS).

**Followups (not in N05):** T single-video leaf 'r' stays a no-op hint (consistent with episodes everywhere). Y channels (`is_yt_channel`) / B UP masters (`is_bili_up`) still no-op on 'r' — their loaders are inline lambdas; extracting them is a later refactor.

## N04 — PIN auth + UDP discovery + WebSocket + embedded BS client + APK source

**User goals:** (1) latest tarball; (2) open the backend address in any IE → control directly; (3) APK: install on modern Android with no dependency issues, auto-scan network for players, PIN pairing (dynamic PIN shown in Panicast popup + universal 6696 for headless), full control + view.

**Auth model (resolves BS-direct vs APK-PIN tension):**
- PIN-based auth replaces `auth_token`. Dynamic 4-digit PIN (`regenerate_pin()`), shown via `:pin` popup, rotatable via `:newpin`. Universal `6696` always valid (headless pairing).
- **Localhost connections are open** (no PIN) → "open http://127.0.0.1:port/ in IE on the Panicast host → control directly". Non-localhost → PIN required. The BS HTML shows a PIN overlay only when the server returns ACK auth-required.
- `password <pin>` valid iff pin == dynamic || pin == "6696".

**UDP discovery (N05):** APK broadcasts `PANICAST_DISCOVER` to udp 18430; Panicast's `discovery_loop` responds `PANICAST 1 tcp=<port> ws=<port+1>`. APK uses the response source IP. Avoids mDNS/ZeroConf complexity + dependencies — plain UDP broadcast (Android `DatagramSocket` + `WifiManager.MulticastLock`).

**WebSocket (N06):** self-written RFC6455 (handshake SHA1+base64 via OpenSSL `SHA1`; frame codec with mask/ping-pong). One HTTP listener on port+1 serves the embedded BS client (GET /) AND upgrades to WS. **socketpair bridge**: RemoteSession was refactored to separate `read_fd_`/`write_fd_`; the WS bridge runs RemoteSession on one end of a socketpair and shuttles WS frames ↔ PRP lines on the other — the PRP engine is unchanged. This keeps ONE protocol (DRY) across raw-TCP and WS transports.

**Embedded BS client:** a single self-contained HTML (inline CSS+JS, IE11-compatible) baked into the binary via `panicast_web_index.h` raw-string include → "open address and control" with zero external files. GitHub Dark, PIN overlay, now-playing + controls + idle live status + progress interpolation + auto-reconnect.

**APK:** native Kotlin + Compose + Material3 (minSdk 24), no runtime deps (Gradle-bundled Compose). UDP scan → PIN → TCP PRP. Source project under `apk/`; this environment has no Android SDK so the APK is built in Android Studio (instructions in `apk/README.md`).

**Verification:** 0-warning build; pin_test (LAN: no-pin ACK / wrong ACK / 6696 OK / status OK), discovery_test (probe→beacon), ws_test (101 handshake + greeting frame + ping/status/volume 60 end-to-end over WS) all PASS; HTTP GET / → 200 + embedded client.

**Open:** APK on-device build (Android Studio); explicit `notify("log"|"tree")`; remaining command coverage (search/mark/edit/download/subtitle/asr/queue); BS/APK UI polish.

---

## N03 — idle event subscription + state sync push

**Goal (user emphasis: "遥控和状态同步"):** let remote clients subscribe to state changes and receive pushed `changed: <subsystem>` events (MPD `idle` semantics), so a remote UI stays live without polling.

**Design:**
- `RemoteSession::handle_idle`: enters subscription mode; multiplexes with `poll(fd, 100ms)` so it simultaneously (a) waits for server-side `notify_change` to populate `idle_pending_`, and (b) watches the socket for `noidle` / a queued command / close. On change → emit `changed: <subsys>` lines + `OK`. This avoids a second thread per session — the single worker thread handles both directions via poll.
- `notify_change(subsys)` is the cross-thread entry (called by `RemoteServer::notify` from the diff thread or App): guards with `idle_mtx_`, dedups, only if subscribed.
- `RemoteServer` keeps a `sessions_` registry; `notify(subsys)` snapshots the set under `sessions_mtx_` then delivers outside the lock (avoids holding the registry lock during per-session idle_mtx_ acquisition — no lock-ordering hazard).
- **Diff poller** (`diff_loop`, 10Hz): the only source of state-change detection for mpv-internal events (seek/pause/track-end). Compares `snapshot_state()` fields → `notify(player|mixer|options|mode|subtitle|art)`. The 100ms cadence IS the coalesce window (no separate debounce needed). `elapsed` is intentionally NOT diffed (would fire every frame) — clients interpolate elapsed; only state/song/title/url changes fire `player`.
- `recv_buf_` became a member + `poll_line(out, timeout_ms)` shared by `run()` (blocking read) and `handle_idle` (multiplexed read) — single buffer, single code path.

**Roadmap condensation:** N02 already absorbed the plan's N03–N06 command-coverage milestones (core controls shipped in N02). So this idle release is sequentially **N03** (was the plan's "N07" milestone). Future plan updates will renumber sequentially.

**Verification:** `idle_test.py` — A `idle mixer`, B `volume 42` → A receives `changed: mixer`+`OK`. Diff→notify→push chain proven. 0-warning build.

**Open:** N04 WebSocket frontend (self-written RFC6455) + static asset hosting → enables browsers; N05 BS web client; deferred explicit `notify("log"|"tree")` hooks + remaining command coverage (search/mark/edit/download/subtitle/asr/queue).

---

## N02 — PRP protocol engine, state snapshot, control command end-to-end

**Goal:** make the remote terminal actually control Panicast — query state AND issue commands that take effect — over the MPD-style line protocol defined in `PANICAST_N_LINE_PROTOCOL_DESIGN.md`.

**Protocol (PRP — Panicast Protocol):** MPD-style text line protocol. Greeting `OK Panicast N02`; request `COMMAND [ARG...]\n` (double-quoted args for spaces); response `key: value` lines ending `OK` or `ACK [code@0] {cmd} msg`. Query commands (`status`/`currentsong`/`playlistinfo`/`ping`/`password`) answered inline; control commands forwarded to the UI thread via the bus. `password <token>` auth gate (MPD semantics). `idle`/`noidle` accepted as no-ops (N07 implements real subscription).

**Threading contract (the two sanctioned crossing points):**
1. WRITE path — `RemoteCommandBus` (N01): server `push()`es control commands; UI thread `drain_remote_commands()` each frame; `dispatch_remote()` maps to existing App methods. UI never touched off-thread.
2. READ path — `RemoteStateSnapshot`: built once per frame on the UI thread (`update_remote_state_cache`) under a dedicated `remote_state_mtx_`; server threads read copies via `snapshot_state()`. This **deliberately avoids cross-locking `tree_mutex`/`playlist_mutex_` from server threads** — only `playlist_mutex_` is taken on the UI thread (same thread that already takes it) to copy playlist titles. Player state uses `MPVController::get_state()` which is already mutex-protected.

**Decoupling:** `RemoteControlInterface` (abstract) — App implements it; `RemoteServer`/`RemoteSession` depend on the interface, not App. Composable + testable.

**Control coverage (N02):** playback / seek / volume / speed / play-mode / sleep / mode-switch / navigation / mpv-passthrough — the demonstrable core. `mpv <cmd>` reuses the `:`-window forwarding. Search/mark/edit/download/subtitle/asr/queue deferred to N03–N05 (some are coupled to TUI input boxes — need non-interactive variants).

**Verification:** 0-warning build; Python PRP client proves end-to-end: `volume 55` → subsequent `status` echoes `volume: 55` (server→bus→UI→player→snapshot). Default `enable=false` leaves local TUI unaffected.

**Open:** N03 full command coverage; N07 idle subscription (notify + 10Hz diff + debounce); N08 WebSocket frontend (self-written RFC6455); N09 BS web client.

---

## N01 — Network control line: foundation skeleton (command bus + TCP server)

**Context:** User requested a network-control feature line (N01–N99) branched from `Panicast_V0.1-Y24.56`, so a remote terminal — first an IE browser (BS), later an Android APK — can exercise **all** local-terminal control functions plus live monitoring. Development must follow `/mnt/e/AI/DEVELOPMENT_PRINCIPLES.md` (i18n / UNIX philosophy / plan-first / async-non-blocking / concurrency-safe / data-layer收敛).

**Key user decisions (confirmed):**
- HTTP/server implementation: **self-written full C++ socket server** (no external lib). Chose POSIX `socket/bind/listen/accept` over adding cpp-httplib — zero new dependency, full control, matches "统一封装" philosophy extended to the server side.
- Real-time state/LOG push: **MPD / ncmpcpp-style technique** — *protocol design deferred*. User will explain the MPD/ncmpcpp approach in detail before N02 protocol is finalized. (MPD uses a persistent TCP line protocol with an `idle` event-subscription command; ncmpcpp is an MPD client. This signals a move away from HTTP/REST toward a daemon TCP protocol + thin clients.)
- Source tree: `/mnt/e/AI/Panicast/Panicast_V0.1-N01/` (parallel to existing Y/F lines).

**Architecture (this version):**
- The UI (ncurses) is single-threaded and not thread-safe. Therefore the network server NEVER calls App/MPVController methods directly. Two sanctioned crossing points:
  1. **`RemoteCommandBus`** — server thread `push()`es a `RemoteCommand{action,args,client_id}`; the TUI main loop `drain_all()`s once per frame and dispatches on the UI thread. Mutex-protected vector, non-blocking swap, `shutdown()` atomic flag. Minimal lock scope.
  2. **state snapshot** (N02) — server thread reads a mutex-guarded snapshot; `MPVController::get_state()` is already thread-safe; App members need a new `snapshot_remote_state()`.
- **`RemoteServer`** threading (concurrency rules): one accept thread; each connection on a tracked worker thread (`struct Worker{ std::thread t; std::atomic<bool> done; }`), reaped each accept iteration so the worker list stays bounded. **No `detach()`** — `stop()` closes the listen socket to unblock `accept()`, then joins the accept thread AND every worker. SIGPIPE avoided via `MSG_NOSIGNAL`.
- Platform scope: POSIX (Linux/macOS). Windows compiles to stubs (`start()` returns false). Mirrors the `mpv` vcpkg dependency which is `linux|osx` only. Keeps the cross-platform build green.
- Config: opt-in `[remote]` section, **default `enable=false`** so the local TUI is byte-for-byte unaffected unless the user opts in. `bind=127.0.0.1` default (localhost-only; `0.0.0.0` for LAN).

**Scope of N01 (deliberately minimal):** version bump, `[remote]` config, command bus, TCP accept-loop skeleton (banner-only handler), App wiring (start/stop/drain), `dispatch_remote()` log-only. The command **protocol** is intentionally NOT implemented — it depends on the pending MPD/ncmpcpp decision.

**Verification:** 0-warning build (`-Wall -Wextra -Wpedantic`); `--version` = N01; smoke test with `enable=true port=18421` → `bash /dev/tcp` connection receives the banner; `timeout` exit joins cleanly (no orphan/hung threads); default `enable=false` → no server started.

**Open for N02 (needs user input):** finalize the command protocol (MPD-style line protocol? `idle`-style event subscription? how does the BS/IE browser fit a non-HTTP protocol — a small WS/TCP bridge, or a separate HTTP endpoint set?). Map `dispatch_remote()` actions to local control methods (playback/navigation/modes/management/`:`-commands).

---

## Y24.56 — IPTV playback LOG-area event messages (off-air / no-video diagnosis)

**Bug (user trial):** playing an IPTV channel whose address is correct but the station is not currently broadcasting → no video stream → mpv does not open a video window, and the user has no idea WHY. The 30s pending timeout only says "stream may have failed", indistinguishable from a wrong address.

**Design (confirmed by user):** enumerate every situation where IPTV playback does not visibly start / no video window, and emit a concise English event message to the on-screen LOG area (EventLog, which double-writes to panicast.log). Two prefixes:
- `MPV:` = mpv-level behavior (the cause) — existing mpv_error_str / fallback messages, kept as-is.
- `IPTV:` = IPTV-context explanation (the user-facing meaning) — new.
- Same event causing both → print BOTH, in time order (MPV: first, IPTV: second); both reach screen + log file via EVENT_LOG.

**The 13 messages (approved):**
1. `IPTV: server unreachable — network, DNS, or timeout; check connection or switch source`  (END_FILE -13, conn keywords)
2. `IPTV: channel not found — 404, address invalid or removed`  (-13 + "404"/"not found")
3. `IPTV: access denied — 403, region-restricted or authorization required`  (-13 + "403"/"forbidden")
4. `IPTV: server error — 5xx, source unavailable; retry later`  (-13 + "500"/"502"/"503"/"504")
5. `IPTV: connected, no stream data — channel may be off-air; retry later`  (polling: FILE_LOADED + core-idle + no codec + no download, held ≥ offair_detect_secs)
6. `IPTV: empty playlist — no playable stream for this channel`  (END_FILE -16)
7. `IPTV: audio-only channel — no video track, playing as audio`  (polling: audio codec + playing + no video track ≥ 8s)
8. `IPTV: audio output init failed — no audio device; check [mpv] ao, PulseAudio, WSLg`  (END_FILE -14)
9. `IPTV: video output init failed — no display, falling back to audio`  (END_FILE -15)
10. `IPTV: cannot decode stream — missing decoder; ensure ffmpeg is installed`  (END_FILE -17)
11. `IPTV: network too slow — sustained buffering, bandwidth insufficient or unstable`  (polling: core-idle + buffering>0 + <1s ahead ≥ 20s)
12. `IPTV: stream dropped mid-playback — source interrupted; switch channel or retry`  (END_FILE r=4 when PLAYBACK_RESTART already fired = had_playback_started_)
13. `Playback pending timeout 30s — stream may have failed`  (existing, parens removed; only fires when FILE_LOADED never came — naturally disjoint from #5 which requires FILE_LOADED)

**Implementation:**
- `ini_config.h`: `get_iptv_offair_detect_secs()` → `[iptv] offair_detect_secs` (default 12).
- `mpv_controller.h/.cpp`: `set_iptv_context(bool)` (atomic, set by app when mode==IPTV). Detection lives in the controller because it has the mpv error code AND the warn/error log text (needed to sub-classify -13 into 404/403/5xx/unreachable).
  - `MPV_EVENT_LOG_MESSAGE`: remember `last_log_text_` for warn/error (INFO still gated to load window).
  - `FILE_LOADED`: stamp `file_loaded_time_`, re-arm one-shots.
  - END_FILE r=4: after the existing `MPV:` message(s), if `iptv_context_`, emit the matching `IPTV:` message. If `had_playback_started_` (PLAYBACK_RESTART fired) → #12 (mid-playback drop); else `iptv_message_for_error_()` (-13 → `classify_iptv_load_error_()` keyword match; -14/-15/-16/-17 fixed).
  - `update_state()`: #5/#7/#11 polling detection, one-shot per track, gated by `iptv_context_`. #5 timer cancels on any data/codec arrival (no false positive on slow initial fill).
  - `reset_iptv_detection_()` (incl. `had_playback_started_`) called at play()/play_list()/play_list_from() entry points and re-armed on FILE_LOADED.
- `app_playback.cpp`: `player.set_iptv_context(mode == AppMode::IPTV)` in play_current + on_playback_ended (auto-advance).
- #5 and #13 are disjoint (#5 needs has_media/FILE_LOADED; #13 fires only when has_media never appears) → no double-fire, no cross-layer pending clear needed.

**Scope notes:**
- mpv warn/error log lines still go ONLY to panicast.log (not routed to the on-screen LOG area) — avoids noise; the IPTV: messages are the curated screen-side diagnostics.
- `mpv_error_str` (the MPV: behavior text) left untouched per "MPV:的行为就用MPV:".

Build: 0 warnings, 74/74, binary runs.
