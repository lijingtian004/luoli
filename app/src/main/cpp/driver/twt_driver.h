// TwT 内核驱动用户态对接层（非交互适配版）
// 基于 docs/reference/kernel.h 改造：去除交互式输入/exit/printf 菜单，
// 仅保留内存读写、进程定位、模块基址、FILE_HIDE、硬件断点能力。
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <linux/types.h>
#include <string>
#include <vector>
#include <atomic>

namespace twt {

// 通过被 hook 的 __NR_reboot 获取驱动 anon_inode fd（aarch64 专用）
#define TWT_MY_CALL(magic1, magic2, cmd, arg) ({    \
    long _ret;                                             \
    register long _x0 __asm__("x0") = (long)(magic1);      \
    register long _x1 __asm__("x1") = (long)(magic2);      \
    register long _x2 __asm__("x2") = (long)(cmd);         \
    register long _x3 __asm__("x3") = (long)(arg);         \
    register long _nr __asm__("x8") = __NR_reboot;         \
    __asm__ __volatile__(                                  \
        "svc #0"                                           \
        : "=r"(_x0)                                        \
        : "r"(_x0), "r"(_x1), "r"(_x2), "r"(_x3), "r"(_nr)\
        : "memory", "cc"                                   \
    );                                                     \
    _ret = _x0;                                            \
    _ret;                                                  \
})

#define TWT_MARK 'T'
#define GET_PID         _IOW(TWT_MARK, 0, twt_request)
#define MODULE_BASE     _IOW(TWT_MARK, 1, twt_request)
#define MODULE_BSS      _IOW(TWT_MARK, 3, twt_request)
#define READ_MEM        _IOW(TWT_MARK, 4, twt_request)
#define READ_MEM_V2     _IOW(TWT_MARK, 11, twt_request)
#define WRITE_MEM       _IOW(TWT_MARK, 5, twt_request)

// ---- FILE_HIDE（阶段三使用）----
#define TWT_FILE_HIDE_DIRECTORY_MAX 256
#define TWT_FILE_HIDE_KEYWORD_MAX   128
#define TWT_FILE_HIDE_PATH_MAX      TWT_FILE_HIDE_DIRECTORY_MAX

struct twt_file_hide_config {
    char directory[TWT_FILE_HIDE_DIRECTORY_MAX];
    char keyword[TWT_FILE_HIDE_KEYWORD_MAX];
    __u32 enabled;
};

struct twt_file_hide_status {
    char directory[TWT_FILE_HIDE_DIRECTORY_MAX];
    char keyword[TWT_FILE_HIDE_KEYWORD_MAX];
    __u32 enabled;
    __u32 hook_active;
};

static_assert(sizeof(twt_file_hide_config) == 388, "unexpected file-hide config ioctl ABI size");
static_assert(sizeof(twt_file_hide_status) == 392, "unexpected file-hide status ioctl ABI size");

#define FILE_HIDE_SET    _IOW(TWT_MARK, 31, struct twt_file_hide_config)
#define FILE_HIDE_CLEAR  _IO(TWT_MARK, 32)
#define FILE_HIDE_STATUS _IOR(TWT_MARK, 33, struct twt_file_hide_status)

// ---- 硬件断点（阶段三使用）----
#define BP_INIT_CMD        _IO(TWT_MARK, 19)
#define BP_CHECK_INITED    _IO(TWT_MARK, 30)
#define BP_GET_NUM_BRPS    _IO(TWT_MARK, 20)
#define BP_GET_NUM_WRPS    _IO(TWT_MARK, 21)
#define BP_INST            _IOWR(TWT_MARK, 22, char *)
#define BP_UNINST          _IOW(TWT_MARK, 23, char *)
#define BP_SUSPEND         _IOW(TWT_MARK, 24, char *)
#define BP_RESUME          _IOW(TWT_MARK, 25, char *)
#define BP_GET_HIT_COUNT   _IOWR(TWT_MARK, 26, char *)
#define BP_MODIFY          _IOW(TWT_MARK, 27, char *)
#define BP_GET_HIT_ITEMS   _IOWR(TWT_MARK, 28, char *)

#define HW_BREAKPOINT_EMPTY   0
#define HW_BREAKPOINT_R       1
#define HW_BREAKPOINT_W       2
#define HW_BREAKPOINT_RW      (HW_BREAKPOINT_R | HW_BREAKPOINT_W)
#define HW_BREAKPOINT_X       4

#define REG_MODIFY_X(N)     (1ULL << (N))
#define REG_MODIFY_SP     (1ULL << 31)
#define REG_MODIFY_PC     (1ULL << 32)
#define REG_MODIFY_PSTATE (1ULL << 33)

#pragma pack(1)
struct bp_user_pt_regs {
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
    uint64_t orig_x0;
    uint64_t syscallno;
    __uint128_t vregs[32];
};

struct bp_hit_item {
    uint64_t task_id;
    uint64_t hit_addr;
    uint64_t hit_time;
    struct bp_user_pt_regs regs_info;
};

