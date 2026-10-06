#include "theme.h"
#include "icon_assets.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 字体是运行时从文件加载的（没有烘进二进制），所以设备上必须把 TTF 一起
 * 部署：SDK 侧装在 oem 分区的 /oem/usr/share/fonts 下。候选按顺序尝试，
 * 开发机上走第一条，设备上走第二条，两边都不必重编。 */
#define QZ_FONT_PATH "/home/jn/QZdesk/NanoTikBazHei-Bold.ttf"
#define QZ_FONT_SLOTS 20

static const char *qz_font_paths[] = {
    QZ_FONT_PATH,
    "/oem/usr/share/fonts/NanoTikBazHei-Bold.ttf",
    "/usr/share/fonts/NanoTikBazHei-Bold.ttf",
};

/* ------------------------------------------------------------------------- *
 * Theme palette
 *
 * The tokens in theme.h name roles, not colours. The table here gives each
 * role a light value and a dark value; qz_color() returns the one for the
 * active theme. Surfaces (canvas / plate / fill / separator / shadow /
 * face bubble / knob / glass rim) flip between the two sets; accents and
 * semantic colours keep their hue and only step the brightness so the accent
 * text still clears 4.5:1 on the dark plate.
 * ------------------------------------------------------------------------- */
static const struct {
    uint32_t light;
    uint32_t dark;
} qz_palette[] = {
    [QZ_BG]             = { 0xE5E5EA, 0x000000 }, /* canvas: systemGray5 / true black (OLED) */
    [QZ_CARD]           = { 0xFFFFFF, 0x1C1C1E }, /* grouped plate: white / systemGray6 dark */
    [QZ_BAR]            = { 0xFFFFFF, 0x1C1C1E }, /* toolbar glass fill */
    [QZ_FILL]           = { 0xF2F2F7, 0x2C2C2E }, /* recessed fill / track */
    [QZ_FILL_PRESSED]   = { 0xE9E9EF, 0x3A3A3C }, /* pressed / switch-off track */
    [QZ_SEPARATOR]      = { 0xC6C6C8, 0x38383A }, /* hairline */
    [QZ_TEXT]           = { 0x000000, 0xFFFFFF },
    [QZ_TEXT_SECONDARY] = { 0x6E6E73, 0xEBEBF5 }, /* 60% label on dark reads as #919195 */
    [QZ_TEXT_TERTIARY]  = { 0x8E8E93, 0x8E8E93 }, /* same neutral grey in both themes */
    [QZ_SHADOW]         = { 0x1C1C1E, 0x000000 },
    [QZ_FACE_BUBBLE]    = { 0xE4EFFF, 0x2C2C2E }, /* AI thinking pips */
    [QZ_KNOB]           = { 0xFFFFFF, 0xFFFFFF }, /* slider / switch knob stays white */
    [QZ_GLASS_RIM]      = { 0xFFFFFF, 0xFFFFFF }, /* specular rim on glass chrome */
    [QZ_ACCENT]         = { 0x007AFF, 0x0A84FF }, /* systemBlue, brighter on dark */
    [QZ_ACCENT_DARK]    = { 0x0062CC, 0x409CFF }, /* pressed accent + accent text on tint */
    [QZ_ACCENT_TEXT]    = { 0x0062CC, 0x409CFF },
    [QZ_ACCENT_TINT]    = { 0xE4EFFF, 0x1C2C4A }, /* 10% accent wash under badges */
    [QZ_TEXT_ON_ACCENT] = { 0xFFFFFF, 0xFFFFFF },
    [QZ_GREEN]          = { 0x248A3D, 0x30D158 }, /* systemGreen */
    [QZ_ORANGE]         = { 0xC93400, 0xFF9F0A }, /* systemOrange */
    [QZ_RED]            = { 0xD70015, 0xFF453A }, /* systemRed */
};

static bool g_dark_mode;

bool qz_theme_is_dark(void) { return g_dark_mode; }

lv_color_t qz_color(qz_color_token_t token)
{
    /* Stay safe if a caller passes a raw int outside the enum range. */
    if ((unsigned)token >= sizeof(qz_palette) / sizeof(qz_palette[0])) {
        return lv_color_black();
    }
    return lv_color_hex(g_dark_mode ? qz_palette[token].dark : qz_palette[token].light);
}

/* ------------------------------------------------------------------------- *
 * Tracked style records
 *
 * Every qz_obj_set_*_color() call below both applies the colour and remembers
 * (obj, prop, selector, token). When the theme changes, qz_theme_refresh_all()
 * walks the list and re-applies every colour from the new palette, so a screen
 * built once under light mode recolours itself in place under dark mode — no
 * rebuild, no state loss. Records are pruned when their object is deleted.
 * ------------------------------------------------------------------------- */
typedef struct qz_style_record_s {
    lv_obj_t *obj;
    lv_style_prop_t prop;
    lv_style_selector_t selector;
    qz_color_token_t token;
    struct qz_style_record_s *next;
} qz_style_record_t;

static qz_style_record_t *g_style_records;

static void track_record(lv_obj_t *obj, lv_style_prop_t prop,
                         lv_style_selector_t sel, qz_color_token_t token)
{
    /* Reuse an existing record for the same (obj, prop, sel) triple so a
     * restyle doesn't grow the list. */
    qz_style_record_t *rec = g_style_records;
    while (rec) {
        if (rec->obj == obj && rec->prop == prop && rec->selector == sel) {
            rec->token = token;
            return;
        }
        rec = rec->next;
    }
    rec = (qz_style_record_t *)lv_malloc(sizeof(*rec));
    if (!rec) return;
    rec->obj = obj;
    rec->prop = prop;
    rec->selector = sel;
    rec->token = token;
    rec->next = g_style_records;
    g_style_records = rec;
}

static void track_delete(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_target(event);
    qz_style_record_t **link = &g_style_records;
    while (*link) {
        qz_style_record_t *rec = *link;
        if (rec->obj == obj) {
            *link = rec->next;
            lv_free(rec);
        } else {
            link = &rec->next;
        }
    }
}

