#include "apps.h"
#include "applets.h"
#include "skills_page.h"
#include "settings.h"
#include "theme.h"
#include "weather_page.h"
#include <stdio.h>

/* Two columns and two visible rows; swipe vertically for the remaining apps. */
#define ROW_W ((QZ_DESIGN_W - 3 * QZ_GUTTER) / 2)
#define ROW_GAP 14
#define ROW_TOP 64
#define ROW_H ((QZ_DESIGN_H - ROW_TOP - QZ_GUTTER - ROW_GAP) / 2)
#define COL_LEFT QZ_GUTTER
#define COL_RIGHT (QZ_DESIGN_W - QZ_GUTTER - ROW_W)

static lv_obj_t *apps_screen;
static lv_obj_t *desktop_screen;
static lv_obj_t *skill_screen;
static lv_obj_t *performance_screen;
static lv_obj_t *settings_screen;

static void back_to_desktop(lv_event_t *event)
{
    (void)event;
    qz_screen_load(desktop_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, QZ_DUR_SCREEN);
}

static void open_applet(lv_event_t *event)
{
    qz_applet_t id = (qz_applet_t)(intptr_t)lv_event_get_user_data(event);
    lv_obj_t *screen = qz_applets_screen(id);
    if (screen) qz_screen_load(screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, QZ_DUR_SCREEN);
}

static void open_clock(lv_event_t *event)
{
    (void)event;
    qz_weather_page_open();
}

static void open_skills(lv_event_t *event)
{
    (void)event;
    if (skill_screen) qz_screen_load(skill_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, QZ_DUR_SCREEN);
}

/** 设置现在住在应用页里（主页那格让给了天气 + 应用两块）。 */
static void open_settings(lv_event_t *event)
{
    (void)event;
    if (settings_screen) {
        qz_settings_set_return(apps_screen);
        qz_screen_load(settings_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, QZ_DUR_SCREEN);
    }
}

static void open_performance(lv_event_t *event)
{
    (void)event;
    if (performance_screen) {
        qz_screen_load(performance_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, QZ_DUR_SCREEN);
    }
}

/** Large tiles inside the bounded vertical scroll area. */
static lv_obj_t *app_row(lv_obj_t *grid, int col, int row, const char *symbol,
                         const char *title, const char *detail, uint32_t tile,
                         uint32_t mark, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *card = qz_card_button(grid, ROW_W, ROW_H);
    lv_obj_set_pos(card, col ? COL_RIGHT : COL_LEFT, row * (ROW_H + ROW_GAP));

    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = qz_squircle(card, 44, tile);
    lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 14, 14);
    lv_obj_t *glyph = qz_symbol(icon, symbol, 24, qz_color(mark));
    lv_obj_center(glyph);

    lv_obj_t *name = qz_text(card, title, 19, qz_color(QZ_TEXT));
    lv_obj_set_width(name, ROW_W - 82);
    lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 68, 24);

    lv_obj_t *summary = qz_text(card, detail, 14, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_set_width(summary, ROW_W - 28);
    lv_label_set_long_mode(summary, LV_LABEL_LONG_WRAP);
    lv_obj_align(summary, LV_ALIGN_BOTTOM_LEFT, 14, -14);

    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, user_data);
    qz_animate_entrance(card, (uint32_t)((col + row) * 40));
    return card;
}

lv_obj_t *qz_apps_create(void)
{
    apps_screen = lv_obj_create(NULL);
    qz_style_screen(apps_screen);

    lv_obj_t *toolbar = lv_obj_create(apps_screen);
    lv_obj_set_size(toolbar, QZ_DESIGN_W - 16, 46);
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 6);
    qz_style_toolbar(toolbar);
    lv_obj_set_style_radius(toolbar, 16, 0);
    lv_obj_set_style_border_width(toolbar, 0, 0);
    lv_obj_set_style_shadow_width(toolbar, 0, 0);
    lv_obj_set_style_pad_all(toolbar, 0, 0);
    lv_obj_clear_flag(toolbar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = qz_back_button(toolbar);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_event_cb(back, back_to_desktop, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = qz_text(toolbar, "应用", 20, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 58, 0);
    lv_obj_t *hint = qz_text(toolbar, "向上滑动查看更多", 12, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(hint, LV_ALIGN_RIGHT_MID, -14, 0);

    lv_obj_t *grid = qz_page_scroll(apps_screen, ROW_TOP);

    /* Tiles alternate between the accent and the neutral fill — the same
     * value-alternation the reference uses for its panels. */
    app_row(grid, 0, 0, LV_SYMBOL_EDIT, "技能", "人设与知识库",
            QZ_ACCENT, QZ_TEXT_ON_ACCENT, open_skills, NULL);
    /* 系统状态并入性能监控：CPU、内存、磁盘、温度、进程一次看全 */
    app_row(grid, 1, 0, LV_SYMBOL_LOOP, "性能监控", "CPU · 内存 · 磁盘",
            QZ_ACCENT_TINT, QZ_ACCENT_DARK, open_performance, NULL);
    app_row(grid, 0, 1, LV_SYMBOL_BELL, "定时提醒", "一次或每日",
            QZ_ACCENT, QZ_TEXT_ON_ACCENT, open_applet,
            (void *)(intptr_t)QZ_APPLET_REMINDER);
    app_row(grid, 1, 1, LV_SYMBOL_PLAY, "番茄钟", "专注与休息",
            QZ_ACCENT_TINT, QZ_ACCENT_DARK, open_applet,
            (void *)(intptr_t)QZ_APPLET_POMODORO);

    app_row(grid, 0, 2, LV_SYMBOL_GPS, "设备控制", "声音 · 背光",
            QZ_ACCENT, QZ_TEXT_ON_ACCENT, open_applet,
            (void *)(intptr_t)QZ_APPLET_CONTROL);

    app_row(grid, 1, 2, LV_SYMBOL_SETTINGS, "设置", "WLAN · 声音 · 显示",
            QZ_ACCENT_TINT, QZ_ACCENT_DARK, open_settings, NULL);
    app_row(grid, 0, 3, LV_SYMBOL_LOOP, "桌面时钟", "时钟与气象站",
            QZ_ACCENT, QZ_TEXT_ON_ACCENT, open_clock, NULL);
    return apps_screen;
}

void qz_apps_set_desktop(lv_obj_t *desktop) { desktop_screen = desktop; }

void qz_apps_set_skill_screen(lv_obj_t *screen) { skill_screen = screen; }

void qz_apps_set_performance_screen(lv_obj_t *screen) { performance_screen = screen; }

void qz_apps_set_settings_screen(lv_obj_t *screen) { settings_screen = screen; }
