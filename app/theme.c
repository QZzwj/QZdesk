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
#define QZ_FONT_SLOTS 32

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
static const uint32_t qz_palette[QZ_THEME_COUNT][QZ_COLOR_COUNT] = {
    /* Signal Workstation: a cool mineral canvas, paper-like working plates,
     * and one deep teal action colour. Orange is reserved for stale / warning
     * states, never decoration. */
    [QZ_THEME_MINERAL] = {
        [QZ_BG]             = 0xE8ECE8,
        [QZ_BG_GRAD]        = 0xE8ECE8,
        [QZ_CARD]           = 0xF8FAF6,
        [QZ_CARD_GRAD]      = 0xF8FAF6,
        [QZ_BAR]            = 0xF8FAF6,
        [QZ_FILL]           = 0xE2E7E3,
        [QZ_FILL_PRESSED]   = 0xD4DCD6,
        [QZ_SEPARATOR]      = 0xC4CBC5,
        [QZ_TEXT]           = 0x14211D,
        [QZ_TEXT_SECONDARY] = 0x52625B,
        [QZ_TEXT_TERTIARY]  = 0x5F6F68,
        [QZ_SHADOW]         = 0x14211D,
        [QZ_FACE_BUBBLE]    = 0xD9EAE4,
        [QZ_KNOB]           = 0xF8FAF6,
        [QZ_GLASS_RIM]      = 0xC4CBC5,
        [QZ_ACCENT]         = 0x0B6258,
        [QZ_ACCENT_DARK]    = 0x084C44,
        [QZ_ACCENT_TEXT]    = 0x084C44,
        [QZ_ACCENT_TINT]    = 0xD9EAE4,
        [QZ_TEXT_ON_ACCENT] = 0xF6FBF8,
        [QZ_GREEN]          = 0x1F6B44,
        [QZ_ORANGE]         = 0xC45B16,
        [QZ_RED]            = 0xB43A34,
    },
    [QZ_THEME_NIGHT] = {
        [QZ_BG]             = 0x101613,
        [QZ_BG_GRAD]        = 0x101613,
        [QZ_CARD]           = 0x18211D,
        [QZ_CARD_GRAD]      = 0x18211D,
        [QZ_BAR]            = 0x18211D,
        [QZ_FILL]           = 0x222C27,
        [QZ_FILL_PRESSED]   = 0x2D3933,
        [QZ_SEPARATOR]      = 0x3A4740,
        [QZ_TEXT]           = 0xEFF5F1,
        [QZ_TEXT_SECONDARY] = 0xB7C3BC,
        [QZ_TEXT_TERTIARY]  = 0x93A39B,
        [QZ_SHADOW]         = 0x000000,
        [QZ_FACE_BUBBLE]    = 0x1A332E,
        [QZ_KNOB]           = 0xEFF5F1,
        [QZ_GLASS_RIM]      = 0x3A4740,
        [QZ_ACCENT]         = 0x3FC4AB,
        [QZ_ACCENT_DARK]    = 0x8EE1D2,
        [QZ_ACCENT_TEXT]    = 0x8EE1D2,
        [QZ_ACCENT_TINT]    = 0x1A332E,
        [QZ_TEXT_ON_ACCENT] = 0x081411,
        [QZ_GREEN]          = 0x52C878,
        [QZ_ORANGE]         = 0xFFAB5E,
        [QZ_RED]            = 0xFF7A72,
    },
    /* A crisp daylight palette with cobalt as the system signal. The warm
     * orange is deliberately separate so time and warnings never disappear
     * into the action colour. */
    [QZ_THEME_PAPER] = {
        [QZ_BG]             = 0xEEF1F0,
        [QZ_BG_GRAD]        = 0xEEF1F0,
        [QZ_CARD]           = 0xFFFFFF,
        [QZ_CARD_GRAD]      = 0xFFFFFF,
        [QZ_BAR]            = 0xFFFFFF,
        [QZ_FILL]           = 0xE5E9ED,
        [QZ_FILL_PRESSED]   = 0xD5DCE4,
        [QZ_SEPARATOR]      = 0xBBC5D0,
        [QZ_TEXT]           = 0x111922,
        [QZ_TEXT_SECONDARY] = 0x4E5C6B,
        [QZ_TEXT_TERTIARY]  = 0x687687,
        [QZ_SHADOW]         = 0x0B1420,
        [QZ_FACE_BUBBLE]    = 0xDDE7F8,
        [QZ_KNOB]           = 0xFFFFFF,
        [QZ_GLASS_RIM]      = 0xBBC5D0,
        [QZ_ACCENT]         = 0x2457B6,
        [QZ_ACCENT_DARK]    = 0x173E8A,
        [QZ_ACCENT_TEXT]    = 0x173E8A,
        [QZ_ACCENT_TINT]    = 0xDDE7F8,
        [QZ_TEXT_ON_ACCENT] = 0xFFFFFF,
        [QZ_GREEN]          = 0x176B4A,
        [QZ_ORANGE]         = 0xB4541A,
        [QZ_RED]            = 0xB32F3A,
    },
    /* Warm graphite and coral for evening use. Surfaces stay neutral so the
     * accent reads as a signal, not as a wash over the whole interface. */
    [QZ_THEME_EMBER] = {
        [QZ_BG]             = 0x141315,
        [QZ_BG_GRAD]        = 0x141315,
        [QZ_CARD]           = 0x201E21,
        [QZ_CARD_GRAD]      = 0x201E21,
        [QZ_BAR]            = 0x201E21,
        [QZ_FILL]           = 0x2B282D,
        [QZ_FILL_PRESSED]   = 0x37333A,
        [QZ_SEPARATOR]      = 0x4A444C,
        [QZ_TEXT]           = 0xF7F1EA,
        [QZ_TEXT_SECONDARY] = 0xC8BDB2,
        [QZ_TEXT_TERTIARY]  = 0xA79A8E,
        [QZ_SHADOW]         = 0x000000,
        [QZ_FACE_BUBBLE]    = 0x402824,
        [QZ_KNOB]           = 0xF7F1EA,
        [QZ_GLASS_RIM]      = 0x4A444C,
        [QZ_ACCENT]         = 0xFF8E72,
        [QZ_ACCENT_DARK]    = 0xFFC0AE,
        [QZ_ACCENT_TEXT]    = 0xFFC0AE,
        [QZ_ACCENT_TINT]    = 0x402824,
        [QZ_TEXT_ON_ACCENT] = 0x1B0E0A,
        [QZ_GREEN]          = 0x69D08A,
        [QZ_ORANGE]         = 0xFFC66D,
        [QZ_RED]            = 0xFF7777,
    },
};

