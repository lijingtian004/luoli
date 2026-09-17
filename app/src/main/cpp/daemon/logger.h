// 日志：内存环形缓冲（默认，UI 可读）+ 可选落盘（中性命名由调用方决定）
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace eng {

class Logger {
public:
    static constexpr size_t kMaxLines = 1024;

    struct Line {
        uint64_t seq;
        std::string text;
    };

    static Logger &get() {
        static Logger inst;
        return inst;
    }

    void setFile(const char *path) {
        std::lock_guard<std::mutex> lk(mu_);
        if (file_) { fclose(file_); file_ = nullptr; }
        if (path && *path) file_ = fopen(path, "a");
    }

    void log(const char *level, const std::string &msg) {
        std::lock_guard<std::mutex> lk(mu_);
        Line l;
        l.seq = ++seq_;
        l.text = std::string("[") + level + "] " + msg;
        lines_.push_back(std::move(l));
        if (lines_.size() > kMaxLines) lines_.pop_front();
        if (file_) {
            fprintf(file_, "%s\n", lines_.back().text.c_str());
            fflush(file_);
        }
    }

    // 返回 seq > afterSeq 的行
    std::vector<Line> since(uint64_t afterSeq) {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<Line> out;
        for (auto &l : lines_)
            if (l.seq > afterSeq) out.push_back(l);
        return out;
    }

private:
    std::mutex mu_;
    std::deque<Line> lines_;
    uint64_t seq_ = 0;
    FILE *file_ = nullptr;
};

#define LOGI(msg) ::eng::Logger::get().log("I", msg)
#define LOGW(msg) ::eng::Logger::get().log("W", msg)
#define LOGE(msg) ::eng::Logger::get().log("E", msg)

} // namespace eng
