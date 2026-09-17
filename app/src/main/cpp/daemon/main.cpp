// 萝莉修改器 root daemon 入口
// 运行形态：su -c exec -a twt_svc /path/libengine.so --socket <name>
#include <getopt.h>
#include <signal.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <cstring>
#include <string>

#include "common.h"
#include "driver/twt_driver.h"
#include "logger.h"
#include "server.h"

using namespace eng;

int main(int argc, char **argv) {
    std::string socketName = kDefaultSocketName;
    std::string procName = kDefaultProcName;
    std::string logFile;

    int opt;
    while ((opt = getopt(argc, argv, "s:n:l:h")) != -1) {
        switch (opt) {
            case 's': socketName = optarg; break;
            case 'n': procName = optarg; break;
            case 'l': logFile = optarg; break;
            default:
                fprintf(stderr, "usage: %s [-s socket] [-n procname] [-l logfile]\n", argv[0]);
                return 1;
        }
    }

    // 隐身：进程名伪装（argv[0] 改写 + comm）
    if (argv[0] && strlen(argv[0]) > 0) {
        size_t argvLen = strlen(argv[0]);
        strncpy(argv[0], procName.c_str(), argvLen);
        memset(argv[0] + procName.size(), 0, argvLen > procName.size() ? argvLen - procName.size() : 0);
    }
    prctl(PR_SET_NAME, procName.c_str(), 0, 0, 0);

    signal(SIGPIPE, SIG_IGN);

    if (!logFile.empty()) Logger::get().setFile(logFile.c_str());
    LOGI("daemon " + std::string(kDaemonVersion) + " starting, socket=@" + socketName);

    // 驱动初始化（非致命：允许启动后通过 driver_status 观察）
    static twt::Driver driver;
    if (driver.init()) {
        LOGI("driver ready, mode=" + driver.mode());
    } else {
        LOGW("driver not available (will fail memory ops until loaded)");
    }

    Server server(driver);
    if (!server.run(socketName)) {
        LOGE("server run failed");
        return 1;
    }
    LOGI("daemon exit");
    return 0;
}
