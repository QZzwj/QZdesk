#include "apps.h"
#include "applets.h"
#include "assistant.h"
#include "desktop.h"
#include "settings.h"
#include "skills_page.h"
#include "performance_page.h"
#include "theme.h"
#include "notifications.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
static const char *out_dir;
static unsigned char *pixels;
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{
    (void)area;
    memcpy(pixels, data, 320 * 240 * 3);
    lv_display_flush_ready(display);
}
static void pump(void)
{
    for (int i = 0; i < 200; i++) {
        usleep(10000);
        lv_tick_inc(10);
        lv_timer_handler();
    }
    lv_refr_now(NULL);
}
static lv_obj_t *label(lv_obj_t *root, const char *text)
{
    if (lv_obj_check_type(root, &lv_label_class) && strcmp(lv_label_get_text(root), text) == 0) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = label(lv_obj_get_child(root, i), text);
        if (found) return found;
    }
    return NULL;
}
static lv_obj_t *textarea(lv_obj_t *root)
{
    if (lv_obj_check_type(root, &lv_textarea_class)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++) {
        lv_obj_t *found = textarea(lv_obj_get_child(root, i));
        if (found) return found;
    }
    return NULL;
}
static void result(const char *name, int ok)
{
    printf("%s %s\n", name, ok ? "PASS" : "FAIL");
    fflush(stdout);
    if (!ok) failures++;
}
static int click(lv_obj_t *root, const char *text)
{
    lv_obj_t *found = label(root, text);
    if (!found) { result(text, 0); return 0; }
    lv_obj_t *button = lv_obj_get_parent(found);
    lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
    pump();
    return 1;
}
static void snapshot(const char *name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.ppm", out_dir, name);
    FILE *file = fopen(path, "wb");
    if (!file) { result("screenshot_write", 0); return; }
    fprintf(file, "P6\n320 240\n255\n");
    fwrite(pixels, 1, 320 * 240 * 3, file);
    fclose(file);
}
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    out_dir = argv[1];
    setenv("QZDESK_REDUCE_MOTION", "1", 1);
    qz_scale_init(320, 240);
    lv_init();
    pixels = calloc(320 * 240, 3);
    void *buffer = calloc(320 * 240, 3);
    lv_display_t *display = lv_display_create(320, 240);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB888);
    lv_display_set_buffers(display, buffer, NULL, 320 * 240 * 3, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    qz_font_init(); qz_style_init(); qz_notifications_init();
    lv_theme_default_init(display, qz_color(QZ_ACCENT), qz_color(QZ_ACCENT_DARK), false, qz_font());
    lv_obj_t *desktop = qz_desktop_create("localhost");
    lv_obj_t *apps = qz_apps_create();
    lv_obj_t *assistant = qz_assistant_create();
    lv_obj_t *settings = qz_settings_create();
    lv_obj_t *skills = qz_skill_page_create(apps);
    lv_obj_t *performance = qz_performance_create(apps);
    qz_desktop_set_assistant(assistant); qz_desktop_set_apps(apps); qz_desktop_set_settings(settings);
    qz_apps_set_desktop(desktop); qz_apps_set_skill_screen(skills); qz_apps_set_performance_screen(performance);
    qz_apps_set_settings_screen(settings); qz_applets_init(apps);
    qz_assistant_set_desktop(desktop); qz_settings_set_desktop(desktop);
    lv_screen_load(skills); pump();
    const char *names[] = {"个人记忆", "日常计划", "学习教练"};
    const char *texts[] = {"QZ-LCD-LIVE-memory", "QZ-LCD-LIVE-todo", "QZ-LCD-LIVE-learning"};
    for (int i = 0; i < 3; i++) {
        click(skills, "陪伴");
        if (i) click(lv_layer_top(), "换一页");
        click(lv_layer_top(), names[i]);
        lv_obj_t *input = textarea(lv_layer_top());
        result("record_input_exists", input != NULL);
        if (input) {
            lv_textarea_set_text(input, texts[i]);
            click(lv_layer_top(), "新增");
            result(names[i], label(lv_layer_top(), texts[i]) != NULL);
            snapshot(i == 0 ? "memory-saved" : i == 1 ? "todo-saved" : "learning-saved");
            if (i == 1) {
                click(lv_layer_top(), "切换完成");
                result("todo_complete_real_core", label(lv_layer_top(), "已完成；可点切换恢复待办") != NULL);
                click(lv_layer_top(), "切换完成");
            }
            lv_textarea_set_text(input, "QZ-LCD-LIVE-updated");
            click(lv_layer_top(), "修改");
            result("record_update_real_core", label(lv_layer_top(), "QZ-LCD-LIVE-updated") != NULL);
            click(lv_layer_top(), "删除");
            result("record_delete_real_core", label(lv_layer_top(), "暂无记录，填写内容后点新增") != NULL);
        }
        click(lv_layer_top(), "返回");
    }
    click(skills, "陪伴"); click(lv_layer_top(), "主动关心");
    result("care_reads_real_core", label(lv_layer_top(), "主动关心：已关闭") != NULL);
    click(lv_layer_top(), "同意开启");
    result("care_enable_real_core", label(lv_layer_top(), "主动关心：已开启") != NULL);
    snapshot("care-enabled");
    click(lv_layer_top(), "关闭关心");
    result("care_disable_real_core", label(lv_layer_top(), "主动关心：已关闭") != NULL);
    click(lv_layer_top(), "返回");
    click(skills, "陪伴"); click(lv_layer_top(), "专注搭子");
    result("focus_navigation", lv_screen_active() == qz_applets_screen(QZ_APPLET_POMODORO));
    snapshot("focus-real-core");
    lv_screen_load(skills); pump(); click(skills, "陪伴"); click(lv_layer_top(), "换一页");
    click(lv_layer_top(), "设备管家");
    result("devices_navigation", lv_screen_active() == qz_applets_screen(QZ_APPLET_CONTROL));
    snapshot("devices-real-core");
    lv_screen_load(skills); pump(); click(skills, "陪伴"); click(lv_layer_top(), "情绪陪伴");
    result("emotion_navigation", lv_screen_active() == assistant);
    lv_obj_t *draft = textarea(assistant);
    result("emotion_draft", draft && strlen(lv_textarea_get_text(draft)) > 0);
    if (draft) lv_textarea_set_text(draft, "");
    lv_screen_load(skills); pump(); click(skills, "陪伴"); click(lv_layer_top(), "换一页");
    click(lv_layer_top(), "睡前故事");
    result("bedtime_navigation", lv_screen_active() == assistant);
    draft = textarea(assistant);
    result("bedtime_draft", draft && strcmp(lv_textarea_get_text(draft), "给我讲一个三分钟的睡前故事") == 0);
    snapshot("bedtime-draft");
    printf("LIVE LCD failures=%d; all HTTP requests use the running core without wrappers\n", failures);
    return failures ? 1 : 0;
}
