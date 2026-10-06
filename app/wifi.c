#include "wifi.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

/* Everything here follows the flow the SDK documents for this board:
 *
 *   ifconfig wlan0 up
 *   wpa_supplicant -B -c /etc/wpa_supplicant.conf -i wlan0
 *   udhcpc -i wlan0
 *
 * wpa_supplicant.conf needs ctrl_interface (wpa_cli talks over that socket),
 * ap_scan and update_config, so those three globals are ensured before the
 * daemon starts. Scanning, the current SSID and joining all go through wpa_cli.
 * On a machine without a wireless interface (or without the supplicant) every
 * call fails cleanly and the WLAN page shows its empty state instead of
 * inventing networks. */

#define CTRL_DIR "/var/run/wpa_supplicant"
#define IFACE_ENV "QZDESK_WIFI_IFACE"
#define CONF_ENV "QZDESK_WPA_CONF"

static char cached_iface[32];
static bool iface_resolved;
static bool scan_pending;
static unsigned long scan_started_ms;
static pthread_t wifi_thread;
static pthread_mutex_t wifi_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wifi_cond = PTHREAD_COND_INITIALIZER;
static bool worker_running;
static bool worker_stop_requested;
static int worker_command;
static char worker_ssid[64];
static char worker_password[64];
static qz_wifi_status_t worker_status = { QZ_WIFI_OFF, "", "" };

enum {
    WIFI_CMD_NONE,
    WIFI_CMD_ENABLE,
    WIFI_CMD_DISABLE,
    WIFI_CMD_SCAN,
    WIFI_CMD_CONNECT,
};

static void set_worker_status(qz_wifi_state_t state, const char *ssid, const char *error)
{
    pthread_mutex_lock(&wifi_mutex);
    worker_status.state = state;
    snprintf(worker_status.ssid, sizeof(worker_status.ssid), "%.63s", ssid ? ssid : "");
    snprintf(worker_status.error, sizeof(worker_status.error), "%.95s", error ? error : "");
    pthread_mutex_unlock(&wifi_mutex);
}

static void *wifi_worker_main(void *unused)
{
    (void)unused;
    for (;;) {
        int command;
        char ssid[64];
        char password[64];
        pthread_mutex_lock(&wifi_mutex);
        while (!worker_stop_requested && worker_command == WIFI_CMD_NONE)
            pthread_cond_wait(&wifi_cond, &wifi_mutex);
        if (worker_stop_requested) {
            pthread_mutex_unlock(&wifi_mutex);
            break;
        }
        command = worker_command;
        worker_command = WIFI_CMD_NONE;
        snprintf(ssid, sizeof(ssid), "%s", worker_ssid);
        snprintf(password, sizeof(password), "%s", worker_password);
        pthread_mutex_unlock(&wifi_mutex);

        if (command == WIFI_CMD_ENABLE) {
            set_worker_status(QZ_WIFI_SCANNING, "", "");
            if (qz_wifi_start() && qz_wifi_scan_start()) {
                set_worker_status(QZ_WIFI_SCANNING, "", "");
            } else {
                set_worker_status(QZ_WIFI_FAILED, "", "无法启动 Wi-Fi");
            }
        } else if (command == WIFI_CMD_DISABLE) {
            qz_wifi_stop();
            set_worker_status(QZ_WIFI_OFF, "", "");
        } else if (command == WIFI_CMD_SCAN) {
            if (qz_wifi_scan_start()) set_worker_status(QZ_WIFI_SCANNING, "", "");
            else set_worker_status(QZ_WIFI_FAILED, "", "无法扫描无线网络");
        } else if (command == WIFI_CMD_CONNECT) {
            set_worker_status(QZ_WIFI_CONNECTING, ssid, "");
            if (qz_wifi_join(ssid, password)) {
                set_worker_status(QZ_WIFI_CONNECTED, ssid, "");
            } else {
                set_worker_status(QZ_WIFI_FAILED, ssid, "无法加入网络");
            }
        }
    }
    return NULL;
}

