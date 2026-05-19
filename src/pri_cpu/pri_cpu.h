#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "pri_types.h"

namespace pri {

bool EncryptSignaturesCPU(
    const std::vector<uint32_t>& signatures,
    int num_hash,
    EncryptMode mode,
    uint64_t secure_hash_key,
    uint64_t oprf_key,
    uint64_t bucket_nonce,
    std::vector<uint32_t>& encrypted_signatures,
    std::string& error_message);

bool FindSimilarPairsCPU(
    const std::vector<uint32_t>& encrypted_signatures,
    int docs_in_bucket,
    int num_hash,
    int threshold_count,
    std::vector<std::pair<int, int>>& similar_pairs,
    std::string& error_message);

}
