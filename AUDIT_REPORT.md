# Panicast 源码审计报告

- **目标**：`src/panicast.cpp`（13912 行，单文件）+ `tests/test_units.cpp`
- **版本**：V0.5.0-B9n3f3
- **方法**：全量逐行精读（1–2550 行主审 + 5 路并行子代理覆盖 2550–13912，关键发现回源核验）
- **日期**：2026-07-14
- **状态**：首轮报告，待指示迭代。⚠ **2026-08-10 重新核实**：本报告针对的是模块化**之前**的单文件 `src/panicast.cpp`（13912 行）；M0–M2 模块化重构（含 F/Y24 系列修复）后，**全部 P1/P2 已修复或废弃**（见下 §0.5 核对表，0 存活）。P3/§5/§6 未逐条重核，其中 §5.2「测试镜像漂移」确认仍存活。

---

## 0. 严重程度汇总

| 级别 | 含义 | 数量 |
|------|------|------|
| P1 | 正常使用路径可触发的数据损坏/丢失/崩溃 | 2 |
| P2 | 并发竞态、资源/状态错误、需特定条件触发的功能错误 | 13 |
| P3 | 边界/健壮性/兼容性/性能/可维护性 | 18+ |
| 误报 | 经核验不成立（已剔除） | 1 |

> 标 **[已核实]** 表示我已回源码确认；标 **[待确认]** 表示需结合范围外代码或运行时进一步验证。

---

## 0.5 P1/P2 修复状态核对（2026-08-10，模块化后重新核实）

> 重新核对结论：**15 个 P1/P2 全部已修复或废弃，0 存活**。M0–M2 模块化重构系统性修复了本报告全部高优先项（非纯架构重构——含 F/Y24 系列正确性修复）。下表给出每条的当前证据（文件:行）。

| # | 状态 | 当前证据（模块化后） |
|---|---|---|
| P1-1 | ✅ 已修复 | `src/storage/feed_cache_repo.cpp:476` — parse 失败 `rollback_txn()`（注释明述 "otherwise DELETE commits while INSERT is empty -> cache cleared"） |
| P1-2 | ✅ 已修复 | `src/app/app_tree_expand.cpp:186-189,230-233` — `push_back(child)` 不重设 parent（注释 "do not reset parent: child is shared with target; resetting would break parent-pointer invariant"） |
| P2-1 | ✅ 已修复 | `src/storage/youtube_cache.cpp:2` — "All DB access goes through DatabaseManager's single shared connection"；经 `youtube_cache_load/save` |
| P2-2 | ✅ 已修复 | `src/parsers/itunes_search.cpp:148` — `if (!contains("feedUrl") \|\| !is_string()) return;`（注释 "to avoid discarding the whole batch"） |
| P2-3 | ⚰ 废弃 | `migrate_from_json` 已移除（DB 为单一真相源，JSON 迁移路径不再存在） |
| P2-4 | ✅ 已修复 | `src/app/app_subscriptions.cpp` — import_feed/import_opml/export_podcasts 均 `pool_.wait_idle()` + `lock(tree_mutex)` 后再序列化；CLI 路径 `pool_.shutdown()`（app_run.cpp:466） |
| P2-5 | ✅ 已修复 | 播放队列状态（current_playlist/shuffle_map_/index）已私有化进 PlaybackService（D8b-1/D8b-2/D9），mutex 归服务统管 |
| P2-6 | ✅ 已修复 | `src/app/app_subscriptions.cpp` — 所有 `*_root().insert()` 均在 `lock_guard(tree_mutex())` 内（协议统一） |
| P2-7 | ✅ 已修复 | `src/ui/popups.cpp:486` — `if (w<20 \|\| h<5) return false;` + side_dashes/ix/iy 钳制 |
| P2-8 | ✅ 已修复 | `src/app/app_nodes.cpp:432` — `start=0; // guard against negative-index overflow` |
| P2-9 | ✅ 已修复 | `src/storage/tree_repo.cpp:40-61` — `save_tree` try{...commit}catch(...){rollback}（替代旧 save_radio_cache；递归在 try 内） |
| P2-10 | ✅ 已修复 | `src/playback/mpv_controller.cpp` — play_list/play_list_from/toggle_pause/set_volume/adjust_speed/set_speed/set_loop_file 均开头 `if (!ctx_) return;` |
| P2-11 | ✅ 已修复 | enter_node 同一性比较已重构，无存活裸 `current_url == node->url` 漏 file:// 形式 |
| P2-12 | ✅ 已处理 | `src/net/network.cpp:140` — `CURLOPT_MAXFILESIZE 64MB`（fetch_once 路径封顶） |
| P2-13 | ✅/⚰ 已修复/废弃 | `toggle_mark`(`app_navigation.cpp:309-314`) 有 bounds + null guard；`build_playlist_from_saved` 已移除 |

