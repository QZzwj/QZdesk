#include "applets.h"
#include "theme.h"
#include "config.h"
#include "qzdesk_core.h"
#include "smarthome.h"
#include "web_client.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/sysinfo.h>

#define CONTENT_W (QZ_DESIGN_W - 2 * QZ_GUTTER)
#define CARD_PAD 12

/* ------------------------------------------------------------------------- *
 * Shared building blocks
 * ------------------------------------------------------------------------- */

static lv_obj_t *screens[QZ_APPLET_COUNT];
static lv_obj_t *apps_screen_ref;

static void set_page_button_enabled(lv_obj_t *button, bool enabled)
{
    if (!button) return;
    if (enabled) lv_obj_remove_state(button, LV_STATE_DISABLED);
    else lv_obj_add_state(button, LV_STATE_DISABLED);
}

static void go_back(lv_event_t *event)
{
    (void)event;
    qz_screen_load(apps_screen_ref, LV_SCR_LOAD_ANIM_MOVE_RIGHT, QZ_DUR_SCREEN);
}

static lv_obj_t *make_toolbar(lv_obj_t *screen, const char *title)
{
    lv_obj_t *toolbar = lv_obj_create(screen);
    lv_obj_set_size(toolbar, QZ_DESIGN_W - 16, 48);
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 6);
    qz_style_toolbar(toolbar);

    lv_obj_t *back = qz_back_button(toolbar);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_event_cb(back, go_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = qz_text(toolbar, title, qz_compact() ? 18 : 15, qz_color(QZ_TEXT));
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    return toolbar;
}

static lv_obj_t *make_card(lv_obj_t *screen, int y, int height)
{
    lv_obj_t *card = lv_obj_create(screen);
    lv_obj_set_size(card, CONTENT_W, height);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, QZ_GUTTER, y);
    lv_obj_set_style_radius(card, QZ_RADIUS_CARD, 0);
    qz_style_plate(card);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

/** Icon + big value + caption, used on the status screen. */
static lv_obj_t *stat_card(lv_obj_t *screen, int x, int width, const char *symbol,
                           uint32_t tile, uint32_t mark, const char *caption)
{
    lv_obj_t *card = make_card(screen, 56, 94);
    lv_obj_set_width(card, width);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, x, 56);

    lv_obj_t *icon = qz_squircle(card, 30, tile);
    lv_obj_align(icon, LV_ALIGN_TOP_LEFT, CARD_PAD, 12);
    lv_obj_t *glyph = qz_symbol(icon, symbol, 15, qz_color(mark));
    lv_obj_center(glyph);

    lv_obj_t *value = qz_text(card, "--", qz_compact() ? 17 : 16, qz_color(QZ_TEXT));
    lv_obj_set_width(value, width - CARD_PAD * 2);
    lv_obj_align(value, LV_ALIGN_TOP_LEFT, CARD_PAD, 50);

    lv_obj_t *hint = qz_text(card, caption, 10, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, CARD_PAD, qz_compact() ? 72 : 74);
    return value;
}

/* ------------------------------------------------------------------------- *
 * 系统状态
 * ------------------------------------------------------------------------- */

static lv_obj_t *status_uptime;
static lv_obj_t *status_memory;
static lv_obj_t *status_battery;
static lv_obj_t *status_lvgl;
static lv_obj_t *status_ip;

static void status_refresh(void)
{
    struct sysinfo si;
    char text[48];

    if (sysinfo(&si) == 0) {
        long up = (long)si.uptime;
        if (up < 90 * 60) {
            snprintf(text, sizeof(text), "%ld 分钟", up / 60);
        } else if (qz_compact() && up >= 24 * 3600) {
            snprintf(text, sizeof(text), "%ld天%ld时", up / 86400, (up % 86400) / 3600);
        } else {
            snprintf(text, sizeof(text), qz_compact() ? "%ld时%02ld分" : "%ld 时 %02ld 分",
                     up / 3600, (up % 3600) / 60);
        }
        lv_label_set_text(status_uptime, text);
        snprintf(text, sizeof(text), "%lu MB",
                 (unsigned long)(si.freeram * si.mem_unit / (1024 * 1024)));
        lv_label_set_text(status_memory, text);
    }

    int battery = qz_battery_level();
    if (battery < 0) {
        snprintf(text, sizeof(text), "--");
    } else {
        snprintf(text, sizeof(text), "%d%%", battery);
    }
    lv_label_set_text(status_battery, text);

    lv_mem_monitor_t monitor;
    lv_mem_monitor(&monitor);
    snprintf(text, sizeof(text), "%d%%（碎片 %d%%）", monitor.used_pct, monitor.frag_pct);
    lv_label_set_text(status_lvgl, text);

    qz_device_ip(text, sizeof(text));
    lv_label_set_text(status_ip, text);
}

static void status_screen_loaded(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_SCREEN_LOADED) {
        status_refresh();
    }
}

static void build_status_screen(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    qz_style_screen(screen);
    screens[QZ_APPLET_STATUS] = screen;

    make_toolbar(screen, "系统状态");
    lv_obj_add_event_cb(screen, status_screen_loaded, LV_EVENT_ALL, NULL);

    const int width = (CONTENT_W - 2 * 10) / 3;
    status_uptime = stat_card(screen, QZ_GUTTER, width, LV_SYMBOL_REFRESH,
                              QZ_ACCENT, QZ_TEXT_ON_ACCENT, "运行时间");
    status_memory = stat_card(screen, QZ_GUTTER + width + 10, width, LV_SYMBOL_CHARGE,
                              QZ_ACCENT_TINT, QZ_ACCENT_DARK, "可用内存");
    status_battery = stat_card(screen, QZ_GUTTER + 2 * (width + 10), width,
                               LV_SYMBOL_BATTERY_FULL, QZ_ACCENT,
                               QZ_TEXT_ON_ACCENT, "电池");

    lv_obj_t *card = make_card(screen, 162, 148);
    lv_obj_set_style_clip_corner(card, true, 0);

    const char *keys[] = { "固件版本", "当前 IP", "LVGL 内存", "界面框架" };
    char framework[32];
    snprintf(framework, sizeof(framework), "LVGL 9 · %dx%d", (int)qz_panel_w, (int)qz_panel_h);
    const char *values[] = { "1.0.0", NULL, NULL, framework };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *key = qz_text(card, keys[i], 13, qz_color(QZ_TEXT));
        lv_obj_align(key, LV_ALIGN_TOP_LEFT, CARD_PAD, i * 37 + 12);
        if (i == 1) {
            status_ip = qz_text(card, "--", 13, qz_color(QZ_TEXT_SECONDARY));
            lv_obj_align(status_ip, LV_ALIGN_TOP_RIGHT, -CARD_PAD, i * 37 + 12);
        } else if (i == 2) {
            status_lvgl = qz_text(card, "--", 13, qz_color(QZ_TEXT_SECONDARY));
            lv_obj_align(status_lvgl, LV_ALIGN_TOP_RIGHT, -CARD_PAD, i * 37 + 12);
        } else {
            lv_obj_t *value = qz_text(card, values[i], 13, qz_color(QZ_TEXT_SECONDARY));
            lv_obj_align(value, LV_ALIGN_TOP_RIGHT, -CARD_PAD, i * 37 + 12);
        }
        if (i < 3) {
            lv_obj_t *sep = qz_separator(card, CONTENT_W - 2 * CARD_PAD, false);
            lv_obj_align(sep, LV_ALIGN_TOP_LEFT, CARD_PAD, (i + 1) * 37);
        }
    }
}

/* ------------------------------------------------------------------------- *
 * 定时提醒
 * ------------------------------------------------------------------------- */

/* 提醒存在核心那边（`timers.json`，与语音用的 MCP 工具同一份），界面这里只是
 * 一份拿来渲染的副本：
 *
 *   - 语音说"提醒我七点半喝水"，核心写进表，这里下一次刷新就能看见；
 *   - 在界面上加的提醒，语音与网页控制台也读得到；
 *   - 重启设备不会再把它清空（以前这张表只在内存里）。
 *
 * 到点的播报也由核心做（写进聊天记录），所以人翻在别的页面上照样收得到。 */
typedef struct {
    int id;          /* 核心分配的 id，删除时用它 */
    int hour;
    int minute;
    bool daily;
    char label[32];
} reminder_t;

#define REMINDER_MAX 8   /* 与核心的 MAX_REMINDERS 一致 */
static reminder_t reminders[REMINDER_MAX];
static int reminder_count;
/** 提醒页每这么多拍（秒）与核心对齐一次。 */
#define REMINDER_POLL_TICKS 10
static lv_obj_t *reminder_list;
static lv_obj_t *reminder_banner;
static lv_obj_t *reminder_banner_text;
static lv_timer_t *banner_timer;
static lv_obj_t *add_overlay;
static lv_obj_t *add_hour_roller;
static lv_obj_t *add_minute_roller;
static lv_obj_t *add_daily_switch;
static lv_obj_t *reminder_page_label;
static lv_obj_t *reminder_previous;
static lv_obj_t *reminder_next;
static int reminder_page;
#define COMPACT_REMINDERS_PER_PAGE 2

