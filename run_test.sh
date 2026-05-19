#!/bin/bash
set -euo pipefail

# 测试脚本 - FED 去重工具测试

echo "=== FED 去重工具测试 ==="
echo ""

# 测试目录
TEST_DATA_DIR="./test_data/test_global"
TEST_OUTPUT_DIR="./test_output"
FED_LOG_FILE="$TEST_OUTPUT_DIR/fed_run.log"
FED_BIN="${FED_BIN:-./build/main}"

clear_dir_content() {
    local target_dir="$1"
    mkdir -p "$target_dir"
    python3 - "$target_dir" <<'PY'
import os
import shutil
import sys

target = sys.argv[1]
for name in os.listdir(target):
    path = os.path.join(target, name)
    if os.path.isdir(path) and not os.path.islink(path):
        shutil.rmtree(path)
    else:
        os.remove(path)
PY
}

# 确保输出目录存在
mkdir -p $TEST_OUTPUT_DIR

# 清理之前的输出
clear_dir_content "$TEST_OUTPUT_DIR"

echo "1. 单进程测试"
echo "================"
t_start=$(date +%s.%N)
"$FED_BIN" "$TEST_DATA_DIR" "$TEST_OUTPUT_DIR" | tee "$FED_LOG_FILE"
t_end=$(date +%s.%N)
echo ""

# echo "2. 多进程测试 (2 进程)"
# echo "=================="
# mpirun -np 2 ./build/main $TEST_DATA_DIR $TEST_OUTPUT_DIR
# echo ""

# echo "3. 多进程测试 (4 进程)"
# echo "=================="
# mpirun -np 4 ./build/main $TEST_DATA_DIR $TEST_OUTPUT_DIR
# echo ""

echo "=== 测试完成 ==="
echo "输出结果位于: $TEST_OUTPUT_DIR"
echo "FED日志位于: $FED_LOG_FILE"
