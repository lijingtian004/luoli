#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <map>
#include "common.h"
#include "scanner.h"

namespace eng {

struct Rule {
    uint64_t id = 0;
    std::string name;
    std::string base;               // 基址说明，如 "0x2986570" 或 "libil2cpp.so+0x1234"
    std::vector<int64_t> offsets;   // 多级指针偏移链，如 [0, 12]
    size_t ptrSize = 4;             // 指针大小：4 (压缩引用) 或 8
    VT type = VT::I32;              // 目标值类型
    std::string action = "lock";    // "lock", "clamp_min", "clamp_max"
    uint64_t targetBits = 0;        // 目标写入/限制位模式
    uint32_t intervalMs = 150;      // 检查周期
    uint64_t lastResolvedAddr = 0;  // 最近一次成功解析的目标绝对地址
    uint64_t lastValue = 0;         // 最近一次观察到的值
    uint32_t failCount = 0;         // 连续解析失败次数
    bool active = true;             // 是否激活

    // RuleEngine 2.0: 动态多变量联动
    bool hasSource2 = false;
    std::string base2;
    std::vector<int64_t> offsets2;
    size_t ptrSize2 = 4;
    VT type2 = VT::I32;
    double ratio = 1.0;
    double offsetVal = 0.0;
    uint64_t lastResolvedAddr2 = 0;
    uint64_t lastValue2 = 0;

    std::chrono::steady_clock::time_point nextDue;
};

class RuleEngine {
public:
    RuleEngine(Scanner &scanner);
    ~RuleEngine();

    void start();
    void stop();

    uint64_t addRule(pid_t pid, const std::string &name, const std::string &base,
                     const std::vector<int64_t> &offsets, size_t ptrSize, VT type,
                     const std::string &action, uint64_t targetBits, uint32_t intervalMs,
                     bool hasSource2 = false, const std::string &base2 = "",
                     const std::vector<int64_t> &offsets2 = {}, size_t ptrSize2 = 4,
                     VT type2 = VT::I32, double ratio = 1.0, double offsetVal = 0.0);

    bool removeRule(uint64_t id);
    void removeAll();
    std::vector<Rule> listRules();

private:
    void loop();

    Scanner &scanner_;
    std::mutex mu_;
    std::map<uint64_t, Rule> rules_;
    pid_t pid_ = 0;
    std::atomic<bool> running_{false};
    uint64_t nextId_{1};
};

} // namespace eng
