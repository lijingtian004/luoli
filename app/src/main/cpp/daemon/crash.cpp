#include "crash.h"

#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <vector>

namespace eng {

bool CrashTriage::getLatestCrash(pid_t targetPid, const std::string &targetPkg, CrashReport &out) {
    out = CrashReport();

    // 扫描 /data/tombstones 查找最新文件
    std::string tombstonesDir = "/data/tombstones";
    DIR *d = opendir(tombstonesDir.c_str());
    if (!d) return false;

    struct TombstoneFile {
        std::string path;
        time_t mtime = 0;
    };
    std::vector<TombstoneFile> files;

    struct dirent *de;
    while ((de = readdir(d)) != nullptr) {
        if (strncmp(de->d_name, "tombstone_", 10) == 0 && strstr(de->d_name, ".pb") == nullptr) {
            std::string fullPath = tombstonesDir + "/" + de->d_name;
            struct stat st = {};
            if (stat(fullPath.c_str(), &st) == 0) {
                files.push_back({fullPath, st.st_mtime});
            }
        }
    }
    closedir(d);

    if (files.empty()) return false;

    std::sort(files.begin(), files.end(), [](const TombstoneFile &a, const TombstoneFile &b) {
        return a.mtime > b.mtime;
    });

    for (const auto &tf : files) {
        FILE *f = fopen(tf.path.c_str(), "r");
        if (!f) continue;

        char line[512];
        bool matched = false;
        int tPid = 0;
        std::string procName;
        std::string sigStr, faultAddr, pcStr, lrStr, spStr;
        std::vector<std::string> bt;
        bool inBacktrace = false;

        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "pid: ")) {
                sscanf(line, "pid: %d", &tPid);
                char *cmd = strstr(line, "name: ");
                if (cmd) {
                    char nbuf[128] = {};
                    sscanf(cmd + 6, "%127s", nbuf);
                    procName = nbuf;
                }
                if (tPid == targetPid || (!targetPkg.empty() && strstr(line, targetPkg.c_str()))) {
                    matched = true;
                }
            }

            if (strstr(line, "signal ")) {
                sigStr = line;
                if (!sigStr.empty() && sigStr.back() == '\n') sigStr.pop_back();
                char *fa = strstr(line, "fault addr ");
                if (fa) {
                    char abuf[64] = {};
                    sscanf(fa + 11, "%63s", abuf);
                    faultAddr = abuf;
                }
            }

            if (strstr(line, "pc  ") && strstr(line, "lr  ")) {
                char *pcP = strstr(line, "pc  ");
                if (pcP) {
                    char abuf[64] = {};
                    sscanf(pcP + 4, "%63s", abuf);
                    pcStr = abuf;
                }
                char *lrP = strstr(line, "lr  ");
                if (lrP) {
                    char abuf[64] = {};
                    sscanf(lrP + 4, "%63s", abuf);
                    lrStr = abuf;
                }
                char *spP = strstr(line, "sp  ");
                if (spP) {
                    char abuf[64] = {};
                    sscanf(spP + 4, "%63s", abuf);
                    spStr = abuf;
                }
            }

            if (strstr(line, "backtrace:") || strstr(line, "Backtrace:")) {
                inBacktrace = true;
                continue;
            }

            if (inBacktrace) {
                if (line[0] == '#' && bt.size() < 12) {
                    std::string btLine = line;
                    if (!btLine.empty() && btLine.back() == '\n') btLine.pop_back();
                    bt.push_back(btLine);
                } else if (line[0] != '#' && line[0] != ' ' && !bt.empty()) {
                    inBacktrace = false;
                }
            }
        }
        fclose(f);

        if (matched) {
            out.hasCrash = true;
            out.pid = tPid;
            out.processName = procName;
            out.signal = sigStr;
            out.faultAddr = faultAddr;
            out.pc = pcStr;
            out.lr = lrStr;
            out.sp = spStr;
            out.backtrace = bt;

            std::string sum = "Crash: " + (procName.empty() ? std::to_string(tPid) : procName) + "\n";
            sum += "  Signal: " + sigStr + "\n";
            if (!faultAddr.empty()) sum += "  Fault Address: " + faultAddr + "\n";
            if (!pcStr.empty()) sum += "  PC: " + pcStr + " | LR: " + lrStr + " | SP: " + spStr + "\n";
            if (!bt.empty()) {
                sum += "  Backtrace:\n";
                for (const auto &b : bt) {
                    sum += "    " + b + "\n";
                }
            }
            out.rawSummary = sum;
            return true;
        }
    }

    return false;
}

} // namespace eng
