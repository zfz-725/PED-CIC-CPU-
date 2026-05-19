#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pri {

enum class EncryptMode {
    kSecureHash = 0,
    kOprf = 1
};

struct DocLocation {
    std::string institution;
    std::string source_file;
    int line_index = 0;
};

struct DocRecord {
    int64_t uid = -1;
    DocLocation location;
    std::vector<uint32_t> signature;
    std::vector<uint32_t> bucket_ids;
};

struct PipelineConfig {
    std::string institution_root;
    std::string lsh_result_root;
    std::string output_root;
    int num_hash = 128;
    int bands = 16;
    double similarity_threshold = 0.8;
    EncryptMode mode = EncryptMode::kSecureHash;
    uint64_t secure_hash_key = 0x9e3779b97f4a7c15ULL;
    uint64_t oprf_key = 0xd1b54a32d192ed03ULL;
};

struct PipelineStats {
    int64_t total_docs = 0;
    int64_t total_buckets = 0;
    int64_t shared_bucket_count = 0;
    int64_t requested_docs = 0;
    int64_t duplicate_pairs = 0;
    int64_t delete_docs = 0;
    double phase_bucket_submit_s = 0.0;
    double phase_request_encrypt_s = 0.0;
    double phase_global_match_s = 0.0;
    double phase_distribution_s = 0.0;
    double total_s = 0.0;
};

}
