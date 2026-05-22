import os
import random
import time
from copy import deepcopy

import numpy as np
import torch
from torch.utils.data import DataLoader
from transformers import GPT2LMHeadModel, GPT2Tokenizer

from config import (
    BATCH_SIZE,
    CACHE_PATH,
    CLIENTS,
    DUPLICATE_RATE,
    EPOCHS,
    LOCAL_DEDUP,
    MODEL_CACHE,
    MODEL_NAME,
    MODEL_PATH,
    PAD_TOKEN,
    PED_CIC_BIN,
    PRI_BANDS,
    PRI_CPU_BIN,
    PRI_MODE,
    PRI_NUM_HASH,
    PRI_THRESHOLD,
    PRI_WORK_DIR,
    ROUNDS,
    SEED,
    TEST_RATIO,
    TOY_DATASET_SIZE,
    USE_PRI_DEDUP,
)
from utils import (
    TextDataset,
    aggregate_state_dicts,
    build_toy_text_dataset,
    compute_perplexity,
    inject_duplicates,
    local_deduplicate,
    make_loaders,
    pri_cross_client_deduplicate,
    split_clients,
    split_train_test,
    train_client,
)


def main():
    torch.manual_seed(SEED)
    np.random.seed(SEED)
    random.seed(SEED)
    os.environ["HF_HOME"] = CACHE_PATH
    os.makedirs(MODEL_PATH, exist_ok=True)
    os.makedirs(MODEL_CACHE, exist_ok=True)

    device = "cuda" if torch.cuda.is_available() else "cpu"
    tokenizer = GPT2Tokenizer.from_pretrained(MODEL_NAME, pad_token=PAD_TOKEN)
    model = GPT2LMHeadModel.from_pretrained(MODEL_NAME)
    model.resize_token_embeddings(len(tokenizer))
    model = model.to(device)

    global_ckpt = os.path.join(MODEL_CACHE, "global.pt")
    torch.save(model.state_dict(), global_ckpt)

    full_data = build_toy_text_dataset(total_samples=TOY_DATASET_SIZE, seed=SEED)
    full_data = list(set(full_data))
    train_data, test_data = split_train_test(full_data, test_ratio=TEST_RATIO, seed=SEED)
    train_data = inject_duplicates(train_data, duplicate_rate=DUPLICATE_RATE, seed=SEED)
    client_data = split_clients(train_data, clients=CLIENTS, seed=SEED)
    if USE_PRI_DEDUP:
        before = sum(len(x) for x in client_data)
        client_data = pri_cross_client_deduplicate(
            client_data=client_data,
            work_dir=PRI_WORK_DIR,
            ped_cic_bin=PED_CIC_BIN,
            pri_cpu_bin=PRI_CPU_BIN,
            mode=PRI_MODE,
            num_hash=PRI_NUM_HASH,
            bands=PRI_BANDS,
            threshold=PRI_THRESHOLD,
        )
        after = sum(len(x) for x in client_data)
        print(f"pri_dedup target=cpu mode={PRI_MODE} before={before} after={after} removed={before-after}")
    if LOCAL_DEDUP:
        client_data = local_deduplicate(client_data)
    client_loaders = make_loaders(client_data, tokenizer=tokenizer, batch_size=BATCH_SIZE)

    best_loss = float("inf")
    best_state = deepcopy(model.state_dict())
    best_round = -1

    for round_idx in range(ROUNDS):
        client_losses = []
        client_states = []
        for client_idx in range(CLIENTS):
            client_model = GPT2LMHeadModel.from_pretrained(MODEL_NAME)
            client_model.resize_token_embeddings(len(tokenizer))
            client_model.load_state_dict(torch.load(global_ckpt, map_location="cpu"))
            start = time.time()
            loss = train_client(
                model=client_model,
                loader=client_loaders[client_idx],
                epochs=EPOCHS,
                rounds=ROUNDS,
                round_idx=round_idx,
                device=device,
            )
            end = time.time()
            print(f"client={client_idx} round={round_idx} loss={loss:.6f} time_s={end-start:.3f}")
            client_losses.append(loss)
            client_states.append(client_model.state_dict())

        avg_loss = sum(client_losses) / max(1, len(client_losses))
        print(f"round={round_idx} avg_client_loss={avg_loss:.6f}")
        agg_state = aggregate_state_dicts(client_states)
        model.load_state_dict(agg_state)
        torch.save(agg_state, global_ckpt)
        if avg_loss < best_loss:
            best_loss = avg_loss
            best_state = deepcopy(agg_state)
            best_round = round_idx

    model.load_state_dict(best_state)
    final_ckpt = os.path.join(MODEL_PATH, f"{MODEL_NAME.replace('/', '_')}_best_round_{best_round}.pt")
    torch.save(model.state_dict(), final_ckpt)

    test_set = TextDataset(test_data, tokenizer)
    test_loader = DataLoader(test_set, batch_size=1, shuffle=False)
    ppl = compute_perplexity(model, test_loader, device=device)
    print(f"best_round={best_round} best_loss={best_loss:.6f} test_perplexity={ppl:.6f}")
    print(f"saved_model={final_ckpt}")


if __name__ == "__main__":
    main()