struct bp_get_hit_count_arg {
    uint64_t handle;
    uint64_t hit_total_count;
    uint64_t hit_item_arr_count;
};

struct bp_get_hit_items_args {
    uint64_t handle;
    uint64_t user_buffer_ptr;
    uint64_t max_bytes;
    uint64_t items_copied;
};

struct bp_inst_args {
    int32_t pid;
    uint64_t addr;
    uint32_t bp_len;
    uint32_t bp_type;
    uint64_t reg_modify_mask;
    uint64_t fp_reg_modify_mask;
    uint64_t regs_to_set_ptr;
    uint32_t flags;
};

struct bp_modify_args {
    uint64_t handle;
    uint64_t reg_modify_mask;
    uint64_t fp_reg_modify_mask;
    uint64_t regs_to_set_ptr;
};
#pragma pack()

// 与内核 ABI 严格一致的请求结构
struct twt_request {
    pid_t pid;
    uintptr_t addr;
    void *buffer;
    size_t size;
};

class Driver {
public:
    Driver() = default;
    ~Driver() { if (fd_ >= 0) close(fd_); }

    Driver(const Driver &) = delete;
    Driver &operator=(const Driver &) = delete;

    // 非阻塞初始化：优先 magic syscall，失败则扫描 /proc/self/fd 兜底
    bool init() {
        if (ready()) return true;
        int fd = -1;
        TWT_MY_CALL(0x114514, 0x1919810, 0x2778, &fd);
        if (fd >= 0) {
            fd_ = fd;
            mode_ = "syscall";
        } else {
            fd_ = findDriverFd();
            mode_ = "procfd";
        }
        if (fd_ < 0) {
            mode_.clear();
            return false;
        }
        return true;
    }

    bool ready() const { return fd_ >= 0; }
    const std::string &mode() const { return mode_; }
    int fd() const { return fd_; }

    // 最近一次失败的 errno（诊断用）
    int lastErrno() const { return lastErrno_.load(); }

    bool readMem(pid_t pid, uintptr_t addr, void *buffer, size_t size) const {
        if (!ready() || pid <= 0 || !buffer || size == 0) { lastErrno_.store(EINVAL); return false; }
        twt_request req = {};
        req.pid = pid;
        req.addr = addr & 0xFFFFFFFFFFFFULL;
        req.buffer = buffer;
        req.size = size;
        if (ioctl(fd_, READ_MEM_V2, &req) != 0) {
            lastErrno_.store(errno);
            return false;
        }
        return true;
    }

    // V2 失败时回退 V1（个别内核版本 V2 行为不一致）
    bool readMemCompat(pid_t pid, uintptr_t addr, void *buffer, size_t size) const {
        if (readMem(pid, addr, buffer, size)) return true;
        int v2err = errno;
        if (!ready() || pid <= 0 || !buffer || size == 0) return false;
        twt_request req = {};
        req.pid = pid;
        req.addr = addr & 0xFFFFFFFFFFFFULL;
        req.buffer = buffer;
        req.size = size;
        if (ioctl(fd_, READ_MEM, &req) != 0) {
            lastErrno_.store(errno);
            lastV2Errno_.store(v2err);
            return false;
        }
        return true;
    }

    int lastV2Errno() const { return lastV2Errno_.load(); }

    bool writeMem(pid_t pid, uintptr_t addr, const void *buffer, size_t size) const {
        if (!ready() || pid <= 0 || !buffer || size == 0) { lastErrno_.store(EINVAL); return false; }
        twt_request req = {};
        req.pid = pid;
        req.addr = addr & 0xFFFFFFFFFFFFULL;
        req.buffer = const_cast<void *>(buffer);
        req.size = size;
        if (ioctl(fd_, WRITE_MEM, &req) != 0) {
            lastErrno_.store(errno);
            return false;
        }
        return true;
    }

    // 进程名 -> pid（内核侧实现，可找到被隐藏的进程）
    pid_t getPidByName(const char *name) const {
        if (!ready() || !name) return -1;
        twt_request req = {};
        char buf[0x100] = {};
        snprintf(buf, sizeof(buf), "%s", name);
        req.buffer = buf;
        if (ioctl(fd_, GET_PID, &req) != 0) return -1;
        return req.pid;
    }

    uintptr_t getModuleBase(pid_t pid, const char *name) const {
        if (!ready() || pid <= 0 || !name) return 0;
        twt_request req = {};
        char buf[0x100] = {};
        snprintf(buf, sizeof(buf), "%s", name);
        req.pid = pid;
        req.buffer = buf;
        if (ioctl(fd_, MODULE_BASE, &req) != 0) return 0;
        return req.addr;
    }

    uintptr_t getModuleBss(pid_t pid, const char *name) const {
        if (!ready() || pid <= 0 || !name) return 0;
        twt_request req = {};
        char buf[0x100] = {};
        snprintf(buf, sizeof(buf), "%s", name);
        req.pid = pid;
        req.buffer = buf;
        if (ioctl(fd_, MODULE_BSS, &req) != 0) return 0;
        return req.addr;
    }

