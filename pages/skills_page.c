#include "skills_page.h"
#include "skills.h"
#include "assistant.h"
#include "applets.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "theme.h"
#include "config.h"
#include "web_client.h"

/* Device side of the core Skill manager. The page is a compact control deck:
 * current runtime state at the top, two scannable Skill cards per page, and a
 * shared sheet language for details, companion abilities and records. Editing
 * and adding still belong to the browser console. */

#define CONTENT_W (QZ_DESIGN_W - 2 * QZ_GUTTER)
#define PANEL_PAD 14
#define COMPACT_SKILLS_PER_PAGE 2
#define SHEET_INFO_CHARS 40
#define OVERLAY_DIM 110

static lv_obj_t *skill_screen;
static lv_obj_t *apps_screen_ref;
static lv_obj_t *overview_card;
static lv_obj_t *overview_tile;
static lv_obj_t *overview_icon;
static lv_obj_t *overview_primary;
static lv_obj_t *overview_meta;
static lv_obj_t *list_card;
static lv_obj_t *service_dot;
static lv_obj_t *service_label;
static qz_skill_t skills[QZ_SKILL_MAX];
static lv_obj_t *list_page_label;
static lv_obj_t *list_previous;
static lv_obj_t *list_next;
static int list_page;
static int skill_count = -1;      /* -1 = the service could not be reached */
static bool role_just_changed;
static bool role_saving;
static bool skills_loading;
static bool reload_pending;
static bool animate_rows;
static int list_transition_direction;
static lv_obj_t *list_current_layer;

/* ------------------------------------------------------------------------- *
 * Shared surface and motion helpers
 * ------------------------------------------------------------------------- */

static void anim_translate_y(void *var, int32_t value)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, value, 0);
}

static void anim_translate_x(void *var, int32_t value)
{
    lv_obj_set_style_translate_x((lv_obj_t *)var, value, 0);
}

