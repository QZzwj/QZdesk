#include "config.h"
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void qz_load_config(char *server, unsigned int server_size)
{
    const char *api_url = getenv("ECHO_API_URL");
    if (api_url && api_url[0] != '\0') {
        snprintf(server, server_size, "%s", api_url);
        return;
    }

    const char *path = getenv("ECHO_CONFIG");
    if (!path || path[0] == '\0') path = "/etc/echo/system_para.conf";
    FILE *config = fopen(path, "r");
    if (!config) return;

    char line[160], host[80] = "", port[16] = "8765";
    while (fgets(line, sizeof(line), config)) {
        if (sscanf(line, "AIChat_server_url=%79s", host) == 1) continue;
        sscanf(line, "AIChat_server_port=%15s", port);
    }
    fclose(config);
    if (host[0] != '\0') snprintf(server, server_size, "%s:%s", host, port);
}

void qz_device_ip(char *output, unsigned int output_size)
{
    struct ifaddrs *interfaces = NULL;
    struct ifaddrs *current;
    if (output_size == 0) return;
    snprintf(output, output_size, "未连接");
    if (getifaddrs(&interfaces) != 0) return;
    for (current = interfaces; current; current = current->ifa_next) {
        struct sockaddr_in *address;
        if (!current->ifa_addr || current->ifa_addr->sa_family != AF_INET ||
            !(current->ifa_flags & IFF_UP) || (current->ifa_flags & IFF_LOOPBACK)) continue;
        address = (struct sockaddr_in *)current->ifa_addr;
        if (inet_ntop(AF_INET, &address->sin_addr, output, output_size)) break;
    }
    freeifaddrs(interfaces);
}

int qz_battery_level(void)
{
    static const char *paths[] = {
        "/sys/class/power_supply/battery/capacity",
        "/sys/class/power_supply/bat0/capacity",
        "/sys/class/power_supply/axp20x-battery/capacity",
    };
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE *file = fopen(paths[i], "r");
        if (!file) continue;
        int level = -1;
        int matched = fscanf(file, "%d", &level);
        fclose(file);
        if (matched == 1 && level >= 0) return level > 100 ? 100 : level;
    }
    return -1;
}

/* ------------------------------------------------------------------------- *
 * Backlight ("echo 49 > /sys/class/backlight/backlight/brightness")
 * ------------------------------------------------------------------------- */

static const char *backlight_path(void)
{
    const char *forced = getenv("QZDESK_BACKLIGHT");
    if (forced && forced[0] != '\0') return forced;
    return "/sys/class/backlight/backlight/brightness";
}

static int read_sysfs_int(const char *path)
{
    FILE *file = fopen(path, "r");
    int value = -1;
    if (!file) return -1;
    if (fscanf(file, "%d", &value) != 1) value = -1;
    fclose(file);
    return value;
}

static int backlight_max(void)
{
    const char *path = backlight_path();
    char sibling[160];
    const char *slash = strrchr(path, '/');
    int maximum;
    if (!slash) return 100;
    snprintf(sibling, sizeof(sibling), "%.*s/max_brightness",
             (int)(slash - path), path);
    maximum = read_sysfs_int(sibling);
    return maximum > 0 ? maximum : 100;
}

int qz_backlight_level(void)
{
    int maximum = backlight_max();
    int value = read_sysfs_int(backlight_path());
    if (value < 0) return -1;
    if (value > maximum) value = maximum;
    return (value * 100 + maximum / 2) / maximum;
}

bool qz_backlight_set(int percent)
{
    int maximum = backlight_max();
    FILE *file;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    /* Never write 0: a fully dark panel looks like a hang. */
    int raw = percent == 0 ? 0 : (percent * maximum + 50) / 100;
    if (percent > 0 && raw < 1) raw = 1;
    file = fopen(backlight_path(), "w");
    if (!file) return false;
    fprintf(file, "%d", raw);
    fclose(file);
    return true;
}

/* ------------------------------------------------------------------------- *
 * Speaker ("amixer -c 0 cset name='DAC LINEOUT Volume' 18", range 0..30)
 * ------------------------------------------------------------------------- */

#define MIXER_CARD 0
#define MIXER_CONTROL "DAC LINEOUT Volume"
#define MIXER_MAX 30

/** Run a command and capture its output (up to size-1 bytes). */
static bool capture(const char *command, char *output, size_t size)
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