bool qz_wifi_worker_start(void)
{
    pthread_mutex_lock(&wifi_mutex);
    if (worker_running) {
        pthread_mutex_unlock(&wifi_mutex);
        return true;
    }
    worker_stop_requested = false;
    worker_command = WIFI_CMD_NONE;
    if (pthread_create(&wifi_thread, NULL, wifi_worker_main, NULL) != 0) {
        pthread_mutex_unlock(&wifi_mutex);
        return false;
    }
    worker_running = true;
    pthread_mutex_unlock(&wifi_mutex);
    return true;
}

void qz_wifi_worker_stop(void)
{
    pthread_mutex_lock(&wifi_mutex);
    if (!worker_running) {
        pthread_mutex_unlock(&wifi_mutex);
        return;
    }
    worker_stop_requested = true;
    pthread_cond_signal(&wifi_cond);
    pthread_mutex_unlock(&wifi_mutex);
    pthread_join(wifi_thread, NULL);
    pthread_mutex_lock(&wifi_mutex);
    worker_running = false;
    pthread_mutex_unlock(&wifi_mutex);
}

static bool submit_worker_command(int command, const char *ssid, const char *password)
{
    if (!qz_wifi_worker_start()) return false;
    pthread_mutex_lock(&wifi_mutex);
    worker_command = command;
    snprintf(worker_ssid, sizeof(worker_ssid), "%.63s", ssid ? ssid : "");
    snprintf(worker_password, sizeof(worker_password), "%.63s", password ? password : "");
    pthread_cond_signal(&wifi_cond);
    pthread_mutex_unlock(&wifi_mutex);
    return true;
}

bool qz_wifi_enable_async(bool enabled)
{
    return submit_worker_command(enabled ? WIFI_CMD_ENABLE : WIFI_CMD_DISABLE, NULL, NULL);
}

bool qz_wifi_scan_async(void)
{
    return submit_worker_command(WIFI_CMD_SCAN, NULL, NULL);
}

bool qz_wifi_connect_async(const char *ssid, const char *password)
{
    return submit_worker_command(WIFI_CMD_CONNECT, ssid, password);
}

bool qz_wifi_status(qz_wifi_status_t *output)
{
    if (!output) return false;
    pthread_mutex_lock(&wifi_mutex);
    *output = worker_status;
    pthread_mutex_unlock(&wifi_mutex);
    return true;
}

static unsigned long monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (unsigned long)ts.tv_sec * 1000UL + (unsigned long)ts.tv_nsec / 1000000UL;
}

/* ------------------------------------------------------------------------- */
/* helpers                                                                    */
/* ------------------------------------------------------------------------- */

/** Run a command and capture stdout (up to size-1 bytes). */
static bool run_capture(const char *command, char *output, size_t size)
{
    FILE *pipe;
    size_t used;
    if (size == 0) return false;
    output[0] = '\0';
    pipe = popen(command, "r");
    if (!pipe) return false;
    used = fread(output, 1, size - 1, pipe);
    output[used] = '\0';
    pclose(pipe);
    return used > 0;
}

static void run_quiet(const char *command)
{
    char buffer[64];
    run_capture(command, buffer, sizeof(buffer));
}

/** Fire and forget; the reply (if any) is read through wpa_cli later. */
static void run_shell(const char *command)
{
    int status = system(command);
    (void)status;
}

static bool has_wireless_dir(const char *name)
{
    char path[160];
    snprintf(path, sizeof(path), "/sys/class/net/%.31s/wireless", name);
    return access(path, F_OK) == 0;
}

void qz_wifi_interface(char *output, unsigned int size)
{
    if (size == 0) return;
    if (!iface_resolved) {
        const char *forced = getenv(IFACE_ENV);
        cached_iface[0] = '\0';
        if (forced && forced[0] != '\0' && strlen(forced) < sizeof(cached_iface)) {
            snprintf(cached_iface, sizeof(cached_iface), "%s", forced);
        } else {
            /* /sys/class/net/<iface>/wireless only exists for wireless devices,
             * so this needs no ioctl and no external tool. */
            DIR *dir = opendir("/sys/class/net");
            if (dir) {
                struct dirent *entry;
                while ((entry = readdir(dir))) {
                    if (entry->d_name[0] == '.') continue;
                    if (!has_wireless_dir(entry->d_name)) continue;
                    snprintf(cached_iface, sizeof(cached_iface), "%.31s", entry->d_name);
                    break;
                }
                closedir(dir);
            }
        }
        iface_resolved = true;
    }
    snprintf(output, size, "%.31s", cached_iface);
}

