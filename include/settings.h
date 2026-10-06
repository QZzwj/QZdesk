#ifndef QZDESK_SETTINGS_H
#define QZDESK_SETTINGS_H

#include "lvgl/lvgl.h"

/** Root settings list, plus the WLAN, general and about pages it opens. */
lv_obj_t *qz_settings_create(void);
void qz_settings_set_desktop(lv_obj_t *desktop);

#endif