static qz_theme_id_t g_theme = QZ_THEME_MINERAL;

static qz_theme_id_t valid_theme(qz_theme_id_t theme)
{
    return (unsigned)theme < QZ_THEME_COUNT ? theme : QZ_THEME_MINERAL;
}

bool qz_theme_is_dark(void) { return qz_theme_id_is_dark(g_theme); }
qz_theme_id_t qz_theme_get_id(void) { return g_theme; }

bool qz_theme_id_is_dark(qz_theme_id_t theme)
{
    return valid_theme(theme) == QZ_THEME_NIGHT ||
           valid_theme(theme) == QZ_THEME_EMBER;
}

const char *qz_theme_id_key(qz_theme_id_t theme)
{
    static const char *const keys[QZ_THEME_COUNT] = {
        "mineral", "night", "paper", "ember",
    };
    return keys[valid_theme(theme)];
}

const char *qz_theme_id_name(qz_theme_id_t theme)
{
    static const char *const names[QZ_THEME_COUNT] = {
        "苔原", "夜海", "纸蓝", "余烬",
    };
    return names[valid_theme(theme)];
}

const char *qz_theme_id_caption(qz_theme_id_t theme)
{
    static const char *const captions[QZ_THEME_COUNT] = {
        "冷灰 · 深青", "墨绿 · 薄荷", "亮白 · 钴蓝", "暖灰 · 珊瑚",
    };
    return captions[valid_theme(theme)];
}

bool qz_theme_parse(const char *value, qz_theme_id_t *theme)
{
    if (!value || value[0] == '\0') return false;
    for (int i = 0; i < QZ_THEME_COUNT; i++) {
        if (strcmp(value, qz_theme_id_key((qz_theme_id_t)i)) == 0) {
            if (theme) *theme = (qz_theme_id_t)i;
            return true;
        }
        if (strcmp(value, qz_theme_id_name((qz_theme_id_t)i)) == 0) {
            if (theme) *theme = (qz_theme_id_t)i;
            return true;
        }
    }
    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
        strcmp(value, "yes") == 0 || strcmp(value, "dark") == 0) {
        if (theme) *theme = QZ_THEME_NIGHT;
        return true;
    }
    if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 ||
        strcmp(value, "no") == 0 || strcmp(value, "light") == 0) {
        if (theme) *theme = QZ_THEME_MINERAL;
        return true;
    }
    return false;
}

lv_color_t qz_color_theme(qz_color_token_t token, qz_theme_id_t theme)
{
    if ((unsigned)token >= QZ_COLOR_COUNT) return lv_color_black();
    return lv_color_hex(qz_palette[valid_theme(theme)][token]);
}

lv_color_t qz_color_mode(qz_color_token_t token, bool dark)
{
    return qz_color_theme(token, dark ? QZ_THEME_NIGHT : QZ_THEME_MINERAL);
}

lv_color_t qz_color(qz_color_token_t token)
{
    return qz_color_theme(token, g_theme);
}

/* Public widget helpers still accept lv_color_t so the existing call sites keep
 * working. Map those colours back to a palette role; dynamic colours that are
 * genuinely not theme roles stay untracked. */
static bool color_token_for(lv_color_t color, qz_color_token_t *token)
{
    uint32_t value = lv_color_to_int(color);
    for (int theme = 0; theme < QZ_THEME_COUNT; theme++) {
        for (int i = 0; i < QZ_COLOR_COUNT; i++) {
            if (lv_color_to_int(lv_color_hex(qz_palette[theme][i])) == value) {
                if (token) *token = (qz_color_token_t)i;
                return true;
            }
        }
    }
    return false;
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
    void *aux;
    struct qz_style_record_s *next;
} qz_style_record_t;

static qz_style_record_t *g_style_records;

static void track_record(lv_obj_t *obj, lv_style_prop_t prop,
                         lv_style_selector_t sel, qz_color_token_t token, void *aux)
{
    /* Reuse an existing record for the same (obj, prop, sel) triple so a
     * restyle doesn't grow the list. */
    qz_style_record_t *rec = g_style_records;
    while (rec) {
        if (rec->obj == obj && rec->prop == prop && rec->selector == sel &&
            rec->aux == aux) {
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
    rec->aux = aux;
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
    /* Application widgets use user_data for their own bindings. Keep the theme
     * marker out of that slot; USER_4 is still free after the material tags. */
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_USER_4)) return;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_USER_4);
    lv_obj_add_event_cb(obj, track_delete, LV_EVENT_DELETE, NULL);
}

