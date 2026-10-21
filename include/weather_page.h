#ifndef QZDESK_WEATHER_PAGE_H
#define QZDESK_WEATHER_PAGE_H

#include "lvgl/lvgl.h"

/**
 * Weather detail page (480x320) — what the home card opens when tapped.
 *
 * The data path is exactly the same as the card's: the Rust core fetches, caches,
 * times out and retries, then pushes one snapshot that both the device and the
 * web console read. This page only draws, so opening it and pressing refresh can
 * never block the LVGL thread, and a dead weather API cannot affect the AI
 * conversation, Wi-Fi or skills.
 *
 * Layout:
 *   ┌ 页头：← 桌面气象站                              [刷新] ┐
 *   ├ 主卡：天气图标 | 城市 / 大字温度 / 描述 | 时间 / 更新时间 ┤
 *   └ 指标卡：湿度 / 风速 / 体感 / 降水 · 紫外线 / 高低 / 日出落 ┘
 */
void qz_weather_page_init(lv_obj_t *back_screen);

/** Open the page (home card tap) and ask the core for its current snapshot. */
void qz_weather_page_open(void);

#endif
