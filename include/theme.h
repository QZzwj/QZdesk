#ifndef QZDESK_THEME_H
#define QZDESK_THEME_H

#include "lvgl/lvgl.h"
#include "icon_assets.h"
#include <stdbool.h>

/* ------------------------------------------------------------------------- *
 * Canvas / layout
 *
 * 面板尺寸在运行时按实际屏幕取（见 include/scale.h）：真机是 /dev/fb0 报的尺寸，
 * 模拟器是 QZDESK_PANEL=320x240（不设则用编译期 QZ_SCREEN_W/H）。
 *
 * 页面里写的坐标与尺寸一律是 480×320 设计稿的像素（QZ_DESIGN_W/H），由 scale.h
 * 统一缩放到当前面板。所以除这里与 scale.h 之外，**不要再用 QZ_SCREEN_W/H 参与
 * 布局计算** —— 那会把「面板尺寸」和「设计常量」混在一起，缩放出错。
 * ------------------------------------------------------------------------- */

#ifndef QZ_SCREEN_W
#define QZ_SCREEN_W 320
#endif
#ifndef QZ_SCREEN_H
#define QZ_SCREEN_H 240
#endif

#include "scale.h"

#define QZ_GUTTER 14
#define QZ_STATUS_H 52
#define QZ_TOOLBAR_H 44
/** 全宽页头的高度：五个页面与各二级页统一用这一条。52 = 44 的返回键 + 上下各
 *  4 的呼吸位，正好让返回键的圆落在页头里而不贴到发丝线上。 */
#define QZ_HEADER_H 52

/* Signal-workstation geometry: utility surfaces stay nearly square, while
 * icon tiles keep a small mechanical radius rather than a squircle. */
#define QZ_RADIUS_CARD 6
#define QZ_RADIUS_ICON 18
#define QZ_ROW_ICON 34   /**< icon tile size in a grouped list row */

/* ------------------------------------------------------------------------- *
 * Design tokens
 *
 * Each named "token" no longer names a colour value — it names a *role*. The
 * palette under each role changes between light and dark themes; the
 * surrounding code only ever asks for "the colour of QZ_BG" and gets the right
 * one back. So a screen that draws a grey canvas and a white plate in light
 * mode draws a true-black canvas and a #1C1C1E plate in dark mode without a
 * single change at the call site.
 *
 * The palette in app/theme.c groups them:
 *   - surfaces (BG / CARD / FILL / SEPARATOR / SHADOW / FACE_BUBBLE /
 *     KNOB / GLASS_RIM) are theme dependent.
 *   - accents and semantic colours (systemBlue and the dark/light/tint/on-acc
 *     steps, plus green / orange / red) are theme aware but the brand hue
 *     stays the same; only the brightness step changes for accent so the
 *     accent text still hits 4.5:1 on the dark plate.
 * ------------------------------------------------------------------------- */
typedef enum {
    /* surfaces — follow the theme */
    QZ_BG = 0,
    QZ_BG_GRAD,     /**< canvas vertical-gradient bottom stop（玻璃需要底子有层次） */
    QZ_CARD,
    QZ_CARD_GRAD,   /**< plate vertical-gradient bottom stop */
    QZ_BAR,
    QZ_FILL,
    QZ_FILL_PRESSED,
    QZ_SEPARATOR,
    QZ_TEXT,
    QZ_TEXT_SECONDARY,
    QZ_TEXT_TERTIARY,
    QZ_SHADOW,
    QZ_FACE_BUBBLE,
    QZ_KNOB,        /**< slider / switch knob fill on top of the track */
    QZ_GLASS_RIM,   /**< hairline around raised chrome */
    /* accents & semantics — same hue in both themes, the dark step is brighter
     * on dark so accent text still clears 4.5:1 on the dark plate. */
    QZ_ACCENT,
    QZ_ACCENT_DARK,
    QZ_ACCENT_TEXT,
    QZ_ACCENT_TINT,
    QZ_TEXT_ON_ACCENT,
    QZ_GREEN,
    QZ_ORANGE,
    QZ_RED,
    QZ_COLOR_COUNT,
} qz_color_token_t;

/* Four complete palettes rather than a light/dark switch. The first two are
 * the original mineral pair and stay the compatibility defaults; the other two
 * give the device visibly different working moods without changing layout. */
