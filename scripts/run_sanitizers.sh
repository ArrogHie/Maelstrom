#!/usr/bin/env bash
# W3 内存 / UB 门禁：把库与全部测试用 ASan + UBSan + LSan 编译后运行。
#
# 这是"隔离"的验收脚本，不参与 include/ 与 src/ 的构建，也不改你的 CMake 配置：
# 它自己找 GoogleTest、自己带 sanitizer 编一遍，专门抓
#   - heap-use-after-free / heap-buffer-overflow（ASan）
#   - 有符号溢出 / 非法移位 / 空指针解引用等未定义行为（UBSan）
#   - 内存泄漏（LSan，Linux 上随 ASan 默认开启）
#
# 用法：
#   scripts/run_sanitizers.sh                      # 跑全部测试
#   scripts/run_sanitizers.sh 'MatchingW3.*'       # 只跑某些用例
#   scripts/run_sanitizers.sh 'Differential/*'
#
# 覆盖目录（一般不用改）：
#   IMPL_DIR   含 include/ 与 src/ 的根目录，默认仓库根
#   TESTS_DIR  含 .cpp 测试与 naive_*.h 的目录，默认仓库根的 tests/
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMPL_DIR="${IMPL_DIR:-$ROOT}"
TESTS_DIR="${TESTS_DIR:-$ROOT/tests}"
FILTER="${1:-}"

CXX="${CXX:-clang++}"
BUILD_DIR="${BUILD_DIR:-$(mktemp -d /tmp/maelstrom-san-XXXXXX)}"

# --- 定位 GoogleTest 源码（优先用仓库里 FetchContent 已下载的副本）---
find_gtest() {
    for base in "$ROOT/build" "$ROOT/out/build"; do
        [ -d "$base" ] || continue
        local hit
        hit="$(find "$base" -maxdepth 4 -type d -name googletest-src 2>/dev/null | head -1)"
        [ -n "$hit" ] && { echo "$hit/googletest"; return 0; }
    done
    return 1
}
GTEST="${GTEST:-$(find_gtest || true)}"
if [ -z "$GTEST" ] || [ ! -f "$GTEST/src/gtest-all.cc" ]; then
    echo "找不到 GoogleTest 源码；先跑一次 cmake -B build ... 让它 FetchContent 下载，或设置 GTEST=<googletest 子目录>" >&2
    exit 1
fi

mapfile -t LIB_SRCS < <(find "$IMPL_DIR/src" -name '*.cpp' | sort)
mapfile -t TEST_SRCS < <(find "$TESTS_DIR" -maxdepth 1 -name '*.cpp' | sort)
[ "${#LIB_SRCS[@]}" -gt 0 ] || { echo "src/ 下没有 .cpp" >&2; exit 1; }
[ "${#TEST_SRCS[@]}" -gt 0 ] || { echo "tests/ 下没有 .cpp" >&2; exit 1; }

echo "编译 [ASan+UBSan+LSan]: ${#LIB_SRCS[@]} 个库源 + ${#TEST_SRCS[@]} 个测试源"
"$CXX" -std=c++17 -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all \
    -I "$IMPL_DIR/include" -I "$TESTS_DIR" -I "$GTEST/include" -I "$GTEST" \
    "${LIB_SRCS[@]}" "${TEST_SRCS[@]}" \
    "$GTEST/src/gtest-all.cc" "$GTEST/src/gtest_main.cc" -lpthread \
    -o "$BUILD_DIR/san_tests"

echo "运行 [ASAN_OPTIONS=detect_leaks=1, halt_on_error=1]"
cd "$BUILD_DIR"
if [ -n "$FILTER" ]; then
    ASAN_OPTIONS=detect_leaks=1 halt_on_error=1 \
    UBSAN_OPTIONS=print_stacktrace=1 halt_on_error=1 \
        ./san_tests --gtest_filter="$FILTER"
else
    ASAN_OPTIONS=detect_leaks=1 halt_on_error=1 \
    UBSAN_OPTIONS=print_stacktrace=1 halt_on_error=1 \
        ./san_tests
fi