void qz_obj_set_bg_color(lv_obj_t *obj, qz_color_token_t token, lv_style_selector_t sel)
{
    lv_obj_set_style_bg_color(obj, qz_color(token), sel);
    track_record(obj, LV_STYLE_BG_COLOR, sel, token, NULL);
    ensure_delete_hook(obj);
}

void qz_obj_set_bg_grad_color(lv_obj_t *obj, qz_color_token_t token, lv_style_selector_t sel)
{
    lv_obj_set_style_bg_grad_color(obj, qz_color(token), sel);
    track_record(obj, LV_STYLE_BG_GRAD_COLOR, sel, token, NULL);
    ensure_delete_hook(obj);
}

void qz_obj_set_text_color(lv_obj_t *obj, qz_color_token_t token, lv_style_selector_t sel)
{
    lv_obj_set_style_text_color(obj, qz_color(token), sel);
    track_record(obj, LV_STYLE_TEXT_COLOR, sel, token, NULL);
    ensure_delete_hook(obj);
}

void qz_obj_set_border_color(lv_obj_t *obj, qz_color_token_t token, lv_style_selector_t sel)
{
    lv_obj_set_style_border_color(obj, qz_color(token), sel);
    track_record(obj, LV_STYLE_BORDER_COLOR, sel, token, NULL);
    ensure_delete_hook(obj);
}

void qz_obj_set_shadow_color(lv_obj_t *obj, qz_color_token_t token, lv_style_selector_t sel)
{
    lv_obj_set_style_shadow_color(obj, qz_color(token), sel);
    track_record(obj, LV_STYLE_SHADOW_COLOR, sel, token, NULL);
    ensure_delete_hook(obj);
}

void qz_obj_set_image_recolor(lv_obj_t *obj, qz_color_token_t token, lv_style_selector_t sel)
{
    lv_obj_set_style_image_recolor(obj, qz_color(token), sel);
    track_record(obj, LV_STYLE_IMAGE_RECOLOR, sel, token, NULL);
    ensure_delete_hook(obj);
}

void qz_obj_set_line_color(lv_obj_t *obj, qz_color_token_t token, lv_style_selector_t sel)
{
    lv_obj_set_style_line_color(obj, qz_color(token), sel);
    track_record(obj, LV_STYLE_LINE_COLOR, sel, token, NULL);
    ensure_delete_hook(obj);
}

void qz_obj_set_arc_color(lv_obj_t *obj, qz_color_token_t token, lv_style_selector_t sel)
{
    lv_obj_set_style_arc_color(obj, qz_color(token), sel);
    track_record(obj, LV_STYLE_ARC_COLOR, sel, token, NULL);
    ensure_delete_hook(obj);
}

