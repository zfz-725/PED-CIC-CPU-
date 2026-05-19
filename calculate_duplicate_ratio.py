#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import json
import os

unique_values = set()
total_lines = 0

# 读取所有全局文件
global_dir = './test_data/test_global'
for filename in os.listdir(global_dir):
    if filename.endswith('.jsonl'):
        file_path = os.path.join(global_dir, filename)
        with open(file_path, 'r') as f:
            for line in f:
                total_lines += 1
                data = json.loads(line)
                unique_values.add(data['text'])

unique_count = len(unique_values)
duplicate_count = total_lines - unique_count
duplicate_ratio = duplicate_count / total_lines if total_lines > 0 else 0

print('总行数: {}'.format(total_lines))
print('唯一值数量: {}'.format(unique_count))
print('重复值数量: {}'.format(duplicate_count))
print('重复率: {:.2f}'.format(duplicate_ratio))
