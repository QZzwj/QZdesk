/* Headless validation of the real page objects. The shell driver links this
 * main in place of app/main.c; GNU ld --wrap keeps fixtures out of production. */
#include "apps.h"
#include "applets.h"
#include "assistant.h"
#include "desktop.h"
#include "performance_page.h"
#include "settings.h"
#include "skills_page.h"
#include "skills.h"
#include "smarthome.h"
#include "theme.h"
#include "wifi.h"
#include "web_client.h"
#include "qzdesk_core.h"
#include "notifications.h"
#include "lvgl/src/display/lv_display_private.h"
#include "lvgl/src/misc/lv_area_private.h"
#include "lvgl/src/widgets/image/lv_image_private.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int failures, warnings, cases;
static int panel_width, panel_height;
static unsigned char *pixels;
static const char *output_dir;
static lv_obj_t *assistant_screen;
static lv_obj_t *apps_screen;
static bool fixture_long_wifi, fixture_connected, fixture_link_up;
static bool fixture_long_names;
static qz_wifi_state_t fixture_wifi_state = QZ_WIFI_IDLE;
static int fixture_skill_count = QZ_SKILL_MAX;
static int timer_delete_count;
typedef struct {
    bool active;
    bool paused;
    bool auto_break;
    char phase[16];
    int remaining_seconds;
    int completed_cycles;
    int total_cycles;
    int focus_minutes;
    int break_minutes;
    int start_count;
    int pause_count;
    int resume_count;
    int stop_count;
} layout_pomodoro_state_t;
static layout_pomodoro_state_t pomodoro_fixture = {
    .active = false,
    .paused = false,
    .auto_break = true,
    .phase = "focus",
    .remaining_seconds = 1500,
    .completed_cycles = 0,
    .total_cycles = 4,
    .focus_minutes = 25,
    .break_minutes = 5,
};
static pthread_mutex_t pomodoro_fixture_mutex = PTHREAD_MUTEX_INITIALIZER;
static qzdesk_core_event_cb_t listeners[16];
static void *listener_data[16];
static int listener_count;
static lv_obj_t *named_screens[32];
static const char *screen_names[32];
static int named_screen_count;
static qzdesk_core_event_cb_t poll_callback;
static void *poll_data;
static char backlight_fixture_path[256];
static char fixture_home_path[] = "/tmp/qzdesk-layout-home-XXXXXX";

/* No hardware, external services, saved settings or core process are changed
 * by these fixtures. Internal LVGL objects and callbacks remain unmodified. */
bool __wrap_qz_wifi_ready(void) { return true; }
bool __wrap_qz_wifi_link_up(void) { return fixture_link_up; }
bool __wrap_qz_wifi_worker_start(void) { return true; }
bool __wrap_qz_wifi_enable_async(bool enabled) { (void)enabled; return true; }
bool __wrap_qz_wifi_autoconnect_async(void) { return true; }
bool __wrap_qz_wifi_scan_async(void) { return true; }
bool __wrap_qz_wifi_scan_pending(void) { return false; }
bool __wrap_qz_wifi_is_saved_network(const char *ssid) { (void)ssid; return false; }
void __wrap_qz_wifi_interface(char *out, unsigned int size) { snprintf(out, size, "wlan0"); }
void __wrap_qz_wifi_current(char *out, unsigned int size)
{
    snprintf(out, size, "%s", fixture_connected ? "Layout test network" : "");
}
void __wrap_qz_wifi_ipv4(char *out, unsigned int size)
{
    snprintf(out, size, "%s", fixture_connected ? "192.168.100.200" : "");
}
void __wrap_qz_wifi_mac(char *out, unsigned int size) { snprintf(out, size, "AA:BB:CC:DD:EE:FF"); }
bool __wrap_qz_wifi_status(qz_wifi_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = fixture_wifi_state;
    return true;
}
int __wrap_qz_wifi_scan_results(qz_wifi_ap_t *out, int maximum)
{
    int count = maximum < 12 ? maximum : 12;
    for (int i = 0; i < count; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        if (fixture_long_wifi) {
            /* The scan buffer allows 64 bytes, but a 24-character no-space
             * SSID already wraps at 320 px and keeps the row's clickable
             * center inside the viewport. */
            memset(out[i].ssid, 'W', 24);
            out[i].ssid[24] = '\0';
        } else {
            snprintf(out[i].ssid, sizeof(out[i].ssid), "Layout WiFi %02d", i + 1);
        }
        out[i].bars = 1 + i % 4;
        out[i].locked = true;
    }
    return count;
}
bool __wrap_qz_skills_set_role(const char *id, qz_skill_role_t role)
{ (void)id; (void)role; return true; }
int __wrap_qz_skills_fetch(qz_skill_t *out, int maximum, qz_skill_summary_t *summary)
{
    int count = fixture_skill_count < maximum ? fixture_skill_count : maximum;
    memset(summary, 0, sizeof(*summary));
    summary->count = count;
    summary->primary = count > 0;
    summary->secondary = count > 1 ? count - 1 : 0;
    for (int i = 0; i < count; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        snprintf(out[i].id, sizeof(out[i].id), "layout-%02d", i);
        snprintf(out[i].name, sizeof(out[i].name), "布局验证技能 %02d", i + 1);
        if (fixture_long_names) memset(out[i].name, 'W', sizeof(out[i].name) - 1);
        snprintf(out[i].description, sizeof(out[i].description),
                 "这段说明用于检查技能详情分页。说明中的文字应完整显示，"
                 "切换后仍应可读。继续检查角色按钮和内容区域的边界。");
        out[i].role = i == 0 ? QZ_SKILL_ROLE_PRIMARY : QZ_SKILL_ROLE_SECONDARY;
    }
    return count;
}

static void pomodoro_fixture_json_locked(char *out, size_t size)
{
    snprintf(out, size,
             "{\"ok\":true,\"state\":{"
             "\"active\":%s,\"paused\":%s,\"phase\":\"%s\","
             "\"remaining_seconds\":%d,\"completed_cycles\":%d,"
             "\"total_cycles\":%d,\"focus_minutes\":%d,"
             "\"break_minutes\":%d,\"auto_break\":%s},"
             "\"message\":\"layout fixture\"}",
             pomodoro_fixture.active ? "true" : "false",
             pomodoro_fixture.paused ? "true" : "false",
             pomodoro_fixture.phase,
             pomodoro_fixture.remaining_seconds,
             pomodoro_fixture.completed_cycles,
             pomodoro_fixture.total_cycles,
             pomodoro_fixture.focus_minutes,
             pomodoro_fixture.break_minutes,
             pomodoro_fixture.auto_break ? "true" : "false");
}

static void pomodoro_fixture_snapshot(layout_pomodoro_state_t *out)
{
    pthread_mutex_lock(&pomodoro_fixture_mutex);
    *out = pomodoro_fixture;
    pthread_mutex_unlock(&pomodoro_fixture_mutex);
}

/* Tests use this to move the core straight to a phase that would otherwise take
 * minutes to reach. It changes only the fake HTTP state, never the UI model. */
static void pomodoro_fixture_set(bool active, bool paused, const char *phase,
                                 int remaining_seconds, int completed_cycles,
                                 int total_cycles, int focus_minutes,
                                 int break_minutes, bool auto_break)
{
    pthread_mutex_lock(&pomodoro_fixture_mutex);
    pomodoro_fixture.active = active;
    pomodoro_fixture.paused = paused;
    snprintf(pomodoro_fixture.phase, sizeof(pomodoro_fixture.phase), "%s", phase);
    pomodoro_fixture.remaining_seconds = remaining_seconds;
    pomodoro_fixture.completed_cycles = completed_cycles;
    pomodoro_fixture.total_cycles = total_cycles;
    pomodoro_fixture.focus_minutes = focus_minutes;
    pomodoro_fixture.break_minutes = break_minutes;
    pomodoro_fixture.auto_break = auto_break;
    pthread_mutex_unlock(&pomodoro_fixture_mutex);
}

