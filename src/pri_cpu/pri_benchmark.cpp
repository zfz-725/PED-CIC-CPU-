#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct BenchConfig {
    std::string fed_bin;
    std::string pri_bin;
    std::string global_input;
    std::string inst_root;
    std::string work_root;
    int mpi_np = 1;
    std::string mode = "sha";
};

std::vector<fs::path> SortedSubDirs(const fs::path& root) {
    std::vector<fs::path> dirs;
    for (const auto& entry : fs::directory_iterator(root)) {
        if (entry.is_directory()) dirs.push_back(entry.path());
    }
    std::sort(dirs.begin(), dirs.end());
    return dirs;
}

void EnsureDir(const fs::path& p) {
    if (!fs::exists(p)) fs::create_directories(p);
}

double RunCommandAndMeasure(const std::string& cmd) {
    using clock = std::chrono::high_resolution_clock;
    const auto t1 = clock::now();
    const int ret = std::system(cmd.c_str());
    const auto t2 = clock::now();
    if (ret != 0) {
        throw std::runtime_error("命令执行失败: " + cmd);
    }
    return std::chrono::duration<double>(t2 - t1).count();
}

std::string MpiPrefix(int np) {
    return "OMPI_ALLOW_RUN_AS_ROOT=1 OMPI_ALLOW_RUN_AS_ROOT_CONFIRM=1 mpirun --allow-run-as-root -np " +
           std::to_string(np);
}

BenchConfig ParseArgs(int argc, char** argv) {
    BenchConfig cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto v = [&](const std::string& k) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("参数缺失: " + k);
            return argv[++i];
        };
        if (arg == "--fed-bin") cfg.fed_bin = v(arg);
        else if (arg == "--pri-bin") cfg.pri_bin = v(arg);
        else if (arg == "--global-input") cfg.global_input = v(arg);
        else if (arg == "--inst-root") cfg.inst_root = v(arg);
        else if (arg == "--work-root") cfg.work_root = v(arg);
        else if (arg == "--np") cfg.mpi_np = std::stoi(v(arg));
        else if (arg == "--mode") cfg.mode = v(arg);
        else throw std::runtime_error("未知参数: " + arg);
    }
    if (cfg.fed_bin.empty() || cfg.pri_bin.empty() || cfg.global_input.empty() ||
        cfg.inst_root.empty() || cfg.work_root.empty()) {
        throw std::runtime_error("必须提供 --fed-bin --pri-bin --global-input --inst-root --work-root");
    }
    return cfg;
}

void PrintUsage() {
    std::cout << "用法:\n"
              << "  pri_cpu_benchmark --fed-bin <FED可执行文件> --pri-bin <PRI可执行文件> "
              << "--global-input <全局目录> --inst-root <机构目录根> --work-root <工作目录> "
              << "[--np 1] [--mode sha|oprf]\n";
}

}

int main(int argc, char** argv) {
    try {
        const BenchConfig cfg = ParseArgs(argc, argv);
        const fs::path work_root(cfg.work_root);
        const fs::path baseline_out = work_root / "baseline_out";
        const fs::path pri_lsh_root = work_root / "pri_lsh";
        const fs::path pri_out = work_root / "pri_out";
        EnsureDir(work_root);
        EnsureDir(baseline_out);
        EnsureDir(pri_lsh_root);
        EnsureDir(pri_out);

        std::cout << "[1/3] 运行原始 FED 全量基线...\n";
        const std::string mpi_prefix = MpiPrefix(cfg.mpi_np);
        const std::string baseline_cmd =
            mpi_prefix + " " + cfg.fed_bin + " " +
            cfg.global_input + " " + baseline_out.string();
        const double baseline_time = RunCommandAndMeasure(baseline_cmd);

        std::cout << "[2/3] 逐机构运行 FED 生成本地哈希结果...\n";
        double local_fed_time = 0.0;
        for (const auto& inst : SortedSubDirs(cfg.inst_root)) {
            const fs::path out = pri_lsh_root / inst.filename();
            EnsureDir(out);
            const std::string cmd =
                mpi_prefix + " " + cfg.fed_bin + " " +
                inst.string() + " " + out.string();
            local_fed_time += RunCommandAndMeasure(cmd);
        }

        std::cout << "[3/3] 运行 PRI 协同去重...\n";
        const std::string pri_cmd =
            cfg.pri_bin + " --inst-root " + cfg.inst_root + " --lsh-root " + pri_lsh_root.string() +
            " --out " + pri_out.string() + " --mode " + cfg.mode;
        const double pri_time = RunCommandAndMeasure(pri_cmd);

        const double federated_total = local_fed_time + pri_time;
        const double overhead = (federated_total - baseline_time) / baseline_time * 100.0;

        std::cout << "========================================\n";
        std::cout << "Baseline FED Time(s): " << baseline_time << "\n";
        std::cout << "PRI Local FED Time(s): " << local_fed_time << "\n";
        std::cout << "PRI Coordinator Time(s): " << pri_time << "\n";
        std::cout << "PRI Total Time(s): " << federated_total << "\n";
        std::cout << "Overhead vs FED(%): " << overhead << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[PRI-CPU-BENCH] error: " << e.what() << "\n";
        PrintUsage();
        return 1;
    }
}
