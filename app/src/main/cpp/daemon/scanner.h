// 扫描引擎：区域解析、分块搜索、增量过滤（批量窗口读）
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <strings.h>
#include <vector>

#include "common.h"

namespace twt { class Driver; }
namespace eng { extern twt::Driver *g_driver; }

namespace eng {

struct Region {
    uint64_t start = 0;
    uint64_t end = 0;      // 排他
    std::string path;      // 可能为空（匿名）
    std::string tag;       // Jh, Ch, A, B, Ca, Cd, S, O, Xa, Xs
    bool writable = false;
    bool readable = true;
    bool executable = false;
};

// 内存段类型分类
inline std::string classifyTag(const std::string &path, bool executable = false) {
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
        path.find("/data/user/") != std::string::npos) {
        return executable ? "Xa" : "Ca";
    }
    if (path.rfind("/", 0) == 0) {
        return executable ? "Xs" : "Cd";
    }
    if (path.rfind("[anon:", 0) == 0) return "A";
    return "O";
}

// 标签与内存权限属性匹配
inline bool matchRegionTag(const Region &rg, const std::string &t) {
    if (strcasecmp(t.c_str(), "all") == 0) return true;
    if (strcasecmp(t.c_str(), "rodata") == 0 || strcasecmp(t.c_str(), "ro") == 0) {
        return rg.readable && !rg.writable && !rg.executable;
    }
    if (strcasecmp(t.c_str(), "code") == 0 || strcasecmp(t.c_str(), "text") == 0) {
        return rg.executable;
    }
    if (strcasecmp(t.c_str(), "data") == 0) {
        return rg.writable;
    }
    if (strcasecmp(t.c_str(), "Ca") == 0) {
        return rg.tag == "Ca" || rg.tag == "Xa";
    }
    if (strcasecmp(t.c_str(), "Cd") == 0) {
        return rg.tag == "Cd" || rg.tag == "Xs";
    }
    return strcasecmp(t.c_str(), rg.tag.c_str()) == 0;
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
    bool writable = true;
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

    // 核心模块信息
    struct CoreModuleInfo {
        std::string name;
        uint64_t base = 0;
        uint64_t end = 0;
        size_t size = 0;
        std::string path;
    };

    // 应用架构与核心特征摘要
    struct AppArchitectureSummary {
        int bitness = 64;
        std::string arch = "AArch64 (64-bit)";
        std::string engine = "Native (C/C++)";
        std::vector<CoreModuleInfo> coreModules;
    };

    static bool inspectArchitecture(pid_t pid, AppArchitectureSummary &out);

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

    // 直接读取连续内存块（不落盘）
    bool readBytes(pid_t pid, uint64_t addr, size_t size, std::vector<uint8_t> &out) const;

    // 结构体多字段读取定义与结果
    struct StructFieldDef {
        std::string name;
        int64_t offset = 0;
        std::string typeStr = "i32";
        VT type = VT::I32;
        bool isPointer = false;
        bool isString = false;
        size_t strLen = 32;
    };

    struct StructFieldValue {
        std::string name;
        int64_t offset = 0;
        uint64_t addr = 0;
        std::string type;
        double numValue = 0.0;
        std::string strValue;
        uint64_t raw = 0;
        bool isPointer = false;
        bool isString = false;
        bool ok = false;
    };

    // 结构体/多字段联动读取
    bool readStruct(pid_t pid, uint64_t baseAddr,
                    const std::vector<StructFieldDef> &fields,
                    std::vector<StructFieldValue> &out) const;

    // 内存临域探查
    bool inspectMemory(pid_t pid, uint64_t centerAddr, int countBefore, int countAfter, size_t unitSize, MemoryInspection &out) const;

    // 群组联合特征扫描
    bool searchGroup(pid_t pid, const GroupScanOptions &opt, std::vector<Hit> &hits,
                     std::mutex &hitsMu, uint64_t &totalOut, bool &truncatedOut);

    // 指针扫描：在指定内存段（支持只读数据段与代码段）中查找指向 targetAddr 的引用
    bool findPointers(pid_t pid, uint64_t targetAddr, int64_t maxOffset, size_t align,
                      const std::vector<std::string> &tags,
                      const std::string &scope,
                      const std::string &nameFilter,
                      std::vector<PointerHit> &out) const;

    // 代码交叉引用命中文本
    struct CodeXRefHit {
        uint64_t pc = 0;
        std::string insnType; // "ADRP+ADD", "ADRP+LDR", "ADRP+STR", "ADR", "LDR_LITERAL", "LITERAL_PTR"
        std::string disasm;   // 反汇编预览
        std::string moduleName; // 相对模块说明，如 "libil2cpp.so+0x1234"
        uint64_t targetAddr = 0;
    };

    // 代码段指令交叉引用反查 (AArch64 PC-Relative XRef Scan)
    bool findCodeXrefs(pid_t pid, uint64_t targetAddr,
                       const std::string &nameFilter,
                       size_t maxResults,
                       std::vector<CodeXRefHit> &out) const;

    // 字符串/文本搜索（支持 utf8 与 utf16le，支持全段与只读段范围过滤）
    bool searchString(pid_t pid, const std::string &text, const std::string &encoding,
                      const std::string &nameFilter, const std::vector<std::string> &tags,
                      const std::string &scope,
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