static void anim_opa(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

static void anim_bg_opa(void *var, int32_t value)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

static void layer_swap_delete_done(lv_anim_t *anim)
{
    lv_obj_t *layer = (lv_obj_t *)anim->var;
    if (layer) lv_obj_delete(layer);
}

static lv_obj_t *motion_layer_create(lv_obj_t *parent)
{
    lv_obj_t *layer = lv_obj_create(parent);
    lv_obj_set_size(layer, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(layer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(layer, 0, 0);
    lv_obj_set_style_pad_all(layer, 0, 0);
    lv_obj_clear_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(layer, LV_OBJ_FLAG_CLICKABLE);
    return layer;
}

/* Keep the semantic state on the final click, not on an animation callback.
 * The outgoing layer exists only as pixels and may be discarded at any time. */
static void motion_layer_swap(lv_obj_t *stage, lv_obj_t *old_layer, lv_obj_t *new_layer,
                              int direction)
{
    if (!old_layer) return;
    if (qz_reduce_motion()) {
        lv_obj_delete(old_layer);
        return;
    }

    lv_anim_delete(old_layer, NULL);
    lv_anim_delete(new_layer, NULL);

    int32_t offset = direction == 0 ? 0 : direction > 0 ? 18 : -18;
    lv_obj_set_style_translate_x(new_layer, offset, 0);
    lv_obj_set_style_opa(new_layer, LV_OPA_TRANSP, 0);

    lv_anim_t incoming_opa;
    lv_anim_init(&incoming_opa);
    lv_anim_set_var(&incoming_opa, new_layer);
    lv_anim_set_exec_cb(&incoming_opa, anim_opa);
    lv_anim_set_values(&incoming_opa, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&incoming_opa, QZ_DUR_SMALL);
    qz_anim_ease_out(&incoming_opa);
    lv_anim_start(&incoming_opa);

    if (offset != 0) {
        lv_anim_t incoming_x;
        lv_anim_init(&incoming_x);
        lv_anim_set_var(&incoming_x, new_layer);
        lv_anim_set_exec_cb(&incoming_x, anim_translate_x);
        lv_anim_set_values(&incoming_x, offset, 0);
        lv_anim_set_duration(&incoming_x, QZ_DUR_SMALL);
        qz_anim_ease_out(&incoming_x);
        lv_anim_start(&incoming_x);
    }

    lv_anim_t outgoing_opa;
    lv_anim_init(&outgoing_opa);
    lv_anim_set_var(&outgoing_opa, old_layer);
    lv_anim_set_exec_cb(&outgoing_opa, anim_opa);
    lv_anim_set_values(&outgoing_opa, lv_obj_get_style_opa(old_layer, 0), LV_OPA_TRANSP);
    lv_anim_set_duration(&outgoing_opa, QZ_DUR_QUICK);
    qz_anim_ease_in_out(&outgoing_opa);
    lv_anim_set_completed_cb(&outgoing_opa, layer_swap_delete_done);
    lv_anim_start(&outgoing_opa);

    if (offset != 0) {
        lv_anim_t outgoing_x;
        lv_anim_init(&outgoing_x);
        lv_anim_set_var(&outgoing_x, old_layer);
        lv_anim_set_exec_cb(&outgoing_x, anim_translate_x);
        lv_anim_set_values(&outgoing_x, lv_obj_get_style_translate_x(old_layer, 0),
                           -offset);
        lv_anim_set_duration(&outgoing_x, QZ_DUR_QUICK);
        qz_anim_ease_in_out(&outgoing_x);
        lv_anim_start(&outgoing_x);
    }

    (void)stage;
}

static void discard_other_layers(lv_obj_t *stage, lv_obj_t *keep)
{
    if (!stage) return;
    uint32_t count = lv_obj_get_child_count(stage);
    for (int i = (int)count - 1; i >= 0; i--) {
        lv_obj_t *child = lv_obj_get_child(stage, i);
        if (child == keep) continue;
        lv_anim_delete(child, NULL);
        lv_obj_delete(child);
    }
}

static void overlay_delete_done(lv_anim_t *anim)
{
    lv_obj_t *overlay = (lv_obj_t *)anim->var;
    if (overlay) lv_obj_delete(overlay);
}

static void overlay_animate_in(lv_obj_t *overlay, lv_obj_t *panel)
{
    if (!overlay || !panel || qz_reduce_motion()) return;

    lv_obj_set_style_bg_opa(overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_translate_y(panel, 24, 0);

    lv_anim_t slide;
    lv_anim_init(&slide);
    lv_anim_set_var(&slide, panel);
    lv_anim_set_exec_cb(&slide, anim_translate_y);
    lv_anim_set_values(&slide, 24, 0);
    lv_anim_set_duration(&slide, QZ_DUR_MODAL);
    qz_anim_ease_out(&slide);
    lv_anim_start(&slide);

    lv_anim_t scrim;
    lv_anim_init(&scrim);
    lv_anim_set_var(&scrim, overlay);
    lv_anim_set_exec_cb(&scrim, anim_bg_opa);
    lv_anim_set_values(&scrim, LV_OPA_TRANSP, OVERLAY_DIM);
    lv_anim_set_duration(&scrim, QZ_DUR_MODAL);
    qz_anim_ease_out(&scrim);
    lv_anim_start(&scrim);
}

static void overlay_animate_out(lv_obj_t *overlay, lv_obj_t *panel)
{
    if (!overlay) return;
    if (!panel || qz_reduce_motion()) {
        lv_obj_delete(overlay);
        return;
    }

    lv_anim_delete(overlay, NULL);
    lv_anim_delete(panel, NULL);
    lv_obj_set_style_translate_y(panel, 0, 0);

    lv_anim_t slide;
    lv_anim_init(&slide);
    lv_anim_set_var(&slide, panel);
    lv_anim_set_exec_cb(&slide, anim_translate_y);
    lv_anim_set_values(&slide, 0, 26);
    lv_anim_set_duration(&slide, QZ_DUR_PANEL);
    qz_anim_ease_out(&slide);
    lv_anim_start(&slide);

    lv_anim_t scrim;
    lv_anim_init(&scrim);
    lv_anim_set_var(&scrim, overlay);
    lv_anim_set_exec_cb(&scrim, anim_bg_opa);
    lv_anim_set_values(&scrim, lv_obj_get_style_bg_opa(overlay, 0), LV_OPA_TRANSP);
    lv_anim_set_duration(&scrim, QZ_DUR_PANEL);
    qz_anim_ease_out(&scrim);
    lv_anim_set_completed_cb(&scrim, overlay_delete_done);
    lv_anim_start(&scrim);
}

static lv_obj_t *icon_tile(lv_obj_t *parent, int size, qz_icon_t icon,
                           qz_color_token_t foreground, qz_color_token_t background)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_set_size(tile, size, size);
    lv_obj_set_style_radius(tile, 4, 0);
    qz_obj_set_bg_color(tile, background, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *glyph = qz_icon_image(tile, icon, 20, qz_color(foreground));
    if (glyph) lv_obj_center(glyph);
    return tile;
}

static lv_obj_t *role_pill(lv_obj_t *parent, qz_skill_role_t role)
{
    qz_color_token_t background = role == QZ_SKILL_ROLE_PRIMARY ? QZ_ACCENT
                                : role == QZ_SKILL_ROLE_SECONDARY ? QZ_ACCENT_TINT
                                                                  : QZ_FILL_PRESSED;
    qz_color_token_t foreground = role == QZ_SKILL_ROLE_PRIMARY ? QZ_TEXT_ON_ACCENT
                                : role == QZ_SKILL_ROLE_SECONDARY ? QZ_ACCENT_TEXT
                                                                  : QZ_TEXT_SECONDARY;
    lv_obj_t *pill = lv_obj_create(parent);
    lv_obj_set_size(pill, 62, 20);
    lv_obj_set_style_radius(pill, 3, 0);
    qz_obj_set_bg_color(pill, background, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pill, 1, 0);
    qz_obj_set_border_color(pill, role == QZ_SKILL_ROLE_PRIMARY ? QZ_ACCENT_DARK
                                                               : QZ_SEPARATOR, 0);
    lv_obj_set_style_border_opa(pill, (lv_opa_t)140, 0);
    lv_obj_set_style_pad_all(pill, 0, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *label = qz_text(pill, qz_skills_role_label(role), 10,
                              qz_color(foreground));
    lv_obj_center(label);
    return pill;
}

static void set_service_dot(qz_color_token_t token)
{
    if (service_dot) qz_obj_set_bg_color(service_dot, token, 0);
}

static void set_service_text(const char *text, qz_color_token_t token)
{
    if (!service_label) return;
    lv_label_set_text(service_label, text);
    qz_obj_set_text_color(service_label, token, 0);
}

/* ------------------------------------------------------------------------- *
 * Skill details
 * ------------------------------------------------------------------------- */

static lv_obj_t *sheet_overlay;
static lv_obj_t *sheet_title;
static lv_obj_t *sheet_description;
static lv_obj_t *sheet_info_page_label;
static lv_obj_t *sheet_feedback;
static lv_obj_t *sheet_role_buttons[3];
static int sheet_info_page;
static int sheet_skill_index;

/* Split at UTF-8 boundaries so every description remains readable, including
 * long descriptions supplied by the service. */
static int description_chunk(const char *source, int page, char *out, size_t size)
{
    const char *start = source;
    const char *cursor = source;
    int characters = 0;
    int pages = 1;
    while (*cursor) {
        if (characters && characters % SHEET_INFO_CHARS == 0) {
            pages++;
            if (pages - 1 == page) start = cursor;
        }
        cursor++;
        while ((*cursor & 0xc0) == 0x80) cursor++;
        characters++;
    }
    cursor = start;
    for (int i = 0; *cursor && i < SHEET_INFO_CHARS; i++) {
        cursor++;
        while ((*cursor & 0xc0) == 0x80) cursor++;
    }
    size_t length = (size_t)(cursor - start);
    if (length >= size) length = size - 1;
    memcpy(out, start, length);
    out[length] = '\0';
    return pages;
}

static void update_description_page_indicator(int pages)
{
    if (!sheet_info_page_label) return;
    char text[32];
    snprintf(text, sizeof(text), "%d / %d", sheet_info_page + 1, pages);
    lv_label_set_text(sheet_info_page_label, text);
}

static void sheet_info_changed(lv_event_t *event)
{
    if (sheet_skill_index < 0 || sheet_skill_index >= skill_count || !sheet_description) return;
    const qz_skill_t *skill = &skills[sheet_skill_index];
    const char *description = skill->description[0] ? skill->description : "未填写说明";
    char text[QZ_SKILL_DESC_MAX];
    int pages = description_chunk(description, 0, text, sizeof(text));
    int direction = (int)(intptr_t)lv_event_get_user_data(event);
    sheet_info_page = (sheet_info_page + direction + pages) % pages;
    description_chunk(description, sheet_info_page, text, sizeof(text));
    lv_label_set_text(sheet_description, text);
    update_description_page_indicator(pages);
    qz_animate_pop_in(sheet_description);
}

static void set_role_buttons_enabled(bool enabled)
{
    for (int i = 0; i < 3; i++) {
        if (!sheet_role_buttons[i]) continue;
        if (enabled) lv_obj_remove_state(sheet_role_buttons[i], LV_STATE_DISABLED);
        else lv_obj_add_state(sheet_role_buttons[i], LV_STATE_DISABLED);
        lv_obj_set_style_opa(sheet_role_buttons[i], enabled ? LV_OPA_COVER : (lv_opa_t)150, 0);
    }
}

static void close_sheet(void)
{
    if (role_saving || !sheet_overlay) return;
    lv_obj_t *overlay = sheet_overlay;
    lv_obj_t *panel = lv_obj_get_child(overlay, 0);
    sheet_overlay = NULL;
    sheet_title = NULL;
    sheet_description = NULL;
    sheet_info_page_label = NULL;
    sheet_feedback = NULL;
    memset(sheet_role_buttons, 0, sizeof(sheet_role_buttons));
    overlay_animate_out(overlay, panel);
}

static void sheet_cancel(lv_event_t *event)
{
    (void)event;
    close_sheet();
}

static void sheet_scrim_clicked(lv_event_t *event)
{
    if (lv_event_get_target(event) != lv_event_get_current_target(event)) return;
    close_sheet();
}

static void reload(void);

static void role_saved(bool ok, const char *body, void *data)
{
    (void)body;
    (void)data;
    role_saving = false;
    if (!sheet_overlay) return;
    set_role_buttons_enabled(true);
    if (!ok) {
        if (sheet_feedback) {
            lv_label_set_text(sheet_feedback, "保存失败，请重试\n角色没有改变");
            qz_obj_set_text_color(sheet_feedback, QZ_RED, 0);
        }
        return;
    }
    role_just_changed = true;
    close_sheet();
    reload();
}

static void sheet_pick_role(lv_event_t *event)
{
    if (role_saving) return;
    const void *data = lv_event_get_user_data(event);
    int packed = (int)(intptr_t)data;
    int index = packed >> 2;
    qz_skill_role_t role = (qz_skill_role_t)(packed & 0x3);
    if (index < 0 || index >= skill_count) return;
    if (skills[index].role == role) {
        if (sheet_feedback) {
            lv_label_set_text(sheet_feedback, "这已经是当前角色");
            qz_obj_set_text_color(sheet_feedback, QZ_TEXT_SECONDARY, 0);
        }
        return;
    }

    char path[160];
    char body[64];
    snprintf(path, sizeof(path), "/api/skills/%s/selection", skills[index].id);
    snprintf(body, sizeof(body), "{\"role\":\"%s\"}", qz_skills_role_key(role));
    role_saving = qz_web_request_async("PUT", path, body, role_saved, NULL);
    if (!role_saving) {
        if (sheet_feedback) {
            lv_label_set_text(sheet_feedback, "无法保存，请重试");
            qz_obj_set_text_color(sheet_feedback, QZ_RED, 0);
        }
        return;
    }
    set_role_buttons_enabled(false);
    if (sheet_feedback) {
        lv_label_set_text(sheet_feedback, "正在保存角色…");
        qz_obj_set_text_color(sheet_feedback, QZ_ACCENT_TEXT, 0);
    }
}

static void open_sheet(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (index < 0 || index >= skill_count || sheet_overlay) return;

    const qz_skill_t *skill = &skills[index];
    sheet_skill_index = index;
    sheet_info_page = 0;
    memset(sheet_role_buttons, 0, sizeof(sheet_role_buttons));

    sheet_overlay = qz_overlay_create(skill_screen);
    lv_obj_add_event_cb(sheet_overlay, sheet_scrim_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(sheet_overlay);
    lv_obj_set_size(panel, QZ_DESIGN_W - 40, 300);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 10);
    qz_style_sheet(panel);
    lv_obj_set_style_pad_all(panel, PANEL_PAD, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    qz_color_token_t tile_fg = skill->role == QZ_SKILL_ROLE_NONE ? QZ_TEXT_TERTIARY
                                                                 : QZ_ACCENT_TEXT;
    qz_icon_t tile_icon = skill->role == QZ_SKILL_ROLE_PRIMARY ? QZ_ICON_SPARKLES
                         : skill->role == QZ_SKILL_ROLE_SECONDARY ? QZ_ICON_BULB
                                                                  : QZ_ICON_PLUG;
    lv_obj_t *tile = icon_tile(panel, 38, tile_icon, tile_fg, QZ_ACCENT_TINT);
    lv_obj_align(tile, LV_ALIGN_TOP_LEFT, 0, 0);

    sheet_title = qz_text(panel, skill->name, 17, qz_color(QZ_TEXT));
    lv_obj_set_size(sheet_title, 250, 24);
    lv_label_set_long_mode(sheet_title, LV_LABEL_LONG_DOT);
    lv_obj_align(sheet_title, LV_ALIGN_TOP_LEFT, 50, 1);

    lv_obj_t *kicker = qz_text(panel, "角色与说明", 10, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(kicker, LV_ALIGN_TOP_LEFT, 50, 25);

    lv_obj_t *pill = role_pill(panel, skill->role);
    lv_obj_align(pill, LV_ALIGN_TOP_RIGHT, 0, 5);

    lv_obj_t *description_well = lv_obj_create(panel);
    lv_obj_set_size(description_well, 412, 84);
    lv_obj_align(description_well, LV_ALIGN_TOP_LEFT, 0, 50);
    lv_obj_set_style_radius(description_well, 4, 0);
    qz_obj_set_bg_color(description_well, QZ_FILL, 0);
    lv_obj_set_style_bg_opa(description_well, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(description_well, 0, 0);
    lv_obj_set_style_pad_all(description_well, 0, 0);
    lv_obj_clear_flag(description_well, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(description_well, LV_OBJ_FLAG_CLICKABLE);

    const char *description = skill->description[0] ? skill->description : "未填写说明";
    char chunk[QZ_SKILL_DESC_MAX];
    int pages = description_chunk(description, 0, chunk, sizeof(chunk));
    sheet_description = qz_text(description_well, chunk, 12, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_set_size(sheet_description, 392, 45);
    lv_label_set_long_mode(sheet_description, LV_LABEL_LONG_WRAP);
    lv_obj_align(sheet_description, LV_ALIGN_TOP_LEFT, 10, 6);

    if (pages > 1) {
        lv_obj_t *previous = qz_button(description_well, "上段说明", 88, 24);
        lv_obj_align(previous, LV_ALIGN_TOP_LEFT, 10, 54);
        lv_obj_add_event_cb(previous, sheet_info_changed, LV_EVENT_CLICKED,
                            (void *)(intptr_t)-1);

        lv_obj_t *next = qz_button(description_well, "下段说明", 88, 24);
        lv_obj_align(next, LV_ALIGN_TOP_RIGHT, -10, 54);
        lv_obj_add_event_cb(next, sheet_info_changed, LV_EVENT_CLICKED,
                            (void *)(intptr_t)1);

        sheet_info_page_label = qz_text(description_well, "", 10,
                                        qz_color(QZ_TEXT_TERTIARY));
        lv_obj_align(sheet_info_page_label, LV_ALIGN_TOP_MID, 0, 59);
        update_description_page_indicator(pages);
    }

    lv_obj_t *role_heading = qz_text(panel, "切换角色", 11, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(role_heading, LV_ALIGN_TOP_LEFT, 0, 146);

    const qz_skill_role_t roles[] = {
        QZ_SKILL_ROLE_PRIMARY, QZ_SKILL_ROLE_SECONDARY, QZ_SKILL_ROLE_NONE,
    };
    const char *labels[] = { "主技能", "备用", "关闭" };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *choice = qz_button(panel, labels[i], 128, 38);
        lv_obj_align(choice, LV_ALIGN_TOP_LEFT, i * 142, 168);
        sheet_role_buttons[i] = choice;
        if (skill->role == roles[i]) {
            qz_style_primary_button(choice);
            lv_obj_t *caption = lv_obj_get_child(choice, 0);
            if (caption) qz_obj_set_text_color(caption, QZ_TEXT_ON_ACCENT, 0);
        }
        lv_obj_add_event_cb(choice, sheet_pick_role, LV_EVENT_CLICKED,
                            (void *)(intptr_t)((index << 2) | (int)roles[i]));
    }

    sheet_feedback = qz_text(panel,
                             "主技能直接生效，备用按需检索\n切换后下一条消息重建会话",
                             11, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_set_size(sheet_feedback, 270, 42);
    lv_label_set_long_mode(sheet_feedback, LV_LABEL_LONG_WRAP);
    lv_obj_align(sheet_feedback, LV_ALIGN_TOP_LEFT, 0, 216);

    lv_obj_t *cancel = qz_button(panel, "取消", 100, 34);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(cancel, sheet_cancel, LV_EVENT_CLICKED, NULL);

    overlay_animate_in(sheet_overlay, panel);
}

/* ------------------------------------------------------------------------- *
 * Skill list
 * ------------------------------------------------------------------------- */

static void rebuild(void);

static void empty_retry_clicked(lv_event_t *event)
{
    (void)event;
    reload();
}

static void list_page_changed(lv_event_t *event)
{
    int direction = (int)(intptr_t)lv_event_get_user_data(event);
    list_page += direction;
    list_transition_direction = direction;
    animate_rows = true;
    rebuild();
}

static void create_skill_card(lv_obj_t *parent, int index, int y)
{
    const qz_skill_t *skill = &skills[index];
    lv_obj_t *card = qz_card_button(parent, CONTENT_W, 53);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_add_event_cb(card, open_sheet, LV_EVENT_CLICKED, (void *)(intptr_t)index);

    qz_color_token_t rail = skill->role == QZ_SKILL_ROLE_PRIMARY ? QZ_ACCENT
                           : skill->role == QZ_SKILL_ROLE_SECONDARY ? QZ_ACCENT_DARK
                                                                    : QZ_SEPARATOR;
    lv_obj_t *rail_obj = lv_obj_create(card);
    lv_obj_set_size(rail_obj, 3, 33);
    lv_obj_align(rail_obj, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(rail_obj, 2, 0);
    qz_obj_set_bg_color(rail_obj, rail, 0);
    lv_obj_set_style_bg_opa(rail_obj,
                            skill->role == QZ_SKILL_ROLE_SECONDARY ? (lv_opa_t)155
                                                                   : LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rail_obj, 0, 0);
    lv_obj_clear_flag(rail_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(rail_obj, LV_OBJ_FLAG_SCROLLABLE);

    qz_icon_t icon = skill->role == QZ_SKILL_ROLE_PRIMARY ? QZ_ICON_SPARKLES
                   : skill->role == QZ_SKILL_ROLE_SECONDARY ? QZ_ICON_BULB
                                                            : QZ_ICON_PLUG;
    qz_color_token_t icon_color = skill->role == QZ_SKILL_ROLE_NONE ? QZ_TEXT_TERTIARY
                                                                   : QZ_ACCENT_TEXT;
    lv_obj_t *tile = icon_tile(card, 36, icon, icon_color, QZ_ACCENT_TINT);
    lv_obj_align(tile, LV_ALIGN_LEFT_MID, 8, 0);

    lv_obj_t *name = qz_text(card, skill->name, 15, qz_color(QZ_TEXT));
    lv_obj_set_size(name, 294, 22);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 54, 7);

    lv_obj_t *description = qz_text(card,
                                    skill->description[0] ? skill->description : "暂无说明",
                                    10, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_set_size(description, 294, 16);
    lv_label_set_long_mode(description, LV_LABEL_LONG_DOT);
    lv_obj_align(description, LV_ALIGN_TOP_LEFT, 54, 30);

    lv_obj_t *pill = role_pill(card, skill->role);
    lv_obj_align(pill, LV_ALIGN_TOP_RIGHT, -10, 8);

    lv_obj_t *chev = qz_chevron(card, qz_color(QZ_TEXT_TERTIARY), 13);
    lv_obj_align(chev, LV_ALIGN_BOTTOM_RIGHT, -10, -4);

    if (animate_rows) qz_animate_entrance(card, (uint32_t)(y ? 34 : 0));
}

static void rebuild(void)
{
    if (!list_card) return;
    lv_obj_t *old_layer = list_current_layer;
    bool animate = animate_rows && !qz_reduce_motion();
    if (old_layer) {
        lv_anim_delete(old_layer, NULL);
        discard_other_layers(list_card, old_layer);
    } else {
        discard_other_layers(list_card, NULL);
    }

    lv_obj_t *layer = motion_layer_create(list_card);
    list_current_layer = layer;
    int pages = skill_count > 0 ? (skill_count + COMPACT_SKILLS_PER_PAGE - 1) /
                                  COMPACT_SKILLS_PER_PAGE : 1;
    if (list_page >= pages) list_page = pages - 1;
    if (list_page < 0) list_page = 0;

    if (list_page_label) {
        char page_text[32];
        snprintf(page_text, sizeof(page_text), "%d / %d", list_page + 1, pages);
        lv_label_set_text(list_page_label, page_text);
        if (pages == 1) {
            lv_obj_add_flag(list_previous, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(list_next, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(list_page_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(list_previous, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(list_next, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(list_page_label, LV_OBJ_FLAG_HIDDEN);
        }
        if (list_page == 0) lv_obj_add_state(list_previous, LV_STATE_DISABLED);
        else lv_obj_remove_state(list_previous, LV_STATE_DISABLED);
        if (list_page == pages - 1) lv_obj_add_state(list_next, LV_STATE_DISABLED);
        else lv_obj_remove_state(list_next, LV_STATE_DISABLED);
    }

    if (skill_count <= 0) {
        lv_obj_t *box = lv_obj_create(layer);
        lv_obj_set_size(box, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_pad_all(box, 0, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *icon = qz_icon_image(box,
                                       skill_count < 0 ? QZ_ICON_PLUG : QZ_ICON_SPARKLES,
                                       32, qz_color(QZ_TEXT_TERTIARY));
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, -34);
        const char *title_text = skill_count < 0 ? "本地服务未连接" : "还没有安装 Skill";
        lv_obj_t *title = qz_text(box, title_text, 15, qz_color(QZ_TEXT));
        lv_obj_align(title, LV_ALIGN_CENTER, 0, -4);
        const char *hint_text = skill_count < 0 ? "请确认 QZdesk 核心正在运行"
                                               : "从浏览器控制台添加，设备端随开随用";
        lv_obj_t *hint = qz_text(box, hint_text, 10, qz_color(QZ_TEXT_TERTIARY));
        lv_obj_align(hint, LV_ALIGN_CENTER, 0, 18);
        lv_obj_t *retry = qz_button(box, "重试", 106, 32);
        lv_obj_align(retry, LV_ALIGN_CENTER, 0, 39);
        lv_obj_add_event_cb(retry, empty_retry_clicked, LV_EVENT_CLICKED, NULL);
        if (animate && !old_layer) qz_animate_entrance(box, 0);
        if (old_layer) motion_layer_swap(list_card, old_layer, layer, list_transition_direction);
        else if (!animate) lv_obj_set_style_opa(layer, LV_OPA_COVER, 0);
        animate_rows = false;
        list_transition_direction = 0;
        return;
    }

    int first = list_page * COMPACT_SKILLS_PER_PAGE;
    int end = first + COMPACT_SKILLS_PER_PAGE;
    if (end > skill_count) end = skill_count;
    for (int i = first; i < end; i++)
        create_skill_card(layer, i, (i - first) * 59);
    if (old_layer) motion_layer_swap(list_card, old_layer, layer, list_transition_direction);
    else if (!animate) lv_obj_set_style_opa(layer, LV_OPA_COVER, 0);
    animate_rows = false;
    list_transition_direction = 0;
}

static void skills_loaded(bool ok, const char *body, void *data)
{
    (void)data;
    skills_loading = false;
    bool refresh_after = reload_pending;
    reload_pending = false;
    qz_skill_summary_t summary = {0};
    int count = ok ? qz_skills_parse(body, skills, QZ_SKILL_MAX, &summary) : -1;
    skill_count = count;

    if (count < 0) {
        if (overview_primary) {
            lv_label_set_text(overview_primary, "本地服务未连接");
            qz_obj_set_text_color(overview_primary, QZ_TEXT_SECONDARY, 0);
        }
        if (overview_meta) lv_label_set_text(overview_meta, "无法读取技能状态");
        qz_obj_set_bg_color(overview_tile, QZ_FILL, 0);
        if (overview_icon) qz_obj_set_image_recolor(overview_icon, QZ_TEXT_TERTIARY, 0);
        set_service_text("本地 Skill 服务未响应", QZ_TEXT_TERTIARY);
        set_service_dot(QZ_ORANGE);
    } else {
        const char *primary_name = NULL;
        for (int i = 0; i < count; i++) {
            if (skills[i].role == QZ_SKILL_ROLE_PRIMARY) {
                primary_name = skills[i].name;
                break;
            }
        }
        if (overview_primary) {
            lv_label_set_text(overview_primary, primary_name ? primary_name : "未设置主技能");
            qz_obj_set_text_color(overview_primary,
                                  primary_name ? QZ_TEXT : QZ_TEXT_SECONDARY, 0);
        }
        if (overview_meta) {
            char meta[64];
            snprintf(meta, sizeof(meta), "主技能 %d · 备用 %d · 共 %d",
                     summary.primary, summary.secondary, summary.count);
            lv_label_set_text(overview_meta, meta);
        }
        qz_obj_set_bg_color(overview_tile, QZ_ACCENT_TINT, 0);
        if (overview_icon) qz_obj_set_image_recolor(overview_icon, QZ_ACCENT_TEXT, 0);
        set_service_dot(primary_name ? QZ_GREEN : QZ_ORANGE);

        if (role_just_changed) {
            set_service_text("选择已保存，当前对话结束后同步", QZ_ACCENT_TEXT);
            role_just_changed = false;
        } else if (!primary_name) {
            set_service_text("没有主技能，对话不会加载指令", QZ_ORANGE);
        } else if (count == 0) {
            char address[32];
            qz_device_ip(address, sizeof(address));
            char text[96];
            snprintf(text, sizeof(text), "浏览器访问 %s:%u 添加技能",
                     address, qz_web_port());
            set_service_text(text, QZ_TEXT_TERTIARY);
        } else {
            char address[32];
            qz_device_ip(address, sizeof(address));
            char text[96];
            snprintf(text, sizeof(text), "浏览器访问 %s:%u 编辑或新增技能",
                     address, qz_web_port());
            set_service_text(text, QZ_TEXT_TERTIARY);
        }
    }
    rebuild();
    if (refresh_after) reload();
}

static void reload(void)
{
    if (skills_loading) {
        reload_pending = true;
        return;
    }
    animate_rows = true;
    set_service_text("正在读取技能服务", QZ_TEXT_TERTIARY);
    set_service_dot(QZ_ACCENT);
    skills_loading = qz_web_request_async("GET", "/api/skills", NULL,
                                          skills_loaded, NULL);
    if (!skills_loading) {
        reload_pending = false;
        set_service_text("无法加载，请重试", QZ_TEXT_TERTIARY);
        set_service_dot(QZ_ORANGE);
        animate_rows = false;
    }
}

static void refresh_clicked(lv_event_t *event)
{
    (void)event;
    reload();
}

static void screen_loaded(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_SCREEN_LOADED) return;
    qz_animate_entrance(overview_card, 0);
    qz_animate_entrance(list_previous, 30);
    qz_animate_entrance(list_next, 30);
    reload();
}

static void back_clicked(lv_event_t *event)
{
    (void)event;
    qz_screen_load(apps_screen_ref, LV_SCR_LOAD_ANIM_MOVE_RIGHT, QZ_DUR_SCREEN);
}

/* ------------------------------------------------------------------------- *
 * Companion sheet
 * ------------------------------------------------------------------------- */

static lv_obj_t *companion_scrim;
static lv_obj_t *companion_panel;
static lv_obj_t *companion_hint;
static lv_obj_t *companion_caption;
static lv_obj_t *companion_grid;
static lv_obj_t *companion_current_layer;
static int companion_page;
static int companion_transition_direction;

static const char *companion_names[8] = {
    "个人记忆", "情绪陪伴", "主动关心", "专注搭子",
    "学习教练", "日常计划", "睡前故事", "设备管家",
};
static const char *companion_prompts[8] = {
    "查看我的个人记忆，并说明如何修改或忘记",
    "我想倾诉一下，请先听我说",
    "查询主动关心设置，帮我设置频率和安静时段；开启前请确认",
    "帮我确定本轮专注目标，再确认番茄钟参数",
    "帮我制定学习目标，先了解我的基础",
    "查看我的待办，帮我安排今天的重点",
    "给我讲一个三分钟的睡前故事",
    "查看设备状态，先不要执行控制",
};
static const char *companion_descriptions[8] = {
    "查看与整理长期偏好", "先倾听，再回应", "频率与安静时段", "目标与专注计时",
    "按基础制定计划", "安排今天的重点", "生成一段睡前故事", "只查看，不直接控制",
};
static const qz_icon_t companion_icons[8] = {
    QZ_ICON_DATABASE, QZ_ICON_USER, QZ_ICON_BELL, QZ_ICON_TIMER,
    QZ_ICON_BULB, QZ_ICON_LIST, QZ_ICON_FILE_TEXT, QZ_ICON_CPU,
};

static void companion_close(lv_event_t *event)
{
    (void)event;
    if (!companion_scrim) return;
    lv_obj_t *overlay = companion_scrim;
    lv_obj_t *panel = companion_panel;
    companion_scrim = NULL;
    companion_panel = NULL;
    companion_hint = NULL;
    companion_caption = NULL;
    companion_grid = NULL;
    companion_current_layer = NULL;
    overlay_animate_out(overlay, panel);
}

static void records_open(const char *kind);
static void companion_pick(lv_event_t *event);

static void companion_render(void)
{
    if (!companion_grid) return;
    lv_obj_t *old_layer = companion_current_layer;
    if (old_layer) {
        lv_anim_delete(old_layer, NULL);
        discard_other_layers(companion_grid, old_layer);
    } else {
        discard_other_layers(companion_grid, NULL);
    }

    lv_obj_t *layer = motion_layer_create(companion_grid);
    companion_current_layer = layer;
    for (int i = 0; i < 4; i++) {
        int index = companion_page * 4 + i;
        lv_obj_t *card = qz_card_button(layer, 204, 52);
        lv_obj_align(card, LV_ALIGN_TOP_LEFT, (i % 2) * 216, (i / 2) * 62);
        lv_obj_add_event_cb(card, companion_pick, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        lv_obj_t *tile = icon_tile(card, 30, companion_icons[index],
                                   QZ_ACCENT_TEXT, QZ_ACCENT_TINT);
        lv_obj_align(tile, LV_ALIGN_LEFT_MID, 8, 0);

        lv_obj_t *title = qz_text(card, companion_names[index], 14, qz_color(QZ_TEXT));
        lv_obj_set_size(title, 150, 20);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, 46, 7);

        lv_obj_t *description = qz_text(card, companion_descriptions[index], 10,
                                        qz_color(QZ_TEXT_TERTIARY));
        lv_obj_set_size(description, 150, 16);
        lv_label_set_long_mode(description, LV_LABEL_LONG_DOT);
        lv_obj_align(description, LV_ALIGN_TOP_LEFT, 46, 28);
    }
    if (old_layer)
        motion_layer_swap(companion_grid, old_layer, layer, companion_transition_direction);
    else
        lv_obj_set_style_opa(layer, LV_OPA_COVER, 0);
    companion_transition_direction = 0;
}

static void companion_page_changed(lv_event_t *event)
{
    (void)event;
    companion_page = 1 - companion_page;
    companion_transition_direction = companion_page == 1 ? 1 : -1;
    if (companion_caption) {
        char text[32];
        snprintf(text, sizeof(text), "%d / 2", companion_page + 1);
        lv_label_set_text(companion_caption, text);
    }
    companion_render();
}

static void companion_pick(lv_event_t *event)
{
    int index = companion_page * 4 + (int)(intptr_t)lv_event_get_user_data(event);
    if (index == 0 || index == 2 || index == 4 || index == 5) {
        records_open(index == 0 ? "memory" : index == 2 ? "care"
                        : index == 4 ? "learning" : "todo");
        return;
    }
    if (index == 3 || index == 7) {
        lv_obj_t *target = qz_applets_screen(index == 3 ? QZ_APPLET_POMODORO
                                                        : QZ_APPLET_CONTROL);
        if (!target) {
            if (companion_hint) lv_label_set_text(companion_hint, "设备页面尚未初始化");
            return;
        }
        companion_close(NULL);
        qz_screen_load(target, LV_SCR_LOAD_ANIM_MOVE_LEFT, QZ_DUR_SCREEN);
        return;
    }
    if (qz_assistant_prepare_text(companion_prompts[index])) {
        companion_close(NULL);
    } else if (companion_hint) {
        lv_label_set_text(companion_hint, "请先处理聊天草稿或当前录音");
    }
}

static void companion_open(lv_event_t *event)
{
    (void)event;
    if (companion_scrim) return;
    companion_page = 0;
    companion_current_layer = NULL;
    companion_transition_direction = 0;
    companion_scrim = qz_overlay_create(lv_layer_top());
    lv_obj_add_event_cb(companion_scrim, companion_close, LV_EVENT_CLICKED, NULL);

    companion_panel = lv_obj_create(companion_scrim);
    lv_obj_set_size(companion_panel, 448, 272);
    lv_obj_align(companion_panel, LV_ALIGN_TOP_MID, 0, 24);
    qz_style_sheet(companion_panel);
    lv_obj_set_style_pad_all(companion_panel, PANEL_PAD, 0);
    lv_obj_clear_flag(companion_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tile = icon_tile(companion_panel, 34, QZ_ICON_SPARKLES,
                               QZ_ACCENT_TEXT, QZ_ACCENT_TINT);
    lv_obj_align(tile, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *title = qz_text(companion_panel, "陪伴能力", 17, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 44, 0);

    companion_caption = qz_text(companion_panel, "1 / 2", 12,
                                qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(companion_caption, LV_ALIGN_TOP_RIGHT, 0, 3);

    companion_hint = qz_text(companion_panel, "沿用主人格，选择后进入输入框", 10,
                             qz_color(QZ_TEXT_TERTIARY));
    lv_obj_set_size(companion_hint, 372, 16);
    lv_label_set_long_mode(companion_hint, LV_LABEL_LONG_DOT);
    lv_obj_align(companion_hint, LV_ALIGN_TOP_LEFT, 44, 22);

    companion_grid = lv_obj_create(companion_panel);
    lv_obj_set_size(companion_grid, 420, 116);
    lv_obj_align(companion_grid, LV_ALIGN_TOP_LEFT, 0, 54);
    lv_obj_set_style_bg_opa(companion_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(companion_grid, 0, 0);
    lv_obj_set_style_pad_all(companion_grid, 0, 0);
    lv_obj_clear_flag(companion_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(companion_grid, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *close = qz_button(companion_panel, "关闭", 96, 36);
    lv_obj_align(close, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(close, companion_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t *next = qz_button(companion_panel, "换一页", 108, 36);
    lv_obj_align(next, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(next, companion_page_changed, LV_EVENT_CLICKED, NULL);

    companion_render();
    overlay_animate_in(companion_scrim, companion_panel);
}

/* ------------------------------------------------------------------------- *
 * Records and care sheet
 * ------------------------------------------------------------------------- */

static lv_obj_t *records_scrim;
static lv_obj_t *records_panel;
static lv_obj_t *records_label;
static lv_obj_t *records_input;
static lv_obj_t *records_keyboard;
static lv_obj_t *records_status;
static lv_obj_t *record_controls[6];
static lv_obj_t *record_chip_row;
static const char *record_kind;
static int record_page;
static int record_id;
static int record_count;
static bool record_done;
static bool record_busy;
static bool care_enabled;
static lv_obj_t *care_inputs[3];
static bool care_mode;

static void records_request(const char *action);

static void set_records_status(const char *text, bool error)
{
    if (!records_status) return;
    lv_label_set_text(records_status, text);
    qz_obj_set_text_color(records_status, error ? QZ_RED : QZ_TEXT_SECONDARY, 0);
}

static void records_close(lv_event_t *event)
{
    (void)event;
    if (record_busy || !records_scrim) return;
    if (records_keyboard) lv_obj_delete(records_keyboard);
    records_keyboard = NULL;
    lv_obj_t *overlay = records_scrim;
    lv_obj_t *panel = records_panel;
    records_scrim = NULL;
    records_panel = NULL;
    records_label = NULL;
    records_input = NULL;
    records_status = NULL;
    record_chip_row = NULL;
    memset(record_controls, 0, sizeof(record_controls));
    memset(care_inputs, 0, sizeof(care_inputs));
    overlay_animate_out(overlay, panel);
}

static void records_received(bool ok, const char *response, void *data)
{
    (void)data;
    record_busy = false;
    if (!records_panel) return;
    for (int i = 0; i < 6; i++) {
        if (record_controls[i])
            lv_obj_remove_state(record_controls[i], LV_STATE_DISABLED);
    }
    if (!ok) {
        char error[128];
        if (!qz_json_string(response, "error", error, sizeof(error)))
            snprintf(error, sizeof(error), "核心未连接，操作未确认，请重试");
        set_records_status(error, true);
        return;
    }
    if (care_mode) {
        const char *care = qz_json_value(response, "care");
        int values[3];
        if (!care || !qz_json_bool(care, "enabled", &care_enabled) ||
            !qz_json_int(care, "interval_minutes", &values[0]) ||
            !qz_json_int(care, "quiet_start", &values[1]) ||
            !qz_json_int(care, "quiet_end", &values[2])) {
            set_records_status("核心响应不完整，设置未确认", true);
            return;
        }
        for (int i = 0; i < 3; i++) {
            char value[12];
            snprintf(value, sizeof(value), "%d", values[i]);
            if (care_inputs[i]) lv_textarea_set_text(care_inputs[i], value);
        }
        if (records_label)
            lv_label_set_text(records_label,
                              care_enabled ? "主动关心：已开启" : "主动关心：已关闭");
        set_records_status("已读取核心设置；安静时段按设备时区", false);
        return;
    }

    const char *records = qz_json_value(response, "records");
    if (!records) {
        set_records_status("核心响应不完整，记录未确认", true);
        return;
    }
    record_count = 0;
    while (qz_json_item(response, "records", record_count)) record_count++;
    if (record_chip_row) {
        if (record_count == 0) lv_obj_clear_flag(record_chip_row, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(record_chip_row, LV_OBJ_FLAG_HIDDEN);
    }
    while (record_page > 0 && !qz_json_item(response, "records", record_page)) record_page--;
    const char *item = qz_json_item(response, "records", record_page);
    record_id = 0;
    record_done = false;
    if (item && records_input) lv_textarea_set_text(records_input, "");
    if (item) {
        char text[8192];
        if (!qz_json_int(item, "id", &record_id) ||
            !qz_json_string(item, "text", text, sizeof(text))) {
            set_records_status("记录响应无效", true);
            return;
        }
        qz_json_bool(item, "done", &record_done);
        if (records_label) lv_label_set_text(records_label, text);
        if (records_input) lv_textarea_set_text(records_input, text);
    } else if (records_label) {
        lv_label_set_text(records_label, "暂无记录，点建议或输入后新增");
    }
    set_records_status(record_done ? "已完成；可点切换恢复待办"
                                   : "已读取核心记录；保存后才会更新", false);
}

static void records_submit(const char *path, const char *body)
{
    if (record_busy) return;
    record_busy = true;
    for (int i = 0; i < 6; i++) {
        if (record_controls[i]) lv_obj_add_state(record_controls[i], LV_STATE_DISABLED);
    }
    set_records_status("正在请求核心，请稍候", false);
    if (!qz_web_request_async("POST", path, body, records_received, NULL))
        records_received(false, "", NULL);
}

static void records_request(const char *action)
{
    if (!records_input) return;
    char quoted[10000];
    char body[11000];
    if (!qz_json_quote(lv_textarea_get_text(records_input), quoted, sizeof(quoted))) {
        set_records_status("内容过长，请缩短", true);
        return;
    }
    snprintf(body, sizeof(body),
             "{\"action\":\"%s\",\"kind\":\"%s\",\"id\":%d,\"text\":%s,\"confirmed\":true}",
             action, record_kind, record_id, quoted);
    records_submit("/api/companion/records", body);
}

static void records_action(lv_event_t *event)
{
    int action = (int)(intptr_t)lv_event_get_user_data(event);
    if (record_busy) return;
    if (care_mode) {
        int values[3];
        for (int i = 0; i < 3; i++) {
            const char *text = care_inputs[i] ? lv_textarea_get_text(care_inputs[i]) : "";
            char *end = NULL;
            long value = strtol(text, &end, 10);
            if (!text[0] || !end || *end ||
                value < (i == 0 ? 30 : 0) || value > (i == 0 ? 1440 : 23)) {
                set_records_status("间隔 30-1440；小时 0-23", true);
                return;
            }
            values[i] = (int)value;
        }
        char body[256];
        snprintf(body, sizeof(body),
                 "{\"action\":\"configure\",\"enabled\":%s,\"confirmed\":true,"
                 "\"interval_minutes\":%d,\"quiet_start\":%d,\"quiet_end\":%d}",
                 action ? "false" : "true", values[0], values[1], values[2]);
        records_submit("/api/companion/care", body);
        return;
    }
    if (action == 4) {
        record_page++;
        records_request("list");
        return;
    }
    if (action != 0 && action != 5 && !record_id) {
        set_records_status("没有可操作的记录", true);
        return;
    }
    const char *actions[] = {"add", "update", "delete", "complete", "list", "list"};
    records_request(action == 3 && record_done ? "reopen" : actions[action]);
}

static const char *const record_templates[3] = {
    "买牛奶", "明天下午三点开会", "周五之前提交周报",
};

static void record_template_clicked(lv_event_t *event)
{
    if (record_busy || !records_input) return;
    lv_textarea_set_text(records_input, lv_event_get_user_data(event));
    records_request("add");
}

static void records_input_event(lv_event_t *event)
{
    if (!records_keyboard) return;
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(records_keyboard, lv_event_get_target(event));
        lv_obj_clear_flag(records_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(records_keyboard);
    } else if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL ||
               code == LV_EVENT_DEFOCUSED) {
        lv_obj_add_flag(records_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static const char *records_title_for(const char *kind)
{
    if (strcmp(kind, "care") == 0) return "主动关心";
    if (strcmp(kind, "memory") == 0) return "个人记忆";
    if (strcmp(kind, "learning") == 0) return "学习教练";
    if (strcmp(kind, "todo") == 0) return "日常计划";
    return "记录";
}

static const char *records_subtitle_for(const char *kind)
{
    if (strcmp(kind, "care") == 0) return "频率与安静时段";
    if (strcmp(kind, "memory") == 0) return "长期偏好与可修改信息";
    if (strcmp(kind, "learning") == 0) return "目标、基础与学习记录";
    if (strcmp(kind, "todo") == 0) return "待办与完成状态";
    return "本地核心记录";
}

static qz_icon_t records_icon_for(const char *kind)
{
    if (strcmp(kind, "care") == 0) return QZ_ICON_BELL;
    if (strcmp(kind, "memory") == 0) return QZ_ICON_DATABASE;
    if (strcmp(kind, "learning") == 0) return QZ_ICON_BULB;
    return QZ_ICON_LIST;
}

static void records_open(const char *kind)
{
    if (records_scrim) return;
    companion_close(NULL);
    care_mode = strcmp(kind, "care") == 0;
    record_kind = kind;
    record_page = 0;
    record_id = 0;
    record_count = 0;
    record_busy = false;
    record_chip_row = NULL;
    memset(record_controls, 0, sizeof(record_controls));
    memset(care_inputs, 0, sizeof(care_inputs));

    records_scrim = qz_overlay_create(lv_layer_top());
    lv_obj_add_event_cb(records_scrim, records_close, LV_EVENT_CLICKED, NULL);
    records_panel = lv_obj_create(records_scrim);
    lv_obj_set_size(records_panel, 456, 300);
    lv_obj_align(records_panel, LV_ALIGN_TOP_MID, 0, 10);
    qz_style_sheet(records_panel);
    lv_obj_set_style_pad_all(records_panel, PANEL_PAD, 0);
    lv_obj_clear_flag(records_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *tile = icon_tile(records_panel, 32, records_icon_for(kind),
                               QZ_ACCENT_TEXT, QZ_ACCENT_TINT);
    lv_obj_align(tile, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *title = qz_text(records_panel, records_title_for(kind), 17,
                              qz_color(QZ_TEXT));
    lv_obj_set_size(title, 260, 22);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 42, 0);

    lv_obj_t *subtitle = qz_text(records_panel, records_subtitle_for(kind), 10,
                                 qz_color(QZ_TEXT_TERTIARY));
    lv_obj_set_size(subtitle, 270, 16);
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);
    lv_obj_align(subtitle, LV_ALIGN_TOP_LEFT, 42, 21);

    lv_obj_t *close = qz_button(records_panel, "返回", 74, 32);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_event_cb(close, records_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t *content_well = lv_obj_create(records_panel);
    lv_obj_set_size(content_well, 428, 56);
    lv_obj_align(content_well, LV_ALIGN_TOP_LEFT, 0, 54);
    lv_obj_set_style_radius(content_well, 4, 0);
    qz_obj_set_bg_color(content_well, QZ_FILL, 0);
    lv_obj_set_style_bg_opa(content_well, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(content_well, 0, 0);
    lv_obj_set_style_pad_all(content_well, 0, 0);
    lv_obj_clear_flag(content_well, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(content_well, LV_OBJ_FLAG_CLICKABLE);

    records_label = qz_text(content_well, "正在读取核心", 13, qz_color(QZ_TEXT));
    lv_obj_set_size(records_label, 408, 42);
    lv_label_set_long_mode(records_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(records_label, LV_ALIGN_TOP_LEFT, 10, 7);

    records_status = qz_text(records_panel, "", 11, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_set_size(records_status, 428, 17);
    lv_label_set_long_mode(records_status, LV_LABEL_LONG_DOT);
    lv_obj_align(records_status, LV_ALIGN_TOP_LEFT, 0, 116);

    if (care_mode) {
        const char *labels[] = {"间隔分钟", "安静开始", "安静结束"};
        for (int i = 0; i < 3; i++) {
            lv_obj_t *label = qz_text(records_panel, labels[i], 11, qz_color(QZ_TEXT));
            lv_obj_align(label, LV_ALIGN_TOP_LEFT, i * 144, 138);
            care_inputs[i] = lv_textarea_create(records_panel);
            lv_obj_set_size(care_inputs[i], 132, 36);
            lv_obj_align(care_inputs[i], LV_ALIGN_TOP_LEFT, i * 144, 156);
            lv_textarea_set_one_line(care_inputs[i], true);
            lv_textarea_set_accepted_chars(care_inputs[i], "0123456789");
            qz_style_textarea(care_inputs[i]);
            lv_obj_add_event_cb(care_inputs[i], records_input_event, LV_EVENT_ALL, NULL);
        }
        for (int i = 0; i < 2; i++) {
            record_controls[i] = qz_button(records_panel,
                                           i ? "关闭关心" : "同意开启", 150, 38);
            lv_obj_align(record_controls[i], LV_ALIGN_TOP_LEFT, i * 158, 206);
            lv_obj_add_event_cb(record_controls[i], records_action, LV_EVENT_CLICKED,
                                (void *)(intptr_t)i);
        }
        if (care_enabled) qz_style_primary_button(record_controls[0]);
    } else {
        record_chip_row = lv_obj_create(records_panel);
        lv_obj_set_size(record_chip_row, 428, 28);
        lv_obj_align(record_chip_row, LV_ALIGN_TOP_LEFT, 0, 136);
        lv_obj_set_style_bg_opa(record_chip_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(record_chip_row, 0, 0);
        lv_obj_set_style_pad_all(record_chip_row, 0, 0);
        lv_obj_clear_flag(record_chip_row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(record_chip_row, LV_OBJ_FLAG_CLICKABLE);
        for (int i = 0; i < 3; i++) {
            lv_obj_t *chip = qz_chip(record_chip_row, record_templates[i], 136, 28);
            lv_obj_align(chip, LV_ALIGN_TOP_LEFT, i * 142, 0);
            lv_obj_t *caption = lv_obj_get_child(chip, 0);
            if (caption) {
                lv_obj_set_style_text_font(caption, qz_font_size(12), 0);
                lv_label_set_long_mode(caption, LV_LABEL_LONG_DOT);
                lv_obj_set_width(caption, 126);
            }
            lv_obj_add_event_cb(chip, record_template_clicked, LV_EVENT_CLICKED,
                                (void *)record_templates[i]);
        }
        lv_obj_add_flag(record_chip_row, LV_OBJ_FLAG_HIDDEN);

        records_input = lv_textarea_create(records_panel);
        lv_obj_set_size(records_input, 428, 36);
        lv_obj_align(records_input, LV_ALIGN_TOP_LEFT, 0, 168);
        lv_textarea_set_one_line(records_input, true);
        lv_textarea_set_max_length(records_input, 500);
        lv_textarea_set_placeholder_text(records_input, "输入英文或数字；中文可点建议");
        qz_style_textarea(records_input);
        lv_obj_add_event_cb(records_input, records_input_event, LV_EVENT_ALL, NULL);

        const char *labels[] = {"新增", "修改", "删除", "切换完成", "下一条", "刷新"};
        for (int i = 0; i < 6; i++) {
            record_controls[i] = qz_button(records_panel, labels[i], 136, 30);
            lv_obj_align(record_controls[i], LV_ALIGN_TOP_LEFT,
                         (i % 3) * 146, 208 + (i / 3) * 32);
            lv_obj_add_event_cb(record_controls[i], records_action, LV_EVENT_CLICKED,
                                (void *)(intptr_t)i);
            if (i == 3 && strcmp(kind, "todo") != 0)
                lv_obj_add_flag(record_controls[i], LV_OBJ_FLAG_HIDDEN);
        }

        records_keyboard = lv_keyboard_create(lv_layer_top());
        lv_obj_set_size(records_keyboard, lv_pct(100), 130);
        lv_obj_align(records_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
        qz_style_keyboard(records_keyboard);
        lv_obj_add_flag(records_keyboard, LV_OBJ_FLAG_HIDDEN);
    }

    if (care_mode)
        records_submit("/api/companion/care", "{\"action\":\"status\"}");
    else
        records_request("list");

    overlay_animate_in(records_scrim, records_panel);
}

/* ------------------------------------------------------------------------- *
 * Page
 * ------------------------------------------------------------------------- */

lv_obj_t *qz_skill_page_create(lv_obj_t *apps_screen)
{
    apps_screen_ref = apps_screen;
    skill_screen = lv_obj_create(NULL);
    qz_style_screen(skill_screen);

    lv_obj_t *toolbar = qz_page_header(skill_screen, "技能");

    lv_obj_t *back = qz_back_button(toolbar);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *companion = qz_button(toolbar, "陪伴", 66, 34);
    lv_obj_align(companion, LV_ALIGN_RIGHT_MID, -52, 0);
    lv_obj_add_event_cb(companion, companion_open, LV_EVENT_CLICKED, NULL);

    lv_obj_t *refresh = qz_icon_button(toolbar, LV_SYMBOL_REFRESH, 34);
    lv_obj_align(refresh, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_add_event_cb(refresh, refresh_clicked, LV_EVENT_CLICKED, NULL);

    overview_card = lv_obj_create(skill_screen);
    lv_obj_set_size(overview_card, CONTENT_W, 52);
    lv_obj_align(overview_card, LV_ALIGN_TOP_LEFT, QZ_GUTTER, 64);
    qz_style_plate(overview_card);
    lv_obj_set_style_pad_all(overview_card, 0, 0);

    overview_tile = icon_tile(overview_card, 36, QZ_ICON_SPARKLES,
                              QZ_ACCENT_TEXT, QZ_ACCENT_TINT);
    lv_obj_align(overview_tile, LV_ALIGN_LEFT_MID, 10, 0);
    overview_icon = lv_obj_get_child(overview_tile, 0);

    overview_primary = qz_text(overview_card, "读取中", 15, qz_color(QZ_TEXT));
    lv_obj_set_size(overview_primary, CONTENT_W - 66, 22);
    lv_label_set_long_mode(overview_primary, LV_LABEL_LONG_DOT);
    lv_obj_align(overview_primary, LV_ALIGN_TOP_LEFT, 54, 6);

    overview_meta = qz_text(overview_card, "正在读取技能状态", 10,
                            qz_color(QZ_TEXT_TERTIARY));
    lv_obj_set_size(overview_meta, CONTENT_W - 66, 16);
    lv_label_set_long_mode(overview_meta, LV_LABEL_LONG_DOT);
    lv_obj_align(overview_meta, LV_ALIGN_TOP_LEFT, 54, 30);

    list_card = lv_obj_create(skill_screen);
    lv_obj_set_size(list_card, CONTENT_W, 112);
    lv_obj_align(list_card, LV_ALIGN_TOP_LEFT, QZ_GUTTER, 124);
    lv_obj_set_style_bg_opa(list_card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_card, 0, 0);
    lv_obj_set_style_pad_all(list_card, 0, 0);
    lv_obj_clear_flag(list_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(list_card, LV_OBJ_FLAG_CLICKABLE);

    service_dot = lv_obj_create(skill_screen);
    lv_obj_set_size(service_dot, 7, 7);
    lv_obj_align(service_dot, LV_ALIGN_TOP_LEFT, QZ_GUTTER + 1, 249);
    lv_obj_set_style_radius(service_dot, LV_RADIUS_CIRCLE, 0);
    qz_obj_set_bg_color(service_dot, QZ_TEXT_TERTIARY, 0);
    lv_obj_set_style_bg_opa(service_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(service_dot, 0, 0);
    lv_obj_clear_flag(service_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(service_dot, LV_OBJ_FLAG_SCROLLABLE);

    service_label = qz_text(skill_screen, "等待技能服务", 11,
                            qz_color(QZ_TEXT_TERTIARY));
    lv_obj_set_size(service_label, CONTENT_W - 20, 18);
    lv_label_set_long_mode(service_label, LV_LABEL_LONG_DOT);
    lv_obj_align(service_label, LV_ALIGN_TOP_LEFT, QZ_GUTTER + 14, 244);

    list_previous = qz_button(skill_screen, "上一页", 116, 38);
    lv_obj_align(list_previous, LV_ALIGN_TOP_LEFT, QZ_GUTTER, 268);
    lv_obj_add_event_cb(list_previous, list_page_changed, LV_EVENT_CLICKED,
                        (void *)(intptr_t)-1);

    list_next = qz_button(skill_screen, "下一页", 116, 38);
    lv_obj_align(list_next, LV_ALIGN_TOP_RIGHT, -QZ_GUTTER, 268);
    lv_obj_add_event_cb(list_next, list_page_changed, LV_EVENT_CLICKED,
                        (void *)(intptr_t)1);

    list_page_label = qz_text(skill_screen, "1 / 1", 12,
                              qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(list_page_label, LV_ALIGN_TOP_MID, 0, 280);

    lv_obj_add_event_cb(skill_screen, screen_loaded, LV_EVENT_ALL, NULL);
    return skill_screen;
}

lv_obj_t *qz_skill_page_screen(void)
{
    return skill_screen;
}
