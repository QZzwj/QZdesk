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
#define KO_DIR_ENV "QZDESK_WIFI_KO_DIR"

static char cached_iface[32];
static bool iface_resolved;
static bool scan_pending;
static unsigned long scan_started_ms;
static char scan_note[96];   /* 最近一次扫描的结果说明（Echo-Mate 的 last_note 做法） */
static pthread_t wifi_thread;
static pthread_mutex_t wifi_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wifi_cond = PTHREAD_COND_INITIALIZER;
static bool worker_running;
static bool worker_stop_requested;
static int worker_command;
static char worker_ssid[64];
static char worker_password[64];
static qz_wifi_status_t worker_status = { QZ_WIFI_OFF, "", "" };

/** 连接失败的具体原因（给界面显示）；空字符串表示没有失败。 */
static char join_reason[96];

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
                set_worker_status(QZ_WIFI_FAILED, ssid,
                                  join_reason[0] ? join_reason : "无法加入网络");
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

/* 开机后有一小段时间网卡还不存在：核心刚起、S30qzdesk 正在 insmod 驱动与补固件
 * （WiFi 模块加载 + udev 建立 wlan0 通常要几秒）。这段窗口里说"设备未提供无线网卡"
 * 是错的，会让人以为硬件不在 —— 宽限期内一律说"正在检测"。 */
#define NIC_GRACE_MS 30000UL
/* 驱动在（模块文件存在或已 insmod）时，说明这块板子确实有无线网卡：SDIO 探测 +
 * 固件下载慢起来要几十秒，这段时间里一律说"正在检测"，别说"没有网卡"。
 * 只有确定驱动都不在（真的没这块硬件）才按 NIC_GRACE_MS 判死。
 * NIC_EXPECT_MAX_MS 是兜底上限，防止驱动坏掉时永远停在"正在检测"。 */
#define NIC_EXPECT_MAX_MS 180000UL
#define NIC_LOAD_INTERVAL_MS 5000UL
static unsigned long nic_first_probe_ms;
static unsigned long nic_last_load_ms;

/** RTL8723BS 驱动模块所在目录；两处都找过（我们随 oem 装的那份优先）。 */
static const char *wifi_ko_dir(void)
{
    const char *forced = getenv(KO_DIR_ENV);       /* 测试用：指向别处 */
    if (forced && forced[0] != '\0') return forced;
    static const char *const dirs[] = { "/oem/usr/lib/wifi", "/lib/modules/wifi" };
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char path[128];
        snprintf(path, sizeof(path), "%s/r8723bs.ko", dirs[i]);
        if (access(path, F_OK) == 0) return dirs[i];
    }
    return NULL;
}

static bool module_loaded(const char *name)
{
    FILE *fp = fopen("/proc/modules", "r");
    if (!fp) return false;
    char line[256];
    size_t length = strlen(name);
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, name, length) == 0 && line[length] == ' ') {
            found = true;
            break;
        }
    }
    fclose(fp);
    return found;
}

/** 这块板子是否"应该有"无线网卡（驱动在场）。 */
static bool nic_expected(void)
{
    return wifi_ko_dir() != NULL || module_loaded("r8723bs");
}

/**
 * 网卡还没出现时,自己把驱动模块 insmod 上去。
 *
 * 正常开机由 S30qzdesk 在起界面前加载；这里兜底的是"开机脚本没更新/没跑到"
 * 的情况（比如只推了模块、没推 init 脚本），让用户不必去命令行手动 insmod。
 * 整条命令丢后台执行并保持顺序（mac80211 依赖 cfg80211），免得 insmod 的
 * 几百毫秒卡住界面线程；5 秒内只尝试一次。
 */
