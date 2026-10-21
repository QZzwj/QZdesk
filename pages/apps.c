#include "apps.h"
#include "applets.h"
#include "skills_page.h"
#include "settings.h"
#include "theme.h"
#include "weather_page.h"
#include <stdio.h>

/* A command board rather than a card catalogue: one active workspace, then six
 * flat utility slots. The stage keeps exactly seven direct children because
 * the layout audit and compact navigation depend on that shape. */
#define CONTENT_W (QZ_DESIGN_W - 2 * QZ_GUTTER)
#define ROW_TOP (QZ_HEADER_H + 8)
#define FEATURED_H 78
#define UTILITY_GAP 8
#define UTILITY_W ((CONTENT_W - UTILITY_GAP) / 2)
#define UTILITY_H 64

static lv_obj_t *apps_screen;
static lv_obj_t *desktop_screen;
static lv_obj_t *skill_screen;
static lv_obj_t *performance_screen;
static lv_obj_t *settings_screen;

/* 宽度足够的矮屏仍适合双列：高度紧缺时更需要横向利用空间，而不是把
 * 480×240 也排成一条很长的单列。 */
static bool apps_stacked(void)
{
    return qz_panel_w <= 320;
}

static void back_to_desktop(lv_event_t *event)
{
    (void)event;
    qz_screen_load(desktop_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, QZ_DUR_SCREEN);
}