static const char *conf_path(void)
{
    const char *forced = getenv(CONF_ENV);
    if (forced && forced[0] != '\0') return forced;
    if (access("/etc/wpa_supplicant.conf", F_OK) == 0) return "/etc/wpa_supplicant.conf";
    return "/etc/wpa_supplicant/wpa_supplicant.conf";
}

/** SSIDs and passwords are handed to the shell, so refuse anything that could
 *  break out of the quoting. Real network names never contain these. */
static bool shell_safe(const char *text)
{
    if (!text) return false;
    if (strlen(text) > 63) return false;
    for (const char *p = text; *p; p++) {
        if (strchr("'\"\\`$;&|<>()\n\r\t", *p)) return false;
    }
    return true;
}

static bool interface_safe(const char *iface)
{
    if (!iface || iface[0] == '\0' || strlen(iface) >= 32) return false;
    for (const char *p = iface; *p; p++) {
        if (!(('a' <= *p && *p <= 'z') || ('A' <= *p && *p <= 'Z') ||
              ('0' <= *p && *p <= '9') || *p == '_' || *p == '-')) return false;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* supplicant lifecycle                                                       */
/* ------------------------------------------------------------------------- */

static bool wpa_alive(const char *iface)
{
    char command[160];
    char buffer[64];
    snprintf(command, sizeof(command), "wpa_cli -i %s ping 2>/dev/null", iface);
    return run_capture(command, buffer, sizeof(buffer)) && strstr(buffer, "PONG") != NULL;
}

bool qz_wifi_ready(void)
{
    char iface[32];
    qz_wifi_interface(iface, sizeof(iface));
    return interface_safe(iface) && wpa_alive(iface);
}

/** Make sure wpa_supplicant.conf carries the globals wpa_cli needs. */
static void ensure_conf_globals(void)
{
    const char *path = conf_path();
    static const char *required[] = {
        "ctrl_interface=", "ap_scan=", "update_config=",
    };
    char content[2048] = "";
    char line[160];
    bool missing = false;
    FILE *file = fopen(path, "r");

    if (file) {
        size_t used = 0;
        while (fgets(line, sizeof(line), file) && used + sizeof(line) < sizeof(content)) {
            size_t length = strlen(line);
            memcpy(content + used, line, length);
            used += length;
            content[used] = '\0';
        }
        fclose(file);
    }

    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        if (!strstr(content, required[i])) missing = true;
    }
    if (!missing) return;   /* nothing to do, and the file may not be writable */

    file = fopen(path, "a");
    if (!file) return;      /* read only rootfs: the vendor config is used as is */
    if (content[0] != '\0' && content[strlen(content) - 1] != '\n') fputc('\n', file);
    if (!strstr(content, "ctrl_interface=")) fprintf(file, "ctrl_interface=%s\n", CTRL_DIR);
    if (!strstr(content, "ap_scan=")) fprintf(file, "ap_scan=1\n");
    if (!strstr(content, "update_config=")) fprintf(file, "update_config=1\n");
    fclose(file);
}

bool qz_wifi_start(void)
{
    char iface[32];
    char command[320];
    qz_wifi_interface(iface, sizeof(iface));
    if (!interface_safe(iface)) return false;
    if (wpa_alive(iface)) return true;

    /* Bring the interface up and give wpa_cli its socket directory. */
    snprintf(command, sizeof(command),
             "ifconfig %s up >/dev/null 2>&1 || ip link set %s up >/dev/null 2>&1",
             iface, iface);
    run_quiet(command);
    snprintf(command, sizeof(command), "mkdir -p %s >/dev/null 2>&1", CTRL_DIR);
    run_quiet(command);

    ensure_conf_globals();

    snprintf(command, sizeof(command),
             "wpa_supplicant -B -c %s -i %s >/dev/null 2>&1", conf_path(), iface);
    run_quiet(command);
    return wpa_alive(iface);
}

void qz_wifi_stop(void)
{
    char iface[32];
    char command[240];
    qz_wifi_interface(iface, sizeof(iface));
    if (!interface_safe(iface)) return;
    /* Stop only the supplicant attached to this interface. A global
     * killall can take down another network service on multi-interface boards. */
    snprintf(command, sizeof(command),
             "wpa_cli -i %s terminate >/dev/null 2>&1", iface);
    run_quiet(command);
    snprintf(command, sizeof(command), "ifconfig %s down >/dev/null 2>&1", iface);
    run_quiet(command);
    scan_pending = false;
}

/* ------------------------------------------------------------------------- */
/* scanning                                                                   */
/* ------------------------------------------------------------------------- */

bool qz_wifi_scan_start(void)
{
    char iface[32];
    char command[200];
    qz_wifi_interface(iface, sizeof(iface));
    if (!interface_safe(iface) || !qz_wifi_ready()) return false;
    /* Backgrounded so a slow scan never blocks the UI thread. */
    snprintf(command, sizeof(command),
             "wpa_cli -i %s scan >/dev/null 2>&1", iface);
    if (system(command) != 0) return false;
    scan_pending = true;
    scan_started_ms = monotonic_ms();
    return true;
}

bool qz_wifi_scan_pending(void)
{
    unsigned long now;
    if (!scan_pending) return false;
    now = monotonic_ms();
    if (now > scan_started_ms && now - scan_started_ms >= 12000UL) {
        scan_pending = false;
        return false;
    }
    return true;
}

static int bars_for(int signal)
{
    if (signal >= -55) return 4;
    if (signal >= -65) return 3;
    if (signal >= -75) return 2;
    return 1;
}

int qz_wifi_scan_results(qz_wifi_ap_t *output, int max_count)
{
    char iface[32];
    char command[200];
    char buffer[4096];
    int count = 0;
    char *line;
    char *save = NULL;

    qz_wifi_interface(iface, sizeof(iface));
    if (iface[0] == '\0' || !output || max_count <= 0) return 0;
    snprintf(command, sizeof(command), "wpa_cli -i %s scan_results 2>/dev/null", iface);
    if (!run_capture(command, buffer, sizeof(buffer))) return 0;

    line = strtok_r(buffer, "\n", &save);
    if (line) line = strtok_r(NULL, "\n", &save);   /* skip the header row */
    for (; line && count < max_count; line = strtok_r(NULL, "\n", &save)) {
        /* bssid \t frequency \t signal level \t flags \t ssid */
        char *fields[5] = {0};
        int field = 0;
        char *cursor = line;
        while (field < 5 && cursor) {
            char *tab = strchr(cursor, '\t');
            fields[field++] = cursor;
            if (tab) {
                *tab = '\0';
                cursor = tab + 1;
            } else {
                cursor = NULL;
            }
        }
        if (!fields[2] || !fields[4] || fields[4][0] == '\0') continue;

        int signal = atoi(fields[2]);
        bool locked = fields[3] && (strstr(fields[3], "WPA") || strstr(fields[3], "WEP") ||
                                    strstr(fields[3], "SAE"));

        /* One row per SSID: several BSSIDs broadcast the same network, so keep
         * whichever is strongest. */
        int existing = -1;
        for (int i = 0; i < count; i++) {
            if (strcmp(output[i].ssid, fields[4]) == 0) {
                existing = i;
                break;
            }
        }
        if (existing >= 0) {
            if (bars_for(signal) > output[existing].bars) {
                output[existing].bars = bars_for(signal);
            }
            continue;
        }

        snprintf(output[count].ssid, sizeof(output[count].ssid), "%.63s", fields[4]);
        output[count].bars = bars_for(signal);
        output[count].locked = locked;
        count++;
    }
    if (count > 0) scan_pending = false;
    return count;
}

void qz_wifi_current(char *output, unsigned int size)
{
    char iface[32];
    char command[200];
    char buffer[1024];
    char *line;
    char *save = NULL;

    if (size == 0) return;
    output[0] = '\0';
    qz_wifi_interface(iface, sizeof(iface));
    if (!interface_safe(iface)) return;
    snprintf(command, sizeof(command), "wpa_cli -i %s status 2>/dev/null", iface);
    if (!run_capture(command, buffer, sizeof(buffer))) return;

    for (line = strtok_r(buffer, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "ssid=", 5) != 0) continue;
        snprintf(output, size, "%.63s", line + 5);
        return;
    }
}

