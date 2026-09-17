#include "storage.h"

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace eng {

// SQLite3 动态符号表
typedef int (*fn_sqlite3_open)(const char *filename, void **ppDb);
typedef int (*fn_sqlite3_prepare_v2)(void *db, const char *zSql, int nByte, void **ppStmt, const char **pzTail);
typedef int (*fn_sqlite3_step)(void *pStmt);
typedef int (*fn_sqlite3_column_count)(void *pStmt);
typedef const char *(*fn_sqlite3_column_name)(void *pStmt, int N);
typedef const unsigned char *(*fn_sqlite3_column_text)(void *pStmt, int iCol);
typedef int (*fn_sqlite3_finalize)(void *pStmt);
typedef int (*fn_sqlite3_close)(void *db);
typedef int (*fn_sqlite3_changes)(void *db);
typedef const char *(*fn_sqlite3_errmsg)(void *db);

struct SqliteLib {
    void *handle = nullptr;
    fn_sqlite3_open open = nullptr;
    fn_sqlite3_prepare_v2 prepare_v2 = nullptr;
    fn_sqlite3_step step = nullptr;
    fn_sqlite3_column_count column_count = nullptr;
    fn_sqlite3_column_name column_name = nullptr;
    fn_sqlite3_column_text column_text = nullptr;
    fn_sqlite3_finalize finalize = nullptr;
    fn_sqlite3_close close = nullptr;
    fn_sqlite3_changes changes = nullptr;
    fn_sqlite3_errmsg errmsg = nullptr;

    bool init() {
        if (handle) return true;
        handle = dlopen("/system/lib64/libsqlite.so", RTLD_NOW);
        if (!handle) handle = dlopen("libsqlite.so", RTLD_NOW);
        if (!handle) return false;

        open = (fn_sqlite3_open)dlsym(handle, "sqlite3_open");
        prepare_v2 = (fn_sqlite3_prepare_v2)dlsym(handle, "sqlite3_prepare_v2");
        step = (fn_sqlite3_step)dlsym(handle, "sqlite3_step");
        column_count = (fn_sqlite3_column_count)dlsym(handle, "sqlite3_column_count");
        column_name = (fn_sqlite3_column_name)dlsym(handle, "sqlite3_column_name");
        column_text = (fn_sqlite3_column_text)dlsym(handle, "sqlite3_column_text");
        finalize = (fn_sqlite3_finalize)dlsym(handle, "sqlite3_finalize");
        close = (fn_sqlite3_close)dlsym(handle, "sqlite3_close");
        changes = (fn_sqlite3_changes)dlsym(handle, "sqlite3_changes");
        errmsg = (fn_sqlite3_errmsg)dlsym(handle, "sqlite3_errmsg");

        return (open && prepare_v2 && step && column_count && column_name && column_text && finalize && close);
    }
};

static SqliteLib g_sqlite;

std::string StorageExplorer::resolvePath(pid_t pid, const std::string &procName, const std::string &subpath) {
    if (subpath.rfind("/", 0) == 0) return subpath; // 已经是绝对路径

    std::string pkg = procName;
    if (pkg.empty() && pid > 0) {
        char cmdline[64];
        snprintf(cmdline, sizeof(cmdline), "/proc/%d/cmdline", pid);
        FILE *f = fopen(cmdline, "r");
        if (f) {
            char buf[256] = {};
            if (fgets(buf, sizeof(buf), f)) pkg = buf;
            fclose(f);
        }
    }
    // 去除冒号子进程名 (如 com.xxx:push)
    size_t colon = pkg.find(':');
    if (colon != std::string::npos) pkg = pkg.substr(0, colon);

    if (subpath.rfind("ext:", 0) == 0 || subpath.rfind("external:", 0) == 0) {
        size_t cPos = subpath.find(':');
        std::string rel = subpath.substr(cPos + 1);
        if (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
        return "/sdcard/Android/data/" + pkg + "/" + rel;
    }

    if (subpath.rfind("int:", 0) == 0 || subpath.rfind("internal:", 0) == 0) {
        size_t cPos = subpath.find(':');
        std::string rel = subpath.substr(cPos + 1);
        if (!rel.empty() && rel.front() == '/') rel.erase(0, 1);
        return "/data/user/0/" + pkg + "/" + rel;
    }

    // 默认优先内部沙箱 /data/user/0/<pkg>/<subpath>
    std::string internalPath = "/data/user/0/" + pkg + "/" + subpath;
    struct stat st;
    if (stat(internalPath.c_str(), &st) == 0) return internalPath;

    // 尝试外部 /sdcard/Android/data/<pkg>/<subpath>
    std::string externalPath = "/sdcard/Android/data/" + pkg + "/" + subpath;
    if (stat(externalPath.c_str(), &st) == 0) return externalPath;

    return internalPath;
}

bool StorageExplorer::listFiles(const std::string &absPath, size_t limit, std::vector<AppFileInfo> &out, std::string &err) {
    out.clear();
    DIR *dir = opendir(absPath.c_str());
    if (!dir) {
        err = "cannot open directory: " + std::string(strerror(errno));
        return false;
    }

    struct dirent *de;
    while ((de = readdir(dir)) != nullptr) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;

        std::string fullPath = absPath;
        if (fullPath.back() != '/') fullPath += '/';
        fullPath += de->d_name;

        struct stat st = {};
        stat(fullPath.c_str(), &st);

        AppFileInfo fi;
        fi.name = de->d_name;
        fi.size = (uint64_t)st.st_size;
        fi.isDir = S_ISDIR(st.st_mode);
        fi.mtime = (uint64_t)st.st_mtime;

        char permBuf[16];
        snprintf(permBuf, sizeof(permBuf), "%o", st.st_mode & 0777);
        fi.perms = permBuf;

        out.push_back(fi);
        if (limit > 0 && out.size() >= limit) break;
    }
    closedir(dir);

    // 目录优先，然后按字母排序
    std::sort(out.begin(), out.end(), [](const AppFileInfo &a, const AppFileInfo &b) {
        if (a.isDir != b.isDir) return a.isDir > b.isDir;
        return a.name < b.name;
    });

    return true;
}

bool StorageExplorer::readFile(const std::string &absPath, size_t maxBytes, size_t offset,
                              std::string &content, size_t &totalSize, bool &isText, std::string &err) {
    content.clear();
    totalSize = 0;
    isText = true;

    FILE *f = fopen(absPath.c_str(), "rb");
    if (!f) {
        err = "cannot open file: " + std::string(strerror(errno));
        return false;
    }

    fseek(f, 0, SEEK_END);
    totalSize = (size_t)ftell(f);
    fseek(f, (long)offset, SEEK_SET);

    size_t toRead = std::min<size_t>(maxBytes, totalSize > offset ? (totalSize - offset) : 0);
    std::vector<uint8_t> buf(toRead + 1, 0);
    size_t rd = fread(buf.data(), 1, toRead, f);
    fclose(f);

    // 检查是否全为文本/可打印字符
    for (size_t i = 0; i < rd; ++i) {
        uint8_t c = buf[i];
        if (c < 9 || (c > 13 && c < 32)) {
            isText = false;
            break;
        }
    }

    if (isText) {
        content.assign((char *)buf.data(), rd);
    } else {
        char hbuf[4];
        for (size_t i = 0; i < rd; ++i) {
            snprintf(hbuf, sizeof(hbuf), "%02x", buf[i]);
            content += hbuf;
        }
    }

    return true;
}

bool StorageExplorer::writeFile(const std::string &absPath, const std::string &data, bool isHex, size_t &written, std::string &err) {
    written = 0;
    FILE *f = fopen(absPath.c_str(), "wb");
    if (!f) {
        err = "cannot open file for writing: " + std::string(strerror(errno));
        return false;
    }

    if (isHex) {
        std::vector<uint8_t> raw;
        for (size_t i = 0; i + 1 < data.size(); i += 2) {
            char b[3] = { data[i], data[i+1], '\0' };
            raw.push_back((uint8_t)strtoul(b, nullptr, 16));
        }
        written = fwrite(raw.data(), 1, raw.size(), f);
    } else {
        written = fwrite(data.data(), 1, data.size(), f);
    }

    fclose(f);
    return true;
}

bool StorageExplorer::sqliteQuery(const std::string &absPath, const std::string &sql, size_t limit, SqliteQueryResult &out) {
    out = SqliteQueryResult();
    if (!g_sqlite.init()) {
        out.err = "failed to dlopen libsqlite.so";
        return false;
    }

    void *db = nullptr;
    if (g_sqlite.open(absPath.c_str(), &db) != 0 || !db) {
        out.err = "cannot open sqlite database: " + absPath;
        return false;
    }

    void *stmt = nullptr;
    if (g_sqlite.prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != 0 || !stmt) {
        out.err = g_sqlite.errmsg ? g_sqlite.errmsg(db) : "sql prepare failed";
        g_sqlite.close(db);
        return false;
    }

    int colCount = g_sqlite.column_count(stmt);
    for (int i = 0; i < colCount; ++i) {
        const char *cname = g_sqlite.column_name(stmt, i);
        out.columns.push_back(cname ? cname : ("col_" + std::to_string(i)));
    }

    size_t count = 0;
    while (g_sqlite.step(stmt) == 100 /* SQLITE_ROW */) {
        std::vector<std::string> row;
        for (int i = 0; i < colCount; ++i) {
            const unsigned char *txt = g_sqlite.column_text(stmt, i);
            row.push_back(txt ? (const char *)txt : "NULL");
        }
        out.rows.push_back(row);
        if (limit > 0 && ++count >= limit) break;
    }

    g_sqlite.finalize(stmt);
    g_sqlite.close(db);

    // 格式化输出对齐表格
    std::string tbl;
    for (size_t i = 0; i < out.columns.size(); ++i) {
        tbl += "| " + out.columns[i] + " ";
    }
    tbl += "|\n";

    for (size_t i = 0; i < out.columns.size(); ++i) {
        tbl += "|---";
    }
    tbl += "|\n";

    for (const auto &r : out.rows) {
        for (const auto &cell : r) {
            tbl += "| " + cell + " ";
        }
        tbl += "|\n";
    }

    out.summary = tbl;
    out.ok = true;
    return true;
}

bool StorageExplorer::sqliteExec(const std::string &absPath, const std::string &sql, int &changes, std::string &err) {
    changes = 0;
    if (!g_sqlite.init()) {
        err = "failed to dlopen libsqlite.so";
        return false;
    }

    void *db = nullptr;
    if (g_sqlite.open(absPath.c_str(), &db) != 0 || !db) {
        err = "cannot open sqlite database: " + absPath;
        return false;
    }

    void *stmt = nullptr;
    if (g_sqlite.prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != 0 || !stmt) {
        err = g_sqlite.errmsg ? g_sqlite.errmsg(db) : "sql prepare failed";
        g_sqlite.close(db);
        return false;
    }

    g_sqlite.step(stmt);
    changes = g_sqlite.changes ? g_sqlite.changes(db) : 0;
    g_sqlite.finalize(stmt);
    g_sqlite.close(db);

    return true;
}

} // namespace eng