    // ---- FILE_HIDE ----
    bool fileHideSet(const char *directory, const char *keyword) {
        if (!ready() || !directory || !keyword) { errno = EINVAL; return false; }
        size_t dlen = strnlen(directory, sizeof(twt_file_hide_config::directory));
        size_t klen = strnlen(keyword, sizeof(twt_file_hide_config::keyword));
        if (dlen == 0 || klen == 0 || dlen >= sizeof(twt_file_hide_config::directory) ||
            klen >= sizeof(twt_file_hide_config::keyword)) {
            errno = EINVAL;
            return false;
        }
        twt_file_hide_config cfg = {};
        memcpy(cfg.directory, directory, dlen);
        memcpy(cfg.keyword, keyword, klen);
        cfg.enabled = 1;
        return ioctl(fd_, FILE_HIDE_SET, &cfg) == 0;
    }

    bool fileHideClear() {
        if (!ready()) return false;
        return ioctl(fd_, FILE_HIDE_CLEAR, 0) == 0;
    }

    bool fileHideStatus(twt_file_hide_status *st) {
        if (!ready() || !st) return false;
        memset(st, 0, sizeof(*st));
        return ioctl(fd_, FILE_HIDE_STATUS, st) == 0;
    }

    // ---- 硬件断点 ----
    bool bpInit() { return ready() && ioctl(fd_, BP_INIT_CMD, 0) == 0; }
    bool bpCheckInited() { return ready() && ioctl(fd_, BP_CHECK_INITED, 0) == 0; }
    int bpNumBrps() { return ready() ? ioctl(fd_, BP_GET_NUM_BRPS, 0) : -1; }
    int bpNumWrps() { return ready() ? ioctl(fd_, BP_GET_NUM_WRPS, 0) : -1; }

    uint64_t bpInstall(pid_t pid, uint64_t addr, uint32_t len, uint32_t type, uint32_t flags = 0x1) {
        if (!ready()) return 0;
        bp_inst_args args = {};
        args.pid = pid;
        args.addr = addr;
        args.bp_len = len;
        args.bp_type = type;
        args.flags = flags;
        if (ioctl(fd_, BP_INST, &args) != 0) return 0;
        return args.addr;
    }

    bool bpUninstall(uint64_t handle) {
        return ready() && handle && ioctl(fd_, BP_UNINST, &handle) == 0;
    }

    bool bpSuspend(uint64_t handle) {
        return ready() && handle && ioctl(fd_, BP_SUSPEND, &handle) == 0;
    }

    bool bpResume(uint64_t handle) {
        return ready() && handle && ioctl(fd_, BP_RESUME, &handle) == 0;
    }

    bool bpGetHitCount(uint64_t handle, uint64_t *total, uint64_t *arrCount) {
        if (!ready() || !handle) return false;
        bp_get_hit_count_arg arg = {};
        arg.handle = handle;
        if (ioctl(fd_, BP_GET_HIT_COUNT, &arg) != 0) return false;
        if (total) *total = arg.hit_total_count;
        if (arrCount) *arrCount = arg.hit_item_arr_count;
        return true;
    }

    size_t bpGetHitItems(uint64_t handle, bp_hit_item *buf, size_t maxBytes) {
        if (!ready() || !handle || !buf || maxBytes == 0) return 0;
        bp_get_hit_items_args args = {};
        args.handle = handle;
        args.user_buffer_ptr = (uint64_t)buf;
        args.max_bytes = maxBytes;
        if (ioctl(fd_, BP_GET_HIT_ITEMS, &args) != 0) return 0;
        return (size_t)args.items_copied;
    }

    std::vector<bp_hit_item> bpGetHits(uint64_t handle, size_t maxItems = 100) {
        std::vector<bp_hit_item> result;
        if (!ready() || !handle || maxItems == 0) return result;
        uint64_t total = 0, arrCount = 0;
        if (!bpGetHitCount(handle, &total, &arrCount) || arrCount == 0) return result;
        size_t count = (arrCount < maxItems) ? (size_t)arrCount : maxItems;
        result.resize(count);
        size_t copied = bpGetHitItems(handle, result.data(), count * sizeof(bp_hit_item));
        result.resize(copied);
        return result;
    }

private:
    // 扫描 /proc/self/fd，寻找驱动 anon_inode
    static int findDriverFd() {
        DIR *dir = opendir("/proc/self/fd");
        if (!dir) return -1;
        int found = -1;
        char path[4096], link[4096];
        struct dirent *entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            snprintf(path, sizeof(path), "/proc/self/fd/%s", entry->d_name);
            ssize_t len = readlink(path, link, sizeof(link) - 1);
            if (len <= 0) continue;
            link[len] = '\0';
            if (strstr(link, "TwT_driver") != nullptr && strstr(link, "anon_inode:") != nullptr) {
                found = atoi(entry->d_name);
                break;
            }
        }
        closedir(dir);
        return found;
    }

    int fd_ = -1;
    std::string mode_;
    mutable std::atomic<int> lastErrno_{0};
    mutable std::atomic<int> lastV2Errno_{0};
};

} // namespace twt
