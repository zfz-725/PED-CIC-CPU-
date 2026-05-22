#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "pri_cpu.h"

namespace {

bool Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "[FAIL] " << message << "\n";
        return false;
    }
    return true;
}

bool TestEncryptSecureHashDeterministic() {
    const std::vector<uint32_t> input = {11, 22, 33, 44, 11, 22, 33, 44};
    std::vector<uint32_t> out1;
    std::vector<uint32_t> out2;
    std::string err;
    bool ok = pri::EncryptSignaturesCPU(
        input,
        4,
        pri::EncryptMode::kSecureHash,
        0x9e3779b97f4a7c15ULL,
        0xd1b54a32d192ed03ULL,
        12345ULL,
        out1,
        err);
    if (!Expect(ok, "SHA 加密调用失败: " + err)) return false;
    ok = pri::EncryptSignaturesCPU(
        input,
        4,
        pri::EncryptMode::kSecureHash,
        0x9e3779b97f4a7c15ULL,
        0xd1b54a32d192ed03ULL,
        12345ULL,
        out2,
        err);
    if (!Expect(ok, "SHA 二次加密调用失败: " + err)) return false;
    if (!Expect(out1 == out2, "SHA 加密结果不稳定")) return false;
    if (!Expect(out1 != input, "SHA 加密结果不应与输入完全相同")) return false;
    if (!Expect(out1.size() == input.size() * 8U, "SHA-256 输出应为每个签名分量 8 个 uint32_t")) return false;
    return true;
}

bool TestEncryptOprfDeterministic() {
    const std::vector<uint32_t> input = {10, 20, 20, 30, 40, 50, 60, 70};
    std::vector<uint32_t> out1;
    std::vector<uint32_t> out2;
    std::string err;
    bool ok = pri::EncryptSignaturesCPU(
        input,
        4,
        pri::EncryptMode::kOprf,
        0x9e3779b97f4a7c15ULL,
        0xd1b54a32d192ed03ULL,
        777ULL,
        out1,
        err);
    if (!Expect(ok, "OPRF 加密调用失败: " + err)) return false;
    ok = pri::EncryptSignaturesCPU(
        input,
        4,
        pri::EncryptMode::kOprf,
        0x9e3779b97f4a7c15ULL,
        0xd1b54a32d192ed03ULL,
        777ULL,
        out2,
        err);
    if (!Expect(ok, "OPRF 二次加密调用失败: " + err)) return false;
    if (!Expect(out1 == out2, "OPRF 加密结果不稳定")) return false;
    return true;
}

bool TestFindSimilarPairs() {
    const int num_hash = 4;
    const int docs = 3;
    const int threshold = 3;
    const std::vector<uint32_t> encrypted = {
        1, 2, 3, 4,
        1, 2, 9, 4,
        7, 8, 9, 0
    };
    std::vector<std::pair<int, int>> pairs;
    std::string err;
    const bool ok = pri::FindSimilarPairsCPU(encrypted, docs, num_hash, threshold, pairs, err);
    if (!Expect(ok, "相似对查找失败: " + err)) return false;
    if (!Expect(pairs.size() == 1U, "相似对数量不正确")) return false;
    if (!Expect(pairs.front().first == 0 && pairs.front().second == 1, "相似对应为(0,1)")) return false;
    return true;
}

bool TestSha256FindSimilarPairs() {
    const int num_hash = 4;
    const int docs = 3;
    const int threshold = 3;
    const std::vector<uint32_t> signatures = {
        1, 2, 3, 4,
        1, 2, 9, 4,
        7, 8, 9, 0
    };
    std::vector<uint32_t> encrypted;
    std::string err;
    bool ok = pri::EncryptSignaturesCPU(
        signatures,
        num_hash,
        pri::EncryptMode::kSecureHash,
        0x9e3779b97f4a7c15ULL,
        0xd1b54a32d192ed03ULL,
        12345ULL,
        encrypted,
        err);
    if (!Expect(ok, "SHA-256 加密调用失败: " + err)) return false;
    std::vector<std::pair<int, int>> pairs;
    ok = pri::FindSimilarPairsCPU(encrypted, docs, num_hash, threshold, pairs, err);
    if (!Expect(ok, "SHA-256 相似对查找失败: " + err)) return false;
    if (!Expect(pairs.size() == 1U, "SHA-256 相似对数量不正确")) return false;
    if (!Expect(pairs.front().first == 0 && pairs.front().second == 1, "SHA-256 相似对应为(0,1)")) return false;
    return true;
}

bool TestOprfFindSimilarPairs() {
    const int num_hash = 4;
    const int docs = 3;
    const int threshold = 3;
    const std::vector<uint32_t> signatures = {
        1, 2, 3, 4,
        1, 2, 9, 4,
        7, 8, 9, 0
    };
    std::vector<uint32_t> encrypted;
    std::string err;
    bool ok = pri::EncryptSignaturesCPU(
        signatures,
        num_hash,
        pri::EncryptMode::kOprf,
        0x9e3779b97f4a7c15ULL,
        0xd1b54a32d192ed03ULL,
        12345ULL,
        encrypted,
        err);
    if (!Expect(ok, "OPRF 加密调用失败: " + err)) return false;
    if (!Expect(encrypted.size() == signatures.size(), "OPRF 输出应为每个签名分量 1 个 uint32_t")) return false;
    std::vector<std::pair<int, int>> pairs;
    ok = pri::FindSimilarPairsCPU(encrypted, docs, num_hash, threshold, pairs, err);
    if (!Expect(ok, "OPRF 相似对查找失败: " + err)) return false;
    if (!Expect(pairs.size() == 1U, "OPRF 相似对数量不正确")) return false;
    if (!Expect(pairs.front().first == 0 && pairs.front().second == 1, "OPRF 相似对应为(0,1)")) return false;
    return true;
}

bool TestInvalidArgs() {
    std::vector<uint32_t> out;
    std::string err;
    bool ok = pri::EncryptSignaturesCPU(
        {1, 2, 3},
        0,
        pri::EncryptMode::kSecureHash,
        1,
        1,
        1,
        out,
        err);
    if (!Expect(!ok, "num_hash=0 应失败")) return false;
    std::vector<std::pair<int, int>> pairs;
    ok = pri::FindSimilarPairsCPU({1, 2, 3, 4}, 2, 2, 0, pairs, err);
    if (!Expect(!ok, "threshold_count=0 应失败")) return false;
    return true;
}

}

int main() {
    const bool ok =
        TestEncryptSecureHashDeterministic() &&
        TestEncryptOprfDeterministic() &&
        TestFindSimilarPairs() &&
        TestSha256FindSimilarPairs() &&
        TestOprfFindSimilarPairs() &&
        TestInvalidArgs();
    if (!ok) return 1;
    std::cout << "pri_cpu_test passed\n";
    return 0;
}