typedef enum {
    QZ_THEME_MINERAL = 0,
    QZ_THEME_NIGHT,
    QZ_THEME_PAPER,
    QZ_THEME_EMBER,
    QZ_THEME_COUNT,
} qz_theme_id_t;

/** Look up the current colour for a token. Thread safe; only reads g_dark. */
lv_color_t qz_color(qz_color_token_t token);
/** Look up a token for a specific mode. Used by theme previews, which must keep
 *  showing the real light and dark palettes at the same time. */
lv_color_t qz_color_mode(qz_color_token_t token, bool dark);
/** Look up a token in one of the four complete palettes. */
lv_color_t qz_color_theme(qz_color_token_t token, qz_theme_id_t theme);

/** Active palette and its metadata. Invalid values fall back to MINERAL. */
qz_theme_id_t qz_theme_get_id(void);
bool qz_theme_id_is_dark(qz_theme_id_t theme);
const char *qz_theme_id_key(qz_theme_id_t theme);
const char *qz_theme_id_name(qz_theme_id_t theme);
const char *qz_theme_id_caption(qz_theme_id_t theme);
bool qz_theme_parse(const char *value, qz_theme_id_t *theme);

/** True when the interface is in dark mode. */
bool qz_theme_is_dark(void);
/** Switch to a complete palette. Persistence is the caller's job. */
bool qz_theme_set(qz_theme_id_t theme);
/**
 * Switch the active theme. The new palette is applied to every screen and
 * widget immediately. Unless reduced motion is requested, a short accent wash
 * bridges the two palettes without rebuilding or cross-fading the screen.
 * Returns true if the state actually changed.
 *
 * Persistence is the caller's job — this only updates the live UI.
 */
bool qz_theme_set_dark(bool dark);

/* ------------------------------------------------------------------------- *
 * Material opacity — yours to set
 *
 * One value drives every white surface, so the whole interface can be dialled
 * from solid to almost clear while it runs. Surfaces are tagged with a user
 * flag when they are styled; the tag is what makes a live change reach them
 * without rebuilding the screen.
 * ------------------------------------------------------------------------- */
#define QZ_MATERIAL_PERCENT_DEFAULT 100
#define QZ_RIM_OPA 210

/** Read the env vars once, before any screen is built. */
void qz_style_init(void);
/** Opacity of the plates, 0..255. */
int qz_material_opa(void);
/** Opacity of the floating chrome, 0..255. */
int qz_material_glass_opa(void);
/** The current value as the slider shows it, 0..100. */
int qz_material_percent(void);
/** Live: sets the material and repaints every surface on the screen. */
bool qz_material_set_percent(int percent);
/** Mark a surface so the live control reaches it. `floating` = chrome. Most
 *  callers get this from qz_style_plate()/qz_style_glass() instead. */
void qz_material_register(lv_obj_t *obj, bool floating);

/* ------------------------------------------------------------------------- *
 * Style setters that the theme knows about
 *
 * These do the same thing as their lv_obj_set_style_*_color() counterparts
 * but they also remember which token was used so the theme can refresh the
 * colour on every recorded widget when the palette changes. Use them in place
 * of lv_obj_set_style_bg_color() etc. when the colour comes from a token.
 * ------------------------------------------------------------------------- */
void qz_obj_set_bg_color(lv_obj_t *obj, qz_color_token_t token, lv_style_selector_t sel);
/** 同上，但作用于渐变的末端色（配合 bg_grad_dir = VER 的纵向微渐变）。 */
void qz_obj_set_bg_grad_color(lv_obj_t *obj, qz_color_token_t token, lv_style_selector_t sel);
void qz_obj_set_text_color(lv_obj_t *obj, qz_color_token_t token, lv_style_selector_t sel);
void qz_obj_set_border_color(lv_obj_t *obj, qz_color_token_t token, lv_style_selector_t sel);
void qz_obj_set_shadow_color(lv_obj_t *obj, qz_color_token_t token, lv_style_selector_t sel);
void qz_obj_set_image_recolor(lv_obj_t *obj, qz_color_token_t token, lv_style_selector_t sel);
void qz_obj_set_line_color(lv_obj_t *obj, qz_color_token_t token, lv_style_selector_t sel);
void qz_obj_set_arc_color(lv_obj_t *obj, qz_color_token_t token, lv_style_selector_t sel);
/** Chart series live outside object styles, so track their colour separately. */
void qz_obj_set_chart_series_color(lv_obj_t *chart, lv_chart_series_t *series,
                                   qz_color_token_t token);