static void hide_banner(lv_timer_t *timer)
{
    (void)timer;
    if (reminder_banner) lv_obj_add_flag(reminder_banner, LV_OBJ_FLAG_HIDDEN);
    banner_timer = NULL;
}

static void show_banner(const char *text)
{
    if (!reminder_banner) return;
    lv_label_set_text(reminder_banner_text, text);
    lv_obj_clear_flag(reminder_banner, LV_OBJ_FLAG_HIDDEN);
    if (banner_timer) lv_timer_del(banner_timer);
    banner_timer = lv_timer_create(hide_banner, 8000, NULL);
    lv_timer_set_repeat_count(banner_timer, 1);
}

/** 发一次请求。成功返回 true；失败时把服务端的错误文案（若有）写进 `error`。 */
static bool reminder_request(const char *method, const char *path, const char *body,
                             char *error, size_t error_size)
{
    static char response[8192];

    response[0] = '\0';
    if (error && error_size > 0) error[0] = '\0';
    if (qz_web_request(method, path, body, response, sizeof(response))) return true;
    if (error && error_size > 0 &&
        !qz_json_string(response, "error", error, error_size)) {
        snprintf(error, error_size, "本地服务未连接");
    }
    return false;
}

/** 拉一次提醒列表，覆盖本地副本。返回 false 表示核心不可达（保留原列表）。 */
static bool reminders_reload(void)
{
    static char body[8192];
    int count = 0;

    if (!qz_web_request("GET", "/api/timers", NULL, body, sizeof(body))) return false;

    for (int i = 0; i < REMINDER_MAX; i++) {
        const char *object = qz_json_item(body, "timers", i);
        reminder_t *r;
        int value;
        if (!object) break;
        r = &reminders[count];
        memset(r, 0, sizeof(*r));
        if (!qz_json_int(object, "id", &value) || value <= 0) continue;
        r->id = value;
        if (!qz_json_int(object, "hour", &value)) continue;
        r->hour = value;
        if (!qz_json_int(object, "minute", &value)) continue;
        r->minute = value;
        qz_json_bool(object, "daily", &r->daily);
        if (!qz_json_string(object, "label", r->label, sizeof(r->label)) ||
            r->label[0] == '\0') {
            snprintf(r->label, sizeof(r->label), "提醒");
        }
        count++;
    }
    reminder_count = count;
    return true;
}

static void remove_reminder(lv_event_t *event);
static void rebuild_reminders(void);

static void reminder_page_changed(lv_event_t *event)
{
    reminder_page += (int)(intptr_t)lv_event_get_user_data(event);
    rebuild_reminders();
}

static void rebuild_reminders(void)
{
    lv_obj_clean(reminder_list);
    int pages = reminder_count > 0 ? (reminder_count + COMPACT_REMINDERS_PER_PAGE - 1) /
                                     COMPACT_REMINDERS_PER_PAGE : 1;
    if (reminder_page >= pages) reminder_page = pages - 1;
    if (reminder_page < 0) reminder_page = 0;
    if (reminder_page_label) {
        char text[32];
        snprintf(text, sizeof(text), "%d / %d", reminder_page + 1, pages);
        lv_label_set_text(reminder_page_label, text);
        set_page_button_enabled(reminder_previous, reminder_page > 0);
        set_page_button_enabled(reminder_next, reminder_page + 1 < pages);
    }
    if (reminder_count == 0) {
        lv_obj_t *empty = qz_text(reminder_list, "还没有提醒，点击下方按钮添加", 12,
                                  qz_color(QZ_TEXT_TERTIARY));
        lv_obj_center(empty);
        return;
    }

    int first = reminder_page * COMPACT_REMINDERS_PER_PAGE;
    int end = first + COMPACT_REMINDERS_PER_PAGE;
    if (end > reminder_count) end = reminder_count;
    for (int i = first; i < end; i++) {
        lv_obj_t *row = lv_obj_create(reminder_list);
        lv_obj_set_size(row, lv_pct(100), 78);
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0,
                     (int32_t)(i - first) * 80 + 2);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        char clock[16];
        snprintf(clock, sizeof(clock), "%02d:%02d", reminders[i].hour, reminders[i].minute);
        lv_obj_t *time_label = qz_text(row, clock, qz_compact() ? 20 : 17, qz_color(QZ_TEXT));
        lv_obj_align(time_label, LV_ALIGN_LEFT_MID, 14, 0);

        lv_obj_t *name = qz_text(row, reminders[i].label, 12, qz_color(QZ_TEXT_SECONDARY));
        lv_obj_set_width(name, CONTENT_W - 166);
        lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 98, reminders[i].daily ? -9 : 0);

        if (reminders[i].daily) {
            lv_obj_t *chip = qz_text(row, "每日", 10, qz_color(QZ_ACCENT_TEXT));
            lv_obj_align(chip, LV_ALIGN_LEFT_MID, 98, 22);
        }

        lv_obj_t *remove = qz_icon_button(row, LV_SYMBOL_CLOSE, qz_compact() ? 40 : 28);
        lv_obj_align(remove, LV_ALIGN_RIGHT_MID, -12, 0);
        /* 删除按核心的 id，而不是这一行的下标：列表随时可能被语音改动过 */
        lv_obj_set_user_data(remove, (void *)(intptr_t)reminders[i].id);
        lv_obj_add_event_cb(remove, remove_reminder, LV_EVENT_CLICKED, NULL);
    }
}

static void remove_reminder(lv_event_t *event)
{
    int id = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(event));
    char path[48];
    if (id <= 0) return;
    snprintf(path, sizeof(path), "/api/timers/%d", id);
    reminder_request("DELETE", path, NULL, NULL, 0);
    if (reminders_reload()) rebuild_reminders();
}

static void close_add_modal(lv_event_t *event)
{
    (void)event;
    if (add_overlay) lv_obj_delete(add_overlay);
    add_overlay = NULL;
}

static void add_modal_dismissed(lv_event_t *event)
{
    if (lv_event_get_target(event) != lv_event_get_current_target(event)) return;
    close_add_modal(event);
}

static void add_confirm(lv_event_t *event)
{
    char body[128];
    char error[96];

    if (!add_hour_roller || !add_minute_roller) {
        close_add_modal(event);
        return;
    }
    /* 直接交给核心：语音、网页控制台与这里用的是同一份提醒表，
     * 重复或超上限时也由核心给出原因。 */
    snprintf(body, sizeof(body),
             "{\"hour\":%d,\"minute\":%d,\"daily\":%s,\"label\":\"提醒\"}",
             lv_roller_get_selected(add_hour_roller),
             lv_roller_get_selected(add_minute_roller) * 5,
             lv_obj_has_state(add_daily_switch, LV_STATE_CHECKED) ? "true" : "false");

    if (reminder_request("POST", "/api/timers", body, error, sizeof(error))) {
        if (reminders_reload()) rebuild_reminders();
    } else {
        char text[112];
        snprintf(text, sizeof(text), "%s", error[0] ? error : "保存失败");
        show_banner(text);
    }
    close_add_modal(event);
}