static void nic_autoload(void)
{
    if (module_loaded("r8723bs")) return;
    const char *dir = wifi_ko_dir();
    if (!dir) return;
    unsigned long now = monotonic_ms();
    if (now - nic_last_load_ms < NIC_LOAD_INTERVAL_MS) return;
    nic_last_load_ms = now;

    char command[320];
    size_t used = 0;
    static const char *const modules[] = { "cfg80211", "r8723bs" };
    command[0] = '\0';
    for (unsigned i = 0; i < sizeof(modules) / sizeof(modules[0]); i++) {
        if (module_loaded(modules[i])) continue;
        used += (size_t)snprintf(command + used, sizeof(command) - used,
                                 "%sinsmod %s/%s.ko", used ? "; " : "", dir, modules[i]);
        if (used >= sizeof(command)) break;
    }
    if (used == 0) return;
    char background[352];
    snprintf(background, sizeof(background), "(%s) >/dev/null 2>&1 &", command);
    run_shell(background);
}

bool qz_wifi_nic_pending(void)
{
    char iface[32];
    qz_wifi_interface(iface, sizeof(iface));       /* 顺带触发一次探测（结果会被缓存） */
    if (iface[0] != '\0') return false;            /* 已经找到网卡了 */
    if (nic_first_probe_ms == 0) nic_first_probe_ms = monotonic_ms();
    unsigned long waited = monotonic_ms() - nic_first_probe_ms;
    if (nic_expected()) return waited < NIC_EXPECT_MAX_MS;
    return waited < NIC_GRACE_MS;
}

/** 网卡缺失时该显示的文案：能等到网卡就说"正在检测"，确定没有才说"未提供"。 */
const char *qz_wifi_nic_text(void)
{
    if (qz_wifi_nic_pending()) return "正在检测无线网卡…";
    if (nic_expected()) return "无线网卡未就绪（驱动已加载）";
    return "设备未提供无线网卡";
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
        /* 只缓存"找到了网卡"的结果。空结果如果被永久缓存，驱动模块晚加载
         * （比如开机后再 insmod / 插 USB 网卡）时界面会一直报"设备未提供无线
         * 网卡"，非得重启界面才能恢复 —— 留着重试即可，探测本身很便宜。 */
        if (cached_iface[0] != '\0') {
            iface_resolved = true;
        } else {
            /* 没找到就问一句"模块在吗"，在就自己加载（后台执行，不卡界面）。 */
            nic_autoload();
        }
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

/* 入网段里的助手，启停也要用（声明在前，定义在后） */
static const char *persist_conf_path(void);
static void install_conf(void);

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

    /* 有持久副本（上次配过的网络）就先装回去，开机能自动回连 */
    if (access(persist_conf_path(), F_OK) == 0) install_conf();
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
/* scan helpers                                                               */
/* ------------------------------------------------------------------------- */

/**
 * 把外部命令解析成绝对路径。
 *
 * 移植自 Echo-Mate 的 sys_wifi_tool_path：设备上的 PATH 常常不含 /usr/sbin，而
 * iw / wpa_supplicant / udhcpc 通常正在那里 —— 直接执行会 "not found"。找不到
 * 就原样返回，交给 PATH 去试。
 */
static const char *tool_path(const char *name, char *buffer, size_t size)
{
    static const char *const dirs[] = { "/usr/sbin", "/sbin", "/usr/bin", "/bin" };
    size_t i;

    if (!name || !buffer || size == 0) return name;
    if (strchr(name, '/')) return name;          /* 已经是路径 */
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        snprintf(buffer, size, "%s/%s", dirs[i], name);
        if (access(buffer, X_OK) == 0) return buffer;
    }
    snprintf(buffer, size, "%s", name);
    return buffer;
}

/** dBm -> 0..100（Echo-Mate 的公式：2*(dBm+100)）。 */
static int percent_from_dbm(int dbm)
{
    int percent = 2 * (dbm + 100);

    if (percent < 0) return 0;
    if (percent > 100) return 100;
    return percent;
}

/** Quality=57/70 这种 -> 0..100。 */
static int percent_from_quality(int quality, int max)
{
    if (max <= 0) return percent_from_dbm(quality);
    if (quality < 0) quality = 0;
    if (quality > max) quality = max;
    return quality * 100 / max;
}

/** 百分比 -> 界面上的 1..4 格。 */
static int bars_for_percent(int percent)
{
    if (percent >= 75) return 4;
    if (percent >= 50) return 3;
    if (percent >= 25) return 2;
    return 1;
}