/* ------------------------------------------------------------------------- *
 * Typography
 * ------------------------------------------------------------------------- */
void qz_font_init(void);
/** Default body font (16px). */
const lv_font_t *qz_font(void);
/** CJK font at an arbitrary pixel size (falls back to Montserrat). */
const lv_font_t *qz_font_size(int32_t px);
/** Built-in icon font (LV_SYMBOL_*) at an arbitrary pixel size. */
const lv_font_t *qz_symbol_font(int32_t px);

/** True when the user asked for reduced motion (QZDESK_REDUCE_MOTION=1). */
bool qz_reduce_motion(void);

/* ------------------------------------------------------------------------- *
 * Widget builders
 * ------------------------------------------------------------------------- */
void qz_style_screen(lv_obj_t *obj);
/** 全宽页头：玻璃材质、直角贴边、底部一根发丝线。 */
void qz_style_header(lv_obj_t *obj);
/** 建一条标准页头（高 QZ_HEADER_H），标题靠左排在返回键右边；调用方再往上
 *  加返回键与右侧动作。返回的就是页头对象本身。 */
lv_obj_t *qz_page_header(lv_obj_t *screen, const char *title);
void qz_style_toolbar(lv_obj_t *obj);
void qz_style_row(lv_obj_t *obj);
void qz_style_slider(lv_obj_t *obj);
void qz_style_switch(lv_obj_t *obj);
void qz_style_primary_button(lv_obj_t *obj);
void qz_style_textarea(lv_obj_t *obj);
/** Workstation keyboard: flat keys, function keys differentiated by fill. */
void qz_style_keyboard(lv_obj_t *obj);
void qz_add_press_feedback(lv_obj_t *obj);
/** A precise top-layer highlight that follows the finger. */
void qz_add_touch_glint(lv_obj_t *obj);

lv_obj_t *qz_text(lv_obj_t *parent, const char *text, int32_t px, lv_color_t text_color);
lv_obj_t *qz_symbol(lv_obj_t *parent, const char *symbol, int32_t px, lv_color_t color);

/**
 * One of the bundled Tabler glyphs (A8 mask), tinted with a theme colour.
 *
 * The returned widget is an `lv_image`, not a label: never call
 * `lv_label_set_text` on it. `qz_symbol` funnels through here for every symbol
 * it recognises, so most call sites get the new icon set without changes.
 */
lv_obj_t *qz_icon_image(lv_obj_t *parent, qz_icon_t icon, int32_t px, lv_color_t color);
lv_obj_t *qz_button(lv_obj_t *parent, const char *caption, int width, int height);
/** 消息页/记录页的"点一下就发"建议 chip：浅底、圆角胶囊、强调色文字。 */
lv_obj_t *qz_chip(lv_obj_t *parent, const char *caption, int width, int height);
lv_obj_t *qz_icon_button(lv_obj_t *parent, const char *icon, int size);
/** Grouped plate: opaque surface, no border, no shadow. */
void qz_style_plate(lv_obj_t *obj);
/** Rounded tappable plate used across the pages: opaque surface, no shadow. */
lv_obj_t *qz_card_button(lv_obj_t *parent, int width, int height);

/* ---- 小屏触控件 ----------------------------------------------------------
 * 设计像素在 320×240 面板上会缩到 2/3，所以"30 设计像素"的旧按钮只有 20 实像素，
 * 手指点不准。下面这几件按"实机也要够大"来定尺寸，各页统一用它们。 */
/** 返回键：直径 44 设计像素（实机 29px），可点区域再外扩 8。 */
#define QZ_TOUCH_BACK 44
lv_obj_t *qz_back_button(lv_obj_t *parent);

/** 可下滑的页面内容区：从 top 一直到底部留白，竖向可滑、溢出自动出滚动条。 */
lv_obj_t *qz_page_scroll(lv_obj_t *screen, int32_t top);

