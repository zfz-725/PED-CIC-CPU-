#!/bin/bash
set -euo pipefail

TEST_DATA_DIR="./test_data/test_institutions"
LSH_OUTPUT_DIR="./test_data/lsh_results"
PRI_CPU_OUTPUT_DIR="./test_output_pri_cpu"
FED_BIN="${FED_BIN:-./build_cpu/fed_lsh_cpu}"
PRI_CPU_BIN_CMAKE="${PRI_CPU_BIN:-./src/pri_cpu/build_cpu/pri_cpu_main}"
PRI_CPU_BIN_GPP="./src/pri_cpu/build_gpp/pri_cpu_main"
REGEN_LSH="${REGEN_LSH:-0}"
TARGET="${1:-cpu}"
LSH_COMMON_STATS_FILE="$LSH_OUTPUT_DIR/lsh_common_stats.txt"
PRI_CPU_LOG_FILE="$PRI_CPU_OUTPUT_DIR/pri_run.log"

if [ "$TARGET" != "cpu" ]; then
    echo "CPU 版仅支持 cpu，不支持: $TARGET"
    echo "用法: bash ./run_pri_test.sh [cpu]"
    exit 1
fi

clear_dir_content() {
    local target_dir="$1"
    mkdir -p "$target_dir"
    python3 - "$target_dir" <<'PY'
import os
import shutil
import sys

target = sys.argv[1]
os.makedirs(target, exist_ok=True)
for name in os.listdir(target):
    path = os.path.join(target, name)
    if os.path.isdir(path) and not os.path.islink(path):
        shutil.rmtree(path)
    else:
        os.remove(path)
PY
}

seconds_diff() {
    python3 - "$1" "$2" <<'PY'
import sys
start = float(sys.argv[1])
end = float(sys.argv[2])
print(f"{end - start:.6f}")
PY
}

seconds_add() {
    python3 - "$1" "$2" <<'PY'
import sys
a = float(sys.argv[1])
b = float(sys.argv[2])
print(f"{a + b:.6f}")
PY
}

parse_fed_timings_from_log() {
    python3 - "$1" <<'PY'
import re
import sys

path = sys.argv[1]
minhash = None
local = None
total = None
with open(path, "r", encoding="utf-8", errors="ignore") as f:
    for line in f:
        m = re.search(r"Min Hash total time:\s*([0-9.eE+-]+)\s*seconds", line)
        if m:
            minhash = float(m.group(1))
        m = re.search(r"Local dedup time:\s*([0-9.eE+-]+)\s*seconds", line)
        if m:
            local = float(m.group(1))
        m = re.search(r"Total time:\s*([0-9.eE+-]+)\s*seconds", line)
        if m:
            total = float(m.group(1))
if minhash is None or total is None:
    print("-1.000000|-1.000000|-1.000000")
else:
    if local is None:
        local = total - minhash
    print(f"{minhash:.6f}|{local:.6f}|{total:.6f}")
PY
}

read_key_from_file() {
    python3 - "$1" "$2" <<'PY'
import sys

path = sys.argv[1]
key = sys.argv[2] + "="
try:
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            if line.startswith(key):
                print(line.strip().split("=", 1)[1])
                break
except FileNotFoundError:
    pass
PY
}

read_total_s_from_stats() {
    local stats_file="$1"
    if [ ! -f "$stats_file" ]; then
        echo "-1.000000"
        return
    fi
    local total_s
    total_s=$(read_key_from_file "$stats_file" "total_s")
    if [ -z "$total_s" ]; then
        echo "-1.000000"
    else
        echo "$total_s"
    fi
}

append_combined_timing_to_stats() {
    local pri_stats_file="$1"
    local lsh_seconds="$2"
    if [ ! -f "$pri_stats_file" ]; then
        return
    fi
    local pri_total
    pri_total=$(read_key_from_file "$pri_stats_file" "total_s")
    if [ -z "$pri_total" ]; then
        pri_total="-1.000000"
    fi
    local combined_total="-1.000000"
    if [ "$lsh_seconds" != "-1.000000" ] && [ "$pri_total" != "-1.000000" ]; then
        combined_total=$(seconds_add "$lsh_seconds" "$pri_total")
    fi
    {
        echo "shared_lsh_generate_total_s=$lsh_seconds"
        echo "pri_pipeline_total_s=$pri_total"
        echo "pri_total_with_lsh_s=$combined_total"
    } >> "$pri_stats_file"
}

