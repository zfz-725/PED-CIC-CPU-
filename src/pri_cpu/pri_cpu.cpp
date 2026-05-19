#include "pri_cpu.h"

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/sha.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace pri {

namespace {

inline void StoreU32BE(uint32_t value, uint8_t* out) {
    out[0] = static_cast<uint8_t>((value >> 24U) & 0xffU);
    out[1] = static_cast<uint8_t>((value >> 16U) & 0xffU);
    out[2] = static_cast<uint8_t>((value >> 8U) & 0xffU);
    out[3] = static_cast<uint8_t>(value & 0xffU);
}

inline void StoreU64BE(uint64_t value, uint8_t* out) {
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>((value >> (56U - static_cast<uint32_t>(i) * 8U)) & 0xffU);
    }
}

inline uint32_t LoadU32BE(const uint8_t* in) {
    return (static_cast<uint32_t>(in[0]) << 24U) |
           (static_cast<uint32_t>(in[1]) << 16U) |
           (static_cast<uint32_t>(in[2]) << 8U) |
           static_cast<uint32_t>(in[3]);
}

constexpr int kSha256Words = SHA256_DIGEST_LENGTH / static_cast<int>(sizeof(uint32_t));

void Sha256FeatureTransform(
    const std::vector<uint32_t>& signatures,
    int num_hash,
    uint64_t secure_hash_key,
    std::vector<uint32_t>& encrypted_signatures) {
    encrypted_signatures.assign(signatures.size() * static_cast<size_t>(kSha256Words), 0U);
    std::array<uint8_t, 24> input{};
    StoreU32BE(0x4645445fU, input.data());      // "FED_"
    StoreU32BE(0x50524931U, input.data() + 4);  // "PRI1"
    StoreU64BE(secure_hash_key, input.data() + 8);

    for (size_t i = 0; i < signatures.size(); ++i) {
        StoreU32BE(static_cast<uint32_t>(i % static_cast<size_t>(num_hash)), input.data() + 16);
        StoreU32BE(signatures[i], input.data() + 20);

        uint8_t digest[SHA256_DIGEST_LENGTH];
        SHA256(input.data(), input.size(), digest);
        const size_t out_base = i * static_cast<size_t>(kSha256Words);
        for (int word = 0; word < kSha256Words; ++word) {
            encrypted_signatures[out_base + static_cast<size_t>(word)] =
                LoadU32BE(digest + static_cast<size_t>(word) * sizeof(uint32_t));
        }
    }
}

bool DeriveNonZeroScalar(
    const uint8_t* data,
    size_t len,
    const BIGNUM* order,
    BN_CTX* ctx,
    BIGNUM* out_scalar) {
    uint8_t digest[SHA256_DIGEST_LENGTH];
    SHA256(data, len, digest);
    BIGNUM* tmp = BN_bin2bn(digest, SHA256_DIGEST_LENGTH, nullptr);
    if (tmp == nullptr) {
        return false;
    }
    const int ok = BN_mod(out_scalar, tmp, order, ctx);
    BN_free(tmp);
    if (ok != 1) {
        return false;
    }
    if (BN_is_zero(out_scalar)) {
        return BN_one(out_scalar) == 1;
    }
    return true;
}

bool EcOprfTransform(
    const std::vector<uint32_t>& signatures,
    uint64_t oprf_key,
    uint64_t bucket_nonce,
    std::vector<uint32_t>& encrypted_signatures,
    std::string& error_message) {
    encrypted_signatures.assign(signatures.size(), 0U);
    if (signatures.empty()) {
        return true;
    }

    EC_GROUP* group = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
    if (group == nullptr) {
        error_message = "EC_GROUP_new_by_curve_name 失败";
        return false;
    }
    BN_CTX* ctx = BN_CTX_new();
    BIGNUM* order = BN_new();
    BIGNUM* server_scalar = BN_new();
    BIGNUM* h_scalar = BN_new();
    BIGNUM* blind_scalar = BN_new();
    if (ctx == nullptr || order == nullptr || server_scalar == nullptr || h_scalar == nullptr || blind_scalar == nullptr) {
        error_message = "OpenSSL BN 初始化失败";
        if (blind_scalar) BN_free(blind_scalar);
        if (h_scalar) BN_free(h_scalar);
        if (server_scalar) BN_free(server_scalar);
        if (order) BN_free(order);
        if (ctx) BN_CTX_free(ctx);
        EC_GROUP_free(group);
        return false;
    }
    if (EC_GROUP_get_order(group, order, ctx) != 1) {
        error_message = "EC_GROUP_get_order 失败";
        BN_free(blind_scalar);
        BN_free(h_scalar);
        BN_free(server_scalar);
        BN_free(order);
        BN_CTX_free(ctx);
        EC_GROUP_free(group);
        return false;
    }

    std::array<uint8_t, 16> key_material{};
    StoreU64BE(oprf_key, key_material.data());
    StoreU64BE(oprf_key ^ 0x9e3779b97f4a7c15ULL, key_material.data() + 8);
    if (!DeriveNonZeroScalar(key_material.data(), key_material.size(), order, ctx, server_scalar)) {
        error_message = "派生 OPRF 服务端标量失败";
        BN_free(blind_scalar);
        BN_free(h_scalar);
        BN_free(server_scalar);
        BN_free(order);
        BN_CTX_free(ctx);
        EC_GROUP_free(group);
        return false;
    }

    EC_POINT* point_h = EC_POINT_new(group);
    EC_POINT* point_blind = EC_POINT_new(group);
    EC_POINT* point_eval = EC_POINT_new(group);
    EC_POINT* point_unblind = EC_POINT_new(group);
    BIGNUM* blind_inv = BN_new();
    if (point_h == nullptr || point_blind == nullptr || point_eval == nullptr || point_unblind == nullptr || blind_inv == nullptr) {
        error_message = "OpenSSL EC_POINT 初始化失败";
        if (blind_inv) BN_free(blind_inv);
        if (point_unblind) EC_POINT_free(point_unblind);
        if (point_eval) EC_POINT_free(point_eval);
        if (point_blind) EC_POINT_free(point_blind);
        if (point_h) EC_POINT_free(point_h);
        BN_free(blind_scalar);
        BN_free(h_scalar);
        BN_free(server_scalar);
        BN_free(order);
        BN_CTX_free(ctx);
        EC_GROUP_free(group);
        return false;
    }

    std::unordered_map<uint32_t, uint32_t> output_cache;
    output_cache.reserve(signatures.size());
    std::vector<uint8_t> point_buf(65);

    for (size_t i = 0; i < signatures.size(); ++i) {
        const uint32_t value = signatures[i];
        const auto cached_it = output_cache.find(value);
        if (cached_it != output_cache.end()) {
            encrypted_signatures[i] = cached_it->second;
            continue;
        }

        std::array<uint8_t, 4> value_input{};
        StoreU32BE(value, value_input.data());
        if (!DeriveNonZeroScalar(value_input.data(), value_input.size(), order, ctx, h_scalar)) {
            error_message = "派生输入点标量失败";
            BN_free(blind_inv);
            EC_POINT_free(point_unblind);
            EC_POINT_free(point_eval);
            EC_POINT_free(point_blind);
            EC_POINT_free(point_h);
            BN_free(blind_scalar);
            BN_free(h_scalar);
            BN_free(server_scalar);
            BN_free(order);
            BN_CTX_free(ctx);
            EC_GROUP_free(group);
            return false;
        }
        if (EC_POINT_mul(group, point_h, h_scalar, nullptr, nullptr, ctx) != 1) {
            error_message = "EC_POINT_mul(point_h) 失败";
            BN_free(blind_inv);
            EC_POINT_free(point_unblind);
            EC_POINT_free(point_eval);
            EC_POINT_free(point_blind);
            EC_POINT_free(point_h);
            BN_free(blind_scalar);
            BN_free(h_scalar);
            BN_free(server_scalar);
            BN_free(order);
            BN_CTX_free(ctx);
            EC_GROUP_free(group);
            return false;
        }

        std::array<uint8_t, 20> blind_input{};
        StoreU64BE(bucket_nonce, blind_input.data());
        StoreU32BE(static_cast<uint32_t>(i), blind_input.data() + 8);
        StoreU32BE(value, blind_input.data() + 12);
        StoreU32BE(static_cast<uint32_t>(oprf_key & 0xffffffffU), blind_input.data() + 16);
        if (!DeriveNonZeroScalar(blind_input.data(), blind_input.size(), order, ctx, blind_scalar)) {
            error_message = "派生盲化标量失败";
            BN_free(blind_inv);
            EC_POINT_free(point_unblind);
            EC_POINT_free(point_eval);
            EC_POINT_free(point_blind);
            EC_POINT_free(point_h);
            BN_free(blind_scalar);
            BN_free(h_scalar);
            BN_free(server_scalar);
            BN_free(order);
            BN_CTX_free(ctx);
            EC_GROUP_free(group);
            return false;
        }
        if (EC_POINT_mul(group, point_blind, nullptr, point_h, blind_scalar, ctx) != 1) {
            error_message = "EC_POINT_mul(point_blind) 失败";
            BN_free(blind_inv);
            EC_POINT_free(point_unblind);
            EC_POINT_free(point_eval);
            EC_POINT_free(point_blind);
            EC_POINT_free(point_h);
            BN_free(blind_scalar);
            BN_free(h_scalar);
            BN_free(server_scalar);
            BN_free(order);
            BN_CTX_free(ctx);
            EC_GROUP_free(group);
            return false;
        }
        if (EC_POINT_mul(group, point_eval, nullptr, point_blind, server_scalar, ctx) != 1) {
            error_message = "EC_POINT_mul(point_eval) 失败";
            BN_free(blind_inv);
            EC_POINT_free(point_unblind);
            EC_POINT_free(point_eval);
            EC_POINT_free(point_blind);
            EC_POINT_free(point_h);
            BN_free(blind_scalar);
            BN_free(h_scalar);
            BN_free(server_scalar);
            BN_free(order);
            BN_CTX_free(ctx);
            EC_GROUP_free(group);
            return false;
        }
        if (BN_mod_inverse(blind_inv, blind_scalar, order, ctx) == nullptr) {
            error_message = "BN_mod_inverse 失败";
            BN_free(blind_inv);
            EC_POINT_free(point_unblind);
            EC_POINT_free(point_eval);
            EC_POINT_free(point_blind);
            EC_POINT_free(point_h);
            BN_free(blind_scalar);
            BN_free(h_scalar);
            BN_free(server_scalar);
            BN_free(order);
            BN_CTX_free(ctx);
            EC_GROUP_free(group);
            return false;
        }
        if (EC_POINT_mul(group, point_unblind, nullptr, point_eval, blind_inv, ctx) != 1) {
            error_message = "EC_POINT_mul(point_unblind) 失败";
            BN_free(blind_inv);
            EC_POINT_free(point_unblind);
            EC_POINT_free(point_eval);
            EC_POINT_free(point_blind);
            EC_POINT_free(point_h);
            BN_free(blind_scalar);
            BN_free(h_scalar);
            BN_free(server_scalar);
            BN_free(order);
            BN_CTX_free(ctx);
            EC_GROUP_free(group);
            return false;
        }

        const size_t oct_len = EC_POINT_point2oct(
            group,
            point_unblind,
            POINT_CONVERSION_COMPRESSED,
            point_buf.data(),
            point_buf.size(),
            ctx);
        if (oct_len == 0U) {
            error_message = "EC_POINT_point2oct 失败";
            BN_free(blind_inv);
            EC_POINT_free(point_unblind);
            EC_POINT_free(point_eval);
            EC_POINT_free(point_blind);
            EC_POINT_free(point_h);
            BN_free(blind_scalar);
            BN_free(h_scalar);
            BN_free(server_scalar);
            BN_free(order);
            BN_CTX_free(ctx);
            EC_GROUP_free(group);
            return false;
        }

        uint8_t digest[SHA256_DIGEST_LENGTH];
        SHA256(point_buf.data(), oct_len, digest);
        const uint32_t out_value = LoadU32BE(digest);
        encrypted_signatures[i] = out_value;
        output_cache.emplace(value, out_value);
    }

    BN_free(blind_inv);
    EC_POINT_free(point_unblind);
    EC_POINT_free(point_eval);
    EC_POINT_free(point_blind);
    EC_POINT_free(point_h);
    BN_free(blind_scalar);
    BN_free(h_scalar);
    BN_free(server_scalar);
    BN_free(order);
    BN_CTX_free(ctx);
    EC_GROUP_free(group);
    return true;
}

}

