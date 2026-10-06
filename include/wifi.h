#ifndef QZDESK_WIFI_H
#define QZDESK_WIFI_H

#include <stdbool.h>

/** One access point from the last scan. */
typedef struct {
    char ssid[64];
    int bars;     /**< 1..4, derived from the signal level. */
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

/** False when there is no wireless interface or no running supplicant, in which
 *  case scanning and joining are not possible. */
bool qz_wifi_ready(void);

/** Bring the interface up and start wpa_supplicant (as documented for this
 *  board), creating its socket directory and fixing up the config globals the
 *  first time. Returns true when wpa_cli answers afterwards. */
bool qz_wifi_start(void);

/** Kill the supplicant and take the interface down. */
void qz_wifi_stop(void);

/** Ask the supplicant for a fresh scan. Returns immediately; results land in
 *  its cache and are picked up by qz_wifi_scan_results(). */
bool qz_wifi_scan_start(void);

/** Return true while a scan requested by qz_wifi_scan_start is pending. */
bool qz_wifi_scan_pending(void);

/** Copy the cached scan results. Returns the number written (0 when there is
 *  nothing to show). */
int qz_wifi_scan_results(qz_wifi_ap_t *output, int max_count);

/** SSID currently associated with the interface, or "" when not connected. */
void qz_wifi_current(char *output, unsigned int size);

/** Join a network through the supplicant. `password` may be "" for open
 *  networks. Returns false when the request could not be sent. */
bool qz_wifi_join(const char *ssid, const char *password);

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
