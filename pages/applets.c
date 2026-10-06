#include "applets.h"
#include "theme.h"
#include "config.h"
#include "qzdesk_core.h"
#include "smarthome.h"
#include "face_camera.h"
#include "web_client.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <sys/sysinfo.h>

#define CONTENT_W (QZ_SCREEN_W - 2 * QZ_GUTTER)
#define CARD_PAD 12

/* ------------------------------------------------------------------------- *
 * Shared building blocks
 * ------------------------------------------------------------------------- */

static lv_obj_t *screens[QZ_APPLET_COUNT];
static lv_obj_t *apps_screen_ref;

static void go_back(lv_event_t *event)
{
    (void)event;
    qz_screen_load(apps_screen_ref, LV_SCR_LOAD_ANIM_MOVE_RIGHT, QZ_DUR_SCREEN);
}

static lv_obj_t *make_toolbar(lv_obj_t *screen, const char *title)
{
    lv_obj_t *toolbar = lv_obj_create(screen);
    lv_obj_set_size(toolbar, QZ_SCREEN_W - 16, QZ_TOOLBAR_H - 4);
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 6);
    qz_style_toolbar(toolbar);

    lv_obj_t *back = qz_icon_button(toolbar, LV_SYMBOL_LEFT, 30);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_event_cb(back, go_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = qz_text(toolbar, title, 15, qz_color(QZ_TEXT));
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

    lv_obj_t *value = qz_text(card, "--", 16, qz_color(QZ_TEXT));
    lv_obj_align(value, LV_ALIGN_TOP_LEFT, CARD_PAD, 50);

    lv_obj_t *hint = qz_text(card, caption, 10, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, CARD_PAD, 74);
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
        } else {
            snprintf(text, sizeof(text), "%ld 时 %02ld 分", up / 3600, (up % 3600) / 60);
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
    const char *values[] = { "1.0.0", NULL, NULL, "LVGL 9 · 480x320" };
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

static void rebuild_reminders(void)
{
    lv_obj_clean(reminder_list);
    if (reminder_count == 0) {
        lv_obj_t *empty = qz_text(reminder_list, "还没有提醒，点击下方按钮添加", 12,
                                  qz_color(QZ_TEXT_TERTIARY));
        lv_obj_center(empty);
        return;
    }

    for (int i = 0; i < reminder_count; i++) {
        lv_obj_t *row = lv_obj_create(reminder_list);
        lv_obj_set_size(row, CONTENT_W, 44);
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, (int32_t)i * 46 + 3);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        char clock[16];
        snprintf(clock, sizeof(clock), "%02d:%02d", reminders[i].hour, reminders[i].minute);
        lv_obj_t *time_label = qz_text(row, clock, 17, qz_color(QZ_TEXT));
        lv_obj_align(time_label, LV_ALIGN_LEFT_MID, 14, 0);

        lv_obj_t *name = qz_text(row, reminders[i].label, 12, qz_color(QZ_TEXT_SECONDARY));
        lv_obj_set_width(name, reminders[i].daily ? 74 : 100);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 78, 0);

        if (reminders[i].daily) {
            lv_obj_t *chip = qz_text(row, "每日", 10, qz_color(QZ_ACCENT_TEXT));
            lv_obj_align(chip, LV_ALIGN_LEFT_MID, 122, 0);
        }

        lv_obj_t *remove = qz_icon_button(row, LV_SYMBOL_CLOSE, 28);
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
    lv_obj_set_size(panel, QZ_SCREEN_W - 40, 236);
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
    lv_obj_set_size(add_daily_switch, 42, 26);
    lv_obj_align(add_daily_switch, LV_ALIGN_TOP_RIGHT, 0, 136);

    lv_obj_t *actions = lv_obj_create(panel);
    lv_obj_set_size(actions, lv_pct(100), 36);
    lv_obj_align(actions, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(actions, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(actions, 0, 0);
    lv_obj_set_style_pad_all(actions, 0, 0);
    lv_obj_set_style_pad_column(actions, 10, 0);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cancel = qz_button(actions, "取消", 88, 32);
    lv_obj_add_event_cb(cancel, close_add_modal, LV_EVENT_CLICKED, NULL);
    lv_obj_t *confirm = lv_button_create(actions);
    lv_obj_set_size(confirm, 96, 32);
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

    reminder_list = make_card(screen, 56, 200);
    reminder_banner = make_card(screen, 56, 38);
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

    pomo_clock = qz_text(pomo_arc, "25:00", 30, qz_color(QZ_TEXT));
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
    lv_obj_set_size(row, lv_pct(100), 46);
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
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 50, 7);
    lv_obj_t *hint = qz_text(row, sub, 10, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 50, 26);

    /* 还没上报过状态的设备显示 "--"，而不是假装它是关着的 */
    if (!device->has_state) {
        snprintf(state_text, sizeof(state_text), "--");
    } else {
        snprintf(state_text, sizeof(state_text), "%s", device->state ? "开" : "关");
    }
    lv_obj_t *state = qz_text(row, state_text, 13,
                              qz_color(device->state && device->has_state ? QZ_ACCENT_TEXT
                                                                         : QZ_TEXT_SECONDARY));
    lv_obj_align(state, LV_ALIGN_RIGHT_MID, -60, 0);

    lv_obj_t *toggle = lv_switch_create(row);
    qz_style_switch(toggle);
    lv_obj_set_size(toggle, 40, 24);
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
    int y = 0;

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
            snprintf(text, sizeof(text), "本地服务未响应，点下方重试");
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
    if (control_device_count <= 0) {
        lv_obj_t *empty = qz_text(control_device_list, "还没有局域网设备", 12,
                                  qz_color(QZ_TEXT_TERTIARY));
        lv_obj_align(empty, LV_ALIGN_TOP_LEFT, 12, 10);
        lv_obj_set_height(control_device_list, 42);
        return;
    }
    for (int i = 0; i < control_device_count; i++) {
        device_row(control_device_list, i, y);
        y += 48;
    }
    lv_obj_set_height(control_device_list, y);
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

static void build_control_screen(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    qz_style_screen(screen);
    screens[QZ_APPLET_CONTROL] = screen;
    make_toolbar(screen, "设备控制");

    /* 这一页会随局域网设备数量变高（本机开关 + 任意多台设备），所以内容放在
     * 一个可滚动的列容器里，而不是像别的页面那样按坐标摆死。 */
    lv_obj_t *page = lv_obj_create(screen);
    lv_obj_set_size(page, QZ_SCREEN_W, QZ_SCREEN_H - QZ_TOOLBAR_H - 4);
    lv_obj_align(page, LV_ALIGN_TOP_LEFT, 0, QZ_TOOLBAR_H);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_left(page, QZ_GUTTER, 0);
    lv_obj_set_style_pad_right(page, QZ_GUTTER, 0);
    lv_obj_set_style_pad_top(page, 6, 0);
    lv_obj_set_style_pad_bottom(page, 16, 0);
    lv_obj_set_style_pad_row(page, 10, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);

    /* 声音 / 背光 / 提示音 */
    lv_obj_t *card = lv_obj_create(page);
    lv_obj_set_size(card, lv_pct(100), 176);
    lv_obj_set_style_radius(card, QZ_RADIUS_CARD, 0);
    qz_style_plate(card);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    struct {
        const char *symbol;
        const char *title;
        int *value;
        bool (*apply)(int);
    } rows[2] = {
        { LV_SYMBOL_VOLUME_MAX, "声音", &control_volume, qz_volume_set },
        { LV_SYMBOL_EYE_OPEN, "背光", &control_backlight, qz_backlight_set },
    };
    for (int i = 0; i < 2; i++) {
        int y = i * 56 + 6;
        lv_obj_t *icon = qz_squircle(card, 32, QZ_ACCENT);
        lv_obj_align(icon, LV_ALIGN_TOP_LEFT, CARD_PAD, y + 4);
        lv_obj_t *glyph = qz_symbol(icon, rows[i].symbol, 15, qz_color(QZ_TEXT_ON_ACCENT));
        lv_obj_center(glyph);

        lv_obj_t *title = qz_text(card, rows[i].title, 13, qz_color(QZ_TEXT));
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, CARD_PAD + 42, y - 2);

        lv_obj_t *slider = lv_slider_create(card);
        lv_obj_set_size(slider, CONTENT_W - CARD_PAD * 2 - 96, 26);
        lv_obj_align(slider, LV_ALIGN_TOP_LEFT, CARD_PAD + 42, y + 16);
        qz_style_slider(slider);
        lv_slider_set_range(slider, 0, 100);
        lv_slider_set_value(slider, *rows[i].value, LV_ANIM_OFF);

        char text[8];
        snprintf(text, sizeof(text), "%d%%", *rows[i].value);
        lv_obj_t *value = qz_text(card, text, 12, qz_color(QZ_TEXT_SECONDARY));
        lv_obj_align(value, LV_ALIGN_TOP_RIGHT, -CARD_PAD, y + 20);
        control_bindings[i].label = value;
        control_bindings[i].slider = slider;
        control_bindings[i].apply = rows[i].apply;
        lv_obj_set_user_data(slider, &control_bindings[i]);
        lv_obj_add_event_cb(slider, slider_changed, LV_EVENT_VALUE_CHANGED, NULL);
    }

    lv_obj_t *sep = qz_separator(card, CONTENT_W - 2 * CARD_PAD, false);
    lv_obj_align(sep, LV_ALIGN_TOP_LEFT, CARD_PAD, 118);
    lv_obj_t *sound_icon = qz_squircle(card, 32, QZ_ACCENT);
    lv_obj_align(sound_icon, LV_ALIGN_TOP_LEFT, CARD_PAD, 130);
    lv_obj_t *sound_glyph = qz_symbol(sound_icon, LV_SYMBOL_BELL, 15, qz_color(QZ_TEXT_ON_ACCENT));
    lv_obj_center(sound_glyph);
    lv_obj_t *sound_title = qz_text(card, "按键提示音", 13, qz_color(QZ_TEXT));
    lv_obj_align(sound_title, LV_ALIGN_TOP_LEFT, CARD_PAD + 42, 138);
    lv_obj_t *sound_toggle = lv_switch_create(card);
    qz_style_switch(sound_toggle);
    lv_obj_set_size(sound_toggle, 42, 26);
    lv_obj_align(sound_toggle, LV_ALIGN_TOP_RIGHT, -CARD_PAD, 133);
    lv_obj_add_state(sound_toggle, LV_STATE_CHECKED);

    /* —— 局域网设备：与「本机」并列的第二块，设备多了这页就往下滚 —— */
    lv_obj_t *hub = lv_obj_create(page);
    lv_obj_set_size(hub, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(hub, QZ_RADIUS_CARD, 0);
    qz_style_plate(hub);
    lv_obj_set_style_pad_all(hub, CARD_PAD, 0);
    lv_obj_set_style_pad_row(hub, 8, 0);
    lv_obj_set_flex_flow(hub, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hub, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(hub, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *head = lv_obj_create(hub);
    lv_obj_set_size(head, lv_pct(100), 30);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *head_title = qz_text(head, "局域网设备", 14, qz_color(QZ_TEXT));
    lv_obj_align(head_title, LV_ALIGN_LEFT_MID, 2, 0);
    control_hub_chip = qz_text(head, "读取中", 11, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(control_hub_chip, LV_ALIGN_LEFT_MID, 88, 1);
    /* 两个次要动作：刷新状态；让网关重报一次设备树（刚配对完新设备时用） */
    lv_obj_t *discover = qz_icon_button(head, LV_SYMBOL_DOWNLOAD, 28);
    lv_obj_align(discover, LV_ALIGN_RIGHT_MID, -34, 0);
    lv_obj_add_event_cb(discover, hub_discover_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *refresh = qz_icon_button(head, LV_SYMBOL_REFRESH, 28);
    lv_obj_align(refresh, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(refresh, hub_refresh_clicked, LV_EVENT_CLICKED, NULL);

    control_hint = qz_text(hub, "", 10, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_set_width(control_hint, lv_pct(100));
    lv_label_set_long_mode(control_hint, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(control_hint, LV_OBJ_FLAG_HIDDEN);

    control_device_list = lv_obj_create(hub);
    lv_obj_set_size(control_device_list, lv_pct(100), 42);
    lv_obj_set_style_bg_opa(control_device_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(control_device_list, 0, 0);
    lv_obj_set_style_pad_all(control_device_list, 0, 0);
    lv_obj_clear_flag(control_device_list, LV_OBJ_FLAG_SCROLLABLE);

    /* 核心连不上时设备表必然是空的，把"重连"放在这里最顺手 */
    lv_obj_t *reconnect = lv_obj_create(hub);
    lv_obj_set_size(reconnect, lv_pct(100), 44);
    lv_obj_set_style_radius(reconnect, 12, 0);
    lv_obj_set_style_bg_color(reconnect, qz_color(QZ_FILL), 0);
    qz_obj_set_bg_color(reconnect, QZ_FILL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(reconnect, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(reconnect, 0, 0);
    lv_obj_set_style_pad_all(reconnect, 0, 0);
    lv_obj_add_flag(reconnect, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(reconnect, LV_OBJ_FLAG_SCROLLABLE);
    qz_add_press_feedback(reconnect);
    qz_add_touch_glint(reconnect);
    lv_obj_add_event_cb(reconnect, reconnect_core, LV_EVENT_CLICKED, NULL);
    /* A secondary action, so it takes the neutral tile and an accent glyph
     * rather than a second hue. */
    lv_obj_t *icon = qz_squircle(reconnect, 28, QZ_ACCENT_TINT);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_t *glyph = qz_symbol(icon, LV_SYMBOL_REFRESH, 14, qz_color(QZ_ACCENT_DARK));
    lv_obj_center(glyph);
    lv_obj_t *title = qz_text(reconnect, "重新连接核心", 13, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 44, 0);
    control_core_value = qz_text(reconnect, "点按执行", 11, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(control_core_value, LV_ALIGN_RIGHT_MID, -12, 0);

    lv_obj_add_event_cb(screen, control_screen_loaded, LV_EVENT_ALL, NULL);
}

static void build_face_screen(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    qz_style_screen(screen);
    screens[QZ_APPLET_FACE] = screen;
    make_toolbar(screen, "人脸识别");

    lv_obj_t *card = make_card(screen, 64, 174);
    lv_obj_t *icon = qz_squircle(card, 42, QZ_ACCENT_TINT);
    lv_obj_align(icon, LV_ALIGN_TOP_LEFT, CARD_PAD, 16);
    lv_obj_t *glyph = qz_symbol(icon, LV_SYMBOL_IMAGE, 19, qz_color(QZ_ACCENT_DARK));
    lv_obj_center(glyph);

    lv_obj_t *title = qz_text(card, "摄像头 + 人脸识别", 15, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, CARD_PAD + 54, 18);
    lv_obj_t *status = qz_text(card, qz_face_camera_status(), 12, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_set_width(status, CONTENT_W - CARD_PAD * 2 - 20);
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
    lv_obj_align(status, LV_ALIGN_TOP_LEFT, CARD_PAD + 54, 47);

    lv_obj_t *hint = qz_text(card,
        "设备版使用 RV1106 摄像头与 RKNN RetinaFace 后端；SDL 模拟器不访问摄像头。",
        11, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_set_width(hint, CONTENT_W - CARD_PAD * 2);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, CARD_PAD, 84);

    lv_obj_t *action = qz_button(card, qz_face_camera_available() ? "启动识别" : "模拟器不可用",
                                 CONTENT_W - CARD_PAD * 2, 38);
    lv_obj_align(action, LV_ALIGN_BOTTOM_MID, 0, -12);
    if (!qz_face_camera_available()) lv_obj_add_state(action, LV_STATE_DISABLED);
}

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
    build_face_screen();
    lv_timer_create(applet_tick, 1000, NULL);
}

lv_obj_t *qz_applets_screen(qz_applet_t id)
{
    return screens[id];
}
