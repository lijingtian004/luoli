// 数值冻结线程：周期性写回目标值
#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "common.h"

namespace eng {

class Freezer {
public:
    struct Entry {
        VT type;
        uint64_t raw;
        uint32_t intervalMs = 150;
        uint32_t failCount = 0;
        std::chrono::steady_clock::time_point nextDue;
        bool hasGuard = false;
        int64_t guardOffset = 0;
        VT guardType = VT::I32;
        uint64_t guardExpected = 0;
    };

    void start();
    void stop();

    void add(pid_t pid, uint64_t addr, VT type, uint64_t raw, uint32_t intervalMs,
             bool hasGuard = false, int64_t guardOffset = 0, VT guardType = VT::I32, uint64_t guardExpected = 0);
    bool remove(uint64_t addr);
    void removeAll();
    size_t count();
    // 供 server 输出列表
    std::map<uint64_t, Entry> snapshot();

    // write 命令调用：自动解除同地址冻结
    void clearAddr(uint64_t addr);

private:
    void loop();

    std::mutex mu_;
    std::map<uint64_t, Entry> entries_;
    pid_t pid_ = 0;
    bool running_ = false;
    int tickCount_ = 0;
};

} // namespace eng
