# 开发流程（每日一模块 · 保持绿色）

> 目标：每轮只做一个模块/一个修复，每步可编译、可测试、可回退，绝不在 bug 中迷失。
> 对应重构计划（`/mnt/e/AI/panicast_重构计划书_V1.md`）的 R0.x / R1.x / R2.x 步骤编号。

## 1. 当日工作循环

1. 从重构计划挑**一个**当日编号（如 `R1.1` 引入 EventBus）。
2. 先读它要修的 bug / 要替换的旧实现，写一句话"今天改完 X，应表现为 Y"。
3. 落地：接口 → 实现 → 切换调用方 → 测试。
4. 跑预检（见 §5）。
5. 提交（格式见 §2），绿了就停，**不贪多**。

## 2. 提交规范

格式：`<type>(<step>): <summary>`

- `type`：`feat` / `fix` / `refactor` / `docs` / `build` / `ci` / `test`
- `step`：重构计划编号（`R1.1`、`R3.4`）或缺陷号（`P1-8`、`FX-1`、`DB-3`）或迭代线（`N08`、`Y24.56`）
- 例：
  - `fix(P1-8): add ~App destructor, fix member init order`
  - `refactor(R1.1): introduce EventBus core, migrate playback signal`
  - `feat(R3.4): add IPTV/M3U parser provider`

## 3. 当日完成定义（DoD）

- [ ] `./build.sh linux` 绿（0 warning，`-Wall -Wextra -Wpedantic`）
- [ ] `ctest --test-dir build --output-on-failure` 绿（若涉及可测逻辑）
- [ ] 冒烟：`./build/panicast --version` 正常
- [ ] `CHANGELOG.md` 加一行（见 §6）
- [ ] 若有架构决策 → `DECISIONS_LOG.md` 加一条 ADR（见 §7）
- [ ] 单个 commit（单文件单 commit 尤佳）

## 4. 改代码的铁律

### 加新源文件
`CMakeLists.txt` 是**显式源列表**（非 glob）。新增 `.cpp` **必须**手动加入 `add_executable(panicast ...)` 列表（约 `:242`），否则不参与编译。头文件放 `include/panicast/<module>/`，无需登记。

### 加新解析器（节目源）
实现 `IFeedParser`（`include/panicast/parsers/feed_parser.h`），在 `.cpp` 末尾写：
```cpp
REGISTER_PARSER(YourParser)
```
零改 switch、零改调度。扩展节目类型（IPTV/RadioTime/抖音…）就走这条路（计划 R3.4+）。

### 加新字幕格式
实现 `ISubtitleParser`（`subtitle_parser.h`），在 `SubtitleParserRegistry` 构造里注册。

### 改存储
- 业务代码**禁止**直接 `sqlite3_exec`，统一走 Repository / `persistence.h`。
- schema 变更：新增 `add_column_if_missing` 式幂等迁移 + bump `SCHEMA_VERSION`，写回滚，配测试。用户数据必须跨版本无损。

### 并发
- UI（ncurses）单线程非线程安全。worker/mpv 线程**绝不**直接调 App/渲染。
- 跨线程通信用既有受控穿越点：写用 `RemoteCommandBus`，读用 `RemoteStateSnapshot`；异步通知逐步迁到 `EventBus`（计划 R1.1–R1.2）。**禁止**新增散落 `pending_select_` 式裸信号。

## 5. 提交前预检

```bash
./scripts/check.sh     # clang-format 自检（咨询，不阻断）+ 构建 + ctest
```
CI 会跑构建 + 测试（Linux x64）；格式检查目前为咨询（存量未 clang-format，待一次性 mass-format 后升级为阻断，见 `.clang-format` 头注释）。

## 6. CHANGELOG 体例

`CHANGELOG.md` 是单一总日志，按 `[模块]` 分节，按发布/迭代倒序。命名方案：基线 `panicast-V0.0.1`；修正线 `-F01..-F99`；账号线 `-Y01..-Y99`；网络控制线 `-N01..-N99`。Y/F/N 并行。每个改动至少一行。

## 7. ADR（架构决策）体例

`DECISIONS_LOG.md` 每条决策包含（照搬现有 N0x 风格）：
- **User goal / Context**：要解决什么、为什么。
- **Approach**：怎么做、关键取舍、为何否决其它方案。
- **Verification**：怎么验证的（编译 0-warning、哪些回归/测试通过）。
- **Followups / Open**：遗留项。

## 8. 文档落点

- 模块首次落地 → `docs/modules/<module>.md`（半页：职责 / 关键接口 / 依赖 / 已知坑）。
- 构建/测试 → `docs/BUILD.md`。
- 架构 → `docs/ARCHITECTURE.md`（改架构时同步更新）。
- 缺陷 → `AUDIT_REPORT.md`；决策 → `DECISIONS_LOG.md`。

## 9. 版本同步（5 处）

改版本号时同步：`CMakeLists.txt` · `vcpkg.json` · `include/panicast/core/constants.h` · `man/panicast.1` · `README.md`。

## 10. 纪律

- **"冻结/永久"只对已被代码+测试验证过的东西用**；未落地的叫"提案/proposed"。
- 先 RFC/ADR → 再接口 → 再实现 → 再测试。禁止无 ADR/接口/测试流程的代码。
- 新抽象须先答"第二个实现是谁"，否则不引入（YAGNI）。
