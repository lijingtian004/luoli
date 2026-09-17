// 扫描引擎：区域解析、分块搜索、增量过滤（批量窗口读）
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "common.h"

namespace twt { class Driver; }
namespace eng { extern twt::Driver *g_driver; }

namespace eng {

struct Region {
    uint64_t start;
    uint64_t end;      // 排他
    std::string path;  // 可能为空（匿名）
    std::string tag;   // Jh, Ch, A, B, Ca, Cd, S, O
    bool writable;
};

// 内存段类型分类
inline std::string classifyTag(const std::string &path) {
    if (path.empty() || path == "[anon]") return "A";
    if (path == "[heap]") return "Ch";
    if (path.find("bionic_alloc") != std::string::npos ||
        path.find("libc_malloc") != std::string::npos ||
        path.find("scudo") != std::string::npos ||
        path.find("GWP-ASan") != std::string::npos) return "Ch";
    if (path.find("dalvik-main space") != std::string::npos ||
        path.find("dalvik-free list") != std::string::npos ||
        path.find("dalvik-alloc") != std::string::npos ||
        path.find("dalvik-card") != std::string::npos ||
        path.find("dalvik-mod") != std::string::npos ||
        path.find("dalvik-large") != std::string::npos) return "Jh";
    if (path.find("dalvik-") != std::string::npos) {
        if (path.find(".art") != std::string::npos) return "Ja";
        if (path.find("code-cache") != std::string::npos) return "Jc";
        if (path.find("LinearAlloc") != std::string::npos) return "Jl";
        return "Jh";
    }
    if (path.find(".bss") != std::string::npos) return "B";
    if (path == "[stack]" || path.find("stack_and_tls") != std::string::npos ||
        path.find("thread signal stack") != std::string::npos) return "S";
    if (path.find("/data/app/") != std::string::npos ||
        path.find("/data/data/") != std::string::npos ||
        path.find("/data/user/") != std::string::npos) return "Ca";
    if (path.rfind("/", 0) == 0) return "Cd";
    if (path.rfind("[anon:", 0) == 0) return "A";
    return "O";
}

struct ScanProgress {
    std::atomic<bool> running{false};
    std::atomic<uint64_t> bytesDone{0};
    std::atomic<uint64_t> bytesTotal{0};
    std::atomic<uint64_t> found{0};
    std::atomic<int> regionIndex{-1};
};

struct ScanOptions {
    VT type = VT::I32;
    uint64_t rawTarget = 0;       // 目标位模式
    bool hasTarget = false;       // 是否设置了目标数值
    size_t align = 4;
    std::string nameFilter;       // 区域路径子串过滤
    std::vector<std::string> tags; // 区域分类标签过滤 (如 ["Jh", "Ch"])
    uint64_t maxResults = 1000000;
    uint64_t rateLimitBytesPerSec = 128ull * 1024 * 1024;  // 隐身：IO 限速
};

// 内存探查词条
struct MemoryWord {
    int64_t offset = 0;
    uint64_t addr = 0;
    uint64_t raw = 0;
    int32_t i32 = 0;
    uint32_t u32 = 0;
    float f32 = 0.0f;
    int64_t i64 = 0;
    double f64 = 0.0;
    std::string hex;
    std::string ascii;
    bool isTarget = false;
};

struct MemoryInspection {
    uint64_t centerAddr = 0;
    std::vector<MemoryWord> words;
    std::string summary;
    bool ok = false;
};

// 群组特征扫描子项
struct GroupPatternItem {
    int64_t offset = 0;
    VT type = VT::I32;
    uint64_t rawTarget = 0;
};

struct GroupScanOptions {
    std::vector<GroupPatternItem> items;
    size_t baseAlign = 4;
    std::string nameFilter;
    std::vector<std::string> tags;
    uint64_t maxResults = 100000;
};

// 指针引用命中文本
struct PointerHit {
    uint64_t pointerAddr = 0;
    uint64_t pointsTo = 0;
    int64_t offset = 0;
    std::string regionTag;
    std::string moduleName;
};

// 多级指针链解析步骤
struct PointerChainStep {
    int64_t offset = 0;
    uint64_t addr = 0;
    uint64_t value = 0;
    bool ok = false;
};

struct PointerChainResult {
    uint64_t baseAddr = 0;
    std::vector<PointerChainStep> steps;
    uint64_t finalAddr = 0;
    uint64_t finalValue = 0;
    bool ok = false;
    std::string err;
};

// AArch64 反汇编行
struct DisasmLine {
    uint64_t addr = 0;
    uint32_t raw = 0;
    std::string hex;
    std::string mnemonic;
    std::string operands;
    std::string symbol;
};

enum class FilterOp {
    EQ, NE, GT, GE, LT, LE, INC, DEC, CHANGED, UNCHANGED,
};

inline bool filterOpFromString(const std::string &s, FilterOp &out) {
    if (s == "eq") out = FilterOp::EQ;
    else if (s == "ne") out = FilterOp::NE;
    else if (s == "gt") out = FilterOp::GT;
    else if (s == "ge") out = FilterOp::GE;
    else if (s == "lt") out = FilterOp::LT;
    else if (s == "le") out = FilterOp::LE;
    else if (s == "inc") out = FilterOp::INC;
    else if (s == "dec") out = FilterOp::DEC;
    else if (s == "changed") out = FilterOp::CHANGED;
    else if (s == "unchanged") out = FilterOp::UNCHANGED;
    else return false;
    return true;
}

class Scanner {
public:
    // 解析 /proc/<pid>/maps
    static bool listRegions(pid_t pid, bool writableOnly, std::vector<Region> &out);