collect_dataset_stats() {
    python3 - "$1" <<'PY'
import os
import sys

root = sys.argv[1]
inst_count = 0
file_count = 0
doc_count = 0
for inst in sorted(os.listdir(root)):
    inst_dir = os.path.join(root, inst)
    if not os.path.isdir(inst_dir):
        continue
    inst_count += 1
    for fn in sorted(os.listdir(inst_dir)):
        fp = os.path.join(inst_dir, fn)
        if not os.path.isfile(fp):
            continue
        file_count += 1
        with open(fp, "r", encoding="utf-8") as f:
            for _ in f:
                doc_count += 1
print(f"{inst_count}|{file_count}|{doc_count}")
PY
}

collect_lsh_stats() {
    python3 - "$1" <<'PY'
import os
import sys

root = sys.argv[1]
inst_dir_count = 0
hash_bin_count = 0
for inst in sorted(os.listdir(root)):
    inst_dir = os.path.join(root, inst)
    if not os.path.isdir(inst_dir):
        continue
    inst_dir_count += 1
    for fn in os.listdir(inst_dir):
        if fn.endswith("_hashresult.bin"):
            hash_bin_count += 1
print(f"{inst_dir_count}|{hash_bin_count}")
PY
}

run_pri_cpu_with_fallback() {
    local mode="$1"
    local out_dir="$2"
    local primary_bin="$PRI_CPU_BIN"
    local backup_bin="$PRI_CPU_BIN_BACKUP"
    set +e
    "$primary_bin" --inst-root "$TEST_DATA_DIR" --lsh-root "$LSH_OUTPUT_DIR" --out "$out_dir" --mode "$mode" --num-hash 128 --bands 16 --threshold 0.8
    local rc=$?
    set -e
    if [ $rc -eq 0 ]; then
        return 0
    fi
    if [ -n "$backup_bin" ] && [ -x "$backup_bin" ]; then
        echo "CPU 可执行文件异常退出（退出码=$rc），回退到: $backup_bin"
        set +e
        "$backup_bin" --inst-root "$TEST_DATA_DIR" --lsh-root "$LSH_OUTPUT_DIR" --out "$out_dir" --mode "$mode" --num-hash 128 --bands 16 --threshold 0.8
        rc=$?
        set -e
        if [ $rc -eq 0 ]; then
            PRI_CPU_BIN="$backup_bin"
            return 0
        fi
    fi
    echo "CPU 模式运行失败，退出码=$rc"
    exit $rc
}

print_final_timing_summary() {
    local grand_total="0.000000"
    local has_valid="0"
    local cpu_sha_total
    local cpu_oprf_total

    echo "4. 时间汇总"
    echo "=========="

    if [ "$lsh_total_seconds" != "-1.000000" ]; then
        echo "  LSH 生成: ${lsh_total_seconds}s"
        grand_total=$(seconds_add "$grand_total" "$lsh_total_seconds")
        has_valid=$((has_valid + 1))
    else
        echo "  LSH 生成: N/A"
    fi

    if [ "$local_dedup_total_seconds" != "-1.000000" ]; then
        echo "  本地去重: ${local_dedup_total_seconds}s"
        grand_total=$(seconds_add "$grand_total" "$local_dedup_total_seconds")
        has_valid=$((has_valid + 1))
    else
        echo "  本地去重: N/A"
    fi

    cpu_sha_total=$(read_total_s_from_stats "$PRI_CPU_OUTPUT_DIR/sha/pri_stats.txt")
    cpu_oprf_total=$(read_total_s_from_stats "$PRI_CPU_OUTPUT_DIR/oprf/pri_stats.txt")
    if [ "$cpu_sha_total" != "-1.000000" ]; then
        echo "  PRI_CPU SHA: ${cpu_sha_total}s"
        grand_total=$(seconds_add "$grand_total" "$cpu_sha_total")
        has_valid=$((has_valid + 1))
    else
        echo "  PRI_CPU SHA: N/A"
    fi
    if [ "$cpu_oprf_total" != "-1.000000" ]; then
        echo "  PRI_CPU OPRF: ${cpu_oprf_total}s"
        grand_total=$(seconds_add "$grand_total" "$cpu_oprf_total")
        has_valid=$((has_valid + 1))
    else
        echo "  PRI_CPU OPRF: N/A"
    fi

    if [ "$has_valid" -gt 0 ]; then
        echo "  总时间: ${grand_total}s"
    else
        echo "  总时间: N/A"
    fi
    echo ""
}