int qz_volume_level(void)
{
    char command[160];
    char buffer[512];
    const char *values;
    int value = -1;

    snprintf(command, sizeof(command),
             "amixer -c %d cget name='%s' 2>/dev/null", MIXER_CARD, MIXER_CONTROL);
    if (!capture(command, buffer, sizeof(buffer))) return -1;
    /* amixer prints the range on the "type=" line and the current reading on
     * the ": values=" line, so match the latter only. */
    values = strstr(buffer, ": values=");
    if (!values || sscanf(values + 9, "%d", &value) != 1) return -1;
    if (value < 0) return -1;
    if (value > MIXER_MAX) value = MIXER_MAX;
    return value * 100 / MIXER_MAX;
}

bool qz_volume_set(int percent)
{
    char command[160];
    int value;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    value = (percent * MIXER_MAX + 50) / 100;
    snprintf(command, sizeof(command),
             "amixer -c %d cset name='%s' %d >/dev/null 2>&1",
             MIXER_CARD, MIXER_CONTROL, value);
    return system(command) == 0;
}

/* ------------------------------------------------------------------------- *
 * Timezone ("export TZ=CST-8" in /etc/profile)
 * ------------------------------------------------------------------------- */

static const char *profile_path(void)
{
    const char *forced = getenv("QZDESK_TZ_PROFILE");
    if (forced && forced[0] != '\0') return forced;
    return "/etc/profile";
}

bool qz_timezone_apply(const char *posix_tz)
{
    const char *path = profile_path();
    char content[4096] = "";
    char line[256];
    FILE *file;
    size_t used = 0;
    bool replaced = false;

    if (!posix_tz || posix_tz[0] == '\0') return false;

    /* Takes effect for this process (the status bar clock reads localtime())
     * and for anything started afterwards. */
    setenv("TZ", posix_tz, 1);
    tzset();

    file = fopen(path, "r");
    if (file) {
        while (fgets(line, sizeof(line), file) && used + sizeof(line) < sizeof(content)) {
            size_t length = strlen(line);
            if (strncmp(line, "export TZ=", 10) == 0) {
                length = (size_t)snprintf(content + used, sizeof(content) - used,
                                          "export TZ=%s\n", posix_tz);
                replaced = true;
            } else {
                memcpy(content + used, line, length);
            }
            used += length;
            content[used] = '\0';
        }
        fclose(file);
    }

    file = fopen(path, replaced || content[0] == '\0' ? "w" : "a");
    if (!file) return true;   /* read only rootfs: the running process is set */
    if (replaced) {
        fputs(content, file);
    } else {
        if (content[0] != '\0' && content[strlen(content) - 1] != '\n') fputc('\n', file);
        fprintf(file, "export TZ=%s\n", posix_tz);
    }
    fclose(file);
    return true;
}

/* ------------------------------------------------------------------------- *
 * Appearance (dark mode)
 *
 * One line, one value, one file. The path prefers /etc/qzdesk so the device
 * keeps it with the rest of its config; the simulator falls back to $HOME so a
 * read-only source tree still persists the user's choice.
 * ------------------------------------------------------------------------- */

static const char *appearance_path(void)
{
    const char *forced = getenv("QZDESK_APPEARANCE_FILE");
    if (forced && forced[0] != '\0') return forced;
    const char *home = getenv("HOME");
    if (home && home[0] != '\0') {
        static char home_path[128];
        snprintf(home_path, sizeof(home_path), "%s/.qzdesk/appearance", home);
        return home_path;
    }
    return "/etc/qzdesk/appearance";
}

void qz_appearance_load(void)
{
    /* An explicit env var wins: it lets a tester force a theme without touching
     * the persisted file. */
    const char *current = getenv("QZDESK_DARK");
    if (current && current[0] != '\0') return;

    FILE *file = fopen(appearance_path(), "r");
    if (!file) return;
    char line[64];
    while (fgets(line, sizeof(line), file)) {
        /* Tolerate "QZDESK_DARK=1", "dark" or "1" — the writer uses the first
         * form so /etc/profile could source it too if needed. */
        if (strncmp(line, "QZDESK_DARK=", 12) == 0) {
            setenv("QZDESK_DARK", line + 12, 1);
            break;
        }
    }
    fclose(file);
}

bool qz_appearance_save(bool dark)
{
    setenv("QZDESK_DARK", dark ? "1" : "0", 1);

    const char *path = appearance_path();
    FILE *file = fopen(path, "w");
    if (!file) return true;   /* read-only fs: the running process is still set */
    fprintf(file, "QZDESK_DARK=%s\n", dark ? "1" : "0");
    fclose(file);
    return dark;
}
