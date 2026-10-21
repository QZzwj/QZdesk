#ifndef QZDESK_NOTIFICATIONS_H
#define QZDESK_NOTIFICATIONS_H
#include <stdbool.h>
void qz_notifications_init(void);
/** Effective DND state: manual DND or an active quiet-hours window. */
bool qz_notifications_dnd(void);
/** The user's manually persisted DND state, independent of quiet hours. */
bool qz_notifications_manual_dnd(void);
bool qz_notifications_set_dnd(bool enabled);
/** Read the persisted quiet-hours schedule. Minutes are in the range 0-1439. */
bool qz_notifications_schedule_get(bool *enabled, int *start_minute, int *end_minute);
/** Persist quiet hours. An enabled schedule must not start and end at the same minute. */
bool qz_notifications_schedule_set(bool enabled, int start_minute, int end_minute);
/** 免打扰状态一变就回调一次（通知中心的开关和设置页里那枚是同一个状态）。 */
void qz_notifications_set_dnd_listener(void (*listener)(bool enabled));
/** 顶部下拉出来的通知中心：最近的通知、免打扰与清空。已经开着就什么都不做。 */
void qz_notifications_open_panel(void);
void qz_notifications_close_panel(void);
bool qz_notifications_panel_open(void);
#endif
