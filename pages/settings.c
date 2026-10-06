#include "settings.h"
#include "ai_face.h"
#include "config.h"
#include "theme.h"
#include "wifi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CONTENT_W (QZ_DESIGN_W - 2 * QZ_GUTTER)
#define CARD_PAD 12
#define MODAL_KEYBOARD_H 140

static lv_obj_t *desktop_screen;
static lv_obj_t *settings_screen;
static lv_obj_t *wlan_screen;
static lv_obj_t *general_screen;
static lv_obj_t *about_screen;

/* Two labels show the same connection summary: the card on the WLAN page and
 * the row value on the settings list. They need separate pointers - reusing one
 * silently orphaned whichever screen was built first. */
static lv_obj_t *wlan_page_summary;
static lv_obj_t *wlan_row_summary;
static lv_obj_t *wlan_toggle;
static lv_obj_t *wlan_list;
static lv_obj_t *time_value;
static lv_obj_t *timezone_value;
static lv_obj_t *dark_mode_value;
static lv_obj_t *dark_mode_switch;
static lv_obj_t *modal_overlay;

static char pending_ssid[64];
static bool wlan_enabled = true;

static void wlan_network_clicked(lv_event_t *event);
static void wlan_refresh_list(void);
static void toggle_dark_mode(lv_event_t *event);

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
    qz_screen_load(target ? target : desktop_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, QZ_DUR_SCREEN);
}

static lv_obj_t *page_toolbar(lv_obj_t *screen, const char *title, lv_obj_t *back_target)
{
    lv_obj_t *toolbar = lv_obj_create(screen);
    lv_obj_set_size(toolbar, QZ_DESIGN_W - 16, QZ_TOOLBAR_H - 4);
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 6);
    qz_style_toolbar(toolbar);

    lv_obj_t *back = qz_icon_button(toolbar, LV_SYMBOL_LEFT, 30);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_event_cb(back, go_back, LV_EVENT_CLICKED, back_target);

    lv_obj_t *label = qz_text(toolbar, title, 15, qz_color(QZ_TEXT));
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
    return toolbar;
}

/** Rounded group card that hosts rows or free content. */
static lv_obj_t *section_card(lv_obj_t *parent, int y, int height, bool interactive)
{
    lv_obj_t *card = interactive ? qz_card_button(parent, CONTENT_W, height)
                                 : lv_obj_create(parent);
    if (!interactive) {
        lv_obj_set_size(card, CONTENT_W, height);
        lv_obj_set_style_radius(card, QZ_RADIUS_CARD, 0);
        qz_style_plate(card);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, QZ_GUTTER, y);
    return card;
}

/** iOS style grouped row: gradient icon, title, optional value and chevron. */
static lv_obj_t *group_row(lv_obj_t *parent, int y, int height, const char *symbol,
                           uint32_t tile, uint32_t mark,
                           const char *title, const char *value, lv_event_cb_t cb,
                           lv_obj_t **value_out)
{
    lv_obj_t *row = section_card(parent, y, height, true);
    lv_obj_set_style_pad_all(row, 0, 0);
    if (cb) {
        lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
    }

    const int32_t icon_size = QZ_ROW_ICON;
    lv_obj_t *icon = qz_squircle(row, icon_size, tile);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, CARD_PAD, 0);
    lv_obj_t *glyph = qz_symbol(icon, symbol, icon_size * 54 / 100, qz_color(mark));
    lv_obj_center(glyph);

    const int32_t text_x = CARD_PAD + icon_size + 10;
    lv_obj_t *title_label = qz_text(row, title, 14, qz_color(QZ_TEXT));
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, text_x, value ? -9 : 0);

    if (value) {
        lv_obj_t *value_label = qz_text(row, value, 11, qz_color(QZ_TEXT_SECONDARY));
        lv_obj_align(value_label, LV_ALIGN_LEFT_MID, text_x, 10);
        lv_obj_set_width(value_label, CONTENT_W - text_x - CARD_PAD - 16);
        lv_label_set_long_mode(value_label, LV_LABEL_LONG_DOT);
        if (value_out) *value_out = value_label;
    }
    lv_obj_t *chev = qz_chevron(row, qz_color(QZ_TEXT_TERTIARY), 14);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -CARD_PAD - 2, 0);
    return row;
}