/** 同 SSID 取最强、按信号降序。两种扫描路径共用，保证"一个网络一行"。 */
static void ap_merge(qz_wifi_ap_t *output, int *count, int max,
                     const char *ssid, int percent, bool locked)
{
    int i;

    if (!output || !count || !ssid || ssid[0] == '\0') return;
    for (i = 0; i < *count; i++) {
        if (strcmp(output[i].ssid, ssid) != 0) continue;
        if (percent > output[i].percent) {
            output[i].percent = percent;
            output[i].bars = bars_for_percent(percent);
        }
        output[i].locked = output[i].locked || locked;
        return;
    }
    if (*count >= max) return;
    snprintf(output[*count].ssid, sizeof(output[*count].ssid), "%.63s", ssid);
    output[*count].percent = percent;
    output[*count].bars = bars_for_percent(percent);
    output[*count].locked = locked;
    (*count)++;
}

static void ap_sort(qz_wifi_ap_t *output, int count)
{
    for (int i = 1; i < count; i++) {
        qz_wifi_ap_t key = output[i];
        int j = i - 1;
        while (j >= 0 && output[j].percent < key.percent) {
            output[j + 1] = output[j];
            j--;
        }
        output[j + 1] = key;
    }
}

/** `iw dev X scan` 输出解析：BSS 起条，SSID:/signal:/RSN:/WPA:/Privacy。 */
static int parse_iw(const char *text, qz_wifi_ap_t *output, int max)
{
    char line[512];
    char ssid[64] = "";
    int percent = 0;
    bool locked = false;
    bool in_bss = false;
    int count = 0;
    const char *cursor = text;

    if (!text) return 0;
    while (*cursor) {
        size_t length = 0;
        while (cursor[length] && cursor[length] != '\n' && length < sizeof(line) - 1) length++;
        memcpy(line, cursor, length);
        line[length] = '\0';
        cursor += length;
        if (*cursor == '\n') cursor++;

        if (strncmp(line, "BSS ", 4) == 0) {
            if (in_bss && ssid[0]) ap_merge(output, &count, max, ssid, percent, locked);
            ssid[0] = '\0';
            percent = 0;
            locked = false;
            in_bss = true;
            continue;
        }
        if (!in_bss && line[0] == ' ') continue;

        /* 缩进后的属性行 */
        {
            const char *attr = line;
            while (*attr == ' ' || *attr == '\t') attr++;
            if (strncmp(attr, "SSID:", 5) == 0) {
                snprintf(ssid, sizeof(ssid), "%.63s", attr + 5);
            } else if (strncmp(attr, "signal:", 7) == 0) {
                percent = percent_from_dbm(atoi(attr + 7));
            } else if (strncmp(attr, "RSN:", 4) == 0 || strncmp(attr, "WPA:", 4) == 0 ||
                       strncmp(attr, "Privacy", 7) == 0 || strstr(attr, "WEP") != NULL) {
                locked = true;
            }
        }
    }
    if (in_bss && ssid[0]) ap_merge(output, &count, max, ssid, percent, locked);
    ap_sort(output, count);
    return count;
}

