#include "server.h"

#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "logger.h"
#include "storage.h"
#include "invoker.h"
#include "crash.h"

using json = nlohmann::json;

namespace eng {

// ---------- socket 工具 ----------

static int createAbstractListener(const std::string &name) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    strncpy(addr.sun_path + 1, name.c_str(), sizeof(addr.sun_path) - 2);
    socklen_t len = (socklen_t)(sizeof(addr.sun_family) + 1 + name.size());
    if (bind(fd, (sockaddr *)&addr, len) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// 读写完整帧：[u32 LE 长度][json]
static bool readFrame(int fd, std::string &out) {
    uint32_t len = 0;
    size_t got = 0;
    auto rd = [&](void *buf, size_t n) -> bool {
        uint8_t *p = (uint8_t *)buf;
        size_t done = 0;
        while (done < n) {
            ssize_t r = ::read(fd, p + done, n - done);
            if (r > 0) { done += (size_t)r; continue; }
            if (r < 0 && errno == EINTR) continue;
            return false;
        }
        return true;
    };
    if (!rd(&len, 4)) return false;
    if (len == 0 || len > kMaxFrameBytes) return false;
    out.resize(len);
    return rd(out.data(), len);
}

static bool writeFrame(int fd, const std::string &data) {
    uint32_t len = (uint32_t)data.size();
    size_t done = 0;
    const uint8_t *p = (const uint8_t *)&len;
    size_t n = 4;
    while (done < n) {
        ssize_t w = ::write(fd, p + done, n - done);
        if (w > 0) { done += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        return false;
    }
    done = 0;
    p = (const uint8_t *)data.data();
    n = data.size();
    while (done < n) {
        ssize_t w = ::write(fd, p + done, n - done);
        if (w > 0) { done += (size_t)w; continue; }
        if (w < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

// ---------- 构造/析构 ----------

Server::Server(twt::Driver &driver) : driver_(driver) {
    g_driver = &driver;
}

Server::~Server() {
    if (listenFd_ >= 0) close(listenFd_);
}

bool Server::run(const std::string &socketName) {
    listenFd_ = createAbstractListener(socketName);
    if (listenFd_ < 0) {
        LOGE(std::string("listen failed: ") + strerror(errno));
        return false;
    }
    LOGI("listening on abstract @" + socketName);
    freezer_.start();
    ruleEngine_.start();

    while (!stopping_.load()) {
        int cfd = accept4(listenFd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        std::thread([this, cfd] { handleConnection(cfd); }).detach();
    }
    ruleEngine_.stop();
    freezer_.stop();
    return true;
}

void Server::handleConnection(int fd) {
    std::string req;
    while (!stopping_.load()) {
        if (!readFrame(fd, req)) break;
        std::string resp = dispatch(req);
        if (!writeFrame(fd, resp)) break;
    }
    close(fd);
}

// ---------- 命令实现辅助 ----------

static json okResp(const json &id, json data = json::object()) {
    data["ok"] = true;
    if (!id.is_null()) data["id"] = id;
    return data;
}

static json errResp(const json &id, const std::string &msg) {
    json j;
    j["ok"] = false;
    j["err"] = msg;
    if (!id.is_null()) j["id"] = id;
    return j;
}

static bool parseAddr(const json &j, uint64_t &out) {
    // 支持 "addr":"0x..." 字符串或数字
    if (j.is_string()) {
        const std::string s = j.get<std::string>();
        if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0)
            out = strtoull(s.c_str() + 2, nullptr, 16);
        else
            out = strtoull(s.c_str(), nullptr, 0);
        return true;
    }
    if (j.is_number_unsigned()) { out = j.get<uint64_t>(); return true; }
    if (j.is_number_integer()) { out = (uint64_t)j.get<int64_t>(); return true; }
    return false;
}

static std::string readCmdlineName(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    FILE *f = fopen(path, "r");
    if (!f) return "";
    char buf[256] = {};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return "";
    std::string s(buf, strnlen(buf, n));
    // 包名形式取整体；路径形式取 basename
    auto slash = s.rfind('/');
    if (slash != std::string::npos && s.find(' ') == std::string::npos)
        s = s.substr(slash + 1);
    return s;
}

// ---------- 命令分发 ----------

std::string Server::dispatch(const std::string &reqStr) {
    json req;
    try {
        req = json::parse(reqStr);
    } catch (...) {
        return errResp(nullptr, "bad json").dump();
    }
    if (!req.is_object() || !req.contains("cmd") || !req["cmd"].is_string())
        return errResp(nullptr, "missing cmd").dump();

    const std::string cmd = req["cmd"].get<std::string>();
    const json id = req.contains("id") ? req["id"] : json(nullptr);

    // ---- 无需附加进程的命令 ----
    if (cmd == "ping") {
        return okResp(id, json{
            {"version", kDaemonVersion},
            {"daemon_pid", (int64_t)getpid()},
            {"driver_ready", driver_.ready()},
            {"driver_mode", driver_.mode()},
        }).dump();
    }

    if (cmd == "driver_status") {
        bool selfTest = false;
        if (driver_.ready()) {
            // 自检：读自身内存
            uint64_t probe = 0;
            selfTest = driver_.readMem(getpid(), (uintptr_t)&probe, &probe, sizeof(probe)) &&
                       probe == (uint64_t)(uintptr_t)&probe;
        }
        return okResp(id, json{
            {"ready", driver_.ready()},
            {"mode", driver_.mode()},
            {"self_test", selfTest},
        }).dump();
    }

    if (cmd == "list_processes") {
        std::string filter = req.value("filter", "");
        std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
        bool all = req.value("all", false);
        size_t limit = req.value("limit", (int64_t)25);

        std::vector<json> arr;
        DIR *dir = opendir("/proc");
        if (!dir) return errResp(id, "opendir /proc failed").dump();
        struct dirent *de;
        while ((de = readdir(dir)) != nullptr) {
            if (!isdigit((unsigned char)de->d_name[0])) continue;
            pid_t p = (pid_t)atoi(de->d_name);
            if (p == getpid()) continue;
            // 只列 App 进程（uid >= 10000），过滤系统噪音
            char statPath[64];
            snprintf(statPath, sizeof(statPath), "/proc/%d/status", p);
            FILE *sf = fopen(statPath, "r");
            if (!sf) continue;
            char line[256];
            int uid = -1;
            char comm[128] = {};
            while (fgets(line, sizeof(line), sf)) {
                if (!strncmp(line, "Uid:", 4)) sscanf(line + 4, "%d", &uid);
                else if (!strncmp(line, "Name:", 5)) sscanf(line + 5, "%127s", comm);
                if (uid >= 0 && comm[0]) break;
            }
            fclose(sf);
            if (uid < 10000) continue;

            std::string name = readCmdlineName(p);
            if (name.empty()) name = comm;

            if (!filter.empty()) {
                std::string lowerName = name;
                std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
                if (lowerName.find(filter) == std::string::npos) continue;
            }

            // 检查是否为前台/活跃进程 (cgroup cpuset:/foreground 或 top-app)
            bool isForeground = false;
            char cgroupPath[64];
            snprintf(cgroupPath, sizeof(cgroupPath), "/proc/%d/cgroup", p);
            FILE *cgf = fopen(cgroupPath, "r");
            if (cgf) {
                char cgLine[256];
                while (fgets(cgLine, sizeof(cgLine), cgf)) {
                    if (strstr(cgLine, "top-app") || strstr(cgLine, "cpuset:/foreground")) {
                        isForeground = true;
                        break;
                    }
                }
                fclose(cgf);
            }

            arr.push_back(json{
                {"pid", (int64_t)p},
                {"name", name},
                {"uid", uid},
                {"foreground", isForeground},
            });
        }
        closedir(dir);

        // 前台应用排在最前面
        std::sort(arr.begin(), arr.end(), [](const json &a, const json &b) {
            bool fa = a.value("foreground", false);
            bool fb = b.value("foreground", false);
            if (fa != fb) return fa > fb;
            return a["pid"].get<int64_t>() < b["pid"].get<int64_t>();
        });

        if (!all && arr.size() > limit) {
            arr.resize(limit);
        }

        return okResp(id, json{{"total", (int64_t)arr.size()}, {"processes", arr}}).dump();
    }

    if (cmd == "logs") {
        uint64_t after = 0;
        if (req.contains("after")) after = req["after"].get<uint64_t>();
        auto lines = Logger::get().since(after);
        std::vector<json> arr;
        for (auto &l : lines) arr.push_back(json{{"seq", l.seq}, {"text", l.text}});
        return okResp(id, json{{"lines", arr}}).dump();
    }

    if (cmd == "stop") {
        stopping_.store(true);
        shutdown(listenFd_, SHUT_RDWR);
        return okResp(id).dump();
    }

    if (cmd == "set_guard") {
        std::string pkg = req.value("package", "com.luoli.modifier");
        if (pkg.empty()) pkg = "com.luoli.modifier";
        guard_.start(pkg);
        return okResp(id, json{{"guard_running", guard_.running()}}).dump();
    }

    if (cmd == "stop_guard") {
        guard_.stop();
        return okResp(id).dump();
    }

    if (cmd == "stealth_hide") {
        if (!driver_.ready()) return errResp(id, "driver not ready").dump();
        std::string dir = req.value("directory", "/data/user/0");
        std::string kw = req.value("keyword", "luoli");
        bool ok = driver_.fileHideSet(dir.c_str(), kw.c_str());
        return okResp(id, json{{"ok", ok}, {"directory", dir}, {"keyword", kw}}).dump();
    }

    if (cmd == "stealth_clear") {
        if (!driver_.ready()) return errResp(id, "driver not ready").dump();
        bool ok = driver_.fileHideClear();
        return okResp(id, json{{"ok", ok}, {"cleared", true}}).dump();
    }

    if (cmd == "stealth_status") {
        if (!driver_.ready()) return errResp(id, "driver not ready").dump();
        twt::twt_file_hide_status st = {};
        bool ok = driver_.fileHideStatus(&st);
        return okResp(id, json{
            {"ok", ok},
            {"enabled", (bool)st.enabled},
            {"hook_active", (bool)st.hook_active},
            {"directory", st.directory},
            {"keyword", st.keyword}
        }).dump();
    }

    // ---- 以下命令需要附加 ----
    if (cmd == "attach" || cmd == "attach_by_name") {
        pid_t p = 0;
        if (cmd == "attach") {
            if (!req.contains("pid")) return errResp(id, "missing pid").dump();
            p = (pid_t)req["pid"].get<int64_t>();
        } else {
            if (!req.contains("name")) return errResp(id, "missing name").dump();
            std::string name = req["name"].get<std::string>();
            p = driver_.ready() ? driver_.getPidByName(name.c_str()) : -1;
            if (p <= 0) {
                // 兜底：遍历 /proc 匹配 cmdline
                DIR *dir = opendir("/proc");
                if (dir) {
                    struct dirent *de;
                    while ((de = readdir(dir)) != nullptr) {
                        if (!isdigit((unsigned char)de->d_name[0])) continue;
                        pid_t c = (pid_t)atoi(de->d_name);
                        std::string n = readCmdlineName(c);
                        if (n == name) { p = c; break; }
                    }
                    closedir(dir);
                }
            }
        }
        if (p <= 0) return errResp(id, "process not found").dump();

        // 校验 maps 可读
        std::vector<Region> probe;
        if (!Scanner::listRegions(p, false, probe) || probe.empty())
            return errResp(id, "cannot read maps (permission?)").dump();

        std::lock_guard<std::mutex> lk(stateMu_);
        pid_ = p;
        procName_ = readCmdlineName(p);
        hits_.clear();
        freezer_.removeAll();
        LOGI("attached pid " + std::to_string(p) + " (" + procName_ + ")");
        return okResp(id, json{{"pid", (int64_t)p}, {"name", procName_}}).dump();
    }

    if (cmd == "detach") {
        std::lock_guard<std::mutex> lk(stateMu_);
        LOGI("detached pid " + std::to_string(pid_));
        pid_ = 0;
        procName_.clear();
        hits_.clear();
        freezer_.removeAll();
        return okResp(id).dump();
    }

    if (cmd == "who") {
        std::lock_guard<std::mutex> lk(stateMu_);
        return okResp(id, json{
            {"pid", (int64_t)pid_},
            {"name", procName_},
            {"attached", pid_ > 0},
        }).dump();
    }

    // ---- 以下命令需要附加（短暂取 pid，扫描不持 stateMu_，避免阻塞进度查询）----
    pid_t pid = 0;
    std::string procName;
    {
        std::lock_guard<std::mutex> stateLk(stateMu_);
        pid = pid_;
        procName = procName_;
    }
    if (pid <= 0) return errResp(id, "not attached").dump();

    // ---- 模块/区域 ----
    if (cmd == "list_modules") {
        std::vector<Scanner::Module> mods;
        if (!Scanner::listModules(pid, mods)) return errResp(id, "maps read failed").dump();
        std::vector<json> arr;
        for (auto &m : mods)
            arr.push_back(json{
                {"name", m.name},
                {"base", "0x" + [&] { char b[32]; snprintf(b, sizeof(b), "%llx", (unsigned long long)m.base); return std::string(b); }()},
                {"end", "0x" + [&] { char b[32]; snprintf(b, sizeof(b), "%llx", (unsigned long long)m.end); return std::string(b); }()},
                {"size", (int64_t)(m.end - m.base)},
            });
        return okResp(id, json{{"modules", arr}}).dump();
    }

    if (cmd == "module_base" || cmd == "module_bss") {
        if (!req.contains("name")) return errResp(id, "missing name").dump();
        if (!driver_.ready()) return errResp(id, "driver not ready").dump();
        std::string name = req["name"].get<std::string>();
        uintptr_t addr = cmd == "module_base" ? driver_.getModuleBase(pid, name.c_str())
                                              : driver_.getModuleBss(pid, name.c_str());
        if (addr == 0) return errResp(id, "module not found").dump();
        char hex[32];
        snprintf(hex, sizeof(hex), "0x%llx", (unsigned long long)addr);
        return okResp(id, json{{"addr", hex}}).dump();
    }

    if (cmd == "list_regions") {
        bool writableOnly = !req.contains("writable_only") || req["writable_only"].get<bool>();
        std::vector<Region> regions;
        if (!Scanner::listRegions(pid, writableOnly, regions)) return errResp(id, "maps read failed").dump();
        std::string nameFilter = req.value("name_filter", "");
        std::vector<json> arr;
        for (auto &rg : regions) {
            if (!nameFilter.empty() && rg.path.find(nameFilter) == std::string::npos) continue;
            arr.push_back(json{
                {"start", "0x" + [&] { char b[32]; snprintf(b, sizeof(b), "%llx", (unsigned long long)rg.start); return std::string(b); }()},
                {"end", "0x" + [&] { char b[32]; snprintf(b, sizeof(b), "%llx", (unsigned long long)rg.end); return std::string(b); }()},
                {"size", (int64_t)(rg.end - rg.start)},
                {"path", rg.path},
                {"tag", rg.tag},
                {"writable", rg.writable},
            });
        }
        return okResp(id, json{{"regions", arr}}).dump();
    }

    // ---- 扫描/过滤 ----
    if (cmd == "driver_probe") {
        // 探测本驱动版本的 READ_MEM 实际命令号（只读形状请求，nr 0..48）
        // out 初值 0x5A5A...：若返回 0 且低 32 位被改写 → 该 nr 很可能是 READ
        if (!driver_.ready()) return errResp(id, "driver not ready").dump();
        std::vector<Region> regions;
        if (!Scanner::listRegions(pid, true, regions) || regions.empty())
            return errResp(id, "no regions").dump();
        uint64_t probeAddr = regions[0].start + 0x1000;

        std::vector<json> found;
        for (int nr = 0; nr <= 48; ++nr) {
            unsigned long variants[2] = {
                _IOW(TWT_MARK, nr, twt::twt_request),
                _IOWR(TWT_MARK, nr, twt::twt_request),
            };
            for (int vi = 0; vi < 2; ++vi) {
                uint64_t out = 0x5A5A5A5A5A5A5A5AULL;
                twt::twt_request r = {};
                r.pid = pid;
                r.addr = probeAddr;
                r.buffer = &out;
                r.size = 4;
                if (::ioctl(driver_.fd(), variants[vi], &r) == 0) {
                    bool looksLikeRead = (out & 0xFFFFFFFFULL) != 0x5A5A5A5AULL;
                    found.push_back(json{
                        {"nr", nr},
                        {"dir", vi == 0 ? "IOW" : "IOWR"},
                        {"value", (long long)out},
                        {"read_like", looksLikeRead},
                    });
                }
            }
        }
        char addrStr[32];
        snprintf(addrStr, sizeof(addrStr), "0x%llx", (unsigned long long)probeAddr);
        return okResp(id, json{{"probe_addr", addrStr}, {"hits", found}}).dump();
    }

    if (cmd == "search" || cmd == "search_unknown" || cmd == "filter") {
        if (scanning_.load()) return errResp(id, "scan already running").dump();
        if (!driver_.ready()) return errResp(id, "driver not ready").dump();

        ScanOptions opt;
        std::string typeStr = req.value("type", "i32");
        if (!vtFromString(typeStr, opt.type)) return errResp(id, "bad type").dump();

        if (req.contains("value") && !req["value"].is_null()) {
            const json &v = req["value"];
            if (opt.type == VT::F32 || opt.type == VT::F64) {
                double d = v.is_number() ? v.get<double>() : strtod(v.get<std::string>().c_str(), nullptr);
                opt.rawTarget = doubleToBits(opt.type, d);
                opt.hasTarget = true;
            } else if (v.is_string()) {
                opt.rawTarget = strtoull(v.get<std::string>().c_str(), nullptr, 0);
                opt.hasTarget = true;
            } else if (v.is_number_unsigned()) {
                opt.rawTarget = v.get<uint64_t>();
                opt.hasTarget = true;
            } else if (v.is_number_integer()) {
                opt.rawTarget = (uint64_t)v.get<int64_t>();
                opt.hasTarget = true;
            }
        }

        if (cmd == "search" && !opt.hasTarget) {
            return errResp(id, "missing value").dump();
        }

        opt.align = req.value("align", (int64_t)vtSize(opt.type));
        if (opt.align <= 0) opt.align = vtSize(opt.type);
        opt.nameFilter = req.value("name_filter", "");
        if (req.contains("tags")) {
            if (req["tags"].is_array()) {
                for (const auto &t : req["tags"]) if (t.is_string()) opt.tags.push_back(t.get<std::string>());
            } else if (req["tags"].is_string()) {
                opt.tags.push_back(req["tags"].get<std::string>());
            }
        }
        opt.maxResults = req.value("max", (int64_t)(cmd == "search_unknown" ? 500000 : 1000000));

        scanning_.store(true);
        uint64_t total = 0;
        bool truncated = false;
        bool ok;
        if (cmd == "search" || cmd == "search_unknown") {
            {
                std::lock_guard<std::mutex> hl(hitsMu_);
                hits_.clear();
            }
            ok = scanner_.search(pid, opt, hits_, hitsMu_, total, truncated);
        } else {
            FilterOp op;
            if (!filterOpFromString(req.value("op", "eq"), op))
                return errResp(id, "bad op").dump();
            ok = scanner_.filter(pid, opt, op, hits_, hitsMu_, total);
            truncated = false;
        }
        scanning_.store(false);
        if (!ok) return errResp(id, "scan failed (see daemon log)").dump();
        return okResp(id, json{{"count", (int64_t)total}, {"truncated", truncated}}).dump();
    }

    if (cmd == "scan_progress") {
        return okResp(id, json{
            {"running", scanner_.progress.running.load()},
            {"bytes_done", (int64_t)scanner_.progress.bytesDone.load()},
            {"bytes_total", (int64_t)scanner_.progress.bytesTotal.load()},
            {"found", (int64_t)scanner_.progress.found.load()},
        }).dump();
    }

    if (cmd == "results") {
        std::lock_guard<std::mutex> hl(hitsMu_);
        uint64_t total = hits_.size();
        size_t offset = req.value("offset", (int64_t)0);
        size_t limit = req.value("limit", (int64_t)200);
        std::vector<json> arr;
        for (size_t i = offset; i < hits_.size() && arr.size() < limit; ++i) {
            const Hit &h = hits_[i];
            char ab[32], bb[32];
            snprintf(ab, sizeof(ab), "0x%llx", (unsigned long long)h.addr);
            snprintf(bb, sizeof(bb), "0x%llx", (unsigned long long)h.raw);
            arr.push_back(json{
                {"addr", ab},
                {"type", vtToString(h.type)},
                {"bits", bb},
                {"value", bitsToDouble(h.type, h.raw)},
            });
        }
        return okResp(id, json{{"total", (int64_t)total}, {"hits", arr}}).dump();
    }

    if (cmd == "reset") {
        std::lock_guard<std::mutex> hl(hitsMu_);
        hits_.clear();
        return okResp(id).dump();
    }

    // ---- 读写 ----
    if (cmd == "read") {
        uint64_t addr;
        if (!parseAddr(req["addr"], addr)) return errResp(id, "bad addr").dump();
        VT type;
        if (!vtFromString(req.value("type", "i32"), type)) return errResp(id, "bad type").dump();
        uint64_t raw;
        if (!scanner_.singleRead(pid, addr, type, raw)) {
            char msg[128];
            snprintf(msg, sizeof(msg), "read failed errno=%d v2errno=%d",
                     driver_.lastErrno(), driver_.lastV2Errno());
            return errResp(id, msg).dump();
        }
        char bb[32];
        snprintf(bb, sizeof(bb), "0x%llx", (unsigned long long)raw);
        return okResp(id, json{{"bits", bb}, {"value", bitsToDouble(type, raw)}}).dump();
    }

    if (cmd == "write") {
        uint64_t addr;
        if (!parseAddr(req["addr"], addr)) return errResp(id, "bad addr").dump();
        VT type;
        if (!vtFromString(req.value("type", "i32"), type)) return errResp(id, "bad type").dump();
        uint64_t raw;
        if (req.contains("bits") && req["bits"].is_string()) {
            raw = strtoull(req["bits"].get<std::string>().c_str(), nullptr, 0);
        } else if (req.contains("value")) {
            const json &v = req["value"];
            double d = v.is_number() ? v.get<double>() : strtod(v.get<std::string>().c_str(), nullptr);
            raw = doubleToBits(type, d);
        } else {
            return errResp(id, "missing value").dump();
        }
        freezer_.clearAddr(addr);   // 写入前解除同地址冻结
        if (!scanner_.singleWrite(pid, addr, type, raw)) return errResp(id, "write failed").dump();
        return okResp(id).dump();
    }

    if (cmd == "inspect_memory") {
        uint64_t addr;
        if (!parseAddr(req["addr"], addr)) return errResp(id, "bad addr").dump();
        int countBefore = (int)req.value("before", (int64_t)4);
        int countAfter = (int)req.value("after", (int64_t)8);
        size_t unitSize = (size_t)req.value("unit_size", (int64_t)4);
        MemoryInspection insp;
        if (!scanner_.inspectMemory(pid, addr, countBefore, countAfter, unitSize, insp)) {
            return errResp(id, "inspect failed").dump();
        }
        std::vector<json> words;
        for (const auto &w : insp.words) {
            char ab[32];
            snprintf(ab, sizeof(ab), "0x%llx", (unsigned long long)w.addr);
            words.push_back(json{
                {"offset", w.offset},
                {"addr", ab},
                {"hex", w.hex},
                {"i32", w.i32},
                {"u32", (int64_t)w.u32},
                {"f32", w.f32},
                {"i64", w.i64},
                {"f64", w.f64},
                {"ascii", w.ascii},
                {"is_target", w.isTarget},
            });
        }
        return okResp(id, json{
            {"center_addr", "0x" + [&]{ char b[32]; snprintf(b, sizeof(b), "%llx", (unsigned long long)insp.centerAddr); return std::string(b); }()},
            {"words", words},
            {"summary", insp.summary},
        }).dump();
    }

    if (cmd == "search_group") {
        if (scanning_.load()) return errResp(id, "scan already running").dump();
        if (!driver_.ready()) return errResp(id, "driver not ready").dump();
        if (!req.contains("patterns") || !req["patterns"].is_array() || req["patterns"].empty()) {
            return errResp(id, "missing or empty patterns").dump();
        }
        GroupScanOptions gopt;
        for (const auto &p : req["patterns"]) {
            GroupPatternItem it;
            it.offset = p.value("offset", (int64_t)0);
            std::string tstr = p.value("type", "i32");
            if (!vtFromString(tstr, it.type)) return errResp(id, "bad type in pattern: " + tstr).dump();
            if (!p.contains("value")) return errResp(id, "missing value in pattern").dump();
            const json &v = p["value"];
            if (it.type == VT::F32 || it.type == VT::F64) {
                double d = v.is_number() ? v.get<double>() : strtod(v.get<std::string>().c_str(), nullptr);
                it.rawTarget = doubleToBits(it.type, d);
            } else if (v.is_string()) {
                it.rawTarget = strtoull(v.get<std::string>().c_str(), nullptr, 0);
            } else if (v.is_number_unsigned()) {
                it.rawTarget = v.get<uint64_t>();
            } else if (v.is_number_integer()) {
                it.rawTarget = (uint64_t)v.get<int64_t>();
            }
            gopt.items.push_back(it);
        }
        gopt.baseAlign = req.value("base_align", (int64_t)4);
        gopt.nameFilter = req.value("name_filter", "");
        if (req.contains("tags")) {
            if (req["tags"].is_array()) {
                for (const auto &t : req["tags"]) if (t.is_string()) gopt.tags.push_back(t.get<std::string>());
            } else if (req["tags"].is_string()) {
                gopt.tags.push_back(req["tags"].get<std::string>());
            }
        }
        gopt.maxResults = req.value("max", (int64_t)100000);

        scanning_.store(true);
        uint64_t total = 0;
        bool truncated = false;
        {
            std::lock_guard<std::mutex> hl(hitsMu_);
            hits_.clear();
        }
        bool ok = scanner_.searchGroup(pid, gopt, hits_, hitsMu_, total, truncated);
        scanning_.store(false);
        if (!ok) return errResp(id, "group search failed").dump();
        return okResp(id, json{{"count", (int64_t)total}, {"truncated", truncated}}).dump();
    }

    if (cmd == "find_pointers") {
        if (!driver_.ready()) return errResp(id, "driver not ready").dump();
        uint64_t targetAddr;
        if (!parseAddr(req["target_addr"], targetAddr)) return errResp(id, "bad target_addr").dump();
        int64_t maxOffset = req.value("max_offset", (int64_t)0);
        size_t align = (size_t)req.value("align", (int64_t)8);
        std::vector<std::string> tags;
        if (req.contains("tags")) {
            if (req["tags"].is_array()) {
                for (const auto &t : req["tags"]) if (t.is_string()) tags.push_back(t.get<std::string>());
            } else if (req["tags"].is_string()) {
                tags.push_back(req["tags"].get<std::string>());
            }
        }
        std::vector<PointerHit> hits;
        if (!scanner_.findPointers(pid, targetAddr, maxOffset, align, tags, hits)) {
            return errResp(id, "find pointers failed").dump();
        }
        std::vector<json> arr;
        for (const auto &h : hits) {
            char pStr[32], tStr[32];
            snprintf(pStr, sizeof(pStr), "0x%llx", (unsigned long long)h.pointerAddr);
            snprintf(tStr, sizeof(tStr), "0x%llx", (unsigned long long)h.pointsTo);
            arr.push_back(json{
                {"pointer_addr", pStr},
                {"points_to", tStr},
                {"offset", h.offset},
                {"tag", h.regionTag},
                {"module", h.moduleName},
            });
        }
        return okResp(id, json{{"total", (int64_t)hits.size()}, {"pointers", arr}}).dump();
    }

    if (cmd == "search_string") {
        if (scanning_.load()) return errResp(id, "scan already running").dump();
        if (!driver_.ready()) return errResp(id, "driver not ready").dump();
        if (!req.contains("text")) return errResp(id, "missing text").dump();
        std::string text = req["text"].get<std::string>();
        std::string encoding = req.value("encoding", "utf8");
        std::string nameFilter = req.value("name_filter", "");
        std::vector<std::string> tags;
        if (req.contains("tags")) {
            if (req["tags"].is_array()) {
                for (const auto &t : req["tags"]) if (t.is_string()) tags.push_back(t.get<std::string>());
            } else if (req["tags"].is_string()) {
                tags.push_back(req["tags"].get<std::string>());
            }
        }
        uint64_t maxResults = req.value("max", (int64_t)100000);

        scanning_.store(true);
        uint64_t total = 0;
        bool truncated = false;
        {
            std::lock_guard<std::mutex> hl(hitsMu_);
            hits_.clear();
        }
        bool ok = scanner_.searchString(pid, text, encoding, nameFilter, tags, maxResults, hits_, hitsMu_, total, truncated);
        scanning_.store(false);
        if (!ok) return errResp(id, "search string failed").dump();
        return okResp(id, json{{"count", (int64_t)total}, {"truncated", truncated}}).dump();
    }

    if (cmd == "search_pattern") {
        if (scanning_.load()) return errResp(id, "scan already running").dump();
        if (!driver_.ready()) return errResp(id, "driver not ready").dump();
        if (!req.contains("pattern")) return errResp(id, "missing pattern").dump();
        std::string pat = req["pattern"].get<std::string>();
        size_t align = (size_t)req.value("align", (int64_t)4);
        std::string nameFilter = req.value("name_filter", "");
        std::vector<std::string> tags;
        if (req.contains("tags")) {
            if (req["tags"].is_array()) {
                for (const auto &t : req["tags"]) if (t.is_string()) tags.push_back(t.get<std::string>());
            } else if (req["tags"].is_string()) {
                tags.push_back(req["tags"].get<std::string>());
            }
        }
        uint64_t maxResults = req.value("max", (int64_t)100000);

        scanning_.store(true);
        uint64_t total = 0;
        bool truncated = false;
        {
            std::lock_guard<std::mutex> hl(hitsMu_);
            hits_.clear();
        }
        bool ok = scanner_.searchPattern(pid, pat, align, nameFilter, tags, maxResults, hits_, hitsMu_, total, truncated);
        scanning_.store(false);
        if (!ok) return errResp(id, "search pattern failed (bad pattern?)").dump();
        return okResp(id, json{{"count", (int64_t)total}, {"truncated", truncated}}).dump();
    }

    if (cmd == "resolve_pointer_chain") {
        if (!driver_.ready()) return errResp(id, "driver not ready").dump();
        if (!req.contains("base")) return errResp(id, "missing base").dump();
        std::string base = req["base"].get<std::string>();
        std::vector<int64_t> offsets;
        if (req.contains("offsets") && req["offsets"].is_array()) {
            for (const auto &o : req["offsets"]) {
                if (o.is_number_integer()) offsets.push_back(o.get<int64_t>());
                else if (o.is_string()) offsets.push_back((int64_t)strtoull(o.get<std::string>().c_str(), nullptr, 0));
            }
        }
        size_t ptrSize = req.value("ptr_size", (int64_t)8);
        VT finalType = VT::I32;
        if (req.contains("type")) vtFromString(req["type"].get<std::string>(), finalType);

        PointerChainResult res;
        if (!scanner_.resolvePointerChain(pid, base, offsets, ptrSize, finalType, res)) {
            return errResp(id, res.err.empty() ? "resolve pointer chain failed" : res.err).dump();
        }
        std::vector<json> steps;
        for (const auto &s : res.steps) {
            char ab[32], vb[32];
            snprintf(ab, sizeof(ab), "0x%llx", (unsigned long long)s.addr);
            snprintf(vb, sizeof(vb), "0x%llx", (unsigned long long)s.value);
            steps.push_back(json{{"offset", s.offset}, {"addr", ab}, {"value", vb}, {"ok", s.ok}});
        }
        char fab[32], fvb[32];
        snprintf(fab, sizeof(fab), "0x%llx", (unsigned long long)res.finalAddr);
        snprintf(fvb, sizeof(fvb), "0x%llx", (unsigned long long)res.finalValue);
        return okResp(id, json{
            {"base_addr", "0x" + [&]{ char b[32]; snprintf(b, sizeof(b), "%llx", (unsigned long long)res.baseAddr); return std::string(b); }()},
            {"steps", steps},
            {"final_addr", fab},
            {"final_bits", fvb},
            {"final_value", bitsToDouble(finalType, res.finalValue)},
            {"ok", res.ok}
        }).dump();
    }

    if (cmd == "disasm") {
        if (!driver_.ready()) return errResp(id, "driver not ready").dump();
        uint64_t addr;
        if (!parseAddr(req["addr"], addr)) return errResp(id, "bad addr").dump();
        size_t count = (size_t)req.value("count", (int64_t)10);
        std::vector<DisasmLine> lines;
        if (!scanner_.disassemble(pid, addr, count, lines)) {
            return errResp(id, "disassemble failed").dump();
        }
        std::vector<json> arr;
        std::string summary;
        for (const auto &l : lines) {
            char ab[32];
            snprintf(ab, sizeof(ab), "0x%llx", (unsigned long long)l.addr);
            arr.push_back(json{
                {"addr", ab},
                {"raw", l.raw},
                {"hex", l.hex},
                {"mnemonic", l.mnemonic},
                {"operands", l.operands},
                {"symbol", l.symbol},
            });
            char lineBuf[256];
            snprintf(lineBuf, sizeof(lineBuf), "0x%llx: %-10s %-8s %-20s %s\n",
                     (unsigned long long)l.addr, l.hex.c_str(), l.mnemonic.c_str(),
                     l.operands.c_str(), l.symbol.empty() ? "" : (" ; " + l.symbol).c_str());
            summary += lineBuf;
        }
        return okResp(id, json{{"instructions", arr}, {"summary", summary}}).dump();
    }

    if (cmd == "patch_code") {
        if (!driver_.ready()) return errResp(id, "driver not ready").dump();
        uint64_t addr;
        if (!parseAddr(req["addr"], addr)) return errResp(id, "bad addr").dump();
        std::vector<uint8_t> patchBytes;
        if (req.value("value", "") == "nop") {
            int count = (int)req.value("count", (int64_t)1);
            for (int i = 0; i < count; ++i) {
                patchBytes.push_back(0x1F);
                patchBytes.push_back(0x20);
                patchBytes.push_back(0x03);
                patchBytes.push_back(0xD5);
            }
        } else if (req.contains("hex")) {
            std::string hstr = req["hex"].get<std::string>();
            if (hstr.rfind("0x", 0) == 0 || hstr.rfind("0X", 0) == 0) hstr = hstr.substr(2);
            for (size_t i = 0; i + 1 < hstr.size(); i += 2) {
                char byteStr[3] = { hstr[i], hstr[i+1], '\0' };
                patchBytes.push_back((uint8_t)strtoul(byteStr, nullptr, 16));
            }
        } else {
            return errResp(id, "missing hex or value='nop'").dump();
        }
        if (patchBytes.empty()) return errResp(id, "empty patch bytes").dump();
        if (!driver_.writeMem(pid, addr, patchBytes.data(), patchBytes.size())) {
            return errResp(id, "write code failed").dump();
        }
        return okResp(id, json{{"patched_bytes", (int64_t)patchBytes.size()}}).dump();
    }

    if (cmd == "watch_point") {
        if (!driver_.ready()) return errResp(id, "driver not ready").dump();
        uint64_t addr;
        if (!parseAddr(req["addr"], addr)) return errResp(id, "bad addr").dump();
        std::string typeStr = req.value("type", "w");
        uint32_t bpType = HW_BREAKPOINT_W;
        if (typeStr == "r") bpType = HW_BREAKPOINT_R;
        else if (typeStr == "rw" || typeStr == "wr") bpType = HW_BREAKPOINT_RW;
        else if (typeStr == "x") bpType = HW_BREAKPOINT_X;

        uint32_t len = (uint32_t)req.value("len", (int64_t)4);
        if (len != 1 && len != 2 && len != 4 && len != 8) len = 4;

        driver_.bpInit();
        uint64_t handle = driver_.bpInstall(pid, addr, len, bpType, 0x1 /* BP_FLAG_RECORD */);
        if (handle == 0) return errResp(id, "install watchpoint failed").dump();
        char hbuf[32];
        snprintf(hbuf, sizeof(hbuf), "0x%llx", (unsigned long long)handle);
        return okResp(id, json{{"handle", hbuf}}).dump();
    }

    if (cmd == "unwatch_point") {
        if (!driver_.ready()) return errResp(id, "driver not ready").dump();
        uint64_t handle;
        if (!parseAddr(req["handle"], handle)) return errResp(id, "bad handle").dump();
        bool ok = driver_.bpUninstall(handle);
        return okResp(id, json{{"uninstalled", ok}}).dump();
    }

    if (cmd == "get_watch_hits") {
        if (!driver_.ready()) return errResp(id, "driver not ready").dump();
        uint64_t handle;
        if (!parseAddr(req["handle"], handle)) return errResp(id, "bad handle").dump();
        size_t maxItems = (size_t)req.value("max_items", (int64_t)50);
        auto hits = driver_.bpGetHits(handle, maxItems);

        std::vector<Scanner::Module> modules;
        Scanner::listModules(pid, modules);

        std::vector<json> arr;
        for (const auto &h : hits) {
            char pStr[32], pcStr[32], lrStr[32], spStr[32];
            snprintf(pStr, sizeof(pStr), "0x%llx", (unsigned long long)h.hit_addr);
            snprintf(pcStr, sizeof(pcStr), "0x%llx", (unsigned long long)h.regs_info.pc);
            snprintf(lrStr, sizeof(lrStr), "0x%llx", (unsigned long long)h.regs_info.regs[30]);
            snprintf(spStr, sizeof(spStr), "0x%llx", (unsigned long long)h.regs_info.sp);

            std::string modDesc;
            for (const auto &m : modules) {
                if (h.regs_info.pc >= m.base && h.regs_info.pc < m.end) {
                    char b[64];
                    snprintf(b, sizeof(b), "+0x%llx", (unsigned long long)(h.regs_info.pc - m.base));
                    size_t slash = m.name.rfind('/');
                    std::string baseName = (slash != std::string::npos) ? m.name.substr(slash + 1) : m.name;
                    modDesc = baseName + b;
                    break;
                }
            }

            // 1. 上下文汇编窗口：前 3 条、当前命中指令、后 3 条
            std::vector<DisasmLine> ctxDisasm;
            uint64_t winStart = (h.regs_info.pc >= 12) ? (h.regs_info.pc - 12) : h.regs_info.pc;
            scanner_.disassemble(pid, winStart, 7, ctxDisasm);

            std::string contextAsm;
            for (const auto &dl : ctxDisasm) {
                char lBuf[128];
                bool isHit = (dl.addr == h.regs_info.pc);
                snprintf(lBuf, sizeof(lBuf), "%s 0x%llx: %-8s %-20s%s\n",
                         isHit ? "==>" : "   ", (unsigned long long)dl.addr,
                         dl.mnemonic.c_str(), dl.operands.c_str(),
                         isHit ? "  <-- HIT INSTRUCTION" : "");
                contextAsm += lBuf;
            }

            // 2. Caller (上一级调用函数) 分析：由 LR (X30) 确定
            uint64_t callerAddr = h.regs_info.regs[30];
            std::string callerDesc;
            std::string callerCallsite;
            for (const auto &m : modules) {
                if (callerAddr >= m.base && callerAddr < m.end) {
                    char b[64];
                    snprintf(b, sizeof(b), "+0x%llx", (unsigned long long)(callerAddr - m.base));
                    size_t slash = m.name.rfind('/');
                    std::string baseName = (slash != std::string::npos) ? m.name.substr(slash + 1) : m.name;
                    callerDesc = baseName + b;
                    break;
                }
            }
            if (callerAddr >= 4) {
                std::vector<DisasmLine> callerLines;
                if (scanner_.disassemble(pid, callerAddr - 4, 1, callerLines) && !callerLines.empty()) {
                    callerCallsite = callerLines[0].mnemonic + " " + callerLines[0].operands;
                }
            }

            // 3. 关键寄存器快照 (X0..X7, X8, SP, LR, PC)
            json regMap;
            for (int r = 0; r <= 8; ++r) {
                char rk[8], rv[32];
                snprintf(rk, sizeof(rk), "x%d", r);
                snprintf(rv, sizeof(rv), "0x%llx", (unsigned long long)h.regs_info.regs[r]);
                regMap[rk] = rv;
            }
            regMap["lr"] = lrStr;
            regMap["sp"] = spStr;
            regMap["pc"] = pcStr;

            arr.push_back(json{
                {"task_id", (int64_t)h.task_id},
                {"hit_addr", pStr},
                {"pc", pcStr},
                {"lr", lrStr},
                {"sp", spStr},
                {"module", modDesc},
                {"disasm_window", contextAsm},
                {"caller", callerDesc},
                {"caller_callsite", callerCallsite},
                {"registers", regMap},
            });
        }
        return okResp(id, json{{"total", (int64_t)hits.size()}, {"hits", arr}}).dump();
    }

    // ---- 冻结 ----
    if (cmd == "freeze") {
        uint64_t addr;
        if (!parseAddr(req["addr"], addr)) return errResp(id, "bad addr").dump();
        VT type;
        if (!vtFromString(req.value("type", "i32"), type)) return errResp(id, "bad type").dump();
        uint64_t raw;
        if (req.contains("bits") && req["bits"].is_string()) {
            raw = strtoull(req["bits"].get<std::string>().c_str(), nullptr, 0);
        } else if (req.contains("value")) {
            const json &v = req["value"];
            double d = v.is_number() ? v.get<double>() : strtod(v.get<std::string>().c_str(), nullptr);
            raw = doubleToBits(type, d);
        } else {
            // 未指定值：读当前值
            if (!scanner_.singleRead(pid, addr, type, raw)) return errResp(id, "cannot read current").dump();
        }
        uint32_t interval = (uint32_t)req.value("interval_ms", (int64_t)150);

        bool hasGuard = req.contains("guard_offset");
        int64_t guardOffset = req.value("guard_offset", (int64_t)0);
        VT guardType = VT::I32;
        if (req.contains("guard_type")) vtFromString(req["guard_type"].get<std::string>(), guardType);
        uint64_t guardExpected = 0;
        if (req.contains("guard_value")) {
            const json &gv = req["guard_value"];
            if (gv.is_string()) guardExpected = strtoull(gv.get<std::string>().c_str(), nullptr, 0);
            else if (gv.is_number_integer()) guardExpected = (uint64_t)gv.get<int64_t>();
            else if (gv.is_number_unsigned()) guardExpected = gv.get<uint64_t>();
        }

        freezer_.add(pid, addr, type, raw, interval, hasGuard, guardOffset, guardType, guardExpected);
        return okResp(id, json{{"frozen", (int64_t)freezer_.count()}}).dump();
    }

    if (cmd == "unfreeze") {
        uint64_t addr;
        if (!parseAddr(req["addr"], addr)) return errResp(id, "bad addr").dump();
        return okResp(id, json{{"removed", freezer_.remove(addr)}}).dump();
    }

    if (cmd == "unfreeze_all") {
        freezer_.removeAll();
        return okResp(id).dump();
    }

    if (cmd == "frozen_list") {
        auto entries = freezer_.snapshot();
        std::vector<json> arr;
        for (auto &[addr, e] : entries) {
            char ab[32], bb[32];
            snprintf(ab, sizeof(ab), "0x%llx", (unsigned long long)addr);
            snprintf(bb, sizeof(bb), "0x%llx", (unsigned long long)e.raw);
            arr.push_back(json{
                {"addr", ab},
                {"type", vtToString(e.type)},
                {"bits", bb},
                {"value", bitsToDouble(e.type, e.raw)},
                {"interval_ms", e.intervalMs},
                {"fails", e.failCount},
                {"has_guard", e.hasGuard},
                {"guard_offset", e.guardOffset},
                {"guard_expected", e.guardExpected},
                {"guard_type", vtToString(e.guardType)},
            });
        }
        return okResp(id, json{{"entries", arr}}).dump();
    }

    // ---- 内存/模块转储 ----
    if (cmd == "dump_memory") {
        if (!driver_.ready()) return errResp(id, "driver not ready").dump();
        uint64_t addr;
        if (!parseAddr(req["addr"], addr)) return errResp(id, "bad addr").dump();
        size_t size = (size_t)req.value("size", (int64_t)0);
        std::string outPath = req.value("path", "");
        if (outPath.empty()) return errResp(id, "missing path").dump();
        size_t written = 0;
        std::string err;
        bool ok = scanner_.dumpMemory(pid, addr, size, outPath, written, err);
        if (!ok) return errResp(id, err.empty() ? "dump memory failed" : err).dump();
        return okResp(id, json{{"path", outPath}, {"bytes_written", (int64_t)written}}).dump();
    }

    if (cmd == "dump_module") {
        if (!driver_.ready()) return errResp(id, "driver not ready").dump();
        std::string mod = req.value("module", "");
        if (mod.empty()) return errResp(id, "missing module").dump();
        std::string outPath = req.value("path", "");
        if (outPath.empty()) {
            size_t slash = mod.rfind('/');
            std::string baseName = (slash != std::string::npos) ? mod.substr(slash + 1) : mod;
            outPath = "/data/local/tmp/" + baseName + ".dump";
        }
        size_t written = 0;
        std::string err;
        bool ok = scanner_.dumpModule(pid, mod, outPath, written, err);
        if (!ok) return errResp(id, err.empty() ? "dump module failed" : err).dump();
        return okResp(id, json{{"path", outPath}, {"bytes_written", (int64_t)written}}).dump();
    }

    // ---- 持久化规则引擎 ----
    if (cmd == "register_rule") {
        if (!driver_.ready()) return errResp(id, "driver not ready").dump();
        std::string name = req.value("name", "");
        if (!req.contains("base")) return errResp(id, "missing base").dump();
        std::string base = req["base"].get<std::string>();
        std::vector<int64_t> offsets;
        if (req.contains("offsets") && req["offsets"].is_array()) {
            for (const auto &o : req["offsets"]) {
                if (o.is_number_integer()) offsets.push_back(o.get<int64_t>());
                else if (o.is_string()) offsets.push_back((int64_t)strtoull(o.get<std::string>().c_str(), nullptr, 0));
            }
        }
        size_t ptrSize = (size_t)req.value("ptr_size", (int64_t)4);
        VT type = VT::I32;
        if (req.contains("type")) vtFromString(req["type"].get<std::string>(), type);
        std::string action = req.value("action", "lock");

        uint64_t targetBits = 0;
        if (req.contains("bits") && req["bits"].is_string()) {
            targetBits = strtoull(req["bits"].get<std::string>().c_str(), nullptr, 0);
        } else if (req.contains("value")) {
            const json &v = req["value"];
            double d = v.is_number() ? v.get<double>() : strtod(v.get<std::string>().c_str(), nullptr);
            targetBits = doubleToBits(type, d);
        } else if (action.find("ratio") == std::string::npos) {
            return errResp(id, "missing value").dump();
        }
        uint32_t intervalMs = (uint32_t)req.value("interval_ms", (int64_t)150);

        bool hasSource2 = req.contains("base2");
        std::string base2 = req.value("base2", "");
        std::vector<int64_t> offsets2;
        if (req.contains("offsets2") && req["offsets2"].is_array()) {
            for (const auto &o : req["offsets2"]) {
                if (o.is_number_integer()) offsets2.push_back(o.get<int64_t>());
                else if (o.is_string()) offsets2.push_back((int64_t)strtoull(o.get<std::string>().c_str(), nullptr, 0));
            }
        }
        size_t ptrSize2 = (size_t)req.value("ptr_size2", (int64_t)4);
        VT type2 = VT::I32;
        if (req.contains("type2")) vtFromString(req["type2"].get<std::string>(), type2);
        double ratio = req.value("ratio", 1.0);
        double offsetVal = req.value("offset_val", 0.0);

        uint64_t ruleId = ruleEngine_.addRule(pid, name, base, offsets, ptrSize, type, action, targetBits, intervalMs,
                                              hasSource2, base2, offsets2, ptrSize2, type2, ratio, offsetVal);
        return okResp(id, json{{"rule_id", (int64_t)ruleId}, {"name", name}}).dump();
    }

    if (cmd == "list_rules") {
        auto rules = ruleEngine_.listRules();
        std::vector<json> arr;
        for (const auto &r : rules) {
            char ab[32], vb[32], ab2[32], vb2[32];
            snprintf(ab, sizeof(ab), "0x%llx", (unsigned long long)r.lastResolvedAddr);
            snprintf(vb, sizeof(vb), "0x%llx", (unsigned long long)r.lastValue);
            snprintf(ab2, sizeof(ab2), "0x%llx", (unsigned long long)r.lastResolvedAddr2);
            snprintf(vb2, sizeof(vb2), "0x%llx", (unsigned long long)r.lastValue2);
            json rj = {
                {"id", (int64_t)r.id},
                {"name", r.name},
                {"base", r.base},
                {"offsets", r.offsets},
                {"ptr_size", r.ptrSize},
                {"type", vtToString(r.type)},
                {"action", r.action},
                {"target_value", bitsToDouble(r.type, r.targetBits)},
                {"interval_ms", r.intervalMs},
                {"last_resolved_addr", ab},
                {"last_value", bitsToDouble(r.type, r.lastValue)},
                {"fail_count", r.failCount},
                {"active", r.active},
                {"has_source2", r.hasSource2},
            };
            if (r.hasSource2) {
                rj["base2"] = r.base2;
                rj["offsets2"] = r.offsets2;
                rj["ratio"] = r.ratio;
                rj["offset_val"] = r.offsetVal;
                rj["last_resolved_addr2"] = ab2;
                rj["last_value2"] = bitsToDouble(r.type2, r.lastValue2);
            }
            arr.push_back(rj);
        }
        return okResp(id, json{{"rules", arr}}).dump();
    }

    if (cmd == "delete_rule") {
        uint64_t ruleId = (uint64_t)req.value("rule_id", (int64_t)0);
        bool removed = ruleEngine_.removeRule(ruleId);
        return okResp(id, json{{"removed", removed}}).dump();
    }

    if (cmd == "clear_rules") {
        ruleEngine_.removeAll();
        return okResp(id).dump();
    }

    // ---- Unity IL2CPP 探针 ----
    if (cmd == "il2cpp_status") {
        if (!driver_.ready()) return errResp(id, "driver not ready").dump();
        Il2CppStatus st;
        il2cppInspector_.getStatus(pid, st);
        char bb[32], eb[32], mb[32];
        snprintf(bb, sizeof(bb), "0x%llx", (unsigned long long)st.moduleBase);
        snprintf(eb, sizeof(eb), "0x%llx", (unsigned long long)st.moduleEnd);
        snprintf(mb, sizeof(mb), "0x%llx", (unsigned long long)st.metadataAddr);
        return okResp(id, json{
            {"detected", st.detected},
            {"module_base", bb},
            {"module_end", eb},
            {"module_size", (int64_t)st.moduleSize},
            {"module_path", st.modulePath},
            {"has_metadata", st.hasMetadata},
            {"metadata_addr", mb},
            {"api_count", (int64_t)st.apiCount},
        }).dump();
    }

    if (cmd == "il2cpp_apis") {
        if (!driver_.ready()) return errResp(id, "driver not ready").dump();
        std::vector<Il2CppApiInfo> apis;
        il2cppInspector_.listApis(pid, apis);
        std::vector<json> arr;
        for (const auto &a : apis) {
            char ab[32];
            snprintf(ab, sizeof(ab), "0x%llx", (unsigned long long)a.addr);
            arr.push_back(json{{"name", a.name}, {"addr", ab}});
        }
        return okResp(id, json{{"count", (int64_t)apis.size()}, {"apis", arr}}).dump();
    }

    if (cmd == "il2cpp_find_class") {
        if (!driver_.ready()) return errResp(id, "driver not ready").dump();
        std::string cname = req.value("class", "");
        if (cname.empty()) return errResp(id, "missing class").dump();
        std::vector<Il2CppClassInfo> classes;
        bool ok = il2cppInspector_.inspectClass(pid, cname, classes);
        std::vector<json> arr;
        std::string summary;
        for (const auto &c : classes) {
            std::vector<json> fields;
            summary += "Class: " + (c.namespaze.empty() ? "" : (c.namespaze + ".")) + c.name + "\n";
            for (const auto &f : c.fields) {
                fields.push_back(json{
                    {"name", f.name},
                    {"offset", f.offset},
                    {"type", f.type},
                });
                char fbuf[128];
                snprintf(fbuf, sizeof(fbuf), "  +0x%x (%-3d) %s\n", f.offset, f.offset, f.name.c_str());
                summary += fbuf;
            }
            arr.push_back(json{
                {"name", c.name},
                {"namespace", c.namespaze},
                {"token", c.token},
                {"fields", fields},
            });
        }
        return okResp(id, json{
            {"count", (int64_t)classes.size()},
            {"classes", arr},
            {"summary", summary},
            {"found", ok}
        }).dump();
    }

    // ---- 态势遥测监控 (Telemetry Monitor) ----
    if (cmd == "telemetry_query") {
        if (!driver_.ready()) return errResp(id, "driver not ready").dump();
        if (!req.contains("items") || !req["items"].is_array()) {
            return errResp(id, "missing items array").dump();
        }
        std::string summary;
        json data = json::object();
        for (const auto &it : req["items"]) {
            std::string label = it.value("label", "");
            uint64_t addr = 0;
            if (!it.contains("addr") || !parseAddr(it["addr"], addr)) continue;
            VT type = VT::I32;
            if (it.contains("type")) vtFromString(it["type"].get<std::string>(), type);

            uint64_t raw = 0;
            if (scanner_.singleRead(pid, addr, type, raw)) {
                double val = bitsToDouble(type, raw);
                data[label.empty() ? ("0x" + std::to_string(addr)) : label] = val;
                char vbuf[64];
                if (type == VT::F32 || type == VT::F64) snprintf(vbuf, sizeof(vbuf), "%.4g", val);
                else snprintf(vbuf, sizeof(vbuf), "%lld", (long long)val);
                summary += "[" + (label.empty() ? "val" : label) + ": " + vbuf + "] ";
            } else {
                data[label] = nullptr;
                summary += "[" + label + ": ?] ";
            }
        }
        return okResp(id, json{{"summary", summary}, {"data", data}}).dump();
    }

    // ---- 应用私有存储与 SQLite 数据库 ----
    if (cmd == "app_storage_list") {
        std::string sub = req.value("subpath", "");
        std::string resolved = StorageExplorer::resolvePath(pid, procName, sub);
        size_t limit = (size_t)req.value("limit", (int64_t)50);
        std::vector<AppFileInfo> files;
        std::string err;
        if (!StorageExplorer::listFiles(resolved, limit, files, err)) {
            return errResp(id, err.empty() ? "list files failed" : err).dump();
        }
        std::vector<json> arr;
        std::string summary = "Directory: " + resolved + "\n";
        for (const auto &f : files) {
            arr.push_back(json{
                {"name", f.name},
                {"size", (int64_t)f.size},
                {"is_dir", f.isDir},
                {"perms", f.perms},
                {"mtime", (int64_t)f.mtime}
            });
            char lBuf[128];
            snprintf(lBuf, sizeof(lBuf), "  %c %-6s %-10lld %s\n",
                     f.isDir ? 'd' : '-', f.perms.c_str(), (long long)f.size, f.name.c_str());
            summary += lBuf;
        }
        return okResp(id, json{{"path", resolved}, {"total", (int64_t)files.size()}, {"files", arr}, {"summary", summary}}).dump();
    }

    if (cmd == "app_storage_read") {
        if (!req.contains("path")) return errResp(id, "missing path").dump();
        std::string sub = req["path"].get<std::string>();
        std::string resolved = StorageExplorer::resolvePath(pid, procName, sub);
        size_t maxBytes = (size_t)req.value("max_bytes", (int64_t)4096);
        size_t offset = (size_t)req.value("offset", (int64_t)0);
        std::string content, err;
        size_t totalSize = 0;
        bool isText = true;
        if (!StorageExplorer::readFile(resolved, maxBytes, offset, content, totalSize, isText, err)) {
            return errResp(id, err.empty() ? "read file failed" : err).dump();
        }
        return okResp(id, json{
            {"path", resolved},
            {"total_size", (int64_t)totalSize},
            {"bytes_read", (int64_t)content.size()},
            {"is_text", isText},
            {"content", content}
        }).dump();
    }

    if (cmd == "app_storage_write") {
        if (!req.contains("path") || !req.contains("content")) return errResp(id, "missing path or content").dump();
        std::string sub = req["path"].get<std::string>();
        std::string resolved = StorageExplorer::resolvePath(pid, procName, sub);
        std::string content = req["content"].get<std::string>();
        bool isHex = req.value("is_hex", false);
        size_t written = 0;
        std::string err;
        if (!StorageExplorer::writeFile(resolved, content, isHex, written, err)) {
            return errResp(id, err.empty() ? "write file failed" : err).dump();
        }
        return okResp(id, json{{"path", resolved}, {"bytes_written", (int64_t)written}}).dump();
    }

    if (cmd == "sqlite_query") {
        if (!req.contains("db") || !req.contains("sql")) return errResp(id, "missing db or sql").dump();
        std::string dbSub = req["db"].get<std::string>();
        std::string resolved = StorageExplorer::resolvePath(pid, procName, dbSub);
        std::string sql = req["sql"].get<std::string>();
        size_t limit = (size_t)req.value("limit", (int64_t)25);
        SqliteQueryResult qres;
        if (!StorageExplorer::sqliteQuery(resolved, sql, limit, qres)) {
            return errResp(id, qres.err.empty() ? "sqlite query failed" : qres.err).dump();
        }
        std::vector<json> rows;
        for (const auto &r : qres.rows) {
            json rj = json::object();
            for (size_t i = 0; i < qres.columns.size() && i < r.size(); ++i) {
                rj[qres.columns[i]] = r[i];
            }
            rows.push_back(rj);
        }
        return okResp(id, json{
            {"db", resolved},
            {"columns", qres.columns},
            {"rows", rows},
            {"summary", qres.summary},
            {"count", (int64_t)rows.size()}
        }).dump();
    }

    if (cmd == "sqlite_exec") {
        if (!req.contains("db") || !req.contains("sql")) return errResp(id, "missing db or sql").dump();
        std::string dbSub = req["db"].get<std::string>();
        std::string resolved = StorageExplorer::resolvePath(pid, procName, dbSub);
        std::string sql = req["sql"].get<std::string>();
        int changes = 0;
        std::string err;
        if (!StorageExplorer::sqliteExec(resolved, sql, changes, err)) {
            return errResp(id, err.empty() ? "sqlite exec failed" : err).dump();
        }
        return okResp(id, json{{"db", resolved}, {"changes", changes}}).dump();
    }

    // ---- 崩溃自愈诊断 (Crash Triage) ----
    if (cmd == "crash_triage") {
        CrashReport report;
        bool found = CrashTriage::getLatestCrash(pid, procName, report);
        if (!found) {
            return okResp(id, json{{"has_crash", false}, {"note", "no crash tombstone found for target"}}).dump();
        }
        return okResp(id, json{
            {"has_crash", true},
            {"pid", report.pid},
            {"process_name", report.processName},
            {"signal", report.signal},
            {"fault_addr", report.faultAddr},
            {"pc", report.pc},
            {"lr", report.lr},
            {"sp", report.sp},
            {"backtrace", report.backtrace},
            {"summary", report.rawSummary}
        }).dump();
    }

    // ---- 进程内原生函数远程调用 (Remote RPC Invocation) ----
    if (cmd == "invoke_function") {
        if (!req.contains("addr")) return errResp(id, "missing addr").dump();
        uint64_t addr = 0;
        if (!parseAddr(req["addr"], addr)) return errResp(id, "bad function addr").dump();

        std::vector<uint64_t> args;
        if (req.contains("args") && req["args"].is_array()) {
            for (const auto &a : req["args"]) {
                if (a.is_number_integer()) args.push_back((uint64_t)a.get<int64_t>());
                else if (a.is_number_unsigned()) args.push_back(a.get<uint64_t>());
                else if (a.is_string()) args.push_back(strtoull(a.get<std::string>().c_str(), nullptr, 0));
            }
        }
        uint32_t timeoutMs = (uint32_t)req.value("timeout_ms", (int64_t)2000);
        InvocationResult ires;
        if (!RemoteInvoker::call(pid, addr, args, ires, timeoutMs)) {
            return errResp(id, ires.err.empty() ? "remote invocation failed" : ires.err).dump();
        }
        return okResp(id, json{
            {"ret_val", (int64_t)ires.retVal},
            {"ret_hex", ires.retHex},
            {"ok", ires.ok}
        }).dump();
    }

    return errResp(id, "unknown cmd: " + cmd).dump();
}

} // namespace eng
