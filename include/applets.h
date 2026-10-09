#ifndef QZDESK_APPLETS_H
#define QZDESK_APPLETS_H

#include "lvgl/lvgl.h"

typedef enum {
    QZ_APPLET_STATUS,    /**< 系统状态 */
    QZ_APPLET_REMINDER,  /**< 定时提醒 */
    QZ_APPLET_POMODORO,  /**< 番茄钟 */
    QZ_APPLET_CONTROL,   /**< 设备控制 */
    QZ_APPLET_COUNT,
} qz_applet_t;

/** Build every applet screen. `apps_screen` is the back navigation target. */
void qz_applets_init(lv_obj_t *apps_screen);

/** Screen for the given applet id (created by qz_applets_init). */
lv_obj_t *qz_applets_screen(qz_applet_t id);

#endif
