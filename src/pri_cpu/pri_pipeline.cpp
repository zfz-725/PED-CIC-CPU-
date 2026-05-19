#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "pri_cpu.h"
#include "pri_types.h"

namespace fs = std::filesystem;

namespace pri {

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

std::vector<fs::path> SortedSubDirs(const fs::path& root) {
    std::vector<fs::path> dirs;
    for (const auto& entry : fs::directory_iterator(root)) {
        if (entry.is_directory()) {
            dirs.push_back(entry.path());
        }
    }
    std::sort(dirs.begin(), dirs.end());
    return dirs;
}

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

int CountLines(const fs::path& file_path) {
    std::ifstream in(file_path);
    if (!in.is_open()) {
        throw std::runtime_error("无法打开文件: " + file_path.string());
    }
    int cnt = 0;
    std::string line;
    while (std::getline(in, line)) {
        ++cnt;
    }
    return cnt;
}

std::vector<uint32_t> LoadHashBin(const fs::path& bin_path, int rows, int cols) {
    std::ifstream in(bin_path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("无法打开哈希结果文件: " + bin_path.string());
    }
    const size_t total = static_cast<size_t>(rows) * static_cast<size_t>(cols);
    std::vector<uint32_t> data(total, 0);
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(sizeof(uint32_t) * total));
    if (in.gcount() != static_cast<std::streamsize>(sizeof(uint32_t) * total)) {
        throw std::runtime_error("哈希结果文件大小不符合预期: " + bin_path.string());
    }
    return data;
}

std::string BucketKey(int band, uint32_t bucket_id) {
    return std::to_string(band) + "#" + std::to_string(bucket_id);
}

uint64_t StableNonce(const std::string& bucket_key) {
    return static_cast<uint64_t>(std::hash<std::string>{}(bucket_key));
}

struct SignatureKey {
    std::vector<uint32_t> values;

    bool operator==(const SignatureKey& other) const {
        return values == other.values;
    }
};

struct SignatureKeyHash {
    size_t operator()(const SignatureKey& key) const {
        uint64_t hash = 1469598103934665603ULL;
        for (uint32_t value : key.values) {
            hash ^= static_cast<uint64_t>(value);
            hash *= 1099511628211ULL;
        }
        return static_cast<size_t>(hash);
    }
};

void EnsureDir(const fs::path& p) {
    if (!fs::exists(p)) {
        fs::create_directories(p);
    }
}

PipelineConfig ParseArgsImpl(int argc, char** argv) {
    PipelineConfig cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto require_value = [&](const std::string& key) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("参数缺少取值: " + key);
            }
            return argv[++i];
        };

        if (arg == "--inst-root") {
            cfg.institution_root = require_value(arg);
        } else if (arg == "--lsh-root") {
            cfg.lsh_result_root = require_value(arg);
        } else if (arg == "--out") {
            cfg.output_root = require_value(arg);
        } else if (arg == "--num-hash") {
            cfg.num_hash = std::stoi(require_value(arg));
        } else if (arg == "--bands") {
            cfg.bands = std::stoi(require_value(arg));
        } else if (arg == "--threshold") {
            cfg.similarity_threshold = std::stod(require_value(arg));
        } else if (arg == "--mode") {
            const std::string mode = require_value(arg);
            if (mode == "sha") {
                cfg.mode = EncryptMode::kSecureHash;
            } else if (mode == "oprf") {
                cfg.mode = EncryptMode::kOprf;
            } else {
                throw std::runtime_error("mode 仅支持 sha 或 oprf");
            }
        } else {
            throw std::runtime_error("未知参数: " + arg);
        }
    }

    if (cfg.institution_root.empty() || cfg.lsh_result_root.empty() || cfg.output_root.empty()) {
        throw std::runtime_error("必须提供 --inst-root --lsh-root --out");
    }
    if (cfg.num_hash <= 0 || cfg.bands <= 0 || cfg.num_hash % cfg.bands != 0) {
        throw std::runtime_error("num_hash 与 bands 非法，且 num_hash 必须可被 bands 整除");
    }
    if (cfg.similarity_threshold <= 0.0 || cfg.similarity_threshold > 1.0) {
        throw std::runtime_error("threshold 必须在 (0, 1] 区间");
    }
    return cfg;
}

