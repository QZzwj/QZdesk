#include "notifications.h"
#include "config.h"
#include "qzdesk_core.h"
#include "theme.h"
#include "web_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool dnd;
static bool sleeping;
static int previous_brightness;
static lv_obj_t *notice;
static char notice_text[1024];
static char preference_path[512];
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
static void show_next(void);

bool qz_notifications_dnd(void) { return dnd; }

bool qz_notifications_set_dnd(bool enabled)
{
    FILE *file = fopen(preference_path, "w");
    if (!file) return false;
    bool ok = fprintf(file, "%d\n", enabled ? 1 : 0) > 0;
    if (fclose(file) != 0) ok = false;
    if (ok) {
        dnd = enabled;
        if (!dnd) show_next();
    }
    return ok;
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
    show_next();
}

static void show_next(void)
{
    if (notice || dnd || !queue_head) return;
    notification_item_t *item = queue_head;
    queue_head = item->next;
    if (!queue_head) queue_tail = NULL;
    snprintf(notice_text, sizeof(notice_text), "%s", item->text);
    free(item);
    if (sleeping) {
        if (qz_backlight_set(previous_brightness)) sleeping = false;
    }
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

static void idle_watch(lv_timer_t *timer)
{
    (void)timer;
    uint32_t idle = lv_display_get_inactive_time(NULL);
    if (sleeping && idle < 1000 && !notice) {
        qz_backlight_set(previous_brightness);
        sleeping = false;
    } else if (!sleeping && idle >= 120000 && !notice) {
        int brightness = qz_backlight_level();
        if (brightness > 0 && qz_backlight_set(0)) {
            previous_brightness = brightness;
            sleeping = true;
        }
    }
}

void qz_notifications_init(void)
{
    const char *home = getenv("HOME");
    snprintf(preference_path, sizeof(preference_path), "%s/.qzdesk-dnd", home ? home : "/tmp");
    FILE *file = fopen(preference_path, "r");
    int enabled = 0;
    if (file) { if (fscanf(file, "%d", &enabled) != 1) enabled = 0; fclose(file); }
    dnd = enabled != 0;
    qzdesk_core_subscribe(notification_event, NULL);
    lv_timer_create(idle_watch, 500, NULL);
}
