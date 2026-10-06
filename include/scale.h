#ifndef QZDESK_SCALE_H
#define QZDESK_SCALE_H

#include "lvgl/lvgl.h"

/* ------------------------------------------------------------------------- *
 * 面板缩放：一份布局，两种屏
 *
 * 页面全部按 **480×320 的设计稿**写绝对坐标（这是它们本来的写法），这里把这些
 * 像素值在进入 LVGL 的边界上按面板比例统一缩放。于是换屏只是换个编译选项：
 *
 *     cmake -DQZDESK_PANEL_W=320 -DQZDESK_PANEL_H=240 …
 *
 * 为什么不在每个页面里改坐标：620 处字面量，漏一处就是一个错位的界面；而且
 * 《QZ_SCREEN_W - 2 * QZ_GUTTER》这类"面板尺寸 - 设计常量"的混合算式会算错。
 * 现在页面里只剩设计单位（见 theme.h 的 QZ_DESIGN_*），缩放只有这一层。
 *
 * 缩放系数取 min(面板宽/设计宽, 面板高/设计高) —— **等比**，宁可上下留白也不
 * 拉伸变形（480×320 与 320×240 并不是同一个比例：3:2 vs 4:3）。默认面板
 * 480×320 下系数正好是 1，所以这层对原有行为是恒等变换，逐像素与以前一致。
 *
 * 为什么不用 transform_scale 缩放根对象：LVGL 会把整棵子树渲染进 ARGB 图层再
 * 变换，全屏一层在设备堆上很难分配（docs/UI与设计.md 里已经写死不要这么做）。
 * ------------------------------------------------------------------------- */

#ifndef QZ_DESIGN_W
#define QZ_DESIGN_W 480
#endif
#ifndef QZ_DESIGN_H
#define QZ_DESIGN_H 320
#endif

#define QZ_SCALE_MIN(a, b) ((a) < (b) ? (a) : (b))

/* 缩放系数 = NUM / DEN（整数分数，避免浮点）。 */
#define QZ_SCALE_NUM \
    (QZ_SCALE_MIN((QZ_SCREEN_W) * (QZ_DESIGN_H), (QZ_SCREEN_H) * (QZ_DESIGN_W)))
#define QZ_SCALE_DEN ((QZ_DESIGN_W) * (QZ_DESIGN_H))

/** 字号下限：等比缩到 7px 的中文已经糊了，宁可略微不"等比"也要能读。 */
#ifndef QZ_MIN_FONT_PX
#define QZ_MIN_FONT_PX 9
#endif

/**
 * 把一个设计像素值换到当前面板上。
 *
 * `lv_pct()` / `LV_SIZE_CONTENT` 这类带标志位的"特殊坐标"原样放过
 * （`LV_COORD_IS_SPEC`），它们本来就是相对面板的，不该再缩。
 */
static inline lv_coord_t qz_scale_px(int32_t value)
{
    int64_t scaled;
    if (LV_COORD_IS_SPEC(value)) {
        return (lv_coord_t)value;
    }
    /* 四舍五入，且正负都往"离零更远"那侧补半格：C 的整除是向零截断的，
     * 若一律加半格，-12 在恒等变换（480 基准）下会算成 -11。 */
    scaled = (int64_t)value * QZ_SCALE_NUM;
    scaled += scaled >= 0 ? QZ_SCALE_DEN / 2 : -(QZ_SCALE_DEN / 2);
    return (lv_coord_t)(scaled / QZ_SCALE_DEN);
}

/** 字号缩放，带上限（见 QZ_MIN_FONT_PX）。 */
static inline int32_t qz_scale_font(int32_t px)
{
#if QZ_SCALE_NUM < QZ_SCALE_DEN
    /* 只有真的在缩小才设下限：默认 480×320 下面字号必须逐像素与以前一致。 */
    lv_coord_t scaled = qz_scale_px(px);
    return scaled < QZ_MIN_FONT_PX ? QZ_MIN_FONT_PX : (int32_t)scaled;
#else
    return px;
#endif
}

/**
 * `lv_obj_set_style_size(obj, w, h, sel)`：宽高一起设。
 *
 * 放在下面那批宏**之前**定义是故意的：这里要调用真正的 LVGL 函数，若放在宏之后
 * 就会被宏再缩一次（双重缩放）。
 */
static inline void qz_set_style_size(lv_obj_t *obj, int32_t width, int32_t height,
                                     lv_style_selector_t selector)
{
    lv_obj_set_style_width(obj, qz_scale_px(width), selector);
    lv_obj_set_style_height(obj, qz_scale_px(height), selector);
}

/* ------------------------------------------------------------------------- *
 * 拦截"像素入口"
 *
 * 只包装坐标与尺寸类 API/属性；百分比类（transform_scale、opa）与角度类
 * （lv_arc_set_angles、lv_arc_set_rotation）一概不碰。列表就是本仓库实际用到的
 * 那些，不做无用功。
 * ------------------------------------------------------------------------- */

