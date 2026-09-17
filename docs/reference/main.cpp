#include "kernel.h"

#include <errno.h>
#include <linux/prctl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

struct tls_sample {
    pid_t tid;
    uint64_t tls;
};

struct tls_worker_context {
    int ready_fd;
    int release_fd;
};

struct pacga_sample {
    int32_t reset_error;
    int32_t kernel_error;
    uint64_t kernel_result;
};

static uint64_t read_tpidr_el0()
{
    uint64_t value;
    asm volatile("mrs %0, tpidr_el0" : "=r"(value));
    return value;
}

static bool read_full(int fd, void *buffer, size_t size)
{
    uint8_t *cursor = static_cast<uint8_t *>(buffer);
    size_t done = 0;

    while (done < size) {
        ssize_t bytes = read(fd, cursor + done, size - done);
        if (bytes > 0) {
            done += static_cast<size_t>(bytes);
            continue;
        }
        if (bytes < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

static bool write_full(int fd, const void *buffer, size_t size)
{
    const uint8_t *cursor = static_cast<const uint8_t *>(buffer);
    size_t done = 0;

    while (done < size) {
        ssize_t bytes = write(fd, cursor + done, size - done);
        if (bytes > 0) {
            done += static_cast<size_t>(bytes);
            continue;
        }
        if (bytes < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

static bool create_empty_file(const char *path)
{
    int file_fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (file_fd < 0)
        return false;
    close(file_fd);
    return true;
}

static bool directory_contains(const char *directory, const char *name)
{
    DIR *dir = opendir(directory);
    struct dirent *entry;
    bool found = false;

    if (!dir)
        return false;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, name) == 0) {
            found = true;
            break;
        }
    }
    closedir(dir);
    return found;
}

static void *tls_worker(void *opaque)
{
    tls_worker_context *context = static_cast<tls_worker_context *>(opaque);
    tls_sample sample = {};
    uint8_t release;

    sample.tid = static_cast<pid_t>(syscall(SYS_gettid));
    sample.tls = read_tpidr_el0();
    if (!write_full(context->ready_fd, &sample, sizeof(sample)))
        return nullptr;

    read_full(context->release_fd, &release, sizeof(release));
    return nullptr;
}

static bool test_current_thread_tls(c_driver &driver)
{
    const pid_t pid = getpid();
    const pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
    const uint64_t local_tls = read_tpidr_el0();
    uint64_t kernel_tls = 0;

    printf("\n=== Test: Current Thread TLS ===\n");
    if (!driver.get_thread_tls(pid, tid, &kernel_tls)) {
        fprintf(stderr, "get_thread_tls failed: %s\n", strerror(errno));
        return false;
    }

    printf("tid=%d local=0x%016llx kernel=0x%016llx\n", tid,
           static_cast<unsigned long long>(local_tls),
           static_cast<unsigned long long>(kernel_tls));
    if (local_tls != kernel_tls) {
        fprintf(stderr, "FAIL: current thread TLS mismatch\n");
        return false;
    }

    printf("PASS: current thread TLS matches TPIDR_EL0\n");
    return true;
}

static bool test_worker_thread_tls(c_driver &driver)
{
    int ready_pipe[2] = {-1, -1};
    int release_pipe[2] = {-1, -1};
    tls_worker_context context = {};
    tls_sample sample = {};
    pthread_t worker;
    bool worker_started = false;
    bool pass = false;
    uint8_t release = 1;
    uint64_t kernel_tls = 0;

    printf("\n=== Test: Worker Thread TLS ===\n");
    if (pipe(ready_pipe) != 0 || pipe(release_pipe) != 0) {
        fprintf(stderr, "pipe failed: %s\n", strerror(errno));
        goto out;
    }

    context.ready_fd = ready_pipe[1];
    context.release_fd = release_pipe[0];
    if (pthread_create(&worker, nullptr, tls_worker, &context) != 0) {
        fprintf(stderr, "pthread_create failed\n");
        goto out;
    }
    worker_started = true;

    if (!read_full(ready_pipe[0], &sample, sizeof(sample))) {
        fprintf(stderr, "failed to read worker TLS sample\n");
        goto release_worker;
    }

    if (!driver.get_thread_tls(getpid(), sample.tid, &kernel_tls)) {
        fprintf(stderr, "get_thread_tls(worker) failed: %s\n", strerror(errno));
        goto release_worker;
    }

    printf("tid=%d local=0x%016llx kernel=0x%016llx\n", sample.tid,
           static_cast<unsigned long long>(sample.tls),
           static_cast<unsigned long long>(kernel_tls));
    pass = sample.tls == kernel_tls;
    if (pass)
        printf("PASS: worker thread TLS matches TPIDR_EL0\n");
    else
        fprintf(stderr, "FAIL: worker thread TLS mismatch\n");

release_worker:
    write_full(release_pipe[1], &release, sizeof(release));
out:
    if (ready_pipe[0] >= 0)
        close(ready_pipe[0]);
    if (ready_pipe[1] >= 0)
        close(ready_pipe[1]);
    if (release_pipe[0] >= 0)
        close(release_pipe[0]);
    if (release_pipe[1] >= 0)
        close(release_pipe[1]);
    if (worker_started)
        pthread_join(worker, nullptr);
    return pass;
}

static bool test_pacga(c_driver &driver)
{
    const uint64_t value = 0x0123456789abcdefULL;
    const uint64_t modifier = 0xfedcba9876543210ULL;
    const uint64_t alternate_value = 0x89abcdef01234567ULL;
    uint64_t kernel_result = 0;
    uint64_t repeat_result = 0;
    uint64_t alternate_result = 0;

    printf("\n=== Test: PACGA ===\n");
    if (!driver.exec_pacga(getpid(), value, modifier, &kernel_result)) {
        fprintf(stderr, "exec_pacga failed: %s\n", strerror(errno));
        return false;
    }

    if (!driver.exec_pacga(getpid(), value, modifier, &repeat_result)) {
        fprintf(stderr, "second exec_pacga failed: %s\n", strerror(errno));
        return false;
    }
    if (!driver.exec_pacga(getpid(), alternate_value, modifier,
                           &alternate_result)) {
        fprintf(stderr, "alternate exec_pacga failed: %s\n", strerror(errno));
        return false;
    }

    printf("kernel=0x%016llx repeat=0x%016llx alternate=0x%016llx\n",
           static_cast<unsigned long long>(kernel_result),
           static_cast<unsigned long long>(repeat_result),
           static_cast<unsigned long long>(alternate_result));
    if (repeat_result != kernel_result) {
        fprintf(stderr, "FAIL: PACGA result is not deterministic\n");
        return false;
    }
    if ((kernel_result & 0xffffffffULL) != 0 ||
        (alternate_result & 0xffffffffULL) != 0) {
        fprintf(stderr, "FAIL: PACGA result is not encoded in the upper 32 bits\n");
        return false;
    }
    if (alternate_result == kernel_result) {
        fprintf(stderr, "FAIL: PACGA result did not change for different input\n");
        return false;
    }

    printf("PASS: kernel PACGA executed successfully\n");
    return true;
}

static bool test_remote_pacga(c_driver &driver)
{
    const uint64_t value = 0x0f1e2d3c4b5a6978ULL;
    const uint64_t modifier = 0x8877665544332211ULL;
    int ready_pipe[2] = {-1, -1};
    int release_pipe[2] = {-1, -1};
    pacga_sample sample = {};
    pid_t child = -1;
    uint8_t release = 1;
    uint64_t parent_before;
    uint64_t parent_after;
    uint64_t kernel_result = 0;
    bool pass = false;

    printf("\n=== Test: Remote Process PACGA ===\n");
    if (pipe(ready_pipe) != 0 || pipe(release_pipe) != 0) {
        fprintf(stderr, "pipe failed: %s\n", strerror(errno));
        goto out;
    }

    if (!driver.exec_pacga(getpid(), value, modifier, &parent_before)) {
        fprintf(stderr, "local kernel PACGA failed: %s\n", strerror(errno));
        goto out;
    }
    child = fork();
    if (child < 0) {
        fprintf(stderr, "fork failed: %s\n", strerror(errno));
        goto out;
    }

    if (child == 0) {
        uint8_t child_release;

        close(ready_pipe[0]);
        close(release_pipe[1]);
        if (prctl(PR_PAC_RESET_KEYS, PR_PAC_APGAKEY, 0, 0, 0) != 0)
            sample.reset_error = errno;
        if (!driver.exec_pacga(getpid(), value, modifier,
                               &sample.kernel_result))
            sample.kernel_error = errno;
        write_full(ready_pipe[1], &sample, sizeof(sample));
        read_full(release_pipe[0], &child_release, sizeof(child_release));
        _exit(sample.kernel_error == 0 ? 0 : 1);
    }

    close(ready_pipe[1]);
    ready_pipe[1] = -1;
    close(release_pipe[0]);
    release_pipe[0] = -1;

    if (!read_full(ready_pipe[0], &sample, sizeof(sample))) {
        fprintf(stderr, "failed to read child PACGA sample\n");
        goto release_child;
    }
    if (sample.reset_error != 0) {
        printf("INFO: child APGA reset unavailable: %s; "
               "testing inherited/shared key\n",
               strerror(sample.reset_error));
    }
    if (sample.kernel_error != 0) {
        fprintf(stderr, "child kernel PACGA failed: %s\n",
                strerror(sample.kernel_error));
        goto release_child;
    }

    if (!driver.exec_pacga(child, value, modifier, &kernel_result)) {
        fprintf(stderr, "exec_pacga(child) failed: %s\n", strerror(errno));
        goto release_child;
    }
    if (!driver.exec_pacga(getpid(), value, modifier, &parent_after)) {
        fprintf(stderr, "caller PACGA restore check failed: %s\n", strerror(errno));
        goto release_child;
    }

    printf("parent=0x%016llx child=0x%016llx kernel=0x%016llx\n",
           static_cast<unsigned long long>(parent_before),
           static_cast<unsigned long long>(sample.kernel_result),
           static_cast<unsigned long long>(kernel_result));
    if (sample.kernel_result != kernel_result) {
        fprintf(stderr, "FAIL: remote PACGA result mismatch\n");
        goto release_child;
    }
    if (sample.reset_error == 0 && sample.kernel_result == parent_before) {
        fprintf(stderr, "FAIL: APGA key reset did not change the PACGA result\n");
        goto release_child;
    }
    if (parent_before != parent_after) {
        fprintf(stderr, "FAIL: caller APGA key was not restored\n");
        goto release_child;
    }

    if (sample.reset_error == 0)
        printf("PASS: remote PACGA matches and caller APGA key is restored\n");
    else
        printf("PASS: remote PACGA matches using inherited/shared APGA key\n");
    pass = true;

release_child:
    write_full(release_pipe[1], &release, sizeof(release));
out:
    if (ready_pipe[0] >= 0)
        close(ready_pipe[0]);
    if (ready_pipe[1] >= 0)
        close(ready_pipe[1]);
    if (release_pipe[0] >= 0)
        close(release_pipe[0]);
    if (release_pipe[1] >= 0)
        close(release_pipe[1]);
    if (child > 0)
        waitpid(child, nullptr, 0);
    return pass;
}

static bool test_file_hide(c_driver &driver)
{
    char directory_template[] = "/data/local/tmp/twt_file_hide_XXXXXX";
    char fallback_template[] = "/tmp/twt_file_hide_XXXXXX";
    char hidden_path[PATH_MAX] = {};
    char visible_path[PATH_MAX] = {};
    char *directory = mkdtemp(directory_template);
    const char *hidden_name = "twt_hidden_marker";
    const char *visible_name = "twt_visible_marker";
    const char *keyword = "twt_hidden";
    bool configured = false;
    bool pass = false;

    printf("\n=== Test: File Hide ===\n");
    if (!directory)
        directory = mkdtemp(fallback_template);
    if (!directory) {
        fprintf(stderr, "mkdtemp failed: %s\n", strerror(errno));
        return false;
    }

    snprintf(hidden_path, sizeof(hidden_path), "%s/%s", directory,
             hidden_name);
    snprintf(visible_path, sizeof(visible_path), "%s/%s", directory,
             visible_name);
    if (!create_empty_file(hidden_path) || !create_empty_file(visible_path)) {
        fprintf(stderr, "failed to create file-hide fixtures: %s\n",
                strerror(errno));
        goto cleanup;
    }

    if (!directory_contains(directory, hidden_name) ||
        !directory_contains(directory, visible_name)) {
        fprintf(stderr, "failed to enumerate file-hide fixtures before setup\n");
        goto cleanup;
    }

    if (!driver.file_hide_set(directory, keyword)) {
        fprintf(stderr, "file_hide_set failed: %s\n", strerror(errno));
        goto cleanup;
    }
    configured = true;

    {
        twt_file_hide_status status = {};
        if (!driver.file_hide_status(&status)) {
            fprintf(stderr, "file_hide_status failed: %s\n", strerror(errno));
            goto cleanup;
        }
        printf("directory=%s keyword=%s enabled=%u hook_active=%u\n",
               status.directory, status.keyword, status.enabled,
               status.hook_active);
        if (status.enabled == 0 || status.hook_active == 0 ||
            strcmp(status.directory, directory) != 0 ||
            strcmp(status.keyword, keyword) != 0) {
            fprintf(stderr, "FAIL: file-hide status does not match configuration\n");
            goto cleanup;
        }
    }

    if (directory_contains(directory, hidden_name) ||
        !directory_contains(directory, visible_name)) {
        fprintf(stderr, "FAIL: file-hide enumeration did not filter keyword\n");
        goto cleanup;
    }

    if (!driver.file_hide_clear()) {
        fprintf(stderr, "file_hide_clear failed: %s\n", strerror(errno));
        goto cleanup;
    }
    configured = false;

    {
        twt_file_hide_status cleared = {};
        if (!driver.file_hide_status(&cleared)) {
            fprintf(stderr, "file-hide status after clear failed: %s\n",
                    strerror(errno));
            goto cleanup;
        }
        if (cleared.enabled != 0 || cleared.hook_active != 0) {
            fprintf(stderr, "FAIL: file-hide hook remains active after clear\n");
            goto cleanup;
        }
    }

    pass = directory_contains(directory, hidden_name) &&
           directory_contains(directory, visible_name);
    if (pass)
        printf("PASS: keyword-matching entry hidden and restored\n");
    else
        fprintf(stderr, "FAIL: hidden entry was not restored after clear\n");

cleanup:
    if (configured)
        driver.file_hide_clear();
    unlink(hidden_path);
    unlink(visible_path);
    rmdir(directory);
    return pass;
}

int main()
{
    c_driver driver(false);
    int passed = 0;
    int failed = 0;

    if (!driver.is_ready()) {
        fprintf(stderr, "driver is not ready\n");
        return 1;
    }

    driver.initialize(getpid());

    if (test_current_thread_tls(driver))
        ++passed;
    else
        ++failed;

    if (test_worker_thread_tls(driver))
        ++passed;
    else
        ++failed;

    if (test_pacga(driver))
        ++passed;
    else
        ++failed;

    if (test_remote_pacga(driver))
        ++passed;
    else
        ++failed;

    if (test_file_hide(driver))
        ++passed;
    else
        ++failed;

    printf("\n=== Results: %d passed, %d failed, 0 skipped ===\n",
           passed, failed);
    return failed == 0 ? 0 : 1;
}
