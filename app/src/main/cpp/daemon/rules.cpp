#include "rules.h"

#include <thread>
#include "logger.h"

namespace eng {

RuleEngine::RuleEngine(Scanner &scanner) : scanner_(scanner) {}

RuleEngine::~RuleEngine() {
    stop();
}

void RuleEngine::start() {
    std::lock_guard<std::mutex> lk(mu_);
    if (running_) return;
    running_ = true;
    std::thread([this] { loop(); }).detach();
}

void RuleEngine::stop() {
    running_ = false;
}

uint64_t RuleEngine::addRule(pid_t pid, const std::string &name, const std::string &base,
                             const std::vector<int64_t> &offsets, size_t ptrSize, VT type,
                             const std::string &action, uint64_t targetBits, uint32_t intervalMs,
                             bool hasSource2, const std::string &base2,
                             const std::vector<int64_t> &offsets2, size_t ptrSize2,
                             VT type2, double ratio, double offsetVal) {
    std::lock_guard<std::mutex> lk(mu_);
    pid_ = pid;
    Rule r;
    r.id = nextId_++;
    r.name = name.empty() ? ("rule_" + std::to_string(r.id)) : name;
    r.base = base;
    r.offsets = offsets;
    r.ptrSize = ptrSize;
    r.type = type;
    r.action = action.empty() ? "lock" : action;
    r.targetBits = targetBits;
    r.intervalMs = intervalMs ? intervalMs : 150;
    r.nextDue = std::chrono::steady_clock::now();
    r.active = true;

    r.hasSource2 = hasSource2;
    r.base2 = base2;
    r.offsets2 = offsets2;
    r.ptrSize2 = ptrSize2;
    r.type2 = type2;
    r.ratio = ratio;
    r.offsetVal = offsetVal;

    rules_[r.id] = r;
    return r.id;
}

bool RuleEngine::removeRule(uint64_t id) {
    std::lock_guard<std::mutex> lk(mu_);
    return rules_.erase(id) > 0;
}

void RuleEngine::removeAll() {
    std::lock_guard<std::mutex> lk(mu_);
    rules_.clear();
}

std::vector<Rule> RuleEngine::listRules() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Rule> list;
    list.reserve(rules_.size());
    for (const auto &[id, r] : rules_) {
        list.push_back(r);
    }
    return list;
}

void RuleEngine::loop() {
    LOGI("rule engine thread started");
    while (running_) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!running_) break;
            auto now = std::chrono::steady_clock::now();
            pid_t pid = pid_;

            for (auto &[id, r] : rules_) {
                if (!r.active || now < r.nextDue) continue;

                PointerChainResult res;
                if (scanner_.resolvePointerChain(pid, r.base, r.offsets, r.ptrSize, r.type, res) && res.ok) {
                    r.lastResolvedAddr = res.finalAddr;
                    r.lastValue = res.finalValue;
                    r.failCount = 0;

                    uint64_t effectiveTarget = r.targetBits;

                    // RuleEngine 2.0: 若配置了第二数据源，计算动态联动值
                    if (r.hasSource2) {
                        PointerChainResult res2;
                        if (scanner_.resolvePointerChain(pid, r.base2, r.offsets2, r.ptrSize2, r.type2, res2) && res2.ok) {
                            r.lastResolvedAddr2 = res2.finalAddr;
                            r.lastValue2 = res2.finalValue;
                            double v2 = bitsToDouble(r.type2, res2.finalValue);
                            double computed = v2 * r.ratio + r.offsetVal;
                            effectiveTarget = doubleToBits(r.type, computed);
                        } else {
                            continue; // 第二数据源未就绪，等待下一次循环
                        }
                    }

                    if (r.action == "lock" || r.action == "freeze" || r.action == "ratio_lock") {
                        scanner_.singleWrite(pid, res.finalAddr, r.type, effectiveTarget);
                    } else if (r.action == "clamp_min" || r.action == "clamp_min_ratio") {
                        if (cmpValues(r.type, res.finalValue, effectiveTarget) < 0) {
                            scanner_.singleWrite(pid, res.finalAddr, r.type, effectiveTarget);
                        }
                    } else if (r.action == "clamp_max" || r.action == "clamp_max_ratio") {
                        if (cmpValues(r.type, res.finalValue, effectiveTarget) > 0) {
                            scanner_.singleWrite(pid, res.finalAddr, r.type, effectiveTarget);
                        }
                    }
                } else {
                    r.failCount++;
                }

                r.nextDue = now + std::chrono::milliseconds(r.intervalMs);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    LOGI("rule engine thread stopped");
}

} // namespace eng
