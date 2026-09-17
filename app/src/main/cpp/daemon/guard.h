// App 进程守护：root daemon 监视 App 进程，死亡则由 root 拉活
#pragma once

#include <mutex>
#include <string>

namespace eng {

class Guard {
public:
    void start(const std::string &pkg);
    void stop();
    bool running();
    const std::string &pkg();

private:
    void loop();

    std::mutex mu_;
    bool running_ = false;
    std::string pkg_;
};

} // namespace eng
