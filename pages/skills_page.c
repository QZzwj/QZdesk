#include "skills_page.h"
#include "skills.h"
#include "assistant.h"
#include "applets.h"
#include <stdlib.h>
#include "theme.h"
#include "config.h"
#include "web_client.h"
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
static bool role_saving;

/* ------------------------------------------------------------------------- *
 * Sheet
 * ------------------------------------------------------------------------- */

static void close_sheet(void)
{
    if (role_saving) return;
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

static void role_saved(bool ok, const char *body, void *data)
{
    (void)data;
    role_saving = false;
    if (!ok) {
        if (sheet_description) lv_label_set_text(sheet_description, "保存或同步失败，请重试");
        return;
    }
    (void)body;
    role_just_changed = true;
    close_sheet();
    reload();
}

static void sheet_pick_role(lv_event_t *event)
{
    if (role_saving) return;
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
        char path[160];
        char body[64];
        snprintf(path, sizeof(path), "/api/skills/%s/selection", skills[index].id);
        snprintf(body, sizeof(body), "{\"role\":\"%s\"}", qz_skills_role_key(role));
        role_saving = qz_web_request_async("PUT", path, body, role_saved, NULL);
        if (!role_saving) lv_label_set_text(sheet_description, "无法保存，请重试");
    }
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
        lv_obj_set_size(row, lv_pct(100), 112);
        qz_style_row(row);
        qz_obj_set_bg_color(row, QZ_CARD, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 16, 0);
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
        lv_obj_align(pill, LV_ALIGN_TOP_RIGHT, -18, 16);
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

        lv_obj_t *name = qz_text(row, skill->name, 22, qz_color(QZ_TEXT));
        lv_obj_set_width(name, CONTENT_W - 130);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_height(name, 32);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 18, 18);
        lv_obj_t *hint = qz_text(row, "点击查看与切换角色", 14, qz_color(QZ_TEXT_SECONDARY));
        lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 18, -18);

        lv_obj_t *chev = qz_chevron(row, qz_color(QZ_TEXT_TERTIARY), 14);
        lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -CARD_PAD, 0);
    }
}

static void skill_row_clicked(lv_event_t *event)
{
    open_sheet(event);
}

static bool skills_loading;

static void skills_loaded(bool ok, const char *body, void *data)
{
    (void)data;
    skills_loading = false;
    qz_skill_summary_t summary = {0};
    char text[96];
    int count = ok ? qz_skills_parse(body, skills, QZ_SKILL_MAX, &summary) : -1;
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
                lv_label_set_text(service_label, "选择已保存，当前对话结束后同步");
                qz_obj_set_text_color(service_label, QZ_ACCENT_TEXT, 0);
            }
            role_just_changed = false;
        } else if (summary.primary == 0) {
            if (service_label) {
                lv_label_set_text(service_label, "没有主技能，对话不会加载指令");
                qz_obj_set_text_color(service_label, QZ_ACCENT_TEXT, 0);
            }
        } else if (service_label) {
            char address[32];
            qz_device_ip(address, sizeof(address));
            snprintf(text, sizeof(text), qz_compact() ? "编辑与新增：%s:%u"
                                                     : "浏览器访问 %s:%u 可编辑或新增",
                     address, qz_web_port());
            lv_label_set_text(service_label, text);
            qz_obj_set_text_color(service_label, QZ_TEXT_TERTIARY, 0);
        }
    }
    rebuild();
}

