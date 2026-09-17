#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <sys/types.h>

namespace eng {

struct CrashReport {
    bool hasCrash = false;
    int pid = 0;
    std::string processName;
    std::string signal;
    std::string faultAddr;
    std::string pc;
    std::string lr;
    std::string sp;
    std::vector<std::string> backtrace;
    std::string rawSummary;
};

class CrashTriage {
public:
    static bool getLatestCrash(pid_t targetPid, const std::string &targetPkg, CrashReport &out);
};

} // namespace eng
