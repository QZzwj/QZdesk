#include "notifications.h"
#include "config.h"
#include "qzdesk_core.h"
#include "theme.h"
#include "web_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static bool manual_dnd;
static bool dnd;
static bool schedule_enabled;
static int schedule_start = 22 * 60;
static int schedule_end = 8 * 60;
static bool schedule_active;
static bool schedule_suppressed;
static bool sleeping;
static bool backlight_fading;
static uint32_t backlight_fade_started;
static int backlight_fade_from;
static int backlight_last_set;
/* 背光是这个进程在管，但进程重启会丢掉睡前记下的亮度。全黑的 LCD 看
 * 起来和死机没区别，所以睡眠值取最低非零，重启后也还能被认出来。 */
#define BACKLIGHT_DEFAULT 40
#define BACKLIGHT_SLEEP 1
#define SCREEN_IDLE_TIMEOUT_MS 120000U
#define SCREEN_WAKE_IDLE_MS 1000U
#define BACKLIGHT_FADE_MS 10000U
#define SCREEN_WATCH_PERIOD_MS 100U
#define SCHEDULE_CHECK_MS 500U
static int previous_brightness = BACKLIGHT_DEFAULT;
static lv_obj_t *notice;
static char notice_text[1024];
static char preference_path[512];
static char schedule_preference_path[512];
static bool snooze_busy;
static lv_obj_t *notice_status;
static lv_obj_t *notice_done;
static lv_obj_t *notice_later;
typedef struct notification_item {
    char text[1024];
    struct notification_item *next;
} notification_item_t;
static notification_item_t *queue_head;
static notification_item_t *queue_tail;

/* ------------------------------------------------------------------------- *
 * 通知中心
 *
 * 顶部下拉出来的面板，和那条"得点一下"的弹窗是同一批通知的两面：弹窗管
 * 当下这一条，面板回答"我刚才错过了什么"。所以免打扰开着、或者面板正开着
 * 的时候通知照样进列表（进队列也进历史），只是先不弹；面板关掉后队列继续。
 * ------------------------------------------------------------------------- */
#define NOTICE_HISTORY 8    /**< 面板里最多留最近 8 条：再多也翻不完，还一直占内存 */
#define NOTICE_PANEL_H 196  /**< 面板高度（设计像素）：盖住上半屏，下面留白点一下关掉 */
#define NOTICE_HEADER_H 48
#define NOTICE_PAD 14

static bool panel_open;
static lv_obj_t *panel_overlay;
static lv_obj_t *panel;
static lv_obj_t *panel_list;
static lv_obj_t *panel_empty;
static lv_obj_t *panel_clear;
static lv_obj_t *panel_dnd_switch;
static notification_item_t *history_head;
static notification_item_t *history_tail;
static int history_count;
static void (*dnd_listener)(bool enabled);

static void show_next(void);
static void rebuild_panel_list(void);
static void sync_dnd_controls(void);

/** Stop an in-progress fade and bring the panel straight back to usable light. */
static void wake_display(void)
{
    if (!sleeping && !backlight_fading) return;
    backlight_fading = false;
    if (qz_backlight_set(previous_brightness)) sleeping = false;
}

bool qz_notifications_dnd(void) { return dnd; }
bool qz_notifications_manual_dnd(void) { return manual_dnd; }

void qz_notifications_set_dnd_listener(void (*listener)(bool enabled))
{
    dnd_listener = listener;
}

static bool minute_valid(int minute)
{
    return minute >= 0 && minute < 24 * 60;
}

static bool schedule_contains(int minute)
{
    if (!schedule_enabled || !minute_valid(minute) || schedule_start == schedule_end)
        return false;
    if (schedule_start < schedule_end)
        return minute >= schedule_start && minute < schedule_end;
    return minute >= schedule_start || minute < schedule_end;
}

static int local_minute_of_day(void)
{
    time_t now = time(NULL);
    struct tm local;
    if (now == (time_t)-1 || !localtime_r(&now, &local)) return -1;
    return local.tm_hour * 60 + local.tm_min;
}