static void open_applet(lv_event_t *event)
{
    qz_applet_t id = (qz_applet_t)(intptr_t)lv_event_get_user_data(event);
    lv_obj_t *screen = qz_applets_screen(id);
    if (!screen) return;
    /* 从应用页进小程序，返回键回应用页：清掉主页「接下来」卡留下的返回目标。 */
    qz_applets_set_back(NULL);
    qz_screen_load(screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, QZ_DUR_SCREEN);
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

static lv_obj_t *featured_app(lv_obj_t *parent, qz_icon_t icon, const char *title,
                              const char *detail, lv_event_cb_t cb)
{
    lv_obj_t *card = qz_card_button(parent, CONTENT_W, FEATURED_H);
    lv_obj_set_pos(card, 0, 0);

    lv_obj_set_style_radius(card, QZ_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *mark = lv_obj_create(card);
    lv_obj_set_size(mark, 5, FEATURED_H - 16);
    lv_obj_align(mark, LV_ALIGN_LEFT_MID, 0, 0);
    qz_obj_set_bg_color(mark, QZ_ACCENT, 0);
    lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mark, 0, 0);
    lv_obj_set_style_radius(mark, 0, 0);
    lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(mark, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tile = qz_squircle(card, 46, QZ_ACCENT);
    lv_obj_align(tile, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_t *glyph = qz_icon_image(tile, icon, 22, qz_color(QZ_TEXT_ON_ACCENT));
    lv_obj_center(glyph);

    lv_obj_t *name = qz_text(card, title, 22, qz_color(QZ_TEXT));
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 78, -12);
    lv_obj_set_width(name, CONTENT_W - 78 - 84);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_t *summary = qz_text(card, detail, 14, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(summary, LV_ALIGN_LEFT_MID, 78, 15);
    lv_obj_set_width(summary, CONTENT_W - 78 - 84);
    lv_label_set_long_mode(summary, LV_LABEL_LONG_DOT);

    lv_obj_t *kind = qz_text(card, "进入", 12, qz_color(QZ_ACCENT_DARK));
    lv_obj_align(kind, LV_ALIGN_RIGHT_MID, -40, 0);
    lv_obj_t *go = qz_chevron(card, qz_color(QZ_ACCENT), 16);
    lv_obj_align(go, LV_ALIGN_RIGHT_MID, -16, 0);

    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, NULL);
    return card;
}

static lv_obj_t *utility_app(lv_obj_t *parent, int index, qz_icon_t icon,
                             const char *title, const char *detail, lv_event_cb_t cb,
                             void *user_data)
{
    bool compact = apps_stacked();
    int width = compact ? CONTENT_W : UTILITY_W;
    int x = compact ? 0 : (index % 2) * (UTILITY_W + UTILITY_GAP);
    int y = FEATURED_H + 9 + (compact ? index : index / 2) *
                   (UTILITY_H + (compact ? 8 : UTILITY_GAP));
    lv_obj_t *card = qz_card_button(parent, width, UTILITY_H);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_radius(card, QZ_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *mark = lv_obj_create(card);
    lv_obj_set_size(mark, 3, 28);
    lv_obj_align(mark, LV_ALIGN_LEFT_MID, 0, 0);
    qz_obj_set_bg_color(mark, QZ_ACCENT, 0);
    lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mark, 0, 0);
    lv_obj_set_style_radius(mark, 0, 0);
    lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(mark, LV_OBJ_FLAG_SCROLLABLE);

    char ordinal[4];
    snprintf(ordinal, sizeof(ordinal), "%02d", index + 1);
    lv_obj_t *number = qz_text(card, ordinal, 11, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(number, LV_ALIGN_TOP_LEFT, 14, 7);
    lv_obj_t *glyph = qz_icon_image(card, icon, 20, qz_color(QZ_ACCENT_DARK));
    lv_obj_align(glyph, LV_ALIGN_LEFT_MID, 14, 8);

    lv_obj_t *name = qz_text(card, title, 18, qz_color(QZ_TEXT));
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 46, -10);
    lv_obj_set_width(name, width - 46 - 34);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_t *summary = qz_text(card, detail, 12, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(summary, LV_ALIGN_LEFT_MID, 46, 13);
    lv_obj_set_width(summary, width - 46 - 34);
    lv_label_set_long_mode(summary, LV_LABEL_LONG_DOT);

    lv_obj_t *go = qz_chevron(card, qz_color(QZ_TEXT_TERTIARY), 13);
    lv_obj_align(go, LV_ALIGN_RIGHT_MID, -13, 0);
    lv_obj_add_event_cb(card, cb, LV_EVENT_CLICKED, user_data);
    return card;
}

lv_obj_t *qz_apps_create(void)
{
    apps_screen = lv_obj_create(NULL);
    qz_style_screen(apps_screen);

    lv_obj_t *header = qz_page_header(apps_screen, "应用");
    lv_obj_t *back = qz_back_button(header);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_add_event_cb(back, back_to_desktop, LV_EVENT_CLICKED, NULL);
    lv_obj_t *count = qz_text(header, "06 个工具 · 01 个工作区", 12,
                              qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(count, LV_ALIGN_RIGHT_MID, -QZ_GUTTER, 0);

    lv_obj_t *scroll = qz_page_scroll(apps_screen, ROW_TOP);
    lv_obj_t *stage = lv_obj_create(scroll);
    lv_obj_set_size(stage, CONTENT_W, apps_stacked() ? 511 : 298);
    lv_obj_set_pos(stage, QZ_GUTTER, 0);
    lv_obj_set_style_bg_opa(stage, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(stage, 0, 0);
    lv_obj_set_style_pad_all(stage, 0, 0);
    lv_obj_clear_flag(stage, LV_OBJ_FLAG_SCROLLABLE);

    featured_app(stage, QZ_ICON_SPARKLES, "技能",
                 "角色、知识与陪伴记录", open_skills);
    /* 系统状态并入性能监控：CPU、内存、磁盘、温度、进程一次看全 */
    utility_app(stage, 0, QZ_ICON_ACTIVITY, "性能监控",
                "CPU、内存与进程", open_performance, NULL);
    utility_app(stage, 1, QZ_ICON_BELL, "定时提醒",
                "一次或每日计划", open_applet,
                (void *)(intptr_t)QZ_APPLET_REMINDER);
    utility_app(stage, 2, QZ_ICON_TIMER, "番茄钟",
                "专注与休息循环", open_applet,
                (void *)(intptr_t)QZ_APPLET_POMODORO);
    utility_app(stage, 3, QZ_ICON_VOLUME, "设备控制",
                "音量、背光与连接", open_applet,
                (void *)(intptr_t)QZ_APPLET_CONTROL);
    utility_app(stage, 4, QZ_ICON_SETTINGS, "设置",
                "WLAN、显示与隐私", open_settings, NULL);
    utility_app(stage, 5, QZ_ICON_CLOCK, "时钟与天气",
                "本地时间与气象", open_clock, NULL);
    return apps_screen;
}

void qz_apps_set_desktop(lv_obj_t *desktop) { desktop_screen = desktop; }

void qz_apps_set_skill_screen(lv_obj_t *screen) { skill_screen = screen; }

void qz_apps_set_performance_screen(lv_obj_t *screen) { performance_screen = screen; }

void qz_apps_set_settings_screen(lv_obj_t *screen) { settings_screen = screen; }