static lv_obj_t *info_glyph(lv_obj_t *parent, int32_t size, lv_color_t color)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, size, size);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ring = lv_obj_create(box);
    lv_obj_set_size(ring, size * 88 / 100, size * 88 / 100);
    lv_obj_center(ring);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, size * 9 / 100, 0);
    lv_obj_set_style_border_color(ring, color, 0);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(ring, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dot = lv_obj_create(box);
    lv_obj_set_size(dot, size * 16 / 100, size * 16 / 100);
    lv_obj_align(dot, LV_ALIGN_CENTER, 0, -size * 18 / 100);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, color, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(box);
    lv_obj_set_size(bar, size * 16 / 100, size * 34 / 100);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, size * 12 / 100);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(bar, color, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    return box;
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
    lv_label_set_long_mode(value_label, LV_LABEL_LONG_DOT);
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
    lv_obj_set_style_radius(panel, QZ_RADIUS_CARD, 0);
    /* A sheet is a deeper material than the cards: slightly more opaque, with a
     * heavier shadow so it reads as floating above the dimmed page. */
    qz_style_glass(panel);
    lv_obj_set_style_bg_opa(panel, (lv_opa_t)238, 0);
    lv_obj_set_style_shadow_width(panel, 30, 0);
    lv_obj_set_style_shadow_opa(panel, (lv_opa_t)60, 0);
    lv_obj_set_style_shadow_offset_y(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, CARD_PAD, 0);
    lv_obj_set_style_clip_corner(panel, true, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

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

    lv_obj_t *cancel = qz_button(row, cancel_caption, 88, 32);
    lv_obj_add_event_cb(cancel, close_modal, LV_EVENT_CLICKED, NULL);

    lv_obj_t *confirm = lv_button_create(row);
    lv_obj_set_size(confirm, 96, 32);
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
    char summary[96];
    qz_wifi_current(connected, sizeof(connected));
    qz_wifi_status_t status;
    qz_wifi_status(&status);
    wlan_state = status.state;
    snprintf(wlan_error, sizeof(wlan_error), "%.95s", status.error);

    if (!wlan_enabled || wlan_state == QZ_WIFI_OFF) {
        snprintf(summary, sizeof(summary), "已关闭");
    } else if (wlan_state == QZ_WIFI_CONNECTING) {
        snprintf(summary, sizeof(summary), "正在连接：%s", status.ssid);
    } else if (wlan_state == QZ_WIFI_OBTAINING_IP) {
        snprintf(summary, sizeof(summary), "正在获取 IP 地址…");
    } else if (wlan_state == QZ_WIFI_FAILED) {
        snprintf(summary, sizeof(summary), "%s", status.error[0] ? status.error : "连接失败");
    } else if (!qz_wifi_ready()) {
        snprintf(summary, sizeof(summary), "设备未提供无线网卡");
    } else if (connected[0] != '\0') {
        snprintf(summary, sizeof(summary), "已连接：%s", connected);
    } else {
        snprintf(summary, sizeof(summary), "未连接");
    }

    lv_obj_t *labels[2] = { wlan_page_summary, wlan_row_summary };
    for (int i = 0; i < 2; i++) {
        if (!labels[i]) continue;
        lv_label_set_text(labels[i], summary);
        lv_obj_set_style_text_color(labels[i],
                                    qz_color(wlan_enabled ? QZ_TEXT_SECONDARY
                                                          : QZ_TEXT_TERTIARY), 0);
    }
    if (wlan_list) {
        lv_obj_set_style_opa(wlan_list, wlan_enabled ? LV_OPA_COVER : (lv_opa_t)110, 0);
    }
}

static void rebuild_wlan_list(void)
{
    if (!wlan_list) return;
    lv_obj_clean(wlan_list);

    if (access_point_count == 0) {
        const char *text = !qz_wifi_ready()    ? "设备未提供无线网卡"
                           : wlan_scanning     ? "正在扫描…"
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
        lv_obj_center(empty);
        return;
    }

    char connected[64];
    qz_wifi_current(connected, sizeof(connected));

    for (int i = 0; i < access_point_count; i++) {
        const qz_wifi_ap_t *ap = &access_points[i];
        lv_obj_t *row = lv_obj_create(wlan_list);
        lv_obj_set_size(row, lv_pct(100), 44);
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
}

/** Pull fresh scan results; rebuild the list only when something changed, so
 *  the scroll position survives the periodic refresh. */
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
        access_point_count = 0;
        rebuild_wlan_list();
        refresh_wlan_summary();
        return;
    }

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
    wlan_scanning = true;
    wlan_signature[0] = '\0';
    rebuild_wlan_list();
    refresh_wlan_summary();
    if (!wlan_enabled) return;
    qz_wifi_worker_start();
    qz_wifi_enable_async(true);
    wlan_refresh_list();
}

