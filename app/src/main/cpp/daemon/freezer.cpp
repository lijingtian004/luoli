#include "freezer.h"

#include <chrono>
#include <thread>

#include "driver/twt_driver.h"
#include "logger.h"
#include "scanner.h"

namespace eng {

void Freezer::start() {
    std::lock_guard<std::mutex> lk(mu_);
    if (running_) return;
    running_ = true;
    std::thread([this] { loop(); }).detach();
}

void Freezer::stop() {
    std::lock_guard<std::mutex> lk(mu_);
    running_ = false;
}

void Freezer::add(pid_t pid, uint64_t addr, VT type, uint64_t raw, uint32_t intervalMs,
                  bool hasGuard, int64_t guardOffset, VT guardType, uint64_t guardExpected) {
    std::lock_guard<std::mutex> lk(mu_);
    pid_ = pid;
    Entry e;
    e.type = type;
    e.raw = raw;
    e.intervalMs = intervalMs ? intervalMs : 150;
    e.nextDue = std::chrono::steady_clock::now();
    e.hasGuard = hasGuard;
    e.guardOffset = guardOffset;
    e.guardType = guardType;
    e.guardExpected = guardExpected;
    entries_[addr] = e;
}

bool Freezer::remove(uint64_t addr) {
    std::lock_guard<std::mutex> lk(mu_);
    return entries_.erase(addr) > 0;
}

void Freezer::removeAll() {
    std::lock_guard<std::mutex> lk(mu_);
    entries_.clear();
}

void Freezer::clearAddr(uint64_t addr) { remove(addr); }

size_t Freezer::count() {
    std::lock_guard<std::mutex> lk(mu_);
    return entries_.size();
}

std::map<uint64_t, Freezer::Entry> Freezer::snapshot() {
    std::lock_guard<std::mutex> lk(mu_);
    return entries_;
}

void Freezer::loop() {
    LOGI("freezer thread started");
    while (true) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!running_) break;
            auto now = std::chrono::steady_clock::now();
            pid_t pid = pid_;
            std::vector<uint64_t> dead;
            for (auto &[addr, e] : entries_) {
                if (now < e.nextDue) continue;
                if (g_driver && g_driver->ready()) {
                    bool guardPassed = true;
                    if (e.hasGuard) {
                        uint64_t gVal = 0;
                        size_t gSize = vtSize(e.guardType);
                        if (g_driver->readMemCompat(pid, addr + e.guardOffset, &gVal, gSize)) {
                            uint64_t mask = (gSize >= 8) ? ~0ULL : ((1ULL << (gSize * 8)) - 1);
                            if ((gVal & mask) != (e.guardExpected & mask)) {
                                guardPassed = false;
                            }
                        } else {
                            guardPassed = false;
                        }
                    }

                    if (guardPassed) {
                        if (g_driver->writeMem(pid, addr, &e.raw, vtSize(e.type))) {
                            e.failCount = 0;
                        } else if (++e.failCount >= 10) {
                            dead.push_back(addr);
                        }
                    } else {
                        // 守卫特征校验未通过（GC 搬迁或结构变更）
                        if (++e.failCount >= 10) {
                            dead.push_back(addr);
                        }
                    }
                }
                e.nextDue = now + std::chrono::milliseconds(e.intervalMs);
            }
            for (auto a : dead) {
                char msg[128];
                snprintf(msg, sizeof(msg), "freeze dropped (write failed x10): 0x%llx",
                         (unsigned long long)a);
                LOGW(msg);
                entries_.erase(a);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    LOGI("freezer thread stopped");
}

} // namespace eng