/** 把已有的内容容器变成竖向可下滑（尺寸不变，只开滚动 + 弹性/惯性）。
 *  各页把卡片摆死在一个整屏容器里时，调它一行就适配小屏。 */
void qz_make_scrollable(lv_obj_t *obj);

/** 2 列网格的格子尺寸（宽、高），与 qz_grid_pos 配套。 */
void qz_grid_metrics(int32_t *tile_w, int32_t *tile_h);
/** 2 列网格里第 (col,row) 格的左上角坐标（相对内容区）。col/row 从 0 起。 */
void qz_grid_pos(int32_t col, int32_t row, int32_t *x, int32_t *y);
/** Flat raised chrome: solid surface and one hairline, no blur or shadow. */
void qz_style_glass(lv_obj_t *obj);
/** Flat icon tile in one colour, no gradient. */
lv_obj_t *qz_squircle(lv_obj_t *parent, int size, qz_color_token_t color);
/** Rounded vertical/horizontal hairline. */
lv_obj_t *qz_separator(lv_obj_t *parent, int length, bool vertical);
/** Thin disclosure chevron pointing right. */
lv_obj_t *qz_chevron(lv_obj_t *parent, lv_color_t color, int32_t size);
/** Thin top-half arc, used for locks and mic cradles. */
lv_obj_t *qz_arc_piece(lv_obj_t *parent, int32_t width, int32_t height, int32_t start,
                       int32_t end, int32_t thickness, lv_color_t color);

/* ------------------------------------------------------------------------- *
 * Overlays
 * ------------------------------------------------------------------------- */
lv_obj_t *qz_overlay_create(lv_obj_t *parent);
void qz_overlay_attach_dismiss(lv_obj_t *overlay);
/** 遮罩上弹出的面板：不透明材质 + 6px 圆角 + 发丝边框，全应用统一。 */
void qz_style_sheet(lv_obj_t *panel);
/** 现在有没有遮罩/弹层盖在页面上：手势据此把返回让给弹层自己处理。 */
bool qz_overlay_open(void);
/** 关闭当前可见的最上层弹层；没有可关闭的弹层时返回 false。 */
bool qz_overlay_close_top(void);

/* ------------------------------------------------------------------------- *
 * Motion
 * ------------------------------------------------------------------------- */
#define QZ_DUR_PRESS 120  /**< press feedback (100-160ms) */
#define QZ_DUR_QUICK 140  /**< peer switches seen tens of times a day (<150ms) */
#define QZ_DUR_SMALL 180  /**< chips, tooltips (125-200ms) */
#define QZ_DUR_PANEL 220  /**< panels and popovers (150-250ms) */
#define QZ_DUR_MODAL 250  /**< modal + scrim, animated as one surface */
#define QZ_DUR_SCREEN 200 /**< page transition: fast, eased, never jump-cut */
#define QZ_DUR_TOAST 240  /**< toast in/out */
#define QZ_DUR_THEME 260  /**< palette wash: brief accent in, soft settle out */

/** Strong ease-out, cubic-bezier(0.23, 1, 0.32, 1): entering / exiting. */
void qz_anim_ease_out(lv_anim_t *anim);
/** Strong ease-in-out, cubic-bezier(0.77, 0, 0.175, 1): on-screen movement. */
void qz_anim_ease_in_out(lv_anim_t *anim);
/**
 * Press feedback for surfaces a finger lands on: scale down on touch-down and
 * back on release, on top of the colour change. Use it for small controls only
 * — LVGL draws a scaled object through an ARGB layer, so a full width card is
 * not a candidate.
 */
void qz_add_press_scale(lv_obj_t *obj, int32_t percent);
void qz_animate_entrance(lv_obj_t *obj, uint32_t delay_ms);
/** Fade + slide in, used when swapping a page section. */
void qz_animate_pop_in(lv_obj_t *obj);
void qz_screen_load(lv_obj_t *screen, lv_scr_load_anim_t anim, uint32_t duration_ms);
/** 退回上一页；没有可退的页就什么都不做。 */
void qz_screen_back(void);
/** 让某一页接管返回（例如对话页要先收键盘），侧滑返回会先问它。 */
void qz_screen_set_back_handler(lv_obj_t *screen, void (*handler)(void));

#endif
