#include "applets.h"
#include "theme.h"
#include "config.h"
#include "qzdesk_core.h"
#include "smarthome.h"
#include "web_client.h"
#include <stdio.h>
#include <stdlib.h>
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
static lv_obj_t *back_screen_ref;

static void set_page_button_enabled(lv_obj_t *button, bool enabled)
{
    if (!button) return;
    if (enabled) lv_obj_remove_state(button, LV_STATE_DISABLED);
    else lv_obj_add_state(button, LV_STATE_DISABLED);
}

static void go_back(lv_event_t *event)
{
    (void)event;
    qz_screen_load(back_screen_ref ? back_screen_ref : apps_screen_ref,
                   LV_SCR_LOAD_ANIM_MOVE_RIGHT, QZ_DUR_SCREEN);
}

static lv_obj_t *make_toolbar(lv_obj_t *screen, const char *title)
{
    lv_obj_t *header = qz_page_header(screen, title);
    lv_obj_t *back = qz_back_button(header);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_add_event_cb(back, go_back, LV_EVENT_CLICKED, NULL);
    return header;
}

static lv_obj_t *make_card(lv_obj_t *screen, int y, int height)
{
    lv_obj_t *card = lv_obj_create(screen);
    lv_obj_set_size(card, CONTENT_W, height);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, QZ_GUTTER, y);
    lv_obj_set_style_radius(card, QZ_RADIUS_CARD, 0);
    qz_style_plate(card);
    lv_obj_set_style_shadow_width(card, 0, 0);
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
    bool enabled;
    char label[80];
    char date[16];
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
static lv_obj_t *add_name_input;
static lv_obj_t *add_date_input;
static lv_obj_t *add_enabled_switch;
static int reminder_edit_id;
static reminder_t reminder_edit_value;
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

static void rebuild_reminders(void);
static bool reminders_loading;

static void reminders_loaded(bool ok, const char *body, void *data)
{
    (void)data;
    reminders_loading = false;
    if (!ok) { show_banner("提醒列表未同步，保留上次内容"); return; }
    int count = 0;

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
        r->enabled = true;
        qz_json_bool(object, "enabled", &r->enabled);
        qz_json_string(object, "date", r->date, sizeof(r->date));
        if (!qz_json_string(object, "label", r->label, sizeof(r->label)) ||
            r->label[0] == '\0') {
            snprintf(r->label, sizeof(r->label), "提醒");
        }
        count++;
    }
    reminder_count = count;
    /* 列表只在提醒页显示时才重建：定时器现在不管在哪个页面都会拉这份缓存
     * （主页的「接下来」卡要读它），翻了页重建只是白跑一趟。 */
    if (!add_overlay && lv_screen_active() == screens[QZ_APPLET_REMINDER])
        rebuild_reminders();
}

static bool reminders_reload(void)
{
    if (reminders_loading) return true;
    reminders_loading = qz_web_request_async("GET", "/api/timers", NULL, reminders_loaded, NULL);
    return reminders_loading;
}

/** 进提醒页时按缓存重画一次，再对一次核心：同步可能在别的页面上就完成了，
 *  那时列表按"只在提醒页重建"的规矩没有重建。 */
static void reminder_screen_loaded(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_SCREEN_LOADED) return;
    if (add_overlay) return;
    rebuild_reminders();
    reminders_reload();
}

static void remove_reminder(lv_event_t *event);
static void open_add_modal(lv_event_t *event);
static void edit_reminder(lv_event_t *event)
{
    if (add_overlay) return;
    reminder_edit_id = (int)(intptr_t)lv_event_get_user_data(event);
    for (int i = 0; i < reminder_count; i++) {
        if (reminders[i].id == reminder_edit_id) {
            reminder_edit_value = reminders[i];
            open_add_modal(event);
            return;
        }
    }
    reminder_edit_id = 0;
}

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
        lv_obj_add_flag(time_label, LV_OBJ_FLAG_CLICKABLE);
        /* 时间本身是编辑入口，字只有 38×19 实像素。标签按内容定宽，构造时
         * 还没有坐标，先让它落一次版再补命中区；上下左右都没有别的可点对象
         * （删除键在最右边），直接补到物理下限。 */
        lv_obj_update_layout(row);
        qz_touch_target(time_label);
        lv_obj_add_event_cb(time_label, edit_reminder, LV_EVENT_CLICKED,
                            (void *)(intptr_t)reminders[i].id);

        lv_obj_t *name = qz_text(row, reminders[i].label, 12, qz_color(QZ_TEXT_SECONDARY));
        lv_obj_set_width(name, CONTENT_W - 166);
        lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 98, reminders[i].daily ? -9 : 0);

        if (reminders[i].daily) {
            lv_obj_t *chip = qz_text(row, reminders[i].enabled ? "每日·点时间编辑" : "已关闭", 10,
                                      qz_color(QZ_ACCENT_TEXT));
            lv_obj_align(chip, LV_ALIGN_LEFT_MID, 98, 22);
        }

        lv_obj_t *remove = qz_icon_button(row, LV_SYMBOL_CLOSE, qz_compact() ? 40 : 28);
        lv_obj_align(remove, LV_ALIGN_RIGHT_MID, -12, 0);
        /* 删除按核心的 id，而不是这一行的下标：列表随时可能被语音改动过 */
        lv_obj_set_user_data(remove, (void *)(intptr_t)reminders[i].id);
        lv_obj_add_event_cb(remove, remove_reminder, LV_EVENT_CLICKED, NULL);
    }
}

static int delete_confirm_id;
static uint32_t delete_confirm_at;

static bool reminder_deleting;

static void reminder_deleted(bool ok, const char *body, void *data)
{
    (void)data;
    reminder_deleting = false;
    if (ok) { reminders_reload(); show_banner("提醒已删除"); }
    else {
        char error[96];
        if (!qz_json_string(body, "error", error, sizeof(error)))
            snprintf(error, sizeof(error), "删除失败，请重试");
        show_banner(error);
    }
}

static void remove_reminder(lv_event_t *event)
{
    if (reminder_deleting) return;
    int id = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(event));
    char path[48];
    if (id <= 0) return;
    if (delete_confirm_id != id || lv_tick_elaps(delete_confirm_at) > 5000) {
        delete_confirm_id = id;
        delete_confirm_at = lv_tick_get();
        show_banner("再点一次删除可确认（5秒内）");
        return;
    }
    delete_confirm_id = 0;
    snprintf(path, sizeof(path), "/api/timers/%d", id);
    reminder_deleting = true;
    show_banner("删除中，请稍候");
    if (!qz_web_request_async("DELETE", path, NULL, reminder_deleted, NULL)) {
        reminder_deleting = false;
        show_banner("无法发起删除，请重试");
    }
}

static bool reminder_saving;

static void close_add_modal(lv_event_t *event)
{
    (void)event;
    if (reminder_saving) return;
    if (add_overlay) lv_obj_delete(add_overlay);
    add_overlay = NULL;
    reminder_edit_id = 0;
}

static void add_modal_dismissed(lv_event_t *event)
{
    if (lv_event_get_target(event) != lv_event_get_current_target(event)) return;
    close_add_modal(event);
}

static void reminder_saved(bool ok, const char *response, void *user_data)
{
    (void)user_data;
    reminder_saving = false;
    if (!ok) {
        char error[112];
        if (!qz_json_string(response, "error", error, sizeof(error)))
            snprintf(error, sizeof(error), "保存失败，请重试；内容已保留");
        show_banner(error);
        return;
    }
    close_add_modal(NULL);
    if (reminders_reload()) rebuild_reminders();
    show_banner("提醒已保存");
}

