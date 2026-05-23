"""用 PED-CIC test_institutions 数据跑 EP_MPD 去重并计时。"""
import json
import os
import sys
import time
from pathlib import Path

# Add EP_MPD to path
EP_MPD_DIR = Path(__file__).resolve().parent.parent / "代码及数据" / "deduplication" / "EP_MPD"
sys.path.insert(0, str(EP_MPD_DIR))

from ep_mpd import MultiPartyDeduplicator, EgPsiType, EgPsiDataType


def load_institutions(inst_root: str, max_clients: int = 10):
    """读取 PED-CIC test_institutions JSONL，按机构返回文本列表。"""
    inst_dir = Path(inst_root)
    client_data = []
    inst_names = sorted(
        [d for d in inst_dir.iterdir() if d.is_dir()],
        key=lambda x: x.name,
    )[:max_clients]

    for inst in inst_names:
        texts = []
        for jsonl in sorted(inst.glob("*.jsonl")):
            with open(jsonl, "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        obj = json.loads(line)
                        texts.append(obj.get("text", line))
                    except json.JSONDecodeError:
                        texts.append(line)
        client_data.append(texts)
    return client_data, inst_names


def count_before_after(client_data):
    before = sum(len(c) for c in client_data)
    all_unique = set()
    for c in client_data:
        all_unique.update(c)
    after = len(all_unique)
    return before, after


def run_epmpd(client_data, eg_type, label):
    before, expected_after = count_before_after(client_data)
    print(f"\n--- EP_MPD {label} ---")
    print(f"  参与方: {len(client_data)}, 总元素: {before}, 去重后应有: {expected_after}")

    t0 = time.perf_counter()
    mpd = MultiPartyDeduplicator(
        client_data=client_data,
        data_type=EgPsiDataType.STR,
        eg_type=eg_type,
        debug=False,
    )
    mpd.deduplicate()
    result = mpd.get_combined_dataset()
    t1 = time.perf_counter()

    wall_s = t1 - t0
    removed = before - len(result)
    print(f"  去重后元素: {len(result)}, 删除: {removed}")
    print(f"  Wall clock: {wall_s:.4f}s")

    # Verify correctness
    if len(result) != expected_after:
        print(f"  ⚠ 结果不一致! 期望 {expected_after}, 实际 {len(result)}")
    else:
        print(f"  ✓ 去重结果正确")

    return wall_s, removed


def main():
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--inst-root", default="./test_data/test_institutions")
    parser.add_argument("--max-clients", type=int, default=10)
    parser.add_argument("--dup-per", type=float, default=0.3)
    args = parser.parse_args()

    # Regenerate test data with consistent params
    print("=== 重新生成测试数据 ===")
    os.system(
        f"DUP_RATIO={args.dup_per} DATA_TYPE=number python3 create_test_data.py"
    )

    client_data, inst_names = load_institutions(args.inst_root, args.max_clients)
    print(f"读取 {len(client_data)} 个机构")
    for i, (name, data) in enumerate(zip(inst_names, client_data)):
        print(f"  {name}: {len(data)} 条文档")

    # EP_MPD Type 1
    t1, r1 = run_epmpd(client_data, EgPsiType.TYPE1, "Type1 (AES-CBC)")

    # EP_MPD Type 2
    t2, r2 = run_epmpd(client_data, EgPsiType.TYPE2, "Type2 (OPRF)")

    print(f"\n=== 对比汇总 ===")
    print(f"  数据规模: {len(client_data)} 方 × ~{len(client_data[0])} 条 = ~{sum(len(c) for c in client_data)} 条")
    print(f"  EP_MPD Type1: {t1:.4f}s, 删除 {r1} 条")
    print(f"  EP_MPD Type2: {t2:.4f}s, 删除 {r2} 条")

    # PED-CIC PRI timing from latest run
    print(f"\n  PED-CIC-CPU- (同数据，见 benchmark 输出):")
    print(f"    PRI SHA:  ~26.45s coordinator, 3000 重复对")
    print(f"    PRI OPRF: ~63.78s coordinator, 3000 重复对")


if __name__ == "__main__":
    main()
