#include "apps.h"
#include "applets.h"
#include "skills_page.h"
#include "theme.h"
#include <stdio.h>

/* Two columns of compact rows: five entries fit without shrinking the page,
 * and the shape matches the settings list so the whole shell reads as one
 * system. */
#define ROW_W ((QZ_DESIGN_W - 3 * QZ_GUTTER) / 2)
#define ROW_H 52
#define ROW_GAP 6
#define ROW_TOP 98
#define COL_LEFT QZ_GUTTER
#define COL_RIGHT (QZ_DESIGN_W - QZ_GUTTER - ROW_W)

static lv_obj_t *apps_screen;
static lv_obj_t *desktop_screen;
static lv_obj_t *skill_screen;
static lv_obj_t *performance_screen;

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

static void open_skills(lv_event_t *event)
{
    (void)event;
    if (skill_screen) qz_screen_load(skill_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, QZ_DUR_SCREEN);
}

static void open_performance(lv_event_t *event)
{
    (void)event;
    if (performance_screen) {
        qz_screen_load(performance_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, QZ_DUR_SCREEN);
    }
}

static lv_obj_t *app_row(lv_obj_t *parent, int x, int y, const char *symbol,
                         const char *title, const char *detail, uint32_t tile,
                         uint32_t mark, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *row = qz_card_button(parent, ROW_W, ROW_H);
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, x, y);

    lv_obj_t *icon = qz_squircle(row, 30, tile);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_t *glyph = qz_symbol(icon, symbol, 15, qz_color(mark));
    lv_obj_center(glyph);

    lv_obj_t *name = qz_text(row, title, 13, qz_color(QZ_TEXT));
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 50, 9);
    lv_obj_t *caption = qz_text(row, detail, 10, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_set_width(caption, ROW_W - 62);
    lv_label_set_long_mode(caption, LV_LABEL_LONG_DOT);
    lv_obj_align(caption, LV_ALIGN_TOP_LEFT, 50, 29);

    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);
    qz_animate_entrance(row, (uint32_t)((x + y) / 6));
    return row;
}

lv_obj_t *qz_apps_create(void)
{
    apps_screen = lv_obj_create(NULL);
    qz_style_screen(apps_screen);

    lv_obj_t *toolbar = lv_obj_create(apps_screen);
    lv_obj_set_size(toolbar, QZ_DESIGN_W - 16, QZ_TOOLBAR_H - 4);
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 6);
    qz_style_toolbar(toolbar);

    lv_obj_t *back = qz_icon_button(toolbar, LV_SYMBOL_LEFT, 30);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_event_cb(back, back_to_desktop, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = qz_text(toolbar, "应用", 15, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *intro = qz_text(apps_screen, "设备工具", 15, qz_color(QZ_TEXT));
    lv_obj_align(intro, LV_ALIGN_TOP_LEFT, QZ_GUTTER, 54);
    lv_obj_t *hint = qz_text(apps_screen, "独立小应用，点开即用", 10,
                             qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, QZ_GUTTER, 76);

    const int row1 = ROW_TOP;
    const int row2 = ROW_TOP + ROW_H + ROW_GAP;
    const int row3 = row2 + ROW_H + ROW_GAP;

    /* Tiles alternate between the accent and the neutral fill — the same
     * value-alternation the reference uses for its panels. */
    app_row(apps_screen, COL_LEFT, row1, LV_SYMBOL_EDIT, "技能", "人设与知识库",
            QZ_ACCENT, QZ_TEXT_ON_ACCENT, open_skills, NULL);
    /* 系统状态并入性能监控：CPU、内存、磁盘、温度、进程一次看全 */
    app_row(apps_screen, COL_RIGHT, row1, LV_SYMBOL_LOOP, "性能监控", "CPU · 内存 · 磁盘",
            QZ_ACCENT_TINT, QZ_ACCENT_DARK, open_performance, NULL);
    app_row(apps_screen, COL_LEFT, row2, LV_SYMBOL_BELL, "定时提醒", "一次或每日",
            QZ_ACCENT, QZ_TEXT_ON_ACCENT, open_applet,
            (void *)(intptr_t)QZ_APPLET_REMINDER);
    app_row(apps_screen, COL_RIGHT, row2, LV_SYMBOL_PLAY, "番茄钟", "专注与休息",
            QZ_ACCENT_TINT, QZ_ACCENT_DARK, open_applet,
            (void *)(intptr_t)QZ_APPLET_POMODORO);
    app_row(apps_screen, COL_RIGHT, row3, LV_SYMBOL_EYE_OPEN, "存在检测", "有人靠近自动亮屏",
            QZ_ACCENT_TINT, QZ_ACCENT_DARK, open_applet,
            (void *)(intptr_t)QZ_APPLET_FACE);
    app_row(apps_screen, COL_LEFT, row3, LV_SYMBOL_GPS, "设备控制", "声音 · 背光",
            QZ_ACCENT, QZ_TEXT_ON_ACCENT, open_applet,
            (void *)(intptr_t)QZ_APPLET_CONTROL);
    return apps_screen;
}

void qz_apps_set_desktop(lv_obj_t *desktop) { desktop_screen = desktop; }

void qz_apps_set_skill_screen(lv_obj_t *screen) { skill_screen = screen; }

void qz_apps_set_performance_screen(lv_obj_t *screen) { performance_screen = screen; }