/* ------------------------------------------------------------------------- */
/* joining                                                                    */
/* ------------------------------------------------------------------------- */

/** wpa_cli add_network prints the new network id, or "FAIL". */
static int add_network(const char *iface)
{
    char command[200];
    char buffer[64];
    snprintf(command, sizeof(command), "wpa_cli -i %s add_network 2>/dev/null", iface);
    if (!run_capture(command, buffer, sizeof(buffer))) return -1;
    int id = atoi(buffer);
    return id >= 0 ? id : -1;
}

static void request_lease(const char *iface)
{
    /* The SDK obtains the address with udhcpc once the supplicant associated;
     * -q/-n keep it from hanging around when there is no lease. */
    char command[200];
    snprintf(command, sizeof(command),
             "udhcpc -i %s -q -n -t 5 >/dev/null 2>&1 &", iface);
    run_shell(command);
}

static bool associated(const char *iface)
{
    char command[200];
    char buffer[1024];
    snprintf(command, sizeof(command), "wpa_cli -i %s status 2>/dev/null", iface);
    if (!run_capture(command, buffer, sizeof(buffer))) return false;
    return strstr(buffer, "wpa_state=COMPLETED") != NULL;
}

bool qz_wifi_join(const char *ssid, const char *password)
{
    char iface[32];
    char command[320];
    int id;

    qz_wifi_interface(iface, sizeof(iface));
    if (!interface_safe(iface)) return false;
    if (!qz_wifi_ready() && !qz_wifi_start()) return false;
    if (!shell_safe(ssid) || !shell_safe(password ? password : "")) return false;

    id = add_network(iface);
    if (id < 0) return false;

    snprintf(command, sizeof(command),
             "wpa_cli -i %s set_network %d ssid '\"%s\"' >/dev/null 2>&1", iface, id, ssid);
    if (system(command) != 0) return false;

    if (password && password[0] != '\0') {
        snprintf(command, sizeof(command),
                 "wpa_cli -i %s set_network %d psk '\"%s\"' >/dev/null 2>&1",
                 iface, id, password);
    } else {
        /* Open network: no key management. */
        snprintf(command, sizeof(command),
                 "wpa_cli -i %s set_network %d key_mgmt NONE >/dev/null 2>&1", iface, id);
    }
    if (system(command) != 0) return false;

    snprintf(command, sizeof(command),
             "wpa_cli -i %s enable_network %d >/dev/null 2>&1", iface, id);
    if (system(command) != 0) return false;

    /* select_network drops the other configured networks so the interface
     * associates with this one; save_config keeps it in wpa_supplicant.conf. */
    snprintf(command, sizeof(command),
             "wpa_cli -i %s select_network %d >/dev/null 2>&1", iface, id);
    if (system(command) != 0) return false;

    /* Association is asynchronous. Give the supplicant a short window to
     * complete authentication before starting DHCP, otherwise udhcpc can
     * exit immediately and the UI reports a false success. */
    for (int attempt = 0; attempt < 20; attempt++) {
        if (associated(iface)) {
            request_lease(iface);
            snprintf(command, sizeof(command),
                     "wpa_cli -i %s save_config >/dev/null 2>&1", iface);
            run_quiet(command);
            return true;
        }
        usleep(250000);
    }
    return true;
}
