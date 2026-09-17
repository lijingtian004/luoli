// Unix abstract socket 服务 + 命令分发
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "driver/twt_driver.h"
#include "freezer.h"
#include "guard.h"
#include "scanner.h"
#include "rules.h"
#include "il2cpp.h"

namespace eng {

class Server {
public:
    Server(twt::Driver &driver);
    ~Server();

    // 监听 abstract socket，阻塞运行（Ctrl 信号退出）
    bool run(const std::string &socketName);

private:
    void handleConnection(int fd);
    // 处理一条 JSON 请求，返回 JSON 响应
    std::string dispatch(const std::string &req);

    twt::Driver &driver_;
    int listenFd_ = -1;
    std::atomic<bool> stopping_{false};

    // 引擎状态（多连接共享）
    std::mutex stateMu_;
    pid_t pid_ = 0;
    std::string procName_;
    std::vector<Hit> hits_;
    std::mutex hitsMu_;
    std::atomic<bool> scanning_{false};

    Scanner scanner_;
    Freezer freezer_;
    Guard guard_;
    RuleEngine ruleEngine_{scanner_};
    Il2CppInspector il2cppInspector_{scanner_};
};

} // namespace eng
