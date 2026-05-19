import os


def _env(name: str, default: str) -> str:
    value = os.environ.get(name)
    return default if value is None else value


MODEL_NAME = _env("FL_MODEL_NAME", "sshleifer/tiny-gpt2")
BATCH_SIZE = int(_env("FL_BATCH_SIZE", "4"))
LEARNING_RATE = float(_env("FL_LEARNING_RATE", "5e-5"))
DUPLICATE_RATE = float(_env("FL_DUPLICATE_RATE", "0.3"))
MAX_SEQ_LEN = int(_env("FL_MAX_SEQ_LEN", "128"))
EPOCHS = int(_env("FL_EPOCHS", "1"))
CLIENTS = int(_env("FL_CLIENTS", "10"))
ROUNDS = int(_env("FL_ROUNDS", "2"))
EOS_TOKEN = _env("FL_EOS_TOKEN", "<|endoftext|>")
BOS_TOKEN = _env("FL_BOS_TOKEN", "<|startoftext|>")
PAD_TOKEN = _env("FL_PAD_TOKEN", "<|pad|>")
TEST_RATIO = float(_env("FL_TEST_RATIO", "0.2"))
SEED = int(_env("FL_SEED", "123"))
LOCAL_DEDUP = _env("FL_LOCAL_DEDUP", "0") == "1"
CACHE_PATH = _env("FL_CACHE_PATH", "./.cache/huggingface")
MODEL_PATH = _env("FL_MODEL_PATH", "./FL/trained_models")
MODEL_CACHE = _env("FL_MODEL_CACHE", "./FL/models_cache")
TOY_DATASET_SIZE = int(_env("FL_TOY_DATASET_SIZE", "2000"))
USE_PRI_DEDUP = _env("FL_USE_PRI_DEDUP", "0") == "1"
PRI_MODE = _env("FL_PRI_MODE", "sha")
PRI_NUM_HASH = int(_env("FL_PRI_NUM_HASH", "128"))
PRI_BANDS = int(_env("FL_PRI_BANDS", "16"))
PRI_THRESHOLD = float(_env("FL_PRI_THRESHOLD", "0.8"))
PRI_WORK_DIR = _env("FL_PRI_WORK_DIR", "./FL/pri_workspace")
FED_BIN = _env("FL_FED_BIN", "./build_cpu/fed_lsh_cpu")
PRI_CPU_BIN = _env("FL_PRI_CPU_BIN", "./src/pri_cpu/build_cpu/pri_cpu_main")
