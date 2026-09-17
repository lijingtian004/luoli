#include "invoker.h"

#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <elf.h>
#include <dirent.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <thread>

namespace eng {

#pragma pack(push, 8)
struct aarch64_user_pt_regs {
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
};
#pragma pack(pop)

static_assert(sizeof(aarch64_user_pt_regs) == 272, "arm64 pt_regs size mismatch");

bool RemoteInvoker::call(pid_t pid, uint64_t funcAddr, const std::vector<uint64_t> &args,
                         InvocationResult &out, uint32_t timeoutMs) {
    out = InvocationResult();
    if (pid <= 0 || funcAddr == 0) {
        out.err = "invalid pid or function address";
        return false;
    }

    // 选取目标进程的线程 (优先主线程或第一个工作线程)
    pid_t tid = pid;
    char taskDir[64];
    snprintf(taskDir, sizeof(taskDir), "/proc/%d/task", pid);
    DIR *d = opendir(taskDir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != nullptr) {
            if (de->d_name[0] >= '1' && de->d_name[0] <= '9') {
                tid = (pid_t)atoi(de->d_name);
                break;
            }
        }
        closedir(d);
    }

    if (ptrace(PTRACE_ATTACH, tid, nullptr, nullptr) != 0) {
        out.err = "ptrace attach failed: " + std::string(strerror(errno));
        return false;
    }

    int status = 0;
    waitpid(tid, &status, 0);

    aarch64_user_pt_regs orig_regs = {};
    iovec iov = { &orig_regs, sizeof(orig_regs) };
    if (ptrace(PTRACE_GETREGSET, tid, (void *)(uintptr_t)NT_PRSTATUS, &iov) != 0) {
        out.err = "ptrace get registers failed: " + std::string(strerror(errno));
        ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
        return false;
    }

    // 设置寄存器上下文
    aarch64_user_pt_regs call_regs = orig_regs;
    // 传参 X0..X7
    for (size_t i = 0; i < 8; ++i) {
        call_regs.regs[i] = (i < args.size()) ? args[i] : 0;
    }
    // LR = 0 (函数 ret 时触发 SIGSEGV 哨兵停下)
    call_regs.regs[30] = 0;
    // SP 16 字节向下对齐
    call_regs.sp = (orig_regs.sp - 128) & ~15ULL;
    // PC 指向函数入口
    call_regs.pc = funcAddr;

    iov.iov_base = &call_regs;
    iov.iov_len = sizeof(call_regs);
    if (ptrace(PTRACE_SETREGSET, tid, (void *)(uintptr_t)NT_PRSTATUS, &iov) != 0) {
        out.err = "ptrace set call registers failed: " + std::string(strerror(errno));
        ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
        return false;
    }

    // 继续线程运行
    if (ptrace(PTRACE_CONT, tid, nullptr, nullptr) != 0) {
        out.err = "ptrace cont failed: " + std::string(strerror(errno));
        iov.iov_base = &orig_regs;
        ptrace(PTRACE_SETREGSET, tid, (void *)(uintptr_t)NT_PRSTATUS, &iov);
        ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
        return false;
    }

    // 等待哨兵返回 (超时轮询)
    auto startTime = std::chrono::steady_clock::now();
    bool finished = false;

    while (true) {
        int r = waitpid(tid, &status, WNOHANG);
        if (r == tid) {
            finished = true;
            break;
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime).count();
        if (elapsed >= timeoutMs) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    if (!finished) {
        out.err = "invocation timed out (" + std::to_string(timeoutMs) + " ms)";
        // 强制挂起并恢复
        ptrace(PTRACE_ATTACH, tid, nullptr, nullptr);
        waitpid(tid, &status, 0);
        iov.iov_base = &orig_regs;
        ptrace(PTRACE_SETREGSET, tid, (void *)(uintptr_t)NT_PRSTATUS, &iov);
        ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
        return false;
    }

    // 读取返回值 X0
    aarch64_user_pt_regs ret_regs = {};
    iov.iov_base = &ret_regs;
    iov.iov_len = sizeof(ret_regs);
    ptrace(PTRACE_GETREGSET, tid, (void *)(uintptr_t)NT_PRSTATUS, &iov);

    out.retVal = ret_regs.regs[0];
    char hbuf[32];
    snprintf(hbuf, sizeof(hbuf), "0x%llx", (unsigned long long)out.retVal);
    out.retHex = hbuf;
    out.ok = true;

    // 恢复原始寄存器状态与现场
    iov.iov_base = &orig_regs;
    iov.iov_len = sizeof(orig_regs);
    ptrace(PTRACE_SETREGSET, tid, (void *)(uintptr_t)NT_PRSTATUS, &iov);
    ptrace(PTRACE_DETACH, tid, nullptr, nullptr);

    return true;
}

} // namespace eng
