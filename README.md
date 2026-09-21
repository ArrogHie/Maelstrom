# Maelstrom — C++17 高频撮合引擎与限价订单簿

为 2027 量化开发实习准备的核心项目。覆盖限价订单簿、连续撮合、无锁并发、低延迟优化、性能压测——国内量化私募 C++ 岗 JD 最高频出现的能力组合。

## 当前状态

W1 工程脚手架已搭建：CMake（target-based）+ GoogleTest v1.18.0（FetchContent）+ clang-tidy + clang-format + GitHub Actions CI。
W2 限价订单簿完成（O(1) 增删查、price-time FIFO，含随机差分对拍）；W3 连续撮合引擎进行中。

- 总体规划：[docs/PROJECT_PLAN.md](docs/PROJECT_PLAN.md)
- W1 搭建指南与文件详解：[docs/W1_SCAFFOLD_GUIDE.md](docs/W1_SCAFFOLD_GUIDE.md)
- W2 订单簿手写任务单：[docs/W2_ORDER_BOOK_GUIDE.md](docs/W2_ORDER_BOOK_GUIDE.md)
- W3 撮合引擎手写任务单：[docs/W3_MATCHING_GUIDE.md](docs/W3_MATCHING_GUIDE.md)

## 构建

```bash
cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DMAELSTROM_BUILD_TESTS=ON \
    -DMAELSTROM_ENABLE_CLANG_TIDY=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## 内存 / UB 检查

`scripts/run_sanitizers.sh` 用 ASan + UBSan + LSan 单独编译库与全部测试并运行，
专门抓 use-after-free / 泄漏 / 未定义行为（不修改工程构建，只读源码另编一份到 `/tmp`）：

```bash
scripts/run_sanitizers.sh                   # 全部测试
scripts/run_sanitizers.sh 'MatchingW3.*'    # 只跑某些用例
```

## 技术栈

- C++17（扩展模块将上 C++20/23）
- CMake ≥ 3.20（现代 target-based 写法）
- GoogleTest v1.18.0（FetchContent 固定版本）
- clang-tidy / clang-format
- GitHub Actions CI
