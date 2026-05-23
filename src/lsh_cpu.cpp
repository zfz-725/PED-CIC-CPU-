#include "lsh_cpu.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace ped_cic_cpu {
namespace {

class UnionFind {
public:
    explicit UnionFind(int n) : parent_(n), rank_(n, 0) {
        std::iota(parent_.begin(), parent_.end(), 0);
    }

    int Find(int x) {
        if (parent_[x] == x) return x;
        parent_[x] = Find(parent_[x]);
        return parent_[x];
    }

    void Unite(int a, int b) {
        a = Find(a);
        b = Find(b);
        if (a == b) return;
        if (rank_[a] < rank_[b]) std::swap(a, b);
        parent_[b] = a;
        if (rank_[a] == rank_[b]) ++rank_[a];
    }

private:
    std::vector<int> parent_;
    std::vector<int> rank_;
};

struct FileResult {
    fs::path source;
    int lines = 0;
    std::vector<uint32_t> row_major;
};

struct DocRef {
    int file_index = 0;
    int line_index = 0;
    std::vector<uint32_t> signature;
    std::vector<uint32_t> buckets;
};

std::vector<fs::path> SortedFiles(const fs::path& root) {
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(root)) {
        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string Trim(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ReadJsonHex4(const std::string& value, size_t pos, uint32_t& code) {
    if (pos + 4U > value.size()) return false;
    uint32_t result = 0;
    for (size_t k = 0; k < 4U; ++k) {
        const int hex = HexValue(value[pos + k]);
        if (hex < 0) return false;
        result = (result << 4U) | static_cast<uint32_t>(hex);
    }
    code = result;
    return true;
}

void AppendUtf8(uint32_t code, std::string& out) {
    if (code <= 0x7fU) {
        out.push_back(static_cast<char>(code));
    } else if (code <= 0x7ffU) {
        out.push_back(static_cast<char>(0xc0U | (code >> 6U)));
        out.push_back(static_cast<char>(0x80U | (code & 0x3fU)));
    } else if (code <= 0xffffU) {
        out.push_back(static_cast<char>(0xe0U | (code >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (code & 0x3fU)));
    } else if (code <= 0x10ffffU) {
        out.push_back(static_cast<char>(0xf0U | (code >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (code & 0x3fU)));
    }
}

std::string JsonUnescape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 >= value.size()) {
            out.push_back(value[i]);
            continue;
        }
        const char esc = value[++i];
        switch (esc) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                uint32_t code = 0;
                if (!ReadJsonHex4(value, i + 1, code)) {
                    out.append("\\u");
                    break;
                }
                i += 4;
                if (code >= 0xd800U && code <= 0xdbffU && i + 2U < value.size() && value[i + 1] == '\\' && value[i + 2] == 'u') {
                    uint32_t low = 0;
                    if (ReadJsonHex4(value, i + 3, low) && low >= 0xdc00U && low <= 0xdfffU) {
                        code = 0x10000U + ((code - 0xd800U) << 10U) + (low - 0xdc00U);
                        i += 6;
                    }
                }
                AppendUtf8(code, out);
                break;
            }
            default:
                out.push_back(esc);
                break;
        }
    }
    return out;
}

std::string NormalizeUtf8ForDedup(const std::string& text, const LshConfig& cfg, LshStats& stats) {
    if (!cfg.normalize_utf8) return text;
    stats.utf8_normalization_fallback = true;
    throw std::runtime_error("当前构建暂未启用 Unicode NFC 归一化，请使用 --no-normalize-utf8");
}

std::string ExtractText(const std::string& line) {
    const auto key_pos = line.find("\"text\"");
    if (key_pos == std::string::npos) return line;
    const auto colon_pos = line.find(':', key_pos);
    if (colon_pos == std::string::npos) return line;
    size_t i = colon_pos + 1;
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
    if (i >= line.size()) return "";
    if (line[i] != '"') {
        size_t end = i;
        while (end < line.size() && line[end] != ',' && line[end] != '}') ++end;
        return Trim(line.substr(i, end - i));
    }

    ++i;
    std::string raw;
    bool escaped = false;
    for (; i < line.size(); ++i) {
        const char c = line[i];
        if (escaped) {
            raw.push_back('\\');
            raw.push_back(c);
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            break;
        } else {
            raw.push_back(c);
        }
    }
    if (escaped) raw.push_back('\\');
    return JsonUnescape(raw);
}

uint32_t HashWindow(const std::string& text, size_t begin, size_t len, uint32_t p, uint32_t q) {
    uint32_t h = 0;
    for (size_t i = 0; i < len; ++i) {
        h = (static_cast<uint64_t>(h) * p + static_cast<unsigned char>(text[begin + i])) % q;
    }
    return h;
}