/** `iwlist X scan` 输出解析（Echo-Mate 的兜底路径）。 */
static int parse_iwlist(const char *text, qz_wifi_ap_t *output, int max)
{
    char line[512];
    char ssid[64] = "";
    int percent = 0;
    bool locked = false;
    bool in_cell = false;
    int count = 0;
    const char *cursor = text;

    if (!text) return 0;
    while (*cursor) {
        size_t length = 0;
        while (cursor[length] && cursor[length] != '\n' && length < sizeof(line) - 1) length++;
        memcpy(line, cursor, length);
        line[length] = '\0';
        cursor += length;
        if (*cursor == '\n') cursor++;

        if (strncmp(line, "Cell ", 5) == 0 || strncmp(line, "          Cell", 14) == 0) {
            if (in_cell && ssid[0]) ap_merge(output, &count, max, ssid, percent, locked);
            ssid[0] = '\0';
            percent = 0;
            locked = false;
            in_cell = true;
            continue;
        }
        if (!in_cell) continue;

        if (strstr(line, "ESSID:") != NULL) {
            const char *start = strchr(line, '"');
            if (start) {
                const char *end = strchr(start + 1, '"');
                size_t n = end ? (size_t)(end - start - 1) : strlen(start + 1);
                if (n >= sizeof(ssid)) n = sizeof(ssid) - 1;
                memcpy(ssid, start + 1, n);
                ssid[n] = '\0';
            } else {
                const char *value = strstr(line, "ESSID:");
                snprintf(ssid, sizeof(ssid), "%.63s", value + 6);
            }
        } else if (strstr(line, "Quality=") != NULL) {
            int quality = 0;
            int qmax = 0;
            if (sscanf(strstr(line, "Quality=") + 8, "%d/%d", &quality, &qmax) == 2) {
                percent = percent_from_quality(quality, qmax);
            } else if (sscanf(strstr(line, "Quality=") + 8, "%d", &quality) == 1) {
                percent = percent_from_quality(quality, 100);
            }
        } else if (strstr(line, "Signal level=") != NULL) {
            percent = percent_from_dbm(atoi(strstr(line, "Signal level=") + 13));
        } else if (strstr(line, "Encryption key:") != NULL) {
            if (strstr(line, "Encryption key:on") != NULL) locked = true;
        } else if (strstr(line, "Privacy") != NULL || strstr(line, "RSN:") != NULL ||
                   strstr(line, "WPA:") != NULL) {
            locked = true;
        }
    }
    if (in_cell && ssid[0]) ap_merge(output, &count, max, ssid, percent, locked);
    ap_sort(output, count);
    return count;
}

/** `wpa_cli scan_results`：制表符分隔 bssid/freq/signal/flags/ssid。 */
static int parse_wpa_results(const char *text, qz_wifi_ap_t *output, int max)
{
    char buffer[8192];
    char *line;
    char *save = NULL;
    int count = 0;

    if (!text) return 0;
    snprintf(buffer, sizeof(buffer), "%.*s", (int)(sizeof(buffer) - 1), text);

    line = strtok_r(buffer, "\n", &save);
    if (line && strstr(line, "bssid") != NULL) line = strtok_r(NULL, "\n", &save);
    for (; line; line = strtok_r(NULL, "\n", &save)) {
        char *fields[5] = { 0 };
        int field = 0;
        char *cursor = line;

        if (strstr(line, "Selected interface") != NULL) continue;
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
        ap_merge(output, &count, max, fields[4], percent_from_dbm(atoi(fields[2])),
                 fields[3] && (strstr(fields[3], "WPA") || strstr(fields[3], "WEP") ||
                               strstr(fields[3], "RSN") || strstr(fields[3], "SAE")));
    }
    ap_sort(output, count);
    return count;
}

/* ------------------------------------------------------------------------- */
/* scanning                                                                   */
/* ------------------------------------------------------------------------- */

#define SCAN_CACHE_MAX 32

static qz_wifi_ap_t scan_cache[SCAN_CACHE_MAX];
static int scan_cache_count;

/** ② wpa_cli 路线：flush -> scan -> 轮询 scan_results（Echo-Mate 的做法）。 */
static int scan_via_wpa_cli(const char *iface)
{
    char command[300];
    char tool[64];
    char buffer[16384];
    const char *wpa_cli = tool_path("wpa_cli", tool, sizeof(tool));
    int count = 0;

    if (!qz_wifi_ready()) return 0;   /* 没有 supplicant 就没有这条路线 */

    snprintf(command, sizeof(command), "%s -i %s flush >/dev/null 2>&1", wpa_cli, iface);
    run_quiet(command);
    snprintf(command, sizeof(command), "%s -i %s scan >/dev/null 2>&1", wpa_cli, iface);
    run_quiet(command);

    /* Echo-Mate 的收敛判据：至少扫 3 秒，且连续 4 轮没有新网络才收网 ——
     * AP 是逐轮冒出来的，固定轮数会提前截断；"连续无新增"既扫得全又有硬上限。 */
    int stable = 0;
    for (int attempt = 0; attempt < 27; attempt++) {
        usleep(300000);
        snprintf(command, sizeof(command), "%s -i %s scan_results 2>/dev/null", wpa_cli, iface);
        if (!run_capture(command, buffer, sizeof(buffer))) continue;
        int fresh = parse_wpa_results(buffer, scan_cache, SCAN_CACHE_MAX);
        if (fresh > count) {
            count = fresh;
            stable = 0;
        } else if (attempt >= 9 && ++stable >= 4) {
            break;   /* 已扫满 3 秒且连续 4 轮无新增 */
        }
    }
    return count;
}

