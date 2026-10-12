#include "assistant.h"
#include "ai_face.h"
#include "applets.h"
#include "theme.h"
#include "qzdesk_core.h"
#include "web_client.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define KEYBOARD_H 132
#define CHAT_TOP 78
#define CHAT_AREA_H (QZ_DESIGN_H - CHAT_TOP)
#define COMPOSER_H 54
#define CHAT_LIST_H (QZ_DESIGN_H - CHAT_TOP - COMPOSER_H)

static lv_obj_t *screen;
static lv_obj_t *chat_view;
static lv_obj_t *face_view;
static lv_obj_t *chat_list;
static lv_obj_t *composer;
static lv_obj_t *message_input;
static lv_obj_t *send_button;
static lv_obj_t *keyboard_button;
static lv_obj_t *keyboard;
static lv_obj_t *big_face;
static lv_obj_t *face_caption;
static lv_obj_t *face_hint;
static lv_obj_t *mode_button;
static lv_obj_t *mode_button_text;
static lv_obj_t *mode_button_icon;
static lv_obj_t *state_label;
static lv_obj_t *state_dot;
static lv_obj_t *desktop_screen;
static lv_obj_t *typing_bubble;
static lv_timer_t *typing_timer;

static void back_to_desktop(lv_event_t *event);
static qz_face_state_t current_face_state = QZ_FACE_IDLE;
static bool face_mode;
static bool talking;
static uint32_t talking_started;
static int32_t typing_phase;
static uint32_t request_started_at;
static unsigned long long pending_text_id;
static uint32_t pending_text_at;
static char pending_draft[241];

/* ------------------------------------------------------------------------- *
 * Helpers
 * ------------------------------------------------------------------------- */

static void stop_typing(void)
{
    if (typing_timer) {
        lv_timer_delete(typing_timer);
        typing_timer = NULL;
    }
    if (typing_bubble) {
        lv_obj_delete(typing_bubble);
        typing_bubble = NULL;
    }
}

static void set_caption(const char *status, const char *caption)
{
    if (state_label) lv_label_set_text(state_label, status);
    if (face_caption) lv_label_set_text(face_caption, caption);
    if (state_dot && state_label) {
        bool online = strcmp(status, "离线") != 0 && strcmp(status, "待激活") != 0;
        lv_obj_set_style_bg_color(state_dot, qz_color(online ? QZ_GREEN : QZ_ORANGE), 0);
        lv_obj_set_style_opa(state_label, online ? LV_OPA_COVER : LV_OPA_60, 0);
    }
}

static void apply_face_state(qz_face_state_t state)
{
    current_face_state = state;
    if (big_face) qz_face_set_state(big_face, state);
}

/* ------------------------------------------------------------------------- *
 * Chat bubbles
 * ------------------------------------------------------------------------- */

static lv_obj_t *bubble_row(bool from_user)
{
    lv_obj_t *row = lv_obj_create(chat_list);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, from_user ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    return row;
}

