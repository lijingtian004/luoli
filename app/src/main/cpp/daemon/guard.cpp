#include "guard.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <cstdio>
#include <dirent.h>
#include <thread>

#include "logger.h"

namespace eng {

// 在 /proc 里按 cmdline 前缀找 App 进程
static bool procExistsByCmdline(const std::string &pkg) {
    DIR *dir = opendir("/proc");
    if (!dir) return false;
    struct dirent *de;
    bool found = false;
    char path[128], buf[256];
    while ((de = readdir(dir)) != nullptr) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        snprintf(path, sizeof(path), "/proc/%s/cmdline", de->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;
        buf[n] = '\0';
        if (strncmp(buf, pkg.c_str(), pkg.size()) == 0) { found = true; break; }
    }
    closedir(dir);
    return found;
}

// root 执行命令，最多等 5 秒
static int runCmd(const char *const argv[]) {
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    int st = 0;
    for (int i = 0; i < 50; ++i) {
        if (waitpid(p, &st, WNOHANG) == p) return st;
        usleep(100 * 1000);
    }
    kill(p, SIGKILL);
    waitpid(p, &st, 0);
    return -1;
}

void Guard::start(const std::string &pkg) {
    std::lock_guard<std::mutex> lk(mu_);
    if (running_) return;
    pkg_ = pkg;
    running_ = true;
    std::thread([this] { loop(); }).detach();
}

void Guard::stop() {
    std::lock_guard<std::mutex> lk(mu_);
    running_ = false;
}

bool Guard::running() {
    std::lock_guard<std::mutex> lk(mu_);
    return running_;
}

const std::string &Guard::pkg() {
    std::lock_guard<std::mutex> lk(mu_);
    return pkg_;
}

void Guard::loop() {
    LOGI("app guard started for " + pkg_);

    // root 加入电池优化白名单（对 ColorOS 也有帮助）
    {
        std::string full = "+" + pkg_;
        const char *argv[] = {"/system/bin/dumpsys", "deviceidle", "whitelist", full.c_str(), nullptr};
        runCmd(argv);
    }

    const std::string component = pkg_ + "/com.luoli.modifier.service.EngineService";
    int miss = 0;
    auto lastRevive = std::chrono::steady_clock::now();

    while (true) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!running_) break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(3));

        if (procExistsByCmdline(pkg_)) {
            miss = 0;
            continue;
        }
        if (++miss < 2) continue;

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastRevive).count() < 10000)
            continue;
        miss = 0;
        lastRevive = now;

        LOGW("app process dead, reviving service...");
        std::string svcArg = component;
        const char *argvSvc[] = {"/system/bin/am", "start-foreground-service", "-n", svcArg.c_str(), nullptr};
        runCmd(argvSvc);

        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (procExistsByCmdline(pkg_)) {
            LOGI("app revived via foreground service");
            continue;
        }

        LOGW("service start failed, launching activity...");
        std::string actArg = pkg_ + "/.MainActivity";
        const char *argvAct[] = {"/system/bin/am", "start", "-n", actArg.c_str(), nullptr};
        runCmd(argvAct);

        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (procExistsByCmdline(pkg_))
            LOGI("app revived via activity");
        else
            LOGE("revive failed (will retry)");
    }
    LOGI("app guard stopped");
}

} // namespace eng