static bool effective_dnd(void)
{
    return manual_dnd || (schedule_enabled && schedule_active && !schedule_suppressed);
}

/** Notify only when the state the user actually experiences changes. */
static void sync_effective_dnd(void)
{
    bool next = effective_dnd();
    if (next == dnd) return;
    dnd = next;
    sync_dnd_controls();
    if (!dnd) show_next();
    if (dnd_listener) dnd_listener(dnd);
}

/** Track the schedule boundary once per UI tick; menus and modals stay responsive. */
static void update_schedule_state(void)
{
    bool active = schedule_contains(local_minute_of_day());
    if (active == schedule_active) return;
    schedule_active = active;
    /* A manual override applies only to the current window. */
    schedule_suppressed = false;
    sync_effective_dnd();
}

static void sync_dnd_controls(void)
{
    if (!panel_dnd_switch) return;
    if (dnd) lv_obj_add_state(panel_dnd_switch, LV_STATE_CHECKED);
    else lv_obj_remove_state(panel_dnd_switch, LV_STATE_CHECKED);
}

bool qz_notifications_schedule_get(bool *enabled, int *start_minute, int *end_minute)
{
    if (!enabled || !start_minute || !end_minute) return false;
    *enabled = schedule_enabled;
    *start_minute = schedule_start;
    *end_minute = schedule_end;
    return true;
}

bool qz_notifications_schedule_set(bool enabled, int start_minute, int end_minute)
{
    if (!minute_valid(start_minute) || !minute_valid(end_minute) ||
        (enabled && start_minute == end_minute))
        return false;
    FILE *file = fopen(schedule_preference_path, "w");
    if (!file) return false;
    bool ok = fprintf(file, "%d %d %d\n", enabled ? 1 : 0,
                      start_minute, end_minute) > 0;
    if (fclose(file) != 0) ok = false;
    if (!ok) return false;

    schedule_enabled = enabled;
    schedule_start = start_minute;
    schedule_end = end_minute;
    schedule_active = schedule_contains(local_minute_of_day());
    if (schedule_active) schedule_suppressed = false;
    if (!schedule_enabled) schedule_suppressed = false;
    sync_effective_dnd();
    return true;
}

bool qz_notifications_set_dnd(bool enabled)
{
    FILE *file = fopen(preference_path, "w");
    if (!file) return false;
    bool ok = fprintf(file, "%d\n", enabled ? 1 : 0) > 0;
    if (fclose(file) != 0) ok = false;
    if (ok) {
        manual_dnd = enabled;
        if (enabled) schedule_suppressed = false;
        else if (schedule_enabled && schedule_active) schedule_suppressed = true;
        sync_effective_dnd();
    }
    return ok;
}

/** 历史是最新在前：面板从上往下读就是"最近发生的事"。 */
static void history_add(const char *text)
{
    notification_item_t *item = calloc(1, sizeof(*item));
    if (!item) return;
    snprintf(item->text, sizeof(item->text), "%s", text);
    item->next = history_head;
    history_head = item;
    if (!history_tail) history_tail = item;
    if (++history_count > NOTICE_HISTORY) {
        /* 掐掉最旧的那条：单链表没有前驱指针，从表头走到尾巴前面那一节。 */
        notification_item_t *node = history_head;
        while (node->next != history_tail) node = node->next;
        free(history_tail);
        node->next = NULL;
        history_tail = node;
        history_count--;
    }
    if (panel_open) rebuild_panel_list();
}

/** 清空列表和等待弹出的队列：清完再关面板，不该又冒出来一条。 */
static void history_clear(void)
{
    while (history_head) {
        notification_item_t *next = history_head->next;
        free(history_head);
        history_head = next;
    }
    history_tail = NULL;
    history_count = 0;
    while (queue_head) {
        notification_item_t *next = queue_head->next;
        free(queue_head);
        queue_head = next;
    }
    queue_tail = NULL;
    if (panel_open) rebuild_panel_list();
}