PRI_CPU_BIN=""
PRI_CPU_BIN_BACKUP=""
if [ -x "$PRI_CPU_BIN_CMAKE" ]; then
    PRI_CPU_BIN="$PRI_CPU_BIN_CMAKE"
    if [ -x "$PRI_CPU_BIN_GPP" ]; then
        PRI_CPU_BIN_BACKUP="$PRI_CPU_BIN_GPP"
    fi
elif [ -x "$PRI_CPU_BIN_GPP" ]; then
    PRI_CPU_BIN="$PRI_CPU_BIN_GPP"
    if [ -x "$PRI_CPU_BIN_CMAKE" ]; then
        PRI_CPU_BIN_BACKUP="$PRI_CPU_BIN_CMAKE"
    fi
else
    echo "未找到可执行文件: $PRI_CPU_BIN_CMAKE 或 $PRI_CPU_BIN_GPP"
    exit 1
fi

mkdir -p "$LSH_OUTPUT_DIR" "$PRI_CPU_OUTPUT_DIR"
clear_dir_content "$PRI_CPU_OUTPUT_DIR"
mkdir -p "$(dirname "$PRI_CPU_LOG_FILE")"
exec > >(tee "$PRI_CPU_LOG_FILE") 2>&1

echo "=== 隐私增强 FED 去重工具测试（CPU-only）==="
echo ""

lsh_reused_existing="1"
lsh_total_seconds="-1.000000"
local_dedup_total_seconds="-1.000000"
fed_local_total_seconds="-1.000000"
lsh_split_valid="1"
lsh_per_inst_lines=""