static void add_confirm(lv_event_t *event)
{
    char body[768];
    char label[512];
    if (reminder_saving) return;
    char date[96];
    if (!qz_json_quote(lv_textarea_get_text(add_name_input), label, sizeof(label)) ||
        !qz_json_quote(lv_textarea_get_text(add_date_input), date, sizeof(date))) {
        show_banner("提醒内容过长，请修改后重试");
        return;
    }

    if (!add_hour_roller || !add_minute_roller) {
        close_add_modal(event);
        return;
    }
    /* 直接交给核心：语音、网页控制台与这里用的是同一份提醒表，
     * 重复或超上限时也由核心给出原因。 */
    snprintf(body, sizeof(body),
             "{\"hour\":%d,\"minute\":%d,\"daily\":%s,\"label\":%s,\"date\":%s,\"enabled\":%s}",
             lv_roller_get_selected(add_hour_roller),
             lv_roller_get_selected(add_minute_roller),
             lv_obj_has_state(add_daily_switch, LV_STATE_CHECKED) ? "true" : "false",
             label, date, lv_obj_has_state(add_enabled_switch, LV_STATE_CHECKED) ? "true" : "false");

    char path[64] = "/api/timers";
    if (reminder_edit_id) snprintf(path, sizeof(path), "/api/timers/%d", reminder_edit_id);
    reminder_saving = true;
    show_banner("保存中，请稍候");
    if (!qz_web_request_async(reminder_edit_id ? "PUT" : "POST", path, body,
                              reminder_saved, NULL)) {
        reminder_saving = false;
        show_banner("无法启动保存，请重试；内容已保留");
    }
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
    lv_obj_set_style_radius(roller, 3, LV_PART_SELECTED);
    qz_obj_set_text_color(roller, QZ_TEXT, LV_PART_SELECTED);
    lv_obj_set_style_text_font(roller, qz_font_size(17), LV_PART_SELECTED);
}

static void reminder_keyboard_close(lv_event_t *event)
{
    lv_obj_delete(lv_event_get_current_target(event));
}

static void reminder_input_focus(lv_event_t *event)
{
    if (!add_overlay) return;
    lv_obj_t *keyboard = lv_keyboard_create(add_overlay);
    lv_obj_set_size(keyboard, lv_pct(100), 136);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    qz_style_keyboard(keyboard);
    lv_keyboard_set_textarea(keyboard, lv_event_get_target(event));
    lv_obj_add_event_cb(keyboard, reminder_keyboard_close, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(keyboard, reminder_keyboard_close, LV_EVENT_CANCEL, NULL);
}

/** 开关行：整行可点，点文字也能切换。
 *
 *  弹层里一行只有 24 设计像素高，开关自己更小（紧凑面板上 28×18 实像素）。
 *  把说明文字和开关放进同一条整宽行，命中面就从一枚小圆点变成整行。 */
static void switch_row_clicked(lv_event_t *event)
{
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_user_data(event);
    if (!sw) return;
    if (lv_obj_has_state(sw, LV_STATE_CHECKED)) lv_obj_remove_state(sw, LV_STATE_CHECKED);
    else lv_obj_add_state(sw, LV_STATE_CHECKED);
}

static lv_obj_t *switch_row(lv_obj_t *parent, int y, int height, const char *caption,
                            int caption_px)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), height);
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *label = qz_text(row, caption, caption_px, qz_color(QZ_TEXT));
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);
    return row;
}