std::vector<uint32_t> ComputeSignature(const std::string& text, const LshConfig& cfg) {
    std::vector<uint32_t> signature(static_cast<size_t>(cfg.num_hash), 0U);
    if (text.empty()) return signature;
    const int ngram = std::min(cfg.shingle_len, static_cast<int>(text.size()));
    for (int hash_idx = 0; hash_idx < cfg.num_hash; ++hash_idx) {
        const uint32_t q = 4294967U;
        const uint32_t p = static_cast<uint32_t>(257 + hash_idx);
        uint32_t best = q - 1U;
        if (cfg.shingle_len == 1) {
            best = HashWindow(text, 0, text.size(), p, q);
        } else {
            for (size_t i = 0; i + static_cast<size_t>(ngram) <= text.size(); ++i) {
                best = std::min(best, HashWindow(text, i, static_cast<size_t>(ngram), p, q));
            }
        }
        signature[static_cast<size_t>(hash_idx)] = best;
    }
    return signature;
}

static uint32_t HashBandValues(const std::vector<uint32_t>& signature, int band,
                                int rows_per_band, uint32_t seed) {
    uint32_t h = static_cast<uint32_t>(band) * seed;
    for (int i = band * rows_per_band; i < (band + 1) * rows_per_band; ++i) {
        h ^= signature[static_cast<size_t>(i)] + seed + (h << 6) + (h >> 2);
    }
    h ^= h >> 16;
    h *= 0x85ebca6bU;
    h ^= h >> 13;
    h *= 0xc2b2ae35U;
    h ^= h >> 16;
    return h;
}

std::vector<uint32_t> ComputeBuckets(const std::vector<uint32_t>& signature, const LshConfig& cfg) {
    std::vector<uint32_t> buckets(static_cast<size_t>(cfg.bands), 0U);
    const int rows_per_band = cfg.num_hash / cfg.bands;
    for (int band = 0; band < cfg.bands; ++band) {
        const uint32_t level1 = HashBandValues(signature, band, rows_per_band, 0x9e3779b9U)
                                % static_cast<uint32_t>(cfg.num_key);
        const uint32_t level2 = HashBandValues(signature, band, rows_per_band, 0x27d4eb2dU)
                                % static_cast<uint32_t>(cfg.max_bucket);
        buckets[static_cast<size_t>(band)] = level1 * static_cast<uint32_t>(cfg.max_bucket) + level2;
    }
    return buckets;
}

std::string BucketKey(int band, uint32_t bucket_id) {
    return std::to_string(band) + "#" + std::to_string(bucket_id);
}

int CountEqualHashes(const std::vector<uint32_t>& left, const std::vector<uint32_t>& right) {
    int count = 0;
    const size_t n = std::min(left.size(), right.size());
    for (size_t i = 0; i < n; ++i) {
        if (left[i] == right[i]) ++count;
    }
    return count;
}

void WriteHashBin(const fs::path& output_dir, const FileResult& result, const LshConfig& cfg) {
    fs::create_directories(output_dir);
    const fs::path out_file = output_dir / (result.source.stem().string() + "_hashresult.bin");
    std::ofstream out(out_file, std::ios::binary);
    if (!out.is_open()) {
        throw std::runtime_error("无法写出哈希结果文件: " + out_file.string());
    }
    out.write(reinterpret_cast<const char*>(result.row_major.data()),
              static_cast<std::streamsize>(result.row_major.size() * sizeof(uint32_t)));
    if (!out.good()) {
        throw std::runtime_error("写出哈希结果文件失败: " + out_file.string());
    }
}

void WriteBucketAssignmentsBin(const fs::path& output_dir, const FileResult& result, const LshConfig& cfg) {
    fs::create_directories(output_dir);
    const fs::path out_file = output_dir / (result.source.stem().string() + "_bucketassign.bin");
    std::ofstream out(out_file, std::ios::binary);
    if (!out.is_open()) {
        throw std::runtime_error("无法写出桶归属文件: " + out_file.string());
    }
    const int cols = cfg.num_hash + cfg.bands;
    for (int row = 0; row < result.lines; ++row) {
        const size_t base = static_cast<size_t>(row) * static_cast<size_t>(cols) + static_cast<size_t>(cfg.num_hash);
        out.write(reinterpret_cast<const char*>(result.row_major.data() + base),
                  static_cast<std::streamsize>(static_cast<size_t>(cfg.bands) * sizeof(uint32_t)));
    }
    if (!out.good()) {
        throw std::runtime_error("写出桶归属文件失败: " + out_file.string());
    }
}

}