static void ensure_delete_hook(lv_obj_t *obj)
{
    /* LV_EVENT_DELETE is delivered once per object, so a single hook is enough.
     * Adding it every time is harmless but wasteful — track whether we have. */
    static void *marker = (void *)0xDEAD;
    if (lv_obj_get_user_data(obj) == marker) return;
    lv_obj_set_user_data(obj, marker);
    lv_obj_add_event_cb(obj, track_delete, LV_EVENT_DELETE, NULL);
}

void qz_obj_set_bg_color(lv_obj_t *obj, qz_color_token_t token, lv_style_selector_t sel)
{
    lv_obj_set_style_bg_color(obj, qz_color(token), sel);
    track_record(obj, LV_STYLE_BG_COLOR, sel, token);
    ensure_delete_hook(obj);
}

void qz_obj_set_text_color(lv_obj_t *obj, qz_color_token_t token, lv_style_selector_t sel)
{
    lv_obj_set_style_text_color(obj, qz_color(token), sel);
    track_record(obj, LV_STYLE_TEXT_COLOR, sel, token);
    ensure_delete_hook(obj);
}

void qz_obj_set_border_color(lv_obj_t *obj, qz_color_token_t token, lv_style_selector_t sel)
{
    lv_obj_set_style_border_color(obj, qz_color(token), sel);
    track_record(obj, LV_STYLE_BORDER_COLOR, sel, token);
    ensure_delete_hook(obj);
}

void qz_obj_set_shadow_color(lv_obj_t *obj, qz_color_token_t token, lv_style_selector_t sel)
{
    lv_obj_set_style_shadow_color(obj, qz_color(token), sel);
    track_record(obj, LV_STYLE_SHADOW_COLOR, sel, token);
    ensure_delete_hook(obj);
}

/* Walk every recorded style and re-apply its colour from the current palette.
 * The records are the source of truth: each one holds the (obj, prop, sel,
 * token) tuple from when the widget was styled, so re-applying is a flat scan
 * of the list rather than a tree walk that would also touch transient
 * children. Records whose object was deleted are already pruned by the
 * LV_EVENT_DELETE hook. */
static void theme_refresh_all_records(void)
{
    qz_style_record_t *rec = g_style_records;
    while (rec) {
        lv_color_t c = qz_color(rec->token);
        switch (rec->prop) {
        case LV_STYLE_BG_COLOR:
            lv_obj_set_style_bg_color(rec->obj, c, rec->selector);
            break;
        case LV_STYLE_TEXT_COLOR:
            lv_obj_set_style_text_color(rec->obj, c, rec->selector);
            break;
        case LV_STYLE_BORDER_COLOR:
            lv_obj_set_style_border_color(rec->obj, c, rec->selector);
            break;
        case LV_STYLE_SHADOW_COLOR:
            lv_obj_set_style_shadow_color(rec->obj, c, rec->selector);
            break;
        default:
            break;
        }
        rec = rec->next;
    }
    /* Invalidate every screen so the new colours are actually drawn. LVGL keeps
     * the registered screens on the display; we only need to repaint the ones
     * that exist. The active screen is repainted by the cross-fade below, so
     * touch the rest here. */
    lv_display_t *disp = lv_display_get_default();
    if (disp) {
        lv_obj_t *active = lv_display_get_screen_active(disp);
        lv_obj_t *prev = lv_display_get_screen_prev(disp);
        /* The transient previous screen during a transition also needs a
         * refresh, but its colours will be redrawn by the transition anyway. */
        (void)prev;
        if (active) lv_obj_invalidate(active);
    }
}

static bool theme_reduce_motion(void);

bool qz_theme_set_dark(bool dark)
{
    if (g_dark_mode == dark) return false;
    g_dark_mode = dark;
    theme_refresh_all_records();
    /* The active screen is reloaded with a short cross-fade so the palette
     * change reads as a single motion instead of a hard repaint. */
    lv_obj_t *active = lv_screen_active();
    if (active) {
        if (theme_reduce_motion()) {
            lv_obj_invalidate(active);
        } else {
            lv_screen_load_anim(active, LV_SCR_LOAD_ANIM_FADE_ON, QZ_DUR_SMALL, 0, false);
        }
    }
    return true;
}

typedef struct {
    int32_t size;
    lv_font_t *font;
} qz_font_slot_t;

static void *custom_font_data;
static size_t custom_font_size;
static lv_font_t *font_body;
static qz_font_slot_t font_slots[QZ_FONT_SLOTS];
static lv_style_transition_dsc_t press_transition;
static bool press_transition_ready;
static lv_style_transition_dsc_t reduced_press_transition;
static bool reduced_press_transition_ready;
static bool reduce_motion;

/* ------------------------------------------------------------------------- *
 * Material opacity
 *
 * One value drives every white surface, so the whole interface can be dialled
 * from solid white to almost clear while it runs. Surfaces are tagged with a
 * user flag when they are styled; the tag is what makes a live change reach
 * them without rebuilding the screen.
 * ------------------------------------------------------------------------- */
#define TAG_PLATE LV_OBJ_FLAG_USER_1 /**< bg opacity follows the material */
#define TAG_GLASS LV_OBJ_FLAG_USER_2 /**< ...as floating chrome */
#define TAG_PRESS LV_OBJ_FLAG_USER_3 /**< only the pressed fill follows it */

static int material_opa = 255 * QZ_MATERIAL_PERCENT_DEFAULT / 100;
static int material_glass_opa = 255 * QZ_MATERIAL_PERCENT_DEFAULT / 100;
static bool material_glass_forced; /**< QZDESK_GLASS_OPA was given */

static int env_percent(const char *name, int fallback)
{
    const char *value = getenv(name);
    if (!value || value[0] == '\0') return fallback;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value) return fallback;
    if (parsed < 0) parsed = 0;
    if (parsed > 100) parsed = 100;
    return (int)parsed;
}

void qz_style_init(void)
{
    /* Pull the persisted dark-mode preference into QZDESK_DARK before any token
     * is read, so the first frame is already in the right theme. */
    qz_appearance_load();
    const char *dark_env = getenv("QZDESK_DARK");
    g_dark_mode = dark_env &&
                  (strcmp(dark_env, "1") == 0 ||
                   strcmp(dark_env, "true") == 0 ||
                   strcmp(dark_env, "yes") == 0);

    material_opa = 255 * env_percent("QZDESK_MATERIAL_OPA", QZ_MATERIAL_PERCENT_DEFAULT) / 100;
    int glass = env_percent("QZDESK_GLASS_OPA", -1);
    material_glass_forced = glass >= 0;
    material_glass_opa = material_glass_forced ? 255 * glass / 100 : material_opa;
}