bool EncryptSignaturesCPU(
    const std::vector<uint32_t>& signatures,
    int num_hash,
    EncryptMode mode,
    uint64_t secure_hash_key,
    uint64_t oprf_key,
    uint64_t bucket_nonce,
    std::vector<uint32_t>& encrypted_signatures,
    std::string& error_message) {
    if (num_hash <= 0) {
        error_message = "num_hash 必须大于 0";
        return false;
    }
    if (signatures.empty() || signatures.size() % static_cast<size_t>(num_hash) != 0U) {
        error_message = "签名数组长度与 num_hash 不匹配";
        return false;
    }

    if (mode == EncryptMode::kOprf) {
        return EcOprfTransform(signatures, oprf_key, bucket_nonce, encrypted_signatures, error_message);
    }

    Sha256FeatureTransform(signatures, num_hash, secure_hash_key, encrypted_signatures);
    return true;
}

bool FindSimilarPairsCPU(
    const std::vector<uint32_t>& encrypted_signatures,
    int docs_in_bucket,
    int num_hash,
    int threshold_count,
    std::vector<std::pair<int, int>>& similar_pairs,
    std::string& error_message) {
    similar_pairs.clear();
    if (docs_in_bucket <= 1) {
        return true;
    }
    if (num_hash <= 0 || threshold_count <= 0) {
        error_message = "num_hash 与 threshold_count 必须大于 0";
        return false;
    }
    const size_t component_count = static_cast<size_t>(docs_in_bucket) * static_cast<size_t>(num_hash);
    if (component_count == 0U || encrypted_signatures.size() % component_count != 0U) {
        error_message = "输入签名长度与 docs_in_bucket*num_hash 不匹配";
        return false;
    }
    const int words_per_hash = static_cast<int>(encrypted_signatures.size() / component_count);
    if (words_per_hash <= 0) {
        error_message = "加密特征宽度非法";
        return false;
    }
    const size_t doc_stride = static_cast<size_t>(num_hash) * static_cast<size_t>(words_per_hash);

    for (int i = 0; i < docs_in_bucket; ++i) {
        const size_t base_i = static_cast<size_t>(i) * doc_stride;
        for (int j = i + 1; j < docs_in_bucket; ++j) {
            const size_t base_j = static_cast<size_t>(j) * doc_stride;
            int equal_cnt = 0;
            for (int k = 0; k < num_hash; ++k) {
                bool same = true;
                const size_t left = base_i + static_cast<size_t>(k) * static_cast<size_t>(words_per_hash);
                const size_t right = base_j + static_cast<size_t>(k) * static_cast<size_t>(words_per_hash);
                for (int word = 0; word < words_per_hash; ++word) {
                    if (encrypted_signatures[left + static_cast<size_t>(word)] !=
                        encrypted_signatures[right + static_cast<size_t>(word)]) {
                        same = false;
                        break;
                    }
                }
                equal_cnt += same ? 1 : 0;
            }
            if (equal_cnt >= threshold_count) {
                similar_pairs.emplace_back(i, j);
            }
        }
    }
    return true;
}

}
