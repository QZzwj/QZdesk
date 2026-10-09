#include "settings.h"
#include "ai_face.h"
#include "config.h"
#include "theme.h"
#include "wifi.h"
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
static lv_obj_t *wlan_page_value;
static lv_obj_t *wlan_previous;
static lv_obj_t *wlan_next;
static int wlan_page;
static lv_obj_t *time_value;
static lv_obj_t *timezone_value;
static lv_obj_t *dark_mode_value;
static lv_obj_t *dark_mode_switch;
static lv_obj_t *modal_overlay;

static char pending_ssid[64];
static bool wlan_enabled = true;

static void wlan_network_clicked(lv_event_t *event);
static void wlan_refresh_list(void);
static void wlan_current_clicked(lv_event_t *event);
static void wlan_browse_clicked(lv_event_t *event);
static void wlan_disconnect_clicked(lv_event_t *event);
static void wlan_forget_clicked(lv_event_t *event);
static void open_details_modal(lv_obj_t *screen);
static void wlan_rebuild_async(void *unused);
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
    lv_obj_set_size(toolbar, QZ_DESIGN_W - 16, QZ_TOOLBAR_H + 2);
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 4);
    qz_style_toolbar(toolbar);

    lv_obj_t *back = qz_back_button(toolbar);
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
    lv_obj_t *title_label = qz_text(row, title, qz_compact() ? 16 : 14, qz_color(QZ_TEXT));
    if (value)
        lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, text_x, 9);
    else
        lv_obj_align(title_label, LV_ALIGN_LEFT_MID, text_x, value ? -9 : 0);

    if (value) {
        lv_obj_t *value_label = qz_text(row, value, 11, qz_color(QZ_TEXT_SECONDARY));
        lv_obj_set_width(value_label, CONTENT_W - text_x - CARD_PAD - 16);
        lv_label_set_long_mode(value_label, LV_LABEL_LONG_WRAP);
        lv_obj_align(value_label, LV_ALIGN_TOP_LEFT, text_x, 30);
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

    lv_obj_t *cancel = qz_button(row, cancel_caption, 100, 38);
    lv_obj_add_event_cb(cancel, close_modal, LV_EVENT_CLICKED, NULL);

    lv_obj_t *confirm = lv_button_create(row);
    lv_obj_set_size(confirm, 104, 38);
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

/* A finite page keeps every result inside the screen, even when a scan returns
 * the maximum number of access points. Long SSIDs wrap inside their own row. */
static int wlan_rows_per_page(void)
{
    for (int i = 0; i < access_point_count; i++) {
        lv_point_t size;
        lv_text_get_size(&size, access_points[i].ssid, qz_font_size(13), 0, 0,
                         qz_scale_x(CONTENT_W - 2 * CARD_PAD - 32 - 54), LV_TEXT_FLAG_NONE);
        if (size.y > qz_scale_y(qz_compact() ? 56 : 44) - 4) return 1;
    }
    return qz_compact() ? 2 : 3;
}

static void wlan_update_pager(int count)
{
    int pages = (count + wlan_rows_per_page() - 1) / wlan_rows_per_page();
    if (pages < 1) pages = 1;
    if (wlan_page >= pages) wlan_page = pages - 1;
    if (wlan_page < 0) wlan_page = 0;
    if (wlan_page_value) {
        char text[32];
        snprintf(text, sizeof(text), "%d / %d", wlan_page + 1, pages);
        lv_label_set_text(wlan_page_value, text);
    }
    if (wlan_previous) {
        if (wlan_page == 0) lv_obj_add_state(wlan_previous, LV_STATE_DISABLED);
        else lv_obj_remove_state(wlan_previous, LV_STATE_DISABLED);
    }
    if (wlan_next) {
        if (wlan_page + 1 >= pages) lv_obj_add_state(wlan_next, LV_STATE_DISABLED);
        else lv_obj_remove_state(wlan_next, LV_STATE_DISABLED);
    }
}

static void wlan_page_clicked(lv_event_t *event)
{
    wlan_page += (int)(intptr_t)lv_event_get_user_data(event);
    lv_async_call(wlan_rebuild_async, NULL);
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
        snprintf(summary, sizeof(summary), "WiFi已关闭");
    } else if (wlan_state == QZ_WIFI_CONNECTING) {
        snprintf(summary, sizeof(summary), "正在连接 WiFi...");
    } else if (wlan_state == QZ_WIFI_OBTAINING_IP) {
        snprintf(summary, sizeof(summary), "正在等待获取 IP...");
    } else if (wlan_state == QZ_WIFI_FAILED) {
        snprintf(summary, sizeof(summary), "%s",
                 status.error[0] ? status.error : "WiFi 连接失败，请检查密码");
    } else if (!qz_wifi_ready()) {
        snprintf(summary, sizeof(summary), "%s", qz_wifi_nic_text());
    } else if (connected[0] != '\0') {
        if (qz_compact()) snprintf(summary, sizeof(summary), "WiFi 已连接");
        else snprintf(summary, sizeof(summary), "WiFi已连接：%s", connected);
    } else {
        snprintf(summary, sizeof(summary), "请选择 WiFi 网络");
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
    wlan_update_pager(access_point_count);

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

    char connected[64];
    qz_wifi_current(connected, sizeof(connected));
    if (connected[0] == '\0') wlan_browse = false;   /* 断开了就回到正常列表 */

    if (connected[0] != '\0' && !wlan_browse) {
        wlan_page = 0;
        wlan_update_pager(0);
        /* Echo-Mate 的已连接视图：列表换成「当前网络 + 搜索其他 WiFi」两行 */
        char title[96];
        lv_obj_t *cur = lv_obj_create(wlan_list);
        lv_obj_set_size(cur, lv_pct(100), qz_compact() ? 72 : 76);
        qz_style_row(cur);
        lv_obj_add_flag(cur, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cur, wlan_current_clicked, LV_EVENT_CLICKED, NULL);
        snprintf(title, sizeof(title), "%s", connected);
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

    int first = wlan_page * wlan_rows_per_page();
    int last = first + wlan_rows_per_page();
    if (last > access_point_count) last = access_point_count;
    for (int i = first; i < last; i++) {
        const qz_wifi_ap_t *ap = &access_points[i];
        lv_obj_t *row = lv_obj_create(wlan_list);
        const int row_height = wlan_rows_per_page() == 1 ? (qz_compact() ? 108 : 128)
                                                       : (qz_compact() ? 56 : 44);
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
    wlan_page = 0;
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
    lv_obj_set_style_bg_color(disconnect, lv_palette_main(LV_PALETTE_ORANGE), 0);
    lv_obj_add_event_cb(disconnect, wlan_disconnect_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *disconnect_label = qz_text(disconnect, "断开连接", 16, qz_color(QZ_TEXT_ON_ACCENT));
    lv_obj_center(disconnect_label);

    lv_obj_t *forget = lv_button_create(row);
    lv_obj_set_size(forget, 112, 38);
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

    if (!wlan_nic_seen) {
        wlan_nic_seen = true;
        /* 网卡是"迟到"的（驱动模块 + 固件要几十秒才把 wlan0 顶出来）：页面打开
         * 时那次 enable 和扫描其实都失败了，这里补一次，用户不用退出重进等着。 */
        if (wlan_enabled) {
            qz_wifi_enable_async(true);
            qz_wifi_scan_async();
            wlan_scanning = true;
            wlan_signature[0] = '\0';
        }
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

    /* 先读 worker 的共享状态：qz_wifi_status() 只锁一下内存，不执行任何外部命令。 */
    qz_wifi_status_t worker;
    if (qz_wifi_status(&worker)) wlan_state = worker.state;

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
    wlan_page = 0;
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

    lv_obj_t *card = section_card(wlan_screen, 52, qz_compact() ? 78 : 58, false);
    lv_obj_t *label = qz_text(card, "WLAN", 14, qz_color(QZ_TEXT));
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, CARD_PAD, qz_compact() ? 10 : 6);
    wlan_page_summary = qz_text(card, "已连接：Home_5G", 11, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(wlan_page_summary, LV_ALIGN_TOP_LEFT, CARD_PAD, qz_compact() ? 34 : 29);
    lv_obj_set_width(wlan_page_summary, CONTENT_W - 2 * CARD_PAD - 72);
    lv_label_set_long_mode(wlan_page_summary, LV_LABEL_LONG_WRAP);

    wlan_toggle = lv_switch_create(card);
    lv_obj_set_size(wlan_toggle, qz_compact() ? 54 : 46, 32);
    lv_obj_align(wlan_toggle, LV_ALIGN_RIGHT_MID, -CARD_PAD, 0);
    qz_style_switch(wlan_toggle);
    lv_obj_add_state(wlan_toggle, LV_STATE_CHECKED);
    lv_obj_add_event_cb(wlan_toggle, wlan_toggled, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *header = qz_text(wlan_screen, "可用网络", 11, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, QZ_GUTTER + 4, qz_compact() ? 136 : 118);

    lv_obj_t *list_card = section_card(wlan_screen, qz_compact() ? 156 : 136,
                                      qz_compact() ? 116 : 136, false);
    lv_obj_set_style_clip_corner(list_card, true, 0);
    wlan_list = list_card;
    lv_obj_set_flex_flow(wlan_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(wlan_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(wlan_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_row(wlan_list, 0, 0);
    lv_obj_set_flex_align(wlan_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    wlan_previous = qz_button(wlan_screen, "上一页", 112, 40);
    lv_obj_align(wlan_previous, LV_ALIGN_TOP_LEFT, QZ_GUTTER, 276);
    lv_obj_add_event_cb(wlan_previous, wlan_page_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    wlan_next = qz_button(wlan_screen, "下一页", 112, 40);
    lv_obj_align(wlan_next, LV_ALIGN_TOP_RIGHT, -QZ_GUTTER, 276);
    lv_obj_add_event_cb(wlan_next, wlan_page_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    wlan_page_value = qz_text(wlan_screen, "1 / 1", 14, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(wlan_page_value, LV_ALIGN_TOP_MID, 0, 286);
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
#define TIMEZONE_PAGE_ROWS 4
#define TIMEZONE_COUNT ((int)(sizeof(timezones) / sizeof(timezones[0])))
static lv_obj_t *timezone_list;
static lv_obj_t *timezone_previous;
static lv_obj_t *timezone_next;
static lv_obj_t *timezone_page_value;
static int timezone_page;

static void timezone_confirm(lv_event_t *event)
{
    const timezone_entry_t *zone = (const timezone_entry_t *)lv_event_get_user_data(event);
    if (zone) {
        if (timezone_value) lv_label_set_text(timezone_value, zone->label);
        qz_timezone_apply(zone->tz);
    }
    close_modal(event);
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
    lv_obj_set_size(timezone_list, lv_pct(100), 192);
    lv_obj_align(timezone_list, LV_ALIGN_TOP_LEFT, 0, 28);
    lv_obj_set_style_bg_opa(timezone_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(timezone_list, 0, 0);
    lv_obj_set_style_pad_all(timezone_list, 0, 0);
    lv_obj_set_style_pad_row(timezone_list, 2, 0);
    lv_obj_set_flex_flow(timezone_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(timezone_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(timezone_list, timezone_list_deleted, LV_EVENT_DELETE, NULL);

    lv_obj_t *actions = lv_obj_create(panel);
    lv_obj_set_size(actions, lv_pct(100), 40);
    lv_obj_align(actions, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(actions, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(actions, 0, 0);
    lv_obj_set_style_pad_all(actions, 0, 0);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);
    timezone_previous = qz_button(actions, "上一页", 100, 40);
    lv_obj_add_event_cb(timezone_previous, timezone_page_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    timezone_page_value = qz_text(actions, "1/3", 14, qz_color(QZ_TEXT_SECONDARY));
    timezone_next = qz_button(actions, "下一页", 100, 40);
    lv_obj_add_event_cb(timezone_next, timezone_page_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_t *cancel = qz_button(actions, "取消", 80, 40);
    lv_obj_add_event_cb(cancel, close_modal, LV_EVENT_CLICKED, NULL);

    timezone_page = 0;
    for (int i = 0; i < TIMEZONE_COUNT; i++) {
        if (timezone_value && strcmp(lv_label_get_text(timezone_value), timezones[i].label) == 0)
            timezone_page = i / TIMEZONE_PAGE_ROWS;
    }
    timezone_rebuild(NULL);
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

    /* Keep all three slider rows and all three settings inside the canvas.
     * In compact mode each settings row remains 24 actual pixels high. */
    const int settings_row_h = 32;
    lv_obj_t *card = section_card(general_screen, 52, 158, false);
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
    lv_obj_t *list_card = section_card(general_screen, 216, 3 * settings_row_h + 4, false);
    lv_obj_set_style_clip_corner(list_card, true, 0);
    info_row(list_card, 0, settings_row_h, "深色模式", qz_theme_is_dark() ? "开" : "关",
             &dark_mode_value);
    /* The switch already conveys the state, so reserve its full area. */
    lv_obj_add_flag(dark_mode_value, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *sep2 = qz_separator(list_card, CONTENT_W - 2 * CARD_PAD, false);
    lv_obj_align(sep2, LV_ALIGN_TOP_LEFT, CARD_PAD, settings_row_h);
    info_row(list_card, settings_row_h, settings_row_h, "时间", "10:30", &time_value);
    lv_obj_t *sep3 = qz_separator(list_card, CONTENT_W - 2 * CARD_PAD, false);
    lv_obj_align(sep3, LV_ALIGN_TOP_LEFT, CARD_PAD, 2 * settings_row_h);
    info_row(list_card, 2 * settings_row_h, settings_row_h, "时区", "(GMT+8) 北京", &timezone_value);

    /* Dark mode: a switch on the row, persisted the instant it moves. */
    lv_obj_t *dark_row = lv_obj_create(list_card);
    lv_obj_set_size(dark_row, lv_pct(100), settings_row_h);
    lv_obj_align(dark_row, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(dark_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dark_row, 0, 0);
    lv_obj_set_style_pad_all(dark_row, 0, 0);
    lv_obj_clear_flag(dark_row, LV_OBJ_FLAG_SCROLLABLE);
    dark_mode_switch = lv_switch_create(dark_row);
    qz_style_switch(dark_mode_switch);
    lv_obj_set_size(dark_mode_switch, 54, 26);
    lv_obj_align(dark_mode_switch, LV_ALIGN_RIGHT_MID, -CARD_PAD - 2, 0);
    if (qz_theme_is_dark()) lv_obj_add_state(dark_mode_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(dark_mode_switch, toggle_dark_mode, LV_EVENT_VALUE_CHANGED,
                        general_screen);

    /* Transparent overlays make the time/timezone rows tappable while keeping
     * the layout. */
    lv_obj_t *time_row = lv_obj_create(list_card);
    lv_obj_set_size(time_row, lv_pct(100), settings_row_h);
    lv_obj_align(time_row, LV_ALIGN_TOP_LEFT, 0, settings_row_h);
    lv_obj_set_style_bg_opa(time_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(time_row, 0, 0);
    lv_obj_set_style_pad_all(time_row, 0, 0);
    lv_obj_add_flag(time_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(time_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(time_row, open_time_modal, LV_EVENT_CLICKED, general_screen);
    lv_obj_t *time_chev = qz_chevron(time_row, qz_color(QZ_TEXT_TERTIARY), 14);
    lv_obj_align(time_chev, LV_ALIGN_RIGHT_MID, -CARD_PAD - 2, 0);

    lv_obj_t *zone_row = lv_obj_create(list_card);
    lv_obj_set_size(zone_row, lv_pct(100), settings_row_h);
    lv_obj_align(zone_row, LV_ALIGN_TOP_LEFT, 0, 2 * settings_row_h);
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

    lv_obj_t *hero = section_card(about_screen, 52, qz_compact() ? 50 : 72, false);
    lv_obj_t *face = qz_face_create(hero, qz_compact() ? 40 : 60);
    lv_obj_align(face, LV_ALIGN_LEFT_MID, 16, 0);
    qz_face_set_state(face, QZ_FACE_LOVE);
    lv_obj_t *name = qz_text(hero, "QZdesk", 17, qz_color(QZ_TEXT));
    lv_obj_align(name, LV_ALIGN_LEFT_MID, qz_compact() ? 66 : 88, -12);
    lv_obj_t *caption = qz_text(hero, "AI 桌面语音助手", 11, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(caption, LV_ALIGN_LEFT_MID, qz_compact() ? 66 : 88, 12);

    snprintf(about_server, sizeof(about_server), "未配置");
    qz_load_config(about_server, sizeof(about_server));
    char address[32];
    qz_device_ip(address, sizeof(address));

    const int device_row_h = 36;
    const int server_row_h = qz_compact() ? 96 : 72;
    lv_obj_t *card = section_card(about_screen, qz_compact() ? 108 : 132,
                                  3 * device_row_h + server_row_h + 4, false);
    lv_obj_set_style_clip_corner(card, true, 0);
    info_row(card, 0, device_row_h, "设备型号", "QZdesk Desk", NULL);
    info_row(card, device_row_h, device_row_h, "固件版本", "1.0.0", NULL);
    lv_obj_t *server_value;
    lv_obj_t *server_row = info_row(card, 2 * device_row_h, server_row_h,
                                    "服务地址", about_server, &server_value);
    lv_obj_align(lv_obj_get_child(server_row, 0), LV_ALIGN_TOP_LEFT, CARD_PAD, 6);
    lv_obj_set_width(server_value, CONTENT_W - 2 * CARD_PAD);
    lv_obj_set_style_text_align(server_value, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(server_value, LV_ALIGN_TOP_LEFT, CARD_PAD, 28);
    lv_obj_add_flag(server_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(server_row, open_server_modal, LV_EVENT_CLICKED, NULL);
    lv_point_t server_size;
    lv_text_get_size(&server_size, about_server, qz_font_size(12), 0, 0,
                     qz_scale_x(CONTENT_W - 2 * CARD_PAD), LV_TEXT_FLAG_NONE);
    if (server_size.y > qz_scale_y(server_row_h - 28) - 2)
        lv_label_set_text(server_value, "点按查看完整地址");
    info_row(card, 2 * device_row_h + server_row_h, device_row_h, "当前 IP", address, NULL);
    const int separator_y[] = { device_row_h, 2 * device_row_h,
                                2 * device_row_h + server_row_h };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *sep = qz_separator(card, CONTENT_W - 2 * CARD_PAD, false);
        lv_obj_align(sep, LV_ALIGN_TOP_LEFT, CARD_PAD, separator_y[i]);
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

    group_row(settings_screen, 52, 74, LV_SYMBOL_WIFI,
              QZ_ACCENT, QZ_TEXT_ON_ACCENT,
              "WLAN", " ", open_wlan, &wlan_row_summary);
    refresh_wlan_summary();

    group_row(settings_screen, 132, 74,
              LV_SYMBOL_SETTINGS, QZ_ACCENT_TINT,
              QZ_ACCENT_DARK, "通用设置", "声音 · 背光 · 时间", open_general, NULL);

    lv_obj_t *about_row = group_row(settings_screen, 212, 74, LV_SYMBOL_FILE,
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
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, qz_compact() ? -4 : -16);
    return settings_screen;
}

void qz_settings_set_desktop(lv_obj_t *desktop) { desktop_screen = desktop; }