bool __wrap_qz_web_request(const char *method, const char *path, const char *body,
                            char *out, size_t size)
{
    if (strcmp(path, "/api/companion/records") == 0) {
        if (strstr(body, "\"text\":\"失败测试\"")) {
            snprintf(out, size, "{\"error\":\"写盘失败，未保存\"}"); return false;
        }
        if (strcmp(method, "POST") == 0 && strstr(body, "\"action\":\"add\""))
            snprintf(out, size, "{\"status\":\"saved\",\"records\":[{\"id\":1,\"kind\":\"memory\",\"text\":\"喜欢简短回复\",\"done\":false}]}");
        else snprintf(out, size, "{\"records\":[{\"id\":1,\"kind\":\"memory\",\"text\":\"喜欢简短回复\",\"done\":false}]}");
        return true;
    }
    if (strcmp(path, "/api/companion/care") == 0) {
        snprintf(out, size, "{\"care\":{\"enabled\":false,\"interval_minutes\":120,\"quiet_start\":22,\"quiet_end\":8}}");
        return true;
    }
    if (strcmp(path, "/api/notifications/snooze") == 0) {
        snprintf(out, size, "{\"error\":\"保存失败\"}");
        return false;
    }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/timers") == 0) {
        snprintf(out, size, "{\"error\":\"保存失败，请重试\"}");
        return false;
    }
    if (strcmp(method, "DELETE") == 0 && strncmp(path, "/api/timers/", 12) == 0) {
        timer_delete_count++;
        snprintf(out, size, "{\"ok\":true}");
        return true;
    }
    if (strcmp(path, "/api/timers") == 0) {
        char name[32];
        memset(name, 'W', sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        size_t used = (size_t)snprintf(out, size, "{\"timers\":[");
        for (int i = 0; i < 8 && used < size; i++) {
            used += (size_t)snprintf(out + used, size - used,
                    "%s{\"id\":%d,\"hour\":%d,\"minute\":30,\"daily\":true,"
                    "\"label\":\"%s\"}", i ? "," : "", i + 1, i + 8,
                    fixture_long_names ? name : "布局提醒");
        }
        if (used < size) snprintf(out + used, size - used, "]}");
    } else if (strcmp(path, "/api/skills") == 0) {
        if (fixture_skill_count < 0) {
            snprintf(out, size, "{\"error\":\"layout fixture: skill service offline\"}");
            return false;
        }
        size_t used = (size_t)snprintf(out, size, "{\"skills\":[");
        for (int i = 0; i < fixture_skill_count && used < size; i++) {
            char name[80];
            snprintf(name, sizeof(name), "布局验证技能 %02d", i + 1);
            if (fixture_long_names) { memset(name, 'W', 60); name[60] = 0; }
            used += (size_t)snprintf(out + used, size - used,
                "%s{\"id\":\"layout-%02d\",\"name\":\"%s\",\"role\":\"%s\",\"description\":\"这段说明用于检查技能详情分页。切换后仍应可读。继续检查角色按钮和内容区域的边界。\"}",
                i ? "," : "", i, name, i ? "secondary" : "primary");
        }
        if (used < size) snprintf(out + used, size - used, "]}");
    } else if (strcmp(path, "/api/devices") == 0) {
        size_t used = (size_t)snprintf(out, size, "{\"hub\":{\"enabled\":true,\"connected\":true},\"count\":%d,\"devices\":[", QZ_DEVICE_MAX);
        for (int i = 0; i < QZ_DEVICE_MAX && used < size; i++) {
            char name[80];
            snprintf(name, sizeof(name), "客厅设备 %02d", i + 1);
            if (fixture_long_names) { memset(name, 'W', 60); name[60] = 0; }
            used += (size_t)snprintf(out + used, size - used,
                "%s{\"id\":\"layout-device-%02d\",\"name\":\"%s\",\"online\":true,\"has_state\":true,\"state\":false,\"kind\":\"light\",\"brightness\":100}",
                i ? "," : "", i, name);
        }
        if (used < size) snprintf(out + used, size - used, "]}");
    } else if (strcmp(path, "/api/pomodoro") == 0) {
        pthread_mutex_lock(&pomodoro_fixture_mutex);
        if (strcmp(method, "POST") == 0) {
            char action[16] = "";
            qz_json_string(body, "action", action, sizeof(action));
            if (strcmp(action, "start") == 0) {
                int focus_minutes = 25;
                int break_minutes = 5;
                int cycles = 4;
                bool auto_break = true;
                qz_json_int(body, "focus_minutes", &focus_minutes);
                qz_json_int(body, "break_minutes", &break_minutes);
                qz_json_int(body, "cycles", &cycles);
                qz_json_bool(body, "auto_break", &auto_break);
                if (focus_minutes < 1) focus_minutes = 1;
                if (focus_minutes > 180) focus_minutes = 180;
                if (break_minutes < 1) break_minutes = 1;
                if (break_minutes > 180) break_minutes = 180;
                if (cycles < 1) cycles = 1;
                if (cycles > 12) cycles = 12;
                pomodoro_fixture.active = true;
                pomodoro_fixture.paused = false;
                snprintf(pomodoro_fixture.phase, sizeof(pomodoro_fixture.phase), "focus");
                pomodoro_fixture.remaining_seconds = focus_minutes * 60;
                pomodoro_fixture.completed_cycles = 0;
                pomodoro_fixture.total_cycles = cycles;
                pomodoro_fixture.focus_minutes = focus_minutes;
                pomodoro_fixture.break_minutes = break_minutes;
                pomodoro_fixture.auto_break = auto_break;
                pomodoro_fixture.start_count++;
            } else if (strcmp(action, "pause") == 0 && pomodoro_fixture.active) {
                pomodoro_fixture.paused = true;
                pomodoro_fixture.pause_count++;
            } else if (strcmp(action, "resume") == 0 && pomodoro_fixture.active) {
                pomodoro_fixture.paused = false;
                pomodoro_fixture.resume_count++;
            } else if (strcmp(action, "stop") == 0) {
                pomodoro_fixture.active = false;
                pomodoro_fixture.paused = false;
                pomodoro_fixture.remaining_seconds = 0;
                pomodoro_fixture.stop_count++;
            }
        }
        pomodoro_fixture_json_locked(out, size);
        pthread_mutex_unlock(&pomodoro_fixture_mutex);
    } else snprintf(out, size, "{}");
    return true;
}
int __wrap_qz_devices_fetch(qz_device_t *out, int maximum, qz_hub_status_t *status)
{
    memset(status, 0, sizeof(*status));
    status->enabled = status->connected = true;
    int count = maximum < QZ_DEVICE_MAX ? maximum : QZ_DEVICE_MAX;
    status->declared = count;
    for (int i = 0; i < count; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        snprintf(out[i].id, sizeof(out[i].id), "layout-device-%02d", i);
        snprintf(out[i].name, sizeof(out[i].name), "客厅设备 %02d", i + 1);
        if (fixture_long_names) memset(out[i].name, 'W', sizeof(out[i].name) - 1);
        out[i].online = out[i].has_state = true;
        out[i].is_light = i % 2 == 0;
        out[i].brightness = 100;
    }
    return count;
}
bool __wrap_qz_face_camera_available(void) { return false; }
bool __wrap_qz_cam_preview_start(void) { return false; }
bool __wrap_qz_cam_preview_snapshot(char *out, size_t size)
{
    memset(out, 'W', size - 1);
    out[size - 1] = 0;
    return true;
}
int __wrap_qz_cam_preview_photos(void) { return 999999; }
bool __wrap_qzdesk_core_send_text(const char *text) { (void)text; return false; }
bool __wrap_qzdesk_core_open(void) { return true; }
bool __wrap_qzdesk_core_request_status(void) { return true; }
bool __wrap_qzdesk_core_request_weather(void) { return true; }
bool __wrap_qzdesk_core_request_performance(void) { return true; }
bool __wrap_qzdesk_core_request_bridge(void) { return true; }
void __wrap_qzdesk_core_poll(qzdesk_core_event_cb_t callback, void *data)
{
    poll_callback = callback;
    poll_data = data;
    static bool delivered;
    if (delivered) return;
    delivered = true;
    qzdesk_core_event_t event = { .type = QZDESK_CORE_EVENT_CHAT };
    snprintf(event.role, sizeof(event.role), "assistant");
    snprintf(event.text, sizeof(event.text),
             "这是布局检查中的长聊天回复。文字应该在聊天区域内换行，"
             "输入时键盘和输入框应完整显示。聊天记录可在区域内滚动查看。");
    callback(&event, data);
}
bool __wrap_qzdesk_core_subscribe(qzdesk_core_event_cb_t callback, void *data)
{
    if (listener_count >= 16) return false;
    listeners[listener_count] = callback;
    listener_data[listener_count++] = data;
    return true;
}

static bool fixture_write_int(const char *path, int value)
{
    FILE *file = fopen(path, "w");
    if (!file) return false;
    bool ok = fprintf(file, "%d\n", value) > 0;
    return fclose(file) == 0 && ok;
}

static int fixture_read_int(const char *path)
{
    FILE *file = fopen(path, "r");
    int value = -1;
    if (!file) return -1;
    if (fscanf(file, "%d", &value) != 1) value = -1;
    fclose(file);
    return value;
}

static bool backlight_fixture_init(void)
{
    char dir[] = "/tmp/qzdesk-backlight-XXXXXX";
    char maximum_path[256];
    if (!mkdtemp(dir)) return false;
    snprintf(maximum_path, sizeof(maximum_path), "%s/max_brightness", dir);
    snprintf(backlight_fixture_path, sizeof(backlight_fixture_path), "%s/brightness", dir);
    if (!fixture_write_int(maximum_path, 100) ||
        !fixture_write_int(backlight_fixture_path, 40)) return false;
    setenv("QZDESK_BACKLIGHT", backlight_fixture_path, 1);
    return true;
}

static int backlight_fixture_level(void)
{
    return fixture_read_int(backlight_fixture_path);
}

static void backlight_watch_tick(uint32_t ms)
{
    lv_tick_inc(ms);
    lv_timer_handler();
}

static void check_backlight_fade(lv_display_t *display)
{
    lv_display_trigger_activity(NULL);
    backlight_watch_tick(100);
    if (backlight_fixture_level() != 40) {
        fprintf(stderr, "BACKLIGHT_FADE fixture did not start at 40%%\n");
        failures++;
        return;
    }

    /* Put the display just past the two-minute idle threshold. The first watch
     * tick begins the fade without changing the panel in the same frame. */
    display->last_activity_time = lv_tick_get() - 120000U;
    backlight_watch_tick(100);
    if (backlight_fixture_level() != 40) {
        fprintf(stderr, "BACKLIGHT_FADE jumped on its first frame\n");
        failures++;
    }

    for (int i = 0; i < 5; i++) backlight_watch_tick(100);
    int early = backlight_fixture_level();
    if (early < 38 || early >= 40) {
        fprintf(stderr, "BACKLIGHT_FADE expected a subtle early step, got %d%%\n", early);
        failures++;
    }

    for (int i = 0; i < 45; i++) backlight_watch_tick(100);
    int middle = backlight_fixture_level();
    if (middle < 18 || middle > 24) {
        fprintf(stderr, "BACKLIGHT_FADE expected nonlinear midpoint, got %d%%\n", middle);
        failures++;
    }
    if (middle >= early) {
        fprintf(stderr, "BACKLIGHT_FADE did not decrease monotonically\n");
        failures++;
    }

    for (int i = 0; i < 50; i++) backlight_watch_tick(100);
    if (backlight_fixture_level() != 1) {
        fprintf(stderr, "BACKLIGHT_FADE did not reach sleep level: %d%%\n",
                backlight_fixture_level());
        failures++;
    }

    lv_display_trigger_activity(NULL);
    backlight_watch_tick(100);
    if (backlight_fixture_level() != 40) {
        fprintf(stderr, "BACKLIGHT_WAKE did not restore 40%%: %d%%\n",
                backlight_fixture_level());
        failures++;
    }
}

void __wrap_qz_device_ip(char *out, unsigned int size) { snprintf(out, size, "192.168.100.200"); }
void __wrap_qz_load_config(char *out, unsigned int size)
{
    if (size == 0) return;
    memset(out, 'W', size - 1);
    out[size - 1] = 0;
}

static void emit(const qzdesk_core_event_t *event)
{
    for (int i = 0; i < listener_count; i++) listeners[i](event, listener_data[i]);
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *map)
{
    int width = lv_area_get_width(area);
    for (int y = area->y1; y <= area->y2; y++) {
        memcpy(pixels + (y * panel_width + area->x1) * 3,
               map + (y - area->y1) * width * 3, (size_t)width * 3);
    }
    lv_display_flush_ready(display);
}

static bool inside(const lv_area_t *child, const lv_area_t *parent)
{
    return child->x1 >= parent->x1 && child->y1 >= parent->y1 &&
           child->x2 <= parent->x2 && child->y2 <= parent->y2;
}

/* Image objects retain their unscaled intrinsic size. Compare the transformed
 * drawing rectangle, including rotation, rather than the unscaled widget box. */
static lv_area_t drawing_area(lv_obj_t *object)
{
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    if (lv_obj_has_class(object, &lv_image_class)) {
        lv_image_t *image = (lv_image_t *)object;
        if (image->w == 0 || image->h == 0 || image->align == LV_IMAGE_ALIGN_TILE) return area;
        /* STRETCH changes widget size and derives a scale from source size.
         * Applying that scale to widget size would shrink it a second time. */
        lv_area_t source = { area.x1, area.y1,
                             area.x1 + image->w - 1, area.y1 + image->h - 1 };
        if (image->align < LV_IMAGE_ALIGN_AUTO_TRANSFORM)
            lv_area_align(&area, &source, image->align, image->offset.x, image->offset.y);
        lv_point_t pivot;
        lv_image_get_pivot(object, &pivot);
        double sx = lv_image_get_scale_x(object) / 256.0;
        double sy = lv_image_get_scale_y(object) / 256.0;
        double angle = lv_image_get_rotation(object) * 3.141592653589793 / 1800.0;
        double sine = sin(angle), cosine = cos(angle);
        double min_x = 1e9, min_y = 1e9, max_x = -1e9, max_y = -1e9;
        int w = image->w, h = image->h;
        for (int i = 0; i < 4; i++) {
            double x = ((i & 1 ? w : 0) - pivot.x) * sx;
            double y = ((i & 2 ? h : 0) - pivot.y) * sy;
            double rx = x * cosine - y * sine + pivot.x + source.x1;
            double ry = x * sine + y * cosine + pivot.y + source.y1;
            if (rx < min_x) min_x = rx;
            if (rx > max_x) max_x = rx;
            if (ry < min_y) min_y = ry;
            if (ry > max_y) max_y = ry;
        }
        area = (lv_area_t){ (int)floor(min_x), (int)floor(min_y),
                            (int)ceil(max_x) - 1, (int)ceil(max_y) - 1 };
    }
    return area;
}

static bool internally_scrolled(lv_obj_t *object)
{
    for (lv_obj_t *parent = lv_obj_get_parent(object); parent; parent = lv_obj_get_parent(parent)) {
        if (lv_obj_has_class(parent, &lv_textarea_class) ||
            lv_obj_has_class(parent, &lv_roller_class)) return true;
    }
    return false;
}

static bool content_scroll_parent(lv_obj_t *parent)
{
    return parent && (lv_obj_get_screen(parent) == assistant_screen ||
                      lv_obj_get_screen(parent) == apps_screen) &&
           lv_obj_has_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
}

/** 内容超出滚动容器不算越界：列表本来就靠滚动看剩下的部分。
 *
 *  只放过容器允许滚动的那个方向，另一半仍然照报 —— 纵向列表里横向溢出
 *  是排版问题，不是滚动。 */
static bool scroll_overflow_parent(lv_obj_t *parent, const lv_area_t *area,
                                   const lv_area_t *parent_area)
{
    if (!parent || !lv_obj_has_flag(parent, LV_OBJ_FLAG_SCROLLABLE)) return false;
    bool over_x = area->x1 < parent_area->x1 || area->x2 > parent_area->x2;
    bool over_y = area->y1 < parent_area->y1 || area->y2 > parent_area->y2;
    if (!over_x && !over_y) return false;
    lv_dir_t dir = lv_obj_get_scroll_dir(parent);
    if (over_x && !(dir & LV_DIR_HOR)) return false;
    if (over_y && !(dir & LV_DIR_VER)) return false;
    /* 真能滚才有话可说；装不下的内容一像素也不该跑出去。 */
    return lv_obj_get_scroll_top(parent) + lv_obj_get_scroll_bottom(parent) +
           lv_obj_get_scroll_left(parent) + lv_obj_get_scroll_right(parent) > 0;
}

