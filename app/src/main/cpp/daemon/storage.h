#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "scanner.h"

namespace eng {

struct AppFileInfo {
    std::string name;
    uint64_t size = 0;
    bool isDir = false;
    std::string perms;
    uint64_t mtime = 0;
};

struct SqliteQueryResult {
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
    std::string summary;
    bool ok = false;
    std::string err;
};

class StorageExplorer {
public:
    static std::string resolvePath(pid_t pid, const std::string &procName, const std::string &subpath);
    static bool listFiles(const std::string &absPath, size_t limit, std::vector<AppFileInfo> &out, std::string &err);
    static bool readFile(const std::string &absPath, size_t maxBytes, size_t offset, std::string &content, size_t &totalSize, bool &isText, std::string &err);
    static bool writeFile(const std::string &absPath, const std::string &data, bool isHex, size_t &written, std::string &err);

    static bool sqliteQuery(const std::string &absPath, const std::string &sql, size_t limit, SqliteQueryResult &out);
    static bool sqliteExec(const std::string &absPath, const std::string &sql, int &changes, std::string &err);
};

} // namespace eng
