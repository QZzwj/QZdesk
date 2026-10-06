/* 面板尺寸：布局缩放的唯一输入（见 include/scale.h）。
 *
 * 默认是编译期设定的 QZ_SCREEN_W/H（也就等于设计稿 480×320），启动时由
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