static const char *object_text(lv_obj_t *object)
{
    return lv_obj_has_class(object, &lv_label_class) ? lv_label_get_text(object) : "";
}

static void inspect(lv_obj_t *object, const char *path)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return;
    if (!internally_scrolled(object)) {
        for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) {
            lv_obj_t *a = lv_obj_get_child(object, i);
            if (!lv_obj_has_class(a, &lv_label_class) || lv_obj_has_flag(a, LV_OBJ_FLAG_HIDDEN) ||
                !lv_label_get_text(a)[0]) continue;
            for (uint32_t j = i + 1; j < lv_obj_get_child_count(object); j++) {
                lv_obj_t *b = lv_obj_get_child(object, j);
                if (!lv_obj_has_class(b, &lv_label_class) || lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN) ||
                    !lv_label_get_text(b)[0]) continue;
                lv_area_t aa, bb, overlap;
                lv_obj_get_coords(a, &aa);
                lv_obj_get_coords(b, &bb);
                if (lv_area_intersect(&overlap, &aa, &bb) && lv_area_get_height(&overlap) > 1 &&
                    lv_area_get_width(&overlap) > 1) {
                    fprintf(stderr, "TEXT_OVERLAP %s %s / %s\n", path,
                            lv_label_get_text(a), lv_label_get_text(b));
                    failures++;
                }
            }
        }
    }
    lv_obj_t *parent = lv_obj_get_parent(object);
    lv_area_t area = drawing_area(object);
    if (parent && !internally_scrolled(object)) {
        lv_area_t parent_area;
        lv_obj_get_coords(parent, &parent_area);
        bool fits = inside(&area, &parent_area);
        if (content_scroll_parent(parent))
            fits = area.x1 >= parent_area.x1 && area.x2 <= parent_area.x2;
        else if (scroll_overflow_parent(parent, &area, &parent_area))
            fits = true;
        if (!fits) {
            fprintf(stderr, "BOUNDARY %s [%d,%d,%d,%d] parent [%d,%d,%d,%d] %s\n",
                    path, area.x1, area.y1, area.x2, area.y2,
                    parent_area.x1, parent_area.y1, parent_area.x2, parent_area.y2,
                    object_text(object));
            failures++;
        }
    }
    if (lv_obj_has_class(object, &lv_label_class) && !internally_scrolled(object)) {
        lv_label_long_mode_t mode = lv_label_get_long_mode(object);
        lv_point_t text_size;
        int width = lv_obj_get_content_width(object);
        lv_text_get_size(&text_size, lv_label_get_text(object),
                        lv_obj_get_style_text_font(object, 0),
                        lv_obj_get_style_text_letter_space(object, 0),
                        lv_obj_get_style_text_line_space(object, 0),
                        width > 0 ? width : 1, LV_TEXT_FLAG_NONE);
        if (mode == LV_LABEL_LONG_WRAP || mode == LV_LABEL_LONG_CLIP) {
            if (text_size.y > lv_obj_get_content_height(object) || text_size.x > width) {
                fprintf(stderr, "TEXT_CLIP %s text %dx%d box %dx%d %s\n", path,
                        text_size.x, text_size.y, width, lv_obj_get_content_height(object),
                        lv_label_get_text(object));
                failures++;
            }
        } else if (mode == LV_LABEL_LONG_DOT || mode == LV_LABEL_LONG_SCROLL ||
                   mode == LV_LABEL_LONG_SCROLL_CIRCULAR) {
            fprintf(stderr, "TEXT_MODE %s mode=%d %s\n", path, mode, lv_label_get_text(object));
            warnings++;
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) {
        char child_path[256];
        snprintf(child_path, sizeof(child_path), "%s/%u", path, i);
        inspect(lv_obj_get_child(object, i), child_path);
    }
}

static void screenshot(lv_obj_t *screen, const char *name)
{
    if (!output_dir) return;
    lv_screen_load(screen);
    lv_obj_invalidate(screen);
    lv_refr_now(NULL);
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", output_dir, name);
    FILE *file = fopen(path, "wb");
    if (!file) { perror(path); failures++; return; }
    fprintf(file, "P6\n%d %d\n255\n", panel_width, panel_height);
    for (int i = 0; i < panel_width * panel_height; i++) {
        unsigned char rgb[3] = { pixels[i * 3 + 2], pixels[i * 3 + 1], pixels[i * 3] };
        fwrite(rgb, sizeof(rgb), 1, file);
    }
    fclose(file);
}

/* 触控命中区审计：仅当 QZ_LAYOUT_TOUCH_AUDIT 设置时输出，避免污染常规报告。
 *
 * 判定口径是**物理像素**的 40×40 下限（见 include/scale.h）。列出来的是候选，
 * 不是失败：两类情况按设计接受，看到它们不用再改。
 *   1. 整行目标（宽度 ≥ 200 实像素）：每行都够宽，手指有富余的横向容错，
 *      例如设置页的行、Wi-Fi 列表、设备列表、提醒页的「添加提醒」。
 *   2. 堆叠滑条（间距 < 40）：同一张卡里三条滑条的**行距只有 33 实像素**，
 *      每行补到 40 会互相抢命中区；这类以整条拖动区（宽 185）为准。
 *
 * 另外这里打印的是对象**名义**命中区：LVGL 只在点在父级坐标内时才继续找子级
 * （见 lv_indev_search_obj），所以套在矮行里的小开关实际被父级裁到行高，行本身
 * 可点就够用。 */
static int touch_audit_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("QZ_LAYOUT_TOUCH_AUDIT") != NULL ? 1 : 0;
    return enabled;
}

/* 列出所有命中区偏小的可点对象（物理像素）。 */
static void audit_touch(lv_obj_t *object, const char *path)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_CLICKABLE)) {
        lv_area_t area;
        lv_obj_get_click_area(object, &area);
        int w = lv_area_get_width(&area);
        int h = lv_area_get_height(&area);
        if (w < 40 || h < 40) {
            const char *text = "";
            if (lv_obj_has_class(object, &lv_label_class)) {
                text = lv_label_get_text(object);
            } else {
                lv_obj_t *child = lv_obj_get_child(object, 0);
                for (uint32_t i = 0; child && i < lv_obj_get_child_count(object); i++) {
                    child = lv_obj_get_child(object, i);
                    if (lv_obj_has_class(child, &lv_label_class)) {
                        text = lv_label_get_text(child);
                        break;
                    }
                }
            }
            fprintf(stderr, "TOUCH_AUDIT %s [%d,%d,%d,%d] %dx%d %s\n", path,
                    area.x1, area.y1, area.x2, area.y2, w, h, text);
        }
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) {
        char child_path[256];
        snprintf(child_path, sizeof(child_path), "%s/%u", path, i);
        audit_touch(lv_obj_get_child(object, i), child_path);
    }
}

static void check(lv_obj_t *screen, const char *name)
{
    lv_screen_load(screen);
    lv_tick_inc(1);
    lv_timer_handler();
    lv_obj_update_layout(screen);
    inspect(screen, name);
    if (touch_audit_enabled()) audit_touch(screen, name);
    screenshot(screen, name);
    cases++;
}

static lv_obj_t *find_label(lv_obj_t *object, const char *text)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_has_class(object, &lv_label_class) && strcmp(lv_label_get_text(object), text) == 0)
        return object;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) {
        lv_obj_t *found = find_label(lv_obj_get_child(object, i), text);
        if (found) return found;
    }
    return NULL;
}

static void settle(void);
static lv_obj_t *find_icon_image(lv_obj_t *object, qz_icon_t icon, int32_t px);

static lv_obj_t *find_label_prefix(lv_obj_t *object, const char *prefix)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_has_class(object, &lv_label_class) &&
        strncmp(lv_label_get_text(object), prefix, strlen(prefix)) == 0)
        return object;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) {
        lv_obj_t *found = find_label_prefix(lv_obj_get_child(object, i), prefix);
        if (found) return found;
    }
    return NULL;
}

static lv_obj_t *find_clickable_user_data(lv_obj_t *object)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_CLICKABLE) && lv_obj_get_user_data(object))
        return object;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) {
        lv_obj_t *found = find_clickable_user_data(lv_obj_get_child(object, i));
        if (found) return found;
    }
    return NULL;
}

static bool activate(lv_obj_t *object)
{
    if (!object || lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN) ||
        lv_obj_has_state(object, LV_STATE_DISABLED)) return false;
    lv_obj_send_event(object, lv_obj_has_class(object, &lv_switch_class)
                              ? LV_EVENT_RELEASED : LV_EVENT_CLICKED, NULL);
    return true;
}

static lv_obj_t *pomodoro_row(lv_obj_t *panel, const char *title)
{
    lv_obj_t *label = find_label(panel, title);
    return label ? lv_obj_get_parent(label) : NULL;
}

static bool pomodoro_step(lv_obj_t *panel, const char *title, bool increase)
{
    lv_obj_t *row = pomodoro_row(panel, title);
    if (!row) return false;
    if (increase) {
        lv_obj_t *glyph = find_icon_image(row, QZ_ICON_PLUS, 15);
        return activate(glyph ? lv_obj_get_parent(glyph) : NULL);
    }
    lv_obj_t *glyph = find_label(row, LV_SYMBOL_MINUS);
    return activate(glyph ? lv_obj_get_parent(glyph) : NULL);
}

static bool pomodoro_row_value(lv_obj_t *panel, const char *title, const char *value)
{
    lv_obj_t *row = pomodoro_row(panel, title);
    return row && find_label(row, value);
}

static void pomodoro_poll_now(void)
{
    /* Five one-second applet ticks force the five-second core reconciliation. */
    for (int i = 0; i < 5; i++) {
        lv_tick_inc(1000);
        lv_timer_handler();
        settle();
    }
}

static void settle(void)
{
    for (int i = 0; i < 30; i++) {
        usleep(1000);
        lv_tick_inc(20);
        lv_timer_handler();
    }
}

static bool settle_until_label(lv_obj_t *screen, const char *text, int attempts)
{
    for (int i = 0; i < attempts; i++) {
        settle();
        if (find_label(screen, text)) {
            /* A label can appear one frame before the queued reload replaces
             * the page. Require a short stable window rather than returning on
             * the first hit; 4 x 40 ms catches that hand-off without adding a
             * full extra settle to every successful state check. */
            bool stable = true;
            for (int frame = 0; frame < 4 && stable; frame++) {
                usleep(40000);
                lv_tick_inc(40);
                lv_timer_handler();
                stable = find_label(screen, text) != NULL;
            }
            if (stable) return true;
        }
        usleep(5000);
    }
    return false;
}

/* WLAN intentionally reconnects before its first scan. Give the page the same
 * 2.5 second poll window the device has, then let its queued async rebuild run. */
static void wlan_fixture_advance_poll(void)
{
    lv_tick_inc(2600);
    lv_timer_handler();
    settle();
}

static bool click_text(lv_obj_t *screen, const char *text)
{
    lv_obj_update_layout(screen);
    lv_obj_t *fallback = NULL;
    /*
     * A screen may repeat a skill name in its overview and its list. Prefer
     * the occurrence whose hit test resolves to an actual control; otherwise a
     * decorative label wins the traversal and the click lands on the canvas.
     */
    lv_obj_t *stack[256];
    int top = 0;
    stack[top++] = screen;
    while (top > 0) {
        lv_obj_t *candidate = stack[--top];
        if (lv_obj_has_flag(candidate, LV_OBJ_FLAG_HIDDEN)) continue;
        if (lv_obj_has_class(candidate, &lv_label_class) &&
            strcmp(lv_label_get_text(candidate), text) == 0) {
            lv_area_t area;
            lv_obj_get_coords(candidate, &area);
            lv_point_t point = { (area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2 };
            lv_obj_t *hit = lv_indev_search_obj(screen, &point);
            if (!fallback) fallback = hit;
            if (hit && hit != screen && lv_obj_has_flag(hit, LV_OBJ_FLAG_CLICKABLE)) {
                fallback = hit;
                break;
            }
        }
        uint32_t count = lv_obj_get_child_count(candidate);
        for (uint32_t i = 0; i < count && top < (int)(sizeof(stack) / sizeof(stack[0])); i++)
            stack[top++] = lv_obj_get_child(candidate, i);
    }
    lv_obj_t *object = fallback;
    if (object && object != screen) {
        lv_area_t area;
        lv_obj_get_coords(object, &area);
        lv_point_t point = { (area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2 };
        object = lv_indev_search_obj(screen, &point);
    }
    if (!object || lv_obj_has_state(object, LV_STATE_DISABLED)) return false;
    lv_obj_send_event(object, LV_EVENT_CLICKED, NULL);
    settle();
    return true;
}

static lv_obj_t *find_type(lv_obj_t *object, const lv_obj_class_t *type)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_has_class(object, type)) return object;
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) {
        lv_obj_t *found = find_type(lv_obj_get_child(object, i), type);
        if (found) return found;
    }
    return NULL;
}

