#include "apps.h"
#include "applets.h"
#include "assistant.h"
#include "performance_page.h"
#include "skills_page.h"
#include "config.h"
#include "desktop.h"
#include "settings.h"
#include "theme.h"
#include "qzdesk_core_process.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#if LV_USE_SIMULATOR
#include <SDL2/SDL.h>
#endif

/* 显示与触摸节点都允许用环境变量覆盖：设备上多一个输入设备就可能把触摸从
 * event0 挤走，改自启脚本（QZDESK_TOUCH=/dev/input/event1）即可，不必重编。 */
#define TOUCH_DEVICE_DEFAULT "/dev/input/event0"
#define FB_DEVICE_DEFAULT "/dev/fb0"

static const char *qz_env_or(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return (value && value[0] != '\0') ? value : fallback;
}
int main(void)
{
    char server[96] = "未配置";
    /* 面板尺寸先定下来：真机读 /dev/fb0、模拟器读 QZDESK_PANEL（不设则用编译期
     * 尺寸）。下面的字体、样式与全部布局都按它缩放，所以必须放在最前面。 */
    qz_scale_init_from_system();
    qz_load_config(server, sizeof(server));
    lv_init();
    qz_font_init();
    qz_style_init();
#if LV_USE_SIMULATOR
    lv_sdl_window_create(qz_panel_w, qz_panel_h);
    lv_sdl_mouse_create();
    /* The UI is touch driven, so the OS pointer only occludes it (and on a
     * desktop without a cursor theme it shows up as a solid block). */
    SDL_ShowCursor(SDL_DISABLE);
#else
    lv_display_t *display = lv_linux_fbdev_create();
    lv_linux_fbdev_set_file(display, qz_env_or("QZDESK_FB", FB_DEVICE_DEFAULT));
    lv_indev_t *touch = lv_evdev_create(LV_INDEV_TYPE_POINTER,
                                        qz_env_or("QZDESK_TOUCH", TOUCH_DEVICE_DEFAULT));
    if (touch) lv_indev_set_display(touch, display);
#endif

    /* Brand the default LVGL theme so unstyled widgets follow the same
     * primary blue as the custom styled ones. */
    lv_theme_default_init(lv_display_get_default(), qz_color(QZ_ACCENT),
                          qz_color(QZ_ACCENT_DARK), false, qz_font());
    lv_obj_t *desktop = qz_desktop_create(server);
    lv_obj_t *apps = qz_apps_create();
    lv_obj_t *assistant = qz_assistant_create();
    lv_obj_t *settings = qz_settings_create();
    lv_obj_t *skills = qz_skill_page_create(apps);
    lv_obj_t *performance = qz_performance_create(apps);
    /* Bind the GUI UDP port before launching the core so its first activation
     * packet cannot be lost during process startup. */
    qzdesk_core_start();
    qz_desktop_set_assistant(assistant);
    qz_desktop_set_apps(apps);
    qz_desktop_set_settings(settings);
    qz_apps_set_desktop(desktop);
    qz_apps_set_skill_screen(skills);
    qz_apps_set_performance_screen(performance);
    qz_applets_init(apps);
    qz_assistant_set_desktop(desktop);
    qz_settings_set_desktop(desktop);
    lv_screen_load(desktop);
    /* 免触摸调试口：QZDESK_OPEN=camera|pomodoro|apps… 启动后直接打开对应页，
     * 无头截图 / 自动化验证用。取 applet 枚举小写名，桌面/助手页也认。 */
    {
        const char *open = getenv("QZDESK_OPEN");
        if (open && *open) {
            if (strcmp(open, "camera") == 0) {
                lv_screen_load(qz_applets_screen(QZ_APPLET_CAMERA));
            } else if (strcmp(open, "apps") == 0) {
                lv_screen_load(apps);
            }
        }
    }
    while (!qzdesk_core_should_stop() && !qzdesk_core_restart_requested()) {
        lv_timer_handler();
        usleep(5000);
    }
    qzdesk_core_stop();
    return 0;
}
