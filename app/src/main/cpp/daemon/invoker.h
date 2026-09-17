#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <sys/types.h>

namespace eng {

struct InvocationResult {
    uint64_t retVal = 0;
    std::string retHex;
    bool ok = false;
    std::string err;
};

class RemoteInvoker {
public:
    static bool call(pid_t pid, uint64_t funcAddr, const std::vector<uint64_t> &args,
                     InvocationResult &out, uint32_t timeoutMs = 2000);
};

} // namespace eng
