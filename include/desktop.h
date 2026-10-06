#ifndef QZDESK_DESKTOP_H
#define QZDESK_DESKTOP_H

#include "lvgl/lvgl.h"

lv_obj_t *qz_desktop_create(const char *server);
void qz_desktop_set_assistant(lv_obj_t *assistant);
void qz_desktop_set_apps(lv_obj_t *apps);
void qz_desktop_set_settings(lv_obj_t *settings);

#endif
