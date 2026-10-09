#ifndef QZDESK_APPS_H
#define QZDESK_APPS_H

#include "lvgl/lvgl.h"

lv_obj_t *qz_apps_create(void);
void qz_apps_set_desktop(lv_obj_t *desktop);
/** Screen opened by the 技能 entry (built by qz_skill_page_create). */
void qz_apps_set_skill_screen(lv_obj_t *screen);
/** Screen opened by the 性能监控 entry (built by qz_performance_create). */
void qz_apps_set_performance_screen(lv_obj_t *screen);
/** Screen opened by the 设置 entry (built by the settings page). */
void qz_apps_set_settings_screen(lv_obj_t *screen);

#endif