static void wlan_toggled(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    wlan_enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (wlan_enabled) {
        /* Same flow as the SDK: interface up, supplicant started, then scan. */
        qz_wifi_enable_async(true);
        wlan_scanning = true;
        wlan_signature[0] = '\0';
        wlan_refresh_list();
    } else {
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
        wlan_signature[0] = '\0';
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

    lv_obj_t *glyph = lv_obj_get_child(eye, 0);
    if (glyph) {
        lv_label_set_text(glyph, hidden ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
        lv_obj_set_style_text_color(glyph, qz_color(hidden ? QZ_ACCENT : QZ_TEXT_TERTIARY), 0);
    }
}

static void open_password_modal(lv_obj_t *screen, const char *ssid)
{
    /* Only remember which network the sheet is asking about: the connection
     * state itself must not change until the user confirms, otherwise a cancel
     * would leave the summary text and the list checkmark out of sync. */
    snprintf(pending_ssid, sizeof(pending_ssid), "%.63s", ssid);

    /* Compact sheet: title, one line of context, the field and the actions. */
    lv_obj_t *panel = open_modal(screen, 146);
    lv_obj_t *title = qz_text(panel, "输入无线网络密码", 14, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    char subtitle[96];
    snprintf(subtitle, sizeof(subtitle), "「%s」需要密码才能加入", ssid);
    lv_obj_t *hint = qz_text(panel, subtitle, 11, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 21);

    lv_obj_t *input = lv_textarea_create(panel);
    lv_obj_set_size(input, lv_pct(100), 36);
    lv_obj_align(input, LV_ALIGN_TOP_LEFT, 0, 44);
    lv_textarea_set_one_line(input, true);
    lv_textarea_set_password_mode(input, true);
    lv_textarea_set_placeholder_text(input, "密码");
    qz_style_textarea(input);
    /* Keep the text clear of the eye button on the right. */
    lv_obj_set_style_pad_right(input, 40, 0);

    lv_obj_t *eye = qz_icon_button(panel, LV_SYMBOL_EYE_CLOSE, 28);
    lv_obj_align_to(eye, input, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_set_style_bg_opa(eye, LV_OPA_TRANSP, 0);
    qz_obj_set_bg_color(eye, QZ_FILL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(eye, (lv_opa_t)120, LV_STATE_PRESSED);
    lv_obj_set_user_data(eye, input);
    lv_obj_add_event_cb(eye, toggle_password_visibility, LV_EVENT_CLICKED, NULL);

    lv_obj_t *keyboard = lv_keyboard_create(modal_overlay);
    lv_obj_set_size(keyboard, lv_pct(100), MODAL_KEYBOARD_H);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard, input);
    qz_style_keyboard(keyboard);

    /* Sit the sheet on top of the keyboard instead of at a fixed offset: the
     * actions can then never end up underneath the keys, whatever the panel
     * height or the keyboard height are. */
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -(MODAL_KEYBOARD_H + 10));

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
            open_password_modal(wlan_screen, ssid);
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

    lv_obj_t *card = section_card(wlan_screen, 52, 58, false);
    lv_obj_t *label = qz_text(card, "WLAN", 14, qz_color(QZ_TEXT));
    lv_obj_align(label, LV_ALIGN_LEFT_MID, CARD_PAD, -11);
    wlan_page_summary = qz_text(card, "已连接：Home_5G", 11, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(wlan_page_summary, LV_ALIGN_LEFT_MID, CARD_PAD, 10);
    lv_obj_set_width(wlan_page_summary, CONTENT_W - 2 * CARD_PAD - 64);
    lv_label_set_long_mode(wlan_page_summary, LV_LABEL_LONG_DOT);

    wlan_toggle = lv_switch_create(card);
    lv_obj_set_size(wlan_toggle, 46, 28);
    lv_obj_align(wlan_toggle, LV_ALIGN_RIGHT_MID, -CARD_PAD, 0);
    qz_style_switch(wlan_toggle);
    lv_obj_add_state(wlan_toggle, LV_STATE_CHECKED);
    lv_obj_add_event_cb(wlan_toggle, wlan_toggled, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *header = qz_text(wlan_screen, "可用网络", 11, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, QZ_GUTTER + 4, 118);

    lv_obj_t *list_card = section_card(wlan_screen, 136, 4 * 44, false);
    lv_obj_set_style_clip_corner(list_card, true, 0);
    wlan_list = list_card;
    lv_obj_set_flex_flow(wlan_list, LV_FLEX_FLOW_COLUMN);
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
    if (binding->apply) binding->apply(value);
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

static void slider_row(lv_obj_t *card, int y, const char *glyph, int initial,
                       slider_binding_t *binding)
{
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_set_size(row, lv_pct(100), 32);
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = qz_symbol(row, glyph, 15, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *value = qz_text(row, "0%", 12, qz_color(QZ_TEXT));
    lv_obj_align(value, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_size(slider, CONTENT_W - 2 * CARD_PAD - 22 - 40, 26);
    lv_obj_align(slider, LV_ALIGN_LEFT_MID, 22, 0);
    lv_slider_set_range(slider, 0, 100);
    qz_style_slider(slider);
    if (binding) {
        binding->label = value;
        binding->slider = slider;
        lv_obj_add_event_cb(slider, slider_changed, LV_EVENT_VALUE_CHANGED, binding);
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
    lv_roller_set_visible_row_count(roller, 3);
    lv_obj_set_style_bg_opa(roller, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(roller, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(roller, 0, LV_PART_MAIN);
    qz_obj_set_text_color(roller, QZ_TEXT_TERTIARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(roller, qz_font_size(15), LV_PART_MAIN);
    lv_obj_set_style_text_line_space(roller, 14, LV_PART_MAIN);
    qz_obj_set_bg_color(roller, QZ_FILL, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(roller, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_radius(roller, 10, LV_PART_SELECTED);
    qz_obj_set_text_color(roller, QZ_TEXT, LV_PART_SELECTED);
    lv_obj_set_style_text_font(roller, qz_font_size(17), LV_PART_SELECTED);
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

static void timezone_confirm(lv_event_t *event)
{
    const timezone_entry_t *zone = (const timezone_entry_t *)lv_event_get_user_data(event);
    if (zone) {
        if (timezone_value) lv_label_set_text(timezone_value, zone->label);
        qz_timezone_apply(zone->tz);
    }
    close_modal(event);
}

static void open_timezone_modal(lv_event_t *event)
{
    lv_obj_t *screen = (lv_obj_t *)lv_event_get_user_data(event);
    static const timezone_entry_t zones[] = {
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
    const int count = (int)(sizeof(zones) / sizeof(zones[0]));
    lv_obj_t *panel = open_modal(screen, 224);
    lv_obj_t *title = qz_text(panel, "选择时区", 14, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *list = lv_obj_create(panel);
    lv_obj_set_size(list, lv_pct(100), 176);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, 24);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 2, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < count; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_set_size(row, lv_pct(100), 38);
        qz_style_row(row);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, timezone_confirm, LV_EVENT_CLICKED, (void *)&zones[i]);
        lv_obj_t *label = qz_text(row, zones[i].label, 13, qz_color(QZ_TEXT));
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);
        if (timezone_value &&
            strcmp(lv_label_get_text(timezone_value), zones[i].label) == 0) {
            lv_obj_t *check = qz_symbol(row, LV_SYMBOL_OK, 14, qz_color(QZ_ACCENT));
            lv_obj_align(check, LV_ALIGN_RIGHT_MID, -12, 0);
        }
    }
}

/* The switch state is the source of truth: when it moves the whole palette
 * flips live, the value label catches up, and the choice is persisted so the
 * next launch comes up in the same theme. */
static void toggle_dark_mode(lv_event_t *event)
{
    (void)event;
    bool dark = lv_obj_has_state(dark_mode_switch, LV_STATE_CHECKED);
    qz_theme_set_dark(dark);
    qz_appearance_save(dark);
    if (dark_mode_value) lv_label_set_text(dark_mode_value, dark ? "开" : "关");
}

static void build_general_screen(void)
{
    general_screen = lv_obj_create(NULL);
    qz_style_screen(general_screen);
    page_toolbar(general_screen, "通用设置", settings_screen);

    /* Three sliders plus three list rows have to close inside 320px, so these
     * rows are 44px and the list rows below are 37px rather than the roomier
     * defaults used elsewhere. */
    lv_obj_t *card = section_card(general_screen, 52, 154, false);
    lv_obj_set_style_pad_all(card, 10, 0);

    /* Start from what the hardware reports; fall back to a neutral default on
     * machines without a mixer or a backlight node. */
    int volume = qz_volume_level();
    int backlight = qz_backlight_level();
    if (volume < 0) volume = 65;
    if (backlight < 0) backlight = 40;

    lv_obj_t *volume_title = qz_text(card, "声音", 12, qz_color(QZ_TEXT));
    lv_obj_align(volume_title, LV_ALIGN_TOP_LEFT, 0, 0);
    volume_binding.apply = qz_volume_set;
    slider_row(card, 14, LV_SYMBOL_VOLUME_MAX, volume, &volume_binding);

    lv_obj_t *sep = qz_separator(card, CONTENT_W - 2 * 10, false);
    lv_obj_align(sep, LV_ALIGN_TOP_LEFT, 0, 42);

    lv_obj_t *brightness_title = qz_text(card, "背光", 12, qz_color(QZ_TEXT));
    lv_obj_align(brightness_title, LV_ALIGN_TOP_LEFT, 0, 44);
    backlight_binding.apply = qz_backlight_set;
    slider_row(card, 60, LV_SYMBOL_TINT, backlight, &backlight_binding);

    lv_obj_t *sep_material = qz_separator(card, CONTENT_W - 2 * 10, false);
    lv_obj_align(sep_material, LV_ALIGN_TOP_LEFT, 0, 88);

    /* How much white the material puts over whatever is behind it: 100% is solid
     * (the flat theme), lower values let the page read through the bars and the
     * plates. Applies the moment the slider moves. */
    lv_obj_t *material_title = qz_text(card, "材质通透度", 12, qz_color(QZ_TEXT));
    lv_obj_align(material_title, LV_ALIGN_TOP_LEFT, 0, 90);
    material_binding.apply = qz_material_set_percent;
    slider_row(card, 106, LV_SYMBOL_IMAGE, qz_material_percent(), &material_binding);

    /* The appearance card holds the dark-mode switch and the time / timezone
     * rows. Dark mode flips the whole palette live; the switch is the only
     * surface in the page that can change the theme while it runs. */
    lv_obj_t *list_card = section_card(general_screen, 214, 111, false);
    lv_obj_set_style_clip_corner(list_card, true, 0);
    info_row(list_card, 0, 37, "深色模式", qz_theme_is_dark() ? "开" : "关",
             &dark_mode_value);
    lv_obj_t *sep2 = qz_separator(list_card, CONTENT_W - 2 * CARD_PAD, false);
    lv_obj_align(sep2, LV_ALIGN_TOP_LEFT, CARD_PAD, 37);
    info_row(list_card, 37, 37, "时间", "10:30", &time_value);
    lv_obj_t *sep3 = qz_separator(list_card, CONTENT_W - 2 * CARD_PAD, false);
    lv_obj_align(sep3, LV_ALIGN_TOP_LEFT, CARD_PAD, 74);
    info_row(list_card, 74, 37, "时区", "(GMT+8) 北京", &timezone_value);

    /* Dark mode: a switch on the row, persisted the instant it moves. */
    lv_obj_t *dark_row = lv_obj_create(list_card);
    lv_obj_set_size(dark_row, lv_pct(100), 37);
    lv_obj_align(dark_row, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(dark_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dark_row, 0, 0);
    lv_obj_set_style_pad_all(dark_row, 0, 0);
    lv_obj_clear_flag(dark_row, LV_OBJ_FLAG_SCROLLABLE);
    dark_mode_switch = lv_switch_create(dark_row);
    qz_style_switch(dark_mode_switch);
    lv_obj_set_size(dark_mode_switch, 42, 26);
    lv_obj_align(dark_mode_switch, LV_ALIGN_RIGHT_MID, -CARD_PAD - 2, 0);
    if (qz_theme_is_dark()) lv_obj_add_state(dark_mode_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(dark_mode_switch, toggle_dark_mode, LV_EVENT_VALUE_CHANGED,
                        general_screen);

    /* Transparent overlays make the time/timezone rows tappable while keeping
     * the layout. */
    lv_obj_t *time_row = lv_obj_create(list_card);
    lv_obj_set_size(time_row, lv_pct(100), 37);
    lv_obj_align(time_row, LV_ALIGN_TOP_LEFT, 0, 37);
    lv_obj_set_style_bg_opa(time_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(time_row, 0, 0);
    lv_obj_set_style_pad_all(time_row, 0, 0);
    lv_obj_add_flag(time_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(time_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(time_row, open_time_modal, LV_EVENT_CLICKED, general_screen);
    lv_obj_t *time_chev = qz_chevron(time_row, qz_color(QZ_TEXT_TERTIARY), 14);
    lv_obj_align(time_chev, LV_ALIGN_RIGHT_MID, -CARD_PAD - 2, 0);

    lv_obj_t *zone_row = lv_obj_create(list_card);
    lv_obj_set_size(zone_row, lv_pct(100), 37);
    lv_obj_align(zone_row, LV_ALIGN_TOP_LEFT, 0, 74);
    lv_obj_set_style_bg_opa(zone_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(zone_row, 0, 0);
    lv_obj_set_style_pad_all(zone_row, 0, 0);
    lv_obj_add_flag(zone_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(zone_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(zone_row, open_timezone_modal, LV_EVENT_CLICKED, general_screen);
    lv_obj_t *zone_chev = qz_chevron(zone_row, qz_color(QZ_TEXT_TERTIARY), 14);
    lv_obj_align(zone_chev, LV_ALIGN_RIGHT_MID, -CARD_PAD - 2, 0);

    time_t now = time(NULL);
    struct tm local_time;
    char clock_text[16];
    localtime_r(&now, &local_time);
    strftime(clock_text, sizeof(clock_text), "%H:%M", &local_time);
    if (time_value) lv_label_set_text(time_value, clock_text);
}

/* ------------------------------------------------------------------------- *
 * About page
 * ------------------------------------------------------------------------- */

static void build_about_screen(void)
{
    about_screen = lv_obj_create(NULL);
    qz_style_screen(about_screen);
    page_toolbar(about_screen, "关于", settings_screen);

    lv_obj_t *hero = section_card(about_screen, 52, 92, false);
    lv_obj_t *face = qz_face_create(hero, 60);
    lv_obj_align(face, LV_ALIGN_LEFT_MID, 16, 0);
    qz_face_set_state(face, QZ_FACE_LOVE);
    lv_obj_t *name = qz_text(hero, "QZdesk", 17, qz_color(QZ_TEXT));
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 88, -14);
    lv_obj_t *caption = qz_text(hero, "AI 桌面语音助手", 11, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(caption, LV_ALIGN_LEFT_MID, 88, 10);

    char server[96] = "未配置";
    qz_load_config(server, sizeof(server));
    char address[32];
    qz_device_ip(address, sizeof(address));

    lv_obj_t *card = section_card(about_screen, 154, 4 * 38, false);
    lv_obj_set_style_clip_corner(card, true, 0);
    info_row(card, 0, 38, "设备型号", "QZdesk Desk", NULL);
    info_row(card, 38, 38, "固件版本", "1.0.0", NULL);
    info_row(card, 76, 38, "服务地址", server, NULL);
    info_row(card, 114, 38, "当前 IP", address, NULL);
    for (int i = 1; i < 4; i++) {
        lv_obj_t *sep = qz_separator(card, CONTENT_W - 2 * CARD_PAD, false);
        lv_obj_align(sep, LV_ALIGN_TOP_LEFT, CARD_PAD, i * 38);
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

lv_obj_t *qz_settings_create(void)
{
    settings_screen = lv_obj_create(NULL);
    qz_style_screen(settings_screen);
    page_toolbar(settings_screen, "设置", NULL);

    build_wlan_screen();
    build_general_screen();
    build_about_screen();

    group_row(settings_screen, 52, 62, LV_SYMBOL_WIFI,
              QZ_ACCENT, QZ_TEXT_ON_ACCENT,
              "WLAN", " ", open_wlan, &wlan_row_summary);
    refresh_wlan_summary();

    group_row(settings_screen, 122, 62, LV_SYMBOL_SETTINGS, QZ_ACCENT_TINT,
              QZ_ACCENT_DARK, "通用设置", "声音 · 背光 · 时间", open_general, NULL);

    lv_obj_t *about_row = group_row(settings_screen, 192, 62, LV_SYMBOL_FILE,
                                    QZ_ACCENT, QZ_TEXT_ON_ACCENT,
                                    "关于", "版本与设备信息", open_about, NULL);
    /* The generic squircle icon is replaced by the "info" glyph, drawn white on
     * the same accent fill. */
    lv_obj_t *about_icon = lv_obj_get_child(about_row, 0);
    if (about_icon) {
        lv_obj_clean(about_icon);
        lv_obj_t *glyph = info_glyph(about_icon, QZ_ROW_ICON - 12, qz_color(QZ_TEXT_ON_ACCENT));
        lv_obj_center(glyph);
    }

    lv_obj_t *footer = qz_text(settings_screen, "QZdesk 桌面助手 · 界面演示版", 10,
                               qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -16);
    return settings_screen;
}

void qz_settings_set_desktop(lv_obj_t *desktop) { desktop_screen = desktop; }
