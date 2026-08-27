# W1 脚手架搭建指南与文件详解

> 本文是 Maelstrom 项目 W1 工程脚手架的完整指南：Part A 是搭建步骤，Part B 是每个文件的作用与代码逐项详解，Part C 是串联总览。

## 版本基线（2026-08）

| 工具 | 版本 | 备注 |
|---|---|---|
| CMake | 4.4.2 | `cmake_minimum_required` ≥ 3.5（4.0 起不兼容更老），本项目用 3.20 |
| GoogleTest | v1.18.0 | 要求 C++17；用 `GIT_TAG` 固定而非 commit hash |
| GitHub Actions | `actions/checkout@v4` | `ubuntu-latest` 预装 CMake/GCC/Clang |

---

# Part A — 搭建步骤

## Step 0 ｜ 开发环境（强烈建议 WSL2）

理由：① W5–W7 要用 `perf` / `flamegraph`（Linux 工具）；② JD 几乎都要求 Linux。

```bash
# WSL2 Ubuntu 里：
sudo apt update
sudo apt install -y build-essential cmake ninja-build clang clang-tidy clang-format git
cmake --version       # apt 给的约 3.28+，够用（FetchContent_MakeAvailable 需 3.14+）
clang-tidy --version
```

> ⚠️ **性能基准红线**：项目要放进 **WSL 原生文件系统**（如 `~/projects/Maelstrom`），**不要**放在 `/mnt/d/...`。WSL 访问 `/mnt/d`（Windows 盘挂载）极慢，会让 W7 的纳秒级延迟测量失真。Windows 侧如需查看可通过 `\\wsl$\...` 访问。从 W1 第一天就养成这个习惯。

> 本仓库当前在 Windows 侧 `D:\~~~C++\Quant\Maelstrom`，便于 git 推送。开发时建议 `git clone` 一份到 WSL home 做实际编码与性能测量。

## Step 1 ｜ 目录结构

```
Maelstrom/
├── .gitignore  .clang-format  .clang-tidy
├── CMakeLists.txt
├── README.md
├── .github/workflows/ci.yml
├── include/maelstrom/      # 公共头文件（对外 API）
│   ├── types.h             # Price/Quantity/OrderId 强类型
│   ├── order.h
│   └── order_book.h        # 订单簿 public 接口（W2 填实现）
├── src/                    # 实现
│   └── order_book.cpp
├── tests/
│   ├── CMakeLists.txt
│   └── test_order_book.cpp
├── bench/                  # W7 才填，先建空目录占位
└── docs/
    ├── PROJECT_PLAN.md
    └── W1_SCAFFOLD_GUIDE.md
```

约定：**公开 API 放 `include/maelstrom/`，实现放 `src/`**。测试只 `#include "maelstrom/order_book.h"`，不碰 `src/`。

## Step 2 ｜ 根 `CMakeLists.txt`（现代 target-based）

见仓库根目录 `CMakeLists.txt`。要点（面试会问）：

- `target_include_directories(... PUBLIC ...)` 让链接 `maelstrom` 的目标自动拿到 include 路径，无需手动 `-I`。
- `CMAKE_EXPORT_COMPILE_COMMANDS=ON` 生成 `compile_commands.json`，是 clang-tidy 和编辑器/clangd 的输入。
- `CMAKE_CXX_CLANG_TIDY` 必须在 `add_library` **之前**设置，才会注入到每个源文件的编译命令里。

## Step 3 ｜ `tests/CMakeLists.txt`（GoogleTest via FetchContent）

见 `tests/CMakeLists.txt`。用 `GIT_TAG v1.18.0` 固定版本，比官方 quickstart 里的 URL+commit-hash 写法更清晰、可升级。

## Step 4 ｜ `.clang-tidy`

白名单策略：先 `-*` 全关，再逐个开 `bugprone-*`/`modernize-*`/`performance-*`/`readability-*`/`cert-*`/`cppcoreguidelines-*`，并禁掉几个噪声大的（trailing return type、单字母变量名、magic number）。前两周跑下来按噪声再调。

## Step 5 ｜ `.clang-format` + `.gitignore`

`.clang-format` 用 Google 风格底、缩进 4 空格。`.gitignore` 排除 `build/`、`compile_commands.json`、`*.o`/`*.obj` 等构建产物。

## Step 6 ｜ GitHub Actions CI（`.github/workflows/ci.yml`）