#define lv_obj_align(obj, align, x, y) \
    lv_obj_align((obj), (align), qz_scale_px(x), qz_scale_px(y))
#define lv_obj_align_to(obj, base, align, x, y) \
    lv_obj_align_to((obj), (base), (align), qz_scale_px(x), qz_scale_px(y))
#define lv_obj_set_size(obj, w, h) lv_obj_set_size((obj), qz_scale_px(w), qz_scale_px(h))
#define lv_obj_set_width(obj, w) lv_obj_set_width((obj), qz_scale_px(w))
#define lv_obj_set_height(obj, h) lv_obj_set_height((obj), qz_scale_px(h))
#define lv_obj_set_x(obj, x) lv_obj_set_x((obj), qz_scale_px(x))
#define lv_obj_set_y(obj, y) lv_obj_set_y((obj), qz_scale_px(y))
#define lv_obj_set_pos(obj, x, y) lv_obj_set_pos((obj), qz_scale_px(x), qz_scale_px(y))

#define lv_obj_set_style_pad_all(obj, value, selector) \
    lv_obj_set_style_pad_all((obj), qz_scale_px(value), selector)
#define lv_obj_set_style_pad_top(obj, value, selector) \
    lv_obj_set_style_pad_top((obj), qz_scale_px(value), selector)
#define lv_obj_set_style_pad_bottom(obj, value, selector) \
    lv_obj_set_style_pad_bottom((obj), qz_scale_px(value), selector)
#define lv_obj_set_style_pad_left(obj, value, selector) \
    lv_obj_set_style_pad_left((obj), qz_scale_px(value), selector)
#define lv_obj_set_style_pad_right(obj, value, selector) \
    lv_obj_set_style_pad_right((obj), qz_scale_px(value), selector)
#define lv_obj_set_style_pad_row(obj, value, selector) \
    lv_obj_set_style_pad_row((obj), qz_scale_px(value), selector)
#define lv_obj_set_style_pad_column(obj, value, selector) \
    lv_obj_set_style_pad_column((obj), qz_scale_px(value), selector)

#define lv_obj_set_style_radius(obj, value, selector) \
    lv_obj_set_style_radius((obj), qz_scale_px(value), selector)
#define lv_obj_set_style_width(obj, value, selector) \
    lv_obj_set_style_width((obj), qz_scale_px(value), selector)
#define lv_obj_set_style_height(obj, value, selector) \
    lv_obj_set_style_height((obj), qz_scale_px(value), selector)
#define lv_obj_set_style_min_width(obj, value, selector) \
    lv_obj_set_style_min_width((obj), qz_scale_px(value), selector)
#define lv_obj_set_style_min_height(obj, value, selector) \
    lv_obj_set_style_min_height((obj), qz_scale_px(value), selector)
#define lv_obj_set_style_max_width(obj, value, selector) \
    lv_obj_set_style_max_width((obj), qz_scale_px(value), selector)
#define lv_obj_set_style_max_height(obj, value, selector) \
    lv_obj_set_style_max_height((obj), qz_scale_px(value), selector)
#define lv_obj_set_style_border_width(obj, value, selector) \
    lv_obj_set_style_border_width((obj), qz_scale_px(value), selector)
#define lv_obj_set_style_outline_width(obj, value, selector) \
    lv_obj_set_style_outline_width((obj), qz_scale_px(value), selector)
#define lv_obj_set_style_arc_width(obj, value, selector) \
    lv_obj_set_style_arc_width((obj), qz_scale_px(value), selector)
#define lv_obj_set_style_line_width(obj, value, selector) \
    lv_obj_set_style_line_width((obj), qz_scale_px(value), selector)
#define lv_obj_set_style_shadow_width(obj, value, selector) \
    lv_obj_set_style_shadow_width((obj), qz_scale_px(value), selector)
#define lv_obj_set_style_shadow_offset_x(obj, value, selector) \
    lv_obj_set_style_shadow_offset_x((obj), qz_scale_px(value), selector)
#define lv_obj_set_style_shadow_offset_y(obj, value, selector) \
    lv_obj_set_style_shadow_offset_y((obj), qz_scale_px(value), selector)
#define lv_obj_set_style_translate_x(obj, value, selector) \
    lv_obj_set_style_translate_x((obj), qz_scale_px(value), selector)
#define lv_obj_set_style_translate_y(obj, value, selector) \
    lv_obj_set_style_translate_y((obj), qz_scale_px(value), selector)
#define lv_obj_set_style_text_line_space(obj, value, selector) \
    lv_obj_set_style_text_line_space((obj), qz_scale_px(value), selector)
#define lv_obj_set_style_text_letter_space(obj, value, selector) \
    lv_obj_set_style_text_letter_space((obj), qz_scale_px(value), selector)

#define lv_obj_set_style_size(obj, w, h, selector) qz_set_style_size((obj), (w), (h), selector)

#endif
