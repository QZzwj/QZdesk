#include "settings.h"
#include "config.h"
#include "theme.h"
#include "wifi.h"
#include "qzdesk_core.h"
#include "notifications.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define CONTENT_W (QZ_DESIGN_W - 2 * QZ_GUTTER)
#define CARD_PAD 12
#define MODAL_KEYBOARD_H 136

static lv_obj_t *desktop_screen;
static lv_obj_t *settings_screen;
static lv_obj_t *settings_return;
static lv_obj_t *wlan_return;
static lv_obj_t *wlan_screen;
static lv_obj_t *general_screen;
static lv_obj_t *about_screen;
static char about_server[96];

/* Two labels show the same connection summary: the card on the WLAN page and
 * the row value on the settings list. They need separate pointers - reusing one
 * silently orphaned whichever screen was built first. */
static lv_obj_t *wlan_page_summary;
static lv_obj_t *wlan_row_summary;
static lv_obj_t *wlan_toggle;
static lv_obj_t *wlan_list;
static lv_obj_t *time_value;
static lv_obj_t *timezone_value;
static lv_obj_t *modal_overlay;
static lv_obj_t *settings_rail_clock;
static lv_timer_t *settings_clock_timer;

#define THEME_CHOICE_MAX 4
#define APPEARANCE_STATUS_MAX 2

typedef struct {
    lv_obj_t *button;
    lv_obj_t *indicator;
    lv_obj_t *label;
    qz_theme_id_t theme;
} theme_choice_t;

static theme_choice_t theme_choices[THEME_CHOICE_MAX];
static int theme_choice_count;
static lv_obj_t *appearance_status[APPEARANCE_STATUS_MAX];
static int appearance_status_count;
static lv_obj_t *theme_summary;

static char pending_ssid[64];
static bool wlan_enabled = true;
static bool wlan_auto_scan;

static void wlan_network_clicked(lv_event_t *event);
static void wlan_refresh_list(void);
static void wlan_current_clicked(lv_event_t *event);
static void wlan_browse_clicked(lv_event_t *event);
static void wlan_disconnect_clicked(lv_event_t *event);
static void wlan_forget_clicked(lv_event_t *event);
static void open_details_modal(lv_obj_t *screen);
static void wlan_rebuild_async(void *unused);
static void open_general(lv_event_t *event);
static void apply_theme(qz_theme_id_t theme);

/* A slider keeps its percentage label and an optional hardware hook together,
 * so moving it both shows and applies the value. */
typedef struct {
    lv_obj_t *label;
    lv_obj_t *slider;
    bool (*apply)(int);
} slider_binding_t;

/* Audio and backlight settings live in the 通用设置 page but are used from the
 * WLAN page handler as well (to re-sync on screen entry). */
static slider_binding_t volume_binding;
static slider_binding_t backlight_binding;
/* Drives the white material's opacity for the whole interface, live. */
static slider_binding_t material_binding;
static void slider_sync(slider_binding_t *binding, int value);

/* ------------------------------------------------------------------------- *
 * Shared building blocks
 * ------------------------------------------------------------------------- */

static void go_back(lv_event_t *event)
{
    lv_obj_t *target = (lv_obj_t *)lv_event_get_user_data(event);
    if (lv_screen_active() == wlan_screen && wlan_return) {
        target = wlan_return;
        wlan_return = NULL;
    }
    if (!target) target = settings_return ? settings_return : desktop_screen;
    qz_screen_load(target, LV_SCR_LOAD_ANIM_MOVE_RIGHT, QZ_DUR_SCREEN);
}

static lv_obj_t *page_toolbar(lv_obj_t *screen, const char *title, lv_obj_t *back_target)
{
    lv_obj_t *header = qz_page_header(screen, title);
    lv_obj_t *back = qz_back_button(header);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_add_event_cb(back, go_back, LV_EVENT_CLICKED, back_target);
    return header;
}

/** Continuous instrument band: square corners, one hairline, no floating card. */
static lv_obj_t *section_card(lv_obj_t *parent, int y, int height, bool interactive)
{
    lv_obj_t *card = interactive ? qz_card_button(parent, CONTENT_W, height)
                                 : lv_obj_create(parent);
    if (!interactive) {
        lv_obj_set_size(card, CONTENT_W, height);
        qz_style_plate(card);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        /* 纯容器：底板自己不带动作，留着默认的 CLICKABLE 只会在命中区审计里
         * 冒出一整块假目标（行内真正可点的子对象不受父级标志影响）。 */
        lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_set_style_radius(card, 0, 0);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_shadow_width(card, 0, 0);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, QZ_GUTTER, y);
    return card;
}

static void theme_option_transition(lv_obj_t *obj)
{
    static const lv_style_prop_t props[] = {
        LV_STYLE_BG_COLOR,
        LV_STYLE_BG_OPA,
        LV_STYLE_BORDER_COLOR,
        LV_STYLE_PROP_INV,
    };
    static lv_style_transition_dsc_t transition;
    static bool ready;
    if (!ready) {
        lv_style_transition_dsc_init(&transition, props, lv_anim_path_ease_out,
                                     qz_reduce_motion() ? 0 : QZ_DUR_SMALL, 0, NULL);
        ready = true;
    }
    lv_obj_set_style_transition(obj, &transition, 0);
}

static void theme_label_transition(lv_obj_t *obj)
{
    static const lv_style_prop_t props[] = {
        LV_STYLE_TEXT_COLOR,
        LV_STYLE_PROP_INV,
    };
    static lv_style_transition_dsc_t transition;
    static bool ready;
    if (!ready) {
        lv_style_transition_dsc_init(&transition, props, lv_anim_path_ease_out,
                                     qz_reduce_motion() ? 0 : QZ_DUR_SMALL, 0, NULL);
        ready = true;
    }
    lv_obj_set_style_transition(obj, &transition, 0);
}

static void theme_indicator_transition(lv_obj_t *obj)
{
    static const lv_style_prop_t props[] = {
        LV_STYLE_BG_OPA,
        LV_STYLE_BG_COLOR,
        LV_STYLE_PROP_INV,
    };
    static lv_style_transition_dsc_t transition;
    static bool ready;
    if (!ready) {
        lv_style_transition_dsc_init(&transition, props, lv_anim_path_ease_out,
                                     qz_reduce_motion() ? 0 : QZ_DUR_SMALL, 0, NULL);
        ready = true;
    }
    lv_obj_set_style_transition(obj, &transition, 0);
}

