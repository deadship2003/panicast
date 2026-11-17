# panicast 架构总纲（精简版 · 设计准则）

> 来源：提炼自 `OpenMediaAI Platform V3.0 Phase-0 最终冻结总纲.md` + `开发概要.md`，砍掉过度设计。
> 用途：panicast 向新架构增量迁移的"为什么这么设计"准则。任务清单见 `docs/ROADMAP.md`。

## 分层（一条线）
```
Core(基础设施,无业务) → Domain(Media/MediaID,纯净) → Runtime(Network/IProxyManager, Media/AI/Download)
   → Modules(parser/subtitle/playback) → Providers(可插拔) → App/Frontend(TUI/ncurses)
```

## 7 条可落地本质
1. **分层 + 边界清晰**：Core 无业务依赖；模块经抽象通信，不互相硬调。
2. **Connectivity = 统一网络前端**：一个 `IProxyManager` 服务 Parser + **Downloader** + 字幕在线下载 + ASR/TTS/LLM 云端；**mpv 播放直连**（CDN 流地址绕代理更快）。
3. **Provider + 自注册 Registry**：复用现有 `IFeedParser`/`ISubtitleParser` + Registry，扩展不另造。
4. **Media 作中心对象（MediaID）**：模块传句柄不传 URL；`TreeNode` 作过渡实现，缓收敛。
5. **EventBus（pub/sub）**：替换 `pending_select_`+散落回调，治本竞态（P1-4/5/8）。
6. **Repository**：业务不碰裸 sqlite（复用 `persistence.h`+`*_repo`，禁裸 `exec_sql`）。
7. **接口先行（轻量）**：接口→实现→测试；决策入 `DECISIONS_LOG.md`(ADR)。
8. **多前端解耦（Core 与 UI 分离）**——信号/事件/交互与具体 UI 无关，UI 可灵活替换/调整：
   - **事件**：全部上 EventBus；Core/Domain/Runtime 是发布者，前端是订阅者。
   - **交互动作**：统一抽象为 `Action`/`Command`（与 ncurses/Qt 无关）；前端只做两件事——订阅事件做渲染、把用户输入（TUI 按键 / Qt 按钮/菜单）映射成 Action。
   - **铁律**：Core/Domain/Runtime **禁止依赖** ncurses/Qt 任何 UI 库；UI 是可插拔前端（ncurses 现、Qt 将来）。`App` god-object 的 UI 耦合必须逐步切除（M3）。

## 目标架构：消息总线 + 抽象层 + UI 解耦（核心设计意图）

**UI（交互层）必须与其它层解耦——只经消息总线通信。** 这是新架构的核心目标；当前代码尚未达标，是后续重构的主线。

```
┌─ Frontend / UI（交互层，可换：ncurses TUI / Qt / Web）────────┐
│  · 订阅事件 → 渲染（纯展示，无业务状态）                       │
│  · 用户输入 → Action → 发总线（无直接业务调用）                │
└───────────────┬──────────────────────────────────────────────┘
                │  事件 ↓（Core→UI）/ Action ↑（UI→Core）
┌───────────────┴──────────────────────────────────────────────┐
│ Message Bus（消息总线 = EventBus + ActionBus）—— UI↔核心唯一通道 │
└───────────────┬──────────────────────────────────────────────┘
                │
┌───────────────┴──────────────────────────────────────────────┐
│ Application Services（功能抽象层）                             │
│  PlaybackService / LibraryService / SearchService /            │
│  SubtitleService / AccountService / …                         │
│  · 订阅 Action → 执行业务 → 发事件；持业务状态；不碰 UI 库     │
└───────────────┬──────────────────────────────────────────────┘
                │
┌───────────────┴──────────────────────────────────────────────┐
│ Domain（Media/MediaID）+ Runtime（Playback/Connectivity/Download）+ Core（EventBus/Repo/ThreadPool）│
└───────────────────────────────────────────────────────────────┘
```

**解耦铁律：**
- UI **只**做两件事：订阅事件渲染、把输入映射成 Action 发总线。**禁止**直接调 Core/Domain/Service 方法、**禁止**持业务状态。
- Core/Services **不**依赖任何 UI 库（ncurses/Qt）；只发事件、收 Action。
- 消息总线是 UI↔核心的**唯一**通道 → UI 可换（TUI/Qt/Web），核心不动。

**当前差距（精确）：**
- UI（App/ui）**直接调** player/parse/storage —— 不经总线/Service。
- EventBus **几乎闲置**（仅 LogEvent 一处）—— 不是 UI↔Core 通道。
- **无 Action/Command 层**：`handle_input` 硬编码 `switch` → 直接方法调用。
- `App` god-object 混 UI + 逻辑 + 状态 —— 没分出 Services；UI 未解耦 → 换 UI 要重写 App。

**达标路径（重构主线，每步可编译可运行、strangler）：**
1. **EventBus 成事件骨干**：Services 发领域事件（`PlaybackStateChanged`/`MediaLoaded`/`LibraryUpdated`…），UI 订阅渲染（替代每帧轮询 `get_state` + 直接读 state）。
2. **Action 层**（D7 Keymap 升级）：UI 按键 → Action → 总线；Services 订阅处理（替代 `handle_input` switch → 直接调用）。
3. **Application Services（功能抽象层）**：从 `App` god-object 抽出 `PlaybackService`/`LibraryService`/… —— 持业务状态、处理 Action、发事件。UI 只跟总线说话。
4. **UI 纯交互化**：只订阅事件渲染 + 按键→Action。无逻辑、无直接 Core 调用。
5. Core/Domain/Runtime 不变（已 ncurses-clean）。

## 砍掉的过度设计（避免重蹈覆辙）
- "永久冻结 / 唯一事实来源 / 对标 Linux Kernel" 修辞——零工程价值。
- "12 框架一次冻结"——按需演进，用到哪个立哪个。
- 双仓库(Spec/Source)、五卷丛书、千页手册、全量 OMPS——solo 项目是负债。
- "Phase-0 已完成"等不实声明——只对被代码+测试验证过的说"完成"。
- **暂不跳 Qt6**——保留 ncurses（已 mass-format 打磨）；Qt6 是独立大决策，另议。

## 现状 → 目标映射
| 层 | 目标 | panicast 现状 | 动作 |
|---|---|---|---|
| Core | 基础设施 | `src/core`(logger/thread_pool/crypto/paths) | 补 **EventBus** |
| Domain | Media 纯领域 | `TreeNode` god-struct | 提炼 **Media/MediaID**（缓） |
| Runtime | Network Runtime | 散在 net/app | 抽 **IProxyManager** |
| Modules/Providers | 可插拔 | `IFeedParser`/`ISubtitleParser`+Registry | **已有，扩展**（Downloader 接入） |
| App/Frontend | TUI | `src/app`+`src/ui`(god-objects) | 拆 god-object、**Keymap** |
| Storage | Repository | `persistence.h`+`*_repo` | extend，禁裸 SQL |
