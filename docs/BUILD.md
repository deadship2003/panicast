# 构建 / 测试 / 运行（开发者向）

> 面向开发者的工程说明。用户向安装见仓库根 `README.md`。构建三件套（LIF-006）：`setup.sh`（环境，绝不编译）→ `build.sh` / `make`（编译，产物统一 `./bin/`）→ `make install`（文件层部署）。

## 0. 标准流程

```bash
./setup.sh             # 环境依赖 + JS 运行时（幂等；--check 仅预检不安装）
./build.sh             # release 编译 → bin/panicast（等价 make build）
sudo make install      # 文件层部署 → /usr/local/bin（含 man/文档）
panicast restart       # 若服务在跑，重启换新二进制
```

调试构建：`./build.sh --debug`（等价 `make build BUILD_TYPE=Debug`）；前台直跑守护引擎：`panicast --debug`（LIF-002）。

## 1. 依赖

### 系统库（按包管理器）

**Debian/Ubuntu：**
```bash
sudo apt-get install -y \
  mpv libmpv-dev libncurses-dev \
  libcurl4-openssl-dev libsqlite3-dev libxml2-dev libfmt-dev \
  nlohmann-json3-dev libqrencode-dev cmake ninja-build g++
```
（Debian 13+ 已把 libncurses5-dev/libncursesw5-dev 合并为 `libncurses-dev`；旧版发行版装旧名也可。）

**Arch：**
```bash
sudo pacman -S --needed mpv ncurses curl libxml2 sqlite fmt nlohmann-json qrencode cmake ninja gcc
```

**Fedora：**
```bash
sudo dnf install -y mpv mpv-devel ncurses-devel libcurl-devel sqlite-devel libxml2-devel \
  fmt-devel nlohmann-json-devel qrencode-devel cmake ninja-build gcc-c++
```

### 测试与格式化（开发者工具，可选但推荐）
- 测试：`libgtest-dev`（Debian/Ubuntu，22.04 起含 cmake config）/ `gtest`（Arch）/ `gtest-devel`（Fedora）。
- 格式化：`clang-format`（同包名）/ `clang-tools-extra`（Fedora，含 clang-tidy）。

### JS 运行时（YouTube 播放/下载**必需**，运行时依赖）
yt-dlp 2026.07+ 求解 YouTube nsig 挑战需要一个 JS 运行时：
- **quickjs-ng**（二进制 `qjs`，~2MB，冷启动快，推荐）：`./setup.sh` 把仓库自带版本装到 `/usr/local/bin/qjs`（手动放入 `vendor/quickjs/qjs`，取法见 `vendor/quickjs/README.md`）。
- **deno**（~106MB，回退）。

缺失时 YouTube 播放/下载会失败；电台/播客/本地文件不受影响。

## 2. 构建

```bash
./build.sh               # = make build = cmake -B build + cmake --build
```
产物：`bin/panicast`。

- `build.sh` / `make` 按内存自适应并行度（约 1 GiB/编译单元，留 2 GiB 余量，封顶 `nproc`），防低内存机 OOM。覆盖：`PANICAST_BUILD_JOBS=N ./build.sh`。
- 本机原生编译（无交叉编译）：各平台在本机各自编译。
- 手动：`cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build --parallel $(nproc)`。
- 全部 Makefile 标准 target：`make all/build/clean/distclean/fmt/lint/test/install/uninstall`。

## 3. 测试（需 GTest）

```bash
make test               # = cmake -DBUILD_TESTING=ON + ctest
```
- `BUILD_TESTING` 默认 `OFF`；开启后 CMake `find_package(GTest)`，找不到则**告警并跳过测试**（不阻断构建）。
- 测试目标 `test_units`（`tests/test_units.cpp`）。
- **已知限制**：当前 `test_units.cpp` 测的是**镜像副本**（自拷贝的 `URLType`/模式表），不是真实的 `url_classifier`。真实集成测试待模块拆分后补（重构计划 R0.5）。

## 4. 运行 / 冒烟

```bash
./bin/panicast               # 启动 TUI
./bin/panicast --version     # 冒烟：打印版本即正常
```

## 5. 清理

```bash
make clean              # 删 bin/（编译产物）
make distclean          # = clean + 删 build/（CMake 缓存）
./build.sh --clean      # 清产物后重新构建
```

## 6. OAuth 客户端（Google 登录，Y 模式）

把你的 `client_secret.json` 放进 `secrets/`（已 `.gitignore`），configure 时会烤进 builtin 客户端（`client_secret_builtin.h`，同样 `.gitignore`）：
- **有** `secrets/client_secret.json` → 重建即更新 builtin。
- **无** → builtin 为空，Google 登录需要运行时 `<data_dir>/client_secret*.json`（见 `google_oauth.cpp client_creds()`）。

> 历史：项目曾误提交一个 fallback client_secret（Y20 及更早），视为已泄露，请在 GCP Console 吊销并轮换。

## 7. compile_commands.json（IDE / clang-tidy）

CMake 已设 `CMAKE_EXPORT_COMPILE_COMMANDS ON`，构建后在 `build/compile_commands.json`，供 IDE 跳转与 `clang-tidy -p build <file>` 使用。

## 8. 一键本地预检（提交前）

```bash
./scripts/check.sh         # clang-format 自检（咨询） + 构建 + ctest
```
