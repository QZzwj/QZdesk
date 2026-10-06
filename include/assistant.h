#ifndef QZDESK_ASSISTANT_H
#define QZDESK_ASSISTANT_H

#include "lvgl/lvgl.h"

lv_obj_t *qz_assistant_create(void);
void qz_assistant_set_desktop(lv_obj_t *desktop);

#endif
