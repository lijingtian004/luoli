#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "scanner.h"

namespace eng {

struct Il2CppApiInfo {
    std::string name;
    uint64_t addr = 0;
};

struct Il2CppFieldInfo {
    std::string name;
    std::string type;
    int32_t offset = 0;
};

struct Il2CppClassInfo {
    std::string name;
    std::string namespaze;
    uint32_t token = 0;
    std::vector<Il2CppFieldInfo> fields;
    bool ok = false;
};

struct Il2CppStatus {
    bool detected = false;
    uint64_t moduleBase = 0;
    uint64_t moduleEnd = 0;
    size_t moduleSize = 0;
    std::string modulePath;
    bool hasMetadata = false;
    uint64_t metadataAddr = 0;
    int32_t metadataVersion = 0;
    size_t apiCount = 0;
};

class Il2CppInspector {
public:
    Il2CppInspector(Scanner &scanner);

    bool getStatus(pid_t pid, Il2CppStatus &out) const;
    bool listApis(pid_t pid, std::vector<Il2CppApiInfo> &out) const;
    bool inspectClass(pid_t pid, const std::string &className, std::vector<Il2CppClassInfo> &out) const;

private:
    Scanner &scanner_;
};

} // namespace eng
