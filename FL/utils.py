import random
import json
import shutil
import subprocess
from copy import deepcopy
from pathlib import Path
from typing import Iterable, List, Sequence, Tuple

import numpy as np
import torch
from torch.nn.utils import clip_grad_norm_
from torch.optim import AdamW
from torch.utils.data import DataLoader, Dataset
from transformers import get_linear_schedule_with_warmup

from config import BOS_TOKEN, EOS_TOKEN, LEARNING_RATE, MAX_SEQ_LEN


class TextDataset(Dataset):
    def __init__(self, data: Sequence[str], tokenizer):
        self.input_ids = []
        self.attn_masks = []
        for text in data:
            encoded = tokenizer(
                BOS_TOKEN + text + EOS_TOKEN,
                truncation=True,
                max_length=MAX_SEQ_LEN,
                padding="max_length",
            )
            self.input_ids.append(torch.tensor(encoded["input_ids"], dtype=torch.long))
            self.attn_masks.append(torch.tensor(encoded["attention_mask"], dtype=torch.long))

    def __len__(self):
        return len(self.input_ids)

    def __getitem__(self, idx):
        return self.input_ids[idx], self.attn_masks[idx]


def build_toy_text_dataset(total_samples: int, seed: int) -> List[str]:
    rng = random.Random(seed)
    samples = []
    for i in range(total_samples):
        topic = rng.choice(["vision", "nlp", "speech", "robotics", "systems"])
        adjective = rng.choice(["robust", "efficient", "scalable", "federated", "private"])
        noun = rng.choice(["training", "aggregation", "tokenization", "optimization", "evaluation"])
        samples.append(f"sample {i} about {topic} with {adjective} {noun}")
    return samples


def split_train_test(data: Sequence[str], test_ratio: float, seed: int) -> Tuple[List[str], List[str]]:
    idx = np.arange(len(data))
    rng = np.random.default_rng(seed)
    rng.shuffle(idx)
    split = int((1.0 - test_ratio) * len(idx))
    train_idx = idx[:split]
    test_idx = idx[split:]
    train = [data[i] for i in train_idx.tolist()]
    test = [data[i] for i in test_idx.tolist()]
    return train, test


def inject_duplicates(data: Sequence[str], duplicate_rate: float, seed: int) -> List[str]:
    if duplicate_rate <= 0:
        return list(data)
    rng = np.random.default_rng(seed)
    n = len(data)
    k = int(duplicate_rate * n)
    if k <= 0:
        return list(data)
    dup_idx = rng.choice(n, size=k, replace=True).tolist()
    out = list(data)
    out.extend([data[i] for i in dup_idx])
    return out


def split_clients(data: Sequence[str], clients: int, seed: int) -> List[List[str]]:
    idx = np.arange(len(data))
    rng = np.random.default_rng(seed)
    rng.shuffle(idx)
    chunks = np.array_split(idx, clients)
    out: List[List[str]] = []
    for c in chunks:
        out.append([data[i] for i in c.tolist()])
    return out


def local_deduplicate(client_data: List[List[str]]) -> List[List[str]]:
    deduped = []
    for data in client_data:
        deduped.append(list(set(data)))
    return deduped


def make_loaders(client_data: List[List[str]], tokenizer, batch_size: int) -> List[DataLoader]:
    loaders = []
    for data in client_data:
        ds = TextDataset(data, tokenizer)
        loaders.append(DataLoader(ds, batch_size=batch_size, shuffle=True))
    return loaders


def train_client(model, loader: DataLoader, epochs: int, rounds: int, round_idx: int, device: str) -> float:
    model.train()
    model.to(device)
    opt = AdamW(model.parameters(), lr=LEARNING_RATE)
    total_steps = max(1, rounds * epochs * max(1, len(loader)))
    warmup_steps = int(total_steps * 0.1)
    scheduler = get_linear_schedule_with_warmup(
        opt,
        num_warmup_steps=warmup_steps,
        num_training_steps=total_steps,
    )
    skip_steps = max(0, round_idx) * epochs * max(1, len(loader))
    for _ in range(skip_steps):
        scheduler.step()

    avg_loss = 0.0
    for _ in range(epochs):
        epoch_loss = 0.0
        for input_ids, masks in loader:
            input_ids = input_ids.to(device)
            masks = masks.to(device)
            model.zero_grad()
            outputs = model(input_ids=input_ids, attention_mask=masks, labels=input_ids)
            loss = outputs[0]
            epoch_loss += loss.item()
            loss.backward()
            clip_grad_norm_(model.parameters(), 1.0)
            opt.step()
            scheduler.step()
        denom = max(1, len(loader))
        avg_loss += epoch_loss / denom
    return avg_loss / max(1, epochs)