    // 融合同路径连续区域后的模块列表
    struct Module { std::string name; uint64_t base; uint64_t end; };
    static bool listModules(pid_t pid, std::vector<Module> &out);

    Scanner() = default;

    void setRateLimit(uint64_t bytesPerSec) { rate_ = bytesPerSec ? bytesPerSec : 1; }

    // 阻塞式初扫；结果追加到 hits（内部持锁）。返回是否成功
    bool search(pid_t pid, const ScanOptions &opt, std::vector<Hit> &hits,
                std::mutex &hitsMu, uint64_t &totalOut, bool &truncatedOut);

    // 增量过滤（重读当前值并与上次 raw 比较）；原地更新 hits
    bool filter(pid_t pid, const ScanOptions &opt, FilterOp op,
                std::vector<Hit> &hits, std::mutex &hitsMu,
                uint64_t &totalOut);

    bool singleRead(pid_t pid, uint64_t addr, VT type, uint64_t &rawOut) const;
    bool singleWrite(pid_t pid, uint64_t addr, VT type, uint64_t raw) const;

    // 内存临域探查
    bool inspectMemory(pid_t pid, uint64_t centerAddr, int countBefore, int countAfter, size_t unitSize, MemoryInspection &out) const;

    // 群组联合特征扫描
    bool searchGroup(pid_t pid, const GroupScanOptions &opt, std::vector<Hit> &hits,
                     std::mutex &hitsMu, uint64_t &totalOut, bool &truncatedOut);

    // 指针扫描：在指定内存段中查找指向 targetAddr 的引用
    bool findPointers(pid_t pid, uint64_t targetAddr, int64_t maxOffset, size_t align,
                      const std::vector<std::string> &tags, std::vector<PointerHit> &out) const;

    // 字符串/文本搜索（支持 utf8 与 utf16le）
    bool searchString(pid_t pid, const std::string &text, const std::string &encoding,
                      const std::string &nameFilter, const std::vector<std::string> &tags,
                      uint64_t maxResults, std::vector<Hit> &hits, std::mutex &hitsMu,
                      uint64_t &totalOut, bool &truncatedOut);

    // 通配符字节特征码扫描（AOB / Hex Pattern Scan，例如 "1F 20 03 D5 ?? ?? ?? 94"）
    bool searchPattern(pid_t pid, const std::string &patternStr, size_t align,
                       const std::string &nameFilter, const std::vector<std::string> &tags,
                       uint64_t maxResults, std::vector<Hit> &hits, std::mutex &hitsMu,
                       uint64_t &totalOut, bool &truncatedOut);

    // 多级指针链解析
    bool resolvePointerChain(pid_t pid, const std::string &baseSpec,
                             const std::vector<int64_t> &offsets, size_t ptrSize,
                             VT finalType, PointerChainResult &out) const;

    // AArch64 指令反汇编
    bool disassemble(pid_t pid, uint64_t addr, size_t count, std::vector<DisasmLine> &out) const;

    // 内存与模块转储
    bool dumpMemory(pid_t pid, uint64_t addr, size_t size, const std::string &outPath, size_t &writtenOut, std::string &err) const;
    bool dumpModule(pid_t pid, const std::string &moduleName, const std::string &outPath, size_t &writtenOut, std::string &err) const;

    ScanProgress progress;

private:
    // 批量读（窗口聚合），失败自动回退小块/单值
    bool readRange(pid_t pid, uint64_t addr, uint8_t *buf, size_t size) const;
    void rateWait(uint64_t bytes);

    std::atomic<uint64_t> rate_{128ull * 1024 * 1024};
    std::atomic<int64_t> nextTickNs_{0};
    std::mutex rateMu_;
};

} // namespace eng
