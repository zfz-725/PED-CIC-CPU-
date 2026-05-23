#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
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

struct LocalDocRef {
    fs::path hash_path;
    int line_index = 0;
};

struct InstitutionState {
    std::string source_id;
    std::unordered_map<int, LocalDocRef> local_docs;
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

std::vector<uint32_t> LoadBucketAssignmentsBin(const fs::path& bin_path, int rows, int bands) {
    std::ifstream in(bin_path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("无法打开桶归属文件: " + bin_path.string());
    }
    const size_t total = static_cast<size_t>(rows) * static_cast<size_t>(bands);
    std::vector<uint32_t> data(total, 0);
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(sizeof(uint32_t) * total));
    if (in.gcount() != static_cast<std::streamsize>(sizeof(uint32_t) * total)) {
        throw std::runtime_error("桶归属文件大小不符合预期: " + bin_path.string());
    }
    return data;
}

struct HashBinCache {
    int cols = 0;
    int num_hash = 0;
    std::vector<uint32_t> row_major;
};

HashBinCache LoadHashBinFile(const fs::path& bin_path, int num_hash, int bands) {
    std::ifstream in(bin_path, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        throw std::runtime_error("无法打开哈希结果文件: " + bin_path.string());
    }
    const auto file_size = in.tellg();
    if (file_size < 0) {
        throw std::runtime_error("无法获取文件大小: " + bin_path.string());
    }
    const int cols = num_hash + bands;
    const size_t total_elems = static_cast<size_t>(file_size) / sizeof(uint32_t);
    in.seekg(0, std::ios::beg);
    HashBinCache cache;
    cache.cols = cols;
    cache.num_hash = num_hash;
    cache.row_major.resize(total_elems);
    in.read(reinterpret_cast<char*>(cache.row_major.data()),
            static_cast<std::streamsize>(total_elems * sizeof(uint32_t)));
    if (!in.good()) {
        throw std::runtime_error("读取哈希结果文件失败: " + bin_path.string());
    }
    return cache;
}

void ExtractSignatureFromCache(const HashBinCache& cache, int line_index,
                               std::vector<uint32_t>& out) {
    out.resize(static_cast<size_t>(cache.num_hash));
    const size_t base = static_cast<size_t>(line_index) * static_cast<size_t>(cache.cols);
    std::copy_n(cache.row_major.begin() + static_cast<long>(base),
                cache.num_hash, out.begin());
}

