#ifndef QZDESK_WIFI_H
#define QZDESK_WIFI_H

#include <stdbool.h>

/** One access point from the last scan. */
typedef struct {
    char ssid[64];
    int bars;     /**< 1..4, derived from the signal level. */
    int percent;  /**< 0..100 signal strength (dBm -> 2*(dBm+100), or Quality=n/m). */
    bool locked;  /**< needs a password (WPA / WPA2 / WPA3 / WEP). */
} qz_wifi_ap_t;

typedef enum {
    QZ_WIFI_OFF,
    QZ_WIFI_SCANNING,
    QZ_WIFI_IDLE,
    QZ_WIFI_CONNECTING,
    QZ_WIFI_OBTAINING_IP,
    QZ_WIFI_CONNECTED,
    QZ_WIFI_FAILED,
} qz_wifi_state_t;

typedef struct {
    qz_wifi_state_t state;
    char ssid[64];
    char error[96];
} qz_wifi_status_t;

/** Managed interface name ("wlan0"), empty when the device has no wireless
 *  interface at all. */
void qz_wifi_interface(char *output, unsigned int size);
/** 网卡还没出现、且仍在开机宽限期内（用来区分"正在检测"与"确实没有网卡"）。 */
bool qz_wifi_nic_pending(void);
/** 网卡缺失时的文案：宽限期内"正在检测无线网卡…"，之后"设备未提供无线网卡"。 */
const char *qz_wifi_nic_text(void);

/** False when there is no wireless interface or no running supplicant, in which
 *  case scanning and joining are not possible. */
bool qz_wifi_ready(void);

/** Bring the interface up and start wpa_supplicant (as documented for this
 *  board), creating its socket directory and fixing up the config globals the
 *  first time. Returns true when wpa_cli answers afterwards. */
bool qz_wifi_start(void);

/** Kill the supplicant and take the interface down. */
void qz_wifi_stop(void);

/**
 * Run a full scan (**blocking, up to a few seconds — worker thread only**) and
 * cache the result.
 *
 * Tries three paths in order, the way Echo-Mate does: `iw dev X scan`, then
 * `wpa_cli` flush/scan/scan_results polling, then `iwlist X scan`. Results are
 * deduplicated per SSID (strongest wins) and sorted strongest first.
 */
bool qz_wifi_scan_start(void);

/** Return true while a scan requested by qz_wifi_scan_start is pending. */
bool qz_wifi_scan_pending(void);

/** Copy the cached scan results (non-blocking, UI thread safe). Returns the
 *  number written (0 when there is nothing to show). */
int qz_wifi_scan_results(qz_wifi_ap_t *output, int max_count);

/** SSID currently associated with the interface, or "" when not connected. */
void qz_wifi_current(char *output, unsigned int size);

/** wlan0 上的 IPv4 地址（没有就空串）——"是否连上"以这个为准。 */
void qz_wifi_ipv4(char *output, unsigned int size);
/** wlan0 的 MAC 地址（读 sysfs），没有就空串。 */
void qz_wifi_mac(char *output, unsigned int size);
/** 最近一次扫描的结果说明（"发现 N 个网络"/"扫描无结果…"/"无线网卡不可用"）。 */
const char *qz_wifi_scan_note(void);

/**
 * Join a network (**blocking, up to 15s — worker thread only**). `password` may
 * be "" for open networks.
 *
 * Writes wpa_supplicant.conf (a persistent copy plus the runtime one), restarts
 * the supplicant and udhcpc, then waits for an IPv4 address. Returns false on
 * timeout — the UI must show a failure rather than a false success.
 */
bool qz_wifi_join(const char *ssid, const char *password);

/** Disconnect but keep the saved network (tapping it reconnects). */
void qz_wifi_disconnect(void);

/** Forget the saved network: disconnect and delete both config files. */
bool qz_wifi_forget(void);

/** Start the Wi-Fi worker. Safe to call more than once. */
bool qz_wifi_worker_start(void);
/** Stop the worker and release its resources. */
void qz_wifi_worker_stop(void);
/** Submit an asynchronous enable/disable/scan/connect operation. */
bool qz_wifi_enable_async(bool enabled);
bool qz_wifi_scan_async(void);
bool qz_wifi_connect_async(const char *ssid, const char *password);
/** Read the latest worker snapshot without blocking. */
bool qz_wifi_status(qz_wifi_status_t *output);

#endif