static lv_obj_t *add_bubble(const char *text, bool from_user)
{
    if (!chat_list || !text || text[0] == '\0') return NULL;

    bool follow = lv_obj_get_scroll_bottom(chat_list) < 24;
    while (lv_obj_get_child_count(chat_list) >= 100) {
        lv_obj_delete(lv_obj_get_child(chat_list, 0));
    }
    lv_obj_t *row = bubble_row(from_user);
    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_set_size(bubble, lv_pct(90), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_left(bubble, 11, 0);
    lv_obj_set_style_pad_right(bubble, 11, 0);
    lv_obj_set_style_pad_top(bubble, 8, 0);
    lv_obj_set_style_pad_bottom(bubble, 8, 0);
    lv_obj_set_style_radius(bubble, 14, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    if (from_user) {
        qz_obj_set_bg_color(bubble, QZ_ACCENT, 0);
        lv_obj_set_style_bg_grad_dir(bubble, LV_GRAD_DIR_NONE, 0);
        lv_obj_set_style_shadow_width(bubble, 0, 0);
    } else {
        /* Opaque fill: a translucent bubble would blend against the backdrop on
         * every scroll frame, and the rim already carries the material. */
        qz_obj_set_bg_color(bubble, QZ_CARD, 0);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bubble, 1, 0);
        qz_obj_set_border_color(bubble, QZ_GLASS_RIM, 0);
        lv_obj_set_style_border_opa(bubble, (lv_opa_t)200, 0);
        /* No shadow on bubbles: the rim reads as material and a soft blur per
         * bubble is the most expensive thing in a long scrolled list. */
        lv_obj_set_style_shadow_width(bubble, 0, 0);
    }
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = qz_text(bubble, text, 17, qz_color(from_user ? QZ_TEXT_ON_ACCENT : QZ_TEXT));
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(label, 4, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_update_layout(chat_list);
    if (follow || from_user) lv_obj_scroll_to_view(row, LV_ANIM_ON);
    return bubble;
}

static void typing_pulse(lv_timer_t *timer)
{
    lv_obj_t *row = (lv_obj_t *)lv_timer_get_user_data(timer);
    if (!row) return;
    lv_obj_t *bubble = lv_obj_get_child(row, 0);
    if (!bubble) return;
    /* Child 0 is the caption, children 1..3 are the animated dots. */
    for (int32_t i = 0; i < 3; i++) {
        lv_obj_t *dot = lv_obj_get_child(bubble, i + 1);
        if (!dot) return;
        lv_obj_set_style_opa(dot, i == typing_phase ? LV_OPA_COVER : (lv_opa_t)70, 0);
    }
    typing_phase = (typing_phase + 1) % 3;
}

static void add_typing_bubble(void)
{
    stop_typing();
    if (!chat_list) return;

    lv_obj_t *row = bubble_row(false);
    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_set_size(bubble, LV_SIZE_CONTENT, 34);
    lv_obj_set_style_pad_left(bubble, 12, 0);
    lv_obj_set_style_pad_right(bubble, 12, 0);
    lv_obj_set_style_pad_column(bubble, 5, 0);
    lv_obj_set_style_radius(bubble, 14, 0);
    qz_obj_set_bg_color(bubble, QZ_CARD, 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bubble, 1, 0);
    qz_obj_set_border_color(bubble, QZ_GLASS_RIM, 0);
    lv_obj_set_style_border_opa(bubble, (lv_opa_t)200, 0);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bubble, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_CLICKABLE);

    qz_text(bubble, "思考中", 12, qz_color(QZ_TEXT_SECONDARY));
    for (int i = 0; i < 3; i++) {
        lv_obj_t *dot = lv_obj_create(bubble);
        lv_obj_set_size(dot, 5, 5);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        qz_obj_set_bg_color(dot, QZ_ACCENT, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_opa(dot, (lv_opa_t)70, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    }
    typing_phase = 0;
    typing_bubble = row;
    typing_timer = lv_timer_create(typing_pulse, 320, row);
    lv_obj_scroll_to_view(row, LV_ANIM_ON);
}

/* ------------------------------------------------------------------------- *
 * Core events
 * ------------------------------------------------------------------------- */

static void qzdesk_core_event(const qzdesk_core_event_t *event, void *user_data)
{
    (void)user_data;
    if (!event) return;
    if (event->type == QZDESK_CORE_EVENT_ACK && strcmp(event->command, "new_chat") == 0) {
        stop_typing();
        add_bubble(event->text, false);
        return;
    }
    if (event->type == QZDESK_CORE_EVENT_ACK && pending_text_id &&
        strcmp(event->command, "chat_text") == 0 && event->request_id == pending_text_id) {
        pending_text_id = 0;
        lv_obj_remove_state(send_button, LV_STATE_DISABLED);
        if (event->ok) {
            if (strcmp(lv_textarea_get_text(message_input), pending_draft) == 0)
                lv_textarea_set_text(message_input, "");
            set_caption("已接收", "正在合成语音");
        } else {
            add_bubble(event->text[0] ? event->text : "消息未接收，草稿已保留，请重试", false);
            set_caption("未接收", "草稿已保留，可点击发送重试");
        }
        return;
    }
    static unsigned long long seen_ids[100];
    static unsigned int seen_index;
    if (event->type == QZDESK_CORE_EVENT_CHAT && event->id) {
        for (unsigned int i = 0; i < 100; i++) {
            if (seen_ids[i] == event->id) return;
        }
        seen_ids[seen_index++ % 100] = event->id;
    }
    if (event->type == QZDESK_CORE_EVENT_STATUS) {
        /* A status datagram is the handshake. Values 0/1/2 are valid core
         * startup or activation states and must not be rendered as offline. */
        request_started_at = 0;
        /* 首次握手成功说明核心在监听：这时补一次历史，屏幕上就能接着显示之前
         * 的对话（包括在网页控制台里进行的那些）。只问一次，之后靠实时推送。 */
        static bool history_requested;
        if (!history_requested) {
            history_requested = true;
            qzdesk_core_request_history();
        }
        const char *status = "离线";
        const char *caption = "本地服务暂未连接";
        if (event->state == 3) {
            status = "在线";
            caption = qz_face_state_text(QZ_FACE_IDLE);
            if (!talking) apply_face_state(QZ_FACE_IDLE);
        } else if (event->state == 5) {
            status = talking ? "聆听中" : "语音处理中";
            caption = talking ? "我在听你说，松开结束" : "等待语音处理结果";
            if (!talking) apply_face_state(QZ_FACE_IDLE);
        } else if (event->state == 6) {
            status = "回复中";
            caption = qz_face_state_text(QZ_FACE_SPEAKING);
            apply_face_state(QZ_FACE_SPEAKING);

        } else {
            status = event->state == 2 ? "待激活" : event->state == 4 ? "重连中" : "启动中";
            caption = event->state == 2 ? "请先完成设备激活" : "正在连接云端服务";
            apply_face_state(QZ_FACE_THINKING);
        }
        set_caption(status, caption);
    } else if (event->type == QZDESK_CORE_EVENT_CHAT && event->text[0] != '\0') {
        /* 聊天记录统一由核心推过来（用户打字、语音识别、助手回复、系统提示），
         * 内容与顺序和网页控制台上看到的是同一份；去重也在核心做，这里不再判断。 */
        bool from_user = strcmp(event->role, "user") == 0;
        stop_typing();
        if (strcmp(event->role, "divider") == 0) {
            lv_obj_t *row = bubble_row(false);
            lv_obj_set_style_pad_top(row, 8, 0);
            lv_obj_set_style_pad_bottom(row, 8, 0);
            lv_obj_t *left = lv_obj_create(row);
            lv_obj_set_size(left, 32, 1);
            lv_obj_set_style_border_width(left, 0, 0);
            lv_obj_set_style_bg_color(left, qz_color(QZ_TEXT_TERTIARY), 0);
            lv_obj_t *label = qz_text(row, "新聊天", 16, qz_color(QZ_TEXT_SECONDARY));
            (void)label;
            lv_obj_t *right = lv_obj_create(row);
            lv_obj_set_size(right, 32, 1);
            lv_obj_set_style_border_width(right, 0, 0);
            lv_obj_set_style_bg_color(right, qz_color(QZ_TEXT_TERTIARY), 0);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(row, 12, 0);
            lv_obj_scroll_to_view(row, LV_ANIM_OFF);
        } else if (strcmp(event->role, "system") == 0) {
            char notice[sizeof(event->text) + 32];
            snprintf(notice, sizeof(notice), "系统提示：%s", event->text);
            add_bubble(notice, false);
        } else add_bubble(event->text, from_user);
        if (strcmp(event->role, "assistant") == 0) {
            set_caption("回复中", qz_face_state_text(QZ_FACE_SPEAKING));
            apply_face_state(QZ_FACE_SPEAKING);

        }
    } else if (event->type == QZDESK_CORE_EVENT_TOAST && event->text[0] != '\0') {
        stop_typing();
        add_bubble(event->text, false);
    } else if (event->type == QZDESK_CORE_EVENT_ACTIVATION && event->text[0] != '\0') {
        static char last_activation[64];
        if (strcmp(last_activation, event->text) != 0) {
            char activation[300];
            snprintf(last_activation, sizeof(last_activation), "%.63s", event->text);
            snprintf(activation, sizeof(activation), "请在手机端输入激活码：%.*s",
                     (int)sizeof(activation) - 35, event->text);
            add_bubble(activation, false);
        }
        set_caption("待激活", "先完成设备激活吧");
        apply_face_state(QZ_FACE_THINKING);
    }
}

static void poll_qzdesk_core(lv_timer_t *timer)
{
    (void)timer;
    static bool status_requested;
    static uint32_t last_retry_at;
    const uint32_t now = lv_tick_get();
    if (pending_text_id && lv_tick_elaps(pending_text_at) >= 4000) {
        pending_text_id = 0;
        lv_obj_remove_state(send_button, LV_STATE_DISABLED);
        add_bubble("接收结果未确认，草稿已保留；请先查看聊天记录，避免重复发送。", false);
        set_caption("未确认", "请检查聊天记录后重试");
    }
    if (talking && lv_tick_elaps(talking_started) >= 60000) {
        talking = false;
        if (!qzdesk_core_listen(false)) set_caption("录音失败", "结束指令未发送，请重试");
        else set_caption("录音结束", "已达到 60 秒录音上限");
    }
    if (status_requested && now - last_retry_at >= 5000) {
        qzdesk_core_request_status();
        last_retry_at = now;
    }
    if (!qzdesk_core_open()) {
        status_requested = false;
        request_started_at = 0;
        last_retry_at = now;
        set_caption("离线", "本地服务暂未连接");
        return;
    }
    if (!status_requested || (request_started_at != 0 && now - request_started_at > 2200)) {
        /* UDP has no connection handshake. Repeat the probe so a core that is
         * still starting, or was restarted, can become visible to the UI. */
        if (!status_requested || now - last_retry_at >= 1000) {
            qzdesk_core_request_status();
            last_retry_at = now;
        }
        if (!status_requested) {
            request_started_at = now;
            set_caption("连接中", qz_face_state_text(QZ_FACE_THINKING));
            apply_face_state(QZ_FACE_THINKING);
        } else if (request_started_at != 0 && now - request_started_at > 2200) {
            request_started_at = now;
            set_caption("重试中", "正在等待本地服务");
        }
        status_requested = true;
    }
    qzdesk_core_poll(qzdesk_core_event, NULL);
}

/* ------------------------------------------------------------------------- *
 * Input
 * ------------------------------------------------------------------------- */

static void send_message(lv_event_t *event)
{
    (void)event;
    if (pending_text_id) return;
    const char *text = lv_textarea_get_text(message_input);
    if (!text || text[0] == '\0') return;
    if (strlen(text) > 240) {
        add_bubble("消息最多 240 字节，请缩短后发送；草稿已保留。", false);
        return;
    }
    stop_typing();
    /* 不在本地先画气泡：核心会把这条记成用户消息再推回来，网页控制台与设备
     * 屏幕因此显示的是同一条记录，不会出现一边有、一边没有。 */
    if (!qzdesk_core_send_text(text)) {
        /* Only a transport failure can land here now: the core owns the
         * "text -> speech" step and reports its own errors as toasts. */
        add_bubble("核心未连接，这条消息没有发出去。可以先用语音输入，或在核心启动后重试。", false);
        set_caption("未连接", "核心未运行");
        apply_face_state(QZ_FACE_IDLE);
    } else {
        snprintf(pending_draft, sizeof(pending_draft), "%s", text);
        pending_text_id = qzdesk_core_text_request_id();
        pending_text_at = lv_tick_get();
        lv_obj_add_state(send_button, LV_STATE_DISABLED);
        set_caption("发送中", "等待核心确认接收");
        apply_face_state(QZ_FACE_THINKING);
    }
}

static void scroll_to_latest(void)
{
    int32_t count = lv_obj_get_child_count(chat_list);
    if (count <= 0) return;
    lv_obj_update_layout(chat_list);
    lv_obj_scroll_to_view(lv_obj_get_child(chat_list, count - 1), LV_ANIM_ON);
}

static void set_keyboard_visible(bool visible)
{
    if (!keyboard || !message_input) return;
    if (visible) {
        lv_obj_add_state(message_input, LV_STATE_FOCUSED);
        lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(composer, LV_ALIGN_BOTTOM_MID, 0, -KEYBOARD_H);
        lv_obj_set_height(chat_list, CHAT_LIST_H - KEYBOARD_H);
        scroll_to_latest();
    } else {
        lv_obj_remove_state(message_input, LV_STATE_FOCUSED);
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(composer, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_height(chat_list, CHAT_LIST_H);
        scroll_to_latest();
    }
}

static void toggle_keyboard(lv_event_t *event)
{
    (void)event;
    bool hidden = lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    set_keyboard_visible(hidden);
}

static void input_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_FOCUSED) {
        lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(composer, LV_ALIGN_BOTTOM_MID, 0, -KEYBOARD_H);
        lv_obj_set_height(chat_list, CHAT_LIST_H - KEYBOARD_H);
        scroll_to_latest();
    } else if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(composer, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_height(chat_list, CHAT_LIST_H);
        scroll_to_latest();
    } else if (code == LV_EVENT_READY) {
        lv_obj_clear_state(message_input, LV_STATE_FOCUSED);
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(composer, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_height(chat_list, CHAT_LIST_H);
        send_message(event);
        scroll_to_latest();
    }
}

/* ------------------------------------------------------------------------- *
 * Mode switch
 * ------------------------------------------------------------------------- */

static void update_mode_button(void)
{
    if (!mode_button_text) return;
    lv_label_set_text(mode_button_text, face_mode ? "聊天" : "表情");
    if (mode_button_icon) lv_obj_delete(mode_button_icon);
    mode_button_icon = qz_symbol(mode_button, face_mode ? LV_SYMBOL_KEYBOARD : LV_SYMBOL_IMAGE,
                                  18, qz_color(QZ_ACCENT_TEXT));
    lv_obj_align(mode_button_icon, LV_ALIGN_LEFT_MID, 10, 0);
}

static void show_mode(bool show_face)
{
    if (face_mode == show_face) return;
    face_mode = show_face;

    if (show_face) {
        lv_obj_remove_state(message_input, LV_STATE_FOCUSED);
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(composer, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_height(chat_list, CHAT_LIST_H);
        lv_obj_add_flag(chat_view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(face_view, LV_OBJ_FLAG_HIDDEN);
        qz_animate_pop_in(face_view);
        apply_face_state(current_face_state);
    } else {
        lv_obj_add_flag(face_view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(chat_view, LV_OBJ_FLAG_HIDDEN);
        qz_animate_pop_in(chat_view);
    }
    update_mode_button();
}

static void toggle_mode(lv_event_t *event)
{
    (void)event;
    show_mode(!face_mode);
}

/* ------------------------------------------------------------------------- *
 * Voice button
 * ------------------------------------------------------------------------- */

static void talk_pressed(lv_event_t *event)
{
    (void)event;
    if (talking) return;
    if (!qzdesk_core_listen(true)) {
        set_caption("离线", "录音未开始，请检查本地服务");
        return;
    }
    talking = true;
    talking_started = lv_tick_get();
    set_caption("准备录音", "等待核心开启麦克风");
    qzdesk_core_request_status();
}

static void talk_released(lv_event_t *event)
{
    (void)event;
    if (!talking) return;
    talking = false;
    if (!qzdesk_core_listen(false)) set_caption("失败", "结束录音指令未发送");
    if (face_caption) lv_label_set_text(face_caption, qz_face_state_text(current_face_state));
}

/* ------------------------------------------------------------------------- *
 * Building blocks
 * ------------------------------------------------------------------------- */

static lv_obj_t *mic_glyph(lv_obj_t *parent, int32_t size, lv_color_t color)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, size, size);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    int32_t thickness = size * 8 / 100;
    int32_t capsule_w = size * 32 / 100;
    int32_t capsule_h = size * 50 / 100;
    int32_t cradle = size * 60 / 100;

    lv_obj_t *stem = lv_obj_create(box);
    lv_obj_set_size(stem, thickness, size * 16 / 100);
    lv_obj_align(stem, LV_ALIGN_TOP_MID, 0, size * 72 / 100);
    lv_obj_set_style_radius(stem, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(stem, color, 0);
    lv_obj_set_style_border_width(stem, 0, 0);
    lv_obj_clear_flag(stem, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *base = lv_obj_create(box);
    lv_obj_set_size(base, size * 40 / 100, thickness);
    lv_obj_align(base, LV_ALIGN_TOP_MID, 0, size * 88 / 100);
    lv_obj_set_style_radius(base, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(base, color, 0);
    lv_obj_set_style_border_width(base, 0, 0);
    lv_obj_clear_flag(base, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *arc = qz_arc_piece(box, cradle, cradle, 0, 180, thickness, color);
    lv_obj_align(arc, LV_ALIGN_TOP_MID, 0, size * 16 / 100);

    lv_obj_t *capsule = lv_obj_create(box);
    lv_obj_set_size(capsule, capsule_w, capsule_h);
    lv_obj_align(capsule, LV_ALIGN_TOP_MID, 0, size * 8 / 100);
    lv_obj_set_style_radius(capsule, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(capsule, color, 0);
    lv_obj_set_style_border_width(capsule, 0, 0);
    lv_obj_clear_flag(capsule, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(capsule, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

static lv_obj_t *memory_screen;
static lv_obj_t *memory_list;
static lv_obj_t *memory_input;
static lv_obj_t *memory_status;
static lv_obj_t *memory_keyboard;
static lv_obj_t *memory_save;
static lv_obj_t *memory_delete;
static unsigned long long memory_selected;
static bool memory_busy;

static void memory_request(const char *body, bool save);

static void memory_back(lv_event_t *event)
{
    (void)event;
    lv_obj_send_event(memory_keyboard, LV_EVENT_CANCEL, NULL);
    qz_screen_load(screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, QZ_DUR_SCREEN);
}

static void memory_select(lv_event_t *event)
{
    memory_selected = (unsigned long long)(uintptr_t)lv_event_get_user_data(event);
    lv_label_set_text(memory_status, "已选择，点击删除确认忘记");
    if (!memory_busy) lv_obj_remove_state(memory_delete, LV_STATE_DISABLED);
}

static void memory_done(bool ok, const char *response, void *user_data)
{
    bool saved = (uintptr_t)user_data != 0;
    memory_busy = false;
    lv_obj_remove_state(memory_save, LV_STATE_DISABLED);
    if (!ok) {
        char error[256];
        if (!qz_json_string(response, "error", error, sizeof(error)))
            snprintf(error, sizeof(error), "本地服务不可达，请稍后重试");
        lv_label_set_text(memory_status, error);
        if (memory_selected) lv_obj_remove_state(memory_delete, LV_STATE_DISABLED);
        return;
    }
    if (!qz_json_value(response, "records")) {
        lv_label_set_text(memory_status, "响应不完整，请重新打开记忆页");
        return;
    }
    if (saved) lv_textarea_set_text(memory_input, "");
    memory_selected = 0;
    lv_obj_add_state(memory_delete, LV_STATE_DISABLED);
    lv_obj_clean(memory_list);
    int count = 0;
    const char *item;
    while ((item = qz_json_item(response, "records", count)) != NULL) {
        char text[8192];
        if (!qz_json_string(item, "text", text, sizeof(text))) break;
        const char *id_value = qz_json_value(item, "id");
        unsigned long long id = id_value ? strtoull(id_value, NULL, 10) : 0;
        lv_obj_t *row = qz_card_button(memory_list, QZ_DESIGN_W - 44, 64);
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, 10, 0);
        lv_obj_t *label = qz_text(row, text, 18, qz_color(QZ_TEXT));
        lv_obj_set_width(label, lv_pct(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_obj_add_event_cb(row, memory_select, LV_EVENT_CLICKED, (void *)(uintptr_t)id);
        count++;
    }
    if (!count) qz_text(memory_list, "还没有记忆，填写后确认保存", 18, qz_color(QZ_TEXT_SECONDARY));
    char status[96];
    snprintf(status, sizeof(status), "%s · %d 条记忆", saved ? "已保存到本机" : "仅保存在本机，点选后可删除", count);
    lv_label_set_text(memory_status, status);
}

static void memory_request(const char *body, bool save)
{
    if (memory_busy) return;
    memory_busy = true;
    lv_obj_add_state(memory_save, LV_STATE_DISABLED);
    lv_obj_add_state(memory_delete, LV_STATE_DISABLED);
    lv_label_set_text(memory_status, "正在处理本地记忆…");
    if (!qz_web_request_async("POST", "/api/companion/records", body,
                              memory_done, (void *)(uintptr_t)save))
        memory_done(false, "", NULL);
}

static void memory_save_clicked(lv_event_t *event)
{
    (void)event;
    char quoted[8192], body[8400];
    const char *text = lv_textarea_get_text(memory_input);
    if (!text[0]) { lv_label_set_text(memory_status, "请先填写需要记住的内容"); return; }
    if (!qz_json_quote(text, quoted, sizeof(quoted))) {
        lv_label_set_text(memory_status, "内容太长，请缩短后再保存"); return;
    }
    snprintf(body, sizeof(body), "{\"action\":\"add\",\"kind\":\"memory\",\"confirmed\":true,\"text\":%s}", quoted);
    memory_request(body, true);
}

static void memory_delete_clicked(lv_event_t *event)
{
    (void)event;
    if (!memory_selected) return;
    char body[160];
    snprintf(body, sizeof(body), "{\"action\":\"delete\",\"kind\":\"memory\",\"id\":%llu}", memory_selected);
    memory_request(body, false);
}

static void memory_edit(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    bool editing = code == LV_EVENT_FOCUSED;
    if (!editing && code != LV_EVENT_READY && code != LV_EVENT_CANCEL) return;
    if (editing) {
        lv_obj_clear_flag(memory_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(memory_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(memory_input, LV_ALIGN_TOP_MID, 0, 60);
        lv_obj_align(memory_save, LV_ALIGN_TOP_LEFT, 14, 126);
        lv_obj_align(memory_delete, LV_ALIGN_TOP_RIGHT, -14, 126);
    } else {
        lv_obj_add_flag(memory_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(memory_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(memory_input, LV_ALIGN_TOP_MID, 0, 194);
        lv_obj_align(memory_save, LV_ALIGN_TOP_LEFT, 14, 254);
        lv_obj_align(memory_delete, LV_ALIGN_TOP_RIGHT, -14, 254);
        lv_obj_remove_state(memory_input, LV_STATE_FOCUSED);
    }
}

static void open_memory(lv_event_t *event)
{
    (void)event;
    if (!memory_screen) {
        memory_screen = lv_obj_create(NULL);
        qz_style_screen(memory_screen);
        lv_obj_t *toolbar = lv_obj_create(memory_screen);
        lv_obj_set_size(toolbar, QZ_DESIGN_W - 16, 46);
        lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 6);
        qz_style_toolbar(toolbar);
        lv_obj_t *back = qz_back_button(toolbar);
        lv_obj_align(back, LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_add_event_cb(back, memory_back, LV_EVENT_CLICKED, NULL);
        lv_obj_t *title = qz_text(toolbar, "本地记忆", 22, qz_color(QZ_TEXT));
        lv_obj_center(title);
        memory_list = lv_obj_create(memory_screen);
        lv_obj_set_size(memory_list, QZ_DESIGN_W - 20, 130);
        lv_obj_align(memory_list, LV_ALIGN_TOP_MID, 0, 58);
        lv_obj_set_style_bg_opa(memory_list, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(memory_list, 0, 0);
        lv_obj_set_style_pad_all(memory_list, 6, 0);
        lv_obj_set_style_pad_row(memory_list, 8, 0);
        lv_obj_set_flex_flow(memory_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(memory_list, LV_DIR_VER);
        memory_input = lv_textarea_create(memory_screen);
        lv_obj_set_size(memory_input, QZ_DESIGN_W - 28, 48);
        lv_obj_align(memory_input, LV_ALIGN_TOP_MID, 0, 194);
        qz_style_textarea(memory_input);
        lv_textarea_set_one_line(memory_input, true);
        lv_textarea_set_max_length(memory_input, 1000);
        lv_textarea_set_placeholder_text(memory_input, "填写偏好，确认后才会保存");
        lv_obj_add_event_cb(memory_input, memory_edit, LV_EVENT_FOCUSED, NULL);
        memory_save = qz_card_button(memory_screen, 218, 44);
        lv_obj_align(memory_save, LV_ALIGN_TOP_LEFT, 14, 254);
        lv_obj_t *save_text = qz_text(memory_save, "确认保存", 18, qz_color(QZ_ACCENT_TEXT));
        lv_obj_center(save_text);
        lv_obj_add_event_cb(memory_save, memory_save_clicked, LV_EVENT_CLICKED, NULL);
        memory_delete = qz_card_button(memory_screen, 218, 44);
        lv_obj_align(memory_delete, LV_ALIGN_TOP_RIGHT, -14, 254);
        lv_obj_t *delete_text = qz_text(memory_delete, "删除选中记忆", 18, qz_color(QZ_RED));
        lv_obj_center(delete_text);
        lv_obj_add_state(memory_delete, LV_STATE_DISABLED);
        lv_obj_add_event_cb(memory_delete, memory_delete_clicked, LV_EVENT_CLICKED, NULL);
        memory_status = qz_text(memory_screen, "正在读取…", 14, qz_color(QZ_TEXT_SECONDARY));
        lv_obj_set_width(memory_status, QZ_DESIGN_W - 28);
        lv_label_set_long_mode(memory_status, LV_LABEL_LONG_WRAP);
        lv_obj_align(memory_status, LV_ALIGN_TOP_LEFT, 14, 300);
        memory_keyboard = lv_keyboard_create(memory_screen);
        lv_obj_set_size(memory_keyboard, lv_pct(100), KEYBOARD_H);
        lv_obj_align(memory_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_keyboard_set_textarea(memory_keyboard, memory_input);
        qz_style_keyboard(memory_keyboard);
        lv_obj_add_event_cb(memory_keyboard, memory_edit, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(memory_keyboard, memory_edit, LV_EVENT_CANCEL, NULL);
        lv_obj_add_flag(memory_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    qz_screen_load(memory_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, QZ_DUR_SCREEN);
    memory_request("{\"action\":\"list\",\"kind\":\"memory\"}", false);
}

static void new_chat_clicked(lv_event_t *event)
{
    (void)event;
    if (talking || pending_text_id) {
        add_bubble("请等待当前消息处理完成后再新建聊天", false);
        return;
    }
    if (!qzdesk_core_new_chat()) add_bubble("本地核心不可达，请稍后重试", false);
    else set_caption("请求中", "等待核心受理新会话");
}

static lv_obj_t *toolbar_action(lv_obj_t *parent, int width, int offset, bool primary)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, width, 52);
    lv_obj_align(button, LV_ALIGN_RIGHT_MID, offset, 0);
    lv_obj_set_style_radius(button, 18, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_border_width(button, 1, 0);
    qz_obj_set_border_color(button, QZ_ACCENT, 0);
    lv_obj_set_style_border_opa(button, primary ? LV_OPA_TRANSP : LV_OPA_20, 0);
    qz_obj_set_bg_color(button, primary ? QZ_ACCENT : QZ_ACCENT_TINT, 0);
    qz_obj_set_bg_color(button, QZ_FILL_PRESSED, LV_STATE_PRESSED);
    qz_add_press_feedback(button);
    return button;
}

static void build_toolbar(void)
{
    lv_obj_t *toolbar = lv_obj_create(screen);
    lv_obj_set_size(toolbar, QZ_DESIGN_W - 16, 66);
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 6);
    qz_style_toolbar(toolbar);

    lv_obj_t *back = qz_back_button(toolbar);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_event_cb(back, back_to_desktop, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = qz_text(toolbar, "AI 助手", 18, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 58, 9);
    state_dot = lv_obj_create(toolbar);
    lv_obj_set_size(state_dot, 6, 6);
    lv_obj_align(state_dot, LV_ALIGN_TOP_LEFT, 58, 44);
    lv_obj_set_style_radius(state_dot, LV_RADIUS_CIRCLE, 0);
    qz_obj_set_bg_color(state_dot, QZ_ORANGE, 0);
    lv_obj_set_style_border_width(state_dot, 0, 0);
    lv_obj_clear_flag(state_dot, LV_OBJ_FLAG_CLICKABLE);
    state_label = qz_text(toolbar, "连接中", 16, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(state_label, LV_ALIGN_TOP_LEFT, 72, 36);

    lv_obj_t *new_chat = toolbar_action(toolbar, 150, -120, true);
    lv_obj_t *new_label = qz_text(new_chat, "新建聊天", 18, lv_color_white());
    lv_obj_center(new_label);
    lv_obj_add_event_cb(new_chat, new_chat_clicked, LV_EVENT_CLICKED, NULL);

    mode_button = toolbar_action(toolbar, 104, -8, false);
    mode_button_icon = qz_symbol(mode_button, LV_SYMBOL_IMAGE, 18, qz_color(QZ_ACCENT_TEXT));
    lv_obj_align(mode_button_icon, LV_ALIGN_LEFT_MID, 10, 0);
    mode_button_text = qz_text(mode_button, "表情", 17, qz_color(QZ_ACCENT_TEXT));
    lv_obj_align(mode_button_text, LV_ALIGN_LEFT_MID, 38, 0);
    lv_obj_add_event_cb(mode_button, toggle_mode, LV_EVENT_CLICKED, NULL);
}

static void build_chat_view(void)
{
    /* 聊天记录在固定的内容窗口内滚动；键盘打开时只缩短这块窗口。 */
    chat_view = lv_obj_create(screen);
    lv_obj_set_size(chat_view, lv_pct(100), QZ_DESIGN_H);
    lv_obj_align(chat_view, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(chat_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chat_view, 0, 0);
    lv_obj_set_style_pad_all(chat_view, 0, 0);
    lv_obj_clear_flag(chat_view, LV_OBJ_FLAG_SCROLLABLE);

    chat_list = lv_obj_create(chat_view);
    lv_obj_set_size(chat_list, lv_pct(100), CHAT_LIST_H);
    lv_obj_align(chat_list, LV_ALIGN_TOP_MID, 0, CHAT_TOP);
    lv_obj_set_style_bg_opa(chat_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chat_list, 0, 0);
    lv_obj_set_style_pad_left(chat_list, 12, 0);
    lv_obj_set_style_pad_right(chat_list, 12, 0);
    lv_obj_set_style_pad_top(chat_list, 6, 0);
    lv_obj_set_style_pad_bottom(chat_list, 6, 0);
    lv_obj_set_style_pad_row(chat_list, 8, 0);
    lv_obj_set_flex_flow(chat_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(chat_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_add_flag(chat_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(chat_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(chat_list, LV_SCROLLBAR_MODE_OFF);

    composer = lv_obj_create(chat_view);
    lv_obj_set_size(composer, lv_pct(100), COMPOSER_H);
    lv_obj_align(composer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(composer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(composer, 0, 0);
    lv_obj_set_style_pad_all(composer, 0, 0);
    lv_obj_clear_flag(composer, LV_OBJ_FLAG_SCROLLABLE);

    /* iOS 26 composer: one floating glass capsule holding the field and the
     * two round controls. */
    lv_obj_t *bar = lv_obj_create(composer);
    lv_obj_set_size(bar, QZ_DESIGN_W - 16, COMPOSER_H - 6);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, -3);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
    qz_style_glass(bar);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    message_input = lv_textarea_create(bar);
    lv_obj_set_size(message_input, 214, 44);
    lv_obj_align(message_input, LV_ALIGN_LEFT_MID, 8, 0);
    lv_textarea_set_one_line(message_input, true);
    lv_textarea_set_placeholder_text(message_input, "点击键盘输入，或按住麦克风说话");
    qz_style_textarea(message_input);
    lv_obj_add_event_cb(message_input, input_event, LV_EVENT_ALL, NULL);

    keyboard_button = lv_button_create(bar);
    lv_obj_set_size(keyboard_button, 38, 38);
    lv_obj_align(keyboard_button, LV_ALIGN_RIGHT_MID, -150, 0);
    lv_obj_set_style_radius(keyboard_button, LV_RADIUS_CIRCLE, 0);
    qz_obj_set_bg_color(keyboard_button, QZ_FILL, 0);
    qz_obj_set_bg_color(keyboard_button, QZ_FILL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(keyboard_button, 0, 0);
    lv_obj_set_style_pad_all(keyboard_button, 0, 0);
    qz_add_press_feedback(keyboard_button);
    lv_obj_t *keyboard_glyph = qz_symbol(keyboard_button, LV_SYMBOL_KEYBOARD, 17, qz_color(QZ_ACCENT_TEXT));
    lv_obj_center(keyboard_glyph);
    lv_obj_add_event_cb(keyboard_button, toggle_keyboard, LV_EVENT_CLICKED, NULL);

    lv_obj_t *mic = lv_button_create(bar);
    lv_obj_set_size(mic, 44, 44);
    lv_obj_align(mic, LV_ALIGN_RIGHT_MID, -62, 0);
    lv_obj_set_style_radius(mic, LV_RADIUS_CIRCLE, 0);
    qz_obj_set_bg_color(mic, QZ_FILL, 0);
    qz_obj_set_bg_color(mic, QZ_FILL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(mic, 0, 0);
    lv_obj_set_style_pad_all(mic, 0, 0);
    qz_add_press_feedback(mic);
    qz_add_press_scale(mic, 94);
    lv_obj_t *mic_box = mic_glyph(mic, 19, qz_color(QZ_ACCENT));
    lv_obj_center(mic_box);
    lv_obj_add_event_cb(mic, talk_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(mic, talk_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(mic, talk_released, LV_EVENT_PRESS_LOST, NULL);

    send_button = lv_button_create(bar);
    lv_obj_set_size(send_button, 44, 44);
    lv_obj_align(send_button, LV_ALIGN_RIGHT_MID, -8, 0);
    qz_style_primary_button(send_button);
    qz_add_press_scale(send_button, 94);
    lv_obj_t *arrow = qz_symbol(send_button, LV_SYMBOL_UP, 20, qz_color(QZ_TEXT_ON_ACCENT));
    lv_obj_center(arrow);
    lv_obj_add_event_cb(send_button, send_message, LV_EVENT_CLICKED, NULL);

    keyboard = lv_keyboard_create(screen);
    lv_obj_set_size(keyboard, lv_pct(100), KEYBOARD_H);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard, message_input);
    qz_style_keyboard(keyboard);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void build_face_view(void)
{
    face_view = lv_obj_create(screen);
    lv_obj_set_size(face_view, lv_pct(100), CHAT_AREA_H - 2);
    lv_obj_align(face_view, LV_ALIGN_TOP_MID, 0, CHAT_TOP);
    lv_obj_set_style_bg_opa(face_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(face_view, 0, 0);
    lv_obj_set_style_pad_all(face_view, 0, 0);
    lv_obj_clear_flag(face_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(face_view, LV_OBJ_FLAG_HIDDEN);

    big_face = qz_face_create(face_view, 160);
    lv_obj_align(big_face, LV_ALIGN_LEFT_MID, 24, -12);

    face_caption = qz_text(face_view, qz_face_state_text(QZ_FACE_IDLE), 20, qz_color(QZ_TEXT));
    lv_obj_align(face_caption, LV_ALIGN_TOP_LEFT, 210, 60);
    lv_obj_set_width(face_caption, 240);
    lv_label_set_long_mode(face_caption, LV_LABEL_LONG_WRAP);

    face_hint = qz_text(face_view, "右上角切回聊天", 16, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(face_hint, LV_ALIGN_TOP_LEFT, 210, 102);

    lv_obj_t *talk = lv_button_create(face_view);
    lv_obj_set_size(talk, 240, 52);
    lv_obj_align(talk, LV_ALIGN_TOP_LEFT, 210, 156);
    lv_obj_set_style_radius(talk, LV_RADIUS_CIRCLE, 0);
    qz_obj_set_bg_color(talk, QZ_ACCENT, 0);
    lv_obj_set_style_bg_grad_dir(talk, LV_GRAD_DIR_NONE, 0);
    qz_obj_set_bg_color(talk, QZ_ACCENT_DARK, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(talk, 0, 0);
    lv_obj_set_style_pad_all(talk, 0, 0);
    /* Flat fill, no shadow: the reference keeps depth to value contrast. */
    lv_obj_set_style_shadow_width(talk, 0, 0);
    qz_add_press_feedback(talk);
    qz_add_press_scale(talk, 97);
    mic_glyph(talk, 21, qz_color(QZ_TEXT_ON_ACCENT));
    lv_obj_t *talk_text = qz_text(talk, "长按说话", 17, qz_color(QZ_TEXT_ON_ACCENT));
    lv_obj_align(talk_text, LV_ALIGN_CENTER, 14, 0);
    lv_obj_t *mic_box = lv_obj_get_child(talk, 0);
    lv_obj_align(mic_box, LV_ALIGN_CENTER, -46, 0);
    lv_obj_add_event_cb(talk, talk_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(talk, talk_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(talk, talk_released, LV_EVENT_PRESS_LOST, NULL);
}

static void back_to_desktop(lv_event_t *event)
{
    (void)event;
    if (keyboard && !lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_state(message_input, LV_STATE_FOCUSED);
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(composer, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_height(chat_list, CHAT_LIST_H);
        return;
    }
    if (talking) {
        talking = false;
        qzdesk_core_listen(false);
    }
    stop_typing();
    qz_screen_load(desktop_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, QZ_DUR_SCREEN);
}

/* ------------------------------------------------------------------------- *
 * Screen
 * ------------------------------------------------------------------------- */

lv_obj_t *qz_assistant_create(void)
{
    screen = lv_obj_create(NULL);
    qz_style_screen(screen);

    build_toolbar();
    build_chat_view();
    build_face_view();
    /* The bar is built first, so the list would paint over it: lift it back on
     * top now that everything behind it exists. */
    lv_obj_move_foreground(lv_obj_get_child(screen, 0));
    face_mode = false;

    set_caption("连接中", qz_face_state_text(QZ_FACE_IDLE));
    lv_timer_create(poll_qzdesk_core, 100, NULL);
    return screen;
}

bool qz_assistant_prepare_text(const char *text)
{
    if (!screen || pending_text_id || talking || !text) return false;
    if (lv_textarea_get_text(message_input)[0]) return false;
    lv_textarea_set_text(message_input, text);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(composer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_height(chat_list, CHAT_LIST_H);
    show_mode(false);
    qz_screen_load(screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, QZ_DUR_SCREEN);
    return true;
}

void qz_assistant_set_desktop(lv_obj_t *desktop) { desktop_screen = desktop; }