> **仍存活（P3/§5 域，未逐条重核）**：§5.2「测试镜像漂移」**确认存活**——`tests/test_units.cpp` 仍镜像 `classify`/`escape_sql`/`parse_time_string` 且与真实实现分歧（D13 仅 link 了 feed_parser，未动这三镜像）。其余 P3（24 项）/§5 架构建议/§6 待确认项大概率亦在模块化中处理，待按需重核。

---

## 1. P1 — 高优先 BUG

### P1-1 事务异常路径提交空结果，episode 缓存被清空  [已核实]
- **位置**：`save_episode_cache` L3508–3542
- **问题**：`begin_txn()` → `exec_sql(DELETE 旧缓存)` → `try { json::parse + 逐条 INSERT } catch { 仅 LOG }` → `commit_txn()`。若 `json::parse(episodes_json)` 抛异常（截断/非法 JSON），catch 仅记日志，随后仍 `commit_txn()`，DELETE 已生效但 INSERT 一条未插入 → **该 feed 的 episode 缓存被清空**。
- **触发**：刷新订阅时 episodes_json 被截断或非法（网络异常、磁盘写半）。
- **修复**：catch 中 `rollback_txn()` 并 return；或引入 RAII 事务守卫（析构未 commit 即 rollback）。

### P1-2 LINK 展开重写共享子节点 parent，破坏父指针不变式  [已核实]
- **位置**：`expand_link_node` L10990、L11049、L11082（三处 `child->parent = node;`，node=LINK）
- **问题**：把 target（真实节点）的 children push 到 LINK 节点时执行 `child->parent = node`。这些 child 与 target **共享 shared_ptr**，重写后 target 子节点的 `parent.lock()` 指向 fav_root 下的 LINK 而非 target。直接违背本文件 L10293–10297、L10908–10909 的注释，也与 `sync_link_node_status`（L10905，正确地不重设 parent）矛盾。
- **后果**：切回 RADIO/PODCAST/ONLINE 模式后，`build_playlist_from_siblings_and_save`（L11544）、`go_back` 等依赖 parent 的逻辑拿到错误父节点 → 错曲播放/导航错乱；LINK 折叠清空 children 后 parent 仍指向已失效语义的 LINK。
- **修复**：删除这三处 `child->parent = node;`，与 `sync_link_node_status` 保持一致。

---

## 2. P2 — BUG

### P2-1 YouTubeCache 独立 sqlite 连接无 busy_timeout，并发写静默失败  [已核实]
- **位置**：`save_to_db` L4507–4543、`load_from_db` L4452–4503
- **问题**：绕过 DatabaseManager 单例自行 `sqlite3_open` 同一 DB 文件，该连接**未设 `PRAGMA busy_timeout`**（单例连接设了 5000ms）。当单例持写锁时，此连接 `sqlite3_step` 立即返回 `SQLITE_BUSY`；而 `save_to_db`（L4539）对 `sqlite3_step` 返回值**完全未检查**，写失败被静默吞掉 → 缓存看似更新却未落库。
- **修复**：下沉到 DatabaseManager（统一连接 + 互斥）；或独立连接须设 busy_timeout 并检查 step 返回码。