static void dismiss(lv_event_t *event)
{
    (void)event;
    if (snooze_busy) return;
    if (notice) lv_obj_delete(notice);
    notice = NULL;
    notice_status = notice_done = notice_later = NULL;
    lv_display_trigger_activity(NULL);
    show_next();
}

static void snoozed(bool ok, const char *body, void *data)
{
    (void)body;
    (void)data;
    snooze_busy = false;
    if (ok) dismiss(NULL);
    else if (notice) {
        lv_obj_remove_state(notice_done, LV_STATE_DISABLED);
        lv_obj_remove_state(notice_later, LV_STATE_DISABLED);
        lv_label_set_text(notice_status, "保存失败，请重试");
    }
}

static void snooze(lv_event_t *event)
{
    (void)event;
    if (snooze_busy) return;
    snooze_busy = true;
    char quoted[1536], body[1600];
    if (!qz_json_quote(notice_text, quoted, sizeof(quoted))) {
        snoozed(false, "", NULL);
        return;
    }
    lv_obj_add_state(notice_done, LV_STATE_DISABLED);
    lv_obj_add_state(notice_later, LV_STATE_DISABLED);
    lv_label_set_text(notice_status, "正在保存，请稍候");
    snprintf(body, sizeof(body), "{\"text\":%s,\"minutes\":5}", quoted);
    if (!qz_web_request_async("POST", "/api/notifications/snooze", body, snoozed, NULL))
        snoozed(false, "", NULL);
}

static void notification_event(const qzdesk_core_event_t *event, void *data)
{
    (void)data;
    if (event->type != QZDESK_CORE_EVENT_TOAST || strcmp(event->role, "timer") != 0) return;
    notification_item_t *item = calloc(1, sizeof(*item));
    if (!item) return;
    snprintf(item->text, sizeof(item->text), "%s", event->text);
    if (queue_tail) queue_tail->next = item;
    else queue_head = item;
    queue_tail = item;
    /* 列表是"已经来过什么"的记录：免打扰和面板开着都照记。 */
    history_add(event->text);
    show_next();
}