std::vector<uint32_t> LoadSignatureForDoc(const LocalDocRef& ref, int num_hash, int bands) {
    std::ifstream in(ref.hash_path, std::ios::binary);
    if (!in.is_open()) {
        throw std::runtime_error("机构本地哈希结果文件缺失: " + ref.hash_path.string());
    }
    const int cols = num_hash + bands;
    const auto offset = static_cast<std::streamoff>(ref.line_index) * static_cast<std::streamoff>(cols) *
                        static_cast<std::streamoff>(sizeof(uint32_t));
    in.seekg(offset, std::ios::beg);
    if (!in.good()) {
        throw std::runtime_error("定位机构本地签名失败: " + ref.hash_path.string());
    }
    std::vector<uint32_t> signature(static_cast<size_t>(num_hash), 0U);
    in.read(reinterpret_cast<char*>(signature.data()), static_cast<std::streamsize>(sizeof(uint32_t) * signature.size()));
    if (in.gcount() != static_cast<std::streamsize>(sizeof(uint32_t) * signature.size())) {
        throw std::runtime_error("读取机构本地签名失败: " + ref.hash_path.string());
    }
    return signature;
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

std::unordered_map<int, std::vector<uint32_t>> InstitutionEncryptHandler(
    const InstitutionState& inst_state,
    const std::vector<int>& requested_docs,
    const PipelineConfig& cfg,
    uint64_t nonce,
    int64_t& requested_unique_signatures,
    int64_t& encrypted_feature_words) {
    std::unordered_set<fs::path> needed_files;
    needed_files.reserve(32);
    for (int doc_idx : requested_docs) {
        auto local_it = inst_state.local_docs.find(doc_idx);
        if (local_it == inst_state.local_docs.end()) {
            throw std::runtime_error("机构本地文档索引缺失，doc_idx=" + std::to_string(doc_idx));
        }
        needed_files.insert(local_it->second.hash_path);
    }

    std::unordered_map<fs::path, HashBinCache> file_cache;
    file_cache.reserve(needed_files.size());
    for (const auto& path : needed_files) {
        file_cache.emplace(path, LoadHashBinFile(path, cfg.num_hash, cfg.bands));
    }

    std::vector<uint32_t> sig_buffer(static_cast<size_t>(cfg.num_hash));
    std::unordered_map<SignatureKey, size_t, SignatureKeyHash> signature_to_unique;
    signature_to_unique.reserve(requested_docs.size());
    std::vector<std::vector<uint32_t>> unique_signatures;
    unique_signatures.reserve(requested_docs.size());
    std::vector<size_t> doc_to_unique(requested_docs.size(), 0U);

    for (size_t i = 0; i < requested_docs.size(); ++i) {
        const int doc_idx = requested_docs[i];
        const auto& ref = inst_state.local_docs.at(doc_idx);
        const auto& cache = file_cache.at(ref.hash_path);
        ExtractSignatureFromCache(cache, ref.line_index, sig_buffer);
        SignatureKey key{sig_buffer};
        auto inserted = signature_to_unique.emplace(std::move(key), unique_signatures.size());
        if (inserted.second) {
            unique_signatures.push_back(inserted.first->first.values);
        }
        doc_to_unique[i] = inserted.first->second;
    }
    requested_unique_signatures += static_cast<int64_t>(unique_signatures.size());

    std::vector<std::vector<uint32_t>> unique_encrypted_results(unique_signatures.size());
    std::vector<std::string> encrypt_errors(unique_signatures.size());
    std::vector<unsigned char> encrypt_ok(unique_signatures.size(), 0U);

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
        const auto& encrypted = unique_encrypted_results[unique_idx];
        encrypted_feature_words += static_cast<int64_t>(encrypted.size());
        encrypted_by_doc.emplace(requested_docs[i], encrypted);
    }
    return encrypted_by_doc;
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

}  // namespace

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

    std::unordered_map<std::string, InstitutionState> institutions;
    std::unordered_map<std::string, std::vector<int>> bucket_to_docs;
    std::unordered_map<std::string, std::unordered_set<std::string>> bucket_to_insts;

    const auto phase1_start = clock::now();
    const auto inst_dirs = SortedSubDirs(cfg.institution_root);
    if (inst_dirs.empty()) {
        throw std::runtime_error("机构目录为空: " + cfg.institution_root);
    }

    for (size_t inst_pos = 0; inst_pos < inst_dirs.size(); ++inst_pos) {
        const auto& inst_dir = inst_dirs[inst_pos];
        const std::string source_inst_id = inst_dir.filename().string();
        const std::string anon_inst_id = "anon_inst_" + std::to_string(inst_pos);
        InstitutionState& inst_state = institutions[anon_inst_id];
        inst_state.source_id = source_inst_id;
        const auto data_files = SortedFiles(inst_dir);
        const fs::path inst_lsh_dir = fs::path(cfg.lsh_result_root) / source_inst_id;
        if (!fs::exists(inst_lsh_dir)) {
            throw std::runtime_error("缺少机构哈希输出目录: " + inst_lsh_dir.string());
        }

        for (const auto& file : data_files) {
            const int lines = CountLines(file);
            if (lines <= 0) continue;
            const std::string stem = file.stem().string();
            const fs::path bucket_path = inst_lsh_dir / (stem + "_bucketassign.bin");
            const fs::path hash_path = inst_lsh_dir / (stem + "_hashresult.bin");
            std::vector<uint32_t> bucket_rows = LoadBucketAssignmentsBin(bucket_path, lines, cfg.bands);

            for (int i = 0; i < lines; ++i) {
                DocRecord rec;
                rec.uid = static_cast<int64_t>(docs.size());
                rec.location.institution = anon_inst_id;
                rec.location.source_file = file.filename().string();
                rec.location.line_index = i;
                rec.bucket_ids.resize(cfg.bands);

                const size_t base = static_cast<size_t>(i) * static_cast<size_t>(cfg.bands);
                std::copy_n(bucket_rows.begin() + static_cast<long>(base), cfg.bands, rec.bucket_ids.begin());

                const int doc_index = static_cast<int>(docs.size());
                inst_state.local_docs.emplace(doc_index, LocalDocRef{hash_path, i});
                for (int b = 0; b < cfg.bands; ++b) {
                    const std::string key = BucketKey(b, rec.bucket_ids[b]);
                    bucket_to_docs[key].push_back(doc_index);
                    bucket_to_insts[key].insert(anon_inst_id);
                    ++stats.total_bucket_assignments;
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
            stats.shared_bucket_doc_assignments += static_cast<int64_t>(bucket_to_docs[kv.first].size());
        }
    }
    std::sort(shared_bucket_keys.begin(), shared_bucket_keys.end());
    stats.shared_bucket_count = static_cast<int64_t>(shared_bucket_keys.size());
    stats.single_institution_bucket_count = stats.total_buckets - stats.shared_bucket_count;
    if (stats.total_buckets > 0) {
        stats.shared_bucket_ratio = static_cast<double>(stats.shared_bucket_count) / static_cast<double>(stats.total_buckets);
    }

    const auto phase2_start = clock::now();
    std::unordered_set<int> requested_doc_set;
    std::unordered_map<std::string, std::vector<int>> requested_by_inst;
    for (const auto& bucket_key : shared_bucket_keys) {
        for (int doc_idx : bucket_to_docs[bucket_key]) {
            requested_doc_set.insert(doc_idx);
            requested_by_inst[docs[doc_idx].location.institution].push_back(doc_idx);
        }
    }
    stats.requested_docs = static_cast<int64_t>(requested_doc_set.size());
    if (stats.total_docs > 0) {
        stats.requested_doc_ratio = static_cast<double>(stats.requested_docs) / static_cast<double>(stats.total_docs);
    }

    std::unordered_map<int, std::vector<uint32_t>> encrypted_by_doc;
    encrypted_by_doc.reserve(requested_doc_set.size());
    const uint64_t nonce = StableNonce("pri_cpu_global_compare_domain");

    for (auto& request_kv : requested_by_inst) {
        std::vector<int>& inst_requested_docs = request_kv.second;
        std::sort(inst_requested_docs.begin(), inst_requested_docs.end());
        inst_requested_docs.erase(std::unique(inst_requested_docs.begin(), inst_requested_docs.end()), inst_requested_docs.end());

        auto inst_it = institutions.find(request_kv.first);
        if (inst_it == institutions.end()) {
            throw std::runtime_error("缺失机构本地状态: " + request_kv.first);
        }
        const InstitutionState& inst_state = inst_it->second;
        auto inst_encrypted = InstitutionEncryptHandler(
            inst_state,
            inst_requested_docs,
            cfg,
            nonce,
            stats.requested_unique_signatures,
            stats.encrypted_feature_words);
        for (auto& encrypted_kv : inst_encrypted) {
            encrypted_by_doc.emplace(encrypted_kv.first, std::move(encrypted_kv.second));
        }
    }

    if (stats.total_docs > 0) {
        stats.exposed_signature_ratio = static_cast<double>(stats.requested_unique_signatures) / static_cast<double>(stats.total_docs);
    }
    stats.phase_request_encrypt_s = std::chrono::duration<double>(clock::now() - phase2_start).count();

    const auto phase3_start = clock::now();
    std::unordered_map<int, int> remaining_bucket_uses_by_doc;
    remaining_bucket_uses_by_doc.reserve(requested_doc_set.size());
    for (const auto& bucket_key : shared_bucket_keys) {
        std::vector<int> candidate_docs = bucket_to_docs[bucket_key];
        std::sort(candidate_docs.begin(), candidate_docs.end());
        candidate_docs.erase(std::unique(candidate_docs.begin(), candidate_docs.end()), candidate_docs.end());
        if (candidate_docs.size() <= 1U) continue;
        for (int doc_idx : candidate_docs) {
            ++remaining_bucket_uses_by_doc[doc_idx];
        }
    }

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

        for (int doc_idx : candidate_docs) {
            auto use_it = remaining_bucket_uses_by_doc.find(doc_idx);
            if (use_it == remaining_bucket_uses_by_doc.end()) continue;
            --use_it->second;
            if (use_it->second == 0) {
                encrypted_by_doc.erase(doc_idx);
                remaining_bucket_uses_by_doc.erase(use_it);
            }
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
        auto inst_it = institutions.find(kv.first);
        if (inst_it == institutions.end()) {
            throw std::runtime_error("删除列表机构映射缺失: " + kv.first);
        }
        fs::path out_file = fs::path(cfg.output_root) / (inst_it->second.source_id + "_delete_ids.txt");
        std::ofstream out(out_file);
        if (!out.is_open()) {
            throw std::runtime_error("无法写出删除列表: " + out_file.string());
        }
        for (int doc_idx : kv.second) {
            const auto& rec = docs[doc_idx];
            std::ostringstream line;
            line << rec.uid << "," << rec.location.source_file << "," << rec.location.line_index << "\n";
            out << line.str();
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
        out << "single_institution_bucket_count=" << stats.single_institution_bucket_count << "\n";
        out << "total_bucket_assignments=" << stats.total_bucket_assignments << "\n";
        out << "shared_bucket_doc_assignments=" << stats.shared_bucket_doc_assignments << "\n";
        out << "requested_docs=" << stats.requested_docs << "\n";
        out << "requested_unique_signatures=" << stats.requested_unique_signatures << "\n";
        out << "encrypted_feature_words=" << stats.encrypted_feature_words << "\n";
        out << "duplicate_pairs=" << stats.duplicate_pairs << "\n";
        out << "delete_docs=" << stats.delete_docs << "\n";
        out << "shared_bucket_ratio=" << stats.shared_bucket_ratio << "\n";
        out << "requested_doc_ratio=" << stats.requested_doc_ratio << "\n";
        out << "exposed_signature_ratio=" << stats.exposed_signature_ratio << "\n";
        out << "phase_bucket_submit_s=" << stats.phase_bucket_submit_s << "\n";
        out << "phase_request_encrypt_s=" << stats.phase_request_encrypt_s << "\n";
        out << "phase_oprf_blind_s=" << stats.phase_oprf_blind_s << "\n";
        out << "phase_oprf_kserver_eval_s=" << stats.phase_oprf_kserver_eval_s << "\n";
        out << "phase_oprf_unblind_s=" << stats.phase_oprf_unblind_s << "\n";
        out << "phase_global_match_s=" << stats.phase_global_match_s << "\n";
        out << "phase_distribution_s=" << stats.phase_distribution_s << "\n";
        out << "total_s=" << stats.total_s << "\n";
    }

    return 0;
}

}  // namespace pri

int main(int argc, char** argv) {
    try {
        const pri::PipelineConfig cfg = pri::ParseArgs(argc, argv);
        pri::PipelineStats stats;
        const int rc = pri::RunPipeline(cfg, stats);
        std::cout << "PRI-CPU pipeline finished\n";
        std::cout << "  total_docs: " << stats.total_docs << "\n";
        std::cout << "  shared_bucket_count: " << stats.shared_bucket_count << "\n";
        std::cout << "  requested_docs: " << stats.requested_docs << "\n";
        std::cout << "  requested_doc_ratio: " << stats.requested_doc_ratio << "\n";
        std::cout << "  exposed_signature_ratio: " << stats.exposed_signature_ratio << "\n";
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
