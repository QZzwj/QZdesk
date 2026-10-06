#ifndef QZDESK_WEATHER_CARD_H
#define QZDESK_WEATHER_CARD_H

#include "lvgl/lvgl.h"
#include "qzdesk_core.h"

/**
 * Create the weather card for the home screen.
 *
 * The card only renders what the core pushes (see `qzdesk_core_weather_t`): it
 * performs no network I/O and never blocks the LVGL main loop. It subscribes to
 * the core event stream, asks for the cached snapshot once the status
 * handshake succeeds, and asks the core to refresh when tapped.
 *
 * Sizing: 176x86 in the 480x320 home layout — a drawn icon plus four short
 * text lines (temperature/description, high-low/wind, feels-like/humidity,
 * city/updated time).
 */
lv_obj_t *qz_weather_card_create(lv_obj_t *parent, int x, int y, int width, int height);

/** Render one snapshot. Called on every core push, and safe to call directly. */
void qz_weather_card_apply(const qzdesk_core_weather_t *weather);

/**
 * Weather glyph, shared by the home card and the detail page.
 *
 * The icon is drawn from primitives on a fixed 40px canvas and scaled as a whole,
 * so the detail page gets a crisp large glyph without a second copy of the
 * drawing code (and without shipping a weather font).
 */
lv_obj_t *qz_weather_icon_create(lv_obj_t *parent, int size);

/** Re-draw the glyph for one snapshot `icon` key (sun / cloud / rain / …). */
void qz_weather_icon_render(lv_obj_t *box, const char *key);

#endif