### P2-2 save_podcast_to_cache 对 null feedUrl 抛异常，整批搜索结果被丢弃  [已核实]
- **位置**：`save_podcast_to_cache` L4166–4180，调用点 L4101–4108
- **问题**：`if (!item.contains("feedUrl")) return;` 后直接 `item["feedUrl"].get<std::string>()`。若 feedUrl 存在但为 null/数字，`.get` 抛异常。该函数在 `search()` 的 try 块内逐条调用，一条畸形项抛出即跳到外层 catch，**已解析的后续结果全部丢失**。
- **触发**：iTunes 返回某条目 feedUrl=null（常见）。
- **修复**：用类型安全取值（`is_string()` 判定或 `value` 默认），或每条调用独立 try。

### P2-3 migrate_from_json 不持久化 podcasts（崩溃首次启动后订阅丢失）  [已核实]
- **位置**：`migrate_from_json` L5509–5559
- **问题**：迁移只把 favs 写入 DB（L5546 `save_favourite`），podcasts 仅入内存向量；末尾将 data.json 改名为 `.migrated`。正常退出时 `save_persistent_data()`（L9011）会调 `save_data` 补救，**但若首次迁移后程序崩溃/被 kill，下次启动 db_podcasts 仍空、`.migrated` 已不存在 → podcasts 永久丢失**。
- **修复**：迁移时对每个 podcast 调 `db.save_node(...)`，与 favs 对称。

### P2-4 CLI 导入/导出路径未持 tree_mutex，与后台加载线程竞争  [已核实]
- **位置**：`import_feed`/`import_opml`/`export_podcasts` L9062–9114
- **问题**：`spawn_load_feed(node)` 后台异步解析并改写 children，主线程随即无锁调用 `Persistence::save_cache/save_data/export_opml` 序列化 `podcast_root->children` → 撕裂读/迭代器失效/崩溃。CLI 路径还跳过了 `run()` 的 `pool_.shutdown()` join 收尾。
- **修复**：序列化前后用 `lock_guard<recursive_mutex> lock(tree_mutex)` 包裹；CLI 路径补"提交任务→join→保存"统一收尾。

### P2-5 current_playlist / shuffle_map_ 多处读写未加 playlist_mutex_  [已核实]
- **位置**：shuffle 重建 L9317；`play_saved_playlist_item` L9474–9497；`enter_node` 批量播放 L11183–11193、L11481–11505
- **问题**：`clear_playlist`（L9333）显式加锁并注释"与 on_playback_ended 互斥"，但上述四处直接读写 `current_playlist`/`current_playlist_index`/`shuffle_map_` 不加锁。mpv 播放结束回调线程并发访问 → 数据竞争/迭代器失效。
- **修复**：所有读写该三成员的路径统一加 `playlist_mutex_`。

### P2-6 多处 *_root->children 写操作未持 tree_mutex  [已核实]
- **位置**：`subscribe_online_podcast` L10418；`subscribe_favourite_single` L10514；`subscribe_favourites_batch` L10567；`add_favourites_batch` 非 ONLINE 分支 L10695；`add_favourite` 无锁去重遍历 L12973–12982；`toggle_sort_order` 锁外读写 L12200–12206
- **问题**：与后台 `spawn_load_feed/spawn_load_radio` 回写 children 并发 → 数据竞争/节点损坏。对比 `subscribe_online_podcasts_batch`（L10482）和 ONLINE 分支（L10620）正确加锁，策略不统一。
- **修复**：统一 insert/erase `*_root->children` 时持 tree_mutex。

### P2-7 confirm_box 无终端尺寸保护，极窄终端负坐标写入  [已核实]
- **位置**：`confirm_box` L7426–7498（`iw-7` @ L7480）
- **问题**：`input_box`/`dialog` 都有 `if (w<? || h<5) return` 前置检查，`confirm_box` 缺失。终端 <8 列时 `iw-7` 为负，ncurses 以负列号写入（UB）；边框绘制循环也越界。CTRL+C 退出确认（L8877）即触发。
- **修复**：入口加 `if (w<20 || h<5) return false;`，`[N]o` 位置钳制 `std::max(2, iw-7)`。

### P2-8 confirm_visual_selection 负索引越界 + 未判空 node  [已核实]
- **位置**：L10746–10761
- **问题**：`start=min(visual_start_, selected_idx)` 可能为 -1 → `display_list[-1]` UB；`display_list[i].node` 未判空即解引用。
- **修复**：`if (start<0) start=0;`，空列表提前 return；`if (!node) continue;`。