push/PR 到 main 时在 `ubuntu-latest` 上自动跑 配置→构建→测试，绿勾是"工程规范"的直接证据。

## Step 7 ｜ C++ 代码骨架

见 `include/maelstrom/types.h`、`order.h`、`order_book.h`、`src/order_book.cpp`、`tests/test_order_book.cpp`。W1 只放占位实现打通流水，W2 替换为真正 O(1) 数据结构。

## Step 8 ｜ 构建、跑测试、跑 clang-tidy

```bash
cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DMAELSTROM_BUILD_TESTS=ON \
    -DMAELSTROM_ENABLE_CLANG_TIDY=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

预期：测试全绿。clang-tidy 若报错，先读提示，多数带 `fix-it` 可自动修。

## Step 9 ｜ 提交首版

```bash
git add -A
git commit -m "W1: scaffold CMake + GoogleTest + clang-tidy + strong types skeleton"
git branch -M main
git remote add origin git@github.com:ArrogHie/Maelstrom.git
git push -u origin main
```

## Step 10 ｜ 验收清单（W1 完成标志）

- [ ] `cmake -B build` 成功，`build/compile_commands.json` 存在
- [ ] `ctest` 全绿，至少 2 个测试通过
- [ ] clang-tidy 在构建时跑起来（哪怕有告警）
- [ ] CI 在 GitHub 上 push 后自动跑通（绿勾）
- [ ] `Price`/`Quantity`/`OrderId` 三类互相赋值会编译失败（强类型生效）
- [ ] README 写一段：项目目标 + 一句话架构 + 构建/测试命令

---

# Part B — 每个文件的作用与代码详解

## 1. `CMakeLists.txt`（根目录）

整个项目的"编译说明书"。OJ 里 `g++ a.cpp` 就行，但真实项目多文件、多平台、多配置、外部依赖——需要构建系统告诉编译器"编什么、怎么编、连什么"。CMake 是 C++ 事实标准。

- `cmake_minimum_required(VERSION 3.20)` — 版本门槛，低于此版本直接报错退出；选 3.20 因 FetchContent_MakeAvailable 需 ≥ 3.14，且 4.0 起拒绝 < 3.5 的声明。
- `project(Maelstrom VERSION 0.1.0 LANGUAGES CXX)` — 项目身份；`LANGUAGES CXX` 只启用 C++ 工具链。
- `CMAKE_CXX_STANDARD=17` + `REQUIRED ON` + `EXTENSIONS OFF` — 强制标准 `-std=c++17`，不支持 17 就报错而非降级；关 GNU 扩展保证可移植性（JD 要"现代标准 C++"）。
- `CMAKE_EXPORT_COMPILE_COMMANDS ON` — 导出每个源文件的完整编译命令到 `compile_commands.json`，clang-tidy/clangd/VSCode 靠它解析头文件。
- `option(...)` — 带默认值的布尔缓存变量，`cmake -D` 切换，CI/本地/调性能各用不同配置。
- clang-tidy 块：`find_program` 找 clang-tidy；`CMAKE_CXX_CLANG_TIDY` 是内建变量，设了它 CMake 编每个 `.cpp` 时自动先调 clang-tidy；`-p ${CMAKE_BINARY_DIR}` 指向 build 目录读 compile_commands。**必须在 add_library 之前设**。
- `function(maelstrom_target_warnings)` — 把"开哪些警告"封装成函数复用；`-Werror` 把警告当错（C++ 里一个 `-Wconversion` 警告往往就是 bug 雏形）；`PRIVATE` 表示 flag 只作用于本目标不传染链接者。
- `add_library(maelstrom STATIC ...)` — 静态库；`ALIAS maelstrom::maelstrom` 给带命名空间的名字，清晰且不被同名普通 target 覆盖（vcpkg/Conan 包都这么命名）。
- `target_include_directories PUBLIC $<BUILD_INTERFACE:...>` — `PUBLIC` = 自己编译要用 + 链接者自动拿到；生成器表达式保证安装时不泄漏内部路径。
- `enable_testing()` + `add_subdirectory(tests)` — 开 ctest，跳进 tests 执行其 CMakeLists；开关一关整个测试子树不参与构建。

## 2. `tests/CMakeLists.txt`

测试子项目构建说明。独立成文件：测试逻辑和主库分开，删 tests 主库照样能编。

- `FetchContent_Declare` + `GIT_TAG v1.18.0` — 配置时从 GitHub clone googletest 到 build 目录，**不污染源码树**；固定版本保证构建可复现。
- `gtest_force_shared_crt ON` — Windows 专属，避免 CRT 冲突链接错。
- `FetchContent_MakeAvailable` — clone + add_subdirectory 一气呵成。
- `GTest::gtest_main` — 自带 `main()`，测试文件只写 `TEST(...)` 不写 main。
- `gtest_discover_tests` — 运行时探查可执行文件里有哪些 TEST，每个注册成独立 ctest 条目，可 `ctest -R Smoke` 精确跑。

## 3. `.clang-tidy`

静态分析配置。编译器警告抓"这行代码有问题"，clang-tidy 抓"这个写法不够现代/有隐患"（该用 `std::move` 没用、该用 `make_unique` 而非 `new`、空指针风险）——C++ 版的 Rust clippy。

- `-*` 先全关再逐个开——白名单策略，避免新检查一上来炸一堆。
- 各前缀含义：`bugprone-*` 易错写法、`modernize-*` 升级现代写法、`performance-*` 性能反模式、`cert-*` 安全规范、`cppcoreguidelines-*` Core Guidelines。
- `-xxx` 关掉吵的：trailing return type（个人偏好）、identifier-length（算法代码 `i,j,k` 满天飞）、magic-numbers（算法里到处常数）。
- `WarningsAsErrors: ''` — 告警不当错，起步阶段。
- `HeaderFilterRegex '.*'` — 连头文件告警也报。

## 4. `.clang-format`

代码格式自动统一。配进编辑器保存时自动格式化，省掉"花括号该不该换行"的口水战，减少团队 diff 噪声。

## 5. `.gitignore`

告诉 git 哪些文件不进版本库。`build/`、`compile_commands.json`、`*.o`/`*.obj` 都是构建产物（可重新生成），提交进去会让仓库膨胀且每次构建都冲突。

## 6. `.github/workflows/ci.yml`

push/PR 到 main 时，GitHub 在云端 Ubuntu 自动跑 配置→构建→测试。意义：① 提交即崩立刻知道；② 仓库有 CI 绿勾是"工程规范"的直接证据；③ CI 是活文档，证明别人 clone 能复现构建。

- `actions/checkout@v4` — 拉代码到 runner，固定 v4 避免 action 升级打破构建。
- `cmake -B build` — out-of-source 构建，不污染源码。
- `cmake --build build -j` — 并行编译。
- `ctest --test-dir build --output-on-failure` — 在 build 目录跑测试，失败时打印详情。

## 7. `include/maelstrom/types.h`

**强类型**——W1 最重要的 C++ 工程知识点之一。

- `StrongType<Tag, T>` 是 **phantom type 模式**：`Tag` 不参与运行时，只在类型系统做标记。`explicit` 禁止隐式转换，所以 `Price p = 100;` 编不过，必须 `Price p{100}`。
- `Price`/`Quantity`/`OrderId` 底层都是 `int64_t`，但类型系统视它们为不同类型：`void execute(Price, Quantity);` 调 `execute(Quantity{10}, Price{100})` 编译失败。**把"参数传反"这类 bug 挡在编译期，零运行时成本**。面试官问"怎么防止金额/数量混淆"就答这个。
- `Price` 用 `int64_t` 而非 `double`：金额永远定点整数（单位 tick），**永远不用浮点**——浮点 0.1+0.2≠0.3，金融场景灾难。W2 封装 tick→价格换算。

## 8. `include/maelstrom/order.h`

领域模型。

- `enum class Side : std::uint8_t { Buy, Sell };` — 强枚举而非老 `enum`：作用域隔离、不隐式转 int、可指定底层类型省内存。一行同时拿到类型安全和内存紧凑。
- `Order` 结构体把 id/price/quantity/side 打包，是订单簿操作的基本单元。放 `include/` 因它是公开 API 的一部分。

## 9. `include/maelstrom/order_book.h`

订单簿的**公共接口**。定义"能做什么"，不定义"怎么做"（后者在 `src/order_book.cpp`）。

- `[[nodiscard]] std::optional<Order> best_bid() const;` — `[[nodiscard]]` 强制调用者接返回值，`book.best_bid()` 单独一行不接结果会警告；`std::optional<Order>` 表示"可能没有"（空簿时 best_bid 不存在），比裸指针/null 安全、比抛异常便宜。C++17 替代"返回 nullptr + 注释所有权"的现代写法。
- `struct Impl; Impl* impl_;` — **pImpl（pointer to implementation）习语**：头文件只声明"有个 Impl 结构体"，真成员（W2 的 hash、链表）写在 `.cpp` 里。收益：① ABI 稳定（W2 大改内部数据结构，头文件不动，依赖者不用重编）；② 编译速度（头文件不暴露重模板）；③ 封装（私有成员真的私有）。代价是一次间接指针访问。学习阶段建议两种都写一遍感受差异。
- 显式 `= delete` 拷贝构造/赋值——pImpl 持有裸指针，默认拷贝会导致双重释放；禁用拷贝防止误用（W2 再定义移动或实现值语义）。

## 10. `src/order_book.cpp`

订单簿的**实现**。W1 占位（vector + `std::find_if`），目的是打通编译/测试流水。

- `struct OrderBook::Impl { std::vector<Order> orders; };` — 这里定义头文件里只声明的 `Impl`。头文件看不到这些成员——pImpl 的"实现隐藏"就靠这一手。
- `best_bid` 用 `std::max_element` O(n) 扫描——**这正是 W2 要消灭的**：真撮合系统每秒千万单，O(n) best_bid 会拖死。保留慢版本有用：W2 拿它当"参考实现"做对照测试（property-based testing：随机操作序列下，快版和慢版结果必须一致）。
- 构造 `new Impl{}`、析构 `delete impl_` — W1 偷懒没上 RAII；W2 换 `std::unique_ptr<Impl>`，符合现代 C++ "不裸 new" 规范。clang-tidy 的 `modernize-*` 会提示，留给你修。

## 11. `tests/test_order_book.cpp`

```cpp
TEST(OrderBook, SmokeAddAndSize) {
    OrderBook book;
    EXPECT_EQ(book.size(), 0u);
    book.add(Order{OrderId{1}, Price{100}, Quantity{10}, Side::Buy});
    EXPECT_EQ(book.size(), 1u);
    EXPECT_TRUE(book.best_bid().has_value());
}
```

**冒烟测试**：不验证复杂逻辑，只逼通整条工具链（CMake 找头文件、链接库、GoogleTest 运行、ctest 发现测试）。W1 这条绿了，说明从源码到测试的管道没断，W2 才有底气加真逻辑。

- `EXPECT_*` 失败不中断当前测试（继续跑后续断言），`ASSERT_*` 失败立刻停——冒烟用 EXPECT 即可。
- `0u`/`1u` 后缀 `u` 是 unsigned 字面量，避免 `size_t` vs `int` 的 `-Wsign-compare` 警告（你开了 `-Wconversion`，这点重要）。

---

# Part C — 串联：每个文件的工程角色

| 文件 | 角色 | 不写它会怎样 |
|---|---|---|
| `CMakeLists.txt` | 构建编排 | 手写几十行 g++ 命令，换机器就崩 |
| `tests/CMakeLists.txt` | 测试编排+拉依赖 | 测试散落，googletest 污染源码树 |
| `.clang-tidy` | 静态分析 | 易错写法/非现代写法没人提醒 |
| `.clang-format` | 格式统一 | 团队格式口水战，diff 噪声 |
| `.gitignore` | 版本库卫生 | build 产物进 git，仓库膨胀冲突 |
| `ci.yml` | 自动验证 | 提交即崩没人知道，仓库无"工程规范"证据 |
| `types.h` | 类型安全防线 | Price/Quantity 传反 bug 进生产 |
| `order.h` | 领域模型 | 没有统一的订单表示 |
| `order_book.h` | 公共接口 | 实现暴露，ABI 不稳，改一处全重编 |
| `order_book.cpp` | 实现 | 接口无体 |
| `test_order_book.cpp` | 工具链冒烟 | 管道没验通，W2 加逻辑如履薄冰 |

**这一周真正在学的是 C++ 工程的"地基"**：构建系统、依赖管理、静态分析、测试集成、类型安全、接口/实现分离、CI。算法题里这些都不存在，但 JD 里"现代 C++ 工程能力"指的就是这一整套。地基扎实了，W2 的 O(1) 订单簿才能盖得稳。
