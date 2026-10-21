/* 面板尺寸：布局缩放的唯一输入（见 include/scale.h）。
 *
 * 默认是编译期设定的 QZ_SCREEN_W/H（最小基准 320×240），启动时由
 * qz_scale_init_from_system() 换成**实际**的屏幕尺寸，于是同一份二进制在任意
 * 比例的屏上都铺满，不必为每块屏重编。 */
#include "scale.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#if !LV_USE_SIMULATOR
#include <linux/fb.h>
#include <sys/ioctl.h>
#endif

int32_t qz_panel_w = QZ_SCREEN_W;
int32_t qz_panel_h = QZ_SCREEN_H;

void qz_scale_init(int32_t panel_w, int32_t panel_h)
{
    if (panel_w > 0 && panel_h > 0) {
        qz_panel_w = panel_w;
        qz_panel_h = panel_h;
    }
}

void qz_scale_init_from_system(void)
{
#if LV_USE_SIMULATOR
    /* 模拟器里没有真面板：用 QZDESK_PANEL=320x240 在启动时挑一块屏来模拟，
     * 不设就用编译期尺寸。这样同一份模拟器二进制能直接看各种比例的效果。 */
    const char *panel = getenv("QZDESK_PANEL");
    int32_t w = QZ_SCREEN_W;
    int32_t h = QZ_SCREEN_H;

    if (!panel || sscanf(panel, "%dx%d", &w, &h) != 2) {
        w = QZ_SCREEN_W;
        h = QZ_SCREEN_H;
    }
    qz_scale_init(w, h);
#else
    /* 真机以 /dev/fb0 为准：LVGL 的 fbdev 驱动读同一个 ioctl（FBIOGET_VSCREENINFO）
     * 来设显示分辨率，所以缩放比例与显示分辨率必然一致。设备树换面板、分辨率变了，
     * 界面自动跟着铺满，不用重编。读不到就退回编译期尺寸。 */
    const char *fb = getenv("QZDESK_FB");
    int fd = open((fb && fb[0] != '\0') ? fb : "/dev/fb0", O_RDONLY);

    if (fd >= 0) {
        struct fb_var_screeninfo vinfo;
        if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) == 0) {
            qz_scale_init((int32_t)vinfo.xres, (int32_t)vinfo.yres);
        }
        close(fd);
    }
#endif
}

/* 见 include/scale.h 的"触控命中区"一节。
 *
 * 尺寸从**样式**读，不从 coords 读：构造函数里 lv_obj_set_size() 之后 coords
 * 还没算（lv_obj_get_width() 这时是 0），照 coords 算就会给每个控件无脑外扩
 * 一半下限，相邻按钮互相抢点击。样式里写死的宽高在 set_size 之后立刻可读，
 * 正好是调用点要的那个值；撑满父级 / 自适应内容（LV_COORD_IS_SPEC）的控件要
 * 等一次布局才知道多大，那就退回 coords。
 *
 * ext_click_pad 是**物理像素**、四边同一个数（lv_obj_get_click_area 用
 * lv_area_increase 对称外扩），所以这里算出的是一个 pad：取宽、高两个方向里
 * 补得多的那一侧。 */
static int32_t object_extent(lv_obj_t *obj, bool horizontal)
{
    lv_coord_t style = horizontal ? lv_obj_get_style_width(obj, LV_PART_MAIN)
                                  : lv_obj_get_style_height(obj, LV_PART_MAIN);
    if (!LV_COORD_IS_SPEC(style) && style > 0) return style;
    return horizontal ? lv_obj_get_width(obj) : lv_obj_get_height(obj);
}

int32_t qz_touch_target(lv_obj_t *obj)
{
    if (!obj) return 0;

    if (!lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE)) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    }

    const int32_t min = qz_touch_min();
    int32_t width = object_extent(obj, true);
    int32_t height = object_extent(obj, false);
    int32_t pad = 0;

    if (width > 0 && width < min) {
        int32_t needed = (min - width + 1) / 2;
        if (needed > pad) pad = needed;
    }
    if (height > 0 && height < min) {
        int32_t needed = (min - height + 1) / 2;
        if (needed > pad) pad = needed;
    }

    /* 调用点若已经手工扩过，不要缩回去：先读回当前命中区（ext_click_pad 是
     * 四边同一个数，左右取较大的一侧即可），只增不减。 */
    lv_area_t coords, area;
    lv_obj_get_coords(obj, &coords);
    lv_obj_get_click_area(obj, &area);
    int32_t current = 0;
    if (area.x1 < coords.x1) current = coords.x1 - area.x1;
    if (area.y1 < coords.y1 && coords.y1 - area.y1 > current) current = coords.y1 - area.y1;

    if (pad > current) {
        lv_obj_set_ext_click_area(obj, pad);
        return pad;
    }
    return current;
}
