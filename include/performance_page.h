#ifndef QZDESK_PERFORMANCE_PAGE_H
#define QZDESK_PERFORMANCE_PAGE_H

#include "lvgl/lvgl.h"

/**
 * Create the performance monitor screen.
 *
 * The page owns no sampler: the core samples every two seconds and pushes
 * `QZDESK_CORE_EVENT_PERFORMANCE`, so this file only renders. It subscribes to
 * the core stream on creation, asks for a fresh snapshot whenever it is shown,
 * and stops touching widgets while another screen is on top — no hidden page
 * keeps redrawing.
 *
 * `apps_screen` is the back navigation target (the list this page is opened
 * from).
 */
lv_obj_t *qz_performance_create(lv_obj_t *apps_screen);

#endif