### P2-9 save_radio_cache 事务异常泄漏，致 DB 写入链路卡死  [已核实]
- **位置**：`save_radio_cache` L3759–3769 + 递归 L3793–3814
- **问题**：`begin_txn()` → `DELETE FROM radio_cache` → 递归 `save_radio_node_recursive`（fmt/exec_sql 可能抛 bad_alloc）→ `commit_txn()`。异常时 commit 被跳过，事务停留在 BEGIN；下一次任何写触发 "cannot start a transaction within a transaction"，**整个 DB 写入链路卡死**。无 try/catch/rollback。
- **修复**：try/catch rollback，或 RAII 事务守卫。

### P2-10 MPV 公共方法未判 ctx_ 空指针  [已核实·待确认可达性]
- **位置**：`play_list`/`play_list_from`/`toggle_pause`/`set_volume`/`adjust_speed`/`set_speed`/`set_loop_file`/`set_loop_playlist` L5972–6101
- **问题**：`play_audio`/`play_video` 检查 `if (!ctx_) return;`，但上述 9 方法直接 `mpv_set_property(ctx_, ...)`。`initialize()` 失败时 ctx_ 置 nullptr（L5843），此后调用向 mpv 传 NULL handle → 段错误。
- **待确认**：App 是否在 initialize 失败时立即退出。若是则不可达，可降级。
- **修复**：各方法开头加 `if (!ctx_) return;`，统一风格。

### P2-11 缓存项同一性判断遗漏 file:// 形式  [已核实]
- **位置**：`enter_node` L11401 `is_same_as_playing = has_media && play_state.current_url == node->url;`
- **问题**：`current_url` 对已缓存项是 `file://<本地路径>`（见 L9484/11176/11489 的 play_url 构造），而 `node->url` 是原始 URL，二者恒不等 → "同一节目"分支（L11416 PAUSED resume、L11422）永不命中，转而走覆盖式重播/重复追加。对比 `case 'L'`（L10057）已做双形式比对。
- **修复**：同样双形式比对（计算 node 的 play_url 再比 current_url）。

### P2-12 Network::fetch 响应体无上限，可被恶意源 OOM  [已核实]
- **位置**：`write_cb` L4615–4618、`fetch` L4554–4599
- **问题**：write_cb 无界 append 到 std::string，恶意/异常 RSS 源可耗尽内存。
- **修复**：设 `CURLOPT_MAXFILESIZE`，或 write_cb 内超限返回 0 中止。

### P2-13 build_playlist_from_saved / toggle_mark 未判空指针  [已核实]
- **位置**：L11764（`current_node->url` 无空检查，对比 `build_playlist_from_siblings` L11799 有）、L12059–12060（`node->marked` 无空检查）
- **修复**：函数开头/解引用前加空检查。

---

## 3. P3 — 健壮性/兼容性/性能

