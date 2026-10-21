#ifndef QZDESK_APPLETS_H
#define QZDESK_APPLETS_H

#include "lvgl/lvgl.h"
#include "ai_face.h"

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

/** 改 applet 的返回目标（默认是应用页）。主页的「接下来」卡直接跳进提醒 /
 *  番茄钟页时，把返回目标改回主页。 */
void qz_applets_set_back(lv_obj_t *screen);

/** 主页「接下来」卡用的只读快照：番茄钟状态 + 最近一项提醒。
 *  数据来自 applet 自己在后台维护的缓存，调用方不触发网络请求。 */
typedef struct {
    bool focus_active;      /**< 番茄钟在跑（含暂停） */
    bool focus_paused;
    bool focus_break;       /**< 当前是休息段 */
    int focus_remaining;    /**< 当前阶段剩余秒数 */
    bool has_reminder;      /**< 缓存里有接下来的一项提醒 */
    int reminder_hour;
    int reminder_minute;
    int reminder_in_minutes; /**< 距现在多少分钟；>= 24*60 表示不是今天 */
    char reminder_label[28];
} qz_applets_glance_t;

/** 填 `out`（可为 NULL）。 */
void qz_applets_glance(qz_applets_glance_t *out);

/** 番茄钟状态对应的表情：专注=思考、休息=开心、暂停=困倦、未开始=待机。
 *  助手页只有在它自己空闲时才会借用这张脸。 */
qz_face_state_t qz_applets_focus_face(void);

#endif
