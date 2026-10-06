#ifndef QZDESK_THEME_H
#define QZDESK_THEME_H

#include "lvgl/lvgl.h"
#include "icon_assets.h"
#include <stdbool.h>

/* ------------------------------------------------------------------------- *
 * Canvas / layout
 *
 * 面板尺寸可以在编译期换成同比例的小屏（见 include/scale.h）：
 *
 *     cmake -DQZDESK_PANEL_W=320 -DQZDESK_PANEL_H=240 …
 *
 * 页面里写的坐标与尺寸一律是 480×320 设计稿的像素（QZ_DESIGN_W/H），由 scale.h
 * 统一缩放到当前面板。所以除这里与 scale.h 之外，**不要再用 QZ_SCREEN_W/H 参与
 * 布局计算** —— 那会把「面板尺寸」和「设计常量」混在一起，缩放出错。
 * ------------------------------------------------------------------------- */

#ifndef QZ_SCREEN_W
#define QZ_SCREEN_W 480
#endif
#ifndef QZ_SCREEN_H
#define QZ_SCREEN_H 320
#endif

#include "scale.h"

#define QZ_GUTTER 14
#define QZ_STATUS_H 30
#define QZ_TOOLBAR_H 44

/* Radii: cards use an absolute pixel corner (iOS grouped style ≈ 10), icon
 * tiles use a percentage of their size (≈ 22%, the squircle-ish ratio). */
#define QZ_RADIUS_CARD 10
#define QZ_RADIUS_ICON 22
#define QZ_ROW_ICON 30   /**< icon tile size in a grouped list row */

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
    QZ_CARD,
    QZ_BAR,
    QZ_FILL,
    QZ_FILL_PRESSED,
    QZ_SEPARATOR,
    QZ_TEXT,
    QZ_TEXT_SECONDARY,
    QZ_TEXT_TERTIARY,
    QZ_SHADOW,
    QZ_FACE_BUBBLE,
    QZ_KNOB,        /**< slider / switch knob fill on top of glass */
    QZ_GLASS_RIM,   /**< specular rim on glass chrome */
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
} qz_color_token_t;

/** Look up the current colour for a token. Thread safe; only reads g_dark. */
lv_color_t qz_color(qz_color_token_t token);

/** True when the interface is in dark mode. */
bool qz_theme_is_dark(void);
/**
 * Switch the active theme. The new palette is applied to every screen and
 * widget immediately (the active screen is reloaded with a cross fade so the
 * change reads as a single motion). Returns true if the state actually
 * changed.
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
#define QZ_MATERIAL_PERCENT_DEFAULT 68
#define QZ_RIM_OPA 230

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
void qz_obj_set_text_color(lv_obj_t *obj, qz_color_token_t token, lv_style_selector_t sel);
void qz_obj_set_border_color(lv_obj_t *obj, qz_color_token_t token, lv_style_selector_t sel);
void qz_obj_set_shadow_color(lv_obj_t *obj, qz_color_token_t token, lv_style_selector_t sel);

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
void qz_style_toolbar(lv_obj_t *obj);
void qz_style_row(lv_obj_t *obj);
void qz_style_slider(lv_obj_t *obj);
void qz_style_switch(lv_obj_t *obj);
void qz_style_primary_button(lv_obj_t *obj);
void qz_style_textarea(lv_obj_t *obj);
/** iOS looking on-screen keyboard: white keys, grey function keys. */
void qz_style_keyboard(lv_obj_t *obj);
void qz_add_press_feedback(lv_obj_t *obj);
/**
 * Specular highlight that follows the finger: a soft glint drawn in the top
 * layer at the touch point while the object is held. This is how the "light
 * moves across the glass" cue is reproduced without real-time refraction.
 */
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
lv_obj_t *qz_icon_button(lv_obj_t *parent, const char *icon, int size);
/** Grouped plate: opaque surface, no border, no shadow. */
void qz_style_plate(lv_obj_t *obj);
/** Rounded tappable plate used across the pages: opaque surface, no shadow. */
lv_obj_t *qz_card_button(lv_obj_t *parent, int width, int height);
/** The floating material: translucent surface, specular rim, neutral shadow.
 * Only for chrome that sits above content (toolbars, composer, overlays). */
void qz_style_glass(lv_obj_t *obj);
/** Flat squircles icon tile in one colour — no gradient. */
lv_obj_t *qz_squircle(lv_obj_t *parent, int size, qz_color_token_t color);
/** Rounded vertical/horizontal hairline. */
lv_obj_t *qz_separator(lv_obj_t *parent, int length, bool vertical);
/** Thin iOS disclosure chevron pointing right. */
lv_obj_t *qz_chevron(lv_obj_t *parent, lv_color_t color, int32_t size);
/** Thin top-half arc, used for locks and mic cradles. */
lv_obj_t *qz_arc_piece(lv_obj_t *parent, int32_t width, int32_t height, int32_t start,
                       int32_t end, int32_t thickness, lv_color_t color);

/* ------------------------------------------------------------------------- *
 * Overlays
 * ------------------------------------------------------------------------- */
lv_obj_t *qz_overlay_create(lv_obj_t *parent);
void qz_overlay_attach_dismiss(lv_obj_t *overlay);

/* ------------------------------------------------------------------------- *
 * Motion
 * ------------------------------------------------------------------------- */
#define QZ_DUR_PRESS 120  /**< press feedback (100-160ms) */
#define QZ_DUR_QUICK 140  /**< peer switches seen tens of times a day (<150ms) */
#define QZ_DUR_SMALL 180  /**< chips, tooltips (125-200ms) */
#define QZ_DUR_PANEL 220  /**< panels and popovers (150-250ms) */
#define QZ_DUR_MODAL 250  /**< modal + scrim, animated as one surface */
#define QZ_DUR_SCREEN 260 /**< page transition */
#define QZ_DUR_TOAST 240  /**< toast in/out */

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

#endif