def aggregate_state_dicts(states: Iterable[dict]) -> dict:
    states = list(states)
    if not states:
        raise ValueError("No client states to aggregate")
    avg = deepcopy(states[0])
    for key in avg.keys():
        for i in range(1, len(states)):
            avg[key] = avg[key] + states[i][key]
        avg[key] = avg[key] / len(states)
    return avg


@torch.no_grad()
def compute_perplexity(model, loader: DataLoader, device: str) -> float:
    model.eval()
    model.to(device)
    total_loss = 0.0
    steps = 0
    for input_ids, masks in loader:
        input_ids = input_ids.to(device)
        masks = masks.to(device)
        outputs = model(input_ids=input_ids, attention_mask=masks, labels=input_ids)
        total_loss += outputs[0].item()
        steps += 1
    mean_loss = total_loss / max(1, steps)
    return torch.exp(torch.tensor(mean_loss)).item()


def pri_cross_client_deduplicate(
    client_data: List[List[str]],
    work_dir: str,
    fed_bin: str,
    pri_cpu_bin: str,
    mode: str,
    num_hash: int,
    bands: int,
    threshold: float,
) -> List[List[str]]:
    mode = mode.lower()
    if mode not in ("sha", "oprf"):
        raise ValueError(f"mode 仅支持 sha/oprf，当前: {mode}")

    pri_bin = pri_cpu_bin
    if not Path(fed_bin).exists():
        raise FileNotFoundError(f"未找到 FED CPU 可执行文件: {fed_bin}")
    if not Path(pri_bin).exists():
        raise FileNotFoundError(f"未找到 PRI CPU 可执行文件: {pri_bin}")

    root = Path(work_dir)
    inst_root = root / "institutions"
    lsh_root = root / "lsh_results"
    out_root = root / f"pri_cpu_{mode}"
    if root.exists():
        shutil.rmtree(root)
    inst_root.mkdir(parents=True, exist_ok=True)
    lsh_root.mkdir(parents=True, exist_ok=True)
    out_root.mkdir(parents=True, exist_ok=True)

    for i, docs in enumerate(client_data):
        inst_dir = inst_root / f"inst_{i}"
        inst_dir.mkdir(parents=True, exist_ok=True)
        data_file = inst_dir / "test_0.jsonl"
        with data_file.open("w", encoding="utf-8") as f:
            for text in docs:
                f.write(json.dumps({"text": text}, ensure_ascii=False) + "\n")

    for i in range(len(client_data)):
        inst_name = f"inst_{i}"
        src_dir = inst_root / inst_name
        dst_dir = lsh_root / inst_name
        dst_dir.mkdir(parents=True, exist_ok=True)
        subprocess.run(
            [fed_bin, str(src_dir), str(dst_dir), "--keep-hash"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )

    subprocess.run(
        [
            pri_bin,
            "--inst-root",
            str(inst_root),
            "--lsh-root",
            str(lsh_root),
            "--out",
            str(out_root),
            "--mode",
            mode,
            "--num-hash",
            str(num_hash),
            "--bands",
            str(bands),
            "--threshold",
            str(threshold),
        ],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    delete_map = {i: set() for i in range(len(client_data))}
    for i in range(len(client_data)):
        p = out_root / f"inst_{i}_delete_ids.txt"
        if not p.exists():
            continue
        with p.open("r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                parts = line.split(",")
                if len(parts) != 3:
                    continue
                line_idx = int(parts[2])
                delete_map[i].add(line_idx)

    deduped: List[List[str]] = []
    for i, docs in enumerate(client_data):
        deleted = delete_map[i]
        deduped.append([doc for idx, doc in enumerate(docs) if idx not in deleted])
    return deduped