static int find_types(lv_obj_t *object, const lv_obj_class_t *type,
                      lv_obj_t **out, int maximum)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return 0;
    int count = 0;
    if (lv_obj_has_class(object, type)) {
        if (count < maximum) out[count] = object;
        count++;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) {
        int child_count = find_types(lv_obj_get_child(object, i), type,
                                     out + (count < maximum ? count : 0),
                                     count < maximum ? maximum - count : 0);
        count += child_count;
    }
    return count;
}

static bool activate_label_parent(lv_obj_t *screen, const char *text)
{
    lv_obj_t *label = find_label(screen, text);
    lv_obj_t *parent = label ? lv_obj_get_parent(label) : NULL;
    if (!parent || !activate(parent)) return false;
    settle();
    return true;
}

static lv_obj_t *find_icon_image(lv_obj_t *object, qz_icon_t icon, int32_t px)
{
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) return NULL;
    if (lv_obj_has_class(object, &lv_image_class) &&
        lv_image_get_src(object) == qz_icon(icon, qz_scale_px(px))) {
        return object;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(object); i++) {
        lv_obj_t *found = find_icon_image(lv_obj_get_child(object, i), icon, px);
        if (found) return found;
    }
    return NULL;
}

static bool click_icon_button(lv_obj_t *screen, qz_icon_t icon, int32_t px)
{
    lv_obj_t *glyph = find_icon_image(screen, icon, px);
    if (!glyph) return false;
    lv_obj_t *button = lv_obj_get_parent(glyph);
    if (!button || lv_obj_has_state(button, LV_STATE_DISABLED)) return false;
    lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
    settle();
    return true;
}

static bool open_assistant_menu(lv_obj_t *screen, const char *case_name)
{
    if (click_icon_button(screen, QZ_ICON_LIST, qz_design_px(24))) {
        lv_obj_update_layout(lv_layer_top());
        if (find_label(lv_layer_top(), "新建聊天")) return true;
    }
    fprintf(stderr, "MISSING_ACTION %s trigger=more\n", case_name);
    failures++;
    return false;
}

static void name_screen(lv_obj_t *screen, const char *name)
{
    if (!screen) {
        fprintf(stderr, "MISSING_SCREEN %s\n", name);
        failures++;
        return;
    }
    if (named_screen_count >= 32) return;
    named_screens[named_screen_count] = screen;
    screen_names[named_screen_count++] = name;
}

static lv_obj_t *find_screen(lv_display_t *display, const char *title)
{
    for (uint32_t i = 0; i < display->screen_cnt; i++) {
        lv_obj_t *screen = display->screens[i];
        if (lv_obj_get_child_count(screen) && find_label(lv_obj_get_child(screen, 0), title)) return screen;
    }
    return NULL;
}

static void check_pages(lv_obj_t *screen, const char *prefix)
{
    for (int page = 0; page < 16 && click_text(screen, "上一页"); page++) {}
    for (int page = 0; page < 16; page++) {
        char name[80];
        snprintf(name, sizeof(name), "%s-page-%02d", prefix, page + 1);
        check(screen, name);
        if (!click_text(screen, "下一页")) return;
    }
    fprintf(stderr, "PAGINATION %s did not terminate after 16 pages\n", prefix);
    failures++;
}

static bool require_click(lv_obj_t *screen, const char *caption, const char *case_name);

static void check_apps_scroll(lv_obj_t *screen)
{
    lv_obj_t *scroll = lv_obj_get_child(screen, 1);
    lv_obj_update_layout(screen);
    lv_obj_t *stage = scroll ? lv_obj_get_child(scroll, 0) : NULL;
    if (!scroll || !lv_obj_has_flag(scroll, LV_OBJ_FLAG_SCROLLABLE) ||
        lv_obj_get_scroll_dir(scroll) != LV_DIR_VER || !stage ||
        lv_obj_get_child_count(stage) != 7) {
        fprintf(stderr, "APPS_SCROLL expected featured workspace plus six utilities in a vertical scroll area\n");
        failures++;
        return;
    }
    lv_screen_load(screen);
    for (int bottom = 0; bottom < 2; bottom++) {
        lv_obj_scroll_to_y(scroll, bottom ? lv_obj_get_scroll_bottom(scroll) : 0, LV_ANIM_OFF);
        lv_obj_update_layout(screen);
        lv_area_t viewport;
        lv_obj_get_coords(scroll, &viewport);
        int visible = 0;
        for (uint32_t i = 0; i < 7; i++) {
            lv_area_t tile;
            lv_obj_get_coords(lv_obj_get_child(stage, i), &tile);
            if (inside(&tile, &viewport)) visible++;
        }
        int expected_visible = lv_obj_get_width(screen) <= 320 ? 3 : (bottom ? 6 : 5);
        if (visible != expected_visible || lv_obj_get_scroll_left(scroll) > 0 || lv_obj_get_scroll_right(scroll) > 0) {
            fprintf(stderr, "APPS_SCROLL %s visible=%d, expected %d without horizontal overflow\n",
                    bottom ? "bottom" : "top", visible, expected_visible);
            failures++;
        }
        check(screen, bottom ? "apps-scroll-bottom" : "apps-scroll-top");
    }
    require_click(screen, "设置", "apps-open-settings");
    lv_obj_scroll_to_y(scroll, 0, LV_ANIM_OFF);
}

static bool open_dialog(lv_obj_t *screen, const char *caption, const char *name)
{
    uint32_t before = lv_obj_get_child_count(screen);
    if (!click_text(screen, caption) || lv_obj_get_child_count(screen) <= before) {
        fprintf(stderr, "MISSING_DIALOG %s trigger=%s\n", name, caption);
        failures++;
        return false;
    }
    check(screen, name);
    return true;
}

static lv_obj_t *open_label_row_modal(lv_obj_t *screen, const char *caption)
{
    uint32_t before = lv_obj_get_child_count(screen);
    if (!activate_label_parent(screen, caption) ||
        lv_obj_get_child_count(screen) <= before) return NULL;
    return lv_obj_get_child(screen, -1);
}

static bool require_click(lv_obj_t *screen, const char *caption, const char *case_name)
{
    if (click_text(screen, caption)) return true;
    fprintf(stderr, "MISSING_ACTION %s trigger=%s\n", case_name, caption);
    failures++;
    return false;
}

static void emit_ascii_chat(const char *role)
{
    if (!poll_callback) {
        fprintf(stderr, "MISSING_CALLBACK assistant core poll\n");
        failures++;
        return;
    }
    qzdesk_core_event_t event = { .type = QZDESK_CORE_EVENT_CHAT };
    snprintf(event.role, sizeof(event.role), "%s", role);
    memset(event.text, 'W', sizeof(event.text) - 1);
    poll_callback(&event, poll_data);
}

/* 识别结果事件：界面据此把"我听到：…"写进字幕。 */
static void emit_asr(const char *text)
{
    if (!poll_callback) {
        fprintf(stderr, "MISSING_CALLBACK assistant core poll\n");
        failures++;
        return;
    }
    qzdesk_core_event_t event = { .type = QZDESK_CORE_EVENT_ASR };
    snprintf(event.text, sizeof(event.text), "%s", text);
    poll_callback(&event, poll_data);
}

static void emit_bridge(bool ok, const char *text)
{
    if (!poll_callback) {
        fprintf(stderr, "MISSING_CALLBACK assistant core poll\n");
        failures++;
        return;
    }
    qzdesk_core_event_t event = { .type = QZDESK_CORE_EVENT_BRIDGE, .ok = ok };
    snprintf(event.text, sizeof(event.text), "%s", text ? text : "");
    poll_callback(&event, poll_data);
}

static void emit_state(int state)
{
    if (!poll_callback) {
        fprintf(stderr, "MISSING_CALLBACK assistant core poll\n");
        failures++;
        return;
    }
    qzdesk_core_event_t event = { .type = QZDESK_CORE_EVENT_STATUS, .state = state };
    poll_callback(&event, poll_data);
}

static void check_weather_states(lv_obj_t *desktop, lv_obj_t *weather)
{
    if (!weather) return;
    qzdesk_core_event_t event = { .type = QZDESK_CORE_EVENT_WEATHER };
    /* 0 是合法值（= 正在下雨）：先摆成"没有临近降水"，各用例再各自覆盖。 */
    event.weather.near_precipitation_minutes = -1;
    event.weather.near_precipitation_probability = -1;
    snprintf(event.weather.status, sizeof(event.weather.status), "error");
    snprintf(event.weather.city, sizeof(event.weather.city), "呼和浩特");
    snprintf(event.weather.message, sizeof(event.weather.message), "天气服务未连接，请稍后重试");
    emit(&event);
    check(desktop, "desktop-weather-error");
    check(weather, "weather-error");
    snprintf(event.weather.status, sizeof(event.weather.status), "offline");
    snprintf(event.weather.message, sizeof(event.weather.message), "WiFi 未连接，天气暂不可用");
    emit(&event);
    check(desktop, "desktop-weather-offline");
    check(weather, "weather-offline");
    snprintf(event.weather.status, sizeof(event.weather.status), "ok");
    memset(event.weather.city, 'W', sizeof(event.weather.city) - 1);
    event.weather.city[sizeof(event.weather.city) - 1] = 0;
    snprintf(event.weather.text, sizeof(event.weather.text), "多云转晴");
    snprintf(event.weather.icon, sizeof(event.weather.icon), "cloud");
    snprintf(event.weather.updated, sizeof(event.weather.updated), "18:30");
    event.weather.has_data = true;
    event.weather.temperature = -18;
    event.weather.apparent = -24;
    event.weather.high = -12;
    event.weather.low = -28;
    event.weather.humidity = 100;
    event.weather.wind_x10 = 238;
    emit(&event);
    check(desktop, "desktop-weather-long-city");
    check(weather, "weather-long-city");
    snprintf(event.weather.city, sizeof(event.weather.city), "内蒙古自治区呼和浩特");
    emit(&event);
    check(desktop, "desktop-weather-long-cjk-city");
    check(weather, "weather-long-cjk-city");
    /* 临近降水优先：描述行要换成长文案，最宽的那种（两位数分钟 + 百分比）。 */
    event.weather.near_precipitation_minutes = 45;
    event.weather.near_precipitation_probability = 100;
    emit(&event);
    check(desktop, "desktop-weather-imminent-rain");
}

