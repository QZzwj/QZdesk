#ifndef QZDESK_SKILLS_PAGE_H
#define QZDESK_SKILLS_PAGE_H

#include "lvgl/lvgl.h"

/** Build the Skill page. `apps_screen` is the back navigation target. */
lv_obj_t *qz_skill_page_create(lv_obj_t *apps_screen);

/** The page screen, for navigation. */
lv_obj_t *qz_skill_page_screen(void);

#endif
