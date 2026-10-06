#ifndef QZDESK_CONFIG_H
#define QZDESK_CONFIG_H

#include <stdbool.h>

void qz_load_config(char *server, unsigned int server_size);
/** Primary IPv4 address, or "未连接" when the device is offline. */
void qz_device_ip(char *output, unsigned int output_size);
/** Battery percentage 0..100, or -1 when the device reports none. */
int qz_battery_level(void);

/* ------------------------------------------------------------------------- *
 * Panel and audio controls
 *
 * Both follow the SDK documentation for this board: the backlight is a sysfs
 * node and the speaker is the "DAC LINEOUT Volume" control of card 0 (0..30).
 * Every entry point returns -1 / false when the hardware is absent, so the UI
 * can keep working (and stay honest) on a machine without them.
 * ------------------------------------------------------------------------- */

/** Backlight percentage 0..100, or -1 when the panel exposes no control. */
int qz_backlight_level(void);
bool qz_backlight_set(int percent);

/** Speaker volume percentage 0..100, or -1 when there is no mixer control. */
int qz_volume_level(void);
bool qz_volume_set(int percent);

/** Apply a POSIX TZ string ("CST-8") to this process and persist it in
 *  /etc/profile the way the SDK does. */
bool qz_timezone_apply(const char *posix_tz);

/* ------------------------------------------------------------------------- *
 * Appearance
 *
 * The dark-mode preference is a single line "QZDESK_DARK=1" written to a small
 * file under /etc (the device) or $HOME (the simulator, where /etc is usually
 * read-only). qz_style_init() reads QZDESK_DARK the same way it reads the
 * other env vars, so the file is only the persistence layer — the live state
 * always comes from the env var, set by qz_appearance_load() at startup and
 * by qz_appearance_save() when the user toggles the switch.
 * ------------------------------------------------------------------------- */

/** Read the persisted preference into the QZDESK_DARK env var. Called once
 *  before qz_style_init() so the first frame is already in the right theme. */
void qz_appearance_load(void);
/** Persist the current preference (QZDESK_DARK env var) and return the new
 *  state, so the caller can hand it to qz_theme_set_dark(). */
bool qz_appearance_save(bool dark);

#endif