static void seed_events(void)
{
    qzdesk_core_event_t event = { .type = QZDESK_CORE_EVENT_WEATHER };
    event.weather.near_precipitation_minutes = -1;
    event.weather.near_precipitation_probability = -1;
    snprintf(event.weather.status, sizeof(event.weather.status), "ok");
    snprintf(event.weather.city, sizeof(event.weather.city), "呼和浩特");
    snprintf(event.weather.text, sizeof(event.weather.text), "多云转晴");
    snprintf(event.weather.icon, sizeof(event.weather.icon), "cloud");
    snprintf(event.weather.updated, sizeof(event.weather.updated), "18:30");
    event.weather.has_data = true;
    event.weather.temperature = -18;
    event.weather.apparent = -24;
    event.weather.high = -12;
    event.weather.low = -28;
    event.weather.humidity = 100;
    event.weather.wind_x10 = 238;
    emit(&event);
    memset(&event, 0, sizeof(event));
    event.type = QZDESK_CORE_EVENT_PERFORMANCE;
    event.performance.timestamp = (unsigned long long)time(NULL);
    event.performance.cpu_x10 = 987;
    event.performance.cpu_level = QZDESK_LEVEL_CRITICAL;
    event.performance.core_count = QZDESK_PERFORMANCE_CORES;
    event.performance.memory_x10 = 925;
    event.performance.storage_x10 = 888;
    event.performance.memory_total_kb = 1024 * 1024;
    event.performance.memory_used_kb = 512 * 1024;
    event.performance.storage_total_kb = 32ULL * 1024 * 1024;
    event.performance.storage_used_kb = 28ULL * 1024 * 1024;
    event.performance.has_temperature = true;
    event.performance.temperature_x10 = 999;
    event.performance.has_wifi = true;
    event.performance.wifi_dbm = -99;
    event.performance.uptime_secs = 123456789;
    event.performance.process_count = QZDESK_PERFORMANCE_PROCESSES;
    for (int i = 0; i < QZDESK_PERFORMANCE_CORES; i++) event.performance.core_x10[i] = 999;
    for (int i = 0; i < QZDESK_PERFORMANCE_PROCESSES; i++)
        snprintf(event.performance.process_text[i], sizeof(event.performance.process_text[i]),
                 "服务进程 %d · qzdesk_core · CPU 99.9%% · 内存 512 MB", i);
    emit(&event);
}