static void reload(void)
{
    if (skills_loading) return;
    skills_loading = qz_web_request_async("GET", "/api/skills", NULL, skills_loaded, NULL);
    if (!skills_loading && service_label) lv_label_set_text(service_label, "无法加载，请重试");
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

static lv_obj_t *companion_overlay;
static lv_obj_t *companion_hint;
static lv_obj_t *companion_caption;
static lv_obj_t *companion_buttons[4];
static int companion_page;
static const char *companion_names[8] = {"个人记忆", "情绪陪伴", "主动关心", "专注搭子", "学习教练", "日常计划", "睡前故事", "设备管家"};
static const char *companion_prompts[8] = {
    "查看我的个人记忆，并说明如何修改或忘记", "我想倾诉一下，请先听我说",
    "查询主动关心设置，帮我设置频率和安静时段；开启前请确认",
    "帮我确定本轮专注目标，再确认番茄钟参数", "帮我制定学习目标，先了解我的基础",
    "查看我的待办，帮我安排今天的重点", "给我讲一个三分钟的睡前故事", "查看设备状态，先不要执行控制"
};

static void companion_close(lv_event_t *event)
{
    (void)event;
    if (companion_overlay) lv_obj_delete(companion_overlay);
    companion_overlay = NULL;
}

static lv_obj_t *records_overlay, *records_label, *records_input, *records_keyboard, *records_status;
static lv_obj_t *record_controls[6];
static const char *record_kind;
static int record_page, record_id;
static bool record_done, record_busy, care_enabled;
static lv_obj_t *care_inputs[3];
static bool care_mode;
static void records_request(const char *action);

static void records_close(lv_event_t *event)
{
    (void)event;
    if (record_busy) return;
    if (records_keyboard) lv_obj_delete(records_keyboard);
    records_keyboard = NULL;
    if (records_overlay) lv_obj_delete(records_overlay);
    records_overlay = NULL;
}

static void records_received(bool ok, const char *response, void *data)
{
    (void)data;
    record_busy = false;
    if (!records_overlay) return;
    for (int i = 0; i < 6; i++) if (record_controls[i]) lv_obj_remove_state(record_controls[i], LV_STATE_DISABLED);
    if (!ok) {
        char error[128];
        if (!qz_json_string(response, "error", error, sizeof(error))) snprintf(error, sizeof(error), "核心未连接，操作未确认，请重试");
        lv_label_set_text(records_status, error);
        return;
    }
    if (care_mode) {
        const char *care = qz_json_value(response, "care");
        int values[3];
        if (!care || !qz_json_bool(care, "enabled", &care_enabled) ||
            !qz_json_int(care, "interval_minutes", &values[0]) ||
            !qz_json_int(care, "quiet_start", &values[1]) || !qz_json_int(care, "quiet_end", &values[2])) {
            lv_label_set_text(records_status, "核心响应不完整，设置未确认"); return;
        }
        for (int i = 0; i < 3; i++) { char v[12]; snprintf(v, sizeof(v), "%d", values[i]); lv_textarea_set_text(care_inputs[i], v); }
        lv_label_set_text(records_label, care_enabled ? "主动关心：已开启" : "主动关心：已关闭");
        lv_label_set_text(records_status, "已读取核心设置；安静时段按设备时区");
        return;
    }
    if (!qz_json_value(response, "records")) { lv_label_set_text(records_status, "核心响应不完整，记录未确认"); return; }
    while (record_page > 0 && !qz_json_item(response, "records", record_page)) record_page--;
    const char *item = qz_json_item(response, "records", record_page);
    record_id = 0; record_done = false;
    if (item) {
        char text[8192];
        if (!qz_json_int(item, "id", &record_id) || !qz_json_string(item, "text", text, sizeof(text))) {
            lv_label_set_text(records_status, "记录响应无效"); return;
        }
        qz_json_bool(item, "done", &record_done);
        lv_label_set_text(records_label, text);
        lv_textarea_set_text(records_input, text);
    } else lv_label_set_text(records_label, "暂无记录，填写内容后点新增");
    lv_label_set_text(records_status, record_done ? "已完成；可点切换恢复待办" : "已读取核心记录；保存后才会更新");
}

static void records_submit(const char *path, const char *body)
{
    if (record_busy) return;
    record_busy = true;
    for (int i = 0; i < 6; i++) if (record_controls[i]) lv_obj_add_state(record_controls[i], LV_STATE_DISABLED);
    lv_label_set_text(records_status, "正在请求核心，请稍候");
    if (!qz_web_request_async("POST", path, body, records_received, NULL)) records_received(false, "", NULL);
}

static void records_request(const char *action)
{
    char quoted[10000], body[11000];
    if (!qz_json_quote(lv_textarea_get_text(records_input), quoted, sizeof(quoted))) {
        lv_label_set_text(records_status, "内容过长，请缩短"); return;
    }
    snprintf(body, sizeof(body), "{\"action\":\"%s\",\"kind\":\"%s\",\"id\":%d,\"text\":%s,\"confirmed\":true}", action, record_kind, record_id, quoted);
    records_submit("/api/companion/records", body);
}

static void records_action(lv_event_t *event)
{
    int action = (int)(intptr_t)lv_event_get_user_data(event);
    if (record_busy) return;
    if (care_mode) {
        int v[3];
        for (int i = 0; i < 3; i++) {
            const char *s = lv_textarea_get_text(care_inputs[i]); char *end;
            long n = strtol(s, &end, 10);
            if (!s[0] || *end || n < (i == 0 ? 30 : 0) || n > (i == 0 ? 1440 : 23)) {
                lv_label_set_text(records_status, "间隔30-1440；小时0-23"); return;
            }
            v[i] = (int)n;
        }
        char body[256];
        snprintf(body, sizeof(body), "{\"action\":\"configure\",\"enabled\":%s,\"confirmed\":true,\"interval_minutes\":%d,\"quiet_start\":%d,\"quiet_end\":%d}", action ? "false" : "true", v[0], v[1], v[2]);
        records_submit("/api/companion/care", body); return;
    }
    if (action == 4) { record_page++; records_request("list"); return; }
    if (action != 0 && action != 5 && !record_id) { lv_label_set_text(records_status, "没有可操作的记录"); return; }
    const char *actions[] = {"add", "update", "delete", "complete", "list", "list"};
    records_request(action == 3 && record_done ? "reopen" : actions[action]);
}

static void records_input_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(records_keyboard, lv_event_get_target(event));
        lv_obj_clear_flag(records_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(records_keyboard);
    } else if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL || code == LV_EVENT_DEFOCUSED) {
        lv_obj_add_flag(records_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void records_open(const char *kind)
{
    companion_close(NULL);
    care_mode = strcmp(kind, "care") == 0; record_kind = kind; record_page = 0; record_id = 0;
    memset(record_controls, 0, sizeof(record_controls));
    records_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(records_overlay, lv_pct(96), lv_pct(96)); lv_obj_center(records_overlay);
    qz_style_plate(records_overlay); lv_obj_set_style_pad_all(records_overlay, 8, 0);
    lv_obj_clear_flag(records_overlay, LV_OBJ_FLAG_SCROLLABLE);
    records_label = qz_text(records_overlay, "正在读取核心", 14, qz_color(QZ_TEXT));
    lv_obj_set_size(records_label, lv_pct(100), 60); lv_obj_align(records_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_long_mode(records_label, LV_LABEL_LONG_SCROLL);
    records_status = qz_text(records_overlay, "", 12, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_set_size(records_status, lv_pct(100), 24); lv_obj_align(records_status, LV_ALIGN_TOP_LEFT, 0, 202);
    if (care_mode) {
        const char *labels[] = {"间隔分钟", "安静开始", "安静结束"};
        for (int i = 0; i < 3; i++) {
            lv_obj_t *label = qz_text(records_overlay, labels[i], 12, qz_color(QZ_TEXT));
            lv_obj_align(label, LV_ALIGN_TOP_LEFT, i * 94, 60);
            care_inputs[i] = lv_textarea_create(records_overlay);
            lv_obj_set_size(care_inputs[i], 84, 40); lv_obj_align(care_inputs[i], LV_ALIGN_TOP_LEFT, i * 94, 80);
            lv_textarea_set_one_line(care_inputs[i], true); lv_textarea_set_accepted_chars(care_inputs[i], "0123456789");
            qz_style_textarea(care_inputs[i]); lv_obj_add_event_cb(care_inputs[i], records_input_event, LV_EVENT_ALL, NULL);
        }
    } else {
        records_input = lv_textarea_create(records_overlay);
        lv_obj_set_size(records_input, lv_pct(100), 40); lv_obj_align(records_input, LV_ALIGN_TOP_LEFT, 0, 63);
        lv_textarea_set_one_line(records_input, true); lv_textarea_set_max_length(records_input, 500);
        lv_textarea_set_placeholder_text(records_input, "新增或修改内容；记忆点保存即同意记录");
        qz_style_textarea(records_input); lv_obj_add_event_cb(records_input, records_input_event, LV_EVENT_ALL, NULL);
    }
    const char *labels[] = {"新增", "修改", "删除", "切换完成", "下一条", "刷新"};
    for (int i = 0; i < (care_mode ? 2 : 6); i++) {
        record_controls[i] = qz_button(records_overlay, care_mode ? (i ? "关闭关心" : "同意开启") : labels[i], 88, 34);
        lv_obj_align(record_controls[i], LV_ALIGN_TOP_LEFT, (i % 3) * 94, 122 + (i / 3) * 38);
        lv_obj_add_event_cb(record_controls[i], records_action, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if (!care_mode && i == 3 && strcmp(kind, "todo") != 0) lv_obj_add_flag(record_controls[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_t *close = qz_button(records_overlay, "返回", 100, 36); lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(close, records_close, LV_EVENT_CLICKED, NULL);
    records_keyboard = lv_keyboard_create(lv_layer_top()); lv_obj_set_size(records_keyboard, lv_pct(100), 130);
    lv_obj_align(records_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0); qz_style_keyboard(records_keyboard);
    lv_obj_add_flag(records_keyboard, LV_OBJ_FLAG_HIDDEN);
    if (care_mode) records_submit("/api/companion/care", "{\"action\":\"status\"}");
    else records_request("list");
}

static void companion_pick(lv_event_t *event)
{
    int index = companion_page * 4 + (int)(intptr_t)lv_event_get_user_data(event);
    if (index == 0 || index == 2 || index == 4 || index == 5) {
        records_open(index == 0 ? "memory" : index == 2 ? "care" : index == 4 ? "learning" : "todo"); return;
    }
    if (index == 3 || index == 7) {
        lv_obj_t *target = qz_applets_screen(index == 3 ? QZ_APPLET_POMODORO : QZ_APPLET_CONTROL);
        if (!target) { lv_label_set_text(companion_hint, "设备页面尚未初始化"); return; }
        companion_close(NULL); qz_screen_load(target, LV_SCR_LOAD_ANIM_MOVE_LEFT, QZ_DUR_SCREEN); return;
    }
    if (qz_assistant_prepare_text(companion_prompts[index])) companion_close(NULL);
    else lv_label_set_text(companion_hint, "请先处理聊天草稿或当前录音");
}

static void companion_page_changed(lv_event_t *event)
{
    (void)event;
    companion_page = 1 - companion_page;
    for (int i = 0; i < 4; i++)
        lv_label_set_text(lv_obj_get_child(companion_buttons[i], 0), companion_names[companion_page * 4 + i]);
    lv_label_set_text(companion_caption, companion_page ? "陪伴能力 2/2" : "陪伴能力 1/2");
}

static void companion_open(lv_event_t *event)
{
    (void)event;
    if (companion_overlay) return;
    companion_page = 0;
    companion_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(companion_overlay, lv_pct(94), lv_pct(94));
    lv_obj_center(companion_overlay);
    qz_style_plate(companion_overlay);
    lv_obj_set_style_pad_all(companion_overlay, 10, 0);
    lv_obj_clear_flag(companion_overlay, LV_OBJ_FLAG_SCROLLABLE);
    companion_caption = qz_text(companion_overlay, "陪伴能力 1/2", 16, qz_color(QZ_TEXT));
    lv_obj_align(companion_caption, LV_ALIGN_TOP_MID, 0, 0);
    companion_hint = qz_text(companion_overlay, "沿用主人格 · 选择后确认发送", 12, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_set_width(companion_hint, lv_pct(100));
    lv_obj_set_style_text_align(companion_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(companion_hint, LV_ALIGN_TOP_MID, 0, 30);
    for (int i = 0; i < 4; i++) {
        companion_buttons[i] = qz_button(companion_overlay, companion_names[i], qz_compact() ? 132 : 180, 44);
        lv_obj_align(companion_buttons[i], i % 2 ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, 0, 60 + (i / 2) * 50);
        lv_obj_add_event_cb(companion_buttons[i], companion_pick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    lv_obj_t *close = qz_button(companion_overlay, "关闭", 100, 40);
    lv_obj_align(close, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(close, companion_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *next = qz_button(companion_overlay, "换一页", 100, 40);
    lv_obj_align(next, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(next, companion_page_changed, LV_EVENT_CLICKED, NULL);
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
    lv_obj_set_style_radius(toolbar, 16, 0);
    lv_obj_set_style_border_width(toolbar, 0, 0);
    lv_obj_set_style_shadow_width(toolbar, 0, 0);
    lv_obj_set_style_pad_all(toolbar, 0, 0);
    lv_obj_clear_flag(toolbar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = qz_back_button(toolbar);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = qz_text(toolbar, "技能", 20, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 58, 0);
    lv_obj_t *companion = qz_button(toolbar, "陪伴", 68, 40);
    lv_obj_align(companion, LV_ALIGN_RIGHT_MID, -58, 0);
    lv_obj_add_event_cb(companion, companion_open, LV_EVENT_CLICKED, NULL);

    lv_obj_t *refresh = qz_icon_button(toolbar, LV_SYMBOL_REFRESH, qz_compact() ? 40 : 30);
    lv_obj_align(refresh, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_add_event_cb(refresh, refresh_clicked, LV_EVENT_CLICKED, NULL);

    summary_label = qz_text(skill_screen, "--", qz_compact() ? 16 : 12, qz_color(QZ_TEXT));
    lv_obj_align(summary_label, LV_ALIGN_TOP_LEFT, QZ_GUTTER + 4, 68);
    service_label = qz_text(skill_screen, "", 10, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_set_width(service_label, CONTENT_W - 8);
    lv_label_set_long_mode(service_label, LV_LABEL_LONG_DOT);
    lv_obj_set_height(service_label, 20);
    lv_obj_align(service_label, LV_ALIGN_TOP_LEFT, QZ_GUTTER + 4, 232);

    list_card = lv_obj_create(skill_screen);
    lv_obj_set_size(list_card, CONTENT_W, 144);
    lv_obj_align(list_card, LV_ALIGN_TOP_LEFT, QZ_GUTTER, 104);
    lv_obj_set_style_radius(list_card, 16, 0);
    lv_obj_set_style_bg_opa(list_card, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_card, 0, 0);
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
