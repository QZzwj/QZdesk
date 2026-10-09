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
#include "qzdesk_core.h"
#include "lvgl/src/display/lv_display_private.h"
#include "lvgl/src/misc/lv_area_private.h"
#include "lvgl/src/widgets/image/lv_image_private.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static int failures, warnings, cases;
static int panel_width, panel_height;
static unsigned char *pixels;
static const char *output_dir;
static lv_obj_t *assistant_screen;
static lv_obj_t *apps_screen;
static bool fixture_long_wifi, fixture_connected;
static bool fixture_long_names;
static int fixture_skill_count = QZ_SKILL_MAX;
static qzdesk_core_event_cb_t listeners[16];
static void *listener_data[16];
static int listener_count;
static lv_obj_t *named_screens[32];
static const char *screen_names[32];
static int named_screen_count;
static qzdesk_core_event_cb_t poll_callback;
static void *poll_data;

/* No hardware, external services, saved settings or core process are changed
 * by these fixtures. Internal LVGL objects and callbacks remain unmodified. */
bool __wrap_qz_wifi_ready(void) { return true; }
bool __wrap_qz_wifi_worker_start(void) { return true; }
bool __wrap_qz_wifi_enable_async(bool enabled) { (void)enabled; return true; }
bool __wrap_qz_wifi_scan_async(void) { return true; }
bool __wrap_qz_wifi_scan_pending(void) { return false; }
void __wrap_qz_wifi_interface(char *out, unsigned int size) { snprintf(out, size, "wlan0"); }
void __wrap_qz_wifi_current(char *out, unsigned int size)
{
    snprintf(out, size, "%s", fixture_connected ? "Layout test network" : "");
}
void __wrap_qz_wifi_ipv4(char *out, unsigned int size) { snprintf(out, size, "192.168.100.200"); }
void __wrap_qz_wifi_mac(char *out, unsigned int size) { snprintf(out, size, "AA:BB:CC:DD:EE:FF"); }
bool __wrap_qz_wifi_status(qz_wifi_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = fixture_connected ? QZ_WIFI_CONNECTED : QZ_WIFI_IDLE;
    return true;
}
int __wrap_qz_wifi_scan_results(qz_wifi_ap_t *out, int maximum)
{
    int count = maximum < 12 ? maximum : 12;
    for (int i = 0; i < count; i++) {
        memset(&out[i], 0, sizeof(out[i]));
        if (fixture_long_wifi) memset(out[i].ssid, 'W', sizeof(out[i].ssid) - 1);
        else snprintf(out[i].ssid, sizeof(out[i].ssid), "Layout WiFi %02d", i + 1);
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
bool __wrap_qz_web_request(const char *method, const char *path, const char *body,
                            char *out, size_t size)
{
    (void)method; (void)body;
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
    } else if (strcmp(path, "/api/pomodoro") == 0) {
        snprintf(out, size, "{\"active\":false,\"paused\":false,\"remaining_seconds\":1500,"
                  "\"focus_minutes\":25,\"break_minutes\":5,\"total_cycles\":4,\"completed_cycles\":0}");
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
bool __wrap_qzdesk_core_open(void) { return true; }
bool __wrap_qzdesk_core_request_status(void) { return true; }
bool __wrap_qzdesk_core_request_weather(void) { return true; }
bool __wrap_qzdesk_core_request_performance(void) { return true; }
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

static void check(lv_obj_t *screen, const char *name)
{
    lv_screen_load(screen);
    lv_tick_inc(1);
    lv_timer_handler();
    lv_obj_update_layout(screen);
    inspect(screen, name);
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

static void settle(void)
{
    /* Async deletion/rebuilding happens in timer_handler. Reduced motion makes
     * this deterministic; no wall-clock wait or GUI window is needed. */
    lv_tick_inc(1);
    lv_timer_handler();
}

static bool click_text(lv_obj_t *screen, const char *text)
{
    lv_obj_update_layout(screen);
    lv_obj_t *object = find_label(screen, text);
    if (object) {
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
    lv_obj_t *grid = lv_obj_get_child(screen, 1);
    lv_obj_update_layout(screen);
    if (!grid || !lv_obj_has_flag(grid, LV_OBJ_FLAG_SCROLLABLE) ||
        lv_obj_get_scroll_dir(grid) != LV_DIR_VER || lv_obj_get_child_count(grid) != 8) {
        fprintf(stderr, "APPS_SCROLL expected 8 tiles in a vertical scroll area\n");
        failures++;
        return;
    }
    lv_screen_load(screen);
    for (int bottom = 0; bottom < 2; bottom++) {
        lv_obj_scroll_to_y(grid, bottom ? lv_obj_get_scroll_bottom(grid) : 0, LV_ANIM_OFF);
        lv_obj_update_layout(screen);
        lv_area_t viewport;
        lv_obj_get_coords(grid, &viewport);
        int visible = 0;
        for (uint32_t i = 0; i < 8; i++) {
            lv_area_t tile;
            lv_obj_get_coords(lv_obj_get_child(grid, i), &tile);
            if (inside(&tile, &viewport)) visible++;
        }
        if (visible != 4 || lv_obj_get_scroll_left(grid) > 0 || lv_obj_get_scroll_right(grid) > 0) {
            fprintf(stderr, "APPS_SCROLL %s visible=%d, expected 4 without horizontal overflow\n",
                    bottom ? "bottom" : "top", visible);
            failures++;
        }
        check(screen, bottom ? "apps-scroll-bottom" : "apps-scroll-top");
    }
    require_click(screen, "设置", "apps-open-settings");
    lv_obj_scroll_to_y(grid, 0, LV_ANIM_OFF);
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

static void check_weather_states(lv_obj_t *desktop, lv_obj_t *weather)
{
    if (!weather) return;
    qzdesk_core_event_t event = { .type = QZDESK_CORE_EVENT_WEATHER };
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
}

static void seed_events(void)
{
    qzdesk_core_event_t event = { .type = QZDESK_CORE_EVENT_WEATHER };
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
    name_screen(find_screen(display, "天气"), "weather");
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
    check_weather_states(desktop, find_screen(display, "天气"));
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
    require_click(assistant_screen, "表情", "assistant-face");
    check(assistant_screen, "assistant-face");
    require_click(assistant_screen, "聊天", "assistant-chat");
    lv_tick_inc(1000);
    settle();
    check(assistant_screen, "assistant-chat");
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
    }
    else {
        fprintf(stderr, "MISSING_INPUT assistant-keyboard\n");
        failures++;
    }
    require_click(assistant_screen, "表情", "assistant-face-return");
    check(assistant_screen, "assistant-face-return");
    require_click(assistant_screen, "聊天", "assistant-chat-return");
    check(assistant_screen, "assistant-chat-return");
    input = find_type(assistant_screen, &lv_textarea_class);
    if (input) {
        lv_obj_send_event(input, LV_EVENT_FOCUSED, NULL);
        check(assistant_screen, "assistant-keyboard-return");
        lv_obj_send_event(input, LV_EVENT_DEFOCUSED, NULL);
    }
    check_pages(skills, "skills");
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
        click_text(reminders, "取消");
    }
    lv_obj_t *camera = qz_applets_screen(QZ_APPLET_CAMERA);
    lv_screen_load(camera);
    settle();
    lv_point_t shutter_point = { panel_width / 2, panel_height - qz_scale_y(40) };
    lv_obj_t *shutter = lv_indev_search_obj(camera, &shutter_point);
    if (shutter) lv_obj_send_event(shutter, LV_EVENT_CLICKED, NULL);
    settle();
    check(camera, "camera-saved");
    if (open_dialog(camera, "已保存·查看位置", "camera-result")) click_text(camera, "关闭");
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
        check_pages(wlan, "wifi-long");
        for (int i = 0; i < 16 && click_text(wlan, "上一页"); i++) {}
        char ssid[64]; memset(ssid, 'W', 63); ssid[63] = 0;
        if (open_dialog(wlan, ssid, "wifi-password")) {
            click_text(wlan, "取消");
        }
        fixture_connected = true;
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
    check(skills, "skills-empty");
    fixture_skill_count = -1;
    lv_obj_send_event(skills, LV_EVENT_SCREEN_LOADED, NULL);
    check(skills, "skills-service-error");
    fprintf(stderr, "LAYOUT %dx%d: %d cases, %d failures, %d text-mode warnings\n",
            panel_width, panel_height, cases, failures, warnings);
    return failures ? 1 : 0;
}