int qz_material_opa(void) { return material_opa; }
int qz_material_glass_opa(void) { return material_glass_opa; }
int qz_material_percent(void) { return (material_opa * 100 + 127) / 255; }

static void material_apply_one(lv_obj_t *obj)
{
    if (lv_obj_has_flag(obj, TAG_PLATE)) {
        lv_obj_set_style_bg_opa(obj, (lv_opa_t)material_opa, 0);
        lv_obj_set_style_bg_opa(obj, (lv_opa_t)material_opa, LV_STATE_PRESSED);
    } else if (lv_obj_has_flag(obj, TAG_GLASS)) {
        lv_obj_set_style_bg_opa(obj, (lv_opa_t)material_glass_opa, 0);
    } else if (lv_obj_has_flag(obj, TAG_PRESS)) {
        lv_obj_set_style_bg_opa(obj, (lv_opa_t)material_opa, LV_STATE_PRESSED);
    }
}

static void material_apply_tree(lv_obj_t *obj)
{
    if (!obj) return;
    material_apply_one(obj);
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; i++) material_apply_tree(lv_obj_get_child(obj, i));
}

void qz_material_register(lv_obj_t *obj, bool floating)
{
    if (!obj) return;
    lv_obj_add_flag(obj, floating ? TAG_GLASS : TAG_PLATE);
    material_apply_one(obj);
}

bool qz_material_set_percent(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    material_opa = (255 * percent + 50) / 100;
    if (!material_glass_forced) material_glass_opa = material_opa;
    /* Only the screen on show is repainted; the others are brought up to date
     * when they are loaded (see qz_screen_load). */
    lv_obj_t *screen = lv_screen_active();
    if (screen) {
        material_apply_tree(screen);
        lv_obj_invalidate(screen);
    }
    return true;
}

/* ------------------------------------------------------------------------- */

static void register_font(int32_t size, lv_font_t *font)
{
    for (int i = 0; i < QZ_FONT_SLOTS; i++) {
        if (font_slots[i].font) continue;
        font_slots[i].size = size;
        font_slots[i].font = font;
        return;
    }
}

static lv_font_t *create_custom_font(int32_t size)
{
    /* The simulator's LVGL config leaves the global cache disabled. Use the
     * direct path so the bundled font renders without cache-allocation errors. */
    return lv_tiny_ttf_create_data_ex(custom_font_data, custom_font_size, size,
                                      LV_FONT_KERNING_NONE, 0);
}

static lv_font_t *fetch_font(int32_t size)
{
    if (!custom_font_data) return NULL;
    for (int i = 0; i < QZ_FONT_SLOTS; i++) {
        if (font_slots[i].font && font_slots[i].size == size) return font_slots[i].font;
    }
    for (int i = 0; i < QZ_FONT_SLOTS; i++) {
        if (font_slots[i].font) continue;
        lv_font_t *font = create_custom_font(size);
        if (!font) return NULL;
        font_slots[i].size = size;
        font_slots[i].font = font;
        return font;
    }
    return NULL;
}

/* Montserrat keeps the LV_SYMBOL_* glyphs that the bundled CJK font lacks. */
static const lv_font_t *montserrat_for(int32_t px)
{
    static const int32_t sizes[] = {12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32, 34, 36, 40};
    static const lv_font_t *fonts[] = {
        &lv_font_montserrat_12, &lv_font_montserrat_14, &lv_font_montserrat_16,
        &lv_font_montserrat_18, &lv_font_montserrat_20, &lv_font_montserrat_22,
        &lv_font_montserrat_24, &lv_font_montserrat_26, &lv_font_montserrat_28,
        &lv_font_montserrat_30, &lv_font_montserrat_32, &lv_font_montserrat_34,
        &lv_font_montserrat_36, &lv_font_montserrat_40,
    };
    const int32_t count = (int32_t)(sizeof(sizes) / sizeof(sizes[0]));
    int32_t best = 0;
    int32_t best_delta = 0x7FFFFFFF;
    for (int32_t i = 0; i < count; i++) {
        int32_t delta = sizes[i] > px ? sizes[i] - px : px - sizes[i];
        if (delta < best_delta) {
            best_delta = delta;
            best = i;
        }
    }
    return fonts[best];
}

bool qz_reduce_motion(void) { return reduce_motion; }

static bool theme_reduce_motion(void) { return reduce_motion; }

void qz_font_init(void)
{
    const char *reduce_motion_env = getenv("QZDESK_REDUCE_MOTION");
    reduce_motion = reduce_motion_env &&
                    (strcmp(reduce_motion_env, "1") == 0 ||
                     strcmp(reduce_motion_env, "true") == 0 ||
                     strcmp(reduce_motion_env, "yes") == 0);
    const char *path = getenv("QZDESK_FONT");
    FILE *file = NULL;
    if (path && path[0] != '\0') {
        file = fopen(path, "rb");
        if (!file) fprintf(stderr, "QZdesk: 环境变量指定的字体打不开：%s\n", path);
    }
    if (!file) {
        size_t index;
        for (index = 0; index < sizeof(qz_font_paths) / sizeof(qz_font_paths[0]); index++) {
            file = fopen(qz_font_paths[index], "rb");
            if (file) {
                path = qz_font_paths[index];
                break;
            }
        }
    }
    if (!file) {
        fprintf(stderr, "QZdesk: unable to open font %s, using fallback\n", qz_font_paths[0]);
        return;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return;
    }
    long size = ftell(file);
    rewind(file);
    if (size <= 0) {
        fclose(file);
        return;
    }
    custom_font_data = malloc((size_t)size);
    if (!custom_font_data || fread(custom_font_data, 1, (size_t)size, file) != (size_t)size) {
        free(custom_font_data);
        custom_font_data = NULL;
        fclose(file);
        fprintf(stderr, "QZdesk: unable to read font %s, using fallback\n", path);
        return;
    }
    fclose(file);
    custom_font_size = (size_t)size;
    /* 正文字号也按面板缩放（见 include/scale.h）：设计稿 16px，320 面板上 11px。 */
    font_body = lv_tiny_ttf_create_data_ex(custom_font_data, (size_t)size, qz_scale_font(16),
                                           LV_FONT_KERNING_NONE, 0);
    if (!font_body) {
        free(custom_font_data);
        custom_font_data = NULL;
        fprintf(stderr, "QZdesk: unable to parse font %s, using fallback\n", path);
        return;
    }
    register_font(qz_scale_font(16), font_body);
    /* Warm the sizes the shell always needs so the first frames stay cheap.
     * 预热的是**缩放后**的字号：调用方拿设计稿尺寸进来，最终问的就是这些。 */
    fetch_font(qz_scale_font(12));
    fetch_font(qz_scale_font(15));
    fetch_font(qz_scale_font(17));
    fetch_font(qz_scale_font(22));
    fetch_font(qz_scale_font(26));
    fetch_font(qz_scale_font(34));
}

