#include "skills_page.h"
#include "skills.h"
#include "theme.h"
#include "config.h"
#include <stdio.h>
#include <string.h>

/* Device side of the core's Skill manager. Same data as the web console, sized
 * for a 480x320 panel: a list of skills with their role, and a sheet to switch
 * a skill between 主技能 / 备用 / 关闭. Editing and adding skills stays in the
 * web console (a text editor does not fit here), which the footer points at. */

#define CONTENT_W (QZ_DESIGN_W - 2 * QZ_GUTTER)
#define CARD_PAD 12
#define ROW_H 52
#define PANEL_PAD 12

static lv_obj_t *skill_screen;
static lv_obj_t *apps_screen_ref;
static lv_obj_t *list_card;
static lv_obj_t *summary_label;
static lv_obj_t *service_label;
static lv_obj_t *sheet_overlay;
static lv_obj_t *sheet_title;
static lv_obj_t *sheet_description;
static qz_skill_t skills[QZ_SKILL_MAX];
static lv_obj_t *list_page_label;
static lv_obj_t *list_previous;
static lv_obj_t *list_next;
static int list_page;
static int sheet_info_page;
static int sheet_skill_index;

#define COMPACT_SKILLS_PER_PAGE 1
#define SHEET_INFO_CHARS 40

/* Split at UTF-8 boundaries so every description remains readable, including
 * long descriptions supplied by the service. The small screen never needs a
 * scrolling sheet or an ellipsis to reach the remaining text. */
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

static void sheet_info_changed(lv_event_t *event)
{
    const qz_skill_t *skill = &skills[sheet_skill_index];
    const char *description = skill->description[0] ? skill->description : "未填写说明";
    char text[QZ_SKILL_DESC_MAX];
    int pages = description_chunk(description, 0, text, sizeof(text));
    int direction = (int)(intptr_t)lv_event_get_user_data(event);
    sheet_info_page = (sheet_info_page + direction + pages) % pages;
    description_chunk(description, sheet_info_page, text, sizeof(text));
    lv_label_set_text(sheet_description, text);
}

static int skill_count = -1;      /* -1 = the service could not be reached */
static bool role_just_changed;

/* ------------------------------------------------------------------------- *
 * Sheet
 * ------------------------------------------------------------------------- */