| # | 位置 | 问题 |
|---|------|------|
| P3-1 | L2123–2133 | `mk_wcwidth` 把 Greek(0x0391–0x03C9)、cent/pound/yen/not-sign 强制宽度 2，违反 Unicode EAW（Ambiguous）。非 CJK 终端会错位。 |
| P3-2 | L4946–4963, L5109–5122 | `media:content`/Atom `link` 的 `type_attr`/`type` 在 `href/url` 为 NULL 时未释放 → xmlChar 小泄漏。 |
| P3-3 | OPML/RSS parse L4637+ | `xmlReadMemory` 成功后异常路径跳过 `xmlFreeDoc` → xmlDoc 泄漏。应用 `unique_ptr<xmlDoc, decltype(&xmlFreeDoc)>`。 |
| P3-4 | L4856, L4929 | `parse_channel`/`parse_item` 未过滤 `p->type==XML_ELEMENT_NODE`，对文本/注释节点 `strcmp(local_name,...)`，罕见 name=NULL 节点致崩溃。 |
| P3-5 | L3033 | DatabaseManager 析构用 `sqlite3_close` 而非 `close_v2`，遗留 stmt 时返回 BUSY 不关闭。 |
| P3-6 | L2848 | `DatabaseManager::init` 非线程安全初始化（无 call_once/原子）。 |
| P3-7 | L2735,4394,5695 | `localtime_r` 在 Windows MSVC 非标准，跨平台需封装。 |
| P3-8 | L5657,6342 | `StatusBarColorRenderer` 静态 `hue`/`mt19937`、`SleepTimer` 成员非线程安全（待确认是否单线程）。 |
| P3-9 | L7550 | UI 成员 `h/w/left_w/right_w/top_h` 未初始化，init 前调用即 UB。 |
| P3-10 | L7759 | `draw_status` 中 `char tbuf[32]` 未零初始化且 `strftime` 返回未检查，失败时栈缓冲越读。 |
| P3-11 | L9027 | 退出时 `show_tree_lines` 硬编码 `true`，用户 T 键偏好不持久化（与同函数 scroll_mode 处理不一致）。 |
| P3-12 | L7556 | `title_emoji_=true` 与注释"默认 ASCII"矛盾。 |
| P3-13 | L12689 | `delete_node` 确认只接受大写 "Y"，输入 "y" 被拒（与全文 `Y||y` 约定不一致）。 |
| P3-14 | L9425,9445 | `move_playlist_item_up/down` 两次独立 DB 写非原子，无回滚。 |
| P3-15 | L12799 | YouTube 进度 `substr(speed_pos+4, speed_end-speed_pos-4)` 第二参可能下溢抛 out_of_range。 |
| P3-16 | L13864–13885 | import/export 路径无 try/catch，抛异常即 terminate，跳过 curl/xml 清理与 atexit。 |
| P3-17 | L13873,13879 | import 后 `sleep_for(5s)` 等待后台加载属竞态（>5s 缺失/<5s 浪费）。应用 future/cv。 |
| P3-18 | L13430,13677 | `load_default_podcasts` 持 tree_mutex 做 DB I/O，阻塞所有等树锁线程。 |
| P3-19 | L9336 | `clear_playlist` 置 `current_playlist_index=-1` 但仍在播放，光标同步逻辑跳过。 |
| P3-20 | L13861,13894 | atexit 注册 tui_cleanup + catch 块手动调用 → 双重调用，非幂等时终端状态错乱。 |
| P3-21 | L13845 | `xmlSetStructuredErrorFunc` 用 C 风格转换强匹配签名，签名不一致为 UB（待确认）。 |
| P3-22 | L4601 | `lookup_apple_feed` 内 `std::regex` 每次调用重编译，应 `static const`。 |
| P3-23 | L2690 | `curl_progress_callback` 负 `bytes_diff`（重定向/续传回退）致负速度/异常 ETA。 |
| P3-24 | L559 | `signal()` 而非 `sigaction`，SYSV/BSD 语义可移植性。 |

---

## 4. 已核验为误报（不作为 bug 修复）

- **批量删除播放列表项"陈旧索引"**（子代理报 P1）：`delete_from_saved_playlist` 调用 `reorder_playlist()`（按 `sort_order` **保序**重编号，L3738）+ `load_saved_playlist()`（按 `sort_order` 加载），`to_delete` 降序排序后删除时，被删项之前的索引不变 → 实际安全。**经核验不成立**。

---

## 5. 架构优化建议

1. **拆分单文件**：13912 行单 TU 无法做真实单元测试，编译/增量构建低效。按现有注释的分层（Parser / Network / Database / MPV / UI / App / Utils）拆为独立头文件+源文件。

2. **测试为镜像副本且已漂移**  ⚠️ 高价值：`tests/test_units.cpp` 复制了 `classify`/`escape_sql`/`parse_time_string` 逻辑而非链接真实函数。镜像 `classify` 无 `suffix` 字段/path 后缀匹配/大小写不敏感 `ends_with_ci`，**已与真实实现分歧**——例如 `https://x.com/feedback`：镜像判 RSS（`url.find("/feed")` 命中）、真实判 UNKNOWN（path 不以 `/feed` 结尾）。测试通过 ≠ 代码正确。拆分模块后对真实函数做单测。

3. **事务 RAII**：`begin/commit/rollback` 裸调用已致 P1-1/P2-9。提供 `class Transaction{ ~Transaction(){ if(!committed_) rollback(); } }`。