static void sync_theme_choices(void)
{
    qz_theme_id_t active = qz_theme_get_id();
    for (int i = 0; i < theme_choice_count; i++) {
        theme_choice_t *choice = &theme_choices[i];
        bool selected = choice->theme == active;
        if (selected) lv_obj_add_state(choice->button, LV_STATE_CHECKED);
        else lv_obj_remove_state(choice->button, LV_STATE_CHECKED);
        if (choice->indicator) {
            lv_obj_set_style_bg_opa(choice->indicator,
                                    selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }
        if (choice->label) {
            qz_obj_set_text_color(choice->label,
                                  selected ? QZ_ACCENT_TEXT : QZ_TEXT_SECONDARY, 0);
        }
    }
    if (theme_summary) {
        char summary[64];
        snprintf(summary, sizeof(summary), "%s · %s",
                 qz_theme_id_name(active), qz_theme_id_caption(active));
        lv_label_set_text(theme_summary, summary);
    }
}

static void sync_appearance_status(bool saved)
{
    const char *text = saved ? "已保存" : "仅本次";
    qz_color_token_t color = saved ? QZ_TEXT_TERTIARY : QZ_RED;
    for (int i = 0; i < appearance_status_count; i++) {
        if (!appearance_status[i]) continue;
        lv_label_set_text(appearance_status[i], text);
        qz_obj_set_text_color(appearance_status[i], color, 0);
    }
}

static void apply_theme(qz_theme_id_t theme)
{
    qz_theme_set(theme);
    bool saved = qz_appearance_save_theme(qz_theme_id_key(theme));
    sync_theme_choices();
    sync_appearance_status(saved);
}

static void theme_choice_clicked(lv_event_t *event)
{
    apply_theme((qz_theme_id_t)(intptr_t)lv_event_get_user_data(event));
}

static void register_theme_choice(lv_obj_t *button, lv_obj_t *indicator,
                                  lv_obj_t *label, qz_theme_id_t theme)
{
    if (theme_choice_count >= THEME_CHOICE_MAX) return;
    theme_choices[theme_choice_count++] = (theme_choice_t){
        .button = button,
        .indicator = indicator,
        .label = label,
        .theme = theme,
    };
}

static void preview_part(lv_obj_t *obj, lv_color_t color)
{
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(obj, color, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

/* Previews stay literal so all four palettes remain comparable while one of
 * them is active. */
static lv_obj_t *theme_preview(lv_obj_t *parent, qz_theme_id_t theme)
{
    lv_obj_t *preview = lv_obj_create(parent);
    lv_obj_set_size(preview, 38, 28);
    lv_obj_clear_flag(preview, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(preview, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(preview, 3, 0);
    lv_obj_set_style_clip_corner(preview, true, 0);
    lv_obj_set_style_bg_color(preview, qz_color_theme(QZ_BG, theme), 0);
    lv_obj_set_style_bg_opa(preview, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(preview, 1, 0);
    lv_obj_set_style_border_color(preview, qz_color_theme(QZ_SEPARATOR, theme), 0);
    lv_obj_set_style_border_opa(preview, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(preview, 0, 0);
    lv_obj_set_style_shadow_width(preview, 0, 0);

    lv_obj_t *header = lv_obj_create(preview);
    lv_obj_set_size(header, lv_pct(100), 8);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 0, 0);
    preview_part(header, qz_color_theme(QZ_CARD, theme));
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(header, qz_color_theme(QZ_SEPARATOR, theme), 0);
    lv_obj_set_style_border_opa(header, (lv_opa_t)150, 0);

    lv_obj_t *dot = lv_obj_create(preview);
    lv_obj_set_size(dot, 4, 4);
    lv_obj_align(dot, LV_ALIGN_TOP_LEFT, 4, 14);
    preview_part(dot, qz_color_theme(QZ_ACCENT, theme));
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);

    lv_obj_t *surface = lv_obj_create(preview);
    lv_obj_set_size(surface, 23, 12);
    lv_obj_align(surface, LV_ALIGN_TOP_LEFT, 11, 12);
    preview_part(surface, qz_color_theme(QZ_CARD, theme));
    lv_obj_set_style_radius(surface, 2, 0);
    lv_obj_set_style_border_width(surface, 1, 0);
    lv_obj_set_style_border_color(surface, qz_color_theme(QZ_SEPARATOR, theme), 0);
    lv_obj_set_style_border_opa(surface, (lv_opa_t)150, 0);

    lv_obj_t *line = lv_obj_create(surface);
    lv_obj_set_size(line, 11, 2);
    lv_obj_align(line, LV_ALIGN_TOP_LEFT, 4, 3);
    preview_part(line, qz_color_theme(QZ_TEXT_TERTIARY, theme));
    lv_obj_set_style_radius(line, 1, 0);

    lv_obj_t *accent_line = lv_obj_create(surface);
    lv_obj_set_size(accent_line, 7, 2);
    lv_obj_align(accent_line, LV_ALIGN_TOP_LEFT, 4, 7);
    preview_part(accent_line, qz_color_theme(QZ_ACCENT, theme));
    lv_obj_set_style_radius(accent_line, 1, 0);
    return preview;
}

static lv_obj_t *theme_option(lv_obj_t *parent, int x, int y, int width,
                              int height, qz_theme_id_t theme)
{
    lv_obj_t *option = lv_button_create(parent);
    lv_obj_set_size(option, width, height);
    lv_obj_align(option, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_radius(option, 4, 0);
    qz_obj_set_bg_color(option, QZ_FILL, 0);
    lv_obj_set_style_bg_opa(option, LV_OPA_COVER, 0);
    qz_obj_set_bg_color(option, QZ_ACCENT_TINT, LV_STATE_CHECKED);
    qz_obj_set_bg_color(option, QZ_FILL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(option, 1, 0);
    qz_obj_set_border_color(option, QZ_SEPARATOR, 0);
    qz_obj_set_border_color(option, QZ_ACCENT, LV_STATE_CHECKED);
    lv_obj_set_style_shadow_width(option, 0, 0);
    lv_obj_set_style_pad_all(option, 0, 0);
    lv_obj_clear_flag(option, LV_OBJ_FLAG_SCROLLABLE);
    qz_touch_target(option);
    theme_option_transition(option);

    lv_obj_t *preview = theme_preview(option, theme);
    lv_obj_align(preview, LV_ALIGN_LEFT_MID, 8, 0);

    lv_obj_t *label = qz_text(option, qz_theme_id_name(theme), 14,
                              qz_color(QZ_TEXT_SECONDARY));
    /* At the 320x240 floor both labels resolve to the same 12 px font. Give
     * them 22 design pixels of separation so their line boxes cannot collide,
     * while keeping the pair vertically centred in the compact 50 px option. */
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 55, -11);
    theme_label_transition(label);

    lv_obj_t *caption = qz_text(option, qz_theme_id_caption(theme), 10,
                                qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(caption, LV_ALIGN_LEFT_MID, 55, 11);

    lv_obj_t *indicator = lv_obj_create(option);
    lv_obj_set_size(indicator, 8, 8);
    lv_obj_align(indicator, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_set_style_radius(indicator, LV_RADIUS_CIRCLE, 0);
    qz_obj_set_bg_color(indicator, QZ_ACCENT, 0);
    lv_obj_set_style_bg_opa(indicator, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(indicator, 0, 0);
    lv_obj_set_style_shadow_width(indicator, 0, 0);
    lv_obj_clear_flag(indicator, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(indicator, LV_OBJ_FLAG_SCROLLABLE);
    theme_indicator_transition(indicator);

    lv_obj_add_event_cb(option, theme_choice_clicked, LV_EVENT_CLICKED,
                        (void *)(intptr_t)theme);
    register_theme_choice(option, indicator, label, theme);
    return option;
}

static lv_obj_t *appearance_panel(lv_obj_t *parent, int y, int height)
{
    lv_obj_t *panel = section_card(parent, y, height, false);
    lv_obj_set_style_clip_corner(panel, true, 0);

    lv_obj_t *title = qz_text(panel, "外观", 13, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, CARD_PAD, 8);
    lv_obj_t *status = qz_text(panel, "已保存", 11,
                               qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(status, LV_ALIGN_TOP_RIGHT, -CARD_PAD, 9);
    if (appearance_status_count < APPEARANCE_STATUS_MAX) {
        appearance_status[appearance_status_count++] = status;
    }

    const int gap = 6;
    const int option_x = CARD_PAD;
    const int option_w = (CONTENT_W - 2 * CARD_PAD - gap) / 2;
    const int option_y = 28;
    const int option_h = (height - 34 - gap) / 2;
    for (int i = 0; i < QZ_THEME_COUNT; i++) {
        int column = i % 2;
        int row = i / 2;
        theme_option(panel, option_x + column * (option_w + gap),
                     option_y + row * (option_h + gap), option_w, option_h,
                     (qz_theme_id_t)i);
    }
    sync_theme_choices();
    return panel;
}

/* Rows share one continuous instrument. The press wash is local to the row, so
 * the list stays calm at rest without turning every setting into a floating
 * card. */
static lv_obj_t *settings_menu_row(lv_obj_t *parent, int y, int height,
                                   qz_icon_t icon, const char *title,
                                   const char *detail, lv_event_cb_t cb,
                                   bool show_chevron,
                                   lv_obj_t **title_out, lv_obj_t **detail_out)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_size(row, lv_pct(100), height);
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    qz_obj_set_bg_color(row, QZ_FILL, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    qz_obj_set_border_color(row, QZ_SEPARATOR, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    qz_touch_target(row);
    qz_add_press_feedback(row);

    lv_obj_t *glyph = qz_icon_image(row, icon, 20, qz_color(QZ_ACCENT_DARK));
    lv_obj_align(glyph, LV_ALIGN_LEFT_MID, CARD_PAD, 0);

    lv_obj_t *title_label = qz_text(row, title, 16, qz_color(QZ_TEXT));
    lv_obj_set_width(title_label, CONTENT_W - 90);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, CARD_PAD + 32, 8);

    lv_obj_t *detail_label = qz_text(row, detail, 11, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_set_width(detail_label, CONTENT_W - 90);
    lv_label_set_long_mode(detail_label, LV_LABEL_LONG_DOT);
    lv_obj_align(detail_label, LV_ALIGN_TOP_LEFT, CARD_PAD + 32, 30);

    if (show_chevron) {
        lv_obj_t *chev = qz_chevron(row, qz_color(QZ_TEXT_TERTIARY), 13);
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -CARD_PAD, 0);
    }

    if (title_out) *title_out = title_label;
    if (detail_out) *detail_out = detail_label;
    if (cb) lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
    return row;
}

static lv_obj_t *info_row(lv_obj_t *parent, int y, int height, const char *title,
                          const char *value, lv_obj_t **value_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, lv_pct(100), height);
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_label = qz_text(row, title, 12, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, CARD_PAD, 0);
    lv_obj_t *value_label = qz_text(row, value, 12, qz_color(QZ_TEXT));
    /* Leave room for the disclosure chevron that the callers place on the row. */
    lv_obj_align(value_label, LV_ALIGN_RIGHT_MID, -CARD_PAD - 20, 0);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(value_label, CONTENT_W / 2 - 10);
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_WRAP);
    if (value_out) *value_out = value_label;
    return row;
}

/* ------------------------------------------------------------------------- *
 * Modal sheet
 * ------------------------------------------------------------------------- */

/* Only one sheet can be open at a time, so the open overlay is tracked in a
 * static. Event user data belongs to the caller (a textarea or a timezone
 * name) and must never be treated as the overlay. */
static void sheet_translate_y(void *var, int32_t value);
static void scrim_opa(void *var, int32_t value);
static void modal_delete_ready(lv_anim_t *anim);

static void close_modal(lv_event_t *event)
{
    (void)event;
    if (!modal_overlay) return;
    if (qz_reduce_motion()) {
        lv_obj_delete(modal_overlay);
        modal_overlay = NULL;
        return;
    }
    /* Leave the way it arrived, then delete: a hard cut on a sheet that slid
     * in reads like a glitch. */
    lv_obj_t *overlay = modal_overlay;
    lv_obj_t *panel = lv_obj_get_child(overlay, 0);
    modal_overlay = NULL;   /* a re-open during the exit starts clean */

    if (panel) {
        lv_anim_t slide;
        lv_anim_init(&slide);
        lv_anim_set_var(&slide, panel);
        lv_anim_set_exec_cb(&slide, sheet_translate_y);
        lv_anim_set_values(&slide, lv_obj_get_style_translate_y(panel, 0), 40);
        lv_anim_set_duration(&slide, QZ_DUR_PANEL);
        qz_anim_ease_out(&slide);
        lv_anim_start(&slide);
    }
    lv_anim_t scrim;
    lv_anim_init(&scrim);
    lv_anim_set_var(&scrim, overlay);
    lv_anim_set_exec_cb(&scrim, scrim_opa);
    lv_anim_set_values(&scrim, lv_obj_get_style_bg_opa(overlay, 0), LV_OPA_TRANSP);
    lv_anim_set_duration(&scrim, QZ_DUR_PANEL);
    qz_anim_ease_out(&scrim);
    lv_anim_set_completed_cb(&scrim, modal_delete_ready);
    lv_anim_start(&scrim);
}

static void modal_dismissed(lv_event_t *event)
{
    if (lv_event_get_target(event) != lv_event_get_current_target(event)) return;
    close_modal(event);
}

static void sheet_translate_y(void *var, int32_t value)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, value, 0);
}

static void sheet_opa(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

/* The scrim dims via its own background opacity: animating the overlay's opa
 * would rasterise a full screen layer. */
static void scrim_opa(void *var, int32_t value)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

static void modal_delete_ready(lv_anim_t *anim)
{
    /* The overlay that was animated out is the one to delete - it is not the
     * tracked one any more, because close_modal() clears the tracker up front
     * so a re-open during the exit starts clean. Comparing against the tracker
     * here would leak the sheet on screen forever. */
    lv_obj_t *overlay = (lv_obj_t *)anim->var;
    if (!overlay) return;
    lv_obj_delete(overlay);
    if (overlay == modal_overlay) modal_overlay = NULL;
}

static lv_obj_t *open_modal(lv_obj_t *screen, int32_t panel_height)
{
    modal_overlay = qz_overlay_create(screen);
    lv_obj_add_event_cb(modal_overlay, modal_dismissed, LV_EVENT_CLICKED, modal_overlay);

    lv_obj_t *panel = lv_obj_create(modal_overlay);
    lv_obj_set_size(panel, QZ_DESIGN_W - 40, panel_height);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 16);
    qz_style_sheet(panel);
    lv_obj_set_style_pad_all(panel, CARD_PAD, 0);

    if (!qz_reduce_motion()) {
        /* Sheet rises in while the scrim dims, both on the same clock so they
         * read as one surface. */
        lv_obj_set_style_translate_y(panel, 40, 0);
        lv_anim_t slide;
        lv_anim_init(&slide);
        lv_anim_set_var(&slide, panel);
        lv_anim_set_exec_cb(&slide, sheet_translate_y);
        lv_anim_set_values(&slide, 40, 0);
        lv_anim_set_duration(&slide, QZ_DUR_MODAL);
        qz_anim_ease_out(&slide);
        lv_anim_start(&slide);

        lv_obj_set_style_bg_opa(modal_overlay, LV_OPA_TRANSP, 0);
        lv_anim_t scrim;
        lv_anim_init(&scrim);
        lv_anim_set_var(&scrim, modal_overlay);
        lv_anim_set_exec_cb(&scrim, scrim_opa);
        lv_anim_set_values(&scrim, LV_OPA_TRANSP, 110);
        lv_anim_set_duration(&scrim, QZ_DUR_MODAL);
        qz_anim_ease_out(&scrim);
        lv_anim_start(&scrim);
    }
    return panel;
}

static void modal_actions(lv_obj_t *panel, const char *cancel_caption,
                          const char *confirm_caption, lv_event_cb_t confirm_cb,
                          void *user_data)
{
    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_set_size(row, lv_pct(100), 38);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cancel = qz_button(row, cancel_caption, 100, 38);
    lv_obj_add_event_cb(cancel, close_modal, LV_EVENT_CLICKED, NULL);

    lv_obj_t *confirm = lv_button_create(row);
    lv_obj_set_size(confirm, 104, 38);
    qz_touch_target(confirm);
    qz_style_primary_button(confirm);
    lv_obj_t *caption = qz_text(confirm, confirm_caption, 14, qz_color(QZ_TEXT_ON_ACCENT));
    lv_obj_center(caption);
    if (confirm_cb) lv_obj_add_event_cb(confirm, confirm_cb, LV_EVENT_CLICKED, user_data);
    else lv_obj_add_event_cb(confirm, close_modal, LV_EVENT_CLICKED, NULL);
}

/* ------------------------------------------------------------------------- *
 * WLAN page
 * ------------------------------------------------------------------------- */

/* Nothing here is hard coded: the list is whatever the supplicant reports for
 * the wireless interface right now, and the connected network comes from the
 * same source. */
#define WLAN_MAX_APS 12

static qz_wifi_ap_t access_points[WLAN_MAX_APS];
static int access_point_count;
static bool wlan_scanning;
static bool wlan_nic_seen;      /* 无线网卡是否已经出现过（用来在它"迟到"时补一次扫描） */
static bool wlan_browse;        /* 已连接时用户点了"搜索其他 WiFi"，暂离开已连接视图 */
static qz_wifi_state_t wlan_state = QZ_WIFI_OFF;
static char wlan_error[96];
static char wlan_signature[512];
static lv_timer_t *wlan_poll_timer;

static lv_obj_t *signal_glyph(lv_obj_t *parent, int level)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, 24, 18);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *bar = lv_obj_create(box);
        lv_obj_set_size(bar, 3, 5 + i * 4);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_LEFT, i * 6, 0);
        lv_obj_set_style_radius(bar, 2, 0);
        lv_obj_set_style_bg_color(bar, qz_color(i < level ? QZ_ACCENT : QZ_SEPARATOR), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    }
    return box;
}

static lv_obj_t *lock_glyph(lv_obj_t *parent, lv_color_t color)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, 12, 16);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *shackle = qz_arc_piece(box, 9, 10, 180, 360, 2, color);
    lv_obj_align(shackle, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *body = lv_obj_create(box);
    lv_obj_set_size(body, 12, 9);
    lv_obj_align(body, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(body, 3, 0);
    lv_obj_set_style_bg_color(body, color, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);
    return box;
}

static void refresh_wlan_summary(void)
{
    /* The checkmark and this label both ask the supplicant, so they can never
     * disagree with each other or with the real association state. */
    char connected[64];
    char ipv4[32];
    char page_text[96];
    char row_text[96];
    qz_wifi_current(connected, sizeof(connected));
    qz_wifi_ipv4(ipv4, sizeof(ipv4));
    qz_wifi_status_t status;
    qz_wifi_status(&status);
    wlan_state = status.state;
    snprintf(wlan_error, sizeof(wlan_error), "%.95s", status.error);

    /* wlan0 can remain UP with an old lease after disassociation. Only carrier,
     * a valid address and a default route together are evidence of a live link. */
    bool live_connected = qz_wifi_link_up();
    if (live_connected) {
        if (connected[0] == '\0') qz_wifi_current(connected, sizeof(connected));
        if (ipv4[0] == '\0') qz_wifi_ipv4(ipv4, sizeof(ipv4));
        wlan_state = QZ_WIFI_CONNECTED;
        wlan_error[0] = '\0';
        wlan_enabled = true;
        if (wlan_toggle) lv_obj_add_state(wlan_toggle, LV_STATE_CHECKED);
    }

    qz_color_token_t state_color = QZ_TEXT_SECONDARY;
    if (!live_connected && ((!wlan_enabled) || wlan_state == QZ_WIFI_OFF)) {
        snprintf(page_text, sizeof(page_text), "Wi-Fi 已关闭");
        snprintf(row_text, sizeof(row_text), "Wi-Fi 已关闭");
        state_color = QZ_TEXT_TERTIARY;
    } else if (wlan_state == QZ_WIFI_CONNECTING) {
        snprintf(page_text, sizeof(page_text), "正在连接 Wi-Fi…");
        snprintf(row_text, sizeof(row_text), "正在连接%s%s",
                 status.ssid[0] ? "  " : "", status.ssid);
    } else if (wlan_state == QZ_WIFI_OBTAINING_IP) {
        snprintf(page_text, sizeof(page_text), "已连接，正在获取 IP 地址…");
        snprintf(row_text, sizeof(row_text), "正在获取 IP 地址");
    } else if (wlan_state == QZ_WIFI_FAILED) {
        snprintf(page_text, sizeof(page_text), "%s",
                 status.error[0] ? status.error : "WiFi 连接失败，请检查密码");
        snprintf(row_text, sizeof(row_text), "连接失败");
    } else if (!qz_wifi_ready()) {
        snprintf(page_text, sizeof(page_text), "%s", qz_wifi_nic_text());
        snprintf(row_text, sizeof(row_text), "未检测到无线网卡");
    } else if (live_connected) {
        const char *ssid = connected[0] ? connected : status.ssid;
        if (ipv4[0] != '\0' && !qz_compact()) {
            if (ssid[0]) {
                snprintf(page_text, sizeof(page_text), "Wi-Fi 已连接：%s · %s", ssid, ipv4);
            } else {
                snprintf(page_text, sizeof(page_text), "Wi-Fi 已连接：%s", ipv4);
            }
        } else {
            snprintf(page_text, sizeof(page_text), "Wi-Fi 已连接");
        }
        snprintf(row_text, sizeof(row_text), "%s", ssid[0] ? ssid : ipv4);
    } else {
        snprintf(page_text, sizeof(page_text), "请选择 Wi-Fi 网络");
        snprintf(row_text, sizeof(row_text), "未连接");
    }

    if (wlan_page_summary) {
        lv_label_set_text(wlan_page_summary, page_text);
        qz_obj_set_text_color(wlan_page_summary, state_color, 0);
    }
    if (wlan_row_summary) {
        lv_label_set_text(wlan_row_summary, row_text);
        qz_obj_set_text_color(wlan_row_summary, state_color, 0);
    }
    if (wlan_list) {
        lv_obj_set_style_opa(wlan_list, wlan_enabled ? LV_OPA_COVER : (lv_opa_t)110, 0);
    }
}

static void refresh_settings_clock(void)
{
    char clock_text[16] = "--:--";
    time_t now = time(NULL);
    struct tm local_time;
    if (now != (time_t)-1 && localtime_r(&now, &local_time)) {
        strftime(clock_text, sizeof(clock_text), "%H:%M", &local_time);
    }

    if (time_value) lv_label_set_text(time_value, clock_text);
    if (settings_rail_clock) {
        char rail_text[24];
        snprintf(rail_text, sizeof(rail_text), "在线  %s", clock_text);
        lv_label_set_text(settings_rail_clock, rail_text);
    }
}

static void settings_clock_tick(lv_timer_t *timer)
{
    (void)timer;
    refresh_settings_clock();
}

static void rebuild_wlan_list(void)
{
    if (!wlan_list) return;
    lv_obj_clean(wlan_list);

    char connected[64];
    char ipv4[32];
    qz_wifi_current(connected, sizeof(connected));
    qz_wifi_ipv4(ipv4, sizeof(ipv4));
    bool live_connected = qz_wifi_link_up();
    if (!live_connected) wlan_browse = false;   /* 断开了就回到正常列表 */

    if (live_connected && !wlan_browse) {
        /* Echo-Mate 的已连接视图：列表换成「当前网络 + 搜索其他 WiFi」两行。
         * It must not depend on scan results: INACTIVE/stale scans can leave the
         * AP cache empty while the interface still has a usable DHCP lease. */
        char title[96];
        lv_obj_t *cur = lv_obj_create(wlan_list);
        lv_obj_set_size(cur, lv_pct(100), qz_compact() ? 72 : 76);
        qz_style_row(cur);
        lv_obj_add_flag(cur, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cur, wlan_current_clicked, LV_EVENT_CLICKED, NULL);
        snprintf(title, sizeof(title), "%s", connected[0] ? connected : ipv4);
        lv_obj_t *name = qz_text(cur, title, 13, qz_color(QZ_TEXT));
        lv_obj_set_width(name, CONTENT_W - 2 * CARD_PAD - 24);
        lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, CARD_PAD, 0);
        lv_obj_t *chev = qz_chevron(cur, qz_color(QZ_TEXT_TERTIARY), 14);
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -CARD_PAD - 2, 0);

        lv_obj_t *search = lv_obj_create(wlan_list);
        lv_obj_set_size(search, lv_pct(100), 40);
        qz_style_row(search);
        lv_obj_add_flag(search, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(search, wlan_browse_clicked, LV_EVENT_CLICKED, NULL);
        lv_obj_t *search_text = qz_text(search, "搜索其他 WiFi", 13, qz_color(QZ_ACCENT));
        lv_obj_align(search_text, LV_ALIGN_LEFT_MID, CARD_PAD, 0);
        return;
    }

    if (access_point_count == 0) {
        /* Echo-Mate 的做法：空状态把"为什么没有列表"说清楚，而不是一句含糊的空话 */
        const char *text = !qz_wifi_ready() ? qz_wifi_nic_text()
                           : wlan_scanning  ? "正在扫描 WiFi..."
                           : qz_wifi_scan_note()[0] ? qz_wifi_scan_note()
                                                    : "未发现可用网络";
        /* The list is a flex column, which ignores lv_obj_center on its direct
         * children, so the message lives in a plain full size box. */
        lv_obj_t *box = lv_obj_create(wlan_list);
        lv_obj_set_size(box, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_pad_all(box, 0, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *empty = qz_text(box, text, 12, qz_color(QZ_TEXT_TERTIARY));
        lv_obj_set_width(empty, CONTENT_W - 2 * CARD_PAD);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(empty);
        return;
    }

    for (int i = 0; i < access_point_count; i++) {
        const qz_wifi_ap_t *ap = &access_points[i];
        lv_obj_t *row = lv_obj_create(wlan_list);
        lv_point_t name_size;
        lv_text_get_size(&name_size, ap->ssid, qz_font_size(13), 0, 0,
                         qz_scale_x(CONTENT_W - 2 * CARD_PAD - 32 - 54),
                         LV_TEXT_FLAG_NONE);
        const int minimum_height = qz_compact() ? 56 : 48;
        const int panel_height = qz_scale_y(QZ_DESIGN_H);
        const int text_height = (name_size.y * QZ_DESIGN_H + panel_height - 1) / panel_height;
        const int row_height = text_height + 16 > minimum_height
                             ? text_height + 16 : minimum_height;
        lv_obj_set_size(row, lv_pct(100), row_height);
        qz_style_row(row);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        qz_obj_set_border_color(row, QZ_SEPARATOR, 0);
        lv_obj_set_style_border_opa(row, (lv_opa_t)140, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, wlan_network_clicked, LV_EVENT_CLICKED,
                            (void *)ap->ssid);

        lv_obj_t *signal = signal_glyph(row, ap->bars);
        lv_obj_align(signal, LV_ALIGN_LEFT_MID, CARD_PAD, 0);
        lv_obj_t *name = qz_text(row, ap->ssid, 13, qz_color(QZ_TEXT));
        lv_obj_set_width(name, CONTENT_W - CARD_PAD - 32 - CARD_PAD - 54);
        lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, CARD_PAD + 32, 0);
        if (ap->locked) {
            lv_obj_t *lock = lock_glyph(row, qz_color(QZ_TEXT_TERTIARY));
            lv_obj_align(lock, LV_ALIGN_RIGHT_MID, -CARD_PAD - 26, 0);
        }
        if (connected[0] != '\0' && strcmp(connected, ap->ssid) == 0) {
            lv_obj_t *check = qz_symbol(row, LV_SYMBOL_OK, 14, qz_color(QZ_ACCENT));
            lv_obj_align(check, LV_ALIGN_RIGHT_MID, -CARD_PAD - 2, 0);
        } else {
            lv_obj_t *chev = qz_chevron(row, qz_color(QZ_TEXT_TERTIARY), 14);
            lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -CARD_PAD - 2, 0);
        }
    }
    lv_obj_update_layout(wlan_list);
}

static void wlan_rebuild_async(void *unused)
{
    (void)unused;
    rebuild_wlan_list();
}

static void wlan_current_clicked(lv_event_t *event)
{
    open_details_modal(lv_obj_get_screen((lv_obj_t *)lv_event_get_target(event)));
}

static void wlan_browse_clicked(lv_event_t *event)
{
    (void)event;
    wlan_browse = true;
    wlan_signature[0] = '\0';
    wlan_scanning = true;
    access_point_count = 0;
    qz_wifi_scan_async();
    /* 不能在点击回调里直接清列表（正点着的行就在里面），丢到下一帧再重建 */
    lv_async_call(wlan_rebuild_async, NULL);
}

static void wlan_disconnect_clicked(lv_event_t *event)
{
    close_modal(event);
    qz_wifi_disconnect();
    wlan_signature[0] = '\0';
    refresh_wlan_summary();
}

static void wlan_forget_clicked(lv_event_t *event)
{
    close_modal(event);
    qz_wifi_forget();
    wlan_signature[0] = '\0';
    access_point_count = 0;
    wlan_scanning = true;
    qz_wifi_scan_async();
    lv_async_call(wlan_rebuild_async, NULL);
    refresh_wlan_summary();
}

/** 已连接网络的详情：SSID / IP / MAC + 断开（橙）+ 取消保存（红），照 Echo-Mate。 */
static void open_details_modal(lv_obj_t *screen)
{
    char ssid[64];
    char ipv4[32];
    char mac[32];
    char line[96];

    qz_wifi_current(ssid, sizeof(ssid));
    qz_wifi_ipv4(ipv4, sizeof(ipv4));
    qz_wifi_mac(mac, sizeof(mac));

    lv_obj_t *panel = open_modal(screen, 182);
    lv_obj_t *title = qz_text(panel, ssid[0] ? ssid : "当前网络", 14, qz_color(QZ_TEXT));
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    snprintf(line, sizeof(line), "IP 地址：%s", ipv4[0] ? ipv4 : "获取中…");
    lv_obj_t *ip = qz_text(panel, line, 11, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(ip, LV_ALIGN_TOP_LEFT, 0, 60);

    snprintf(line, sizeof(line), "MAC：%s", mac[0] ? mac : "--");
    lv_obj_t *mac_label = qz_text(panel, line, 11, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(mac_label, LV_ALIGN_TOP_LEFT, 0, 82);

    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_set_size(row, lv_pct(100), 40);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *disconnect = lv_button_create(row);
    lv_obj_set_size(disconnect, 112, 38);
    qz_touch_target(disconnect);
    lv_obj_set_style_bg_color(disconnect, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_add_event_cb(disconnect, wlan_disconnect_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *disconnect_label = qz_text(disconnect, "断开连接", 16, qz_color(QZ_TEXT_ON_ACCENT));
    lv_obj_center(disconnect_label);

    lv_obj_t *forget = lv_button_create(row);
    lv_obj_set_size(forget, 112, 38);
    qz_touch_target(forget);
    lv_obj_set_style_bg_color(forget, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_add_event_cb(forget, wlan_forget_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *forget_label = qz_text(forget, "取消保存", 16, qz_color(QZ_TEXT_ON_ACCENT));
    lv_obj_center(forget_label);
}

/** Keep the selected page while polling unchanged scan results. */
static void wlan_refresh_list(void)
{
    qz_wifi_ap_t found[WLAN_MAX_APS];
    char signature[512];
    char connected[64];
    char iface[32];
    int count;
    size_t used = 0;

    qz_wifi_interface(iface, sizeof(iface));
    if (iface[0] == '\0') {
        wlan_nic_seen = false;
        access_point_count = 0;
        rebuild_wlan_list();
        refresh_wlan_summary();
        return;
    }

    /* 扫描由页面加载/重新启用或“搜索其他 WiFi”明确发起。这里只读缓存，
     * 否则 2.5 秒轮询会在用户看列表时反复重启无线链路。 */
    wlan_nic_seen = true;

    count = qz_wifi_scan_results(found, WLAN_MAX_APS);
    qz_wifi_current(connected, sizeof(connected));
    signature[0] = '\0';
    for (int i = 0; i < count && used + 80 < sizeof(signature); i++) {
        used += (size_t)snprintf(signature + used, sizeof(signature) - used,
                                 "%s|%d|%d;", found[i].ssid, found[i].bars,
                                 found[i].locked ? 1 : 0);
    }
    used += (size_t)snprintf(signature + used, sizeof(signature) - used, "@%s",
                             connected);

    bool same = strcmp(signature, wlan_signature) == 0;
    if (!same) {
        // NOLINTNEXTLINE(bugprone-undefined-memory-manipulation)
        memcpy(access_points, found, sizeof(found[0]) * (size_t)count);
        access_point_count = count;
        snprintf(wlan_signature, sizeof(wlan_signature), "%s", signature);
        wlan_scanning = qz_wifi_scan_pending();
        rebuild_wlan_list();
    }
    if (wlan_scanning && !qz_wifi_scan_pending()) wlan_scanning = false;
    refresh_wlan_summary();
}

static void wlan_poll(lv_timer_t *timer)
{
    (void)timer;
    if (!wlan_enabled) return;
    if (lv_screen_active() != wlan_screen) return;

    /* 先读 worker 的共享状态：qz_wifi_status() 只锁一下内存，不执行任何外部命令。 */
    qz_wifi_status_t worker;
    if (qz_wifi_status(&worker)) wlan_state = worker.state;

    /* A saved network is reconnected before scanning. Only fall back to a scan
     * when the worker reports that there is nothing saved (IDLE) or the saved
     * connection failed. Scanning an associated RTL8723BS can itself drop the
     * carrier, so never do it while the full link is healthy. */
    bool live_connected = qz_wifi_link_up();
    if (live_connected) {
        wlan_auto_scan = false;
    } else if (wlan_auto_scan &&
               (wlan_state == QZ_WIFI_IDLE || wlan_state == QZ_WIFI_FAILED)) {
        wlan_auto_scan = false;
        wlan_scanning = true;
        wlan_signature[0] = '\0';
        qz_wifi_scan_async();
    }

    /* 连接期间不做探测：refresh_wlan_summary()/wlan_refresh_list() 内部会跑
     * wpa_cli / wpa_supplicant 查询，而四次握手时 ctrl socket 正忙，命令会阻塞
     * 界面线程好几秒 —— 那就是"点连接之后界面卡住"的原因。等 worker 报
     * CONNECTED/FAILED 再恢复完整刷新即可。 */
    if (wlan_state == QZ_WIFI_CONNECTING) {
        char text[96];
        const char *ssid_text = worker.ssid[0] ? worker.ssid : pending_ssid;
        if (qz_compact()) snprintf(text, sizeof(text), "正在连接 WiFi…");
        else snprintf(text, sizeof(text), "正在连接 %s…", ssid_text[0] ? ssid_text : "网络");
        if (wlan_page_summary) lv_label_set_text(wlan_page_summary, text);
        if (wlan_row_summary) lv_label_set_text(wlan_row_summary, text);
        return;
    }

    refresh_wlan_summary();
    if (wlan_state == QZ_WIFI_FAILED) wlan_scanning = false;
    wlan_refresh_list();
}

static void wlan_screen_loaded(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_SCREEN_LOADED) return;
    /* Reading the panel state is cheap, so the sliders always show the truth. */
    slider_sync(&volume_binding, qz_volume_level());
    slider_sync(&backlight_binding, qz_backlight_level());

    access_point_count = 0;
    wlan_scanning = false;
    wlan_signature[0] = '\0';
    wlan_nic_seen = true;
    rebuild_wlan_list();
    refresh_wlan_summary();
    if (!wlan_enabled) return;
    qz_wifi_worker_start();

    /* Opening the page must not disturb a working link. A scan is only needed
     * while browsing; reconnecting first also keeps disable -> enable from
     * leaving an old IP behind a dead carrier. */
    if (qz_wifi_link_up()) {
        wlan_auto_scan = false;
        wlan_state = QZ_WIFI_CONNECTED;
        wlan_refresh_list();
        return;
    }
    wlan_auto_scan = true;
    wlan_state = QZ_WIFI_CONNECTING;
    qz_wifi_autoconnect_async();
    refresh_wlan_summary();
}

static void wlan_toggled(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    wlan_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (wlan_enabled) {
        /* Reconnect the saved network first. A direct scan can disassociate an
         * RTL8723BS link, which made the switch look "on" while the UI showed
         * the stale lease as offline. */
        wlan_auto_scan = true;
        wlan_state = QZ_WIFI_CONNECTING;
        wlan_scanning = false;
        wlan_signature[0] = '\0';
        access_point_count = 0;
        qz_wifi_autoconnect_async();
        rebuild_wlan_list();
    } else {
        wlan_auto_scan = false;
        wlan_scanning = false;
        wlan_state = QZ_WIFI_OFF;
        qz_wifi_enable_async(false);
        access_point_count = 0;
        wlan_signature[0] = '\0';
        rebuild_wlan_list();
    }
    refresh_wlan_summary();
}

static void password_connect(lv_event_t *event)
{
    lv_obj_t *input = (lv_obj_t *)lv_event_get_user_data(event);
    if (input) {
        qz_wifi_connect_async(pending_ssid, lv_textarea_get_text(input));
        wlan_state = QZ_WIFI_CONNECTING;
        wlan_browse = false;   /* 连上之后回到已连接视图 */
        wlan_signature[0] = '\0';
        /* 先给个即时反馈：轮询在连接期间不会去探测（会阻塞），别让用户以为没反应 */
        if (wlan_page_summary) {
            char connect_text[96];
            if (qz_compact()) snprintf(connect_text, sizeof(connect_text), "正在连接 WiFi…");
            else snprintf(connect_text, sizeof(connect_text),
                          "正在连接 %s…", pending_ssid[0] ? pending_ssid : "网络");
            lv_label_set_text(wlan_page_summary, connect_text);
        }
    }
    close_modal(event);
}

/** Eye button inside the password field: reveals / hides the typed password. */
static void toggle_password_visibility(lv_event_t *event)
{
    lv_obj_t *eye = lv_event_get_current_target(event);
    lv_obj_t *input = (lv_obj_t *)lv_obj_get_user_data(eye);
    if (!input) return;

    bool hidden = lv_textarea_get_password_mode(input);
    lv_textarea_set_password_mode(input, !hidden);

    /* qz_symbol may create an image rather than a label. Rebuild the glyph so
     * password visibility works with either icon implementation. */
    lv_obj_clean(eye);
    lv_obj_t *glyph = qz_symbol(eye, hidden ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE,
                                18, qz_color(hidden ? QZ_ACCENT : QZ_TEXT_TERTIARY));
    lv_obj_center(glyph);
}

static void open_password_modal(lv_obj_t *screen, const char *ssid)
{
    /* Only remember which network the sheet is asking about: the connection
     * state itself must not change until the user confirms, otherwise a cancel
     * would leave the summary text and the list checkmark out of sync. */
    snprintf(pending_ssid, sizeof(pending_ssid), "%.63s", ssid);

    /* Reserve three context lines for a long SSID, then the input and actions. */
    const int keyboard_height = qz_compact() ? 128 : MODAL_KEYBOARD_H;
    lv_obj_t *panel = open_modal(screen, qz_compact() ? 186 : 176);
    lv_obj_t *title = qz_text(panel, "输入无线网络密码", 14, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    char subtitle[96];
    if (qz_compact()) snprintf(subtitle, sizeof(subtitle), "%s", ssid);
    else snprintf(subtitle, sizeof(subtitle), "「%s」需要密码才能加入", ssid);
    lv_obj_t *hint = qz_text(panel, subtitle, 11, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 22);

    lv_obj_t *input = lv_textarea_create(panel);
    lv_obj_set_size(input, lv_pct(100), 40);
    lv_obj_align(input, LV_ALIGN_TOP_LEFT, 0, qz_compact() ? 84 : 68);
    lv_textarea_set_one_line(input, true);
    lv_textarea_set_password_mode(input, true);
    lv_textarea_set_max_length(input, 63);
    lv_textarea_set_placeholder_text(input, "密码");
    qz_style_textarea(input);
    /* one_line resets the textarea height to content; restore the touch area
     * after applying it and leave vertical space around the glyphs. */
    lv_obj_set_height(input, 40);
    lv_obj_set_style_pad_top(input, 8, 0);
    lv_obj_set_style_pad_bottom(input, 8, 0);
    /* Keep the text clear of the eye button on the right. */
    lv_obj_set_style_pad_right(input, 52, 0);

    lv_obj_t *eye = qz_icon_button(panel, LV_SYMBOL_EYE_CLOSE, 40);
    lv_obj_align(eye, LV_ALIGN_TOP_RIGHT, -6, qz_compact() ? 84 : 68);
    lv_obj_set_style_bg_opa(eye, LV_OPA_TRANSP, 0);
    qz_obj_set_bg_color(eye, QZ_FILL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(eye, (lv_opa_t)120, LV_STATE_PRESSED);
    lv_obj_set_user_data(eye, input);
    lv_obj_add_event_cb(eye, toggle_password_visibility, LV_EVENT_CLICKED, NULL);

    lv_obj_t *keyboard = lv_keyboard_create(modal_overlay);
    lv_obj_set_size(keyboard, lv_pct(100), keyboard_height);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard, input);
    qz_style_keyboard(keyboard);
    lv_obj_add_event_cb(keyboard, password_connect, LV_EVENT_READY, input);
    lv_obj_add_event_cb(keyboard, close_modal, LV_EVENT_CANCEL, NULL);
    if (qz_compact()) {
        lv_obj_set_style_pad_all(keyboard, 2, 0);
        lv_obj_set_style_pad_row(keyboard, 2, 0);
        lv_obj_set_style_pad_column(keyboard, 3, 0);
    }

    /* Sit the sheet on top of the keyboard instead of at a fixed offset: the
     * actions can then never end up underneath the keys, whatever the panel
     * height or the keyboard height are. */
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -(keyboard_height + 4));

    modal_actions(panel, "取消", "连接", password_connect, input);
}

static void wlan_network_clicked(lv_event_t *event)
{
    if (!wlan_enabled) return;
    const char *ssid = (const char *)lv_event_get_user_data(event);
    if (!ssid) return;

    char connected[64];
    qz_wifi_current(connected, sizeof(connected));
    if (strcmp(connected, ssid) == 0) return;   /* already on this network */

    for (int i = 0; i < access_point_count; i++) {
        if (strcmp(access_points[i].ssid, ssid) != 0) continue;
        if (access_points[i].locked) {
            if (qz_wifi_is_saved_network(ssid) && qz_wifi_reconnect_async(ssid)) {
                wlan_state = QZ_WIFI_CONNECTING;
                wlan_scanning = false;
                wlan_signature[0] = '\0';
                if (wlan_page_summary) {
                    char text[96];
                    snprintf(text, sizeof(text), "正在重新连接 %s…", ssid);
                    lv_label_set_text(wlan_page_summary, text);
                }
            } else {
                open_password_modal(wlan_screen, ssid);
            }
        } else if (qz_wifi_connect_async(ssid, "")) {
            wlan_state = QZ_WIFI_CONNECTING;
            wlan_scanning = false;
            wlan_signature[0] = '\0';   /* force the next poll to refresh */
        }
        return;
    }
}

static void build_wlan_screen(void)
{
    wlan_screen = lv_obj_create(NULL);
    qz_style_screen(wlan_screen);
    page_toolbar(wlan_screen, "WLAN", settings_screen);

    /* Current link, radio control, and discovered networks are one continuous
     * instrument. The list scrolls inside it instead of becoming a second card. */
    lv_obj_t *card = section_card(wlan_screen, 60,
                                  QZ_DESIGN_H - 60 - QZ_GUTTER, false);
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_t *label = qz_text(card, "WLAN", 14, qz_color(QZ_TEXT));
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, CARD_PAD, 10);
    wlan_page_summary = qz_text(card, "已连接：Home_5G", 11, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(wlan_page_summary, LV_ALIGN_TOP_LEFT, CARD_PAD, 34);
    lv_obj_set_width(wlan_page_summary, CONTENT_W - 2 * CARD_PAD - 84);
    lv_label_set_long_mode(wlan_page_summary, LV_LABEL_LONG_WRAP);

    wlan_toggle = lv_switch_create(card);
    lv_obj_set_size(wlan_toggle, qz_compact() ? 54 : 46, 28);
    qz_touch_target(wlan_toggle);
    lv_obj_align(wlan_toggle, LV_ALIGN_TOP_RIGHT, -CARD_PAD, 18);
    qz_style_switch(wlan_toggle);
    lv_obj_add_state(wlan_toggle, LV_STATE_CHECKED);
    lv_obj_add_event_cb(wlan_toggle, wlan_toggled, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *rule = qz_separator(card, CONTENT_W, false);
    lv_obj_align(rule, LV_ALIGN_TOP_LEFT, 0, 64);
    lv_obj_t *header = qz_text(card, "可用网络", 10, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, CARD_PAD, 73);
    lv_obj_t *hint = qz_text(card, "点选后连接", 10, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(hint, LV_ALIGN_TOP_RIGHT, -CARD_PAD, 73);

    wlan_list = lv_obj_create(card);
    lv_obj_set_size(wlan_list, lv_pct(100), QZ_DESIGN_H - 60 - QZ_GUTTER - 96);
    lv_obj_align(wlan_list, LV_ALIGN_TOP_LEFT, 0, 96);
    lv_obj_set_style_bg_opa(wlan_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wlan_list, 0, 0);
    lv_obj_set_style_pad_all(wlan_list, 0, 0);
    lv_obj_set_flex_flow(wlan_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(wlan_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(wlan_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(wlan_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_row(wlan_list, 0, 0);
    lv_obj_set_flex_align(wlan_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_add_event_cb(wlan_screen, wlan_screen_loaded, LV_EVENT_ALL, NULL);
    /* The supplicant only scans on request and the UI must not block on it, so
     * results are polled while the page is on screen. */
    wlan_poll_timer = lv_timer_create(wlan_poll, 2500, NULL);
    rebuild_wlan_list();
    refresh_wlan_summary();
}

/* ------------------------------------------------------------------------- *
 * General settings page
 * ------------------------------------------------------------------------- */

static void slider_changed(lv_event_t *event)
{
    slider_binding_t *binding = (slider_binding_t *)lv_event_get_user_data(event);
    lv_obj_t *slider = lv_event_get_target(event);
    int value = (int)lv_slider_get_value(slider);
    if (!binding) return;
    if (binding->label) {
        char text[16];
        snprintf(text, sizeof(text), "%d%%", value);
        lv_label_set_text(binding->label, text);
    }
    if (binding->apply &&
        (binding == &material_binding || lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED)) {
        bool applied = binding->apply(value);
        if (binding->label && !applied) lv_label_set_text(binding->label, "失败");
        else if (binding->label && binding != &material_binding)
            lv_label_set_text(binding->label, "本次生效");
    }
}

/** Re-read the hardware and move the slider without firing the hook again. */
static void slider_sync(slider_binding_t *binding, int value)
{
    if (!binding || !binding->slider || value < 0) return;
    lv_slider_set_value(binding->slider, value, LV_ANIM_OFF);
    if (binding->label) {
        char text[16];
        snprintf(text, sizeof(text), "%d%%", value);
        lv_label_set_text(binding->label, text);
    }
}

static void slider_row(lv_obj_t *card, int y, qz_icon_t icon, int icon_px,
                       const char *caption, int initial,
                       slider_binding_t *binding)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_set_size(row, lv_pct(100), 64);
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *glyph = qz_icon_image(row, icon, icon_px, qz_color(QZ_ACCENT_DARK));
    lv_obj_align(glyph, LV_ALIGN_TOP_LEFT,
                 icon_px > 20 ? CARD_PAD - 2 : CARD_PAD,
                 icon_px > 20 ? 6 : 11);
    lv_obj_t *title = qz_text(row, caption, 14, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, CARD_PAD + 30, 8);

    lv_obj_t *value = qz_text(row, "0%", 12, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(value, LV_ALIGN_TOP_RIGHT, -CARD_PAD, 9);

    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_size(slider, CONTENT_W - 2 * CARD_PAD, 26);
    lv_obj_align(slider, LV_ALIGN_TOP_LEFT, CARD_PAD, 31);
    lv_slider_set_range(slider, 0, 100);
    qz_style_slider(slider);
    if (binding) {
        binding->label = value;
        binding->slider = slider;
        lv_obj_add_event_cb(slider, slider_changed, LV_EVENT_VALUE_CHANGED, binding);
        lv_obj_add_event_cb(slider, slider_changed, LV_EVENT_RELEASED, binding);
        lv_obj_add_event_cb(slider, slider_changed, LV_EVENT_PRESS_LOST, binding);
    }
    lv_slider_set_value(slider, initial, LV_ANIM_OFF);

    char text[8];
    snprintf(text, sizeof(text), "%d%%", initial);
    lv_label_set_text(value, text);
}

static void build_number_options(char *buffer, size_t size, int count, int digits)
{
    size_t used = 0;
    for (int i = 0; i < count; i++) {
        if (i > 0 && used + 1 < size) buffer[used++] = '\n';
        int written = snprintf(buffer + used, size - used, digits == 2 ? "%02d" : "%d", i);
        if (written < 0 || (size_t)written >= size - used) break;
        used += (size_t)written;
    }
}

static void style_roller(lv_obj_t *roller)
{
    lv_obj_set_style_bg_opa(roller, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(roller, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(roller, 0, LV_PART_MAIN);
    qz_obj_set_text_color(roller, QZ_TEXT_TERTIARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(roller, qz_font_size(15), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(roller, 14, LV_PART_MAIN);
    qz_obj_set_bg_color(roller, QZ_FILL, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_radius(roller, 3, LV_PART_SELECTED);
    qz_obj_set_text_color(roller, QZ_TEXT, LV_PART_SELECTED);
    lv_obj_set_style_text_font(roller, qz_font_size(17), LV_PART_SELECTED);
    /* Row count derives the widget height from the font and line spacing.
     * Calling it before those styles are applied leaves the wheel content
     * taller than its box, which lets adjacent rows paint over the copy below. */
    lv_roller_set_visible_row_count(roller, 3);
}

/* The two rollers are remembered by name: looking them up by child index broke
 * silently as soon as another widget (the ":" label) was added to the panel. */
static lv_obj_t *time_hour_roller;
static lv_obj_t *time_minute_roller;

static void time_confirm(lv_event_t *event)
{
    (void)event;
    if (time_hour_roller && time_minute_roller && time_value) {
        char text[16];
        snprintf(text, sizeof(text), "%02d:%02d",
                 (int)lv_roller_get_selected(time_hour_roller),
                 (int)lv_roller_get_selected(time_minute_roller));
        lv_label_set_text(time_value, text);
    }
    close_modal(NULL);
}

static void open_time_modal(lv_event_t *event)
{
    lv_obj_t *screen = (lv_obj_t *)lv_event_get_user_data(event);
    lv_obj_t *panel = open_modal(screen, 206);
    lv_obj_t *title = qz_text(panel, "选择时间", 14, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    static char hours[256];
    static char minutes[256];
    build_number_options(hours, sizeof(hours), 24, 2);
    build_number_options(minutes, sizeof(minutes), 60, 2);

    int hour = 10;
    int minute = 30;
    if (time_value) sscanf(lv_label_get_text(time_value), "%d:%d", &hour, &minute);

    lv_obj_t *colon = qz_text(panel, ":", 17, qz_color(QZ_TEXT));
    lv_obj_align(colon, LV_ALIGN_TOP_MID, 0, 62);

    time_hour_roller = lv_roller_create(panel);
    lv_roller_set_options(time_hour_roller, hours, LV_ROLLER_MODE_NORMAL);
    lv_obj_set_size(time_hour_roller, 92, 104);
    lv_obj_align(time_hour_roller, LV_ALIGN_TOP_MID, -56, 24);
    style_roller(time_hour_roller);
    lv_roller_set_selected(time_hour_roller, (uint32_t)hour, LV_ANIM_OFF);

    time_minute_roller = lv_roller_create(panel);
    lv_roller_set_options(time_minute_roller, minutes, LV_ROLLER_MODE_NORMAL);
    lv_obj_set_size(time_minute_roller, 92, 104);
    lv_obj_align(time_minute_roller, LV_ALIGN_TOP_MID, 56, 24);
    style_roller(time_minute_roller);
    lv_roller_set_selected(time_minute_roller, (uint32_t)minute, LV_ANIM_OFF);

    modal_actions(panel, "取消", "确定", time_confirm, NULL);
}

/** The POSIX TZ string is what actually drives the clock (the SDK uses
 *  "export TZ=CST-8"); the label is only what the user sees. */
typedef struct {
    const char *label;
    const char *tz;
} timezone_entry_t;

static const timezone_entry_t timezones[] = {
    { "(GMT-8) 洛杉矶", "PST8" },
    { "(GMT-5) 纽约", "EST5" },
    { "(GMT+0) 伦敦", "GMT0" },
    { "(GMT+1) 柏林", "CET-1" },
    { "(GMT+5) 塔什干", "UZT-5" },
    { "(GMT+7) 曼谷", "ICT-7" },
    { "(GMT+8) 北京", "CST-8" },
    { "(GMT+9) 东京", "JST-9" },
    { "(GMT+10) 悉尼", "AEST-10" },
};
#define TIMEZONE_PAGE_ROWS 3
#define TIMEZONE_COUNT ((int)(sizeof(timezones) / sizeof(timezones[0])))
static lv_obj_t *timezone_list;
static lv_obj_t *timezone_previous;
static lv_obj_t *timezone_next;
static lv_obj_t *timezone_page_value;
static int timezone_page;

static bool timezone_waiting;
static bool timezone_saved;

static void timezone_ack(const qzdesk_core_event_t *event, void *data)
{
    (void)data;
    if (!timezone_waiting || event->type != QZDESK_CORE_EVENT_ACK ||
        strcmp(event->command, "timezone") != 0) return;
    timezone_waiting = false;
    if (timezone_value) lv_label_set_text(timezone_value,
        !event->ok ? "核心同步失败" : timezone_saved ? "已保存·已同步" : "已同步·本次生效");
}

static void timezone_confirm(lv_event_t *event)
{
    const timezone_entry_t *zone = (const timezone_entry_t *)lv_event_get_user_data(event);
    if (zone) {
        bool saved = qz_timezone_apply(zone->tz);
        bool sent = qzdesk_core_set_timezone(zone->tz);
        timezone_waiting = sent;
        timezone_saved = saved;
        if (timezone_value) lv_label_set_text(timezone_value,
            !saved ? "本次生效·未保存" : !sent ? "已保存·核心未同步" : "已保存·等待核心确认");
        if (saved && sent) close_modal(event);
    }
}

static void timezone_rebuild(void *unused)
{
    (void)unused;
    if (!timezone_list) return;
    const int pages = (TIMEZONE_COUNT + TIMEZONE_PAGE_ROWS - 1) / TIMEZONE_PAGE_ROWS;
    if (timezone_page < 0) timezone_page = 0;
    if (timezone_page >= pages) timezone_page = pages - 1;
    lv_obj_clean(timezone_list);
    const int first = timezone_page * TIMEZONE_PAGE_ROWS;
    int last = first + TIMEZONE_PAGE_ROWS;
    if (last > TIMEZONE_COUNT) last = TIMEZONE_COUNT;
    for (int i = first; i < last; i++) {
        lv_obj_t *row = lv_obj_create(timezone_list);
        lv_obj_set_size(row, lv_pct(100), 44);
        qz_style_row(row);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, timezone_confirm, LV_EVENT_CLICKED, (void *)&timezones[i]);
        lv_obj_t *label = qz_text(row, timezones[i].label, 16, qz_color(QZ_TEXT));
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);
        if (timezone_value &&
            strcmp(lv_label_get_text(timezone_value), timezones[i].label) == 0) {
            lv_obj_t *check = qz_symbol(row, LV_SYMBOL_OK, 14, qz_color(QZ_ACCENT));
            lv_obj_align(check, LV_ALIGN_RIGHT_MID, -12, 0);
        }
    }
    char page_text[32];
    snprintf(page_text, sizeof(page_text), "%d/%d", timezone_page + 1, pages);
    lv_label_set_text(timezone_page_value, page_text);
    if (timezone_page == 0) lv_obj_add_state(timezone_previous, LV_STATE_DISABLED);
    else lv_obj_remove_state(timezone_previous, LV_STATE_DISABLED);
    if (timezone_page + 1 == pages) lv_obj_add_state(timezone_next, LV_STATE_DISABLED);
    else lv_obj_remove_state(timezone_next, LV_STATE_DISABLED);
}

static void timezone_page_clicked(lv_event_t *event)
{
    timezone_page += (int)(intptr_t)lv_event_get_user_data(event);
    lv_async_call(timezone_rebuild, NULL);
}

static void timezone_list_deleted(lv_event_t *event)
{
    if (lv_event_get_target(event) == timezone_list) timezone_list = NULL;
}

static void open_timezone_modal(lv_event_t *event)
{
    lv_obj_t *screen = (lv_obj_t *)lv_event_get_user_data(event);
    lv_obj_t *panel = open_modal(screen, 286);
    lv_obj_t *title = qz_text(panel, "选择时区", 16, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    timezone_list = lv_obj_create(panel);
    lv_obj_set_size(timezone_list, lv_pct(100), 148);
    lv_obj_align(timezone_list, LV_ALIGN_TOP_LEFT, 0, 28);
    lv_obj_set_style_bg_opa(timezone_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(timezone_list, 0, 0);
    lv_obj_set_style_pad_all(timezone_list, 0, 0);
    lv_obj_set_style_pad_row(timezone_list, 2, 0);
    lv_obj_set_flex_flow(timezone_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(timezone_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(timezone_list, timezone_list_deleted, LV_EVENT_DELETE, NULL);

    lv_obj_t *actions = lv_obj_create(panel);
    lv_obj_set_size(actions, lv_pct(100), 88);
    lv_obj_align(actions, LV_ALIGN_TOP_LEFT, 0, 184);
    lv_obj_set_style_bg_opa(actions, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(actions, 0, 0);
    lv_obj_set_style_pad_all(actions, 0, 0);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *pager = lv_obj_create(actions);
    lv_obj_set_size(pager, lv_pct(100), 40);
    lv_obj_align(pager, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(pager, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pager, 0, 0);
    lv_obj_set_style_pad_all(pager, 0, 0);
    lv_obj_set_flex_flow(pager, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pager, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(pager, LV_OBJ_FLAG_SCROLLABLE);

    timezone_previous = qz_button(pager, "上一页", 108, 40);
    lv_obj_add_event_cb(timezone_previous, timezone_page_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    timezone_page_value = qz_text(pager, "1/3", 14, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_set_width(timezone_page_value, 64);
    lv_obj_set_style_text_align(timezone_page_value, LV_TEXT_ALIGN_CENTER, 0);
    timezone_next = qz_button(pager, "下一页", 108, 40);
    lv_obj_add_event_cb(timezone_next, timezone_page_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_t *cancel = qz_button(actions, "取消", 96, 40);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(cancel, close_modal, LV_EVENT_CLICKED, NULL);

    timezone_page = 0;
    for (int i = 0; i < TIMEZONE_COUNT; i++) {
        if (timezone_value && strcmp(lv_label_get_text(timezone_value), timezones[i].label) == 0)
            timezone_page = i / TIMEZONE_PAGE_ROWS;
    }
    timezone_rebuild(NULL);
}

static void build_general_screen(void)
{
    general_screen = lv_obj_create(NULL);
    qz_style_screen(general_screen);
    page_toolbar(general_screen, "通用设置", settings_screen);

    /* Clock preferences stay above the fold. Appearance and the three
     * calibrated channels follow in one continuous, vertically elastic stage. */
    lv_obj_t *scroll = qz_page_scroll(general_screen, 60);
    lv_obj_t *stage = lv_obj_create(scroll);
    lv_obj_set_size(stage, QZ_DESIGN_W, 488);
    lv_obj_set_pos(stage, 0, 0);
    lv_obj_set_style_bg_opa(stage, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(stage, 0, 0);
    lv_obj_set_style_pad_all(stage, 0, 0);
    lv_obj_clear_flag(stage, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *preferences = section_card(stage, 0, 92, false);
    lv_obj_set_style_clip_corner(preferences, true, 0);

    /* Two 45 px rows fit inside the 92 px instrument at both scale ratios.
     * Using 46 px made the second row round one physical pixel outside its
     * parent on the 320x240 panel. */
    const int preference_row_h = 45;
    lv_obj_t *time_row = info_row(preferences, 0, preference_row_h,
                                  "时间", "10:30", &time_value);
    lv_obj_add_flag(time_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(time_row, open_time_modal, LV_EVENT_CLICKED, general_screen);
    lv_obj_t *time_chev = qz_chevron(time_row, qz_color(QZ_TEXT_TERTIARY), 13);
    lv_obj_align(time_chev, LV_ALIGN_RIGHT_MID, -CARD_PAD - 2, 0);

    lv_obj_t *zone_row = info_row(preferences, preference_row_h, preference_row_h, "时区",
                                  "(GMT+8) 北京", &timezone_value);
    lv_obj_add_flag(zone_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(zone_row, open_timezone_modal, LV_EVENT_CLICKED, general_screen);
    lv_obj_t *zone_chev = qz_chevron(zone_row, qz_color(QZ_TEXT_TERTIARY), 13);
    lv_obj_align(zone_chev, LV_ALIGN_RIGHT_MID, -CARD_PAD - 2, 0);
    lv_obj_t *preference_rule = qz_separator(preferences, CONTENT_W, false);
    lv_obj_align(preference_rule, LV_ALIGN_TOP_LEFT, 0, preference_row_h);

    appearance_panel(stage, 98, 174);

    lv_obj_t *card = section_card(stage, 280, 200, false);
    lv_obj_set_style_clip_corner(card, true, 0);

    /* Start from what the hardware reports; fall back to a neutral default on
     * machines without a mixer or a backlight node. */
    int volume = qz_volume_level();
    int backlight = qz_backlight_level();
    if (volume < 0) volume = 65;
    if (backlight < 0) backlight = 40;

    volume_binding.apply = qz_volume_set;
    slider_row(card, 0, QZ_ICON_VOLUME, 20, "音量", volume, &volume_binding);

    backlight_binding.apply = qz_backlight_set;
    slider_row(card, 64, QZ_ICON_SUN, 32, "亮度", backlight, &backlight_binding);

    material_binding.apply = qz_material_set_percent;
    slider_row(card, 128, QZ_ICON_PALETTE, 20, "界面材质",
               qz_material_percent(), &material_binding);

    const int rules[] = { 64, 128 };
    for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
        lv_obj_t *sep = qz_separator(card, CONTENT_W, false);
        lv_obj_align(sep, LV_ALIGN_TOP_LEFT, 0, rules[i]);
    }

    refresh_settings_clock();
}

/* ------------------------------------------------------------------------- *
 * About page
 * ------------------------------------------------------------------------- */

static void open_server_modal(lv_event_t *event)
{
    (void)event;
    lv_obj_t *panel = open_modal(about_screen, 212);
    lv_obj_t *title = qz_text(panel, "服务地址", 16, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *address = qz_text(panel, about_server, 12, qz_color(QZ_TEXT));
    lv_obj_set_width(address, lv_pct(100));
    lv_label_set_long_mode(address, LV_LABEL_LONG_WRAP);
    lv_obj_align(address, LV_ALIGN_TOP_LEFT, 0, 28);
    lv_obj_t *close = qz_button(panel, "关闭", 104, 40);
    lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(close, close_modal, LV_EVENT_CLICKED, NULL);
}

static void build_about_screen(void)
{
    about_screen = lv_obj_create(NULL);
    qz_style_screen(about_screen);
    page_toolbar(about_screen, "关于", settings_screen);

    const int hero_h = qz_compact() ? 50 : 72;
    const int device_row_h = 40;
    const int server_row_h = qz_compact() ? qz_design_y(QZ_MIN_TOUCH_PX) : device_row_h;
    lv_obj_t *hero = section_card(about_screen, 60,
                                  hero_h + 3 * device_row_h + server_row_h, false);
    lv_obj_set_style_clip_corner(hero, true, 0);
    int mark_size = qz_compact() ? 40 : 60;
    lv_obj_t *mark = qz_squircle(hero, mark_size, QZ_ACCENT);
    lv_obj_align(mark, LV_ALIGN_TOP_LEFT, 16, (hero_h - mark_size) / 2);
    lv_obj_t *mark_icon = qz_icon_image(mark, QZ_ICON_SPARKLES,
                                        qz_compact() ? 22 : 30,
                                        qz_color(QZ_TEXT_ON_ACCENT));
    if (mark_icon) lv_obj_center(mark_icon);
    lv_obj_t *name = qz_text(hero, "QZdesk", 17, qz_color(QZ_TEXT));
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, qz_compact() ? 66 : 88, hero_h / 2 - 18);
    lv_obj_t *caption = qz_text(hero, "AI 桌面语音助手", 11, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(caption, LV_ALIGN_TOP_LEFT, qz_compact() ? 66 : 88, hero_h / 2 + 5);

    snprintf(about_server, sizeof(about_server), "未配置");
    qz_load_config(about_server, sizeof(about_server));
    char address[32];
    qz_device_ip(address, sizeof(address));

    /* Identity heads the same information band as the facts below it. */
    lv_obj_t *rule = qz_separator(hero, CONTENT_W, false);
    lv_obj_align(rule, LV_ALIGN_TOP_LEFT, 0, hero_h);
    info_row(hero, hero_h, device_row_h, "设备型号", "QZdesk Desk", NULL);
    info_row(hero, hero_h + device_row_h, device_row_h,
             "固件版本", "1.0.0", NULL);
    lv_obj_t *server_value;
    lv_obj_t *server_row = info_row(hero, hero_h + 2 * device_row_h, server_row_h,
                                    "服务地址", about_server, &server_value);
    lv_obj_add_flag(server_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(server_row, open_server_modal, LV_EVENT_CLICKED, NULL);
    lv_point_t server_size;
    lv_text_get_size(&server_size, about_server, qz_font_size(12), 0, 0,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (server_size.x > qz_scale_x(CONTENT_W / 2 - 10))
        lv_label_set_text(server_value, "点按查看");
    lv_obj_t *server_chev = qz_chevron(server_row, qz_color(QZ_TEXT_TERTIARY), 14);
    lv_obj_align(server_chev, LV_ALIGN_RIGHT_MID, -CARD_PAD - 2, 0);
    info_row(hero, hero_h + 2 * device_row_h + server_row_h, device_row_h,
             "当前 IP", address, NULL);
    const int separator_y[] = { hero_h + device_row_h,
                                hero_h + 2 * device_row_h,
                                hero_h + 2 * device_row_h + server_row_h };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *sep = qz_separator(hero, CONTENT_W, false);
        lv_obj_align(sep, LV_ALIGN_TOP_LEFT, 0, separator_y[i]);
    }
}

/* ------------------------------------------------------------------------- *
 * Root list
 * ------------------------------------------------------------------------- */

static void open_wlan(lv_event_t *event)
{
    (void)event;
    qz_screen_load(wlan_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, QZ_DUR_SCREEN);
}

static void open_general(lv_event_t *event)
{
    (void)event;
    qz_screen_load(general_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, QZ_DUR_SCREEN);
}

static void open_about(lv_event_t *event)
{
    (void)event;
    qz_screen_load(about_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, QZ_DUR_SCREEN);
}

static lv_obj_t *dnd_switch;
static lv_obj_t *dnd_status;
static lv_obj_t *dnd_schedule_summary;
static lv_obj_t *dnd_schedule_switch;
static lv_obj_t *dnd_schedule_status;
static lv_obj_t *dnd_start_hour_roller;
static lv_obj_t *dnd_start_minute_roller;
static lv_obj_t *dnd_end_hour_roller;
static lv_obj_t *dnd_end_minute_roller;
static void refresh_dnd_schedule_summary(void);

/* 免打扰现在是一行带开关的列表项：写不进去就把开关退回原位，并留下一行
 * "未保存" —— 开关停在没生效的位置比不响应更糟。 */
static void dnd_apply(bool enabled)
{
    if (qz_notifications_set_dnd(enabled)) {
        if (enabled) lv_obj_add_state(dnd_switch, LV_STATE_CHECKED);
        else lv_obj_remove_state(dnd_switch, LV_STATE_CHECKED);
        if (dnd_status) lv_obj_add_flag(dnd_status, LV_OBJ_FLAG_HIDDEN);
        if (dnd_schedule_summary) lv_obj_remove_flag(dnd_schedule_summary,
                                                     LV_OBJ_FLAG_HIDDEN);
        refresh_dnd_schedule_summary();
    } else if (dnd_status) {
        lv_obj_remove_flag(dnd_status, LV_OBJ_FLAG_HIDDEN);
        if (dnd_schedule_summary) lv_obj_add_flag(dnd_schedule_summary,
                                                  LV_OBJ_FLAG_HIDDEN);
    }
}

static void dnd_switch_changed(lv_event_t *event)
{
    (void)event;
    dnd_apply(lv_obj_has_state(dnd_switch, LV_STATE_CHECKED));
}

/* 通知中心里的免打扰开关改的是同一份状态：那边一动手，这枚开关也得跟上，
 * 否则从设置页看它停在上次的位置。 */
static void dnd_changed_elsewhere(bool enabled)
{
    if (!dnd_switch) return;
    if (enabled) lv_obj_add_state(dnd_switch, LV_STATE_CHECKED);
    else lv_obj_remove_state(dnd_switch, LV_STATE_CHECKED);
    refresh_dnd_schedule_summary();
}

static void refresh_dnd_schedule_summary(void)
{
    if (!dnd_schedule_summary) return;
    bool enabled = false;
    int start = 22 * 60;
    int end = 8 * 60;
    qz_notifications_schedule_get(&enabled, &start, &end);
    bool manual = qz_notifications_manual_dnd();
    char text[64];
    if (enabled) {
        snprintf(text, sizeof(text), "每天 %02d:%02d-%02d:%02d%s",
                 start / 60, start % 60, end / 60, end % 60,
                 manual ? " · 手动" : "");
    } else if (manual) {
        snprintf(text, sizeof(text), "手动开启");
    } else {
        snprintf(text, sizeof(text), "未启用");
    }
    lv_label_set_text(dnd_schedule_summary, text);
}

static void dnd_schedule_saved(lv_event_t *event)
{
    if (!dnd_start_hour_roller || !dnd_start_minute_roller ||
        !dnd_end_hour_roller || !dnd_end_minute_roller)
        return;
    int start = (int)lv_roller_get_selected(dnd_start_hour_roller) * 60 +
                (int)lv_roller_get_selected(dnd_start_minute_roller);
    int end = (int)lv_roller_get_selected(dnd_end_hour_roller) * 60 +
              (int)lv_roller_get_selected(dnd_end_minute_roller);
    bool enabled = dnd_schedule_switch &&
                   lv_obj_has_state(dnd_schedule_switch, LV_STATE_CHECKED);
    if (start < 0 || start >= 24 * 60 || end < 0 || end >= 24 * 60) {
        if (dnd_schedule_status) {
            lv_label_set_text(dnd_schedule_status, "时间无效");
            lv_obj_remove_flag(dnd_schedule_status, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (enabled && start == end) {
        if (dnd_schedule_status) {
            lv_label_set_text(dnd_schedule_status, "开始和结束不能相同");
            lv_obj_remove_flag(dnd_schedule_status, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (!qz_notifications_schedule_set(enabled, start, end)) {
        if (dnd_schedule_status) {
            lv_label_set_text(dnd_schedule_status, "未保存，请重试");
            lv_obj_remove_flag(dnd_schedule_status, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    refresh_dnd_schedule_summary();
    close_modal(event);
}

static void style_dnd_roller(lv_obj_t *roller)
{
    lv_obj_set_style_bg_opa(roller, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(roller, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(roller, 0, LV_PART_MAIN);
    qz_obj_set_text_color(roller, QZ_TEXT_TERTIARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(roller, qz_font_size(13), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(roller, 4, LV_PART_MAIN);
    lv_obj_set_style_text_align(roller, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    qz_obj_set_bg_color(roller, QZ_FILL, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_radius(roller, 4, LV_PART_SELECTED);
    qz_obj_set_text_color(roller, QZ_TEXT, LV_PART_SELECTED);
    lv_obj_set_style_text_font(roller, qz_font_size(16), LV_PART_SELECTED);
    lv_roller_set_visible_row_count(roller, 3);
}

static lv_obj_t *dnd_roller(lv_obj_t *panel, const char *options, int selected, int x)
{
    lv_obj_t *roller = lv_roller_create(panel);
    lv_roller_set_options(roller, options, LV_ROLLER_MODE_NORMAL);
    lv_obj_set_width(roller, 66);
    lv_obj_align(roller, LV_ALIGN_TOP_LEFT, x, 121);
    style_dnd_roller(roller);
    lv_roller_set_selected(roller, (uint32_t)selected, LV_ANIM_OFF);
    return roller;
}

static void open_dnd_schedule_modal(lv_event_t *event)
{
    (void)event;
    lv_obj_t *panel = open_modal(settings_screen, 286);
    lv_obj_t *title = qz_text(panel, "安静时段", 16, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *caption = qz_text(panel, "到点自动开启，跨天会自动识别", 12,
                                qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(caption, LV_ALIGN_TOP_LEFT, 0, 24);
    dnd_schedule_status = qz_text(panel, "", 11, qz_color(QZ_RED));
    lv_obj_align(dnd_schedule_status, LV_ALIGN_TOP_RIGHT, 0, 42);
    lv_obj_add_flag(dnd_schedule_status, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *enable_row = lv_obj_create(panel);
    lv_obj_set_size(enable_row, lv_pct(100), 34);
    lv_obj_align(enable_row, LV_ALIGN_TOP_LEFT, 0, 58);
    lv_obj_set_style_bg_opa(enable_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(enable_row, 0, 0);
    lv_obj_set_style_pad_all(enable_row, 0, 0);
    lv_obj_clear_flag(enable_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *enable_label = qz_text(enable_row, "启用定时安静时段", 13,
                                     qz_color(QZ_TEXT));
    lv_obj_align(enable_label, LV_ALIGN_LEFT_MID, 2, 0);
    dnd_schedule_switch = lv_switch_create(enable_row);
    qz_style_switch(dnd_schedule_switch);
    lv_obj_set_size(dnd_schedule_switch, 54, 26);
    qz_touch_target(dnd_schedule_switch);
    lv_obj_align(dnd_schedule_switch, LV_ALIGN_RIGHT_MID, -2, 0);

    bool enabled = false;
    int start = 22 * 60;
    int end = 8 * 60;
    qz_notifications_schedule_get(&enabled, &start, &end);
    if (enabled) lv_obj_add_state(dnd_schedule_switch, LV_STATE_CHECKED);

    static char hours[256];
    static char minutes[256];
    build_number_options(hours, sizeof(hours), 24, 2);
    build_number_options(minutes, sizeof(minutes), 60, 2);

    lv_obj_t *rule = qz_separator(panel, lv_pct(100), false);
    lv_obj_align(rule, LV_ALIGN_TOP_LEFT, 0, 96);
    lv_obj_t *start_label = qz_text(panel, "开始", 11, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(start_label, LV_ALIGN_TOP_LEFT, 86, 104);
    lv_obj_t *end_label = qz_text(panel, "结束", 11, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(end_label, LV_ALIGN_TOP_LEFT, 290, 104);

    dnd_start_hour_roller = dnd_roller(panel, hours, start / 60, 30);
    dnd_start_minute_roller = dnd_roller(panel, minutes, start % 60, 116);
    dnd_end_hour_roller = dnd_roller(panel, hours, end / 60, 234);
    dnd_end_minute_roller = dnd_roller(panel, minutes, end % 60, 320);
    lv_obj_t *start_colon = qz_text(panel, ":", 16, qz_color(QZ_TEXT));
    lv_obj_align_to(start_colon, dnd_start_hour_roller, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
    lv_obj_t *end_colon = qz_text(panel, ":", 16, qz_color(QZ_TEXT));
    lv_obj_align_to(end_colon, dnd_end_hour_roller, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

    modal_actions(panel, "取消", "保存", dnd_schedule_saved, NULL);
}

static void dnd_row_clicked(lv_event_t *event)
{
    /* The switch owns its own gesture; the rest of the tile opens the schedule. */
    if (lv_event_get_target(event) == dnd_switch) return;
    open_dnd_schedule_modal(event);
}

static void settings_screen_loaded(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_SCREEN_LOADED) return;
    refresh_wlan_summary();
    refresh_settings_clock();
    refresh_dnd_schedule_summary();
}

lv_obj_t *qz_settings_create(void)
{
    qzdesk_core_subscribe(timezone_ack, NULL);
    settings_screen = lv_obj_create(NULL);
    qz_style_screen(settings_screen);
    lv_obj_t *header = page_toolbar(settings_screen, "设置", NULL);
    settings_rail_clock = qz_text(header, "在线  --:--", 11,
                                  qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(settings_rail_clock, LV_ALIGN_RIGHT_MID, -QZ_GUTTER, 0);
    lv_obj_add_event_cb(settings_screen, settings_screen_loaded, LV_EVENT_SCREEN_LOADED, NULL);

    build_wlan_screen();
    build_general_screen();
    build_about_screen();

    /* Settings is one quiet control surface: current link first, then the
     * small number of preferences people actually change on-device. The list
     * scrolls only by a few pixels on the compact panel. */
    lv_obj_t *scroll = qz_page_scroll(settings_screen, 58);
    lv_obj_t *stage = lv_obj_create(scroll);
    lv_obj_set_size(stage, QZ_DESIGN_W, 270);
    lv_obj_set_pos(stage, 0, 0);
    lv_obj_set_style_bg_opa(stage, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(stage, 0, 0);
    lv_obj_set_style_pad_all(stage, 0, 0);
    lv_obj_clear_flag(stage, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *list = section_card(stage, 0, 270, false);
    lv_obj_set_style_clip_corner(list, true, 0);
    const int row_h = 54;

    settings_menu_row(list, 0, row_h, QZ_ICON_WIFI, "WLAN", "未连接",
                      open_wlan, true, NULL, &wlan_row_summary);
    settings_menu_row(list, row_h, row_h, QZ_ICON_VOLUME, "声音与显示",
                      "音量、亮度与界面材质", open_general, true, NULL, NULL);
    lv_obj_t *dnd_row = settings_menu_row(list, row_h * 2, row_h, QZ_ICON_BELL,
                                          "安静时段", "未启用", dnd_row_clicked,
                                          false, NULL, &dnd_schedule_summary);
    lv_obj_set_width(dnd_schedule_summary, CONTENT_W - 90 - 54);
    dnd_status = qz_text(dnd_row, "未保存", 11, qz_color(QZ_RED));
    lv_obj_set_width(dnd_status, CONTENT_W - 90 - 54);
    lv_label_set_long_mode(dnd_status, LV_LABEL_LONG_DOT);
    lv_obj_align(dnd_status, LV_ALIGN_TOP_LEFT, CARD_PAD + 32, 30);
    lv_obj_add_flag(dnd_status, LV_OBJ_FLAG_HIDDEN);
    dnd_switch = lv_switch_create(dnd_row);
    qz_style_switch(dnd_switch);
    lv_obj_set_size(dnd_switch, qz_compact() ? 50 : 44, qz_compact() ? 26 : 24);
    qz_touch_target(dnd_switch);
    lv_obj_align(dnd_switch, LV_ALIGN_RIGHT_MID, -CARD_PAD, 0);
    if (qz_notifications_dnd()) lv_obj_add_state(dnd_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(dnd_switch, dnd_switch_changed, LV_EVENT_VALUE_CHANGED, NULL);
    qz_notifications_set_dnd_listener(dnd_changed_elsewhere);

    settings_menu_row(list, row_h * 3, row_h, QZ_ICON_PALETTE, "外观",
                      "主题与界面材质", open_general, true, NULL, &theme_summary);
    settings_menu_row(list, row_h * 4, row_h, QZ_ICON_INFO_CIRCLE, "关于",
                      "版本、设备与服务地址", open_about, true, NULL, NULL);

    sync_theme_choices();
    refresh_dnd_schedule_summary();

    refresh_wlan_summary();
    refresh_settings_clock();
    if (settings_clock_timer) lv_timer_delete(settings_clock_timer);
    settings_clock_timer = lv_timer_create(settings_clock_tick, 30000, NULL);

    return settings_screen;
}

void qz_settings_set_desktop(lv_obj_t *desktop) { desktop_screen = desktop; }
void qz_settings_set_return(lv_obj_t *screen) { settings_return = screen; }
void qz_settings_open_wlan(lv_obj_t *return_screen)
{
    wlan_return = return_screen;
    if (wlan_screen) qz_screen_load(wlan_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, QZ_DUR_SCREEN);
}