static void show_next(void)
{
    /* 面板开着的时候先别弹：用户正在看列表，等他关掉再按队列顺序来。 */
    if (notice || dnd || panel_open || !queue_head) return;
    notification_item_t *item = queue_head;
    queue_head = item->next;
    if (!queue_head) queue_tail = NULL;
    snprintf(notice_text, sizeof(notice_text), "%s", item->text);
    free(item);
    wake_display();
    lv_display_trigger_activity(NULL);
    notice = lv_obj_create(lv_layer_top());
    lv_obj_set_size(notice, lv_pct(94), lv_pct(72));
    lv_obj_center(notice);
    qz_obj_set_bg_color(notice, QZ_CARD, 0);
    lv_obj_t *label = qz_text(notice, notice_text, 14, qz_color(QZ_TEXT));
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_t *done = qz_button(notice, "完成", 90, 40);
    lv_obj_align(done, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(done, dismiss, LV_EVENT_CLICKED, NULL);
    lv_obj_t *later = qz_button(notice, "5分钟后", 112, 40);
    lv_obj_align(later, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(later, snooze, LV_EVENT_CLICKED, NULL);
    notice_done = done;
    notice_later = later;
    lv_obj_set_height(label, lv_pct(50));
    notice_status = qz_text(notice, "", 12, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_set_width(notice_status, lv_pct(100));
    lv_obj_align(notice_status, LV_ALIGN_BOTTOM_LEFT, 0, -44);
}

/* ------------------------------------------------------------------------- *
 * 通知中心面板
 * ------------------------------------------------------------------------- */

static void panel_translate_y(void *var, int32_t value)
{
    /* 动画给的是已经缩好的面板坐标，别再走一次 scale.h 的缩放宏。 */
    (lv_obj_set_style_translate_y)((lv_obj_t *)var, (lv_coord_t)value, 0);
}

static void panel_scrim_opa(void *var, int32_t value)
{
    (lv_obj_set_style_bg_opa)((lv_obj_t *)var, (lv_opa_t)value, 0);
}

static void panel_delete_ready(lv_anim_t *anim)
{
    lv_obj_t *overlay = (lv_obj_t *)anim->var;
    if (overlay) lv_obj_delete(overlay);
}

/** 一条通知：连续记录行，只靠发丝线分隔。 */
static lv_obj_t *history_row(lv_obj_t *parent, const char *text)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, 40, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    qz_obj_set_border_color(row, QZ_SEPARATOR, 0);
    lv_obj_set_style_border_opa(row, (lv_opa_t)150, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 4, 0);
    lv_obj_set_style_pad_ver(row, 9, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *label = qz_text(row, text, 15, qz_color(QZ_TEXT));
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return row;
}

/** 列表按历史重建：最多 8 条，重建比逐条增删好懂，也不怕中途出错。 */
static void rebuild_panel_list(void)
{
    if (!panel_list) return;
    lv_obj_clean(panel_list);
    if (panel_empty) {
        if (history_count == 0) lv_obj_clear_flag(panel_empty, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(panel_empty, LV_OBJ_FLAG_HIDDEN);
    }
    if (panel_clear) {
        if (history_count == 0) lv_obj_add_flag(panel_clear, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(panel_clear, LV_OBJ_FLAG_HIDDEN);
    }
    for (notification_item_t *item = history_head; item; item = item->next) {
        history_row(panel_list, item->text);
    }
}

static void panel_dnd_changed(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    bool enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (qz_notifications_set_dnd(enabled)) return;
    /* 写不进文件就把开关退回原位：停在没生效的位置比不响应更糟。 */
    if (enabled) lv_obj_remove_state(sw, LV_STATE_CHECKED);
    else lv_obj_add_state(sw, LV_STATE_CHECKED);
}

static void panel_clear_clicked(lv_event_t *event)
{
    (void)event;
    history_clear();
    lv_display_trigger_activity(NULL);
}

static void panel_scrim_clicked(lv_event_t *event)
{
    /* 只有点在遮罩本身上才算"点在外面"：面板里控件的事件会冒上来。 */
    if (lv_event_get_target(event) != lv_event_get_current_target(event)) return;
    qz_notifications_close_panel();
}

void qz_notifications_open_panel(void)
{
    if (panel_open) return;
    panel_open = true;
    /* 面板本身就是"用户在看"：先亮屏；那条弹窗让位，因为它已经进列表了。 */
    wake_display();
    lv_display_trigger_activity(NULL);
    if (notice) {
        lv_obj_delete(notice);
        notice = NULL;
        notice_status = notice_done = notice_later = NULL;
    }

    panel_overlay = qz_overlay_create(lv_layer_top());
    lv_obj_add_event_cb(panel_overlay, panel_scrim_clicked, LV_EVENT_CLICKED, NULL);

    panel = lv_obj_create(panel_overlay);
    lv_obj_set_size(panel, QZ_DESIGN_W - 2 * QZ_GUTTER, NOTICE_PANEL_H);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, QZ_GUTTER);
    qz_style_sheet(panel);
    lv_obj_set_style_pad_all(panel, 0, 0);

    /* 页头：标题在左，免打扰开关和清空在右。三个都对着一条中线排，
     * 免得字号不一样就显得高低不齐。 */
    lv_obj_t *header = lv_obj_create(panel);
    lv_obj_set_size(header, lv_pct(100), NOTICE_HEADER_H);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_shadow_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = qz_text(header, "通知", 18, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_LEFT_MID, NOTICE_PAD, 0);

    panel_clear = qz_icon_button(header, LV_SYMBOL_TRASH, 30);
    lv_obj_align(panel_clear, LV_ALIGN_RIGHT_MID, -NOTICE_PAD, 0);
    lv_obj_add_event_cb(panel_clear, panel_clear_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *sw = lv_switch_create(header);
    panel_dnd_switch = sw;
    qz_style_switch(sw);
    lv_obj_set_size(sw, 48, 26);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -NOTICE_PAD - 44, 0);
    sync_dnd_controls();
    lv_obj_add_event_cb(sw, panel_dnd_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *dnd_label = qz_text(header, "免打扰", 12, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align_to(dnd_label, sw, LV_ALIGN_OUT_LEFT_MID, -8, 0);

    /* 列表：从页头底下一直到面板底部，多了就滚。 */
    panel_list = lv_obj_create(panel);
    lv_obj_set_width(panel_list, lv_pct(100));
    lv_obj_set_height(panel_list, NOTICE_PANEL_H - NOTICE_HEADER_H);
    lv_obj_align(panel_list, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(panel_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(panel_list, 0, 0);
    lv_obj_set_style_shadow_width(panel_list, 0, 0);
    lv_obj_set_style_pad_all(panel_list, NOTICE_PAD, 0);
    lv_obj_set_style_pad_row(panel_list, 8, 0);
    lv_obj_set_flex_flow(panel_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(panel_list, LV_DIR_VER);
    qz_make_scrollable(panel_list);

    panel_empty = qz_text(panel, "暂无通知", 15, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(panel_empty, LV_ALIGN_TOP_MID, 0, NOTICE_HEADER_H + 48);

    rebuild_panel_list();

    if (!qz_reduce_motion()) {
        /* 从屏幕上方滑下来，遮罩同时压暗：一次下拉的两种表现。 */
        int32_t off = -(int32_t)qz_scale_y(NOTICE_PANEL_H + QZ_GUTTER);
        (lv_obj_set_style_translate_y)(panel, off, 0);
        lv_anim_t slide;
        lv_anim_init(&slide);
        lv_anim_set_var(&slide, panel);
        lv_anim_set_exec_cb(&slide, panel_translate_y);
        lv_anim_set_values(&slide, off, 0);
        lv_anim_set_duration(&slide, QZ_DUR_PANEL);
        qz_anim_ease_out(&slide);
        lv_anim_start(&slide);

        (lv_obj_set_style_bg_opa)(panel_overlay, LV_OPA_TRANSP, 0);
        lv_anim_t scrim;
        lv_anim_init(&scrim);
        lv_anim_set_var(&scrim, panel_overlay);
        lv_anim_set_exec_cb(&scrim, panel_scrim_opa);
        lv_anim_set_values(&scrim, LV_OPA_TRANSP, 110);
        lv_anim_set_duration(&scrim, QZ_DUR_PANEL);
        qz_anim_ease_out(&scrim);
        lv_anim_start(&scrim);
    }
}

bool qz_notifications_panel_open(void) { return panel_open; }

void qz_notifications_close_panel(void)
{
    if (!panel_overlay) return;
    lv_obj_t *overlay = panel_overlay;
    lv_obj_t *sheet = panel;
    panel_overlay = NULL;
    panel = panel_list = panel_empty = panel_clear = NULL;
    panel_dnd_switch = NULL;
    panel_open = false;

    if (qz_reduce_motion()) {
        lv_obj_delete(overlay);
    } else {
        /* 原路收回，动画结束再删：直接消失会像掉帧。 */
        int32_t off = -(int32_t)qz_scale_y(NOTICE_PANEL_H + QZ_GUTTER);
        lv_anim_t slide;
        lv_anim_init(&slide);
        lv_anim_set_var(&slide, sheet);
        lv_anim_set_exec_cb(&slide, panel_translate_y);
        lv_anim_set_values(&slide, lv_obj_get_style_translate_y(sheet, 0), off);
        lv_anim_set_duration(&slide, QZ_DUR_PANEL);
        qz_anim_ease_out(&slide);
        lv_anim_start(&slide);

        lv_anim_t scrim;
        lv_anim_init(&scrim);
        lv_anim_set_var(&scrim, overlay);
        lv_anim_set_exec_cb(&scrim, panel_scrim_opa);
        lv_anim_set_values(&scrim, lv_obj_get_style_bg_opa(overlay, 0), LV_OPA_TRANSP);
        lv_anim_set_duration(&scrim, QZ_DUR_PANEL);
        qz_anim_ease_out(&scrim);
        lv_anim_set_completed_cb(&scrim, panel_delete_ready);
        lv_anim_start(&scrim);
    }
    /* 关掉面板，攒下的通知按队列继续弹（免打扰开着就继续压着）。 */
    show_next();
}

static lv_obj_t *pair_dialog;
static char pair_id[64];
static bool pair_polling;
static bool pair_saving;

static void pair_decided(bool ok, const char *body, void *data)
{
    (void)body; (void)data;
    pair_saving = false;
    if (ok && pair_dialog) {
        lv_obj_delete(pair_dialog);
        pair_dialog = NULL;
        pair_id[0] = '\0';
    }
}

static void pair_decide(lv_event_t *event)
{
    if (pair_saving) return;
    char body[160];
    snprintf(body, sizeof(body), "{\"id\":\"%s\",\"allow\":%s,\"control\":%s}", pair_id,
             lv_event_get_user_data(event) ? "true" : "false",
             lv_event_get_user_data(event) == (void *)2 ? "true" : "false");
    pair_saving = true;
    if (!qz_web_request_async("POST", "/api/computer/decide", body, pair_decided, NULL))
        pair_saving = false;
}

static void pair_received(bool ok, const char *body, void *data)
{
    (void)data;
    pair_polling = false;
    if (!ok || pair_saving) return;
    char id[64], name[160], code[32], peer[64], text[400];
    if (!qz_json_string(body, "id", id, sizeof(id))) {
        if (pair_dialog) lv_obj_delete(pair_dialog);
        pair_dialog = NULL;
        pair_id[0] = '\0';
        return;
    }
    if (pair_dialog && strcmp(pair_id, id) == 0) return;
    if (pair_dialog) lv_obj_delete(pair_dialog);
    snprintf(pair_id, sizeof(pair_id), "%s", id);
    qz_json_string(body, "name", name, sizeof(name));
    qz_json_string(body, "code", code, sizeof(code));
    qz_json_string(body, "peer", peer, sizeof(peer));
    snprintf(text, sizeof(text), "电脑配对请求\n%s · %s\n校验码：%s\n核对校验码。仅查看读取文字；允许控制可点击、输入。", name, peer, code);
    wake_display();
    lv_display_trigger_activity(NULL);
    pair_dialog = lv_obj_create(lv_layer_top());
    lv_obj_set_size(pair_dialog, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(pair_dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(pair_dialog, 12, 0);
    lv_obj_center(pair_dialog);
    qz_obj_set_bg_color(pair_dialog, QZ_CARD, 0);
    lv_obj_t *label = qz_text(pair_dialog, text, 18, qz_color(QZ_TEXT));
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_height(label, lv_pct(72));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_t *deny = qz_button(pair_dialog, "拒绝", 110, 56);
    lv_obj_t *view = qz_button(pair_dialog, "仅查看", 130, 56);
    lv_obj_align(view, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(view, pair_decide, LV_EVENT_CLICKED, (void *)1);
    lv_obj_align(deny, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(deny, pair_decide, LV_EVENT_CLICKED, NULL);
    lv_obj_t *allow = qz_button(pair_dialog, "允许控制", 140, 56);
    lv_obj_align(allow, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(allow, pair_decide, LV_EVENT_CLICKED, (void *)2);
}

static void pair_watch(lv_timer_t *timer)
{
    (void)timer;
    if (!pair_polling && !pair_saving) {
        pair_polling = true;
        if (!qz_web_request_async("GET", "/api/computer/pending", NULL, pair_received, NULL))
            pair_polling = false;
    }
}

/** Smoothstep eases both ends: the first visible change is subtle and the last
 *  step into the sleep level does not snap. */
static int fade_percent(int from, uint32_t elapsed)
{
    if (elapsed >= BACKLIGHT_FADE_MS) return BACKLIGHT_SLEEP;
    int64_t t = (int64_t)elapsed * 1000 / BACKLIGHT_FADE_MS;
    int64_t eased = t * t * (3000 - 2 * t) / 1000000;
    int delta = BACKLIGHT_SLEEP - from;
    int64_t scaled = (int64_t)delta * eased;
    /* Round away from zero while dimming: adding the usual +500 would round
     * tiny negative steps back to zero and leave the first second at 40%. */
    int offset = (int)(delta < 0 ? (scaled - 999) / 1000 : (scaled + 500) / 1000);
    int value = from + offset;
    if (value < BACKLIGHT_SLEEP) value = BACKLIGHT_SLEEP;
    if (value > 100) value = 100;
    return value;
}

static void update_backlight_fade(void)
{
    uint32_t elapsed = lv_tick_elaps(backlight_fade_started);
    if (elapsed >= BACKLIGHT_FADE_MS) {
        bool ok = qz_backlight_set(BACKLIGHT_SLEEP);
        backlight_fading = false;
        sleeping = ok;
        return;
    }

    int value = fade_percent(backlight_fade_from, elapsed);
    if (value == backlight_last_set) return;
    if (qz_backlight_set(value)) backlight_last_set = value;
}

static void begin_backlight_fade(void)
{
    int brightness = qz_backlight_level();
    if (brightness <= BACKLIGHT_SLEEP) return;
    previous_brightness = brightness;
    backlight_fade_from = brightness;
    backlight_last_set = brightness;
    backlight_fade_started = lv_tick_get();
    backlight_fading = true;
}

static void idle_watch(lv_timer_t *timer)
{
    (void)timer;
    uint32_t now = lv_tick_get();
    static uint32_t schedule_checked;
    if (lv_tick_elaps(schedule_checked) >= SCHEDULE_CHECK_MS) {
        update_schedule_state();
        schedule_checked = now;
    }

    uint32_t idle = lv_display_get_inactive_time(NULL);
    bool attention = notice || pair_dialog || panel_open;
    /* 触摸后立刻中止渐暗；弹窗或通知中心出现时也不把用户留在暗屏上。 */
    if ((sleeping || backlight_fading) &&
        (idle < SCREEN_WAKE_IDLE_MS || attention)) {
        wake_display();
        return;
    }
    if (backlight_fading) {
        update_backlight_fade();
        return;
    }
    if (!sleeping && !attention && idle >= SCREEN_IDLE_TIMEOUT_MS) {
        begin_backlight_fade();
    }
}

void qz_notifications_init(void)
{
    /* 上一条命要是睡到一半被换掉，系统里留着的就是最暗那档，这里必须
     * 拉回能看见的亮度，否则重启完屏幕一直黑着。 */
    int current = qz_backlight_level();
    if (current >= 0 && current <= BACKLIGHT_SLEEP) {
        qz_backlight_set(BACKLIGHT_DEFAULT);
        current = BACKLIGHT_DEFAULT;
    }
    if (current > BACKLIGHT_SLEEP) previous_brightness = current;

    const char *home = getenv("HOME");
    snprintf(preference_path, sizeof(preference_path), "%s/.qzdesk-dnd", home ? home : "/tmp");
    FILE *file = fopen(preference_path, "r");
    int enabled = 0;
    if (file) { if (fscanf(file, "%d", &enabled) != 1) enabled = 0; fclose(file); }
    manual_dnd = enabled != 0;

    snprintf(schedule_preference_path, sizeof(schedule_preference_path),
             "%s/.qzdesk-dnd-schedule", home ? home : "/tmp");
    file = fopen(schedule_preference_path, "r");
    if (file) {
        int scheduled = 0;
        int start = 22 * 60;
        int end = 8 * 60;
        if (fscanf(file, "%d %d %d", &scheduled, &start, &end) == 3 &&
            minute_valid(start) && minute_valid(end) &&
            (!scheduled || start != end)) {
            schedule_enabled = scheduled != 0;
            schedule_start = start;
            schedule_end = end;
        }
        fclose(file);
    }
    schedule_active = schedule_contains(local_minute_of_day());
    schedule_suppressed = false;
    dnd = effective_dnd();
    qzdesk_core_subscribe(notification_event, NULL);
    lv_timer_create(idle_watch, SCREEN_WATCH_PERIOD_MS, NULL);
    lv_timer_create(pair_watch, 1500, NULL);
}
