#define _POSIX_C_SOURCE 200809L
#include "qzdesk_core_process.h"
#include <errno.h>
#include <arpa/inet.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static volatile sig_atomic_t stop_requested;
static pid_t started_core_pid = -1;

static void wait_for_core_exit(void)
{
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 50000000L};
    nanosleep(&delay, NULL);
}

static void qzdesk_stop_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void install_stop_handlers(void)
{
    struct sigaction action = {0};
    action.sa_handler = qzdesk_stop_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
}

static bool env_disabled(const char *name)
{
    const char *value = getenv(name);
    return value && (strcmp(value, "0") == 0 || strcasecmp(value, "false") == 0 ||
                     strcasecmp(value, "off") == 0);
}

static bool executable_file(const char *path)
{
    return path && access(path, X_OK) == 0;
}

static bool core_port_in_use(void)
{
    const char *value = getenv("QZDESK_CORE_PORT");
    unsigned long port = value && value[0] ? strtoul(value, NULL, 10) : 5678;
    struct sockaddr_in address = {0};
    int socket_fd;
    int result;
    if (port < 1 || port > 65535) port = 5678;
    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0) return false;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((unsigned short)port);
    result = bind(socket_fd, (const struct sockaddr *)&address, sizeof(address));
    close(socket_fd);
    return result < 0 && errno == EADDRINUSE;
}

static bool executable_directory(char *directory, size_t directory_size)
{
    char executable[PATH_MAX];
    ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    char *slash;
    if (length <= 0 || (size_t)length >= sizeof(executable)) return false;
    executable[length] = '\0';
    slash = strrchr(executable, '/');
    if (!slash) return false;
    *slash = '\0';
    snprintf(directory, directory_size, "%s", executable);
    return true;
}

bool qzdesk_core_start(void)
{
    const char *binary;
    char default_binary[PATH_MAX];
    char core_directory[PATH_MAX];
    pid_t child;

    install_stop_handlers();
    atexit(qzdesk_core_stop);

    if (env_disabled("QZDESK_CORE_AUTOSTART")) return false;
    if (core_port_in_use()) {
        fprintf(stderr, "QZdesk: core is already running; reusing it\n");
        return false;
    }
    binary = getenv("QZDESK_CORE_BIN");
    if (!binary || binary[0] == '\0') {
        if (executable_directory(core_directory, sizeof(core_directory))) {
            if (snprintf(default_binary, sizeof(default_binary), "%s/xiaozhi_linux_rs",
                         core_directory) >= (int)sizeof(default_binary)) {
                return false;
            }
            binary = default_binary;
        } else {
            binary = "./xiaozhi_linux_rs";
        }
    }
    if (!executable_file(binary)) {
        fprintf(stderr, "QZdesk: core not found at %s; start it separately\n", binary);
        return false;
    }

    child = fork();
    if (child < 0) {
        fprintf(stderr, "QZdesk: unable to start core: %s\n", strerror(errno));
        return false;
    }
    if (child == 0) {
        if (executable_directory(core_directory, sizeof(core_directory))) {
            if (chdir(core_directory) < 0) {
                fprintf(stderr, "QZdesk: unable to change core directory: %s\n",
                        strerror(errno));
            }
        }
        execl(binary, binary, (char *)NULL);
        _exit(127);
    }
    started_core_pid = child;
    fprintf(stderr, "QZdesk: core started (pid %ld)\n", (long)child);
    return true;
}

bool qzdesk_core_should_stop(void)
{
    return stop_requested != 0;
}

bool qzdesk_core_restart_requested(void)
{
    const char *flag = getenv("QZDESK_SIMULATOR_RESTART_FLAG");

    /* Real devices never inspect a filesystem restart marker. */
    return flag && flag[0] != '\0' && access(flag, F_OK) == 0;
}

void qzdesk_core_stop(void)
{
    int status;
    int waited_ms = 0;

    if (started_core_pid <= 0) return;

    if (kill(started_core_pid, SIGTERM) < 0 && errno != ESRCH) {
        fprintf(stderr, "QZdesk: unable to stop core: %s\n", strerror(errno));
    }
    while (waited_ms < 2000) {
        pid_t result = waitpid(started_core_pid, &status, WNOHANG);
        if (result == started_core_pid || (result < 0 && errno == ECHILD)) {
            started_core_pid = -1;
            return;
        }
        if (result < 0 && errno != EINTR) break;
        wait_for_core_exit();
        waited_ms += 50;
    }

    if (kill(started_core_pid, SIGKILL) < 0 && errno != ESRCH) {
        fprintf(stderr, "QZdesk: unable to force-stop core: %s\n", strerror(errno));
    }
    while (waitpid(started_core_pid, &status, 0) < 0 && errno == EINTR) {}
    started_core_pid = -1;
}