const lv_font_t *qz_font(void)
{
    return font_body ? font_body : &lv_font_simsun_16_cjk;
}

/* 传进来的是设计稿字号，先按面板缩放再取字体：这样页面里所有字号都跟着屏幕走，
 * 一处都不用改（ttf 字体按需生成，任意字号都行；内置蒙塞拉特取最接近的字号）。 */
const lv_font_t *qz_font_size(int32_t px)
{
    int32_t size = qz_scale_font(px);
    lv_font_t *font = fetch_font(size);
    return font ? font : montserrat_for(size);
}

const lv_font_t *qz_symbol_font(int32_t px) { return montserrat_for(qz_scale_font(px)); }

/* qz_color() and the tracked setters are defined with the palette above. */

/* ------------------------------------------------------------------------- */

void qz_style_screen(lv_obj_t *obj)
{
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    /* Flat systemGray5 canvas. The reference carries its whole hierarchy on two
     * greys and a white, so there is no wash, no bloom and nothing to bake. */
    qz_obj_set_bg_color(obj, QZ_BG, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    qz_obj_set_text_color(obj, QZ_TEXT, 0);
    lv_obj_set_style_text_font(obj, qz_font(), 0);
}

void qz_style_toolbar(lv_obj_t *obj)
{
    /* Floating chrome: a capsule of translucent white inset from the edges, so
     * content stays visible all the way round it. Together with the composer it
     * is the only translucent surface in the interface — plates are opaque, and
     * glass never sits on glass. The rim stands in for a hairline: light
     * catching the material separates it from the page without a hard divider.
     *
     * The chrome is glass: in light mode it is translucent white, in dark mode
     * it is translucent #1C1C1E — same role, same opacity, opposite palette. */
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    qz_obj_set_bg_color(obj, QZ_BAR, 0);
    lv_obj_set_style_bg_opa(obj, (lv_opa_t)material_glass_opa, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    qz_obj_set_border_color(obj, QZ_GLASS_RIM, 0);
    lv_obj_set_style_border_opa(obj, (lv_opa_t)QZ_RIM_OPA, 0);
    lv_obj_set_style_shadow_width(obj, 10, 0);
    lv_obj_set_style_shadow_opa(obj, (lv_opa_t)46, 0);
    qz_obj_set_shadow_color(obj, QZ_SHADOW, 0);
    lv_obj_set_style_shadow_offset_y(obj, 4, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    qz_material_register(obj, true);
}

void qz_style_row(lv_obj_t *obj)
{
    /* A row inside a plate: the plate shows through and a hairline does the
     * separating (the call site picks the side). Pressing paints the neutral
     * grey over the row — that highlight is what makes a list feel alive. */
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    qz_obj_set_border_color(obj, QZ_SEPARATOR, 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_BOTTOM, 0);
    qz_obj_set_bg_color(obj, QZ_FILL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(obj, (lv_opa_t)material_opa, LV_STATE_PRESSED);
    lv_obj_add_flag(obj, TAG_PRESS);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    qz_add_press_feedback(obj);
    qz_add_touch_glint(obj);
}

void qz_style_slider(lv_obj_t *obj)
{
    /* The object is 26px tall: 9px of padding leave an 8px iOS track while the
     * knob keeps a 26px circle. */
    /* The track is the neutral fill rather than a translucent white: on a white
     * plate a translucent white track is invisible. */
    qz_obj_set_bg_color(obj, QZ_FILL, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_top(obj, 9, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(obj, 9, LV_PART_MAIN);

    qz_obj_set_bg_color(obj, QZ_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

    qz_obj_set_bg_color(obj, QZ_KNOB, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(obj, 8, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(obj, (lv_opa_t)80, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(obj, lv_color_black(), LV_PART_KNOB);
    lv_obj_set_style_shadow_offset_y(obj, 2, LV_PART_KNOB);
    /* A hairline keeps the white knob readable on the white cards. */
    lv_obj_set_style_border_width(obj, 1, LV_PART_KNOB);
    qz_obj_set_border_color(obj, QZ_SEPARATOR, LV_PART_KNOB);
    lv_obj_set_style_border_opa(obj, (lv_opa_t)120, LV_PART_KNOB);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

void qz_style_switch(lv_obj_t *obj)
{
    qz_obj_set_bg_color(obj, QZ_FILL_PRESSED, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    /* The indicator is drawn over the whole track in both states, so it has to
     * be transparent while OFF (the grey main shows through) and only take the
     * accent when CHECKED, otherwise an off switch still reads as on. */
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_INDICATOR);
    qz_obj_set_bg_color(obj, QZ_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    qz_obj_set_bg_color(obj, QZ_KNOB, LV_PART_KNOB);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(obj, 8, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(obj, (lv_opa_t)80, LV_PART_KNOB);
    lv_obj_set_style_shadow_color(obj, lv_color_black(), LV_PART_KNOB);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

void qz_style_textarea(lv_obj_t *obj)
{
    /* A filled field on the white composer bar, the way iOS fills a search
     * field: neutral fill, no border, no translucency. */
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    qz_obj_set_bg_color(obj, QZ_FILL, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    qz_obj_set_text_color(obj, QZ_TEXT, 0);
    lv_obj_set_style_text_font(obj, qz_font_size(15), 0);
    lv_obj_set_style_pad_left(obj, 14, 0);
    lv_obj_set_style_pad_right(obj, 14, 0);
    lv_obj_set_style_pad_top(obj, 0, 0);
    lv_obj_set_style_pad_bottom(obj, 0, 0);
    qz_obj_set_bg_color(obj, QZ_FILL_PRESSED, LV_STATE_FOCUSED);
    qz_obj_set_text_color(obj, QZ_TEXT_TERTIARY, LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

void qz_style_keyboard(lv_obj_t *obj)
{
    /* The key glyphs (backspace, enter, space, ok) are LV_SYMBOL_* characters,
     * so the keys must use the symbol font rather than the CJK one. */
    lv_obj_set_style_text_font(obj, qz_symbol_font(12), 0);
    qz_obj_set_bg_color(obj, QZ_BG, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 4, 0);
    lv_obj_set_style_pad_row(obj, 5, 0);
    lv_obj_set_style_pad_column(obj, 5, 0);

    qz_obj_set_bg_color(obj, QZ_CARD, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_ITEMS);
    qz_obj_set_text_color(obj, QZ_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(obj, qz_symbol_font(12), LV_PART_ITEMS);
    lv_obj_set_style_radius(obj, 7, LV_PART_ITEMS);
    lv_obj_set_style_border_width(obj, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_ITEMS);
    /* LVGL flags the function keys (1#/ABC/backspace/enter/space/ok) as
     * checked so they can be styled apart from the letter keys. */
    qz_obj_set_bg_color(obj, QZ_FILL, LV_PART_ITEMS | LV_STATE_CHECKED);
    qz_obj_set_bg_color(obj, QZ_FILL_PRESSED, LV_PART_ITEMS | LV_STATE_PRESSED);
}

lv_obj_t *qz_text(lv_obj_t *parent, const char *text, int32_t px, lv_color_t text_color)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_style_text_font(obj, qz_font_size(px), 0);
    lv_obj_set_style_text_color(obj, text_color, 0);
    lv_obj_set_style_text_letter_space(obj, 0, 0);
    return obj;
}

/* ------------------------------------------------------------------------- *
 * 图标
 *
 * 界面统一用 Tabler 线性图标（烘成 A8 蒙版，运行时按主题色染色）。老代码里
 * 到处是 LV_SYMBOL_*，全部改写工作量太大也没必要：qz_symbol 做一层映射，
 * 认得出的符号直接换成新图标，认不出的退回内置字体 —— 调用点一行不用动。
 * ------------------------------------------------------------------------- */

static const struct {
    const char *symbol;
    qz_icon_t icon;
} qz_symbol_map[] = {
    { LV_SYMBOL_LEFT, QZ_ICON_CHEVRON_LEFT },
    { LV_SYMBOL_REFRESH, QZ_ICON_REFRESH },
    { LV_SYMBOL_IMAGE, QZ_ICON_PHOTO },
    { LV_SYMBOL_OK, QZ_ICON_CHECK },
    { LV_SYMBOL_BELL, QZ_ICON_BELL },
    { LV_SYMBOL_BATTERY_FULL, QZ_ICON_BATTERY_4 },
    { LV_SYMBOL_BATTERY_3, QZ_ICON_BATTERY_3 },
    { LV_SYMBOL_BATTERY_2, QZ_ICON_BATTERY_2 },
    { LV_SYMBOL_BATTERY_1, QZ_ICON_BATTERY_1 },
    { LV_SYMBOL_BATTERY_EMPTY, QZ_ICON_BATTERY_OFF },
    { LV_SYMBOL_CHARGE, QZ_ICON_BOLT },
    { LV_SYMBOL_WIFI, QZ_ICON_WIFI },
    { LV_SYMBOL_VOLUME_MAX, QZ_ICON_VOLUME },
    { LV_SYMBOL_TINT, QZ_ICON_DROPLET },
    { LV_SYMBOL_SETTINGS, QZ_ICON_SETTINGS },
    { LV_SYMBOL_EYE_OPEN, QZ_ICON_EYE },
    { LV_SYMBOL_EYE_CLOSE, QZ_ICON_EYE_OFF },
    { LV_SYMBOL_UP, QZ_ICON_ARROW_UP },
    { LV_SYMBOL_POWER, QZ_ICON_POWER },
    { LV_SYMBOL_PLUS, QZ_ICON_PLUS },
    { LV_SYMBOL_PLAY, QZ_ICON_PLAYER_PLAY },
    { LV_SYMBOL_LOOP, QZ_ICON_REPEAT },
    { LV_SYMBOL_LIST, QZ_ICON_LIST },
    { LV_SYMBOL_KEYBOARD, QZ_ICON_KEYBOARD },
    { LV_SYMBOL_GPS, QZ_ICON_MAP_PIN },
    { LV_SYMBOL_FILE, QZ_ICON_FILE_TEXT },
    { LV_SYMBOL_EDIT, QZ_ICON_PENCIL },
    { LV_SYMBOL_DOWNLOAD, QZ_ICON_DOWNLOAD },
    { LV_SYMBOL_CLOSE, QZ_ICON_X },
};

static qz_icon_t qz_symbol_icon(const char *symbol)
{
    size_t index;
    for (index = 0; index < sizeof(qz_symbol_map) / sizeof(qz_symbol_map[0]); index++) {
        if (strcmp(qz_symbol_map[index].symbol, symbol) == 0) {
            return qz_symbol_map[index].icon;
        }
    }
    return QZ_ICON_COUNT;
}

lv_obj_t *qz_icon_image(lv_obj_t *parent, qz_icon_t icon, int32_t px, lv_color_t color)
{
    /* 传进来的是设计稿尺寸：先缩到当前面板再挑素材（素材是"不小于请求尺寸里
     * 最小的一档"，见 qz_icon）。 */
    int32_t want = qz_scale_px(px);
    const lv_image_dsc_t *glyph = qz_icon(icon, (int)want);
    lv_obj_t *image;
    if (!glyph) return NULL;

    image = lv_image_create(parent);
    lv_image_set_src(image, glyph);
    /* A8 蒙版 + 全量重着色：颜色就是主题色，浅色/深色共用一份资源 */
    lv_obj_set_style_image_recolor(image, color, 0);
    lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
#if QZ_SCALE_NUM < QZ_SCALE_DEN
    /* 面板比设计稿小：素材最小一档只有 14px，仍会比周围的字与留白大出一截，
     * 于是按设计尺寸反算一个 zoom（`lv_image_set_scale` 会被 scale.h 再缩到
     * 面板），把它压到想要的大小。480 基准下这段不参与编译，行为与以前一致。 */
    if (px > 0 && (int32_t)glyph->header.w > want) {
        lv_image_set_scale(image, (uint32_t)(256 * px / glyph->header.w));
    }
#endif
    lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE);
    return image;
}

lv_obj_t *qz_symbol(lv_obj_t *parent, const char *symbol, int32_t px, lv_color_t color)
{
    qz_icon_t icon = qz_symbol_icon(symbol);
    if (icon != QZ_ICON_COUNT) {
        lv_obj_t *image = qz_icon_image(parent, icon, (int)px, color);
        if (image) return image;
        /* 该尺寸没烘出来时退回内置字体，界面不能缺图标 */
    }
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, symbol);
    lv_obj_set_style_text_font(obj, qz_symbol_font(px), 0);
    lv_obj_set_style_text_color(obj, color, 0);
    return obj;
}

lv_obj_t *qz_squircle(lv_obj_t *parent, int size, qz_color_token_t color)
{
    /* Flat, one colour, no shadow — a filled glyph tile the way iOS Settings
     * draws them. Neighbouring tiles alternate between the accent and the
     * neutral fill; a second hue is never the answer. */
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, size, size);
    lv_obj_set_style_radius(obj, size * QZ_RADIUS_ICON / 100, 0);
    qz_obj_set_bg_color(obj, color, 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

lv_obj_t *qz_separator(lv_obj_t *parent, int length, bool vertical)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_size(line, vertical ? 1 : length, vertical ? length : 1);
    qz_obj_set_bg_color(line, QZ_SEPARATOR, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    return line;
}

static void chevron_delete(lv_event_t *event)
{
    lv_obj_t *line = lv_event_get_target(event);
    lv_free(lv_obj_get_user_data(line));
}

lv_obj_t *qz_chevron(lv_obj_t *parent, lv_color_t color, int32_t size)
{
    /* lv_line keeps the point array by reference, so it lives on the heap and is
     * released together with the widget. */
    lv_point_precise_t *points = lv_malloc(sizeof(lv_point_precise_t) * 3);
    if (!points) return NULL;
    points[0].x = 0;
    points[0].y = 0;
    points[1].x = size / 2;
    points[1].y = size / 2;
    points[2].x = 0;
    points[2].y = size;

    lv_obj_t *line = lv_line_create(parent);
    lv_line_set_points(line, points, 3);
    lv_obj_set_style_line_width(line, size > 20 ? 3 : 2, 0);
    lv_obj_set_style_line_color(line, color, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_set_style_line_opa(line, LV_OPA_COVER, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(line, points);
    lv_obj_add_event_cb(line, chevron_delete, LV_EVENT_DELETE, NULL);
    return line;
}

lv_obj_t *qz_arc_piece(lv_obj_t *parent, int32_t width, int32_t height, int32_t start,
                       int32_t end, int32_t thickness, lv_color_t color)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, width, height);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, thickness, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc, 0, 0);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_arc_set_angles(arc, start, end);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_SCROLLABLE);
    return arc;
}

void qz_style_plate(lv_obj_t *obj)
{
    /* The workhorse surface: opaque plate on the grey canvas, no border and no
     * shadow. Depth is the value difference between the plate and the canvas —
     * a shadow would only duplicate what the grey already says. */
    qz_obj_set_bg_color(obj, QZ_CARD, 0);
    lv_obj_set_style_bg_opa(obj, (lv_opa_t)material_opa, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    qz_material_register(obj, false);
}

void qz_style_glass(lv_obj_t *obj)
{
    /* Translucent surface + the specular rim + a soft neutral shadow. Reserve
     * it for chrome above content; a plate is opaque with no shadow. The glass
     * role follows the theme: white in light, #1C1C1E in dark. */
    qz_obj_set_bg_color(obj, QZ_BAR, 0);
    lv_obj_set_style_bg_opa(obj, (lv_opa_t)material_glass_opa, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    qz_obj_set_border_color(obj, QZ_GLASS_RIM, 0);
    lv_obj_set_style_border_opa(obj, (lv_opa_t)QZ_RIM_OPA, 0);
    lv_obj_set_style_shadow_width(obj, 8, 0);
    lv_obj_set_style_shadow_opa(obj, (lv_opa_t)38, 0);
    qz_obj_set_shadow_color(obj, QZ_SHADOW, 0);
    lv_obj_set_style_shadow_offset_y(obj, 3, 0);
    qz_material_register(obj, true);
}

lv_obj_t *qz_card_button(lv_obj_t *parent, int width, int height)
{
    lv_obj_t *card = lv_button_create(parent);
    lv_obj_set_size(card, width, height);
    lv_obj_set_style_radius(card, QZ_RADIUS_CARD, 0);
    /* A plate, not a material: opaque plate, no border, no shadow. The grey
     * canvas around it is what makes it read as raised. */
    qz_obj_set_bg_color(card, QZ_CARD, 0);
    lv_obj_set_style_bg_opa(card, (lv_opa_t)material_opa, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    qz_obj_set_bg_color(card, QZ_FILL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(card, (lv_opa_t)material_opa, LV_STATE_PRESSED);
    qz_material_register(card, false);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    qz_add_press_feedback(card);
    qz_add_touch_glint(card);
    /* Press scale costs an ARGB layer of width x height, keep it for the
     * compact cards (tiles, home cards) and let wide rows just tint. */
    if (width <= 260 && height <= 160) qz_add_press_scale(card, 97);
    return card;
}

lv_obj_t *qz_button(lv_obj_t *parent, const char *caption, int width, int height)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, width, height);
    lv_obj_set_style_radius(btn, height / 2, 0);
    qz_obj_set_bg_color(btn, QZ_FILL, 0);
    qz_obj_set_bg_color(btn, QZ_FILL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    qz_add_press_feedback(btn);
    qz_add_press_scale(btn, 96);
    lv_obj_t *caption_label = qz_text(btn, caption, 15, qz_color(QZ_TEXT));
    lv_obj_center(caption_label);
    return btn;
}

lv_obj_t *qz_icon_button(lv_obj_t *parent, const char *icon, int size)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, size, size);
    /* A filled circle, so it also reads on top of the glass bar. */
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    qz_obj_set_bg_color(btn, QZ_FILL, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    qz_obj_set_bg_color(btn, QZ_FILL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    qz_add_press_feedback(btn);
    qz_add_press_scale(btn, 96);
    qz_add_touch_glint(btn);
    lv_obj_t *label = qz_symbol(btn, icon, size / 2, qz_color(QZ_TEXT));
    lv_obj_center(label);
    return btn;
}

void qz_style_primary_button(lv_obj_t *obj)
{
    /* Apple ships exactly two button materials. This is the prominent one:
     * opaque accent fill, white label, capsule. Nothing shows through it — it
     * is the one action the screen is asking for. */
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    qz_obj_set_bg_color(obj, QZ_ACCENT, 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_NONE, 0);
    qz_obj_set_bg_color(obj, QZ_ACCENT_DARK, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_shadow_offset_y(obj, 5, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    qz_add_press_feedback(obj);
    qz_add_touch_glint(obj);
}

void qz_add_press_feedback(lv_obj_t *obj)
{
    /* Only colour/opacity transitions are used here on purpose: scaling an
     * object turns it into a transformed layer, and LVGL has to allocate that
     * layer in one piece. On a 480x320 panel a full width card would need a
     * ~300KB ARGB buffer, so the press effect stays layer free. */
    static const lv_style_prop_t props[] = {
        LV_STYLE_BG_COLOR,
        LV_STYLE_BG_OPA,
        LV_STYLE_PROP_INV,
    };
    lv_style_transition_dsc_t *transition = reduce_motion ? &reduced_press_transition
                                                          : &press_transition;
    bool *ready = reduce_motion ? &reduced_press_transition_ready : &press_transition_ready;
    if (!*ready) {
        lv_style_transition_dsc_init(transition, props, lv_anim_path_ease_out,
                                     reduce_motion ? 0 : 140, 0, NULL);
        *ready = true;
    }
    lv_obj_set_style_transition(obj, transition, 0);
}

/* ------------------------------------------------------------------------- */

void qz_anim_ease_out(lv_anim_t *anim)
{
    if (!anim) return;
    anim->parameter.bezier3 = (lv_anim_bezier3_para_t){
        LV_BEZIER_VAL_FLOAT(0.23), LV_BEZIER_VAL_FLOAT(1),
        LV_BEZIER_VAL_FLOAT(0.32), LV_BEZIER_VAL_FLOAT(1)};
    lv_anim_set_path_cb(anim, lv_anim_path_custom_bezier3);
}

void qz_anim_ease_in_out(lv_anim_t *anim)
{
    if (!anim) return;
    anim->parameter.bezier3 = (lv_anim_bezier3_para_t){
        LV_BEZIER_VAL_FLOAT(0.77), LV_BEZIER_VAL_FLOAT(0),
        LV_BEZIER_VAL_FLOAT(0.175), LV_BEZIER_VAL_FLOAT(1)};
    lv_anim_set_path_cb(anim, lv_anim_path_custom_bezier3);
}

static void press_scale_apply(void *var, int32_t value)
{
    lv_obj_set_style_transform_scale_x((lv_obj_t *)var, value, 0);
    lv_obj_set_style_transform_scale_y((lv_obj_t *)var, value, 0);
}

/** Scale to `to`, animating from whatever is on screen right now. */
static void press_scale_animate(lv_obj_t *obj, int32_t to, uint32_t duration_ms, bool strong)
{
    lv_anim_t anim;
    lv_anim_delete(obj, press_scale_apply);
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_exec_cb(&anim, press_scale_apply);
    lv_anim_set_values(&anim, lv_obj_get_style_transform_scale_x(obj, 0), to);
    lv_anim_set_duration(&anim, duration_ms);
    if (strong) {
        qz_anim_ease_out(&anim);
    } else {
        lv_anim_set_path_cb(&anim, lv_anim_path_overshoot);
    }
    lv_anim_start(&anim);
}

static void press_scale_event(lv_event_t *event)
{
    lv_obj_t *obj = lv_event_get_current_target(event);
    int32_t pressed = (int32_t)(intptr_t)lv_event_get_user_data(event);

    switch (lv_event_get_code(event)) {
    case LV_EVENT_PRESSED:
        /* Down: strong ease-out, 120ms — the response to the finger. */
        press_scale_animate(obj, pressed, QZ_DUR_PRESS, true);
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        /* Up: a touch slower with a small overshoot, so the release reads as the
         * surface springing back rather than the press simply ending. */
        press_scale_animate(obj, 256, QZ_DUR_PANEL, false);
        break;
    default:
        break;
    }
}

void qz_add_press_scale(lv_obj_t *obj, int32_t percent)
{
    if (!obj || reduce_motion) return;
    if (percent > 100) percent = 100;
    if (percent < 90) percent = 90;

    const int32_t pressed = 256 * percent / 100;
    /* Explicit anims rather than a style transition: a transition would have to
     * use one of LVGL's built-in curves, and those are weak — the strong
     * ease-out is the house curve everywhere else. */
    lv_obj_add_event_cb(obj, press_scale_event, LV_EVENT_PRESSED,
                        (void *)(intptr_t)pressed);
    lv_obj_add_event_cb(obj, press_scale_event, LV_EVENT_RELEASED,
                        (void *)(intptr_t)pressed);
    lv_obj_add_event_cb(obj, press_scale_event, LV_EVENT_PRESS_LOST,
                        (void *)(intptr_t)pressed);
}

/* ------------------------------------------------------------------------- */
/* Touch glint                                                                */
/* ------------------------------------------------------------------------- */

/** Lives in the top layer so it is never clipped or laid out by a flex parent. */
static lv_obj_t *touch_glint;

static void entrance_opa(void *var, int32_t value);   /* defined below */

static lv_obj_t *glint_create(void)
{
    lv_obj_t *glint = lv_obj_create(lv_layer_top());
    lv_obj_set_size(glint, 72, 72);
    lv_obj_set_style_radius(glint, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(glint, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(glint, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(glint, 0, 0);
    lv_obj_set_style_pad_all(glint, 0, 0);
    /* Two concentric discs fake a soft radial falloff: the plain software
     * renderer has no radial gradients. */
    for (int i = 0; i < 2; i++) {
        lv_obj_t *ring = lv_obj_create(glint);
        int size = i == 0 ? 44 : 22;
        lv_obj_set_size(ring, size, size);
        lv_obj_center(ring);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(ring, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(ring, i == 0 ? (lv_opa_t)70 : (lv_opa_t)110, 0);
        lv_obj_set_style_border_width(ring, 0, 0);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_clear_flag(glint, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(glint, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(glint, LV_OBJ_FLAG_HIDDEN);
    return glint;
}

static void glint_follow(void)
{
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t point;
    if (!indev || !touch_glint) return;
    lv_indev_get_point(indev, &point);
    lv_obj_set_pos(touch_glint, point.x - 36, point.y - 36);
}

/** Fade the glint out. Safe to call when nothing is showing. */
static void glint_hide(void)
{
    lv_anim_t fade;
    if (!touch_glint) return;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, touch_glint);
    lv_anim_set_exec_cb(&fade, entrance_opa);
    lv_anim_set_values(&fade, lv_obj_get_style_opa(touch_glint, 0), LV_OPA_TRANSP);
    lv_anim_set_duration(&fade, QZ_DUR_SMALL);
    qz_anim_ease_out(&fade);
    lv_anim_start(&fade);
}

/** The glint lives outside the pressed widget, so it also listens on the input
 *  device: a press that ends because the widget was deleted (a screen change,
 *  a row removing itself) never reaches the widget's own handlers and would
 *  otherwise leave the highlight stuck on screen. */
static void glint_indev_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) glint_hide();
}

static void glint_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    lv_anim_t fade;
    if (!touch_glint) touch_glint = glint_create();

    if (code == LV_EVENT_PRESSED) {
        glint_follow();
        lv_obj_clear_flag(touch_glint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(touch_glint, LV_OPA_TRANSP, 0);
        lv_anim_init(&fade);
        lv_anim_set_var(&fade, touch_glint);
        lv_anim_set_exec_cb(&fade, entrance_opa);
        lv_anim_set_values(&fade, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_duration(&fade, QZ_DUR_PRESS);
        qz_anim_ease_out(&fade);
        lv_anim_start(&fade);
    } else if (code == LV_EVENT_PRESSING) {
        glint_follow();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST ||
               code == LV_EVENT_CLICKED) {
        glint_hide();
    }
}

void qz_add_touch_glint(lv_obj_t *obj)
{
    static bool indev_hooked;
    if (!obj || reduce_motion) return;

    if (!indev_hooked) {
        for (lv_indev_t *indev = lv_indev_get_next(NULL); indev;
             indev = lv_indev_get_next(indev)) {
            if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) continue;
            lv_indev_add_event_cb(indev, glint_indev_event, LV_EVENT_ALL, NULL);
        }
        indev_hooked = true;
    }

    lv_obj_add_event_cb(obj, glint_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(obj, glint_event, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(obj, glint_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(obj, glint_event, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(obj, glint_event, LV_EVENT_CLICKED, NULL);
}

lv_obj_t *qz_overlay_create(lv_obj_t *parent)
{
    lv_obj_t *overlay = lv_obj_create(parent);
    lv_obj_set_size(overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, (lv_opa_t)110, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    return overlay;
}

/* ------------------------------------------------------------------------- */

static void entrance_opa(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

static void entrance_translate_y(void *var, int32_t value)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, value, 0);
}

void qz_animate_entrance(lv_obj_t *obj, uint32_t delay_ms)
{
    if (!obj) return;
    lv_anim_delete(obj, NULL);
    if (reduce_motion) {
        lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
        lv_obj_set_style_translate_y(obj, 0, 0);
        return;
    }
    lv_obj_set_style_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_translate_y(obj, 12, 0);

    lv_anim_t opacity;
    lv_anim_init(&opacity);
    lv_anim_set_var(&opacity, obj);
    lv_anim_set_exec_cb(&opacity, entrance_opa);
    lv_anim_set_values(&opacity, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&opacity, QZ_DUR_PANEL);
    lv_anim_set_delay(&opacity, delay_ms);
    qz_anim_ease_out(&opacity);
    lv_anim_start(&opacity);

    lv_anim_t translate;
    lv_anim_init(&translate);
    lv_anim_set_var(&translate, obj);
    lv_anim_set_exec_cb(&translate, entrance_translate_y);
    lv_anim_set_values(&translate, 12, 0);
    lv_anim_set_duration(&translate, QZ_DUR_PANEL);
    lv_anim_set_delay(&translate, delay_ms);
    qz_anim_ease_out(&translate);
    lv_anim_start(&translate);
}

void qz_animate_pop_in(lv_obj_t *obj)
{
    if (!obj) return;
    lv_anim_delete(obj, NULL);
    lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_translate_y(obj, 0, 0);
    if (reduce_motion) return;

    /* Cross-fade only. This is the chat/face swap — two peers the user flips
     * dozens of times a day, so it gets the frequency-appropriate treatment: no
     * movement (a slide would imply a hierarchy that isn't there) and under
     * 150ms. Opacity alone also stays cheap: LVGL renders it through a chunked
     * "simple" layer instead of one panel sized ARGB layer. */
    lv_anim_t opacity;
    lv_anim_init(&opacity);
    lv_anim_set_var(&opacity, obj);
    lv_anim_set_exec_cb(&opacity, entrance_opa);
    lv_anim_set_values(&opacity, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&opacity, QZ_DUR_QUICK);
    qz_anim_ease_out(&opacity);
    lv_anim_start(&opacity);
}

void qz_screen_load(lv_obj_t *screen, lv_scr_load_anim_t anim, uint32_t duration_ms)
{
    if (!screen) return;
    /* A screen change can delete the widget that was being pressed, which is
     * exactly when the release event never arrives. */
    glint_hide();
    /* Bring the page about to be shown up to the current material. */
    material_apply_tree(screen);
    if (reduce_motion) {
        /* Reduced motion still needs the change to be legible: cross fade
         * instead of a slide, and no movement. */
        lv_screen_load_anim(screen, LV_SCR_LOAD_ANIM_FADE_ON, QZ_DUR_SMALL, 0, false);
        return;
    }
    lv_screen_load_anim(screen, anim, duration_ms, 0, false);
}