LshStats RunDirectory(const fs::path& input_dir, const fs::path& output_dir, const LshConfig& cfg) {
    if (!fs::exists(input_dir) || !fs::is_directory(input_dir)) {
        throw std::runtime_error("输入目录不存在: " + input_dir.string());
    }
    if (cfg.num_hash <= 0 || cfg.bands <= 0 || cfg.num_hash % cfg.bands != 0) {
        throw std::runtime_error("num_hash 与 bands 非法，且 num_hash 必须可被 bands 整除");
    }
    if (cfg.shingle_len <= 0 || cfg.num_key <= 0) {
        throw std::runtime_error("shingle_len 与 num_key 必须大于 0");
    }

    using clock = std::chrono::high_resolution_clock;
    const auto total_begin = clock::now();
    const auto minhash_begin = clock::now();

    LshStats stats;
    std::vector<FileResult> file_results;
    std::vector<DocRef> docs;
    const int cols = cfg.num_hash + cfg.bands;

    for (const auto& file : SortedFiles(input_dir)) {
        std::ifstream in(file);
        if (!in.is_open()) {
            throw std::runtime_error("无法打开输入文件: " + file.string());
        }

        FileResult result;
        result.source = file;
        std::string line;
        while (std::getline(in, line)) {
            std::string text = NormalizeUtf8ForDedup(ExtractText(line), cfg, stats);
            if (cfg.min_text_len > 0 && static_cast<int>(text.size()) < cfg.min_text_len) {
                text.clear();
            }
            auto signature = ComputeSignature(text, cfg);
            auto buckets = ComputeBuckets(signature, cfg);

            result.row_major.insert(result.row_major.end(), signature.begin(), signature.end());
            result.row_major.insert(result.row_major.end(), buckets.begin(), buckets.end());

            DocRef ref;
            ref.file_index = static_cast<int>(file_results.size());
            ref.line_index = result.lines;
            ref.signature = std::move(signature);
            ref.buckets = std::move(buckets);
            docs.push_back(std::move(ref));
            ++result.lines;
            ++stats.docs;
        }
        if (cfg.keep_hash) {
            WriteHashBin(output_dir, result, cfg);
        }
        WriteBucketAssignmentsBin(output_dir, result, cfg);
        file_results.push_back(std::move(result));
        ++stats.files;
    }

    stats.minhash_s = std::chrono::duration<double>(clock::now() - minhash_begin).count();

    const auto dedup_begin = clock::now();
    std::unordered_map<std::string, std::vector<int>> bucket_to_docs;
    for (int doc_idx = 0; doc_idx < static_cast<int>(docs.size()); ++doc_idx) {
        for (int band = 0; band < cfg.bands; ++band) {
            bucket_to_docs[BucketKey(band, docs[doc_idx].buckets[static_cast<size_t>(band)])].push_back(doc_idx);
        }
    }

    UnionFind uf(static_cast<int>(docs.size()));
    std::unordered_set<uint64_t> seen_pairs;
    const int threshold_count = static_cast<int>(cfg.threshold * static_cast<double>(cfg.num_hash));
    for (auto& kv : bucket_to_docs) {
        auto& bucket_docs = kv.second;
        std::sort(bucket_docs.begin(), bucket_docs.end());
        bucket_docs.erase(std::unique(bucket_docs.begin(), bucket_docs.end()), bucket_docs.end());
        for (size_t i = 0; i < bucket_docs.size(); ++i) {
            for (size_t j = i + 1; j < bucket_docs.size(); ++j) {
                const int a = bucket_docs[i];
                const int b = bucket_docs[j];
                const uint64_t key = (static_cast<uint64_t>(a) << 32U) ^ static_cast<uint32_t>(b);
                if (!seen_pairs.insert(key).second) continue;
                if (CountEqualHashes(docs[a].signature, docs[b].signature) >= threshold_count) {
                    uf.Unite(a, b);
                    ++stats.local_duplicate_pairs;
                }
            }
        }
    }

    std::unordered_map<int, int> root_keep;
    for (int i = 0; i < static_cast<int>(docs.size()); ++i) {
        const int root = uf.Find(i);
        auto it = root_keep.find(root);
        if (it == root_keep.end()) {
            root_keep[root] = i;
            continue;
        }
        const int keep = std::min(it->second, i);
        const int remove = std::max(it->second, i);
        root_keep[root] = keep;
        if (remove == i) ++stats.local_delete_docs;
    }

    stats.local_dedup_s = std::chrono::duration<double>(clock::now() - dedup_begin).count();
    stats.total_s = std::chrono::duration<double>(clock::now() - total_begin).count();
    return stats;
}

}