void PrintUsageImpl() {
    std::cout << "用法:\n"
              << "  pri_cpu_main --inst-root <机构数据根目录> --lsh-root <机构哈希结果根目录> "
              << "--out <输出目录> [--mode sha|oprf] [--num-hash 128] [--bands 16] [--threshold 0.8]\n";
}

}

PipelineConfig ParseArgs(int argc, char** argv) {
    return ParseArgsImpl(argc, argv);
}

void PrintUsage() {
    PrintUsageImpl();
}

int RunPipeline(const PipelineConfig& cfg, PipelineStats& stats) {
    using clock = std::chrono::high_resolution_clock;
    const auto t_begin = clock::now();

    std::vector<DocRecord> docs;
    docs.reserve(1024);

    std::unordered_map<std::string, std::vector<int>> bucket_to_docs;
    std::unordered_map<std::string, std::unordered_set<std::string>> bucket_to_insts;

    const auto phase1_start = clock::now();
    const auto inst_dirs = SortedSubDirs(cfg.institution_root);
    if (inst_dirs.empty()) {
        throw std::runtime_error("机构目录为空: " + cfg.institution_root);
    }

    for (const auto& inst_dir : inst_dirs) {
        const std::string inst_id = inst_dir.filename().string();
        const auto data_files = SortedFiles(inst_dir);
        const fs::path inst_lsh_dir = fs::path(cfg.lsh_result_root) / inst_id;
        if (!fs::exists(inst_lsh_dir)) {
            throw std::runtime_error("缺少机构哈希输出目录: " + inst_lsh_dir.string());
        }

        for (const auto& file : data_files) {
            const int lines = CountLines(file);
            if (lines <= 0) continue;
            const std::string stem = file.stem().string();
            const fs::path hash_path = inst_lsh_dir / (stem + "_hashresult.bin");
            const int cols = cfg.num_hash + cfg.bands;
            std::vector<uint32_t> row_major = LoadHashBin(hash_path, lines, cols);

            for (int i = 0; i < lines; ++i) {
                DocRecord rec;
                rec.uid = static_cast<int64_t>(docs.size());
                rec.location.institution = inst_id;
                rec.location.source_file = file.filename().string();
                rec.location.line_index = i;
                rec.signature.resize(cfg.num_hash);
                rec.bucket_ids.resize(cfg.bands);

                const size_t base = static_cast<size_t>(i) * static_cast<size_t>(cols);
                std::copy_n(row_major.begin() + static_cast<long>(base), cfg.num_hash, rec.signature.begin());
                std::copy_n(row_major.begin() + static_cast<long>(base + cfg.num_hash), cfg.bands, rec.bucket_ids.begin());

                const int doc_index = static_cast<int>(docs.size());
                for (int b = 0; b < cfg.bands; ++b) {
                    const std::string key = BucketKey(b, rec.bucket_ids[b]);
                    bucket_to_docs[key].push_back(doc_index);
                    bucket_to_insts[key].insert(inst_id);
                }
                docs.push_back(std::move(rec));
            }
        }
    }

    stats.total_docs = static_cast<int64_t>(docs.size());
    stats.total_buckets = static_cast<int64_t>(bucket_to_docs.size());
    stats.phase_bucket_submit_s = std::chrono::duration<double>(clock::now() - phase1_start).count();

    std::vector<std::string> shared_bucket_keys;
    shared_bucket_keys.reserve(bucket_to_docs.size());
    for (const auto& kv : bucket_to_insts) {
        if (kv.second.size() >= 2U) {
            shared_bucket_keys.push_back(kv.first);
        }
    }
    std::sort(shared_bucket_keys.begin(), shared_bucket_keys.end());
    stats.shared_bucket_count = static_cast<int64_t>(shared_bucket_keys.size());

    const auto phase2_start = clock::now();
    std::unordered_set<int> requested_doc_set;
    for (const auto& bucket_key : shared_bucket_keys) {
        for (int doc_idx : bucket_to_docs[bucket_key]) {
            requested_doc_set.insert(doc_idx);
        }
    }
    stats.requested_docs = static_cast<int64_t>(requested_doc_set.size());

    std::vector<int> requested_docs(requested_doc_set.begin(), requested_doc_set.end());
    std::sort(requested_docs.begin(), requested_docs.end());

    std::unordered_map<SignatureKey, size_t, SignatureKeyHash> signature_to_unique;
    signature_to_unique.reserve(requested_docs.size());
    std::vector<std::vector<uint32_t>> unique_signatures;
    unique_signatures.reserve(requested_docs.size());
    std::vector<size_t> doc_to_unique(requested_docs.size(), 0U);
    for (size_t i = 0; i < requested_docs.size(); ++i) {
        const int doc_idx = requested_docs[i];
        SignatureKey key{docs[doc_idx].signature};
        auto inserted = signature_to_unique.emplace(std::move(key), unique_signatures.size());
        if (inserted.second) {
            unique_signatures.push_back(inserted.first->first.values);
        }
        doc_to_unique[i] = inserted.first->second;
    }

    std::vector<std::vector<uint32_t>> unique_encrypted_results(unique_signatures.size());
    std::vector<std::string> encrypt_errors(unique_signatures.size());
    std::vector<unsigned char> encrypt_ok(unique_signatures.size(), 0U);
    const uint64_t nonce = StableNonce("pri_cpu_global_compare_domain");

#pragma omp parallel for schedule(dynamic)
    for (int i = 0; i < static_cast<int>(unique_signatures.size()); ++i) {
        std::string local_error;
        std::vector<uint32_t> encrypted;
        if (EncryptSignaturesCPU(
                unique_signatures[static_cast<size_t>(i)],
                cfg.num_hash,
                cfg.mode,
                cfg.secure_hash_key,
                cfg.oprf_key,
                nonce,
                encrypted,
                local_error)) {
            unique_encrypted_results[static_cast<size_t>(i)] = std::move(encrypted);
            encrypt_ok[static_cast<size_t>(i)] = 1U;
        } else {
            encrypt_errors[static_cast<size_t>(i)] = std::move(local_error);
        }
    }

    std::unordered_map<int, std::vector<uint32_t>> encrypted_by_doc;
    encrypted_by_doc.reserve(requested_docs.size());
    for (size_t i = 0; i < requested_docs.size(); ++i) {
        const size_t unique_idx = doc_to_unique[i];
        if (encrypt_ok[unique_idx] == 0U) {
            throw std::runtime_error("CPU 加密失败: " + encrypt_errors[unique_idx]);
        }
        encrypted_by_doc.emplace(requested_docs[i], unique_encrypted_results[unique_idx]);
    }
    stats.phase_request_encrypt_s = std::chrono::duration<double>(clock::now() - phase2_start).count();

    const auto phase3_start = clock::now();
    std::string cpu_error;
    std::unordered_set<uint64_t> unique_pairs;
    const int threshold_count = static_cast<int>(cfg.similarity_threshold * static_cast<double>(cfg.num_hash));
    for (const auto& bucket_key : shared_bucket_keys) {
        auto candidate_docs = bucket_to_docs[bucket_key];
        std::sort(candidate_docs.begin(), candidate_docs.end());
        candidate_docs.erase(std::unique(candidate_docs.begin(), candidate_docs.end()), candidate_docs.end());
        if (candidate_docs.size() <= 1U) continue;

        std::vector<uint32_t> bucket_signatures;
        bucket_signatures.reserve(candidate_docs.size() * static_cast<size_t>(cfg.num_hash));
        for (int doc_idx : candidate_docs) {
            auto it = encrypted_by_doc.find(doc_idx);
            if (it == encrypted_by_doc.end()) {
                throw std::runtime_error("缺失加密特征，doc_idx=" + std::to_string(doc_idx));
            }
            bucket_signatures.insert(bucket_signatures.end(), it->second.begin(), it->second.end());
        }

        std::vector<std::pair<int, int>> local_pairs;
        if (!FindSimilarPairsCPU(
                bucket_signatures,
                static_cast<int>(candidate_docs.size()),
                cfg.num_hash,
                threshold_count,
                local_pairs,
                cpu_error)) {
            throw std::runtime_error("CPU 相似度比较失败: " + cpu_error);
        }

        for (const auto& p : local_pairs) {
            const int left = candidate_docs[p.first];
            const int right = candidate_docs[p.second];
            if (docs[left].location.institution == docs[right].location.institution) continue;
            const int a = std::min(left, right);
            const int b = std::max(left, right);
            const uint64_t key = (static_cast<uint64_t>(a) << 32U) ^ static_cast<uint64_t>(b);
            unique_pairs.insert(key);
        }
    }

    stats.duplicate_pairs = static_cast<int64_t>(unique_pairs.size());
    stats.phase_global_match_s = std::chrono::duration<double>(clock::now() - phase3_start).count();

    const auto phase4_start = clock::now();
    UnionFind uf(static_cast<int>(docs.size()));
    for (uint64_t packed : unique_pairs) {
        const int a = static_cast<int>(packed >> 32U);
        const int b = static_cast<int>(packed & 0xffffffffU);
        uf.Unite(a, b);
    }

    std::unordered_map<int, int> root_keep;
    std::unordered_map<std::string, std::vector<int>> delete_doc_indices_by_inst;
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
        delete_doc_indices_by_inst[docs[remove].location.institution].push_back(remove);
    }

    EnsureDir(cfg.output_root);
    for (auto& kv : delete_doc_indices_by_inst) {
        std::sort(kv.second.begin(), kv.second.end());
        kv.second.erase(std::unique(kv.second.begin(), kv.second.end()), kv.second.end());
        fs::path out_file = fs::path(cfg.output_root) / (kv.first + "_delete_ids.txt");
        std::ofstream out(out_file);
        if (!out.is_open()) {
            throw std::runtime_error("无法写出删除列表: " + out_file.string());
        }
        for (int doc_idx : kv.second) {
            const auto& rec = docs[doc_idx];
            out << rec.uid << "," << rec.location.source_file << "," << rec.location.line_index << "\n";
        }
        stats.delete_docs += static_cast<int64_t>(kv.second.size());
    }

    stats.phase_distribution_s = std::chrono::duration<double>(clock::now() - phase4_start).count();
    stats.total_s = std::chrono::duration<double>(clock::now() - t_begin).count();

    {
        fs::path stat_file = fs::path(cfg.output_root) / "pri_stats.txt";
        std::ofstream out(stat_file);
        if (!out.is_open()) {
            throw std::runtime_error("无法写出统计文件: " + stat_file.string());
        }
        out << "total_docs=" << stats.total_docs << "\n";
        out << "total_buckets=" << stats.total_buckets << "\n";
        out << "shared_bucket_count=" << stats.shared_bucket_count << "\n";
        out << "requested_docs=" << stats.requested_docs << "\n";
        out << "duplicate_pairs=" << stats.duplicate_pairs << "\n";
        out << "delete_docs=" << stats.delete_docs << "\n";
        out << "phase_bucket_submit_s=" << stats.phase_bucket_submit_s << "\n";
        out << "phase_request_encrypt_s=" << stats.phase_request_encrypt_s << "\n";
        out << "phase_global_match_s=" << stats.phase_global_match_s << "\n";
        out << "phase_distribution_s=" << stats.phase_distribution_s << "\n";
        out << "total_s=" << stats.total_s << "\n";
    }

    return 0;
}

}

int main(int argc, char** argv) {
    try {
        const pri::PipelineConfig cfg = pri::ParseArgs(argc, argv);
        pri::PipelineStats stats;
        const int rc = pri::RunPipeline(cfg, stats);
        std::cout << "PRI-CPU pipeline finished\n";
        std::cout << "  total_docs: " << stats.total_docs << "\n";
        std::cout << "  shared_bucket_count: " << stats.shared_bucket_count << "\n";
        std::cout << "  requested_docs: " << stats.requested_docs << "\n";
        std::cout << "  duplicate_pairs: " << stats.duplicate_pairs << "\n";
        std::cout << "  delete_docs: " << stats.delete_docs << "\n";
        std::cout << "  total_time_s: " << stats.total_s << "\n";
        return rc;
    } catch (const std::exception& e) {
        std::cerr << "[PRI-CPU] error: " << e.what() << "\n";
        pri::PrintUsage();
        return 1;
    }
}