int main(int argc, char **argv)
{
    fixture_long_names = getenv("QZ_LAYOUT_LONG_NAMES") != NULL;
    panel_width = argc > 1 ? atoi(argv[1]) : 320;
    panel_height = argc > 2 ? atoi(argv[2]) : 240;
    output_dir = argc > 3 ? argv[3] : NULL;
    if (panel_width < 320 || panel_height < 240) return 2;
    setenv("QZDESK_REDUCE_MOTION", "1", 1);
    qz_scale_init(panel_width, panel_height);
    lv_init();
    lv_display_t *display = lv_display_create(panel_width, panel_height);
    pixels = calloc((size_t)panel_width * panel_height, 3);
    void *draw_buffer = calloc((size_t)panel_width * panel_height, 3);
    if (!pixels || !draw_buffer) return 2;
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB888);
    lv_display_set_buffers(display, draw_buffer, NULL, (uint32_t)panel_width * panel_height * 3,
                            LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    qz_font_init();
    qz_style_init();
    lv_theme_default_init(display, qz_color(QZ_ACCENT), qz_color(QZ_ACCENT_DARK), false, qz_font());
    lv_obj_t *desktop = qz_desktop_create("ws://192.168.100.200:8080");
    lv_obj_t *apps = qz_apps_create();
    apps_screen = apps;
    assistant_screen = qz_assistant_create();
    lv_obj_t *settings = qz_settings_create();
    lv_obj_t *skills = qz_skill_page_create(apps);
    lv_obj_t *performance = qz_performance_create(apps);
    qz_desktop_set_assistant(assistant_screen);
    qz_desktop_set_apps(apps);
    qz_desktop_set_settings(settings);
    qz_apps_set_desktop(desktop);
    qz_apps_set_skill_screen(skills);
    qz_apps_set_performance_screen(performance);
    qz_apps_set_settings_screen(settings);
    qz_applets_init(apps);
    qz_assistant_set_desktop(desktop);
    qz_settings_set_desktop(desktop);
    name_screen(desktop, "desktop");
    name_screen(apps, "apps");
    name_screen(assistant_screen, "assistant");
    name_screen(settings, "settings");
    name_screen(skills, "skills");
    name_screen(performance, "performance");
    name_screen(find_screen(display, "WLAN"), "wifi");
    name_screen(find_screen(display, "通用设置"), "general");
    name_screen(find_screen(display, "关于"), "about");
    name_screen(find_screen(display, "桌面气象站"), "weather");
    static const char *const applet_names[] = {
        "status", "reminders", "pomodoro", "control", "presence", "camera"
    };
    for (int i = 0; i < QZ_APPLET_COUNT; i++)
        name_screen(qz_applets_screen((qz_applet_t)i), applet_names[i]);
    seed_events();
    /* Enumerate all created pages, including private settings/weather screens. */
    for (uint32_t i = 0; i < display->screen_cnt; i++) {
        lv_obj_t *screen = display->screens[i];
        if (!lv_obj_get_child_count(screen)) continue;
        char name[64];
        snprintf(name, sizeof(name), "screen-%02u", i);
        for (int j = 0; j < named_screen_count; j++)
            if (named_screens[j] == screen) snprintf(name, sizeof(name), "%s", screen_names[j]);
        lv_screen_load(screen);
        settle();
        check(screen, name);
    }
    lv_screen_load(apps);
    lv_obj_t *clock_grid = lv_obj_get_child(apps, 1);
    lv_obj_update_layout(apps);
    lv_obj_scroll_to_y(clock_grid, lv_obj_get_scroll_bottom(clock_grid), LV_ANIM_OFF);
    require_click(apps, "时钟与天气", "apps-open-clock");
    settle();
    lv_obj_t *clock_screen = lv_screen_active();
    if (clock_screen == desktop || !find_label(clock_screen, "桌面气象站")) failures++;
    check(clock_screen, "desktop-clock-weather-station");
    if (!click_icon_button(clock_screen, QZ_ICON_CHEVRON_LEFT, qz_design_px(QZ_TOUCH_BACK / 2)) ||
        lv_screen_active() != apps) {
        fprintf(stderr, "FAIL desktop-clock-weather-return\n");
        failures++;
    }
    check(apps, "desktop-clock-weather-return");
    lv_screen_load(performance);
    settle();
    seed_events();
    check(performance, "performance-data");
    static const char *const performance_tabs[] = { "概览", "核心", "资源", "状态", "进程" };
    for (int i = 0; i < 5; i++) {
        char name[64];
        snprintf(name, sizeof(name), "performance-tab-%d", i + 1);
        require_click(performance, performance_tabs[i], name);
        check(performance, name);
        if (i == 1) {
            for (int core = 0; core < QZDESK_PERFORMANCE_CORES; core++) {
                char label[32];
                snprintf(label, sizeof(label), "%d 99%%", core);
                if (!find_label(performance, label)) {
                    fprintf(stderr, "MISSING_DATA %s core=%d\n", name, core);
                    failures++;
                }
            }
        }
    }
    check_weather_states(desktop, find_screen(display, "桌面气象站"));
    check_apps_scroll(apps);
    lv_obj_t *control = qz_applets_screen(QZ_APPLET_CONTROL);
    require_click(control, "局域网设备", "control-devices");
    check(control, "control-devices");
    for (int i = 0; i < QZ_DEVICE_MAX; i++) {
        char name[40];
        snprintf(name, sizeof(name), "control-device-%02d", i + 1);
        check(control, name);
        if (i + 1 < QZ_DEVICE_MAX) require_click(control, "下一台", name);
    }
    require_click(control, "本机", "control-local-return");
    check(control, "control-local-return");
    lv_screen_load(assistant_screen);
    check(assistant_screen, "assistant-empty");
    if (open_assistant_menu(assistant_screen, "assistant-menu")) {
        check(assistant_screen, "assistant-menu");
        const char *menu_items[] = { "新建聊天", "本地记忆", "停止回复", "表情模式" };
        for (int i = 0; i < 4; i++) {
            lv_obj_t *label = find_label(lv_layer_top(), menu_items[i]);
            lv_obj_t *row = label ? lv_obj_get_parent(label) : NULL;
            lv_area_t area;
            if (!row) {
                fprintf(stderr, "MISSING_MENU_ITEM assistant-menu %s\n", menu_items[i]);
                failures++;
                continue;
            }
            lv_obj_get_coords(row, &area);
            if (lv_area_get_height(&area) < QZ_MIN_TOUCH_PX) {
                fprintf(stderr, "TOUCH_MENU assistant-menu %s height=%d\n",
                        menu_items[i], lv_area_get_height(&area));
                failures++;
            }
        }
        if (!click_text(lv_layer_top(), "表情模式")) {
            fprintf(stderr, "MISSING_ACTION assistant-face trigger=mode-menu\n");
            failures++;
        }
        if (find_label(lv_layer_top(), "新建聊天")) {
            fprintf(stderr, "MENU_LEFT_OPEN assistant-face\n");
            failures++;
        }
    }
    check(assistant_screen, "assistant-face");
    if (open_assistant_menu(assistant_screen, "assistant-chat")) {
        if (!click_text(lv_layer_top(), "聊天模式")) {
            fprintf(stderr, "MISSING_ACTION assistant-chat trigger=mode-menu\n");
            failures++;
        }
        if (find_label(lv_layer_top(), "新建聊天")) {
            fprintf(stderr, "MENU_LEFT_OPEN assistant-chat\n");
            failures++;
        }
    }
    lv_tick_inc(1000);
    settle();
    check(assistant_screen, "assistant-chat");
    if (open_assistant_menu(assistant_screen, "assistant-memory")) {
        if (!click_text(lv_layer_top(), "本地记忆")) {
            fprintf(stderr, "MISSING_ACTION assistant-memory trigger=menu\n");
            failures++;
        }
    }
    lv_obj_t *memory_page = lv_screen_active();
    check(memory_page, "assistant-memory");
    /* 设备上没有中文输入法，记忆页不再提供输入框：应该一个 textarea 都没有，
     * 只有"回对话页说"的说明和入口。 */
    if (find_type(memory_page, &lv_textarea_class) || find_type(memory_page, &lv_keyboard_class))
        failures++;
    check(memory_page, "assistant-memory-readonly");
    lv_screen_load(assistant_screen);
    emit_ascii_chat("assistant");
    check(assistant_screen, "assistant-ascii-reply");
    emit_ascii_chat("user");
    check(assistant_screen, "assistant-ascii-user");
    lv_obj_t *input = find_type(assistant_screen, &lv_textarea_class);
    if (input) {
        lv_obj_send_event(input, LV_EVENT_FOCUSED, NULL);
        check(assistant_screen, "assistant-keyboard");
        lv_obj_send_event(input, LV_EVENT_DEFOCUSED, NULL);
        check(assistant_screen, "assistant-keyboard-closed");
        lv_textarea_set_text(input, "发送失败时保留这份草稿");
        lv_obj_send_event(input, LV_EVENT_READY, NULL);
        if (strcmp(lv_textarea_get_text(input), "发送失败时保留这份草稿") != 0) {
            fprintf(stderr, "DRAFT_LOST transport failure\n");
            failures++;
        }
        check(assistant_screen, "assistant-send-failed");
        lv_textarea_set_text(input, "");
    }
    else {
        fprintf(stderr, "MISSING_INPUT assistant-keyboard\n");
        failures++;
    }
    if (open_assistant_menu(assistant_screen, "assistant-face-return")) {
        if (!click_text(lv_layer_top(), "表情模式")) {
            fprintf(stderr, "MISSING_ACTION assistant-face-return trigger=mode-menu\n");
            failures++;
        }
    }
    check(assistant_screen, "assistant-face-return");
    /* 语音闭环：回复中语音键换成"打断重说"；识别结果写进字幕，长句子也不许
     * 压到下面的语音键上。两条都只看表情模式 —— 聊天模式下识别结果是气泡。 */
    emit_state(6);
    if (!find_label(assistant_screen, "打断重说")) {
        fprintf(stderr, "MISSING_LABEL assistant-face-interrupt\n");
        failures++;
    }
    check(assistant_screen, "assistant-face-interrupt");
    emit_asr("把明天下午三点的会改到四点，另外提醒我带充电器");
    check(assistant_screen, "assistant-face-heard");
    emit_state(3);
    if (open_assistant_menu(assistant_screen, "assistant-chat-return")) {
        if (!click_text(lv_layer_top(), "聊天模式")) {
            fprintf(stderr, "MISSING_ACTION assistant-chat-return trigger=mode-menu\n");
            failures++;
        }
    }
    check(assistant_screen, "assistant-chat-return");

    /* 手机输入桥：输入栏里的手机键要能打开二维码；核心成功时画码，失败时
     * 原样显示原因。两条都要在关闭后不把浮层留给后续用例。 */
    lv_obj_t *bridge_icon = find_icon_image(assistant_screen, QZ_ICON_DEVICE_MOBILE,
                                            qz_design_px(34));
    if (!bridge_icon) {
        fprintf(stderr, "MISSING_ACTION assistant-bridge trigger=phone\n");
        failures++;
    } else {
        lv_obj_send_event(lv_obj_get_parent(bridge_icon), LV_EVENT_CLICKED, NULL);
        settle();
        if (!find_label(lv_layer_top(), "手机输入")) {
            fprintf(stderr, "MISSING_BRIDGE assistant-bridge-open\n");
            failures++;
        }
        emit_bridge(true, "http://192.168.100.200:8080/#chat");
        settle();
        lv_obj_update_layout(lv_layer_top());
        if (!find_type(lv_layer_top(), &lv_qrcode_class) ||
            !find_label(lv_layer_top(), "手机扫码，打开对话页直接在手机上打字") ||
            !find_label(lv_layer_top(), "http://192.168.100.200:8080/#chat")) {
            fprintf(stderr, "MISSING_BRIDGE assistant-bridge-url\n");
            failures++;
        }
        inspect(lv_layer_top(), "assistant-bridge");
        check(assistant_screen, "assistant-bridge");
        if (!click_text(lv_layer_top(), "关闭")) {
            fprintf(stderr, "MISSING_BRIDGE assistant-bridge-close\n");
            failures++;
        }
        if (find_label(lv_layer_top(), "手机输入")) {
            fprintf(stderr, "BRIDGE_LEFT_OPEN assistant-bridge-close\n");
            failures++;
        }

        lv_obj_send_event(lv_obj_get_parent(bridge_icon), LV_EVENT_CLICKED, NULL);
        settle();
        emit_bridge(false, "设备当前没有局域网地址，请先连接 WiFi");
        settle();
        lv_obj_update_layout(lv_layer_top());
        if (find_type(lv_layer_top(), &lv_qrcode_class) ||
            !find_label(lv_layer_top(), "设备当前没有局域网地址，请先连接 WiFi")) {
            fprintf(stderr, "MISSING_BRIDGE assistant-bridge-error\n");
            failures++;
        }
        inspect(lv_layer_top(), "assistant-bridge-error");
        check(assistant_screen, "assistant-bridge-error");
        if (!click_text(lv_layer_top(), "关闭")) {
            fprintf(stderr, "MISSING_BRIDGE assistant-bridge-error-close\n");
            failures++;
        }
    }

    input = find_type(assistant_screen, &lv_textarea_class);
    if (input) {
        lv_obj_send_event(input, LV_EVENT_FOCUSED, NULL);
        check(assistant_screen, "assistant-keyboard-return");
        lv_obj_send_event(input, LV_EVENT_DEFOCUSED, NULL);
    }
    check_pages(skills, "skills");
    if (click_text(skills, "陪伴")) {
        inspect(lv_layer_top(), "lcd-companion-1");
        check(skills, "lcd-companion-1");
        if (!find_label(lv_layer_top(), "个人记忆") || !find_label(lv_layer_top(), "专注搭子")) failures++;
        if (!click_text(lv_layer_top(), "换一页")) failures++;
        inspect(lv_layer_top(), "lcd-companion-2");
        check(skills, "lcd-companion-2");
        if (!find_label(lv_layer_top(), "学习教练") || !find_label(lv_layer_top(), "设备管家")) failures++;
        if (input) {
            lv_textarea_set_text(input, "已有草稿");
            click_text(lv_layer_top(), "睡前故事");
            if (strcmp(lv_textarea_get_text(input), "已有草稿") != 0 ||
                !find_label(lv_layer_top(), "请先处理聊天草稿或当前录音")) failures++;
            lv_textarea_set_text(input, "");
        }
        if (!click_text(lv_layer_top(), "睡前故事")) failures++;
        if (!input || strcmp(lv_textarea_get_text(input), "给我讲一个三分钟的睡前故事") != 0) failures++;
        check(assistant_screen, "lcd-companion-draft");
        if (input) lv_textarea_set_text(input, "");
    } else failures++;
    click_text(skills, "陪伴");
    click_text(lv_layer_top(), "个人记忆");
    if (!find_label(lv_layer_top(), "喜欢简短回复")) failures++;
    inspect(lv_layer_top(), "lcd-memory-live"); check(skills, "lcd-memory-live");
    lv_obj_t *record_input = find_type(lv_layer_top(), &lv_textarea_class);
    if (record_input) {
        lv_textarea_set_text(record_input, "喜欢简短回复");
        click_text(lv_layer_top(), "新增");
        lv_obj_send_event(record_input, LV_EVENT_FOCUSED, NULL);
        inspect(lv_layer_top(), "lcd-memory-keyboard"); check(skills, "lcd-memory-keyboard");
        lv_obj_send_event(record_input, LV_EVENT_DEFOCUSED, NULL);
        lv_textarea_set_text(record_input, "失败测试"); click_text(lv_layer_top(), "新增");
        if (strcmp(lv_textarea_get_text(record_input), "失败测试") != 0 || !find_label(lv_layer_top(), "写盘失败，未保存")) failures++;
        inspect(lv_layer_top(), "lcd-memory-save-failed"); check(skills, "lcd-memory-save-failed");
    } else failures++;
    click_text(lv_layer_top(), "返回");
    click_text(skills, "陪伴"); click_text(lv_layer_top(), "主动关心");
    if (!find_label(lv_layer_top(), "主动关心：已关闭")) failures++;
    inspect(lv_layer_top(), "lcd-care-live"); check(skills, "lcd-care-live");
    click_text(lv_layer_top(), "返回");
    for (int i = 0; i < 16 && click_text(skills, "上一页"); i++) {}
    char skill_name[QZ_SKILL_NAME_MAX];
    memset(skill_name, 'W', sizeof(skill_name) - 1);
    skill_name[sizeof(skill_name) - 1] = 0;
    if (open_dialog(skills, fixture_long_names ? skill_name : "布局验证技能 01", "skill-details")) {
        click_text(skills, "下段说明");
        check(skills, "skill-details-next");
        click_text(skills, "取消");
    }
    lv_obj_t *reminders = qz_applets_screen(QZ_APPLET_REMINDER);
    check_pages(reminders, "reminders");
    if (open_dialog(reminders, "添加提醒", "reminder-add")) {
        lv_obj_t *overlay = lv_obj_get_child(reminders, -1);
        click_text(reminders, "确定");
        for (int i = 0; i < 50; i++) {
            usleep(1000);
            lv_tick_inc(20);
            lv_timer_handler();
        }
        if (!lv_obj_is_valid(overlay)) {
            fprintf(stderr, "FORM_LOST reminder save failure\n");
            failures++;
        }
        check(reminders, "reminder-save-failed");
        click_text(reminders, "取消");
    }

    lv_tick_inc(8100);
    lv_timer_handler();
    for (int i = 0; i < 8 && click_text(reminders, "上一页"); i++) {}
    if (open_dialog(reminders, "08:30", "reminder-edit")) {
        click_text(reminders, "取消");
    }
    {
        lv_obj_t *delete_button = find_clickable_user_data(reminders);
        if (!delete_button) {
            fprintf(stderr, "MISSING_ACTION reminder-delete\n");
            failures++;
        } else {
            lv_area_t area;
            lv_obj_send_event(delete_button, LV_EVENT_CLICKED, NULL);
            settle();
            if (!find_label(reminders, "再点一次删除可确认（5秒内）")) {
                fprintf(stderr, "REMINDER_DELETE confirmation banner missing\n");
                failures++;
            }
            lv_obj_get_coords(delete_button, &area);
            lv_point_t point = { (area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2 };
            lv_obj_t *hit = lv_indev_search_obj(reminders, &point);
            if (!hit || !lv_obj_has_flag(hit, LV_OBJ_FLAG_CLICKABLE)) {
                fprintf(stderr, "REMINDER_DELETE confirmation hit blocked\n");
                failures++;
            } else {
                lv_obj_send_event(hit, LV_EVENT_CLICKED, NULL);
                settle();
                if (timer_delete_count != 1) {
                    fprintf(stderr, "REMINDER_DELETE second tap did not delete\n");
                    failures++;
                }
            }
        }
    }
    lv_obj_t *pomodoro = qz_applets_screen(QZ_APPLET_POMODORO);
    lv_screen_load(pomodoro);
    settle();
    if (!find_label(pomodoro, "25:00") || find_label(pomodoro, "00:00") ||
        !find_label(pomodoro, "准备专注") || !find_label(pomodoro, "开始专注") ||
        !find_label(pomodoro, "已完成 0 / 4 轮") ||
        !find_label(pomodoro, "开始后计算结束时刻")) {
        fprintf(stderr, "POMODORO_IDLE expected 25:00 focus defaults\n");
        failures++;
    }
    check(pomodoro, "pomodoro-idle");

    if (!click_icon_button(pomodoro, QZ_ICON_SETTINGS, 16)) {
        fprintf(stderr, "MISSING_ACTION pomodoro-settings trigger=header\n");
        failures++;
    } else {
        lv_obj_t *panel = pomodoro;
        if (!find_label(panel, "节律编辑") || !find_label(panel, "专注分钟") ||
            !find_label(panel, "休息分钟") || !find_label(panel, "轮次") ||
            !find_label(panel, "自动开始休息")) {
            fprintf(stderr, "MISSING_PANEL pomodoro-settings\n");
            failures++;
        }
        lv_obj_t *auto_switch = find_type(panel, &lv_switch_class);
        if (!auto_switch || !lv_obj_has_state(auto_switch, LV_STATE_CHECKED)) {
            fprintf(stderr, "POMODORO_SETTINGS switch should start checked\n");
            failures++;
        }
        inspect(lv_layer_top(), "pomodoro-settings");
        check(pomodoro, "pomodoro-settings");

        for (int i = 0; i < 190; i++) pomodoro_step(panel, "专注分钟", true);
        for (int i = 0; i < 190; i++) pomodoro_step(panel, "休息分钟", true);
        for (int i = 0; i < 20; i++) pomodoro_step(panel, "轮次", true);
        if (!pomodoro_row_value(panel, "专注分钟", "180") ||
            !pomodoro_row_value(panel, "休息分钟", "180") ||
            !pomodoro_row_value(panel, "轮次", "12")) {
            fprintf(stderr, "POMODORO_STEPPER upper bounds not clamped\n");
            failures++;
        }
        inspect(lv_layer_top(), "pomodoro-settings-limits");
        check(pomodoro, "pomodoro-settings-limits");
        for (int i = 0; i < 190; i++) pomodoro_step(panel, "专注分钟", false);
        for (int i = 0; i < 190; i++) pomodoro_step(panel, "休息分钟", false);
        for (int i = 0; i < 20; i++) pomodoro_step(panel, "轮次", false);
        if (!pomodoro_row_value(panel, "专注分钟", "1") ||
            !pomodoro_row_value(panel, "休息分钟", "1") ||
            !pomodoro_row_value(panel, "轮次", "1")) {
            fprintf(stderr, "POMODORO_STEPPER lower bounds not clamped\n");
            failures++;
        }
        if (!click_text(panel, "25/5/4")) {
            fprintf(stderr, "MISSING_ACTION pomodoro-preset\n");
            failures++;
        }
        if (auto_switch) {
            activate(auto_switch);
            settle();
            if (lv_obj_has_state(auto_switch, LV_STATE_CHECKED)) {
                fprintf(stderr, "POMODORO_SETTINGS switch did not toggle\n");
                failures++;
            }
            activate(auto_switch);
            settle();
            if (!lv_obj_has_state(auto_switch, LV_STATE_CHECKED)) {
                fprintf(stderr, "POMODORO_SETTINGS switch did not restore\n");
                failures++;
            }
        }
        bool preset_clicked = click_text(panel, "45/10/2");
        bool preset_values = pomodoro_row_value(panel, "专注分钟", "45") &&
                             pomodoro_row_value(panel, "休息分钟", "10") &&
                             pomodoro_row_value(panel, "轮次", "2");
        bool saved = click_text(panel, "保存");
        bool idle_clock = find_label(pomodoro, "45:00") != NULL;
        bool saved_notice = find_label(pomodoro, "节律已更新") != NULL;
        if (!preset_clicked || !preset_values || !saved || !idle_clock || !saved_notice) {
            fprintf(stderr,
                    "POMODORO_PRESET save did not update idle focus "
                    "click=%d values=%d save=%d clock=%d notice=%d\n",
                    preset_clicked, preset_values, saved, idle_clock, saved_notice);
            failures++;
        }
    }

    if (!click_icon_button(pomodoro, QZ_ICON_SETTINGS, 16)) {
        fprintf(stderr, "MISSING_ACTION pomodoro-settings-reopen\n");
        failures++;
    } else {
        lv_obj_t *panel = pomodoro;
        lv_obj_t *auto_switch = find_type(panel, &lv_switch_class);
        if (!auto_switch || !lv_obj_has_state(auto_switch, LV_STATE_CHECKED)) {
            fprintf(stderr, "POMODORO_SETTINGS switch lost checked state after preset\n");
            failures++;
        }
        click_text(panel, "15/5/4");
        if (auto_switch) {
            activate(auto_switch);
            settle();
        }
        if (!click_text(panel, "保存") || !find_label(pomodoro, "15:00")) {
            fprintf(stderr, "POMODORO_SETTINGS second save failed\n");
            failures++;
        }
    }
    check(pomodoro, "pomodoro-customized-idle");

    if (!click_text(pomodoro, "开始专注")) {
        fprintf(stderr, "MISSING_ACTION pomodoro-start\n");
        failures++;
    } else {
        layout_pomodoro_state_t state;
        pomodoro_fixture_snapshot(&state);
        if (!state.active || state.paused || state.focus_minutes != 15 ||
            state.break_minutes != 5 || state.total_cycles != 4 ||
            state.auto_break || state.start_count != 1) {
            fprintf(stderr, "POMODORO_START wrong request state focus=%d break=%d cycles=%d auto=%d\n",
                    state.focus_minutes, state.break_minutes, state.total_cycles,
                    state.auto_break);
            failures++;
        }
        if (!find_label(pomodoro, "专注") || !find_label(pomodoro, "暂停") ||
            !find_label_prefix(pomodoro, "预计 ")) {
            fprintf(stderr, "POMODORO_FOCUS missing live time state\n");
            failures++;
        }
        check(pomodoro, "pomodoro-focus");

        if (!click_text(pomodoro, "暂停")) {
            fprintf(stderr, "MISSING_ACTION pomodoro-pause\n");
            failures++;
        } else {
            pomodoro_fixture_snapshot(&state);
            if (!state.paused || state.pause_count != 1 ||
                !find_label(pomodoro, "专注已暂停") ||
                !find_label(pomodoro, "继续专注") ||
                !find_label(pomodoro, "继续后计算结束时刻")) {
                fprintf(stderr, "POMODORO_PAUSE incorrect focus pause state\n");
                failures++;
            }
            check(pomodoro, "pomodoro-focus-paused");
            if (!click_text(pomodoro, "继续专注")) {
                fprintf(stderr, "MISSING_ACTION pomodoro-resume\n");
                failures++;
            } else {
                pomodoro_fixture_snapshot(&state);
                if (state.paused || state.resume_count != 1 ||
                    !find_label(pomodoro, "专注") || !find_label(pomodoro, "暂停")) {
                    fprintf(stderr, "POMODORO_RESUME did not return to focus\n");
                    failures++;
                }
            }
        }
    }

    if (!click_icon_button(pomodoro, QZ_ICON_SETTINGS, 16)) {
        fprintf(stderr, "MISSING_ACTION pomodoro-settings-active\n");
        failures++;
    } else {
        lv_obj_t *panel = pomodoro;
        click_text(panel, "45/10/2");
        lv_obj_t *auto_switch = find_type(panel, &lv_switch_class);
        if (auto_switch && !lv_obj_has_state(auto_switch, LV_STATE_CHECKED)) {
            activate(auto_switch);
            settle();
        }
        if (!click_text(panel, "保存") ||
            !find_label(pomodoro, "已保存，将在下一次开始时生效") ||
            !find_label(pomodoro, "第 1 / 4 轮")) {
            fprintf(stderr, "POMODORO_ACTIVE_SAVE changed the running phase\n");
            failures++;
        }
    }
    if (!click_text(pomodoro, "结束") || !find_label(pomodoro, "45:00") ||
        !find_label(pomodoro, "准备专注")) {
        fprintf(stderr, "POMODORO_STOP did not restore idle configuration\n");
        failures++;
    }
    {
        layout_pomodoro_state_t state;
        pomodoro_fixture_snapshot(&state);
        if (state.active || state.paused || state.stop_count != 1) {
            fprintf(stderr, "POMODORO_STOP wrong fixture state\n");
            failures++;
        }
    }

    if (!click_text(pomodoro, "开始专注")) {
        fprintf(stderr, "MISSING_ACTION pomodoro-restart\n");
        failures++;
    } else {
        layout_pomodoro_state_t state;
        pomodoro_fixture_snapshot(&state);
        if (!state.active || state.focus_minutes != 45 ||
            state.break_minutes != 10 || state.total_cycles != 2 ||
            !state.auto_break || state.start_count != 2) {
            fprintf(stderr, "POMODORO_NEXT_START did not use saved config\n");
            failures++;
        }
    }
    pomodoro_fixture_set(true, false, "break", 300, 1, 2, 45, 10, true);
    pomodoro_poll_now();
    if (!find_label(pomodoro, "休息") || !find_label(pomodoro, "暂停") ||
        !find_label(pomodoro, "已完成 1 / 2 轮") ||
        !find_label_prefix(pomodoro, "预计 ")) {
        fprintf(stderr, "POMODORO_REST missing break state\n");
        failures++;
    }
    check(pomodoro, "pomodoro-rest");

    pomodoro_fixture_set(true, true, "break", 300, 1, 2, 45, 10, false);
    pomodoro_poll_now();
    if (!find_label(pomodoro, "休息已暂停") || !find_label(pomodoro, "继续休息") ||
        !find_label(pomodoro, "继续后开始休息")) {
        fprintf(stderr, "POMODORO_REST_PAUSED missing pending break copy\n");
        failures++;
    }
    check(pomodoro, "pomodoro-rest-paused");
    if (!click_text(pomodoro, "继续休息")) {
        fprintf(stderr, "MISSING_ACTION pomodoro-resume-break\n");
        failures++;
    } else {
        layout_pomodoro_state_t state;
        pomodoro_fixture_snapshot(&state);
        if (state.paused || !state.active || strcmp(state.phase, "break") != 0 ||
            state.resume_count != 2) {
            fprintf(stderr, "POMODORO_RESUME_BREAK failed\n");
            failures++;
        }
    }
    if (!click_text(pomodoro, "结束")) {
        fprintf(stderr, "MISSING_ACTION pomodoro-stop-rest\n");
        failures++;
    }

    bool backlight_ready = backlight_fixture_init();
    bool fixture_home_ready = mkdtemp(fixture_home_path) != NULL;
    if (fixture_home_ready) setenv("HOME", fixture_home_path, 1);
    qz_notifications_init();
    if (!fixture_home_ready) {
        fprintf(stderr, "NOTIFICATION_FIXTURE_HOME_FAILED\n");
        failures++;
    } else {
        if (!qz_notifications_set_dnd(true)) {
            fprintf(stderr, "DND_FIXTURE_MANUAL_SET_FAILED\n");
            failures++;
        }
        if (!find_label(settings, "手动开启")) {
            fprintf(stderr, "MISSING_DND_SUMMARY manual\n");
            failures++;
        }
        if (!qz_notifications_schedule_set(true, 22 * 60, 8 * 60)) {
            fprintf(stderr, "DND_FIXTURE_SCHEDULE_SET_FAILED\n");
            failures++;
        }
        lv_obj_send_event(settings, LV_EVENT_SCREEN_LOADED, NULL);
        if (!find_label(settings, "每天 22:00-08:00 · 手动")) {
            fprintf(stderr, "MISSING_DND_SUMMARY schedule-and-manual\n");
            failures++;
        }
        if (!qz_notifications_schedule_set(false, 22 * 60, 8 * 60)) {
            fprintf(stderr, "DND_FIXTURE_SCHEDULE_RESET_FAILED\n");
            failures++;
        }
        if (!qz_notifications_set_dnd(false)) {
            fprintf(stderr, "DND_FIXTURE_MANUAL_RESET_FAILED\n");
            failures++;
        }
        lv_obj_send_event(settings, LV_EVENT_SCREEN_LOADED, NULL);
        if (!find_label(settings, "未启用")) {
            fprintf(stderr, "MISSING_DND_SUMMARY disabled\n");
            failures++;
        }

        lv_obj_t *schedule_rollers[8];
        lv_obj_t *schedule_switches[4];
        int roller_count = 0;
        int switch_count = 0;
        lv_screen_load(settings);
        settle();
        lv_obj_t *schedule_modal = open_label_row_modal(settings, "安静时段");
        if (!schedule_modal) {
            fprintf(stderr, "MISSING_ACTION dnd-schedule-open\n");
            failures++;
        } else {
            lv_obj_update_layout(schedule_modal);
            roller_count = find_types(schedule_modal, &lv_roller_class,
                                      schedule_rollers, 8);
            switch_count = find_types(schedule_modal, &lv_switch_class,
                                      schedule_switches, 4);
            if (roller_count != 4 || switch_count != 1 ||
                !find_label(schedule_modal, "安静时段") ||
                !find_label(schedule_modal, "到点自动开启，跨天会自动识别") ||
                !find_label(schedule_modal, "启用定时安静时段") ||
                !find_label(schedule_modal, "开始") ||
                !find_label(schedule_modal, "结束") ||
                !find_label(schedule_modal, "取消") ||
                !find_label(schedule_modal, "保存")) {
                fprintf(stderr,
                        "DND_SCHEDULE_UI rollers=%d switches=%d missing controls\n",
                        roller_count, switch_count);
                failures++;
            }
            check(settings, "dnd-schedule");
            inspect(schedule_modal, "dnd-schedule");

            /* Cancelling a draft must not change persisted state or the row. */
            if (switch_count != 1 ||
                !activate(schedule_switches[0]) ||
                roller_count != 4) {
                fprintf(stderr, "MISSING_ACTION dnd-schedule-draft\n");
                failures++;
            } else {
                lv_roller_set_selected(schedule_rollers[0], 21, LV_ANIM_OFF);
                lv_roller_set_selected(schedule_rollers[1], 10, LV_ANIM_OFF);
                lv_roller_set_selected(schedule_rollers[2], 6, LV_ANIM_OFF);
                lv_roller_set_selected(schedule_rollers[3], 45, LV_ANIM_OFF);
                if (!click_text(schedule_modal, "取消")) {
                    fprintf(stderr, "MISSING_ACTION dnd-schedule-cancel\n");
                    failures++;
                }
                bool enabled = true;
                int start = 0;
                int end = 0;
                qz_notifications_schedule_get(&enabled, &start, &end);
                if (enabled || start != 22 * 60 || end != 8 * 60 ||
                    !find_label(settings, "未启用")) {
                    fprintf(stderr,
                            "DND_SCHEDULE_CANCEL changed state enabled=%d start=%d end=%d\n",
                            enabled, start, end);
                    failures++;
                }
            }
        }

        /* Save a cross-midnight window, then verify the modal restores it. */
        schedule_modal = open_label_row_modal(settings, "安静时段");
        if (schedule_modal) {
            lv_obj_update_layout(schedule_modal);
            roller_count = find_types(schedule_modal, &lv_roller_class,
                                      schedule_rollers, 8);
            switch_count = find_types(schedule_modal, &lv_switch_class,
                                      schedule_switches, 4);
            if (switch_count != 1 || !activate(schedule_switches[0]) ||
                roller_count != 4) {
                fprintf(stderr, "MISSING_ACTION dnd-schedule-enable\n");
                failures++;
            } else {
                lv_roller_set_selected(schedule_rollers[0], 23, LV_ANIM_OFF);
                lv_roller_set_selected(schedule_rollers[1], 30, LV_ANIM_OFF);
                lv_roller_set_selected(schedule_rollers[2], 7, LV_ANIM_OFF);
                lv_roller_set_selected(schedule_rollers[3], 15, LV_ANIM_OFF);
                if (!click_text(schedule_modal, "保存")) {
                    fprintf(stderr, "MISSING_ACTION dnd-schedule-save\n");
                    failures++;
                }
                bool enabled = false;
                int start = 0;
                int end = 0;
                qz_notifications_schedule_get(&enabled, &start, &end);
                if (!enabled || start != 23 * 60 + 30 || end != 7 * 60 + 15 ||
                    !find_label(settings, "每天 23:30-07:15")) {
                    fprintf(stderr,
                            "DND_SCHEDULE_SAVE wrong state enabled=%d start=%d end=%d\n",
                            enabled, start, end);
                    failures++;
                }
            }
        } else {
            fprintf(stderr, "MISSING_ACTION dnd-schedule-reopen\n");
            failures++;
        }

        schedule_modal = open_label_row_modal(settings, "安静时段");
        if (schedule_modal) {
            lv_obj_update_layout(schedule_modal);
            roller_count = find_types(schedule_modal, &lv_roller_class,
                                      schedule_rollers, 8);
            switch_count = find_types(schedule_modal, &lv_switch_class,
                                      schedule_switches, 4);
            if (roller_count != 4 || switch_count != 1 ||
                !lv_obj_has_state(schedule_switches[0], LV_STATE_CHECKED) ||
                lv_roller_get_selected(schedule_rollers[0]) != 23 ||
                lv_roller_get_selected(schedule_rollers[1]) != 30 ||
                lv_roller_get_selected(schedule_rollers[2]) != 7 ||
                lv_roller_get_selected(schedule_rollers[3]) != 15) {
                fprintf(stderr, "DND_SCHEDULE_REOPEN lost saved values\n");
                failures++;
            }

            /* Equal times are ambiguous: keep the modal open with an inline
             * error instead of silently doing nothing or saving a 24-hour DND. */
            if (roller_count != 4) {
                fprintf(stderr, "MISSING_ACTION dnd-schedule-equal-time-draft\n");
                failures++;
            } else {
                lv_roller_set_selected(schedule_rollers[2], 23, LV_ANIM_OFF);
                lv_roller_set_selected(schedule_rollers[3], 30, LV_ANIM_OFF);
                if (!click_text(schedule_modal, "保存") ||
                    !find_label(schedule_modal, "开始和结束不能相同")) {
                    fprintf(stderr, "DND_SCHEDULE_EQUAL_TIMES saved or hid error\n");
                    failures++;
                }
                inspect(schedule_modal, "dnd-schedule-equal-times");
                if (!click_text(schedule_modal, "取消")) {
                    fprintf(stderr, "MISSING_ACTION dnd-schedule-error-cancel\n");
                    failures++;
                }
                bool enabled = false;
                int start = 0;
                int end = 0;
                qz_notifications_schedule_get(&enabled, &start, &end);
                if (!enabled || start != 23 * 60 + 30 || end != 7 * 60 + 15 ||
                    !find_label(settings, "每天 23:30-07:15")) {
                    fprintf(stderr, "DND_SCHEDULE_EQUAL_TIMES changed saved state\n");
                    failures++;
                }
            }
        } else {
            fprintf(stderr, "MISSING_ACTION dnd-schedule-restore-check\n");
            failures++;
        }

        /* Keep later notification tests deterministic regardless of the clock. */
        if (!qz_notifications_schedule_set(false, 23 * 60 + 30, 7 * 60 + 15) ||
            !qz_notifications_set_dnd(false)) {
            fprintf(stderr, "DND_FIXTURE_FINAL_RESET_FAILED\n");
            failures++;
        }
        lv_obj_send_event(settings, LV_EVENT_SCREEN_LOADED, NULL);
        if (!find_label(settings, "未启用")) {
            fprintf(stderr, "DND_FIXTURE_FINAL_SUMMARY_FAILED\n");
            failures++;
        }

        char fixture_file[256];
        snprintf(fixture_file, sizeof(fixture_file), "%s/.qzdesk-dnd", fixture_home_path);
        unlink(fixture_file);
        snprintf(fixture_file, sizeof(fixture_file), "%s/.qzdesk-dnd-schedule", fixture_home_path);
        unlink(fixture_file);
        rmdir(fixture_home_path);
    }
    if (backlight_ready) check_backlight_fade(display);
    qzdesk_core_event_t notification = { .type = QZDESK_CORE_EVENT_TOAST };
    snprintf(notification.role, sizeof(notification.role), "timer");
    snprintf(notification.text, sizeof(notification.text), "专注完成，起身喝水，休息一下");
    if (qz_notifications_dnd()) {
        fprintf(stderr, "NOTIFICATION_FIXTURE_DND_ENABLED\n");
        failures++;
    } else {
        emit(&notification);
        settle();
        inspect(lv_layer_top(), "timer-notification");
        check(pomodoro, "timer-notification");
        snprintf(notification.text, sizeof(notification.text), "第二条提醒：喝水");
        emit(&notification);
        if (!find_label(lv_layer_top(), "专注完成，起身喝水，休息一下")) {
            fprintf(stderr, "NOTIFICATION_OVERWRITTEN\n"); failures++;
        }
        if (!click_text(lv_layer_top(), "完成") ||
            !find_label(lv_layer_top(), "第二条提醒：喝水")) {
            fprintf(stderr, "NOTIFICATION_QUEUE_LOST\n"); failures++;
        }
        check(pomodoro, "timer-notification-queued");
        if (!click_text(lv_layer_top(), "完成") ||
            find_label(lv_layer_top(), "5分钟后")) failures++;
        emit(&notification);
        if (!click_text(lv_layer_top(), "5分钟后") ||
            !find_label(lv_layer_top(), "保存失败，请重试") ||
            !find_label(lv_layer_top(), "第二条提醒：喝水")) failures++;
        inspect(lv_layer_top(), "timer-snooze-failed");
        check(pomodoro, "timer-snooze-failed");
        if (!click_text(lv_layer_top(), "完成")) failures++;
    }

    /* 顶部下拉出来的通知中心：刚才那几条都留在历史里，清空之后回到空状态。 */
    qz_notifications_open_panel();
    settle();
    lv_obj_update_layout(lv_layer_top());
    if (!find_label(lv_layer_top(), "通知") || !find_label(lv_layer_top(), "免打扰") ||
        !find_label(lv_layer_top(), "第二条提醒：喝水")) {
        fprintf(stderr, "MISSING_PANEL notification-center\n");
        failures++;
    }
    inspect(lv_layer_top(), "notification-center");
    check(pomodoro, "notification-center");
    lv_obj_t *trash = find_icon_image(lv_layer_top(), QZ_ICON_TRASH, 15);
    if (!trash) {
        fprintf(stderr, "MISSING_ACTION notification-center-clear\n");
        failures++;
    } else {
        lv_obj_send_event(lv_obj_get_parent(trash), LV_EVENT_CLICKED, NULL);
        settle();
        if (!find_label(lv_layer_top(), "暂无通知") ||
            find_label(lv_layer_top(), "第二条提醒：喝水")) {
            fprintf(stderr, "STALE_HISTORY notification-center-clear\n");
            failures++;
        }
        check(pomodoro, "notification-center-empty");
    }
    qz_notifications_close_panel();
    settle();

    lv_obj_t *general = find_screen(display, "通用设置");
    if (general) {
        if (open_dialog(general, "时间", "time-modal")) click_text(general, "取消");
        if (open_dialog(general, "时区", "timezone-modal")) {
            check_pages(general, "timezone");
            click_text(general, "取消");
        }
    }
    lv_obj_t *about = find_screen(display, "关于");
    if (about && open_dialog(about, "服务地址", "server-address")) {
        click_text(about, "关闭");
    }
    lv_obj_t *wlan = find_screen(display, "WLAN");
    if (wlan) {
        lv_screen_load(wlan);
        check_pages(wlan, "wifi");
        fixture_long_wifi = true;
        lv_obj_send_event(wlan, LV_EVENT_SCREEN_LOADED, NULL);
        wlan_fixture_advance_poll();
        check_pages(wlan, "wifi-long");
        for (int i = 0; i < 16 && click_text(wlan, "上一页"); i++) {}
        /* Long enough to wrap at 320 px, but short enough that the row's
         * clickable center remains inside the list viewport. The fixture SSID
         * above intentionally matches this trigger. */
        char ssid[64]; memset(ssid, 'W', 24); ssid[24] = 0;
        if (open_dialog(wlan, ssid, "wifi-password")) {
            click_text(wlan, "取消");
        }
        /* A previous lease can outlive carrier. It must not turn the page into
         * a connected view or lock the enable switch. */
        fixture_connected = true;
        fixture_link_up = false;
        fixture_wifi_state = QZ_WIFI_IDLE;
        lv_obj_send_event(wlan, LV_EVENT_SCREEN_LOADED, NULL);
        lv_obj_update_layout(wlan);
        lv_obj_t *wlan_switch = find_type(wlan, &lv_switch_class);
        if (find_label_prefix(wlan, "Wi-Fi 已连接") ||
            find_label(wlan, "搜索其他 WiFi") ||
            !find_label(wlan, "请选择 Wi-Fi 网络") ||
            !wlan_switch || lv_obj_has_state(wlan_switch, LV_STATE_DISABLED)) {
            fprintf(stderr, "STALE_WIFI_LINK wifi-stale-lease\n");
            failures++;
        }
        check(wlan, "wifi-stale-lease");
        fixture_link_up = true;
        fixture_wifi_state = QZ_WIFI_CONNECTED;
        lv_obj_send_event(wlan, LV_EVENT_SCREEN_LOADED, NULL);
        if (open_dialog(wlan, "Layout test network", "wifi-details")) {
            /* Clicking the scrim avoids sending a real disconnect command. */
            lv_obj_t *overlay = lv_obj_get_child(wlan, -1);
            lv_obj_send_event(overlay, LV_EVENT_CLICKED, NULL);
            settle();
        }
    }
    fixture_skill_count = 0;
    lv_obj_send_event(skills, LV_EVENT_SCREEN_LOADED, NULL);
    if (!settle_until_label(skills, "还没有安装 Skill", 80) ||
        !find_label(skills, "从浏览器控制台添加，设备端随开随用") ||
        !find_label(skills, "重试")) {
        fprintf(stderr, "MISSING_SKILLS_STATE skills-empty\n");
        failures++;
    }
    /* A queued reload must not replace the empty state after it has settled. */
    settle();
    if (!find_label(skills, "还没有安装 Skill") ||
        find_label(skills, "正在读取技能服务")) {
        fprintf(stderr, "UNSTABLE_SKILLS_STATE skills-empty\n");
        failures++;
    }
    check(skills, "skills-empty");
    fixture_skill_count = -1;
    lv_obj_send_event(skills, LV_EVENT_SCREEN_LOADED, NULL);
    if (!settle_until_label(skills, "本地服务未连接", 80) ||
        !find_label(skills, "请确认 QZdesk 核心正在运行") ||
        !find_label(skills, "本地 Skill 服务未响应")) {
        fprintf(stderr, "MISSING_SKILLS_STATE skills-service-error\n");
        failures++;
    }
    settle();
    if (!find_label(skills, "本地服务未连接") ||
        find_label(skills, "还没有安装 Skill")) {
        fprintf(stderr, "UNSTABLE_SKILLS_STATE skills-service-error\n");
        failures++;
    }
    check(skills, "skills-service-error");

    /* 每个 qz_icon() 请求都必须命中一个烘好的精确档位：不然 LVGL 的最近邻
     * 缩图会把线性图标拉成灰边，小屏上尤其明显。漏档就在这里红。 */
    int icon_misses = qz_icon_size_misses();
    if (icon_misses != 0) {
        fprintf(stderr, "ICON_SIZE_MISS: %d 次取图没有精确档位，"
                        "按调用点补 tools/mk_icon_assets.py 的 SIZES\n", icon_misses);
        failures++;
    }
    fprintf(stderr, "LAYOUT %dx%d: %d cases, %d failures, %d text-mode warnings\n",
            panel_width, panel_height, cases, failures, warnings);
    return failures ? 1 : 0;
}