/**
 * 扫描三级回退（移植自 Echo-Mate 的 sys_wifi_scan）：
 *   ① `iw dev X scan`         —— 最干净、信息最全，优先；
 *   ② `wpa_cli` flush/scan/scan_results —— 没有 iw 或 iw 失败时；
 *   ③ `iwlist X scan`         —— 最后兜底（wireless-tools 不一定编进 rootfs）。
 * 任何一级拿到结果就收工。**这个函数会阻塞几秒，只能在 worker 线程调用。**
 */
static int scan_collect(const char *iface)
{
    char command[300];
    char tool[64];
    char buffer[16384];
    int count = 0;

    snprintf(command, sizeof(command), "%s dev %s scan 2>/dev/null",
             tool_path("iw", tool, sizeof(tool)), iface);
    if (run_capture(command, buffer, sizeof(buffer))) {
        count = parse_iw(buffer, scan_cache, SCAN_CACHE_MAX);
        if (count > 0) return count;
    }

    count = scan_via_wpa_cli(iface);
    if (count > 0) return count;

    snprintf(command, sizeof(command), "%s %s scan 2>/dev/null",
             tool_path("iwlist", tool, sizeof(tool)), iface);
    if (run_capture(command, buffer, sizeof(buffer))) {
        count = parse_iwlist(buffer, scan_cache, SCAN_CACHE_MAX);
    }
    return count;
}

