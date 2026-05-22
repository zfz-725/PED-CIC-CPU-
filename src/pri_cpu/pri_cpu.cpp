#include "pri_cpu.h"

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>
#include <openssl/sha.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
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
constexpr uint64_t kOprfClientSeed = 0x8f3f73b5d9a4e1c7ULL;

void Sha256FeatureTransform(
    const std::vector<uint32_t>& signatures,
    int num_hash,
    uint64_t secure_hash_key,
    std::vector<uint32_t>& encrypted_signatures) {
    encrypted_signatures.assign(signatures.size() * static_cast<size_t>(kSha256Words), 0U);
    std::array<uint8_t, 24> input{};
    StoreU32BE(0x5045445fU, input.data());      // "PED_"
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

class EcContext {
public:
    explicit EcContext(std::string& error_message) {
        group_ = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
        ctx_ = BN_CTX_new();
        order_ = BN_new();
        if (group_ == nullptr || ctx_ == nullptr || order_ == nullptr) {
            error_message = "OpenSSL EC/BN 初始化失败";
            return;
        }
        if (EC_GROUP_get_order(group_, order_, ctx_) != 1) {
            error_message = "EC_GROUP_get_order 失败";
            return;
        }
        ok_ = true;
    }

    ~EcContext() {
        if (order_) BN_free(order_);
        if (ctx_) BN_CTX_free(ctx_);
        if (group_) EC_GROUP_free(group_);
    }

    EcContext(const EcContext&) = delete;
    EcContext& operator=(const EcContext&) = delete;

    bool ok() const { return ok_; }
    const EC_GROUP* group() const { return group_; }
    BN_CTX* ctx() const { return ctx_; }
    const BIGNUM* order() const { return order_; }

private:
    EC_GROUP* group_ = nullptr;
    BN_CTX* ctx_ = nullptr;
    BIGNUM* order_ = nullptr;
    bool ok_ = false;
};

bool SerializePoint(EcContext& ec, const EC_POINT* point, std::vector<uint8_t>& out, std::string& error_message) {
    out.resize(EC_POINT_point2oct(ec.group(), point, POINT_CONVERSION_COMPRESSED, nullptr, 0, ec.ctx()));
    if (out.empty()) {
        error_message = "EC_POINT_point2oct 长度计算失败";
        return false;
    }
    const size_t oct_len = EC_POINT_point2oct(
        ec.group(), point, POINT_CONVERSION_COMPRESSED, out.data(), out.size(), ec.ctx());
    if (oct_len == 0U) {
        error_message = "EC_POINT_point2oct 失败";
        return false;
    }
    out.resize(oct_len);
    return true;
}

bool DeserializePoint(EcContext& ec, const std::vector<uint8_t>& in, EC_POINT* point, std::string& error_message) {
    if (in.empty()) {
        error_message = "OPRF 点序列化为空";
        return false;
    }
    if (EC_POINT_oct2point(ec.group(), point, in.data(), in.size(), ec.ctx()) != 1) {
        error_message = "EC_POINT_oct2point 失败";
        return false;
    }
    return true;
}

class OprfKServer {
public:
    OprfKServer(EcContext& ec, uint64_t oprf_key, std::string& error_message) : ec_(ec) {
        server_scalar_ = BN_new();
        if (server_scalar_ == nullptr) {
            error_message = "OPRF KServer 标量初始化失败";
            return;
        }
        std::array<uint8_t, 16> key_material{};
        StoreU64BE(oprf_key, key_material.data());
        StoreU64BE(oprf_key ^ 0x9e3779b97f4a7c15ULL, key_material.data() + 8);
        ok_ = DeriveNonZeroScalar(key_material.data(), key_material.size(), ec_.order(), ec_.ctx(), server_scalar_);
        if (!ok_) {
            error_message = "派生 OPRF KServer 标量失败";
        }
    }

    ~OprfKServer() {
        if (server_scalar_) BN_free(server_scalar_);
    }

    OprfKServer(const OprfKServer&) = delete;
    OprfKServer& operator=(const OprfKServer&) = delete;

    bool ok() const { return ok_; }

    bool Evaluate(
        const std::vector<std::vector<uint8_t>>& blinded_request,
        std::vector<std::vector<uint8_t>>& evaluated_response,
        std::string& error_message) const {
        evaluated_response.clear();
        evaluated_response.reserve(blinded_request.size());
        EC_POINT* point_blind = EC_POINT_new(ec_.group());
        EC_POINT* point_eval = EC_POINT_new(ec_.group());
        if (point_blind == nullptr || point_eval == nullptr) {
            error_message = "OPRF KServer EC_POINT 初始化失败";
            if (point_eval) EC_POINT_free(point_eval);
            if (point_blind) EC_POINT_free(point_blind);
            return false;
        }

        for (const auto& blinded : blinded_request) {
            if (!DeserializePoint(ec_, blinded, point_blind, error_message)) {
                EC_POINT_free(point_eval);
                EC_POINT_free(point_blind);
                return false;
            }
            if (EC_POINT_mul(ec_.group(), point_eval, nullptr, point_blind, server_scalar_, ec_.ctx()) != 1) {
                error_message = "EC_POINT_mul(point_eval) 失败";
                EC_POINT_free(point_eval);
                EC_POINT_free(point_blind);
                return false;
            }
            std::vector<uint8_t> serialized;
            if (!SerializePoint(ec_, point_eval, serialized, error_message)) {
                EC_POINT_free(point_eval);
                EC_POINT_free(point_blind);
                return false;
            }
            evaluated_response.push_back(std::move(serialized));
        }

        EC_POINT_free(point_eval);
        EC_POINT_free(point_blind);
        return true;
    }

private:
    EcContext& ec_;
    BIGNUM* server_scalar_ = nullptr;
    bool ok_ = false;
};

class OprfClientContext {
public:
    OprfClientContext(EcContext& ec, uint64_t bucket_nonce) : ec_(ec), bucket_nonce_(bucket_nonce) {}

    ~OprfClientContext() {
        for (BIGNUM* scalar : blind_scalars_) {
            BN_free(scalar);
        }
    }

    OprfClientContext(const OprfClientContext&) = delete;
    OprfClientContext& operator=(const OprfClientContext&) = delete;

    bool BlindVector(
        const std::vector<uint32_t>& signatures,
        int num_hash,
        std::vector<std::vector<uint8_t>>& blinded_request,
        std::string& error_message) {
        blinded_request.clear();
        blinded_request.reserve(signatures.size());
        blind_scalars_.reserve(signatures.size());

        EC_POINT* point_h = EC_POINT_new(ec_.group());
        EC_POINT* point_blind = EC_POINT_new(ec_.group());
        BIGNUM* h_scalar = BN_new();
        BIGNUM* blind_scalar = BN_new();
        if (point_h == nullptr || point_blind == nullptr || h_scalar == nullptr || blind_scalar == nullptr) {
            error_message = "OPRF client 盲化对象初始化失败";
            if (blind_scalar) BN_free(blind_scalar);
            if (h_scalar) BN_free(h_scalar);
            if (point_blind) EC_POINT_free(point_blind);
            if (point_h) EC_POINT_free(point_h);
            return false;
        }

        for (size_t i = 0; i < signatures.size(); ++i) {
            const uint32_t value = signatures[i];
            const uint32_t hash_index = static_cast<uint32_t>(i % static_cast<size_t>(num_hash));

            std::array<uint8_t, 12> value_input{};
            StoreU32BE(0x50494443U, value_input.data());  // "PIDC"
            StoreU32BE(hash_index, value_input.data() + 4);
            StoreU32BE(value, value_input.data() + 8);
            if (!DeriveNonZeroScalar(value_input.data(), value_input.size(), ec_.order(), ec_.ctx(), h_scalar)) {
                error_message = "派生 OPRF 输入点标量失败";
                BN_free(blind_scalar);
                BN_free(h_scalar);
                EC_POINT_free(point_blind);
                EC_POINT_free(point_h);
                return false;
            }
            if (EC_POINT_mul(ec_.group(), point_h, h_scalar, nullptr, nullptr, ec_.ctx()) != 1) {
                error_message = "EC_POINT_mul(point_h) 失败";
                BN_free(blind_scalar);
                BN_free(h_scalar);
                EC_POINT_free(point_blind);
                EC_POINT_free(point_h);
                return false;
            }

            std::array<uint8_t, 28> blind_input{};
            StoreU32BE(0x424c4e44U, blind_input.data());  // "BLND"
            StoreU64BE(bucket_nonce_, blind_input.data() + 4);
            StoreU64BE(kOprfClientSeed, blind_input.data() + 12);
            StoreU32BE(static_cast<uint32_t>(i), blind_input.data() + 20);
            StoreU32BE(value, blind_input.data() + 24);
            if (!DeriveNonZeroScalar(blind_input.data(), blind_input.size(), ec_.order(), ec_.ctx(), blind_scalar)) {
                error_message = "派生 OPRF 盲化标量失败";
                BN_free(blind_scalar);
                BN_free(h_scalar);
                EC_POINT_free(point_blind);
                EC_POINT_free(point_h);
                return false;
            }
            if (EC_POINT_mul(ec_.group(), point_blind, nullptr, point_h, blind_scalar, ec_.ctx()) != 1) {
                error_message = "EC_POINT_mul(point_blind) 失败";
                BN_free(blind_scalar);
                BN_free(h_scalar);
                EC_POINT_free(point_blind);
                EC_POINT_free(point_h);
                return false;
            }

            std::vector<uint8_t> serialized;
            if (!SerializePoint(ec_, point_blind, serialized, error_message)) {
                BN_free(blind_scalar);
                BN_free(h_scalar);
                EC_POINT_free(point_blind);
                EC_POINT_free(point_h);
                return false;
            }
            BIGNUM* saved_blind = BN_dup(blind_scalar);
            if (saved_blind == nullptr) {
                error_message = "保存 OPRF 盲化标量失败";
                BN_free(blind_scalar);
                BN_free(h_scalar);
                EC_POINT_free(point_blind);
                EC_POINT_free(point_h);
                return false;
            }
            blind_scalars_.push_back(saved_blind);
            blinded_request.push_back(std::move(serialized));
        }

        BN_free(blind_scalar);
        BN_free(h_scalar);
        EC_POINT_free(point_blind);
        EC_POINT_free(point_h);
        return true;
    }

    bool FinalizeVector(
        const std::vector<std::vector<uint8_t>>& evaluated_response,
        std::vector<uint32_t>& encrypted_signatures,
        std::string& error_message) const {
        if (evaluated_response.size() != blind_scalars_.size()) {
            error_message = "OPRF 响应数量与请求不匹配";
            return false;
        }
        encrypted_signatures.assign(evaluated_response.size(), 0U);

        EC_POINT* point_eval = EC_POINT_new(ec_.group());
        EC_POINT* point_unblind = EC_POINT_new(ec_.group());
        BIGNUM* blind_inv = BN_new();
        if (point_eval == nullptr || point_unblind == nullptr || blind_inv == nullptr) {
            error_message = "OPRF client 去盲化对象初始化失败";
            if (blind_inv) BN_free(blind_inv);
            if (point_unblind) EC_POINT_free(point_unblind);
            if (point_eval) EC_POINT_free(point_eval);
            return false;
        }

        for (size_t i = 0; i < evaluated_response.size(); ++i) {
            if (!DeserializePoint(ec_, evaluated_response[i], point_eval, error_message)) {
                BN_free(blind_inv);
                EC_POINT_free(point_unblind);
                EC_POINT_free(point_eval);
                return false;
            }
            if (BN_mod_inverse(blind_inv, blind_scalars_[i], ec_.order(), ec_.ctx()) == nullptr) {
                error_message = "BN_mod_inverse 失败";
                BN_free(blind_inv);
                EC_POINT_free(point_unblind);
                EC_POINT_free(point_eval);
                return false;
            }
            if (EC_POINT_mul(ec_.group(), point_unblind, nullptr, point_eval, blind_inv, ec_.ctx()) != 1) {
                error_message = "EC_POINT_mul(point_unblind) 失败";
                BN_free(blind_inv);
                EC_POINT_free(point_unblind);
                EC_POINT_free(point_eval);
                return false;
            }
            std::vector<uint8_t> serialized;
            if (!SerializePoint(ec_, point_unblind, serialized, error_message)) {
                BN_free(blind_inv);
                EC_POINT_free(point_unblind);
                EC_POINT_free(point_eval);
                return false;
            }

            uint8_t digest[SHA256_DIGEST_LENGTH];
            SHA256(serialized.data(), serialized.size(), digest);
            encrypted_signatures[i] = LoadU32BE(digest);
        }

        BN_free(blind_inv);
        EC_POINT_free(point_unblind);
        EC_POINT_free(point_eval);
        return true;
    }

private:
    EcContext& ec_;
    uint64_t bucket_nonce_ = 0;
    std::vector<BIGNUM*> blind_scalars_;
};

bool EcOprfTransform(
    const std::vector<uint32_t>& signatures,
    int num_hash,
    uint64_t oprf_key,
    uint64_t bucket_nonce,
    std::vector<uint32_t>& encrypted_signatures,
    std::string& error_message) {
    encrypted_signatures.clear();
    if (signatures.empty()) {
        return true;
    }

    EcContext ec(error_message);
    if (!ec.ok()) return false;

    OprfClientContext client(ec, bucket_nonce);
    std::vector<std::vector<uint8_t>> blinded_request;
    if (!client.BlindVector(signatures, num_hash, blinded_request, error_message)) {
        return false;
    }

    OprfKServer kserver(ec, oprf_key, error_message);
    if (!kserver.ok()) return false;

    std::vector<std::vector<uint8_t>> evaluated_response;
    if (!kserver.Evaluate(blinded_request, evaluated_response, error_message)) {
        return false;
    }

    return client.FinalizeVector(evaluated_response, encrypted_signatures, error_message);
}

}  // namespace

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
        return EcOprfTransform(signatures, num_hash, oprf_key, bucket_nonce, encrypted_signatures, error_message);
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

}  // namespace pri