4. **SQL 参数化**：DatabaseManager 全靠 `fmt::format` + 手写 `escape_sql`，YouTubeCache 已改参数化，应推广到全部方法，消除 escape_sql 防御层。

5. **YouTubeCache 下沉单例**：消除自开连接、统一 PRAGMA/锁（修 P2-1）。

6. **统一锁协议**：tree_mutex/playlist_mutex_ 大量"锁内收集→锁外访问字段→重新加锁"模式（P2-5/P2-6）。定义契约：节点字段访问一律持锁；提供 `add_child_locked` 等工具。

7. **回调线程不驱动业务**：`on_playback_ended` 在 mpv 事件线程持锁下发 `player.play_list`（P2-5 根因）。改为回调仅置标志，主循环处理续播，降耦合与锁面（顺带消除 P2-5 类隐患）。

8. **mpv 事件驱动**：`update_state` 每 50ms 全量 `mpv_get_property`（L6222）改 `mpv_observe_property`，降 CPU/分配。

9. **拆分巨型函数**：`enter_node`(~370 行)、`delete_node`(~325 行)、`draw_status`(~240 行)、`draw_info`(16 参数，vector by-value 每帧拷贝)。按 AppMode 分派 + `DrawContext` 聚合参数 + const 引用。

10. **popup 窗口复用**：LIST_MODE 每帧 `newwin/delwin`（L7091）→ 缓存重绘。

11. **默认订阅外置**：L13446–13672 硬编码 200 条 URL，数据与代码耦合，应外置资源/DB 表。

12. **linked_node 所有权**：用 `shared_ptr` 跨树引用（L13002）破坏所有权、阻碍回收、删除原节点后 LINK 指向游离节点。改 `weak_ptr` 或按 URL 重建。

13. **布局逻辑单一来源**：`LayoutGuard::compute` 与 `LayoutMetrics::recalculate_metrics` 重复计算 layout_ratio，应合并。

14. **CLI/TUI 路径对称收尾**：CLI 路径缺 `pool_.shutdown()` join 与 try/catch（P2-4/P3-16 根因）。

15. **清理死代码**：`needs_full_redraw` 恒 true（L7096）、Persistence `save_tree/load_tree`、`parse_channel` 取了不用的 itunes 分支、`delete_node` L12686 死分支、`YouTubeChannelParser` L5233 死变量 `line_accum`。

---

## 6. 待确认项（需进一步验证）

1. **P2-10 可达性**：App 是否在 `MPVController::initialize()` 失败时立即退出？
2. **P3-8**：StatusBarColorRenderer / SleepTimer 是否仅单线程调用？
3. **P3-21**：`xml_structured_error_handler` 实际签名是否与 `xmlStructuredErrorFunc` 严格匹配？
4. **play_list loadlist 同步性**：`play_list` 在 `mpv_command(loadlist)` 返回后立即 `SafeTmpFile::remove(tmp)`（L5997/6034），mpv 是否已同步读入？异步则列表为空。
5. **sync_link_node_status 递归**（L10887）：持有 tree_mutex 递归遍历 fav_root+podcast_root 全树，LINK 互引成环会栈溢出——确认 LINK 是否可能成环。
6. **expand_link_node 步骤 3**（L11097）：target 未找到时新建节点 insert 到 target_root 顶部，是否会被 `Persistence::save_cache` 持久化为"幽灵"节点污染数据。

---

## 7. 建议的修复顺序

1. **P1-1、P2-9**（事务 RAII，一并修两处数据丢失/卡死）— 引入 Transaction 守卫，性价比最高。
2. **P1-2**（LINK parent 不变式）— 一行删除 ×3，修导航/续播错乱。
3. **P2-5、P2-6**（锁协议统一）— 集中一轮，消除并发竞态簇。
4. **P2-1、P2-2、P2-3**（DB 层数据丢失簇）。
5. **P2-4**（CLI 收尾对称化）。
6. **P2-7、P2-8、P2-13**（空指针/越界防守）。
7. P3 批量清理 + 架构项（拆模块、真实单测）作为 v0.6 重构主线。

---

*报告生成于 2026-07-14，待用户指示后迭代（深入验证待确认项 / 修复实现 / 补充某模块细节）。*