bool qz_wifi_scan_start(void)
{
    char iface[32];

    qz_wifi_interface(iface, sizeof(iface));
    if (!interface_safe(iface)) {
        snprintf(scan_note, sizeof(scan_note), "无线网卡不可用");
        return false;
    }

    scan_cache_count = 0;
    scan_pending = true;
    scan_started_ms = monotonic_ms();
    scan_cache_count = scan_collect(iface);
    scan_pending = false;
    if (scan_cache_count > 0) {
        snprintf(scan_note, sizeof(scan_note), "发现 %d 个网络", scan_cache_count);
    } else {
        /* 无结果时把原因说清楚，别让用户对着空列表猜（Echo-Mate 的做法） */
        snprintf(scan_note, sizeof(scan_note), "扫描无结果（附近没有网络，或网卡还没就绪）");
    }
    return scan_cache_count > 0;
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

/** UI 线程只读缓存：扫描的几秒都在 worker 线程里做完了（原来这里每次轮询都要
 *  跑一遍 wpa_cli）。 */
int qz_wifi_scan_results(qz_wifi_ap_t *output, int max_count)
{
    int count;

    if (!output || max_count <= 0) return 0;
    count = scan_cache_count < max_count ? scan_cache_count : max_count;
    if (count > 0) memcpy(output, scan_cache, sizeof(qz_wifi_ap_t) * (size_t)count);
    return count;
}

/** 最近一次扫描的结果说明（空状态时显示，让用户知道"为什么没有列表"）。 */
const char *qz_wifi_scan_note(void)
{
    return scan_note;
}

/** wlan0 上的 IPv4（Echo-Mate 的判定：有没有 IP 就是有没有连上），无则空串。 */
void qz_wifi_ipv4(char *output, unsigned int size)
{
    char iface[32];
    char command[200];
    char buffer[2048];
    const char *start;
    const char *end;

    if (size == 0) return;
    output[0] = '\0';
    qz_wifi_interface(iface, sizeof(iface));
    if (!interface_safe(iface)) return;
    snprintf(command, sizeof(command), "ifconfig %s 2>/dev/null", iface);
    if (!run_capture(command, buffer, sizeof(buffer))) return;
    start = strstr(buffer, "inet addr:");
    if (start) start += 10;
    else {
        start = strstr(buffer, "inet ");
        if (!start) return;
        start += 5;
    }
    end = start;
    while ((*end >= '0' && *end <= '9') || *end == '.') end++;
    if (end == start) return;
    snprintf(output, size, "%.*s", (int)(end - start), start);
}

/** wlan0 的 MAC 地址（读 sysfs，不需要任何外部工具），无则空串。 */
void qz_wifi_mac(char *output, unsigned int size)
{
    char iface[32];
    char path[96];
    FILE *file;

    if (size == 0) return;
    output[0] = '\0';
    qz_wifi_interface(iface, sizeof(iface));
    if (!interface_safe(iface)) return;
    snprintf(path, sizeof(path), "/sys/class/net/%.31s/address", iface);
    file = fopen(path, "r");
    if (!file) return;
    if (fgets(output, (int)size, file)) {
        char *nl = strchr(output, '\n');
        if (nl) *nl = '\0';
    }
    fclose(file);
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

/** 持久副本：Echo-Mate 把配网写在自己的数据目录下（升级不丢），再 cp 到
 *  supplicant 实际读取的路径。/userdata 不可写时退化成只写运行副本。 */
static const char *persist_conf_path(void)
{
    const char *forced = getenv("QZDESK_WIFI_PERSIST_CONF");
    if (forced && forced[0] != '\0') return forced;
    return "/userdata/qzdesk/wpa_supplicant.conf";
}

/** 接口上是否拿到 IPv4。只看 ifconfig，不依赖 wpa_cli（它可能没装）。 */
static bool has_ipv4(const char *iface)
{
    char command[200];
    char buffer[2048];

    snprintf(command, sizeof(command), "ifconfig %s 2>/dev/null", iface);
    if (!run_capture(command, buffer, sizeof(buffer))) return false;
    return strstr(buffer, "inet addr:") != NULL || strstr(buffer, "inet ") != NULL;
}

/** 写一份最小可用的 wpa_supplicant.conf：全局项 + 一个 network 块。
 *  不写明文 psk 之外的任何东西，凭据直接来自界面上的输入。 */
static bool write_supplicant_conf(const char *ssid, const char *password)
{
    const char *path = persist_conf_path();
    FILE *file;
    char dir[128];
    const char *slash;

    slash = strrchr(path, '/');
    if (slash && (size_t)(slash - path) < sizeof(dir)) {
        char command[200];
        memcpy(dir, path, (size_t)(slash - path));
        dir[slash - path] = '\0';
        snprintf(command, sizeof(command), "mkdir -p %s >/dev/null 2>&1", dir);
        run_quiet(command);
    }

    file = fopen(path, "w");
    if (!file) return false;
    fprintf(file, "ctrl_interface=%s\n", CTRL_DIR);
    fprintf(file, "ap_scan=1\n");
    fprintf(file, "update_config=1\n\n");
    fprintf(file, "network={\n");
    fprintf(file, "\tssid=\"%s\"\n", ssid);
    if (password && password[0] != '\0') {
        fprintf(file, "\tpsk=\"%s\"\n", password);
        fprintf(file, "\tkey_mgmt=WPA-PSK\n");
    } else {
        fprintf(file, "\tkey_mgmt=NONE\n");   /* 开放网络 */
    }
    fprintf(file, "\tscan_ssid=1\n");
    fprintf(file, "}\n");
    fclose(file);
    return true;
}

/** 把持久副本装到 supplicant 实际读取的路径。 */
static void install_conf(void)
{
    char command[400];

    snprintf(command, sizeof(command), "cp -f %s %s >/dev/null 2>&1",
             persist_conf_path(), conf_path());
    run_quiet(command);
}

/** 起网：ifconfig up -> 重启 supplicant 与 udhcpc（Echo-Mate 的路线）。 */
static bool bring_up(const char *iface)
{
    char command[300];
    char tool[64];

    snprintf(command, sizeof(command), "mkdir -p %s >/dev/null 2>&1", CTRL_DIR);
    run_quiet(command);
    snprintf(command, sizeof(command),
             "ifconfig %s up >/dev/null 2>&1 || ip link set %s up >/dev/null 2>&1",
             iface, iface);
    run_quiet(command);

    /* 只收掉 DHCP 客户端与 supplicant 本身，按进程名精确匹配，不误伤别的服务 */
    run_quiet("killall udhcpc >/dev/null 2>&1");
    snprintf(command, sizeof(command), "%s -i %s terminate >/dev/null 2>&1",
             tool_path("wpa_cli", tool, sizeof(tool)), iface);
    run_quiet(command);
    run_quiet("killall wpa_supplicant >/dev/null 2>&1");

    snprintf(command, sizeof(command), "%s -B -c %s -i %s >/dev/null 2>&1",
             tool_path("wpa_supplicant", tool, sizeof(tool)), conf_path(), iface);
    run_quiet(command);

    /* DHCP 放后台跑，前台只等结果 */
    snprintf(command, sizeof(command), "%s -i %s -b -q -n -t 4 >/dev/null 2>&1 &",
             tool_path("udhcpc", tool, sizeof(tool)), iface);
    run_shell(command);
    return true;
}

void qz_wifi_disconnect(void)
{
    char iface[32];
    char command[300];
    char tool[64];

    qz_wifi_interface(iface, sizeof(iface));
    if (!interface_safe(iface)) return;
    run_quiet("killall udhcpc >/dev/null 2>&1");
    snprintf(command, sizeof(command), "%s -i %s terminate >/dev/null 2>&1",
             tool_path("wpa_cli", tool, sizeof(tool)), iface);
    run_quiet(command);
    run_quiet("killall wpa_supplicant >/dev/null 2>&1");
    snprintf(command, sizeof(command), "ifconfig %s down >/dev/null 2>&1", iface);
    run_quiet(command);
    scan_pending = false;
}

bool qz_wifi_forget(void)
{
    char command[400];

    qz_wifi_disconnect();
    unlink(persist_conf_path());
    snprintf(command, sizeof(command), "rm -f %s >/dev/null 2>&1", conf_path());
    run_quiet(command);
    return true;
}

/**
 * 加入网络（**阻塞，最多 15s，只能在 worker 线程调用**）。
 *
 * 路线移植自 Echo-Mate 的 sys_wifi_connect：把 network 块写进
 * wpa_supplicant.conf（持久副本 + 运行副本），重启 supplicant 与 udhcpc，然后
 * 用"接口上是否拿到 IPv4"判定成功 —— 全程不依赖 wpa_cli 的 ctrl socket，所以
 * 在 wpa_cli 连不上（或压根没装）的板子上也能成。
 *
 * 与旧实现的关键差别：**失败会如实返回 false**。旧版等不到 wpa_state=COMPLETED
 * 时仍然 `return true`，界面会误报"连接成功"。
 */
/* ------------------------------------------------------------------------- *
 * 加入结果的判定（这里是"有 IP 却说失败 / 有 IP 却上不了网"的根源）
 * ------------------------------------------------------------------------- */

/** 读网卡当前 IPv4。169.254.* 是 DHCP 失败后的自分配地址，不算拿到地址。 */
static bool iface_ipv4(const char *iface, char *addr, size_t size)
{
    char command[96];
    char text[512];
    const char *p;
    size_t n = 0;

    if (size) addr[0] = '\0';
    snprintf(command, sizeof(command), "ifconfig %s 2>/dev/null", iface);
    if (!run_capture(command, text, sizeof(text))) return false;
    /* busybox 的 ifconfig 输出 "inet addr:192.168.1.5"；有些版本是 "inet 192.168.1.5" */
    p = strstr(text, "inet addr:");
    p = p ? p + 10 : strstr(text, "inet ");
    if (!p) return false;
    if (*p == ':') p++;
    while (*p == ' ') p++;
    while (p[n] && p[n] != ' ' && p[n] != '\n' && p[n] != '\t' && n + 1 < size) {
        addr[n] = p[n];
        n++;
    }
    if (size) addr[n] = '\0';
    if (n == 0) return false;
    if (strncmp(addr, "169.254.", 8) == 0) return false;
    return true;
}

/** 有没有走这块网卡的默认路由（有地址不等于能上网，缺路由是常见坑）。 */
static bool has_default_route(const char *iface)
{
    FILE *file = fopen("/proc/net/route", "r");
    char line[192];
    bool found = false;

    if (!file) return false;
    while (fgets(line, sizeof(line), file)) {
        char name[32];
        char destination[32];
        if (sscanf(line, "%31s %31s", name, destination) != 2) continue;
        if (strcmp(name, iface) != 0) continue;
        if (strcmp(destination, "00000000") != 0) continue;   /* 只有默认路由才算 */
        found = true;
        break;
    }
    fclose(file);
    return found;
}

/** wpa_supplicant 是否已完成四次握手（关联成功）。 */
static bool wpa_completed(const char *iface)
{
    char command[256];
    char text[512];
    char tool[128];

    snprintf(command, sizeof(command), "%.120s -i %.31s status 2>/dev/null",
             tool_path("wpa_cli", tool, sizeof(tool)), iface);
    if (!run_capture(command, text, sizeof(text))) return false;
    return strstr(text, "wpa_state=COMPLETED") != NULL;
}

bool qz_wifi_join(const char *ssid, const char *password)
{
    char iface[32];

    qz_wifi_interface(iface, sizeof(iface));
    if (!interface_safe(iface)) return false;
    if (!ssid || ssid[0] == '\0' || !shell_safe(ssid)) return false;
    if (!shell_safe(password ? password : "")) return false;
    /* WPA-PSK 的合法长度是 8..63；短于此一定是输错了，早点报错省 15 秒等待 */
    if (password && password[0] != '\0' &&
        (strlen(password) < 8 || strlen(password) > 63)) {
        return false;
    }

    if (!write_supplicant_conf(ssid, password)) return false;
    install_conf();
    bring_up(iface);

    /* 判定改成三件事同时成立才算连上（20 秒窗口）：
     *   ① 关联成功（wpa_state=COMPLETED）
     *   ② 拿到真地址（不是 169.254 的自分配地址）
     *   ③ 存在默认路由
     * 旧实现只看"有 IPv4"，于是出现两种误判：拿旧租约就报成功、缺默认路由还报成功。 */
    join_reason[0] = '\0';
    bool associated = false;
    bool dhcp_retried = false;
    for (int attempt = 0; attempt < 80; attempt++) {
        char address[32];
        bool has_ip = iface_ipv4(iface, address, sizeof(address));

        if (wpa_completed(iface)) associated = true;
        if (has_ip && has_default_route(iface)) return true;

        /* 已经关联却一直没地址：多半是 DHCP 没起来，补跑一次（只补一次，避免风暴） */
        if (associated && !has_ip && !dhcp_retried && attempt >= 8) {
            char command[128];
            snprintf(command, sizeof(command),
                     "udhcpc -i %s -n -q -t 5 >/dev/null 2>&1 &", iface);
            run_quiet(command);
            dhcp_retried = true;
        }
        usleep(250000);
    }

    {
        char address[32];
        if (!associated) {
            snprintf(join_reason, sizeof(join_reason),
                     "关联失败：检查密码，或热点用了 WPA3（本机不支持）");
        } else if (!iface_ipv4(iface, address, sizeof(address))) {
            snprintf(join_reason, sizeof(join_reason), "已关联但没拿到地址（DHCP 超时）");
        } else {
            snprintf(join_reason, sizeof(join_reason),
                     "已拿到地址但没有默认路由（只有本机链路）");
        }
    }
    return false;
}