if [ "$REGEN_LSH" = "1" ]; then
    if [ ! -x "$FED_BIN" ]; then
        echo "REGEN_LSH=1 时未找到 CPU 可执行文件: $FED_BIN"
        exit 1
    fi
    clear_dir_content "$LSH_OUTPUT_DIR"
    echo "1. 为每个机构生成 CPU LSH 哈希结果"
    echo "================================"
    lsh_reused_existing="0"
    lsh_total_seconds="0.000000"
    local_dedup_total_seconds="0.000000"
    fed_local_total_seconds="0.000000"
    for inst_dir in "$TEST_DATA_DIR"/*; do
        if [ -d "$inst_dir" ]; then
            inst_name=$(basename "$inst_dir")
            inst_lsh_output="$LSH_OUTPUT_DIR/$inst_name"
            mkdir -p "$inst_lsh_output"
            echo "处理机构: $inst_name"
            inst_log_tmp=$(mktemp)
            t_start=$(date +%s.%N)
            "$FED_BIN" "$inst_dir" "$inst_lsh_output" --keep-hash 2>&1 | tee "$inst_log_tmp"
            t_end=$(date +%s.%N)
            inst_elapsed=$(seconds_diff "$t_start" "$t_end")
            fed_local_total_seconds=$(seconds_add "$fed_local_total_seconds" "$inst_elapsed")
            inst_timing_tuple=$(parse_fed_timings_from_log "$inst_log_tmp")
            rm -f "$inst_log_tmp"
            inst_lsh_seconds="${inst_timing_tuple%%|*}"
            inst_tail="${inst_timing_tuple#*|}"
            inst_local_dedup_seconds="${inst_tail%%|*}"
            if [ "$inst_lsh_seconds" != "-1.000000" ] && [ "$inst_local_dedup_seconds" != "-1.000000" ]; then
                lsh_total_seconds=$(seconds_add "$lsh_total_seconds" "$inst_lsh_seconds")
                local_dedup_total_seconds=$(seconds_add "$local_dedup_total_seconds" "$inst_local_dedup_seconds")
            else
                lsh_split_valid="0"
            fi
            inst_docs=$(python3 - "$inst_dir" <<'PY'
import os
import sys

cnt = 0
for fn in os.listdir(sys.argv[1]):
    fp = os.path.join(sys.argv[1], fn)
    if os.path.isfile(fp):
        with open(fp, "r", encoding="utf-8") as f:
            for _ in f:
                cnt += 1
print(cnt)
PY
)
            lsh_per_inst_lines="${lsh_per_inst_lines}${inst_name}_docs=${inst_docs}\n${inst_name}_fed_local_total_s=${inst_elapsed}\n${inst_name}_lsh_generate_s=${inst_lsh_seconds}\n${inst_name}_local_dedup_s=${inst_local_dedup_seconds}\n"
            echo ""
        fi
    done
    if [ "$lsh_split_valid" != "1" ]; then
        lsh_total_seconds="-1.000000"
        local_dedup_total_seconds="-1.000000"
    fi
else
    echo "1. 复用已有 LSH 哈希结果"
    echo "========================"
    if [ -z "$(find "$LSH_OUTPUT_DIR" -mindepth 1 -maxdepth 1 -type d 2>/dev/null)" ]; then
        echo "未检测到可用 LSH 目录，请先生成，或使用 REGEN_LSH=1 重新生成"
        exit 1
    fi
    if [ -z "$(find "$LSH_OUTPUT_DIR" -type f -name '*_hashresult.bin' 2>/dev/null)" ]; then
        echo "未检测到 *_hashresult.bin 文件，请先生成，或使用 REGEN_LSH=1 重新生成"
        exit 1
    fi
    if [ -f "$LSH_COMMON_STATS_FILE" ]; then
        existing_lsh_total=$(read_key_from_file "$LSH_COMMON_STATS_FILE" "fed_lsh_generate_total_s")
        if [ -n "$existing_lsh_total" ]; then
            lsh_total_seconds="$existing_lsh_total"
        fi
        existing_local_dedup_total=$(read_key_from_file "$LSH_COMMON_STATS_FILE" "fed_local_dedup_total_s")
        if [ -n "$existing_local_dedup_total" ]; then
            local_dedup_total_seconds="$existing_local_dedup_total"
        fi
        existing_fed_local_total=$(read_key_from_file "$LSH_COMMON_STATS_FILE" "fed_local_total_s")
        if [ -n "$existing_fed_local_total" ]; then
            fed_local_total_seconds="$existing_fed_local_total"
        elif [ -n "$existing_lsh_total" ]; then
            fed_local_total_seconds="$existing_lsh_total"
        fi
    fi
fi

dataset_stats=$(collect_dataset_stats "$TEST_DATA_DIR")
inst_count="${dataset_stats%%|*}"
remaining="${dataset_stats#*|}"
source_file_count="${remaining%%|*}"
source_doc_count="${remaining##*|}"
lsh_stats=$(collect_lsh_stats "$LSH_OUTPUT_DIR")
lsh_inst_dir_count="${lsh_stats%%|*}"
lsh_hash_bin_count="${lsh_stats##*|}"
{
    echo "test_data_root=$TEST_DATA_DIR"
    echo "institutions=$inst_count"
    echo "source_jsonl_files=$source_file_count"
    echo "source_docs=$source_doc_count"
    echo "lsh_inst_dirs=$lsh_inst_dir_count"
    echo "lsh_hash_bins=$lsh_hash_bin_count"
    echo "lsh_reused_existing=$lsh_reused_existing"
    echo "fed_lsh_generate_total_s=$lsh_total_seconds"
    echo "fed_local_dedup_total_s=$local_dedup_total_seconds"
    echo "fed_local_total_s=$fed_local_total_seconds"
    echo "pri_shared_lsh_generate_total_s=$lsh_total_seconds"
    if [ -n "$lsh_per_inst_lines" ]; then
        printf "%b" "$lsh_per_inst_lines"
    fi
} > "$LSH_COMMON_STATS_FILE"

echo "2. 运行 CPU 版 PRI 去重"
echo "======================="
echo "  2.1 PRI_CPU SHA 模式"
run_pri_cpu_with_fallback "sha" "$PRI_CPU_OUTPUT_DIR/sha"
append_combined_timing_to_stats "$PRI_CPU_OUTPUT_DIR/sha/pri_stats.txt" "$lsh_total_seconds"
echo ""
echo "  2.2 PRI_CPU OPRF 模式"
run_pri_cpu_with_fallback "oprf" "$PRI_CPU_OUTPUT_DIR/oprf"
append_combined_timing_to_stats "$PRI_CPU_OUTPUT_DIR/oprf/pri_stats.txt" "$lsh_total_seconds"
echo ""

print_final_timing_summary

echo "=== 测试完成 ==="
echo "LSH 哈希结果位于: $LSH_OUTPUT_DIR"
echo "PRI_CPU 输出位于: $PRI_CPU_OUTPUT_DIR"
echo "PRI_CPU 日志位于: $PRI_CPU_LOG_FILE"
