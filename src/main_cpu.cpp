#include "lsh_cpu.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

namespace {

void PrintUsage() {
    std::cout << "用法:\n"
              << "  fed_lsh_cpu <输入目录> <输出目录> [--keep-hash] "
              << "[--num-hash 128] [--bands 16] [--shingle-len 5] "
              << "[--num-key 10240] [--threshold 0.8] [--min-text-len 0]\n";
}

}

int main(int argc, char** argv) {
    if (argc < 3) {
        PrintUsage();
        return 1;
    }

    fs::path input_dir = argv[1];
    fs::path output_dir = argv[2];
    fed_cpu::LshConfig cfg;

    try {
        for (int i = 3; i < argc; ++i) {
            const std::string arg = argv[i];
            auto require_value = [&](const std::string& key) -> std::string {
                if (i + 1 >= argc) {
                    throw std::runtime_error("参数缺少取值: " + key);
                }
                return argv[++i];
            };

            if (arg == "--keep-hash") {
                continue;
            } else if (arg == "--num-hash") {
                cfg.num_hash = std::stoi(require_value(arg));
            } else if (arg == "--bands") {
                cfg.bands = std::stoi(require_value(arg));
            } else if (arg == "--shingle-len") {
                cfg.shingle_len = std::stoi(require_value(arg));
            } else if (arg == "--num-key") {
                cfg.num_key = std::stoi(require_value(arg));
            } else if (arg == "--threshold") {
                cfg.threshold = std::stod(require_value(arg));
            } else if (arg == "--min-text-len") {
                cfg.min_text_len = std::stoi(require_value(arg));
            } else {
                throw std::runtime_error("未知参数: " + arg);
            }
        }

        const fed_cpu::LshStats stats = fed_cpu::RunDirectory(input_dir, output_dir, cfg);
        std::cout << "FED CPU LSH finished\n";
        std::cout << "  files: " << stats.files << "\n";
        std::cout << "  docs: " << stats.docs << "\n";
        std::cout << "  local_duplicate_pairs: " << stats.local_duplicate_pairs << "\n";
        std::cout << "  local_delete_docs: " << stats.local_delete_docs << "\n";
        std::cout << "Min Hash total time: " << stats.minhash_s << " seconds\n";
        std::cout << "Local dedup time: " << stats.local_dedup_s << " seconds\n";
        std::cout << "Total time: " << stats.total_s << " seconds\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[FED-CPU] error: " << e.what() << "\n";
        PrintUsage();
        return 1;
    }
}