void qz_obj_set_chart_series_color(lv_obj_t *chart, lv_chart_series_t *series,
                                   qz_color_token_t token)
{
    if (!chart || !series) return;
    lv_chart_set_series_color(chart, series, qz_color(token));
    track_record(chart, LV_STYLE_PROP_INV, 0, token, series);
    ensure_delete_hook(chart);
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
        case LV_STYLE_PROP_INV:
            lv_chart_set_series_color(rec->obj, (lv_chart_series_t *)rec->aux, c);
            break;
        case LV_STYLE_BG_COLOR:
            lv_obj_set_style_bg_color(rec->obj, c, rec->selector);
            break;
        case LV_STYLE_BG_GRAD_COLOR:
            lv_obj_set_style_bg_grad_color(rec->obj, c, rec->selector);
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
        case LV_STYLE_IMAGE_RECOLOR:
            lv_obj_set_style_image_recolor(rec->obj, c, rec->selector);
            break;
        case LV_STYLE_LINE_COLOR:
            lv_obj_set_style_line_color(rec->obj, c, rec->selector);
            break;
        case LV_STYLE_ARC_COLOR:
            lv_obj_set_style_arc_color(rec->obj, c, rec->selector);
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

/* ------------------------------------------------------------------------- *
 * Theme wash
 *
 * Palette records redraw in place. A full-screen rectangle on the top layer
 * animates only its background opacity, so the change reads as one soft accent
 * wash without an ARGB transform layer or a screen reload. The wash is inert:
 * it never owns input and it is removed when the settle animation completes.
 * ------------------------------------------------------------------------- */
static lv_obj_t *theme_wash;

static void theme_wash_opa(void *var, int32_t value)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

static void theme_wash_deleted(lv_event_t *event)
{
    if (lv_event_get_target(event) == theme_wash) theme_wash = NULL;
}

static void theme_wash_out(void *var, int32_t value)
{
    theme_wash_opa(var, value);
}

static void theme_wash_out_complete(lv_anim_t *anim)
{
    lv_obj_t *wash = (lv_obj_t *)anim->var;
    if (wash != theme_wash) return;
    lv_obj_delete(wash);
    theme_wash = NULL;
}

static void theme_wash_in_complete(lv_anim_t *anim)
{
    if ((lv_obj_t *)anim->var != theme_wash || !theme_wash) return;
    lv_anim_t out;
    lv_anim_init(&out);
    lv_anim_set_var(&out, theme_wash);
    lv_anim_set_exec_cb(&out, theme_wash_out);
    lv_anim_set_values(&out, 18, LV_OPA_TRANSP);
    lv_anim_set_duration(&out, QZ_DUR_THEME * 70 / 100);
    qz_anim_ease_in_out(&out);
    lv_anim_set_completed_cb(&out, theme_wash_out_complete);
    lv_anim_start(&out);
}

static void theme_wash_start(void)
{
    if (theme_wash) {
        lv_obj_delete(theme_wash);
        theme_wash = NULL;
    }

    theme_wash = lv_obj_create(lv_layer_top());
    lv_obj_set_size(theme_wash, lv_pct(100), lv_pct(100));
    lv_obj_align(theme_wash, LV_ALIGN_TOP_LEFT, 0, 0);
    qz_obj_set_bg_color(theme_wash, QZ_ACCENT, 0);
    lv_obj_set_style_bg_opa(theme_wash, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(theme_wash, 0, 0);
    lv_obj_set_style_radius(theme_wash, 0, 0);
    lv_obj_set_style_pad_all(theme_wash, 0, 0);
    lv_obj_clear_flag(theme_wash, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(theme_wash, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(theme_wash, theme_wash_deleted, LV_EVENT_DELETE, NULL);

    lv_anim_t in;
    lv_anim_init(&in);
    lv_anim_set_var(&in, theme_wash);
    lv_anim_set_exec_cb(&in, theme_wash_opa);
    lv_anim_set_values(&in, LV_OPA_TRANSP, 18);
    lv_anim_set_duration(&in, QZ_DUR_THEME * 30 / 100);
    qz_anim_ease_out(&in);
    lv_anim_set_completed_cb(&in, theme_wash_in_complete);
    lv_anim_start(&in);
}

bool qz_theme_set(qz_theme_id_t theme)
{
    theme = valid_theme(theme);
    if (g_theme == theme) return false;
    g_theme = theme;
    theme_refresh_all_records();
    lv_obj_t *active = lv_screen_active();
    if (active) {
        if (theme_reduce_motion()) {
            lv_obj_invalidate(active);
        } else {
            theme_wash_start();
        }
    }
    return true;
}

bool qz_theme_set_dark(bool dark)
{
    if (qz_theme_is_dark() == dark) return false;
    return qz_theme_set(dark ? QZ_THEME_NIGHT : QZ_THEME_MINERAL);
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
    /* Pull the persisted palette into the environment before any token is
     * read, so the first frame is already in the right theme. */
    qz_appearance_load();
    const char *theme_env = getenv("QZDESK_THEME");
    qz_theme_id_t loaded;
    if (theme_env && qz_theme_parse(theme_env, &loaded)) {
        g_theme = loaded;
    } else {
        const char *dark_env = getenv("QZDESK_DARK");
        if (dark_env && qz_theme_parse(dark_env, &loaded)) g_theme = loaded;
    }

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
    /* 正文字号也按面板缩放（见 include/scale.h）：设计稿 16px，320 面板上 12px。 */
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
    /* One neutral canvas. Flat surfaces keep rules and data exactly aligned
     * instead of introducing decorative tonal shifts. */
    qz_obj_set_bg_color(obj, QZ_BG, 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    qz_obj_set_text_color(obj, QZ_TEXT, 0);
    lv_obj_set_style_text_font(obj, qz_font(), 0);
}

void qz_style_header(lv_obj_t *obj)
{
    /* 页头是**贴边**的：全宽、直角、无阴影，底部一根发丝线。以前它是缩进 8px
     * 的浮空胶囊，那圈留白加阴影在 320×240 上要吃掉三行内容的高度，换来的只是
     * 一层玻璃感 —— 而玻璃的价值是"浮在内容之上"，页头并不浮在什么之上。
     * 材质本身留着：填充仍是 QZ_BAR + 材质不透明度，滑杆一样调得动。 */
    lv_obj_set_style_radius(obj, 0, 0);
    qz_obj_set_bg_color(obj, QZ_BAR, 0);
    lv_obj_set_style_bg_opa(obj, (lv_opa_t)material_glass_opa, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    qz_obj_set_border_color(obj, QZ_SEPARATOR, 0);
    lv_obj_set_style_border_opa(obj, (lv_opa_t)150, 0);
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    /* 页头是chrome，本身没有动作：默认的 CLICKABLE 只会让它在命中区审计里
     * 冒充一个 320×39 的可点对象，也顺手把没有目标的空白变成"点了有反应"。 */
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    qz_material_register(obj, true);
}

lv_obj_t *qz_page_header(lv_obj_t *screen, const char *title)
{
    lv_obj_t *header = lv_obj_create(screen);
    lv_obj_set_size(header, lv_pct(100), QZ_HEADER_H);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    qz_style_header(header);
    /* 标题固定排在返回键右边（58 = 8 内缩 + 44 返回键 + 6）：五个页面与所有
     * 二级页的标题都落在同一条竖线上，翻页时文字不会左右跳。 */
    lv_obj_t *label = qz_text(header, title, 22, qz_color(QZ_TEXT));
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 58, 0);
    return header;
}

void qz_style_toolbar(lv_obj_t *obj)
{
    /* Toolbars are work surfaces, not floating pills. */
    lv_obj_set_style_radius(obj, QZ_RADIUS_CARD, 0);
    qz_obj_set_bg_color(obj, QZ_BAR, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    qz_obj_set_border_color(obj, QZ_SEPARATOR, 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
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
}

void qz_style_slider(lv_obj_t *obj)
{
    /* The object is 26px tall: 9px of padding leave an 8px precision track. */
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
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_KNOB);
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
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_KNOB);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

void qz_style_textarea(lv_obj_t *obj)
{
    /* A filled field on the composer bar: neutral fill, no translucency. */
    lv_obj_set_style_radius(obj, 6, 0);
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
    lv_obj_set_style_radius(obj, 4, LV_PART_ITEMS);
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
    qz_color_token_t token;
    if (color_token_for(text_color, &token)) {
        qz_obj_set_text_color(obj, token, 0);
    } else {
        lv_obj_set_style_text_color(obj, text_color, 0);
    }
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
    { LV_SYMBOL_TRASH, QZ_ICON_TRASH },
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
    /* 传进来的是设计稿尺寸：先按当前面板换成物理像素，再挑"最接近那一档"
     * 的素材（见 qz_icon）。小屏用到的 11~23px 都有精确档位，所以这里永远
     * 1:1 贴图；LVGL 的软件 transform 缩图是最近邻，线性图标一缩就发虚。 */
    int32_t want = qz_scale_px(px);
    const lv_image_dsc_t *glyph = qz_icon(icon, (int)want);
    if (!glyph) return NULL;

    lv_obj_t *image = lv_image_create(parent);
    lv_image_set_src(image, glyph);
    /* A8 蒙版 + 全量重着色：颜色就是主题色，浅色/深色共用一份资源 */
    qz_color_token_t token;
    if (color_token_for(color, &token)) {
        qz_obj_set_image_recolor(image, token, 0);
    } else {
        lv_obj_set_style_image_recolor(image, color, 0);
    }
    lv_obj_set_style_image_recolor_opa(image, LV_OPA_COVER, 0);
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
    qz_color_token_t token;
    if (color_token_for(color, &token)) {
        qz_obj_set_text_color(obj, token, 0);
    } else {
        lv_obj_set_style_text_color(obj, color, 0);
    }
    return obj;
}

lv_obj_t *qz_squircle(lv_obj_t *parent, int size, qz_color_token_t color)
{
    /* Flat, one colour, no shadow — a filled glyph tile the way iOS Settings
     * draws them. The caller passes the role: tiles now keep one role across a
     * whole grid instead of alternating accent / neutral, so a filled tile can
     * still mean "this one is selected" wherever it appears. */
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
    points[1].x = qz_scale_x(size / 2);
    points[1].y = qz_scale_y(size / 2);
    points[2].x = 0;
    points[2].y = qz_scale_y(size);

    lv_obj_t *line = lv_line_create(parent);
    lv_line_set_points(line, points, 3);
    lv_obj_set_style_line_width(line, size > 20 ? 3 : 2, 0);
    qz_color_token_t token;
    if (color_token_for(color, &token)) {
        qz_obj_set_line_color(line, token, 0);
    } else {
        lv_obj_set_style_line_color(line, color, 0);
    }
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
    /* An arc is geometrically square. Scale one axis only, otherwise an
     * anisotropic panel (for example 320x320 against the 480x320 design)
     * turns it into an oval and, for small glyph parts, makes it overflow
     * the square parent. */
    int32_t side = qz_scale_px(width < height ? width : height);
    (lv_obj_set_size)(arc, side, side);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(arc, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, thickness, LV_PART_INDICATOR);
    qz_color_token_t token;
    if (color_token_for(color, &token)) {
        qz_obj_set_arc_color(arc, token, LV_PART_INDICATOR);
    } else {
        lv_obj_set_style_arc_color(arc, color, LV_PART_INDICATOR);
    }
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
    /* The workhorse surface is a solid sheet separated by a hairline. */
    qz_obj_set_bg_color(obj, QZ_CARD, 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(obj, (lv_opa_t)material_opa, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    qz_obj_set_border_color(obj, QZ_SEPARATOR, 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, QZ_RADIUS_CARD, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    qz_material_register(obj, false);
}

void qz_style_glass(lv_obj_t *obj)
{
    /* Chrome uses the same flat surface language as content, separated by a
     * rule rather than a glow. */
    qz_obj_set_bg_color(obj, QZ_BAR, 0);
    lv_obj_set_style_bg_opa(obj, (lv_opa_t)material_glass_opa, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    qz_obj_set_border_color(obj, QZ_SEPARATOR, 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, QZ_RADIUS_CARD, 0);
    qz_material_register(obj, true);
}

void qz_style_sheet(lv_obj_t *panel)
{
    /* Modal sheets stay opaque; the scrim and a precise border carry depth. */
    qz_obj_set_bg_color(panel, QZ_CARD, 0);
    lv_obj_set_style_bg_grad_dir(panel, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    qz_obj_set_border_color(panel, QZ_SEPARATOR, 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_COVER, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(panel, QZ_RADIUS_CARD, 0);
    lv_obj_set_style_shadow_width(panel, 0, 0);
    lv_obj_set_style_clip_corner(panel, true, 0);
}

lv_obj_t *qz_card_button(lv_obj_t *parent, int width, int height)
{
    lv_obj_t *card = lv_button_create(parent);
    lv_obj_set_size(card, width, height);
    qz_touch_target(card);
    lv_obj_set_style_radius(card, QZ_RADIUS_CARD, 0);
    /* A compact working tile, separated by the canvas rather than a shadow. */
    qz_obj_set_bg_color(card, QZ_CARD, 0);
    lv_obj_set_style_bg_opa(card, (lv_opa_t)material_opa, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    qz_obj_set_border_color(card, QZ_SEPARATOR, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    qz_obj_set_bg_color(card, QZ_FILL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(card, (lv_opa_t)material_opa, LV_STATE_PRESSED);
    qz_material_register(card, false);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    qz_add_press_feedback(card);
    /* Touch feedback stays a single causal colour response. */
    return card;
}

/* 2 列网格：间距与格子高。实际显示 2×2，多的行往下滑。 */
#define QZ_GRID_GAP 14
#define QZ_GRID_TILE_H 96

lv_obj_t *qz_back_button(lv_obj_t *parent)
{
    lv_obj_t *btn = qz_icon_button(parent, LV_SYMBOL_LEFT, QZ_TOUCH_BACK);
    lv_obj_clean(btn);
    lv_obj_t *glyph = qz_icon_image(btn, QZ_ICON_CHEVRON_LEFT,
                                    qz_design_px(QZ_TOUCH_BACK / 2),
                                    qz_color(QZ_TEXT));
    if (glyph) lv_obj_center(glyph);
    /* 视觉上仍是原来的小圆，命中区由 qz_touch_target 补到物理下限；再往外
     * 多给一圈，因为在页头里它边上没有别的控件可抢。 */
    lv_obj_set_ext_click_area(btn, qz_touch_target(btn) + 4);
    return btn;
}

/* 滚动条是"这根列表还能拽"的反馈，不是装饰：静止时不该有一条灰线钉在屏幕
 * 边缘（290 宽的屏上尤其显眼）。LVGL 的 AUTO 是"内容能滚就常显"，ACTIVE 才是
 * "正在滚时才显"。同时把它压细、往内收，免得贴着边框像掉了一根线。 */
static void style_scrollbar(lv_obj_t *obj)
{
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(obj, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(obj, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(obj, LV_OPA_50, LV_PART_SCROLLBAR);
    qz_obj_set_bg_color(obj, QZ_TEXT_TERTIARY, LV_PART_SCROLLBAR);
}

void qz_make_scrollable(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(obj, LV_DIR_VER);
    style_scrollbar(obj);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_SCROLL_MOMENTUM);
}

lv_obj_t *qz_page_scroll(lv_obj_t *screen, int32_t top)
{
    lv_obj_t *area = lv_obj_create(screen);
    lv_obj_set_size(area, lv_pct(100), QZ_DESIGN_H - top - QZ_GUTTER);
    lv_obj_align(area, LV_ALIGN_TOP_MID, 0, top);
    lv_obj_set_style_bg_opa(area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(area, 0, 0);
    lv_obj_set_style_shadow_width(area, 0, 0);
    lv_obj_set_style_pad_all(area, 0, 0);
    lv_obj_set_scroll_dir(area, LV_DIR_VER);
    style_scrollbar(area);
    lv_obj_add_flag(area, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_flag(area, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    return area;
}

void qz_grid_metrics(int32_t *tile_w, int32_t *tile_h)
{
    int32_t content = QZ_DESIGN_W - 2 * QZ_GUTTER;
    if (tile_w) *tile_w = (content - QZ_GRID_GAP) / 2;
    /* 高度按"实机至少 64px"取：太小手指会点串行 */
    if (tile_h) *tile_h = QZ_GRID_TILE_H;
}

void qz_grid_pos(int32_t col, int32_t row, int32_t *x, int32_t *y)
{
    int32_t w, h;
    qz_grid_metrics(&w, &h);
    if (x) *x = QZ_GUTTER + col * (w + QZ_GRID_GAP);
    if (y) *y = row * (h + QZ_GRID_GAP);
}

lv_obj_t *qz_button(lv_obj_t *parent, const char *caption, int width, int height)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, width, height);
    qz_touch_target(btn);
    lv_obj_set_style_radius(btn, 4, 0);
    qz_obj_set_bg_color(btn, QZ_FILL, 0);
    qz_obj_set_bg_color(btn, QZ_FILL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    qz_add_press_feedback(btn);
    lv_obj_t *caption_label = qz_text(btn, caption, 15, qz_color(QZ_TEXT));
    lv_obj_center(caption_label);
    return btn;
}

lv_obj_t *qz_chip(lv_obj_t *parent, const char *caption, int width, int height)
{
    lv_obj_t *chip = qz_button(parent, caption, width, height);
    /* Suggestion chips are compact keys, not decorative pills. */
    qz_obj_set_bg_color(chip, QZ_ACCENT_TINT, 0);
    qz_obj_set_bg_color(chip, QZ_FILL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_bg_grad_dir(chip, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_border_width(chip, 0, 0);
    lv_obj_set_style_radius(chip, 4, 0);
    lv_obj_t *caption_label = lv_obj_get_child(chip, 0);
    if (caption_label) qz_obj_set_text_color(caption_label, QZ_ACCENT_TEXT, 0);
    return chip;
}

lv_obj_t *qz_icon_button(lv_obj_t *parent, const char *icon, int size)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, size, size);
    qz_touch_target(btn);
    /* Square-ish controls align to the workstation grid. */
    lv_obj_set_style_radius(btn, 5, 0);
    qz_obj_set_bg_color(btn, QZ_FILL, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    qz_obj_set_bg_color(btn, QZ_FILL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    qz_add_press_feedback(btn);
    lv_obj_t *label = qz_symbol(btn, icon, size / 2, qz_color(QZ_TEXT));
    lv_obj_center(label);
    return btn;
}

void qz_style_primary_button(lv_obj_t *obj)
{
    /* The single decisive action on a screen. */
    lv_obj_set_style_radius(obj, 4, 0);
    qz_obj_set_bg_color(obj, QZ_ACCENT, 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_NONE, 0);
    qz_obj_set_bg_color(obj, QZ_ACCENT_DARK, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    qz_add_press_feedback(obj);
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
                                     reduce_motion ? 0 : QZ_DUR_PRESS, 0, NULL);
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

static void glint_hide(void) {}

void qz_add_touch_glint(lv_obj_t *obj)
{
    /* Compatibility hook. The workstation language uses state colour and
     * content motion instead of a moving specular highlight. */
    (void)obj;
}

/** 当前盖在页面上的遮罩/弹层：手势用它判断"这一下返回该不该交给弹层"。 */
#define QZ_OVERLAY_MAX 8
static lv_obj_t *overlays[QZ_OVERLAY_MAX];
static int overlay_count;

static void overlay_deleted(lv_event_t *event)
{
    lv_obj_t *overlay = lv_event_get_target(event);
    for (int i = 0; i < overlay_count; i++) {
        if (overlays[i] != overlay) continue;
        memmove(&overlays[i], &overlays[i + 1], sizeof(overlays[0]) * (overlay_count - i - 1));
        overlays[--overlay_count] = NULL;
        return;
    }
}

/** 弹层是不是盖在眼前那页上。
 *
 *  页面对象一直活着，随手势/语音翻走的那一页上留着的弹层也还在——只认"还
 *  看得见的"，否则一次"开弹层 → 语音翻页"就把返回手势永久锁死了。 */
static bool overlay_visible(lv_obj_t *overlay)
{
    if (!overlay) return false;
    lv_display_t *display = lv_obj_get_display(overlay);
    lv_obj_t *active = lv_display_get_screen_active(display);
    lv_obj_t *top = lv_display_get_layer_top(display);
    for (lv_obj_t *object = overlay; object; object = lv_obj_get_parent(object)) {
        if (object == active || object == top) return true;
    }
    return false;
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
    if (overlay_count < QZ_OVERLAY_MAX) overlays[overlay_count++] = overlay;
    lv_obj_add_event_cb(overlay, overlay_deleted, LV_EVENT_DELETE, NULL);
    return overlay;
}

bool qz_overlay_open(void)
{
    for (int i = 0; i < overlay_count; i++) {
        if (overlay_visible(overlays[i])) return true;
    }
    return false;
}

bool qz_overlay_close_top(void)
{
    for (int i = overlay_count - 1; i >= 0; i--) {
        lv_obj_t *overlay = overlays[i];
        if (!overlay_visible(overlay)) continue;
        /* 所有经由 qz_overlay_create() 创建的弹层都把点空白关闭挂在遮罩
         * 本身；显式发送 CLICKED 比复制各页面自己的关闭状态更可靠。 */
        lv_obj_send_event(overlay, LV_EVENT_CLICKED, overlay);
        return true;
    }
    return false;
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
    lv_obj_set_style_translate_y(obj, 2, 0);

    lv_anim_t opacity;
    lv_anim_init(&opacity);
    lv_anim_set_var(&opacity, obj);
    lv_anim_set_exec_cb(&opacity, entrance_opa);
    lv_anim_set_values(&opacity, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&opacity, QZ_DUR_SMALL);
    lv_anim_set_delay(&opacity, delay_ms);
    qz_anim_ease_out(&opacity);
    lv_anim_start(&opacity);

    lv_anim_t translate;
    lv_anim_init(&translate);
    lv_anim_set_var(&translate, obj);
    lv_anim_set_exec_cb(&translate, entrance_translate_y);
    lv_anim_set_values(&translate, 2, 0);
    lv_anim_set_duration(&translate, QZ_DUR_SMALL);
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

/* ------------------------------------------------------------------------- *
 * Screen history
 *
 * 返回键和侧滑返回共用这条历史：MOVE_LEFT 是往前翻一页（入栈），MOVE_RIGHT
 * 是退回去（出栈）。页面对象一直活着，所以这里只记"刚才是从哪来的"，退回去
 * 不需要销毁任何东西。
 * ------------------------------------------------------------------------- */
#define QZ_NAV_DEPTH 8
static lv_obj_t *nav_stack[QZ_NAV_DEPTH];
static int nav_depth;

/** 少数页面有比"退回上一页"更细的返回逻辑（对话页要先收键盘），登记在这里。 */
#define QZ_BACK_HANDLERS 4
static struct {
    lv_obj_t *screen;
    void (*handler)(void);
} back_handlers[QZ_BACK_HANDLERS];

void qz_screen_set_back_handler(lv_obj_t *screen, void (*handler)(void))
{
    if (!screen || !handler) return;
    int free_slot = -1;
    for (int i = 0; i < QZ_BACK_HANDLERS; i++) {
        if (back_handlers[i].screen == screen) {
            back_handlers[i].handler = handler;
            return;
        }
        if (!back_handlers[i].screen && free_slot < 0) free_slot = i;
    }
    if (free_slot >= 0) {
        back_handlers[free_slot].screen = screen;
        back_handlers[free_slot].handler = handler;
    }
}

static void nav_record(lv_obj_t *screen)
{
    if (!screen) return;
    if (nav_depth > 0 && nav_stack[nav_depth - 1] == screen) return;
    if (nav_depth == QZ_NAV_DEPTH) {
        /* 满栈就丢掉最老的一页：用户记得住的是刚才那几屏。 */
        memmove(nav_stack, nav_stack + 1, sizeof(nav_stack[0]) * (QZ_NAV_DEPTH - 1));
        nav_depth--;
    }
    nav_stack[nav_depth++] = screen;
}

static void nav_push(lv_obj_t *screen)
{
    /* 第一次导航时栈还是空的（主页是开机时直接 load 的），把离开的那页补成起点。 */
    if (nav_depth == 0) nav_record(lv_screen_active());
    nav_record(screen);
}

static void nav_pop(lv_obj_t *screen)
{
    if (lv_screen_active() == screen) return;   /* 原地重载：历史不动 */
    if (nav_depth > 0 && nav_stack[nav_depth - 1] == lv_screen_active()) nav_depth--;
    /* 目标页未必紧挨着上一页：小程序既能回主页也能回应用页，WLAN 页也能直接
     * 回主页。一路退到它为止；真找不到就把它当成新的起点。 */
    while (nav_depth > 0 && nav_stack[nav_depth - 1] != screen) nav_depth--;
    if (nav_depth == 0) nav_record(screen);
}

void qz_screen_back(void)
{
    lv_obj_t *active = lv_screen_active();
    for (int i = 0; i < QZ_BACK_HANDLERS; i++) {
        if (back_handlers[i].screen == active && back_handlers[i].handler) {
            back_handlers[i].handler();
            return;
        }
    }
    if (nav_depth < 2) return;
    qz_screen_load(nav_stack[nav_depth - 2], LV_SCR_LOAD_ANIM_MOVE_RIGHT, QZ_DUR_SCREEN);
}

/* A second navigation request must not cancel a screen animation halfway
 * through: LVGL snaps the interrupted page into place before starting the next
 * one, which reads as a flash. Keep only the newest request and start it after
 * the current SCREEN_LOADED event has fully unwound. */
static lv_obj_t *nav_transition_target;
static lv_obj_t *nav_pending_target;
static lv_scr_load_anim_t nav_pending_anim;
static uint32_t nav_pending_duration;
static bool nav_pending_scheduled;
static int nav_motion_token;

static void nav_pending_start(void *unused)
{
    (void)unused;
    nav_pending_scheduled = false;
    if (!nav_pending_target) return;
    lv_obj_t *target = nav_pending_target;
    lv_scr_load_anim_t anim = nav_pending_anim;
    uint32_t duration = nav_pending_duration;
    nav_pending_target = NULL;
    qz_screen_load(target, anim, duration);
}

static void nav_screen_loaded(lv_event_t *event)
{
    lv_obj_t *loaded = lv_event_get_target(event);
    if (loaded != nav_transition_target) return;
    nav_transition_target = NULL;
    if (!nav_pending_target || nav_pending_scheduled) return;
    nav_pending_scheduled = true;
    /* Queue for the next timer tick; starting a new load inside SCREEN_LOADED
     * would race LVGL's own cleanup of the previous transition. */
    if (lv_async_call(nav_pending_start, NULL) != LV_RESULT_OK) {
        nav_pending_scheduled = false;
        nav_pending_target = NULL;
    }
}

void qz_screen_load(lv_obj_t *screen, lv_scr_load_anim_t anim, uint32_t duration_ms)
{
    if (!screen) return;
    /* 记历史要用调用方原本的动画方向，减速动效把它换成淡入之前就得记完。 */
    if (anim == LV_SCR_LOAD_ANIM_MOVE_LEFT) nav_push(screen);
    else if (anim == LV_SCR_LOAD_ANIM_MOVE_RIGHT) nav_pop(screen);
    /* A screen change can delete the widget that was being pressed, which is
     * exactly when the release event never arrives. */
    glint_hide();
    /* Bring the page about to be shown up to the current material. */
    material_apply_tree(screen);

    if (nav_transition_target) {
        nav_pending_target = screen;
        nav_pending_anim = reduce_motion ? LV_SCR_LOAD_ANIM_FADE_ON : anim;
        nav_pending_duration = reduce_motion ? QZ_DUR_QUICK : duration_ms;
        return;
    }

    if (screen == lv_screen_active()) return;

    if (reduce_motion) {
        /* Reduced motion still needs the change to be legible: cross fade
         * instead of a slide, and no translation. */
        anim = LV_SCR_LOAD_ANIM_FADE_ON;
        duration_ms = QZ_DUR_QUICK;
    }
    if (duration_ms == 0) {
        lv_screen_load_anim(screen, anim, 0, 0, false);
        return;
    }

    lv_obj_t *previous = lv_screen_active();
    lv_screen_load_anim(screen, anim, duration_ms, 0, false);

    lv_anim_t *incoming = lv_anim_get(screen, NULL);
    if (!incoming) return;  /* LVGL treated it as an immediate load. */
    lv_anim_t *outgoing = previous ? lv_anim_get(previous, NULL) : NULL;
    qz_anim_ease_in_out(incoming);
    if (outgoing) qz_anim_ease_in_out(outgoing);

    nav_transition_target = screen;
    /* One watcher per destination, replaced rather than accumulated. */
    lv_obj_remove_event_cb_with_user_data(screen, nav_screen_loaded, &nav_motion_token);
    lv_obj_add_event_cb(screen, nav_screen_loaded, LV_EVENT_SCREEN_LOADED,
                        &nav_motion_token);
}
