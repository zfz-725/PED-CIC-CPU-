#!/usr/bin/env python3
import os
import json
import random
import math

def _normalize_dup_ratio(dup_ratio):
    ratio = float(dup_ratio)
    if ratio < 0 or ratio > 1:
        raise ValueError(f"dup_ratio 必须在 [0, 1] 区间，当前: {dup_ratio}")
    return ratio

def create_string_test_data(output_dir, num_files=5, lines_per_file=1000, dup_ratio=0.8):
    dup_ratio = _normalize_dup_ratio(dup_ratio)
    os.makedirs(output_dir, exist_ok=True)
    
    # 生成一些基础文本
    base_texts = [
        "这是一段测试文本，用于生成重复数据。",
        "This is a test text for generating duplicate data.",
        "这是另一段测试文本，可能会在多个文件中重复出现。",
        "Another test text that may appear in multiple files.",
        "测试数据生成器正在创建重复内容。",
        "The test data generator is creating duplicate content.",
        "这是一段唯一的文本，不会在其他文件中出现。",
        "This is a unique text that won't appear in other files.",
        "重复内容测试，重复内容测试，重复内容测试。",
        "Duplicate content test, duplicate content test, duplicate content test."
    ]
    
    # 为每个文件生成内容
    for i in range(num_files):
        file_path = os.path.join(output_dir, f"test_{i}.jsonl")
        with open(file_path, 'w', encoding='utf-8') as f:
            for j in range(lines_per_file):
                if random.random() < dup_ratio:
                    # 从基础文本中随机选择
                    text = base_texts[random.randint(0, 5)]
                else:
                    # 生成唯一内容
                    text = f"唯一内容 {i}_{j}: {random.randint(1000, 9999)}"
                
                # 写入JSONL格式
                f.write(json.dumps({"text": text}, ensure_ascii=False) + '\n')
    
    print(f"测试数据已创建在 {output_dir}")

def create_number_institutions_pairwise(base_dir, num_institutions=10, num_files=2, lines_per_file=500, dup_ratio=0.3):
    dup_ratio = _normalize_dup_ratio(dup_ratio)
    if num_institutions < 2:
        raise ValueError("number 模式至少需要 2 个机构")
    os.makedirs(base_dir, exist_ok=True)

    institution_data_size = num_files * lines_per_file
    total_data = num_institutions * institution_data_size
    target_unique = math.floor(total_data * (1 - dup_ratio))
    min_unique_required = institution_data_size
    if target_unique < min_unique_required:
        raise ValueError(
            f"当前配置无法同时满足“机构内全唯一”和 dup_ratio={dup_ratio}。"
            f"在 num_institutions={num_institutions} 时，dup_ratio 最大约为 {1 - 1 / num_institutions:.4f}"
        )
    unique_pool = list(range(target_unique))
    random.shuffle(unique_pool)

    client_data = [[] for _ in range(num_institutions)]
    client_sets = [set() for _ in range(num_institutions)]

    inst_cursor = 0
    for value in unique_pool:
        while len(client_data[inst_cursor]) >= institution_data_size:
            inst_cursor = (inst_cursor + 1) % num_institutions
        client_data[inst_cursor].append(value)
        client_sets[inst_cursor].add(value)
        inst_cursor = (inst_cursor + 1) % num_institutions

    value_cursor = 0
    for i in range(num_institutions):
        while len(client_data[i]) < institution_data_size:
            found = False
            for _ in range(target_unique):
                value = unique_pool[value_cursor % target_unique]
                value_cursor += 1
                if value not in client_sets[i]:
                    client_data[i].append(value)
                    client_sets[i].add(value)
                    found = True
                    break
            if not found:
                raise RuntimeError("未能为机构填充足够的唯一元素，请检查数据生成参数")
        random.shuffle(client_data[i])

    for i in range(num_institutions):
        inst_dir = os.path.join(base_dir, f"inst_{i}")
        os.makedirs(inst_dir, exist_ok=True)
        elements = client_data[i]
        for file_idx in range(num_files):
            start = file_idx * lines_per_file
            end = start + lines_per_file
            chunk = elements[start:end]
            out = os.path.join(inst_dir, f"test_{file_idx}.jsonl")
            with open(out, "w", encoding="utf-8") as f:
                for element in chunk:
                    f.write(json.dumps({"text": element}, ensure_ascii=False) + "\n")

    actual_unique = len(set(v for row in client_data for v in row))
    actual_dup_ratio = 1 - actual_unique / total_data
    print(f"机构目录结构已创建在 {base_dir}")
    print(f"全局重复率(按总数据): {actual_dup_ratio:.6f}")

# 创建机构目录结构
def create_institution_structure(base_dir, num_institutions=3, data_type="number", dup_ratio=0.8, num_files=2, lines_per_file=500):
    os.makedirs(base_dir, exist_ok=True)

    if data_type == "string":
        for i in range(num_institutions):
            inst_dir = os.path.join(base_dir, f"inst_{i}")
            create_string_test_data(inst_dir, num_files=num_files, lines_per_file=lines_per_file, dup_ratio=dup_ratio)
        print(f"机构目录结构已创建在 {base_dir}")
    elif data_type == "number":
        create_number_institutions_pairwise(
            base_dir,
            num_institutions=num_institutions,
            num_files=num_files,
            lines_per_file=lines_per_file,
            dup_ratio=dup_ratio,
        )
    else:
        raise ValueError(f"data_type 仅支持 string 或 number，当前: {data_type}")

def create_global_from_institutions(institution_dir, global_dir, num_files=10, dup_ratio=0.3, data_type="number"):
    os.makedirs(global_dir, exist_ok=True)

    for name in os.listdir(global_dir):
        path = os.path.join(global_dir, name)
        if os.path.isfile(path):
            os.remove(path)

    if data_type == "string":
        create_string_test_data(global_dir, num_files=num_files, lines_per_file=1000, dup_ratio=dup_ratio)
        return
    if data_type != "number":
        raise ValueError(f"data_type 仅支持 string 或 number，当前: {data_type}")

    total_count = 10000
    target_unique_count = int(total_count * (1 - dup_ratio))
    unique_values = list(range(target_unique_count))
    all_data = []
    for value in unique_values:
        all_data.append({"text": value})
    while len(all_data) < total_count:
        value = random.choice(unique_values)
        all_data.append({"text": value})
    random.shuffle(all_data)
    buckets = [[] for _ in range(num_files)]
    for idx, data in enumerate(all_data):
        buckets[idx % num_files].append(json.dumps(data, ensure_ascii=False) + "\n")
    for i in range(num_files):
        out = os.path.join(global_dir, f"test_{i}.jsonl")
        with open(out, "w", encoding="utf-8") as f:
            f.writelines(buckets[i])

    print(f"全局测试数据已创建在 {global_dir}")

if __name__ == "__main__":
    dup_ratio = float(os.environ.get("DUP_RATIO", "0.3"))
    data_type = os.environ.get("DATA_TYPE", "number").strip().lower()
    create_institution_structure("./test_data/test_institutions", num_institutions=10, data_type=data_type, dup_ratio=dup_ratio)
    create_global_from_institutions("./test_data/test_institutions", "./test_data/test_global", num_files=10, dup_ratio=dup_ratio, data_type=data_type)