static void open_add_modal(lv_event_t *event)
{
    (void)event;
    if (add_overlay) return;
    lv_obj_t *screen = screens[QZ_APPLET_REMINDER];
    add_overlay = qz_overlay_create(screen);
    lv_obj_add_event_cb(add_overlay, add_modal_dismissed, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(add_overlay);
    lv_obj_set_size(panel, QZ_DESIGN_W - 40, 316);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 2);
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
    static char minutes[60 * 4];
    minutes[0] = '\0';
    for (int i = 0; i < 60; i++) {
        snprintf(minutes + strlen(minutes), 5, "%02d\n", i);
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
    lv_roller_set_selected(add_minute_roller, (uint32_t)local_now.tm_min, LV_ANIM_OFF);

    lv_obj_t *daily_row = switch_row(panel, 136, qz_compact() ? 32 : 26, "每日重复", 13);
    add_daily_switch = lv_switch_create(daily_row);
    qz_style_switch(add_daily_switch);
    lv_obj_set_size(add_daily_switch, qz_compact() ? 60 : 42, qz_compact() ? 32 : 26);
    lv_obj_align(add_daily_switch, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(daily_row, switch_row_clicked, LV_EVENT_CLICKED, add_daily_switch);

    add_name_input = lv_textarea_create(panel);
    lv_textarea_set_one_line(add_name_input, true);
    lv_obj_add_event_cb(add_name_input, reminder_input_focus, LV_EVENT_FOCUSED, NULL);
    lv_textarea_set_placeholder_text(add_name_input, "提醒内容");
    lv_obj_set_size(add_name_input, lv_pct(100), 32);
    lv_obj_set_style_text_font(add_name_input, qz_font_size(12), 0);
    lv_textarea_set_max_length(add_name_input, 24);
    lv_obj_align(add_name_input, LV_ALIGN_TOP_LEFT, 0, 168);
    lv_textarea_set_text(add_name_input, reminder_edit_id ? reminder_edit_value.label : "提醒");

    add_date_input = lv_textarea_create(panel);
    lv_textarea_set_one_line(add_date_input, true);
    lv_obj_add_event_cb(add_date_input, reminder_input_focus, LV_EVENT_FOCUSED, NULL);
    lv_textarea_set_placeholder_text(add_date_input, "日期 YYYY-MM-DD（可选）");
    lv_obj_set_size(add_date_input, lv_pct(100), 32);
    lv_obj_set_style_text_font(add_date_input, qz_font_size(12), 0);
    lv_textarea_set_max_length(add_date_input, 10);
    lv_obj_align(add_date_input, LV_ALIGN_TOP_LEFT, 0, 202);
    lv_textarea_set_text(add_date_input, reminder_edit_id ? reminder_edit_value.date : "");

    lv_obj_t *enabled_row = switch_row(panel, 234, 26, "启用提醒", 12);
    add_enabled_switch = lv_switch_create(enabled_row);
    qz_style_switch(add_enabled_switch);
    lv_obj_set_size(add_enabled_switch, 42, 24);
    lv_obj_align(add_enabled_switch, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(enabled_row, switch_row_clicked, LV_EVENT_CLICKED, add_enabled_switch);
    if (!reminder_edit_id || reminder_edit_value.enabled) lv_obj_add_state(add_enabled_switch, LV_STATE_CHECKED);

    if (reminder_edit_id) {
        lv_roller_set_selected(add_hour_roller, (uint32_t)reminder_edit_value.hour, LV_ANIM_OFF);
        lv_roller_set_selected(add_minute_roller, (uint32_t)reminder_edit_value.minute, LV_ANIM_OFF);
        if (reminder_edit_value.daily) lv_obj_add_state(add_daily_switch, LV_STATE_CHECKED);
    }

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
    /* 这排按钮的底板压在「启用提醒」那一行上：底板自己要透明可穿透，
     * 否则被盖住的下半行点不动。 */
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *cancel = qz_button(actions, "取消", 88, qz_compact() ? 44 : 32);
    lv_obj_add_event_cb(cancel, close_add_modal, LV_EVENT_CLICKED, NULL);
    lv_obj_t *confirm = lv_button_create(actions);
    lv_obj_set_size(confirm, 96, qz_compact() ? 44 : 32);
    qz_touch_target(confirm);
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
    /* 提醒列表定期与核心对齐：语音刚加的、或刚响过被撤掉的一次性提醒。
     * 主页的「接下来」卡读的是同一份缓存，所以不在提醒页也要继续拉。 */
    reminders_reload();
}

static void build_reminder_screen(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    qz_style_screen(screen);
    screens[QZ_APPLET_REMINDER] = screen;
    lv_obj_add_event_cb(screen, reminder_screen_loaded, LV_EVENT_SCREEN_LOADED, NULL);
    make_toolbar(screen, "定时提醒");

    reminder_list = make_card(screen, 56, 166);
    reminder_banner = make_card(screen, 56, 64);
    qz_obj_set_bg_color(reminder_banner, QZ_CARD, 0);
    lv_obj_set_style_bg_opa(reminder_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(reminder_banner, 0, 0);
    qz_material_register(reminder_banner, true);
    /* 提示条只是反馈，不能拦住下面的提醒行。删除确认需要第二次点到原删除键。 */
    lv_obj_clear_flag(reminder_banner, LV_OBJ_FLAG_CLICKABLE);
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
static lv_obj_t *pomo_workbench;
static lv_obj_t *pomo_clock;
static lv_obj_t *pomo_phase_chip;
static lv_obj_t *pomo_stage_dot;
static lv_obj_t *pomo_progress_caption;
static lv_obj_t *pomo_progress;
static lv_obj_t *pomo_round_track;
static lv_obj_t *pomo_count;
static lv_obj_t *pomo_end_caption;
static lv_obj_t *pomo_local_clock;
static lv_obj_t *pomo_start_button;
static lv_obj_t *pomo_start_caption;
static lv_obj_t *pomo_start_play;
static lv_obj_t *pomo_start_pause;
static lv_obj_t *pomo_stop_button;

static bool pomo_active;
static bool pomo_paused;
static bool pomo_break;
static int pomo_remaining;
static int pomo_phase_total = 25 * 60;
static int pomo_done;
static int pomo_cycles = 4;
static int pomo_next_focus = 25;
static int pomo_next_break = 5;
static int pomo_next_cycles = 4;
static bool pomo_next_auto_break = true;
static bool pomo_config_loaded;
static uintptr_t pomo_request_epoch = 1;
static int pomo_round_track_count;
static int pomo_visual_stage = -1;

static lv_obj_t *pomo_settings_overlay;
static lv_obj_t *pomo_settings_panel;
static lv_obj_t *pomo_edit_labels[3];
static lv_obj_t *pomo_preset_buttons[3];
static lv_obj_t *pomo_auto_switch;
static int pomo_edit_focus = 25;
static int pomo_edit_break = 5;
static int pomo_edit_cycles = 4;
static const int pomo_presets[][3] = {{25, 5, 4}, {45, 10, 2}, {15, 5, 4}};
static char pomo_notice[96];
static bool pomo_notice_error;

/** 番茄钟页每这么多拍（秒）与核心对一次表。 */
#define POMO_POLL_TICKS 5
#define POMO_PROGRESS_MS 380
#define POMO_SETTINGS_W 264
#define POMO_SETTINGS_H 304
#define POMO_MAX_MINUTES 180
#define POMO_MAX_CYCLES 12

static void pomo_refresh(void);

/** 把核心返回的番茄钟状态读进界面变量。 */
static bool pomo_sync(const char *json)
{
    const char *state;
    char phase[16] = "";
    bool flag = false;
    int value = 0;
    int focus_minutes = 25;
    int break_minutes = 5;
    int total_cycles = 4;
    bool auto_break = true;

    if (!json) return false;
    /* /api/pomodoro 返回 {"ok":true,"state":{...},"message":"..."}。
     * 旧核心曾经把字段直接平铺在顶层，这里继续兼容。 */
    state = qz_json_value(json, "state");
    if (state && *state == '{') json = state;

    if (!qz_json_bool(json, "active", &flag)) return false;
    pomo_active = flag;
    pomo_paused = false;
    qz_json_bool(json, "paused", &pomo_paused);
    if (qz_json_string(json, "phase", phase, sizeof(phase))) {
        pomo_break = strcmp(phase, "break") == 0;
    }
    if (qz_json_int(json, "remaining_seconds", &value)) pomo_remaining = value < 0 ? 0 : value;
    if (qz_json_int(json, "completed_cycles", &value)) pomo_done = value < 0 ? 0 : value;
    if (qz_json_int(json, "total_cycles", &value) && value > 0) total_cycles = value;
    qz_json_int(json, "focus_minutes", &focus_minutes);
    qz_json_int(json, "break_minutes", &break_minutes);
    qz_json_bool(json, "auto_break", &auto_break);
    if (total_cycles < 1) total_cycles = 1;
    if (total_cycles > POMO_MAX_CYCLES) total_cycles = POMO_MAX_CYCLES;
    if (focus_minutes < 1) focus_minutes = 1;
    if (focus_minutes > POMO_MAX_MINUTES) focus_minutes = POMO_MAX_MINUTES;
    if (break_minutes < 1) break_minutes = 1;
    if (break_minutes > POMO_MAX_MINUTES) break_minutes = POMO_MAX_MINUTES;
    pomo_cycles = total_cycles;
    if (pomo_done > pomo_cycles) pomo_done = pomo_cycles;

    if (!pomo_config_loaded) {
        pomo_next_focus = focus_minutes;
        pomo_next_break = break_minutes;
        pomo_next_cycles = total_cycles;
        pomo_next_auto_break = auto_break;
        pomo_config_loaded = true;
    }

    pomo_phase_total = (pomo_break ? break_minutes : focus_minutes) * 60;
    if (pomo_phase_total <= 0) pomo_phase_total = 60;
    if (pomo_remaining > pomo_phase_total) pomo_remaining = pomo_phase_total;
    return true;
}

static bool pomo_loading;

static void pomo_set_notice(const char *text, bool error)
{
    snprintf(pomo_notice, sizeof(pomo_notice), "%s", text ? text : "");
    pomo_notice_error = error;
}

static void pomo_loaded(bool ok, const char *body, void *data)
{
    pomo_loading = false;
    if ((uintptr_t)data != pomo_request_epoch) return;
    if (ok && pomo_sync(body)) {
        pomo_refresh();
    } else {
        pomo_set_notice("未同步，请检查服务", true);
        pomo_refresh();
    }
}

static bool pomo_reload(void)
{
    if (pomo_loading) return true;
    pomo_loading = qz_web_request_async("GET", "/api/pomodoro", NULL, pomo_loaded,
                                        (void *)pomo_request_epoch);
    return pomo_loading;
}

/** 给核心发一个动作，成功后用核心返回的状态刷新界面。 */
static bool pomo_busy;

static void pomo_request_done(bool ok, const char *body, void *user_data)
{
    (void)user_data;
    pomo_busy = false;
    if (!ok) {
        char error[96];
        if (!qz_json_string(body, "error", error, sizeof(error)))
            snprintf(error, sizeof(error), "操作失败，请重试");
        pomo_set_notice(error, true);
        pomo_refresh();
        return;
    }
    if (pomo_sync(body)) {
        pomo_set_notice(NULL, false);
        pomo_refresh();
    } else {
        pomo_set_notice("状态解析失败，请重试", true);
        pomo_refresh();
    }
}

static void pomo_send(const char *action)
{
    if (pomo_busy) return;
    char payload[160];
    pomo_request_epoch++;

    if (strcmp(action, "start") == 0) {
        snprintf(payload, sizeof(payload),
            "{\"action\":\"start\",\"focus_minutes\":%d,\"break_minutes\":%d,\"cycles\":%d,\"auto_break\":%s}",
            pomo_next_focus, pomo_next_break, pomo_next_cycles, pomo_next_auto_break ? "true" : "false");
    } else snprintf(payload, sizeof(payload), "{\"action\":\"%s\"}", action);
    pomo_busy = true;
    pomo_set_notice("正在同步…", false);
    pomo_refresh();
    if (!qz_web_request_async("POST", "/api/pomodoro", payload, pomo_request_done, NULL)) {
        pomo_busy = false;
        pomo_set_notice("无法发起操作，请重试", true);
        pomo_refresh();
    }
}

static void pomo_progress_anim(void *var, int32_t value)
{
    lv_bar_set_value((lv_obj_t *)var, value, LV_ANIM_OFF);
}

static void pomo_progress_set(int value)
{
    if (!pomo_progress) return;
    lv_anim_delete(pomo_progress, pomo_progress_anim);
    if (qz_reduce_motion() || lv_bar_get_value(pomo_progress) == value) {
        lv_bar_set_value(pomo_progress, value, LV_ANIM_OFF);
        return;
    }
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, pomo_progress);
    lv_anim_set_exec_cb(&anim, pomo_progress_anim);
    lv_anim_set_values(&anim, lv_bar_get_value(pomo_progress), value);
    lv_anim_set_duration(&anim, POMO_PROGRESS_MS);
    qz_anim_ease_out(&anim);
    lv_anim_start(&anim);
}

static void pomo_round_track_build(void)
{
    if (!pomo_round_track) return;
    int cycles = pomo_cycles < 1 ? 1
               : pomo_cycles > POMO_MAX_CYCLES ? POMO_MAX_CYCLES : pomo_cycles;
    int gap = cycles > 8 ? 2 : 3;
    int32_t track_w = qz_scale_x(208);
    int32_t track_h = qz_scale_y(8);
    int32_t gap_px = qz_scale_x(gap);
    int32_t bar_w = (track_w - (cycles - 1) * gap_px) / cycles;
    int32_t bar_h = qz_scale_y(6);
    if (bar_w < 3) bar_w = 3;
    if (bar_h < 1) bar_h = 1;
    int32_t used = cycles * bar_w + (cycles - 1) * gap_px;
    int32_t start = (track_w - used) / 2;

    lv_obj_clean(pomo_round_track);
    for (int i = 0; i < cycles; i++) {
        lv_obj_t *bar = lv_obj_create(pomo_round_track);
        /* Position the bars in physical pixels: design-unit rounding can push
         * the last bar one pixel outside a narrow round track. */
        (lv_obj_set_size)(bar, bar_w, bar_h);
        (lv_obj_set_pos)(bar, start + i * (bar_w + gap_px), (track_h - bar_h) / 2);
        lv_obj_set_style_radius(bar, 1, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        qz_obj_set_bg_color(bar, QZ_FILL, 0);
    }
    pomo_round_track_count = cycles;
}

static void pomo_round_track_refresh(void)
{
    if (!pomo_round_track) return;
    if (pomo_round_track_count != pomo_cycles) pomo_round_track_build();
    uint32_t child_count = lv_obj_get_child_count(pomo_round_track);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t *bar = lv_obj_get_child(pomo_round_track, i);
        qz_color_token_t color = QZ_FILL;
        if ((int)i < pomo_done) color = QZ_ACCENT;
        else if (pomo_active && (int)i == pomo_done)
            color = pomo_break ? QZ_GREEN : QZ_ACCENT_DARK;
        qz_obj_set_bg_color(bar, color, 0);
    }
}

static void pomo_footer_refresh(void)
{
    if (!pomo_end_caption) return;
    if (pomo_notice[0]) {
        lv_label_set_text(pomo_end_caption, pomo_notice);
        qz_obj_set_text_color(pomo_end_caption,
                              pomo_notice_error ? QZ_RED : QZ_TEXT_SECONDARY, 0);
        return;
    }

    char text[64];
    if (!pomo_active) {
        snprintf(text, sizeof(text), "开始后计算结束时刻");
    } else if (pomo_paused) {
        snprintf(text, sizeof(text), pomo_break ? "继续后开始休息" : "继续后计算结束时刻");
    } else if (pomo_remaining <= 0) {
        snprintf(text, sizeof(text), "正在切换阶段");
    } else {
        time_t end = time(NULL) + pomo_remaining;
        struct tm local;
        localtime_r(&end, &local);
        snprintf(text, sizeof(text), "预计 %02d:%02d 结束", local.tm_hour, local.tm_min);
    }
    lv_label_set_text(pomo_end_caption, text);
    qz_obj_set_text_color(pomo_end_caption, QZ_TEXT_SECONDARY, 0);
}

static void pomo_local_clock_refresh(void)
{
    if (!pomo_local_clock) return;
    time_t now = time(NULL);
    struct tm local;
    char text[24];
    localtime_r(&now, &local);
    snprintf(text, sizeof(text), "本地 %02d:%02d", local.tm_hour, local.tm_min);
    lv_label_set_text(pomo_local_clock, text);
}

static void pomo_busy_refresh(void)
{
    if (pomo_start_button) {
        if (pomo_busy) lv_obj_add_state(pomo_start_button, LV_STATE_DISABLED);
        else lv_obj_remove_state(pomo_start_button, LV_STATE_DISABLED);
    }
    if (pomo_stop_button) {
        if (pomo_busy || !pomo_active) lv_obj_add_state(pomo_stop_button, LV_STATE_DISABLED);
        else lv_obj_remove_state(pomo_stop_button, LV_STATE_DISABLED);
    }
}

static void pomo_refresh(void)
{
    if (!pomo_clock || !pomo_phase_chip || !pomo_progress) return;

    int total = pomo_active ? (pomo_phase_total > 0 ? pomo_phase_total : 60)
                            : pomo_next_focus * 60;
    if (total < 1) total = 60;
    int shown = pomo_active ? pomo_remaining : pomo_next_focus * 60;
    int progress = pomo_active ? total - pomo_remaining : 0;
    if (progress < 0) progress = 0;
    if (progress > total) progress = total;
    lv_bar_set_range(pomo_progress, 0, total);
    pomo_progress_set(progress);
    qz_obj_set_bg_color(pomo_progress, pomo_break && pomo_active ? QZ_GREEN : QZ_ACCENT,
                        LV_PART_INDICATOR);
    if (pomo_progress_caption) {
        lv_label_set_text(pomo_progress_caption,
                          pomo_break && pomo_active ? "休息进度" : "专注进度");
        qz_obj_set_text_color(pomo_progress_caption,
                              pomo_break && pomo_active ? QZ_GREEN : QZ_ACCENT_TEXT, 0);
    }

    char clock[16];
    snprintf(clock, sizeof(clock), "%02d:%02d", shown / 60, shown % 60);
    lv_label_set_text(pomo_clock, clock);
    if (!pomo_active) {
        lv_label_set_text(pomo_phase_chip, "准备专注");
        qz_obj_set_text_color(pomo_phase_chip, QZ_TEXT_TERTIARY, 0);
    } else if (pomo_paused) {
        lv_label_set_text(pomo_phase_chip, pomo_break ? "休息已暂停" : "专注已暂停");
        qz_obj_set_text_color(pomo_phase_chip, QZ_TEXT_SECONDARY, 0);
    } else if (pomo_break) {
        lv_label_set_text(pomo_phase_chip, "休息");
        qz_obj_set_text_color(pomo_phase_chip, QZ_GREEN, 0);
    } else {
        lv_label_set_text(pomo_phase_chip, "专注");
        qz_obj_set_text_color(pomo_phase_chip, QZ_ACCENT_TEXT, 0);
    }
    if (pomo_stage_dot) {
        qz_color_token_t dot = !pomo_active ? QZ_TEXT_TERTIARY
                             : pomo_paused ? QZ_ORANGE
                             : pomo_break ? QZ_GREEN : QZ_ACCENT;
        qz_obj_set_bg_color(pomo_stage_dot, dot, 0);
    }

    pomo_round_track_refresh();
    char count[40];
    if (pomo_active && !pomo_break)
        snprintf(count, sizeof(count), "第 %d / %d 轮", pomo_done + 1, pomo_cycles);
    else
        snprintf(count, sizeof(count), "已完成 %d / %d 轮", pomo_done, pomo_cycles);
    lv_label_set_text(pomo_count, count);

    if (pomo_start_caption) {
        const char *caption = "暂停";
        if (!pomo_active) caption = "开始专注";
        else if (pomo_paused) caption = pomo_break ? "继续休息" : "继续专注";
        lv_label_set_text(pomo_start_caption, caption);
    }
    if (pomo_start_play && pomo_start_pause) {
        bool running = pomo_active && !pomo_paused;
        if (running) lv_obj_add_flag(pomo_start_play, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(pomo_start_play, LV_OBJ_FLAG_HIDDEN);
        if (running) lv_obj_clear_flag(pomo_start_pause, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(pomo_start_pause, LV_OBJ_FLAG_HIDDEN);
    }

    int stage = pomo_active ? (pomo_break ? 2 : 1) : 0;
    if (pomo_visual_stage >= 0 && stage != pomo_visual_stage) {
        if (pomo_clock) qz_animate_pop_in(pomo_clock);
        if (pomo_phase_chip) qz_animate_pop_in(pomo_phase_chip);
    }
    pomo_visual_stage = stage;

    pomo_local_clock_refresh();
    pomo_footer_refresh();
    pomo_busy_refresh();
}

/** 每拍走一秒；换阶段与结束由核心判定，这里只让秒数看起来是连续走的。 */
static void pomo_tick(void)
{
    static int since_sync;

    if (++since_sync >= POMO_POLL_TICKS) {
        since_sync = 0;
        pomo_reload();
    }
    if (pomo_active && !pomo_paused && pomo_remaining > 0) {
        pomo_remaining--;
        if (pomo_remaining == 0) pomo_reload();
    }
    if (lv_screen_active() == screens[QZ_APPLET_POMODORO]) pomo_refresh();
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

static void pomo_settings_render(void)
{
    char text[16];
    int values[3] = {pomo_edit_focus, pomo_edit_break, pomo_edit_cycles};
    for (int i = 0; i < 3; i++) {
        if (!pomo_edit_labels[i]) continue;
        snprintf(text, sizeof(text), "%d", values[i]);
        lv_label_set_text(pomo_edit_labels[i], text);
    }

    for (int i = 0; i < 3; i++) {
        lv_obj_t *button = pomo_preset_buttons[i];
        if (!button) continue;
        bool selected = values[0] == pomo_presets[i][0] &&
                        values[1] == pomo_presets[i][1] &&
                        values[2] == pomo_presets[i][2];
        qz_obj_set_bg_color(button, selected ? QZ_ACCENT_TINT : QZ_FILL, 0);
        qz_obj_set_border_color(button, QZ_ACCENT, 0);
        lv_obj_set_style_border_width(button, selected ? 1 : 0, 0);
        lv_obj_t *label = lv_obj_get_child(button, 0);
        if (label) qz_obj_set_text_color(label, selected ? QZ_ACCENT_TEXT : QZ_TEXT, 0);
    }
}

static void pomo_step_changed(lv_event_t *event)
{
    intptr_t code = (intptr_t)lv_event_get_user_data(event);
    int index = (int)(code / 2);
    int delta = (code % 2) ? 1 : -1;
    int *values[3] = {&pomo_edit_focus, &pomo_edit_break, &pomo_edit_cycles};
    int limits[3] = {POMO_MAX_MINUTES, POMO_MAX_MINUTES, POMO_MAX_CYCLES};
    if (index < 0 || index > 2) return;
    *values[index] += delta;
    if (*values[index] < 1) *values[index] = 1;
    if (*values[index] > limits[index]) *values[index] = limits[index];
    pomo_settings_render();
}

static void pomo_preset_chosen(lv_event_t *event)
{
    intptr_t index = (intptr_t)lv_event_get_user_data(event);
    if (index < 0 || index > 2) return;
    pomo_edit_focus = pomo_presets[index][0];
    pomo_edit_break = pomo_presets[index][1];
    pomo_edit_cycles = pomo_presets[index][2];
    pomo_settings_render();
}

static void pomo_settings_delete_ready(lv_anim_t *anim)
{
    lv_obj_t *overlay = (lv_obj_t *)anim->var;
    if (overlay) lv_obj_delete(overlay);
}

static void pomo_settings_translate_x(void *var, int32_t value)
{
    /* Animation values are already physical pixels; bypass the design-unit
     * scaling macro so close/open start from the exact current offset. */
    (lv_obj_set_style_translate_x)((lv_obj_t *)var, value, 0);
}

static void pomo_scrim_opa(void *var, int32_t value)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

static void pomo_settings_close(lv_event_t *event)
{
    (void)event;
    if (!pomo_settings_overlay) return;
    lv_obj_t *overlay = pomo_settings_overlay;
    lv_obj_t *panel = lv_obj_get_child(overlay, 0);
    pomo_settings_overlay = NULL;
    pomo_settings_panel = NULL;

    if (qz_reduce_motion() || !panel) {
        lv_obj_delete(overlay);
        return;
    }

    lv_anim_delete(panel, pomo_settings_translate_x);
    lv_anim_t slide;
    lv_anim_init(&slide);
    lv_anim_set_var(&slide, panel);
    lv_anim_set_exec_cb(&slide, pomo_settings_translate_x);
    lv_anim_set_values(&slide, lv_obj_get_style_translate_x(panel, 0),
                       qz_scale_x(POMO_SETTINGS_W + 16));
    lv_anim_set_duration(&slide, QZ_DUR_PANEL);
    qz_anim_ease_out(&slide);
    lv_anim_start(&slide);

    lv_anim_t scrim;
    lv_anim_init(&scrim);
    lv_anim_set_var(&scrim, overlay);
    lv_anim_set_exec_cb(&scrim, pomo_scrim_opa);
    lv_anim_set_values(&scrim, lv_obj_get_style_bg_opa(overlay, 0), LV_OPA_TRANSP);
    lv_anim_set_duration(&scrim, QZ_DUR_PANEL);
    qz_anim_ease_out(&scrim);
    lv_anim_set_completed_cb(&scrim, pomo_settings_delete_ready);
    lv_anim_start(&scrim);
}

static void pomo_settings_dismissed(lv_event_t *event)
{
    if (lv_event_get_target(event) != lv_event_get_current_target(event)) return;
    pomo_settings_close(event);
}

static void pomo_settings_save(lv_event_t *event)
{
    (void)event;
    pomo_request_epoch++;
    pomo_next_focus = pomo_edit_focus;
    pomo_next_break = pomo_edit_break;
    pomo_next_cycles = pomo_edit_cycles;
    pomo_next_auto_break = lv_obj_has_state(pomo_auto_switch, LV_STATE_CHECKED);
    /* An initial GET may still be in flight when the user saves. Treat this
     * explicit edit as the newer configuration so the late response cannot
     * overwrite it with the values read before the save. */
    pomo_config_loaded = true;
    pomo_set_notice(pomo_active ? "已保存，将在下一次开始时生效" : "节律已更新", false);
    pomo_settings_close(NULL);
    pomo_refresh();
}

static void pomo_settings_stepper(lv_obj_t *panel, int y, const char *title, int index)
{
    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_set_size(row, POMO_SETTINGS_W - 32, 42);
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, 16, y);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = qz_text(row, title, 12, qz_color(QZ_TEXT));
    lv_obj_set_width(label, 108);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *minus = qz_icon_button(row, LV_SYMBOL_MINUS, 30);
    lv_obj_align(minus, LV_ALIGN_RIGHT_MID, -86, 0);
    lv_obj_add_event_cb(minus, pomo_step_changed, LV_EVENT_CLICKED,
                        (void *)(intptr_t)(index * 2));

    pomo_edit_labels[index] = qz_text(row, "0", 17, qz_color(QZ_TEXT));
    lv_obj_set_width(pomo_edit_labels[index], 44);
    lv_obj_set_style_text_align(pomo_edit_labels[index], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(pomo_edit_labels[index], LV_ALIGN_RIGHT_MID, -36, 0);

    lv_obj_t *plus = qz_icon_button(row, LV_SYMBOL_PLUS, 30);
    lv_obj_align(plus, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(plus, pomo_step_changed, LV_EVENT_CLICKED,
                        (void *)(intptr_t)(index * 2 + 1));
}

static void pomo_settings_build(void)
{
    pomo_settings_panel = lv_obj_create(pomo_settings_overlay);
    lv_obj_set_size(pomo_settings_panel, POMO_SETTINGS_W, POMO_SETTINGS_H);
    lv_obj_align(pomo_settings_panel, LV_ALIGN_TOP_RIGHT, -8, 8);
    qz_style_sheet(pomo_settings_panel);
    lv_obj_set_style_pad_all(pomo_settings_panel, 0, 0);
    lv_obj_add_flag(pomo_settings_panel, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = qz_text(pomo_settings_panel, "节律编辑", 18, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 8);
    lv_obj_t *subtitle = qz_text(pomo_settings_panel, "仅影响下一次开始", 10,
                                 qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(subtitle, LV_ALIGN_TOP_LEFT, 16, 31);

    lv_obj_t *close = qz_icon_button(pomo_settings_panel, LV_SYMBOL_CLOSE, 30);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, -10, 7);
    lv_obj_add_event_cb(close, pomo_settings_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t *sep = qz_separator(pomo_settings_panel, POMO_SETTINGS_W - 32, false);
    lv_obj_align(sep, LV_ALIGN_TOP_LEFT, 16, 49);

    const char *titles[3] = {"专注分钟", "休息分钟", "轮次"};
    for (int i = 0; i < 3; i++) {
        pomo_settings_stepper(pomo_settings_panel, 55 + i * 44, titles[i], i);
        if (i < 2) {
            lv_obj_t *line = qz_separator(pomo_settings_panel, POMO_SETTINGS_W - 32, false);
            lv_obj_align(line, LV_ALIGN_TOP_LEFT, 16, 97 + i * 44);
        }
    }

    lv_obj_t *preset_title = qz_text(pomo_settings_panel, "快速预设", 10,
                                     qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(preset_title, LV_ALIGN_TOP_LEFT, 16, 186);
    for (int i = 0; i < 3; i++) {
        char caption[16];
        snprintf(caption, sizeof(caption), "%d/%d/%d", pomo_presets[i][0],
                 pomo_presets[i][1], pomo_presets[i][2]);
        lv_obj_t *button = qz_button(pomo_settings_panel, caption, 70, 30);
        lv_obj_align(button, LV_ALIGN_TOP_LEFT, 16 + i * 78, 201);
        lv_obj_add_event_cb(button, pomo_preset_chosen, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        pomo_preset_buttons[i] = button;
    }

    lv_obj_t *auto_label = qz_text(pomo_settings_panel, "自动开始休息", 11,
                                   qz_color(QZ_TEXT));
    lv_obj_align(auto_label, LV_ALIGN_TOP_LEFT, 16, 239);
    pomo_auto_switch = lv_switch_create(pomo_settings_panel);
    lv_obj_set_size(pomo_auto_switch, 42, 24);
    qz_style_switch(pomo_auto_switch);
    qz_touch_target(pomo_auto_switch);
    lv_obj_align(pomo_auto_switch, LV_ALIGN_TOP_RIGHT, -16, 233);
    if (pomo_next_auto_break)
        lv_obj_add_state(pomo_auto_switch, LV_STATE_CHECKED);

    lv_obj_t *save = qz_button(pomo_settings_panel, "保存", 130, 35);
    lv_obj_align(save, LV_ALIGN_BOTTOM_LEFT, 16, -8);
    qz_style_primary_button(save);
    lv_obj_t *save_caption = lv_obj_get_child(save, 0);
    if (save_caption) qz_obj_set_text_color(save_caption, QZ_TEXT_ON_ACCENT, 0);
    lv_obj_add_event_cb(save, pomo_settings_save, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel = qz_button(pomo_settings_panel, "取消", 86, 35);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_RIGHT, -16, -8);
    lv_obj_add_event_cb(cancel, pomo_settings_close, LV_EVENT_CLICKED, NULL);

    pomo_settings_render();
}

static void pomo_settings_open(lv_event_t *event)
{
    (void)event;
    if (pomo_settings_overlay || !screens[QZ_APPLET_POMODORO]) return;
    pomo_edit_focus = pomo_next_focus;
    pomo_edit_break = pomo_next_break;
    pomo_edit_cycles = pomo_next_cycles;
    memset(pomo_edit_labels, 0, sizeof(pomo_edit_labels));
    memset(pomo_preset_buttons, 0, sizeof(pomo_preset_buttons));
    pomo_auto_switch = NULL;

    pomo_settings_overlay = qz_overlay_create(screens[QZ_APPLET_POMODORO]);
    lv_obj_add_event_cb(pomo_settings_overlay, pomo_settings_dismissed,
                        LV_EVENT_CLICKED, NULL);
    pomo_settings_build();

    if (!qz_reduce_motion()) {
        (lv_obj_set_style_translate_x)(pomo_settings_panel,
                                       qz_scale_x(POMO_SETTINGS_W + 16), 0);
        lv_anim_t slide;
        lv_anim_init(&slide);
        lv_anim_set_var(&slide, pomo_settings_panel);
        lv_anim_set_exec_cb(&slide, pomo_settings_translate_x);
        lv_anim_set_values(&slide, qz_scale_x(POMO_SETTINGS_W + 16), 0);
        lv_anim_set_duration(&slide, QZ_DUR_PANEL);
        qz_anim_ease_out(&slide);
        lv_anim_start(&slide);

        lv_obj_set_style_bg_opa(pomo_settings_overlay, LV_OPA_TRANSP, 0);
        lv_anim_t scrim;
        lv_anim_init(&scrim);
        lv_anim_set_var(&scrim, pomo_settings_overlay);
        lv_anim_set_exec_cb(&scrim, pomo_scrim_opa);
        lv_anim_set_values(&scrim, LV_OPA_TRANSP, 110);
        lv_anim_set_duration(&scrim, QZ_DUR_PANEL);
        qz_anim_ease_out(&scrim);
        lv_anim_start(&scrim);
    }
}

static void pomo_screen_loaded(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_SCREEN_LOADED || qz_reduce_motion()) return;
    qz_animate_entrance(pomo_workbench, 0);
    if (pomo_end_caption) qz_animate_entrance(pomo_end_caption, 35);
    if (pomo_start_button) qz_animate_entrance(pomo_start_button, 65);
    if (pomo_stop_button) qz_animate_entrance(pomo_stop_button, 85);
}

static void build_pomodoro_screen(void)
{
    lv_obj_t *screen = lv_obj_create(NULL);
    qz_style_screen(screen);
    screens[QZ_APPLET_POMODORO] = screen;
    lv_obj_t *header = make_toolbar(screen, "番茄钟");

    lv_obj_t *settings = qz_icon_button(header, LV_SYMBOL_SETTINGS, 32);
    lv_obj_align(settings, LV_ALIGN_RIGHT_MID, -9, 0);
    lv_obj_add_event_cb(settings, pomo_settings_open, LV_EVENT_CLICKED, NULL);

    pomo_workbench = make_card(screen, 58, 166);

    lv_obj_t *rail = lv_obj_create(pomo_workbench);
    lv_obj_set_size(rail, 3, 138);
    lv_obj_align(rail, LV_ALIGN_LEFT_MID, 0, 0);
    qz_obj_set_bg_color(rail, QZ_ACCENT, 0);
    lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rail, 0, 0);
    lv_obj_set_style_radius(rail, 0, 0);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_SCROLLABLE);

    pomo_stage_dot = lv_obj_create(pomo_workbench);
    lv_obj_set_size(pomo_stage_dot, 7, 7);
    lv_obj_align(pomo_stage_dot, LV_ALIGN_TOP_LEFT, 16, 17);
    qz_obj_set_bg_color(pomo_stage_dot, QZ_TEXT_TERTIARY, 0);
    lv_obj_set_style_bg_opa(pomo_stage_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pomo_stage_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(pomo_stage_dot, 0, 0);
    lv_obj_set_style_pad_all(pomo_stage_dot, 0, 0);
    lv_obj_clear_flag(pomo_stage_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(pomo_stage_dot, LV_OBJ_FLAG_SCROLLABLE);

    pomo_phase_chip = qz_text(pomo_workbench, "准备专注", 12,
                              qz_color(QZ_TEXT_TERTIARY));
    lv_obj_set_width(pomo_phase_chip, 164);
    lv_obj_set_height(pomo_phase_chip, 20);
    lv_label_set_long_mode(pomo_phase_chip, LV_LABEL_LONG_CLIP);
    lv_obj_align(pomo_phase_chip, LV_ALIGN_TOP_LEFT, 30, 11);

    pomo_clock = qz_text(pomo_workbench, "25:00", 49, qz_color(QZ_TEXT));
    lv_obj_set_width(pomo_clock, 190);
    lv_obj_set_height(pomo_clock, 66);
    lv_obj_align(pomo_clock, LV_ALIGN_TOP_LEFT, 15, 34);

    pomo_progress_caption = qz_text(pomo_workbench, "专注进度", 10,
                                    qz_color(QZ_ACCENT_TEXT));
    lv_obj_set_width(pomo_progress_caption, 190);
    lv_obj_set_height(pomo_progress_caption, 20);
    lv_label_set_long_mode(pomo_progress_caption, LV_LABEL_LONG_CLIP);
    lv_obj_align(pomo_progress_caption, LV_ALIGN_TOP_LEFT, 16, 110);

    pomo_progress = lv_bar_create(pomo_workbench);
    lv_obj_set_size(pomo_progress, 190, 8);
    lv_obj_align(pomo_progress, LV_ALIGN_TOP_LEFT, 16, 132);
    lv_bar_set_range(pomo_progress, 0, pomo_phase_total);
    lv_bar_set_value(pomo_progress, 0, LV_ANIM_OFF);
    qz_obj_set_bg_color(pomo_progress, QZ_FILL, LV_PART_MAIN);
    qz_obj_set_bg_color(pomo_progress, QZ_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(pomo_progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pomo_progress, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(pomo_progress, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(pomo_progress, 3, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(pomo_progress, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(pomo_progress, 0, 0);
    lv_obj_clear_flag(pomo_progress, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(pomo_progress, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *split = qz_separator(pomo_workbench, 136, true);
    lv_obj_align(split, LV_ALIGN_TOP_LEFT, 218, 15);

    lv_obj_t *round_caption = qz_text(pomo_workbench, "轮次轨道", 10,
                                      qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(round_caption, LV_ALIGN_TOP_LEFT, 234, 12);

    pomo_count = qz_text(pomo_workbench, "已完成 0 / 4 轮", 15, qz_color(QZ_TEXT));
    lv_obj_set_width(pomo_count, 200);
    lv_obj_set_height(pomo_count, 22);
    lv_label_set_long_mode(pomo_count, LV_LABEL_LONG_DOT);
    lv_obj_align(pomo_count, LV_ALIGN_TOP_LEFT, 234, 32);

    pomo_round_track = lv_obj_create(pomo_workbench);
    lv_obj_set_size(pomo_round_track, 208, 8);
    lv_obj_align(pomo_round_track, LV_ALIGN_TOP_LEFT, 234, 62);
    lv_obj_set_style_bg_opa(pomo_round_track, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pomo_round_track, 0, 0);
    lv_obj_set_style_pad_all(pomo_round_track, 0, 0);
    lv_obj_clear_flag(pomo_round_track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(pomo_round_track, LV_OBJ_FLAG_CLICKABLE);
    pomo_round_track_build();

    lv_obj_t *round_rule = qz_separator(pomo_workbench, 200, false);
    lv_obj_align(round_rule, LV_ALIGN_TOP_LEFT, 234, 82);

    lv_obj_t *local_caption = qz_text(pomo_workbench, "当前时刻", 10,
                                      qz_color(QZ_TEXT_TERTIARY));
    lv_obj_set_width(local_caption, 200);
    lv_obj_set_height(local_caption, 20);
    lv_label_set_long_mode(local_caption, LV_LABEL_LONG_CLIP);
    lv_obj_align(local_caption, LV_ALIGN_TOP_LEFT, 234, 94);

    pomo_local_clock = qz_text(pomo_workbench, "本地 --:--", 23,
                               qz_color(QZ_TEXT));
    lv_obj_set_width(pomo_local_clock, 200);
    lv_obj_align(pomo_local_clock, LV_ALIGN_TOP_LEFT, 234, 114);

    lv_obj_t *local_hint = qz_text(pomo_workbench, "暂停时计时停在当前秒数", 10,
                                   qz_color(QZ_TEXT_TERTIARY));
    lv_obj_set_width(local_hint, 200);
    lv_label_set_long_mode(local_hint, LV_LABEL_LONG_DOT);
    lv_obj_align(local_hint, LV_ALIGN_TOP_LEFT, 234, qz_compact() ? 142 : 150);

    pomo_end_caption = qz_text(screen, "开始后计算结束时刻", 11,
                               qz_color(QZ_TEXT_SECONDARY));
    lv_obj_set_width(pomo_end_caption, CONTENT_W);
    lv_obj_set_height(pomo_end_caption, 18);
    lv_obj_set_style_text_align(pomo_end_caption, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(pomo_end_caption, LV_LABEL_LONG_DOT);
    lv_obj_align(pomo_end_caption, LV_ALIGN_TOP_MID, 0, 236);

    pomo_start_button = lv_button_create(screen);
    lv_obj_set_size(pomo_start_button, 312, 40);
    qz_touch_target(pomo_start_button);
    lv_obj_align(pomo_start_button, LV_ALIGN_BOTTOM_LEFT, QZ_GUTTER, -6);
    qz_style_primary_button(pomo_start_button);
    pomo_start_play = qz_icon_image(pomo_start_button, QZ_ICON_PLAYER_PLAY, 20,
                                    qz_color(QZ_TEXT_ON_ACCENT));
    if (pomo_start_play) lv_obj_align(pomo_start_play, LV_ALIGN_CENTER, -67, 0);
    pomo_start_pause = qz_symbol(pomo_start_button, LV_SYMBOL_PAUSE, 18,
                                 qz_color(QZ_TEXT_ON_ACCENT));
    lv_obj_align(pomo_start_pause, LV_ALIGN_CENTER, -67, 0);
    lv_obj_add_flag(pomo_start_pause, LV_OBJ_FLAG_HIDDEN);
    pomo_start_caption = qz_text(pomo_start_button, "开始专注", 15,
                                 qz_color(QZ_TEXT_ON_ACCENT));
    lv_obj_align(pomo_start_caption, LV_ALIGN_CENTER, 14, 0);
    lv_obj_add_event_cb(pomo_start_button, pomo_toggle, LV_EVENT_CLICKED, NULL);

    pomo_stop_button = qz_button(screen, "结束", 126, 40);
    lv_obj_align(pomo_stop_button, LV_ALIGN_BOTTOM_RIGHT, -QZ_GUTTER, -6);
    lv_obj_add_event_cb(pomo_stop_button, pomo_reset, LV_EVENT_CLICKED, NULL);
    lv_obj_t *stop_caption = lv_obj_get_child(pomo_stop_button, 0);
    if (stop_caption) lv_obj_align(stop_caption, LV_ALIGN_CENTER, 12, 0);
    lv_obj_t *stop_icon = qz_icon_image(pomo_stop_button, QZ_ICON_X, 16, qz_color(QZ_TEXT));
    if (stop_icon) lv_obj_align(stop_icon, LV_ALIGN_CENTER, -30, 0);

    lv_obj_add_event_cb(screen, pomo_screen_loaded, LV_EVENT_SCREEN_LOADED, NULL);

    /* 建页时先对一次表：核心可能已经有一段在跑的番茄钟（语音开的） */
    pomo_refresh();
    pomo_reload();
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

static bool device_command_busy;
static bool device_waiting;
static bool device_target_state;
static char device_pending_id[QZ_DEVICE_ID_MAX];
static uint32_t device_pending_at;

static void device_command_done(bool ok, const char *response, void *user_data)
{
    (void)response;
    (void)user_data;
    device_command_busy = false;
    device_waiting = ok;
    device_pending_at = lv_tick_get();
    control_reload_later();
    if (control_hint) {
        lv_label_set_text(control_hint, ok ? "命令已发送，等待设备上报状态" : "命令发送失败，请重试");
        lv_obj_clear_flag(control_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

static void device_switch_changed(lv_event_t *event)
{
    lv_obj_t *toggle = lv_event_get_target(event);
    int index = (int)(intptr_t)lv_obj_get_user_data(toggle);
    if (index < 0 || index >= control_device_count) return;
    bool on = lv_obj_has_state(toggle, LV_STATE_CHECKED);
    if (on) lv_obj_remove_state(toggle, LV_STATE_CHECKED);
    else lv_obj_add_state(toggle, LV_STATE_CHECKED);
    if (device_command_busy) {
        if (control_hint) lv_label_set_text(control_hint, "上一条命令仍在执行");
        return;
    }
    char quoted[QZ_DEVICE_ID_MAX * 6 + 3];
    char payload[sizeof(quoted) + 48];
    bool encoded = qz_json_quote(control_devices[index].id, quoted, sizeof(quoted));
    snprintf(payload, sizeof(payload), "{\"id\":%s,\"action\":\"%s\"}",
             encoded ? quoted : "\"\"", on ? "on" : "off");
    if (!control_devices[index].online || !encoded ||
        !qz_web_request_async("POST", "/api/devices/command", payload, device_command_done, NULL)) {
        if (control_hint) {
            lv_label_set_text(control_hint, "设备离线或命令发送失败，请重试");
            lv_obj_clear_flag(control_hint, LV_OBJ_FLAG_HIDDEN);
        }
        if (on) lv_obj_remove_state(toggle, LV_STATE_CHECKED);
        else lv_obj_add_state(toggle, LV_STATE_CHECKED);
        return;
    }
    device_command_busy = true;
    device_target_state = on;
    snprintf(device_pending_id, sizeof(device_pending_id), "%s", control_devices[index].id);
    if (control_hint) {
        lv_label_set_text(control_hint, "执行中，等待设备状态");
        lv_obj_clear_flag(control_hint, LV_OBJ_FLAG_HIDDEN);
    }
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
    qz_touch_target(toggle);
    lv_obj_align(toggle, LV_ALIGN_RIGHT_MID, -10, 0);
    if (device->has_state && device->state) lv_obj_add_state(toggle, LV_STATE_CHECKED);
    if (!device->online) lv_obj_add_state(toggle, LV_STATE_DISABLED);
    lv_obj_set_user_data(toggle, (void *)(intptr_t)index);
    lv_obj_add_event_cb(toggle, device_switch_changed, LV_EVENT_VALUE_CHANGED, NULL);
}

/** 取一次设备表，把中枢状态与设备行重画一遍。 */
static bool control_loading;

static void control_loaded(bool ok, const char *body, void *data)
{
    (void)data;
    control_loading = false;
    qz_hub_status_t hub = {0};
    char text[64];
    control_device_count = ok ? qz_devices_parse(body, control_devices, QZ_DEVICE_MAX, &hub) : -1;

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
            snprintf(text, sizeof(text), "浏览器打开设备 IP:%u 配置智能家居", qz_web_port());
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

static void control_reload(void)
{
    if (control_loading) return;
    control_loading = qz_web_request_async("GET", "/api/devices", NULL, control_loaded, NULL);
    if (!control_loading && control_hint) lv_label_set_text(control_hint, "无法刷新，请重试");
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
        /* 拖拽条本体只有 20 实像素高，补到物理下限；行距 48 实像素，
         * 两条滑条的命中区之间还留得下缝。 */
        qz_touch_target(slider);
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
    qz_touch_target(toggle);
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
    if (device_waiting) {
        control_reload();
        bool confirmed = false;
        for (int i = 0; i < control_device_count; i++) {
            if (strcmp(control_devices[i].id, device_pending_id) == 0 &&
                control_devices[i].has_state && control_devices[i].state == device_target_state)
                confirmed = true;
        }
        if (confirmed || lv_tick_elaps(device_pending_at) >= 10000) {
            device_waiting = false;
            if (control_hint) {
                lv_label_set_text(control_hint, confirmed ? "设备状态已确认" : "设备未确认执行，显示最后上报状态");
                lv_obj_clear_flag(control_hint, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
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

void qz_applets_set_back(lv_obj_t *screen)
{
    back_screen_ref = screen;
}

/** 这条提醒距离下一次响还有多少分钟；不会再响的（过期的一次性提醒）返回 -1。
 *
 *  与核心 `fire_due_reminders()` 同一套判断：每日提醒取今天或明天那一次，
 *  写了日期的一次性提醒只在当天算数，没写日期的按"下一次时钟对上"算，
 *  也就是今天还没到就今天、已经过了就明天。 */
static int reminder_minutes_until(const reminder_t *reminder, const struct tm *now)
{
    int now_minutes = now->tm_hour * 60 + now->tm_min;
    int target = reminder->hour * 60 + reminder->minute;

    if (reminder->daily) {
        int delta = target - now_minutes;
        return delta > 0 ? delta : delta + 24 * 60;
    }
    if (reminder->date[0] != '\0') {
        int year, month, day;
        if (sscanf(reminder->date, "%4d-%2d-%2d", &year, &month, &day) != 3) return -1;
        struct tm when;
        memset(&when, 0, sizeof(when));
        when.tm_year = year - 1900;
        when.tm_mon = month - 1;
        when.tm_mday = day;
        when.tm_isdst = -1;
        struct tm today = *now;
        today.tm_hour = 0;
        today.tm_min = 0;
        today.tm_sec = 0;
        today.tm_isdst = -1;
        time_t today_at = mktime(&today);
        time_t when_at = mktime(&when);
        if (today_at == (time_t)-1 || when_at == (time_t)-1) return -1;
        /* 用两个本地零点的差算天数：能跨过夏令时的 23/25 小时日。 */
        long days = (long)((when_at - today_at) / (24 * 60 * 60));
        if (days < 0) return -1;                     /* 那天已经过去 */
        int delta = (int)days * 24 * 60 + target - now_minutes;
        return delta > 0 ? delta : -1;               /* 今天的时间也已过 */
    }
    int delta = target - now_minutes;
    return delta > 0 ? delta : delta + 24 * 60;
}

void qz_applets_glance(qz_applets_glance_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->focus_active = pomo_active;
    out->focus_paused = pomo_paused;
    out->focus_break = pomo_break;
    out->focus_remaining = pomo_remaining;

    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    int best = -1;
    for (int i = 0; i < reminder_count; i++) {
        if (!reminders[i].enabled) continue;
        int minutes = reminder_minutes_until(&reminders[i], &local);
        if (minutes < 0 || (best >= 0 && minutes >= best)) continue;
        best = minutes;
        out->has_reminder = true;
        out->reminder_in_minutes = minutes;
        out->reminder_hour = reminders[i].hour;
        out->reminder_minute = reminders[i].minute;
        /* 按完整字符截，半个汉字会以乱码出现在主页上。 */
        size_t used = 0;
        const char *text = reminders[i].label;
        while (*text && used + 1 < sizeof(out->reminder_label)) {
            unsigned char lead = (unsigned char)*text;
            size_t width = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
            if (used + width + 1 > sizeof(out->reminder_label)) break;
            memcpy(out->reminder_label + used, text, width);
            used += width;
            text += width;
        }
        out->reminder_label[used] = '\0';
    }
}

qz_face_state_t qz_applets_focus_face(void)
{
    if (!pomo_active) return QZ_FACE_IDLE;
    if (pomo_paused) return QZ_FACE_SLEEPY;
    return pomo_break ? QZ_FACE_HAPPY : QZ_FACE_THINKING;
}