static void style_roller(lv_obj_t *roller)
{
    lv_roller_set_visible_row_count(roller, 3);
    lv_obj_set_style_bg_opa(roller, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(roller, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(roller, 0, LV_PART_MAIN);
    qz_obj_set_text_color(roller, QZ_TEXT_TERTIARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(roller, qz_font_size(15), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(roller, 14, LV_PART_MAIN);
    qz_obj_set_bg_color(roller, QZ_FILL, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_radius(roller, 10, LV_PART_SELECTED);
    qz_obj_set_text_color(roller, QZ_TEXT, LV_PART_SELECTED);
    lv_obj_set_style_text_font(roller, qz_font_size(17), LV_PART_SELECTED);
}

static void open_add_modal(lv_event_t *event)
{
    (void)event;
    if (add_overlay) return;
    lv_obj_t *screen = screens[QZ_APPLET_REMINDER];
    add_overlay = qz_overlay_create(screen);
    lv_obj_add_event_cb(add_overlay, add_modal_dismissed, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(add_overlay);
    lv_obj_set_size(panel, QZ_DESIGN_W - 40, 236);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_set_style_radius(panel, QZ_RADIUS_CARD, 0);
    qz_obj_set_bg_color(panel, QZ_CARD, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, CARD_PAD, 0);
    lv_obj_set_style_clip_corner(panel, true, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = qz_text(panel, "新提醒", 15, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    static char hours[24 * 4];
    hours[0] = '\0';
    for (int i = 0; i < 24; i++) {
        snprintf(hours + strlen(hours), 5, "%02d\n", i);
    }
    hours[strlen(hours) - 1] = '\0';
    static char minutes[12 * 4];
    minutes[0] = '\0';
    for (int i = 0; i < 12; i++) {
        snprintf(minutes + strlen(minutes), 5, "%02d\n", i * 5);
    }
    minutes[strlen(minutes) - 1] = '\0';

    add_hour_roller = lv_roller_create(panel);
    lv_roller_set_options(add_hour_roller, hours, LV_ROLLER_MODE_NORMAL);
    lv_obj_set_size(add_hour_roller, 84, 104);
    lv_obj_align(add_hour_roller, LV_ALIGN_TOP_MID, -52, 26);
    style_roller(add_hour_roller);
    time_t now = time(NULL);
    struct tm local_now;
    localtime_r(&now, &local_now);
    lv_roller_set_selected(add_hour_roller, (uint32_t)local_now.tm_hour, LV_ANIM_OFF);

    lv_obj_t *colon = qz_text(panel, ":", 20, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(colon, LV_ALIGN_TOP_MID, 0, 66);

    add_minute_roller = lv_roller_create(panel);
    lv_roller_set_options(add_minute_roller, minutes, LV_ROLLER_MODE_NORMAL);
    lv_obj_set_size(add_minute_roller, 84, 104);
    lv_obj_align(add_minute_roller, LV_ALIGN_TOP_MID, 52, 26);
    style_roller(add_minute_roller);
    lv_roller_set_selected(add_minute_roller, (uint32_t)(local_now.tm_min / 5), LV_ANIM_OFF);

    lv_obj_t *daily_caption = qz_text(panel, "每日重复", 13, qz_color(QZ_TEXT));
    lv_obj_align(daily_caption, LV_ALIGN_TOP_LEFT, 0, 140);
    add_daily_switch = lv_switch_create(panel);
    qz_style_switch(add_daily_switch);
    lv_obj_set_size(add_daily_switch, qz_compact() ? 60 : 42, qz_compact() ? 32 : 26);
    lv_obj_align(add_daily_switch, LV_ALIGN_TOP_RIGHT, 0, 136);

    lv_obj_t *actions = lv_obj_create(panel);
    lv_obj_set_size(actions, lv_pct(100), qz_compact() ? 44 : 36);
    lv_obj_align(actions, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(actions, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(actions, 0, 0);
    lv_obj_set_style_pad_all(actions, 0, 0);
    lv_obj_set_style_pad_column(actions, 10, 0);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cancel = qz_button(actions, "取消", 88, qz_compact() ? 44 : 32);
    lv_obj_add_event_cb(cancel, close_add_modal, LV_EVENT_CLICKED, NULL);
    lv_obj_t *confirm = lv_button_create(actions);
    lv_obj_set_size(confirm, 96, qz_compact() ? 44 : 32);
    qz_style_primary_button(confirm);
    lv_obj_t *caption = qz_text(confirm, "确定", 14, qz_color(QZ_TEXT_ON_ACCENT));
    lv_obj_center(caption);
    lv_obj_add_event_cb(confirm, add_confirm, LV_EVENT_CLICKED, NULL);
}

/** 与核心对齐提醒列表。
 *
 *  到点判定已经搬到核心（它把播报写进聊天记录，界面翻在别处也收得到），这里只
 *  做界面该做的那件事：把语音刚加的提醒、或刚响过被撤掉的一次性提醒反映到
 *  屏幕上。 */
static void reminder_poll(void)
{
    if (lv_screen_active() != screens[QZ_APPLET_REMINDER]) return;
    if (reminders_reload()) rebuild_reminders();
}

static void build_reminder_screen(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    qz_style_screen(screen);
    screens[QZ_APPLET_REMINDER] = screen;
    make_toolbar(screen, "定时提醒");

    reminder_list = make_card(screen, 56, 166);
    reminder_banner = make_card(screen, 56, 64);
    /* A banner is chrome floating over the page, so it is the one surface here
     * that keeps a shadow; the fill itself stays a plain white plate. */
    qz_obj_set_bg_color(reminder_banner, QZ_CARD, 0);
    lv_obj_set_style_bg_opa(reminder_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(reminder_banner, 12, 0);
    lv_obj_set_style_shadow_opa(reminder_banner, (lv_opa_t)52, 0);
    qz_obj_set_shadow_color(reminder_banner, QZ_SHADOW, 0);
    lv_obj_set_style_shadow_offset_y(reminder_banner, 4, 0);
    qz_material_register(reminder_banner, true);
    lv_obj_add_flag(reminder_banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *bell = qz_symbol(reminder_banner, LV_SYMBOL_BELL, 15, qz_color(QZ_ACCENT));
    lv_obj_align(bell, LV_ALIGN_LEFT_MID, CARD_PAD, 0);
    reminder_banner_text = qz_text(reminder_banner, "", 12, qz_color(QZ_TEXT));
    lv_obj_set_width(reminder_banner_text, CONTENT_W - CARD_PAD * 2 - 24);
    lv_label_set_long_mode(reminder_banner_text, LV_LABEL_LONG_WRAP);
    lv_obj_align(reminder_banner_text, LV_ALIGN_LEFT_MID, CARD_PAD + 24, 0);

    lv_obj_t *add = lv_button_create(screen);
    lv_obj_set_size(add, CONTENT_W, 44);
    lv_obj_align(add, LV_ALIGN_TOP_LEFT, QZ_GUTTER, 266);
    qz_style_primary_button(add);
    lv_obj_t *caption = qz_text(add, "添加提醒", 15, qz_color(QZ_TEXT_ON_ACCENT));
    lv_obj_align(caption, LV_ALIGN_CENTER, 12, 0);
    lv_obj_t *plus = qz_symbol(add, LV_SYMBOL_PLUS, 15, qz_color(QZ_TEXT_ON_ACCENT));
    lv_obj_align(plus, LV_ALIGN_CENTER, -42, 0);
    lv_obj_add_event_cb(add, open_add_modal, LV_EVENT_CLICKED, NULL);

    {
        reminder_previous = qz_button(screen, "上一页", 116, 40);
        lv_obj_align(reminder_previous, LV_ALIGN_TOP_LEFT, QZ_GUTTER, 222);
        lv_obj_add_event_cb(reminder_previous, reminder_page_changed, LV_EVENT_CLICKED,
                            (void *)(intptr_t)-1);
        reminder_next = qz_button(screen, "下一页", 116, 40);
        lv_obj_align(reminder_next, LV_ALIGN_TOP_RIGHT, -QZ_GUTTER, 222);
        lv_obj_add_event_cb(reminder_next, reminder_page_changed, LV_EVENT_CLICKED,
                            (void *)(intptr_t)1);
        reminder_page_label = qz_text(screen, "1 / 1", 16, qz_color(QZ_TEXT_SECONDARY));
        lv_obj_align(reminder_page_label, LV_ALIGN_TOP_MID, 0, 233);
    }

    reminder_count = 0;
    /* 建页时先拉一次：核心可能已经装着语音加过的提醒 */
    reminders_reload();
    rebuild_reminders();
}

/* ------------------------------------------------------------------------- *
 * 番茄钟
 * ------------------------------------------------------------------------- */

/* 番茄钟的状态机同样在核心：界面只显示与操作。
 *
 * 于是语音说的"开始番茄钟"和屏幕上的按钮动的是同一个计时器，切页面、退出界面
 * 都不会把它弄丢；换阶段与结束时的播报由核心写进聊天记录。界面保留一个本地
 * 倒计时，只为让秒数看起来是连续走的，每隔几拍与核心对一次表。 */
static lv_obj_t *pomo_arc;
static lv_obj_t *pomo_clock;
static lv_obj_t *pomo_phase_chip;
static lv_obj_t *pomo_count;
static lv_obj_t *pomo_start_caption;

static bool pomo_active;
static bool pomo_paused;
static bool pomo_break;
static int pomo_remaining;
static int pomo_phase_total = 25 * 60;
static int pomo_done;
static int pomo_cycles = 4;
/** 番茄钟页每这么多拍（秒）与核心对一次表。 */
#define POMO_POLL_TICKS 5

static void pomo_refresh(void);

/** 把核心返回的番茄钟状态读进界面变量。 */
static bool pomo_sync(const char *json)
{
    char phase[16] = "";
    bool flag = false;
    int value = 0;
    int focus_minutes = 25;
    int break_minutes = 5;

    if (!qz_json_bool(json, "active", &flag)) return false;
    pomo_active = flag;
    pomo_paused = false;
    qz_json_bool(json, "paused", &pomo_paused);
    if (qz_json_string(json, "phase", phase, sizeof(phase))) {
        pomo_break = strcmp(phase, "break") == 0;
    }
    if (qz_json_int(json, "remaining_seconds", &value)) pomo_remaining = value;
    if (qz_json_int(json, "completed_cycles", &value)) pomo_done = value;
    if (qz_json_int(json, "total_cycles", &value) && value > 0) pomo_cycles = value;
    qz_json_int(json, "focus_minutes", &focus_minutes);
    qz_json_int(json, "break_minutes", &break_minutes);
    pomo_phase_total = (pomo_break ? break_minutes : focus_minutes) * 60;
    if (pomo_phase_total <= 0) pomo_phase_total = 60;
    return true;
}

static bool pomo_reload(void)
{
    static char body[2048];

    if (!qz_web_request("GET", "/api/pomodoro", NULL, body, sizeof(body))) return false;
    return pomo_sync(body);
}

/** 给核心发一个动作，成功后用核心返回的状态刷新界面。 */
static void pomo_send(const char *action)
{
    static char body[2048];
    char payload[48];

    snprintf(payload, sizeof(payload), "{\"action\":\"%s\"}", action);
    if (!qz_web_request("POST", "/api/pomodoro", payload, body, sizeof(body))) return;
    if (pomo_sync(body)) pomo_refresh();
}

static void pomo_refresh(void)
{
    lv_arc_set_range(pomo_arc, 0, pomo_phase_total);
    lv_arc_set_value(pomo_arc, pomo_phase_total - pomo_remaining);
    /* Focus is the accent; rest is the healthy-state green. */
    lv_obj_set_style_arc_color(pomo_arc, qz_color(pomo_break ? QZ_GREEN : QZ_ACCENT),
                               LV_PART_INDICATOR);

    char clock[16];
    snprintf(clock, sizeof(clock), "%02d:%02d", pomo_remaining / 60, pomo_remaining % 60);
    lv_label_set_text(pomo_clock, clock);
    if (!pomo_active) {
        lv_label_set_text(pomo_phase_chip, "未开始");
        lv_obj_set_style_text_color(pomo_phase_chip, qz_color(QZ_TEXT_TERTIARY), 0);
    } else if (pomo_paused) {
        lv_label_set_text(pomo_phase_chip, "已暂停");
        lv_obj_set_style_text_color(pomo_phase_chip, qz_color(QZ_TEXT_SECONDARY), 0);
    } else {
        lv_label_set_text(pomo_phase_chip, pomo_break ? "休息一下" : "专注中");
        lv_obj_set_style_text_color(pomo_phase_chip,
                                   qz_color(pomo_break ? QZ_GREEN : QZ_ACCENT_TEXT), 0);
    }
    char count[40];
    snprintf(count, sizeof(count), "已完成 %d/%d 轮", pomo_done, pomo_cycles);
    lv_label_set_text(pomo_count, count);
    if (pomo_start_caption) {
        lv_label_set_text(pomo_start_caption,
                          !pomo_active ? "开始" : (pomo_paused ? "继续" : "暂停"));
    }
}

/** 每拍走一秒；换阶段与结束由核心判定，这里只让秒数看起来是连续走的。 */
static void pomo_tick(void)
{
    static int since_sync;

    if (lv_screen_active() != screens[QZ_APPLET_POMODORO]) {
        since_sync = 0;
        return;
    }
    if (since_sync <= 0) {
        since_sync = POMO_POLL_TICKS;
        if (pomo_reload()) pomo_refresh();
        return;
    }
    since_sync--;
    if (pomo_active && !pomo_paused && pomo_remaining > 0) {
        pomo_remaining--;
        pomo_refresh();
    }
}

static void pomo_toggle(lv_event_t *event)
{
    (void)event;
    if (!pomo_active) pomo_send("start");
    else if (pomo_paused) pomo_send("resume");
    else pomo_send("pause");
}

static void pomo_reset(lv_event_t *event)
{
    (void)event;
    pomo_send("stop");
}

static void build_pomodoro_screen(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    qz_style_screen(screen);
    screens[QZ_APPLET_POMODORO] = screen;
    make_toolbar(screen, "番茄钟");

    pomo_arc = lv_arc_create(screen);
    lv_obj_set_size(pomo_arc, 186, 186);
    lv_obj_align(pomo_arc, LV_ALIGN_TOP_MID, 0, 54);
    lv_arc_set_rotation(pomo_arc, 135);
    lv_arc_set_bg_angles(pomo_arc, 0, 270);
    lv_arc_set_range(pomo_arc, 0, pomo_phase_total);
    lv_arc_set_value(pomo_arc, 0);
    lv_obj_remove_style(pomo_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(pomo_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(pomo_arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(pomo_arc, qz_color(QZ_FILL), LV_PART_MAIN);
    lv_obj_set_style_arc_width(pomo_arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(pomo_arc, qz_color(QZ_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(pomo_arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(pomo_arc, true, LV_PART_INDICATOR);

    pomo_phase_chip = qz_text(pomo_arc, "专注中", 11, qz_color(QZ_ACCENT_TEXT));
    lv_obj_align(pomo_phase_chip, LV_ALIGN_CENTER, 0, -38);

    pomo_clock = qz_text(pomo_arc, "25:00", qz_compact() ? 36 : 30, qz_color(QZ_TEXT));
    lv_obj_align(pomo_clock, LV_ALIGN_CENTER, 0, -4);

    pomo_count = qz_text(pomo_arc, "已完成 0 个番茄", 10, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(pomo_count, LV_ALIGN_CENTER, 0, 32);

    lv_obj_t *reset = qz_icon_button(screen, LV_SYMBOL_REFRESH, 44);
    lv_obj_align(reset, LV_ALIGN_TOP_LEFT, 132, 252);
    lv_obj_add_event_cb(reset, pomo_reset, LV_EVENT_CLICKED, NULL);

    lv_obj_t *start = lv_button_create(screen);
    lv_obj_set_size(start, 160, 44);
    lv_obj_align(start, LV_ALIGN_TOP_LEFT, 188, 252);
    qz_style_primary_button(start);
    pomo_start_caption = qz_text(start, "开始", 15, qz_color(QZ_TEXT_ON_ACCENT));
    lv_obj_center(pomo_start_caption);
    lv_obj_add_event_cb(start, pomo_toggle, LV_EVENT_CLICKED, NULL);

    /* 建页时先对一次表：核心可能已经有一段在跑的番茄钟（语音开的） */
    pomo_reload();
    pomo_refresh();
}

/* ------------------------------------------------------------------------- *
 * 设备控制
 * ------------------------------------------------------------------------- */

static lv_obj_t *control_core_value;
static lv_obj_t *control_hub_chip;
static lv_obj_t *control_hint;
static lv_obj_t *control_device_list;
static lv_obj_t *control_local_card;
static lv_obj_t *control_hub_card;
static lv_obj_t *control_local_tab;
static lv_obj_t *control_devices_tab;
static lv_obj_t *control_page_label;
static lv_obj_t *control_previous;
static lv_obj_t *control_next;
static int control_page;
/* Neutral defaults; overwritten with the real values when the page loads. */
static int control_volume = 65;
static int control_backlight = 40;

/* 局域网设备：就地存一份。行里的开关回调只能拿到一个下标，靠它索引这张表。 */
static qz_device_t control_devices[QZ_DEVICE_MAX];
static int control_device_count;

/* Each slider carries its percentage label and the hardware hook that applies
 * the new value (speaker via amixer, backlight via sysfs). */
typedef struct {
    lv_obj_t *label;
    lv_obj_t *slider;
    bool (*apply)(int);
} control_binding_t;

static control_binding_t control_bindings[2];

static void slider_changed(lv_event_t *event)
{
    lv_obj_t *slider = lv_event_get_target(event);
    control_binding_t *binding = (control_binding_t *)lv_obj_get_user_data(slider);
    int value = (int)lv_slider_get_value(slider);
    if (!binding) return;
    char text[8];
    snprintf(text, sizeof(text), "%d%%", value);
    if (binding->label) lv_label_set_text(binding->label, text);
    if (binding->apply) binding->apply(value);
}

static void control_reload(void);

/* 重建列表要等事件处理完再做：开关的回调里若当场删掉自己所在的控件，
 * LVGL 会在事件派发中还拿着一个已释放的对象。延时一帧最省事。 */
static void control_reload_deferred(lv_timer_t *timer)
{
    lv_timer_delete(timer);
    control_reload();
}

static void control_reload_later(void)
{
    lv_timer_t *timer = lv_timer_create(control_reload_deferred, 40, NULL);
    lv_timer_set_repeat_count(timer, 1);
}

static void device_switch_changed(lv_event_t *event)
{
    lv_obj_t *toggle = lv_event_get_target(event);
    int index = (int)(intptr_t)lv_obj_get_user_data(toggle);
    if (index < 0 || index >= control_device_count) return;
    bool on = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    qz_device_command(control_devices[index].id, on ? "on" : "off", 0);
    control_reload_later();
}

/** 一台设备一行：图标 + 名字/在线状态 + 状态字 + 开关。 */
static void device_row(lv_obj_t *parent, int index, int y)
{
    const qz_device_t *device = &control_devices[index];
    char sub[40];
    char state_text[16];

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), 80);
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = qz_squircle(row, 30, QZ_ACCENT_TINT);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_t *glyph = qz_symbol(icon, device->is_light ? LV_SYMBOL_TINT : LV_SYMBOL_POWER,
                                14, qz_color(QZ_ACCENT_DARK));
    lv_obj_center(glyph);

    if (device->is_light && device->has_state && device->brightness > 0) {
        snprintf(sub, sizeof(sub), "%s · 亮度 %d%%", device->online ? "在线" : "离线",
                 device->brightness);
    } else {
        snprintf(sub, sizeof(sub), "%s", device->online ? "在线" : "离线");
    }
    lv_obj_t *name = qz_text(row, device->name, 13, qz_color(QZ_TEXT));
    lv_obj_set_width(name, lv_pct(46));
    lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 50, 7);
    lv_obj_t *hint = qz_text(row, sub, 10, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 50, 60);

    /* 还没上报过状态的设备显示 "--"，而不是假装它是关着的 */
    if (!device->has_state) {
        snprintf(state_text, sizeof(state_text), "--");
    } else {
        snprintf(state_text, sizeof(state_text), "%s", device->state ? "开" : "关");
    }
    lv_obj_t *state = qz_text(row, state_text, 13,
                              qz_color(device->state && device->has_state ? QZ_ACCENT_TEXT
                                                                         : QZ_TEXT_SECONDARY));
    lv_obj_align(state, LV_ALIGN_RIGHT_MID, -86, 0);

    lv_obj_t *toggle = lv_switch_create(row);
    qz_style_switch(toggle);
    lv_obj_set_size(toggle, 60, 32);
    lv_obj_align(toggle, LV_ALIGN_RIGHT_MID, -10, 0);
    if (device->has_state && device->state) lv_obj_add_state(toggle, LV_STATE_CHECKED);
    lv_obj_set_user_data(toggle, (void *)(intptr_t)index);
    lv_obj_add_event_cb(toggle, device_switch_changed, LV_EVENT_VALUE_CHANGED, NULL);
}

/** 取一次设备表，把中枢状态与设备行重画一遍。 */
static void control_reload(void)
{
    qz_hub_status_t hub;
    char text[64];
    control_device_count = qz_devices_fetch(control_devices, QZ_DEVICE_MAX, &hub);

    if (control_hub_chip) {
        if (control_device_count < 0) {
            lv_label_set_text(control_hub_chip, "服务未连接");
            lv_obj_set_style_text_color(control_hub_chip, qz_color(QZ_TEXT_TERTIARY), 0);
        } else if (!hub.enabled) {
            lv_label_set_text(control_hub_chip, "未启用");
            lv_obj_set_style_text_color(control_hub_chip, qz_color(QZ_TEXT_SECONDARY), 0);
        } else if (!hub.connected) {
            lv_label_set_text(control_hub_chip, "未连接");
            lv_obj_set_style_text_color(control_hub_chip, qz_color(QZ_ORANGE), 0);
        } else {
            snprintf(text, sizeof(text), "已连接 · %d 台", hub.declared);
            lv_label_set_text(control_hub_chip, text);
            lv_obj_set_style_text_color(control_hub_chip, qz_color(QZ_GREEN), 0);
        }
    }

    /* 没配上 broker 时，最有用的一句话是"去哪里配" */
    if (control_hint) {
        char address[32];
        if (control_device_count < 0) {
            snprintf(text, sizeof(text), qz_compact() ? "核心未响应，点顶部重连"
                                                     : "本地服务未响应，点下方重试");
            lv_label_set_text(control_hint, text);
            lv_obj_clear_flag(control_hint, LV_OBJ_FLAG_HIDDEN);
        } else if (!hub.enabled || !hub.connected) {
            qz_device_ip(address, sizeof(address));
            snprintf(text, sizeof(text), "浏览器打开设备 IP:8080 配置智能家居");
            lv_label_set_text(control_hint, text);
            lv_obj_clear_flag(control_hint, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(control_hint, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (!control_device_list) return;
    lv_obj_clean(control_device_list);
    int pages = control_device_count > 0 ? control_device_count : 1;
    if (control_page >= pages) control_page = pages - 1;
    if (control_page < 0) control_page = 0;
    if (control_page_label) {
        char page_text[32];
        snprintf(page_text, sizeof(page_text), "%d / %d", control_page + 1, pages);
        lv_label_set_text(control_page_label, page_text);
        set_page_button_enabled(control_previous, control_page > 0);
        set_page_button_enabled(control_next, control_page + 1 < pages);
    }
    if (control_device_count <= 0) {
        lv_obj_t *empty = qz_text(control_device_list, "还没有局域网设备", 12,
                                  qz_color(QZ_TEXT_TERTIARY));
        lv_obj_align(empty, LV_ALIGN_TOP_LEFT, 12, 10);
        lv_obj_set_height(control_device_list, 80);
        return;
    }
    device_row(control_device_list, control_page, 0);
    lv_obj_set_height(control_device_list, 80);
}

static void hub_refresh_clicked(lv_event_t *event)
{
    (void)event;
    control_reload();
}

static void hub_discover_clicked(lv_event_t *event)
{
    (void)event;
    qz_devices_discover();
    control_reload_later();
}

/** Pull the real values back in whenever the page is opened. */
static void control_screen_loaded(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_SCREEN_LOADED) return;
    int volume = qz_volume_level();
    int backlight = qz_backlight_level();
    if (volume >= 0) control_volume = volume;
    if (backlight >= 0) control_backlight = backlight;
    lv_slider_set_value(control_bindings[0].slider, control_volume, LV_ANIM_OFF);
    lv_slider_set_value(control_bindings[1].slider, control_backlight, LV_ANIM_OFF);
    char text[8];
    snprintf(text, sizeof(text), "%d%%", control_volume);
    lv_label_set_text(control_bindings[0].label, text);
    snprintf(text, sizeof(text), "%d%%", control_backlight);
    lv_label_set_text(control_bindings[1].label, text);
    control_reload();
}

static void reconnect_core(lv_event_t *event)
{
    (void)event;
    if (qzdesk_core_open() && qzdesk_core_request_status()) {
        lv_label_set_text(control_core_value, "已请求状态");
    } else {
        lv_label_set_text(control_core_value, "核心未运行");
    }
    control_reload_later();
}

static void control_page_changed(lv_event_t *event)
{
    control_page += (int)(intptr_t)lv_event_get_user_data(event);
    control_reload();
}

static void control_tab_changed(lv_event_t *event)
{
    bool devices = (intptr_t)lv_event_get_user_data(event) != 0;
    if (devices) {
        lv_obj_add_flag(control_local_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(control_hub_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(control_previous, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(control_next, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(control_page_label, LV_OBJ_FLAG_HIDDEN);
        control_reload();
    } else {
        lv_obj_clear_flag(control_local_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(control_hub_card, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(control_previous, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(control_next, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(control_page_label, LV_OBJ_FLAG_HIDDEN);
    }
    qz_obj_set_bg_color(control_local_tab, devices ? QZ_FILL : QZ_ACCENT_TINT, 0);
    qz_obj_set_bg_color(control_devices_tab, devices ? QZ_ACCENT_TINT : QZ_FILL, 0);
}

/* A fixed local view and a paged device view leave room for readable labels
 * and 30px controls at the minimum panel size. Dynamic device counts never
 * increase the height of the screen. */
static void build_compact_control(lv_obj_t *screen, lv_obj_t *toolbar)
{
    lv_obj_t *reconnect = qz_icon_button(toolbar, LV_SYMBOL_REFRESH, 40);
    lv_obj_align(reconnect, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_add_event_cb(reconnect, reconnect_core, LV_EVENT_CLICKED, NULL);

    control_local_tab = qz_button(screen, "本机", 216, 40);
    lv_obj_align(control_local_tab, LV_ALIGN_TOP_LEFT, QZ_GUTTER, 58);
    qz_obj_set_bg_color(control_local_tab, QZ_ACCENT_TINT, 0);
    lv_obj_add_event_cb(control_local_tab, control_tab_changed, LV_EVENT_CLICKED, NULL);
    control_devices_tab = qz_button(screen, "局域网设备", 216, 40);
    lv_obj_align(control_devices_tab, LV_ALIGN_TOP_RIGHT, -QZ_GUTTER, 58);
    lv_obj_add_event_cb(control_devices_tab, control_tab_changed, LV_EVENT_CLICKED,
                        (void *)(intptr_t)1);

    control_local_card = make_card(screen, 106, 208);
    struct {
        const char *symbol;
        const char *title;
        int *value;
        bool (*apply)(int);
    } rows[] = {
        { LV_SYMBOL_VOLUME_MAX, "声音", &control_volume, qz_volume_set },
        { LV_SYMBOL_EYE_OPEN, "背光", &control_backlight, qz_backlight_set },
    };
    for (int i = 0; i < 2; i++) {
        int y = i * 64;
        lv_obj_t *icon = qz_squircle(control_local_card, 32, QZ_ACCENT);
        lv_obj_align(icon, LV_ALIGN_TOP_LEFT, CARD_PAD, y + 12);
        lv_obj_t *glyph = qz_symbol(icon, rows[i].symbol, 16, qz_color(QZ_TEXT_ON_ACCENT));
        lv_obj_center(glyph);
        lv_obj_t *title = qz_text(control_local_card, rows[i].title, 16, qz_color(QZ_TEXT));
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 54, y + 8);
        lv_obj_t *slider = lv_slider_create(control_local_card);
        lv_obj_set_size(slider, CONTENT_W - 160, 26);
        lv_obj_align(slider, LV_ALIGN_TOP_LEFT, 54, y + 34);
        qz_style_slider(slider);
        lv_obj_set_ext_click_area(slider, 5);
        lv_slider_set_range(slider, 0, 100);
        lv_slider_set_value(slider, *rows[i].value, LV_ANIM_OFF);
        char text[8];
        snprintf(text, sizeof(text), "%d%%", *rows[i].value);
        lv_obj_t *value = qz_text(control_local_card, text, 16, qz_color(QZ_TEXT_SECONDARY));
        lv_obj_align(value, LV_ALIGN_TOP_RIGHT, -CARD_PAD, y + 36);
        control_bindings[i].label = value;
        control_bindings[i].slider = slider;
        control_bindings[i].apply = rows[i].apply;
        lv_obj_set_user_data(slider, &control_bindings[i]);
        lv_obj_add_event_cb(slider, slider_changed, LV_EVENT_VALUE_CHANGED, NULL);
    }
    lv_obj_t *sep = qz_separator(control_local_card, CONTENT_W - CARD_PAD * 2, false);
    lv_obj_align(sep, LV_ALIGN_TOP_LEFT, CARD_PAD, 140);
    lv_obj_t *sound = qz_text(control_local_card, "按键提示音", 16, qz_color(QZ_TEXT));
    lv_obj_align(sound, LV_ALIGN_TOP_LEFT, CARD_PAD, 154);
    lv_obj_t *toggle = lv_switch_create(control_local_card);
    qz_style_switch(toggle);
    lv_obj_set_size(toggle, 60, 32);
    lv_obj_align(toggle, LV_ALIGN_TOP_RIGHT, -CARD_PAD, 148);
    lv_obj_add_state(toggle, LV_STATE_CHECKED);
    control_core_value = qz_text(control_local_card, "顶部刷新可重新连接核心", 14,
                                 qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(control_core_value, LV_ALIGN_TOP_LEFT, CARD_PAD, 185);

    control_hub_card = make_card(screen, 106, 164);
    lv_obj_t *head = lv_obj_create(control_hub_card);
    lv_obj_set_size(head, CONTENT_W - CARD_PAD * 2, 44);
    lv_obj_align(head, LV_ALIGN_TOP_LEFT, CARD_PAD, 6);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *head_title = qz_text(head, "网关", 16, qz_color(QZ_TEXT));
    lv_obj_align(head_title, LV_ALIGN_LEFT_MID, 0, 0);
    control_hub_chip = qz_text(head, "读取中", 14, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(control_hub_chip, LV_ALIGN_LEFT_MID, 64, 0);
    lv_obj_t *discover = qz_icon_button(head, LV_SYMBOL_DOWNLOAD, 40);
    lv_obj_align(discover, LV_ALIGN_RIGHT_MID, -48, 0);
    lv_obj_add_event_cb(discover, hub_discover_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *refresh = qz_icon_button(head, LV_SYMBOL_REFRESH, 40);
    lv_obj_align(refresh, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(refresh, hub_refresh_clicked, LV_EVENT_CLICKED, NULL);

    control_device_list = lv_obj_create(control_hub_card);
    lv_obj_set_size(control_device_list, CONTENT_W - CARD_PAD * 2, 80);
    lv_obj_align(control_device_list, LV_ALIGN_TOP_LEFT, CARD_PAD, 56);
    lv_obj_set_style_bg_opa(control_device_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(control_device_list, 0, 0);
    lv_obj_set_style_pad_all(control_device_list, 0, 0);
    lv_obj_clear_flag(control_device_list, LV_OBJ_FLAG_SCROLLABLE);
    control_hint = qz_text(control_hub_card, "", 14, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_set_width(control_hint, CONTENT_W - CARD_PAD * 2);
    lv_label_set_long_mode(control_hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(control_hint, LV_ALIGN_TOP_LEFT, CARD_PAD, 140);

    control_previous = qz_button(screen, "上一台", 116, 40);
    lv_obj_align(control_previous, LV_ALIGN_TOP_LEFT, QZ_GUTTER, 276);
    lv_obj_add_event_cb(control_previous, control_page_changed, LV_EVENT_CLICKED,
                        (void *)(intptr_t)-1);
    control_next = qz_button(screen, "下一台", 116, 40);
    lv_obj_align(control_next, LV_ALIGN_TOP_RIGHT, -QZ_GUTTER, 276);
    lv_obj_add_event_cb(control_next, control_page_changed, LV_EVENT_CLICKED,
                        (void *)(intptr_t)1);
    control_page_label = qz_text(screen, "1 / 1", 16, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(control_page_label, LV_ALIGN_TOP_MID, 0, 286);
    lv_obj_add_flag(control_hub_card, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(control_previous, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(control_next, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(control_page_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(screen, control_screen_loaded, LV_EVENT_ALL, NULL);
}

static void build_control_screen(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    qz_style_screen(screen);
    screens[QZ_APPLET_CONTROL] = screen;
    lv_obj_t *toolbar = make_toolbar(screen, "设备控制");
    build_compact_control(screen, toolbar);
}

#if 0  /* 存在检测（摄像头） 已移除（相机不可用） */
/* ------------------------------------------------------------------------- *
 * 存在检测（摄像头）
 * ------------------------------------------------------------------------- */

/* 后端与接口说明见 include/face_camera.h。这里只做两件事：
 *
 *   1. 「有人靠近」时把屏幕点亮（背光被调暗过就拉回来，用户调得更亮就不动）；
 *   2. 把这件事报给核心，由它决定怎么打招呼 —— 问候语因此会和其他消息一样进
 *      聊天记录，网页控制台也看得到，而不是只在设备上闪一下。
 *
 * 检测回调跑在工作线程上，所以它只置一个标记，真正的处理都在 LVGL 线程的
 * `face_tick`（由 applet_tick 每秒调用）里。 */
#define FACE_WAKE_BRIGHTNESS 35

static lv_obj_t *face_state_label;
static lv_obj_t *face_backend_label;
static lv_obj_t *face_count_label;
static lv_obj_t *face_action_caption;
static lv_obj_t *face_detail_overlay;
static char face_backend_message[144];
static volatile int face_presence_pending;
static volatile int face_presence_arrived;

static void face_detail_close(lv_event_t *event)
{
    (void)event;
    if (face_detail_overlay) lv_obj_delete(face_detail_overlay);
    face_detail_overlay = NULL;
}

static void face_detail_open(lv_event_t *event)
{
    (void)event;
    if (face_detail_overlay) return;
    lv_obj_t *panel = lv_obj_create(face_detail_overlay);
    lv_obj_set_size(panel, CONTENT_W, 288);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);
    qz_style_plate(panel);
    lv_obj_set_style_pad_all(panel, CARD_PAD, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = qz_text(panel, "摄像头状态", 18, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *message = qz_text(panel, face_backend_message, 14,
                                qz_color(QZ_TEXT_SECONDARY));
    lv_obj_set_width(message, lv_pct(100));
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
    lv_obj_align(message, LV_ALIGN_TOP_LEFT, 0, 30);
    lv_obj_t *close = qz_button(panel, "关闭", 120, 44);
    lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(close, face_detail_close, LV_EVENT_CLICKED, NULL);
}

/** 工作线程：只记标记，不要碰 LVGL 对象。 */
static void face_presence_event(qz_presence_event_t event, void *user_data)
{
    (void)user_data;
    face_presence_arrived = (event == QZ_PRESENCE_ARRIVE);
    face_presence_pending = 1;
}

static void face_refresh(void)
{
    bool running = qz_face_camera_running();
    bool available = qz_face_camera_available();
    bool here = running && qz_face_camera_present();
    char text[144];
    char clock[24];

    if (face_state_label) {
        const char *state = running ? (here ? "已检测到有人" : "运行中 · 暂无人")
                                    : (available ? "未启动" : "不可用");
        lv_label_set_text(face_state_label, state);
        lv_obj_set_style_text_color(face_state_label,
                                   qz_color(here ? QZ_GREEN : QZ_TEXT_SECONDARY), 0);
    }
    if (face_backend_label) {
        snprintf(face_backend_message, sizeof(face_backend_message), "%s · %s", qz_face_camera_backend(),
                 qz_face_camera_status());
        snprintf(text, sizeof(text), "%s · 查看状态", qz_face_camera_backend());
        lv_label_set_text(face_backend_label, text);
    }
    if (face_count_label) {
        unsigned long long stamp = qz_face_camera_last_seen();
        if (stamp) {
            time_t seconds = (time_t)stamp;
            struct tm local;
            localtime_r(&seconds, &local);
            snprintf(clock, sizeof(clock), "%02d:%02d", local.tm_hour, local.tm_min);
        } else {
            snprintf(clock, sizeof(clock), "还没检测到");
        }
        snprintf(text, sizeof(text), "靠近 %d 次 · 最近 %s", qz_face_camera_arrivals(), clock);
        lv_label_set_text(face_count_label, text);
    }
    if (face_action_caption) {
        lv_label_set_text(face_action_caption, running ? "停止检测" : "启动检测");
    }
}

static void face_toggle(lv_event_t *event)
{
    (void)event;
    if (qz_face_camera_running()) {
        qz_face_camera_stop();
    } else {
        /* 起不来时状态行会写明原因（没摄像头 / 格式不支持） */
        qz_face_camera_start(face_presence_event, NULL);
    }
    face_refresh();
}

/** 每秒一次：把工作线程攒下的结果落到界面与核心上。 */
static void face_tick(void)
{
    if (!face_presence_pending) return;
    face_presence_pending = 0;

    if (face_presence_arrived) {
        int target = control_backlight > FACE_WAKE_BRIGHTNESS
                         ? control_backlight
                         : FACE_WAKE_BRIGHTNESS;
        int current = qz_backlight_level();
        if (current >= 0 && current < target) qz_backlight_set(target);
        qzdesk_core_notify_presence(true);
    } else {
        qzdesk_core_notify_presence(false);
    }
    face_refresh();
}

static void build_face_screen(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    qz_style_screen(screen);
    make_toolbar(screen, "存在检测");

    lv_obj_t *card = make_card(screen, 56, 178);
    lv_obj_t *icon = qz_squircle(card, 42, QZ_ACCENT_TINT);
    lv_obj_align(icon, LV_ALIGN_TOP_LEFT, CARD_PAD, 16);
    lv_obj_t *glyph = qz_symbol(icon, LV_SYMBOL_EYE_OPEN, 19, qz_color(QZ_ACCENT_DARK));
    lv_obj_center(glyph);

    lv_obj_t *title = qz_text(card, "有人靠近自动亮屏", 15, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, CARD_PAD + 54, 18);

    face_state_label = qz_text(card, "未启动", 12, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(face_state_label, LV_ALIGN_TOP_LEFT, CARD_PAD + 54, 45);

    lv_obj_t *details = qz_button(card, "查看摄像头状态", CONTENT_W - CARD_PAD * 2, 44);
    lv_obj_align(details, LV_ALIGN_TOP_LEFT, CARD_PAD, 70);
    lv_obj_add_event_cb(details, face_detail_open, LV_EVENT_CLICKED, NULL);
    face_backend_label = lv_obj_get_child(details, 0);

    lv_obj_t *hint = qz_text(card, "靠近时自动亮屏并问候。\n每 5 分钟最多问候一次。",
        10, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_set_width(hint, CONTENT_W - CARD_PAD * 2);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, CARD_PAD, 134);

    lv_obj_t *counter = make_card(screen, 240, 28);
    face_count_label = qz_text(counter, "靠近 0 次 · 最近还没检测到", 12,
                               qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(face_count_label, LV_ALIGN_LEFT_MID, CARD_PAD, 0);

    lv_obj_t *action = lv_button_create(screen);
    lv_obj_set_size(action, CONTENT_W, 44);
    lv_obj_align(action, LV_ALIGN_TOP_LEFT, QZ_GUTTER, 272);
    qz_style_primary_button(action);
    face_action_caption = qz_text(action, "启动检测", 14, qz_color(QZ_TEXT_ON_ACCENT));
    lv_obj_center(face_action_caption);
    lv_obj_add_event_cb(action, face_toggle, LV_EVENT_CLICKED, NULL);

    face_refresh();
}

#endif
#if 0  /* 运动相机 已移除（相机不可用） */
/* ------------------------------------------------------------------------- *
 * 运动相机（形态参考 Echo-Mate 的相机页）
 *
 * 全屏取景 + 极简 overlay，长运动相机的样子而不是应用卡片：
 *   - 顶部一行：后端状态 · 时钟 · 电量；
 *   - 底部一行：照片计数 · 快门（把当前帧存成 BMP）· 返回按钮；
 *   - 左/右滑返回（Echo-Mate 同款手势），不用为它腾一条工具栏。
 *
 * 与存在检测共用同一颗摄像头、不能同时开：进入本页先停存在检测，退出时
 * 恢复（cam_presence_was_running 记着账）。帧数据由 app/camera_preview.c
 * 在工作线程里转成面板尺寸的 RGB565，这里只负责在定时器里换源。
 * ------------------------------------------------------------------------- */
static lv_obj_t *cam_view;
static lv_obj_t *cam_hint;          /**< 还没出第一帧时居中的状态说明 */
static lv_obj_t *cam_state_label;
static lv_obj_t *cam_clock_label;
static lv_obj_t *cam_battery_label;
static lv_obj_t *cam_photo_label;
static lv_obj_t *cam_path_label;
static lv_obj_t *cam_flash;         /**< 快门白闪 */
static lv_timer_t *cam_frame_timer;
static lv_image_dsc_t cam_dsc;      /**< header 定面板尺寸；data 随帧切换 */
static uint32_t cam_last_id;
static int cam_flash_ticks;
static int cam_slow_ticks;          /**< 低频工作的计数器（时钟/状态 1s 一次） */
static bool cam_presence_was_running;
static int cam_wait_ticks;          /**< 等首帧的计数；-1 = 已把失败原因写进提示 */
static char cam_result[224];
static lv_obj_t *cam_result_overlay;

static void cam_result_close(lv_event_t *event)
{
    (void)event;
    if (cam_result_overlay) lv_obj_delete(cam_result_overlay);
    cam_result_overlay = NULL;
}

static void cam_result_open(lv_event_t *event)
{
    (void)event;
    if (!cam_result[0] || cam_result_overlay) return;
    lv_obj_t *panel = lv_obj_create(cam_result_overlay);
    lv_obj_set_size(panel, CONTENT_W, 288);
    lv_obj_align(panel, LV_ALIGN_CENTER, 0, 0);
    qz_style_plate(panel);
    lv_obj_set_style_pad_all(panel, CARD_PAD, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = qz_text(panel, "拍摄结果", 18, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *result = qz_text(panel, cam_result, 14, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_set_width(result, lv_pct(100));
    lv_label_set_long_mode(result, LV_LABEL_LONG_WRAP);
    lv_obj_align(result, LV_ALIGN_TOP_LEFT, 0, 30);
    lv_obj_t *close = qz_button(panel, "关闭", 120, 44);
    lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(close, cam_result_close, LV_EVENT_CLICKED, NULL);
}

#define CAM_TIMER_PERIOD_MS 66      /**< ≈15fps，取景比这更密没有意义 */

/** 运动相机的 OSD 底片：半透明黑圆角条，白字压在任何画面上都可读 ——
 * 白色 OSD 直接落在彩条的白色那道上是看不见的。 */
static lv_obj_t *cam_osd(lv_obj_t *parent, const char *text, int32_t size)
{
    lv_obj_t *label = qz_text(parent, text, size, qz_color(QZ_TEXT));
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_color(label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(label, (lv_opa_t)96, 0);
    lv_obj_set_style_radius(label, 6, 0);
    lv_obj_set_style_pad_hor(label, 6, 0);
    lv_obj_set_style_pad_ver(label, 2, 0);
    return label;
}

static void cam_refresh_state(void)
{
    bool running = qz_cam_preview_running();
    lv_label_set_text(cam_state_label, running ? "取景中" : "无画面");
    {
        int battery = qz_battery_level();
        char text[12];
        if (battery >= 0) snprintf(text, sizeof(text), "%d%%", battery);
        else snprintf(text, sizeof(text), "--");
        lv_label_set_text(cam_battery_label, text);
    }
    {
        char clock[8];
        time_t now = time(NULL);
        struct tm local;
        localtime_r(&now, &local);
        strftime(clock, sizeof(clock), "%H:%M", &local);
        lv_label_set_text(cam_clock_label, clock);
    }
    lv_obj_set_style_text_color(cam_state_label,
                                lv_color_hex(running ? 0xFFFFFF : 0xDDDDDD), 0);
}

static void cam_shutter(lv_event_t *event)
{
    (void)event;
    char path[224];

    if (qz_cam_preview_snapshot(path, sizeof(path))) {
        char photos[24];
        snprintf(photos, sizeof(photos), "照片 %d", qz_cam_preview_photos());
        lv_label_set_text(cam_photo_label, photos);
        snprintf(cam_result, sizeof(cam_result), "%s", path);
        lv_label_set_text(cam_path_label, "已保存·查看位置");
        cam_flash_ticks = 5;                       /* 快门白闪 ≈ 0.3s */
        lv_obj_set_style_bg_opa(cam_flash, (lv_opa_t)200, 0);
    } else {
        snprintf(cam_result, sizeof(cam_result), "%s", qz_cam_preview_status());
        lv_label_set_text(cam_path_label, "失败·查看原因");
    }
}

static void cam_gesture(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_GESTURE) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
        qz_screen_load(apps_screen_ref, LV_SCR_LOAD_ANIM_MOVE_RIGHT, QZ_DUR_SCREEN);
    }
}

/**
 * 取景起不来（或起了却一直没画面）时把原因写明。
 *
 * 之前只有"出帧才隐藏提示"，于是没摄像头时"正在打开取景…"会永远挂着 ——
 * 用户看到的就是一个卡死的页面。这里把 qz_cam_preview_status() 的说明摆出来，
 * 并告诉用户能按返回。
 */
static void cam_show_reason(void)
{
    const char *status = qz_cam_preview_status();

    /* 不管"根本没启动"还是"启动了没画面"，用户要的是原因 + 出路：第一句说现象，
     * 第二句直接用采集侧的状态说明（例如"打不开摄像头（… /dev/video0..3）"）。 */
    lv_label_set_text_fmt(cam_hint, "取景没有画面\n%s\n（点右下角返回）", status ? status : "");
    lv_obj_set_style_text_color(cam_hint, qz_color(QZ_ORANGE), 0);
    lv_obj_clear_flag(cam_hint, LV_OBJ_FLAG_HIDDEN);
    cam_wait_ticks = -1;
}

/** 取景页主循环：换帧 + 快门闪 + 低频的时钟/状态刷新。 */
static void cam_tick(lv_timer_t *timer)
{
    (void)timer;

    const uint8_t *frame = qz_cam_preview_frame(&cam_last_id);
    if (frame) {
        cam_dsc.data = frame;
        lv_image_set_src(cam_view, &cam_dsc);      /* 换源触发重绘 */
        lv_obj_add_flag(cam_hint, LV_OBJ_FLAG_HIDDEN);
        cam_wait_ticks = 0;
        if (cam_slow_ticks == 0) cam_refresh_state();  /* 出帧后把状态行换成后端名 */
    } else if (cam_wait_ticks >= 0 && ++cam_wait_ticks >= 45) {
        /* ≈3 秒还没第一帧：不再让用户对着"正在打开取景…"干等 */
        cam_show_reason();
        cam_refresh_state();
    }

    if (cam_flash_ticks > 0 && --cam_flash_ticks == 0) {
        lv_obj_set_style_bg_opa(cam_flash, (lv_opa_t)0, 0);
    }

    if (++cam_slow_ticks >= 15) {                  /* ≈1s */
        cam_slow_ticks = 0;
        cam_refresh_state();
    }
}

static void cam_screen_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_SCREEN_LOADED) {
        /* 同一颗摄像头：先停存在检测，退出时再恢复（记在 cam_presence_was_running） */
        cam_presence_was_running = qz_face_camera_running();
        if (cam_presence_was_running) qz_face_camera_stop();
        /* 每次进来都复位提示：上次进来时写的"取景未能启动"不能留着 */
        cam_wait_ticks = 0;
        lv_label_set_text(cam_hint, "正在打开取景…");
        lv_obj_set_style_text_color(cam_hint, qz_color(QZ_TEXT_SECONDARY), 0);
        lv_obj_clear_flag(cam_hint, LV_OBJ_FLAG_HIDDEN);
        if (!qz_cam_preview_running() && !qz_cam_preview_start()) {
            cam_show_reason();          /* 立刻就知道起不来，不必等 3 秒 */
        }
        cam_slow_ticks = 0;
        cam_refresh_state();
    } else if (code == LV_EVENT_SCREEN_UNLOADED) {
        cam_result_close(NULL);
        if (qz_cam_preview_running()) qz_cam_preview_stop();
        if (cam_presence_was_running) {
            qz_face_camera_start(face_presence_event, NULL);
            cam_presence_was_running = false;
        }
    }
}

static void build_camera_screen(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    qz_style_screen(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);   /* 运动相机永远黑底 */
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    /* 取景层：全屏 image，内容是面板尺寸的 RGB565 缓冲 */
    cam_view = lv_image_create(screen);
    lv_obj_set_size(cam_view, QZ_DESIGN_W, QZ_DESIGN_H);
    lv_obj_align(cam_view, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_clear_flag(cam_view, LV_OBJ_FLAG_CLICKABLE);
    memset(&cam_dsc, 0, sizeof(cam_dsc));
    cam_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    cam_dsc.header.w = (uint32_t)qz_panel_w;
    cam_dsc.header.h = (uint32_t)qz_panel_h;
    cam_dsc.data_size = (size_t)qz_panel_w * qz_panel_h * 2;
    cam_dsc.data = NULL;

    /* 快门白闪层：平时全透明，拍下的一瞬提亮再淡掉 */
    cam_flash = lv_obj_create(screen);
    lv_obj_set_size(cam_flash, QZ_DESIGN_W, QZ_DESIGN_H);
    lv_obj_align(cam_flash, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(cam_flash, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(cam_flash, (lv_opa_t)0, 0);
    lv_obj_set_style_border_width(cam_flash, 0, 0);
    lv_obj_set_style_radius(cam_flash, 0, 0);
    lv_obj_clear_flag(cam_flash, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(cam_flash, LV_OBJ_FLAG_SCROLLABLE);

    /* 顶部一行：状态 · 时钟 · 电量（OSD 压在画面上） */
    cam_state_label = cam_osd(screen, "取景未启动", 10);
    lv_obj_align(cam_state_label, LV_ALIGN_TOP_LEFT, QZ_GUTTER, 8);
    cam_clock_label = cam_osd(screen, "--:--", 12);
    lv_obj_align(cam_clock_label, LV_ALIGN_TOP_MID, 0, 7);
    cam_battery_label = cam_osd(screen, "--", 10);
    lv_obj_align(cam_battery_label, LV_ALIGN_TOP_RIGHT, -QZ_GUTTER, 8);

    /* 底部一行：照片计数 · 快门 · 返回 */
    cam_photo_label = cam_osd(screen, "照片 0", 11);
    lv_obj_align(cam_photo_label, LV_ALIGN_BOTTOM_LEFT, QZ_GUTTER, -36);
    cam_path_label = cam_osd(screen, "", 8);
    lv_obj_align(cam_path_label, LV_ALIGN_BOTTOM_LEFT, QZ_GUTTER, -8);
    lv_obj_set_style_text_color(cam_path_label, lv_color_hex(0xDDDDDD), 0);
    lv_obj_set_width(cam_path_label, 180);
    lv_obj_add_flag(cam_path_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(cam_path_label, 4);
    lv_obj_add_event_cb(cam_path_label, cam_result_open, LV_EVENT_CLICKED, NULL);

    lv_obj_t *shutter = lv_obj_create(screen);
    lv_obj_set_size(shutter, 56, 56);
    lv_obj_align(shutter, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_radius(shutter, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(shutter, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(shutter, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(shutter, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(shutter, 3, 0);
    lv_obj_set_style_border_opa(shutter, (lv_opa_t)120, 0);
    lv_obj_add_event_cb(shutter, cam_shutter, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back = qz_icon_button(screen, LV_SYMBOL_LEFT, qz_compact() ? 44 : 30);
    lv_obj_align(back, LV_ALIGN_BOTTOM_RIGHT, -QZ_GUTTER, -14);
    lv_obj_add_event_cb(back, go_back, LV_EVENT_CLICKED, NULL);

    /* 还没出帧（起摄像头慢 / 根本没有）时给一行居中说明 */
    cam_hint = qz_text(screen, "正在打开取景…", 12, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(cam_hint, LV_ALIGN_CENTER, 0, 0);
    /* 失败时这里要放两行说明（原因可能带节点名与格式），给足宽度并居中换行 */
    lv_obj_set_width(cam_hint, QZ_DESIGN_W - 2 * QZ_GUTTER);
    lv_obj_set_style_text_align(cam_hint, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_add_event_cb(screen, cam_screen_event, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(screen, cam_screen_event, LV_EVENT_SCREEN_UNLOADED, NULL);
    lv_obj_add_event_cb(screen, cam_gesture, LV_EVENT_GESTURE, NULL);

    cam_frame_timer = lv_timer_create(cam_tick, CAM_TIMER_PERIOD_MS, NULL);
}

#endif
/* ------------------------------------------------------------------------- *
 * Public API
 * ------------------------------------------------------------------------- */

static void applet_tick(lv_timer_t *timer)
{
    static int reminder_ticks;

    (void)timer;
    pomo_tick();
    /* 提醒列表定期与核心对齐：语音刚加的、或刚响过被撤掉的一次性提醒 */
    if (--reminder_ticks <= 0) {
        reminder_ticks = REMINDER_POLL_TICKS;
        reminder_poll();
    }
}

void qz_applets_init(lv_obj_t *apps_screen)
{
    apps_screen_ref = apps_screen;
    build_status_screen();
    build_reminder_screen();
    build_pomodoro_screen();
    build_control_screen();
    /* 运动相机与存在检测已移除（本板相机链路跑不通，2026-10-09） */
    lv_timer_create(applet_tick, 1000, NULL);
}

lv_obj_t *qz_applets_screen(qz_applet_t id)
{
    return screens[id];
}
