# Maelstrom — C++17 高频撮合引擎与限价订单簿

为 2027 量化开发实习准备的核心项目。覆盖限价订单簿、连续撮合、无锁并发、低延迟优化、性能压测——国内量化私募 C++ 岗 JD 最高频出现的能力组合。

## 当前状态

W1 工程脚手架已搭建：CMake（target-based）+ GoogleTest v1.18.0（FetchContent）+ clang-tidy + clang-format + GitHub Actions CI。
订单簿/撮合实现进行中（W2 起）。

- 总体规划：[docs/PROJECT_PLAN.md](docs/PROJECT_PLAN.md)
- W1 搭建指南与文件详解：[docs/W1_SCAFFOLD_GUIDE.md](docs/W1_SCAFFOLD_GUIDE.md)

## 构建

```bash
cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DMAELSTROM_BUILD_TESTS=ON \
    -DMAELSTROM_ENABLE_CLANG_TIDY=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## 技术栈

- C++17（扩展模块将上 C++20/23）
- CMake ≥ 3.20（现代 target-based 写法）
- GoogleTest v1.18.0（FetchContent 固定版本）
- clang-tidy / clang-format
- GitHub Actions CI