static void close_sheet(void)
{
    if (sheet_overlay) lv_obj_delete(sheet_overlay);
    sheet_overlay = NULL;
    sheet_title = NULL;
    sheet_description = NULL;
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

static void sheet_pick_role(lv_event_t *event)
{
    const void *data = lv_event_get_user_data(event);
    /* The skill index and the role are packed into one user data value: the
     * list is rebuilt on every change, so nothing else stays valid. */
    int packed = (int)(intptr_t)data;
    int index = packed >> 2;
    qz_skill_role_t role = (qz_skill_role_t)(packed & 0x3);

    if (index >= 0 && index < skill_count) {
        /* The core answers the next turn with a rebuilt session, so a failure
         * here means the choice was never stored — say so instead of silently
         * showing the old role. */
        role_just_changed = qz_skills_set_role(skills[index].id, role);
    }
    close_sheet();
    reload();
}

static void open_sheet(lv_event_t *event)
{
    int index = (int)(intptr_t)lv_event_get_user_data(event);
    if (index < 0 || index >= skill_count) return;

    const qz_skill_t *skill = &skills[index];
    lv_obj_t *panel;

    close_sheet();
    sheet_overlay = qz_overlay_create(skill_screen);
    lv_obj_add_event_cb(sheet_overlay, sheet_scrim_clicked, LV_EVENT_CLICKED, NULL);

    panel = lv_obj_create(sheet_overlay);
    lv_obj_set_size(panel, QZ_DESIGN_W - 40, 304);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, qz_compact() ? 8 : 14);
    lv_obj_set_style_radius(panel, QZ_RADIUS_CARD, 0);
    qz_style_plate(panel);
    lv_obj_set_style_bg_opa(panel, (lv_opa_t)238, 0);
    lv_obj_set_style_shadow_width(panel, 30, 0);
    lv_obj_set_style_shadow_opa(panel, (lv_opa_t)60, 0);
    lv_obj_set_style_pad_all(panel, PANEL_PAD, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    sheet_title = qz_text(panel, skill->name, 16, qz_color(QZ_TEXT));
    lv_obj_set_width(sheet_title, lv_pct(100));
    lv_label_set_long_mode(sheet_title, LV_LABEL_LONG_WRAP);
    lv_obj_align(sheet_title, LV_ALIGN_TOP_LEFT, 0, 0);

    sheet_description = qz_text(panel, skill->description[0] ? skill->description
                                                            : "未填写说明",
                                10, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_set_width(sheet_description, lv_pct(100));
    lv_label_set_long_mode(sheet_description, LV_LABEL_LONG_WRAP);
    lv_obj_align(sheet_description, LV_ALIGN_TOP_LEFT, 0, 72);

    {
        char chunk[QZ_SKILL_DESC_MAX];
        const char *description = skill->description[0] ? skill->description : "未填写说明";
        sheet_skill_index = index;
        sheet_info_page = 0;
        int pages = description_chunk(description, 0, chunk, sizeof(chunk));
        lv_label_set_text(sheet_description, chunk);
        if (pages > 1) {
            lv_obj_t *previous = qz_button(panel, "上段说明", 132, 36);
            lv_obj_align(previous, LV_ALIGN_TOP_LEFT, 0, 116);
            lv_obj_add_event_cb(previous, sheet_info_changed, LV_EVENT_CLICKED,
                                (void *)(intptr_t)-1);
            lv_obj_t *next = qz_button(panel, "下段说明", 132, 36);
            lv_obj_align(next, LV_ALIGN_TOP_RIGHT, 0, 116);
            lv_obj_add_event_cb(next, sheet_info_changed, LV_EVENT_CLICKED,
                                (void *)(intptr_t)1);
        }
        const qz_skill_role_t roles[] = {
            QZ_SKILL_ROLE_PRIMARY, QZ_SKILL_ROLE_SECONDARY, QZ_SKILL_ROLE_NONE,
        };
        const char *labels[] = { "主技能", "备用", "关闭" };
        for (int i = 0; i < 3; i++) {
            lv_obj_t *choice = qz_button(panel, labels[i], 128, 44);
            lv_obj_align(choice, LV_ALIGN_TOP_LEFT, i * 140, 154);
            if (skill->role == roles[i]) qz_style_primary_button(choice);
            lv_obj_add_event_cb(choice, sheet_pick_role, LV_EVENT_CLICKED,
                                (void *)(intptr_t)((index << 2) | (int)roles[i]));
        }
        lv_obj_t *notice = qz_text(panel, "主技能直接生效，备用按需检索\n切换后下一条消息重建会话",
                                    14, qz_color(QZ_TEXT_SECONDARY));
        lv_obj_set_width(notice, lv_pct(100));
        lv_obj_align(notice, LV_ALIGN_TOP_LEFT, 0, 204);
        lv_obj_t *cancel = qz_button(panel, "取消", 132, 36);
        lv_obj_align(cancel, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        lv_obj_add_event_cb(cancel, sheet_cancel, LV_EVENT_CLICKED, NULL);
        return;
    }

}

/* ------------------------------------------------------------------------- *
 * List
 * ------------------------------------------------------------------------- */

static void skill_row_clicked(lv_event_t *event);
static void rebuild(void);

static void list_page_changed(lv_event_t *event)
{
    list_page += (int)(intptr_t)lv_event_get_user_data(event);
    rebuild();
}

static void rebuild(void)
{
    if (!list_card) return;
    lv_obj_clean(list_card);
    int pages = skill_count > 0 ? (skill_count + COMPACT_SKILLS_PER_PAGE - 1) /
                                  COMPACT_SKILLS_PER_PAGE : 1;
    if (list_page >= pages) list_page = pages - 1;
    if (list_page < 0) list_page = 0;
    if (list_page_label) {
        char page_text[32];
        snprintf(page_text, sizeof(page_text), "%d / %d", list_page + 1, pages);
        lv_label_set_text(list_page_label, page_text);
        if (list_page == 0) lv_obj_add_state(list_previous, LV_STATE_DISABLED);
        else lv_obj_remove_state(list_previous, LV_STATE_DISABLED);
        if (list_page == pages - 1) lv_obj_add_state(list_next, LV_STATE_DISABLED);
        else lv_obj_remove_state(list_next, LV_STATE_DISABLED);
    }

    if (skill_count <= 0) {
        /* The card is a flex column, which ignores align on direct children, so
         * the message lives in a plain full size box. */
        lv_obj_t *box = lv_obj_create(list_card);
        const char *title_text = skill_count < 0 ? "本地服务未连接" : "还没有安装 Skill";
        const char *hint_text = skill_count < 0 ? "请确认 QZdesk 核心正在运行"
                                               : "在浏览器打开下方地址即可添加";
        lv_obj_set_size(box, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(box, 0, 0);
        lv_obj_set_style_pad_all(box, 0, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = qz_text(box, title_text, 13, qz_color(QZ_TEXT_SECONDARY));
        lv_obj_align(title, LV_ALIGN_CENTER, 0, -10);
        lv_obj_t *hint = qz_text(box, hint_text, 10, qz_color(QZ_TEXT_TERTIARY));
        lv_obj_align(hint, LV_ALIGN_CENTER, 0, 11);
        return;
    }

    int first = list_page * COMPACT_SKILLS_PER_PAGE;
    int end = first + COMPACT_SKILLS_PER_PAGE;
    if (end > skill_count) end = skill_count;
    for (int i = first; i < end; i++) {
        const qz_skill_t *skill = &skills[i];
        lv_obj_t *row = lv_obj_create(list_card);
        lv_obj_set_size(row, lv_pct(100), 140);
        qz_style_row(row);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        qz_obj_set_border_color(row, QZ_SEPARATOR, 0);
        lv_obj_set_style_border_opa(row, (lv_opa_t)140, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, skill_row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        /* Role pill: filled accent for the primary skill, the accent tint for a
         * standby one, neutral fill for an off one — three states, no new hue. */
        uint32_t pill_color = skill->role == QZ_SKILL_ROLE_PRIMARY ? QZ_ACCENT
                            : skill->role == QZ_SKILL_ROLE_SECONDARY ? QZ_ACCENT_TINT
                                                                     : QZ_FILL_PRESSED;
        uint32_t pill_text = skill->role == QZ_SKILL_ROLE_PRIMARY ? QZ_TEXT_ON_ACCENT
                           : skill->role == QZ_SKILL_ROLE_SECONDARY ? QZ_ACCENT_DARK
                                                                  : QZ_TEXT_SECONDARY;
        lv_obj_t *pill = lv_obj_create(row);
        lv_obj_set_size(pill, qz_compact() ? 74 : 54, qz_compact() ? 30 : 20);
        lv_obj_align(pill, LV_ALIGN_LEFT_MID, CARD_PAD, 0);
        lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(pill, qz_color(pill_color), 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(pill, 0, 0);
        lv_obj_set_style_pad_all(pill, 0, 0);
        lv_obj_clear_flag(pill, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *pill_text_label = qz_text(pill, qz_skills_role_label(skill->role), 10,
                                            qz_color(pill_text));
        lv_obj_center(pill_text_label);

        lv_obj_t *name = qz_text(row, skill->name, qz_compact() ? 16 : 13, qz_color(QZ_TEXT));
        lv_obj_set_width(name, CONTENT_W - CARD_PAD * 2 - (qz_compact() ? 116 : 96));
        lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
        lv_obj_align(name, LV_ALIGN_LEFT_MID,
                     CARD_PAD + (qz_compact() ? 86 : 62), 0);

        lv_obj_t *chev = qz_chevron(row, qz_color(QZ_TEXT_TERTIARY), 14);
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -CARD_PAD, 0);
    }
}

static void skill_row_clicked(lv_event_t *event)
{
    open_sheet(event);
}

static void reload(void)
{
    qz_skill_summary_t summary;
    char text[96];
    int count = qz_skills_fetch(skills, QZ_SKILL_MAX, &summary);
    skill_count = count;

    if (count < 0) {
        if (summary_label) lv_label_set_text(summary_label, "未连接");
        if (service_label) {
            lv_label_set_text(service_label, "本地 Skill 服务未响应");
            qz_obj_set_text_color(service_label, QZ_TEXT_TERTIARY, 0);
        }
    } else {
        snprintf(text, sizeof(text), "主技能 %d · 备用 %d · 共 %d 个",
                 summary.primary, summary.secondary, summary.count);
        if (summary_label) lv_label_set_text(summary_label, text);
        if (role_just_changed) {
            /* The switch itself is already stored; tell the user when it lands
             * instead of leaving them to guess whether anything happened. */
            if (service_label) {
                lv_label_set_text(service_label, "已切换，下一条消息会重建会话");
                qz_obj_set_text_color(service_label, QZ_ACCENT_TEXT, 0);
            }
        } else if (summary.primary == 0) {
            if (service_label) {
                lv_label_set_text(service_label, "没有主技能，对话不会加载指令");
                qz_obj_set_text_color(service_label, QZ_ACCENT_TEXT, 0);
            }
        } else if (service_label) {
            char address[32];
            qz_device_ip(address, sizeof(address));
            snprintf(text, sizeof(text), qz_compact() ? "编辑与新增：%s:8080"
                                                     : "浏览器访问 %s:8080 可编辑或新增", address);
            lv_label_set_text(service_label, text);
            qz_obj_set_text_color(service_label, QZ_TEXT_TERTIARY, 0);
        }
    }
    rebuild();
}

static void refresh_clicked(lv_event_t *event)
{
    (void)event;
    reload();
}

static void screen_loaded(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_SCREEN_LOADED) return;
    reload();
}

static void back_clicked(lv_event_t *event)
{
    (void)event;
    qz_screen_load(apps_screen_ref, LV_SCR_LOAD_ANIM_MOVE_RIGHT, QZ_DUR_SCREEN);
}

lv_obj_t *qz_skill_page_create(lv_obj_t *apps_screen)
{
    apps_screen_ref = apps_screen;
    skill_screen = lv_obj_create(NULL);
    qz_style_screen(skill_screen);

    lv_obj_t *toolbar = lv_obj_create(skill_screen);
    lv_obj_set_size(toolbar, QZ_DESIGN_W - 16, 48);
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 6);
    qz_style_toolbar(toolbar);

    lv_obj_t *back = qz_back_button(toolbar);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = qz_text(toolbar, "技能", 15, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *refresh = qz_icon_button(toolbar, LV_SYMBOL_REFRESH, qz_compact() ? 40 : 30);
    lv_obj_align(refresh, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_add_event_cb(refresh, refresh_clicked, LV_EVENT_CLICKED, NULL);

    summary_label = qz_text(skill_screen, "--", qz_compact() ? 16 : 12, qz_color(QZ_TEXT));
    lv_obj_align(summary_label, LV_ALIGN_TOP_LEFT, QZ_GUTTER, 58);
    service_label = qz_text(skill_screen, "", 10, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_set_width(service_label, CONTENT_W);
    lv_label_set_long_mode(service_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(service_label, LV_ALIGN_TOP_LEFT, QZ_GUTTER, 80);

    list_card = lv_obj_create(skill_screen);
    lv_obj_set_size(list_card, CONTENT_W, 144);
    lv_obj_align(list_card, LV_ALIGN_TOP_LEFT, QZ_GUTTER, 106);
    lv_obj_set_style_radius(list_card, QZ_RADIUS_CARD, 0);
    qz_style_plate(list_card);
    lv_obj_set_style_pad_all(list_card, 0, 0);
    lv_obj_set_style_pad_row(list_card, 0, 0);
    lv_obj_set_flex_flow(list_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scrollbar_mode(list_card, LV_SCROLLBAR_MODE_OFF);
    {
        lv_obj_clear_flag(list_card, LV_OBJ_FLAG_SCROLLABLE);
        list_previous = qz_button(skill_screen, "上一页", 128, 44);
        lv_obj_align(list_previous, LV_ALIGN_TOP_LEFT, QZ_GUTTER, 266);
        lv_obj_add_event_cb(list_previous, list_page_changed, LV_EVENT_CLICKED,
                            (void *)(intptr_t)-1);
        list_next = qz_button(skill_screen, "下一页", 128, 44);
        lv_obj_align(list_next, LV_ALIGN_TOP_RIGHT, -QZ_GUTTER, 266);
        lv_obj_add_event_cb(list_next, list_page_changed, LV_EVENT_CLICKED,
                            (void *)(intptr_t)1);
        list_page_label = qz_text(skill_screen, "1 / 1", 16, qz_color(QZ_TEXT_SECONDARY));
        lv_obj_align(list_page_label, LV_ALIGN_TOP_MID, 0, 279);
    }

    /* No fetch here: the page is built while the app starts, and the service
     * is read when the page is actually opened. */
    lv_obj_add_event_cb(skill_screen, screen_loaded, LV_EVENT_ALL, NULL);
    return skill_screen;
}

lv_obj_t *qz_skill_page_screen(void)
{
    return skill_screen;
}
