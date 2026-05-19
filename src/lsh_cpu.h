#pragma once

#include <filesystem>

namespace fed_cpu {

struct LshConfig {
    int num_hash = 128;
    int bands = 16;
    int shingle_len = 5;
    int num_key = 10240;
    double threshold = 0.8;
    int min_text_len = 0;
};

struct LshStats {
    int files = 0;
    int docs = 0;
    int local_duplicate_pairs = 0;
    int local_delete_docs = 0;
    double minhash_s = 0.0;
    double local_dedup_s = 0.0;
    double total_s = 0.0;
};

LshStats RunDirectory(const std::filesystem::path& input_dir,
                      const std::filesystem::path& output_dir,
                      const LshConfig& cfg);

}
