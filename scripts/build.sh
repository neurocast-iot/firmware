#!/bin/bash
# 一键构建脚本
# 用法：./scripts/build.sh [arm-ak3918|x86-debug|x86-release]（默认 x86-debug）
#
# x86-debug 默认开 NC_BUILD_TESTS，编译完自动跑 ctest
# 单测失败：exit 1（不打包、直接退出），让 CI 能捕获
# x86-release 和 arm-ak3918 不开测试（避免拖慢）
set -e

PRESET="${1:-x86-debug}"
cd "$(dirname "$0")/.."

# 编译
cmake --preset "${PRESET}"
cmake --build --preset "${PRESET}" -j"$(nproc)"

# x86-debug 跑单测（前提是 NC_BUILD_TESTS=ON，这个 preset 默认开）
if [ "${PRESET}" = "x86-debug" ]; then
    echo ""
    echo "==> 跑单测（ctest, GoogleTest）"
    # --output-on-failure 让失败 case 打印详细日志
    # --timeout 防止单测卡住
    ctest --test-dir "build/${PRESET}" --output-on-failure --timeout 120
    echo "==> 单测全部通过 ✓"
fi
