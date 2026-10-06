#include "assistant.h"
#include "ai_face.h"
#include "applets.h"
#include "theme.h"
#include "qzdesk_core.h"
#include <stdio.h>
#include <string.h>

#define KEYBOARD_H 140
#define CHAT_AREA_H (QZ_DESIGN_H - QZ_TOOLBAR_H)
#define COMPOSER_H 48
#define CHAT_LIST_H (CHAT_AREA_H - COMPOSER_H)

static lv_obj_t *screen;
static lv_obj_t *chat_view;
static lv_obj_t *face_view;
static lv_obj_t *chat_list;
static lv_obj_t *composer;
static lv_obj_t *message_input;
static lv_obj_t *send_button;
static lv_obj_t *keyboard;
static lv_obj_t *big_face;
static lv_obj_t *face_caption;
static lv_obj_t *face_hint;
static lv_obj_t *mode_button;
static lv_obj_t *mode_button_text;
static lv_obj_t *state_label;
static lv_obj_t *state_dot;
static lv_obj_t *desktop_screen;
static lv_obj_t *typing_bubble;
static lv_timer_t *typing_timer;
static lv_timer_t *state_timer;

static void back_to_desktop(lv_event_t *event);
static qz_face_state_t current_face_state = QZ_FACE_IDLE;
static bool face_mode;
static bool talking;
static int32_t typing_phase;
static uint32_t request_started_at;

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

    lv_obj_t *row = bubble_row(from_user);
    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(bubble, lv_pct(84), 0);
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

    lv_obj_t *label = qz_text(bubble, text, 13, qz_color(from_user ? QZ_TEXT_ON_ACCENT : QZ_TEXT));
    lv_obj_set_width(label, LV_SIZE_CONTENT);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(label, 4, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_update_layout(chat_list);
    lv_obj_scroll_to_view(row, LV_ANIM_ON);
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

static qz_face_state_t settle_target = QZ_FACE_IDLE;

static void state_settle(lv_timer_t *timer)
{
    (void)timer;
    state_timer = NULL;
    if (talking) return;
    apply_face_state(settle_target);
    if (face_caption) lv_label_set_text(face_caption, qz_face_state_text(settle_target));
}

/** Return to `target` after the expression has been shown long enough. */
static void arm_state_timer(uint32_t delay_ms, qz_face_state_t target)
{
    settle_target = target;
    if (state_timer) {
        lv_timer_delete(state_timer);
        state_timer = NULL;
    }
    state_timer = lv_timer_create(state_settle, delay_ms, NULL);
    lv_timer_set_repeat_count(state_timer, 1);
}

/** 首次握手补历史的时间戳：补发窗口内不触发下面的自动跳页。 */
static uint32_t history_started_at;

/**
 * 助手回复宣布「开始番茄钟」时自动跳到番茄钟页。
 *
 * 触发条件刻意保守：消息要同时含「番茄钟 / pomodoro」和「开始」——语音链路
 * 的「% pomodoro…番茄钟开始啦，专注25分钟！」与核心自己写的播报都认得出；
 * 而阶段性与收尾的播报（「休息结束，开始 25 分钟专注」不含番茄钟、「番茄钟
 * 已结束 / 完成」不含开始）都不会误跳。番茄钟页建好时会自己与核心对一次表，
 * 跳过去看到的就是当前倒计时。
 */
static void maybe_jump_pomodoro(const char *text)
{
    lv_obj_t *pomo;

    /* GUI 连上核心时会补发最近聊天记录，协议上与实时消息无法区分；历史里若有
     * 旧的「番茄钟开始啦」，开机就会把人拽过去。补发是毫秒级的，3 秒窗口足够。 */
    if (lv_tick_elaps(history_started_at) < 3000) return;
    if (!text || !strstr(text, "开始")) return;
    if (!strstr(text, "番茄钟") && !strstr(text, "pomodoro") && !strstr(text, "Pomodoro")) return;

    pomo = qz_applets_screen(QZ_APPLET_POMODORO);
    if (pomo && lv_screen_active() != pomo) {
        qz_screen_load(pomo, LV_SCR_LOAD_ANIM_FADE_ON, QZ_DUR_SCREEN);
    }
}

static void qzdesk_core_event(const qzdesk_core_event_t *event, void *user_data)
{
    (void)user_data;
    if (!event) return;
    if (event->type == QZDESK_CORE_EVENT_STATUS) {
        /* A status datagram is the handshake. Values 0/1/2 are valid core
         * startup or activation states and must not be rendered as offline. */
        request_started_at = 0;
        /* 首次握手成功说明核心在监听：这时补一次历史，屏幕上就能接着显示之前
         * 的对话（包括在网页控制台里进行的那些）。只问一次，之后靠实时推送。 */
        static bool history_requested;
        if (!history_requested) {
            history_requested = true;
            history_started_at = lv_tick_get();
            qzdesk_core_request_history();
        }
        const char *status = "离线";
        const char *caption = "本地服务暂未连接";
        if (event->state == 3) {
            status = "在线";
            caption = qz_face_state_text(QZ_FACE_IDLE);
            if (!talking) apply_face_state(QZ_FACE_IDLE);
        } else if (event->state == 5) {
            status = "聆听中";
            caption = qz_face_state_text(QZ_FACE_IDLE);
            if (!talking) apply_face_state(QZ_FACE_IDLE);
        } else if (event->state == 6) {
            status = "回复中";
            caption = qz_face_state_text(QZ_FACE_SPEAKING);
            apply_face_state(QZ_FACE_SPEAKING);
            arm_state_timer(4200, QZ_FACE_IDLE);
        } else {
            apply_face_state(QZ_FACE_THINKING);
        }
        set_caption(status, caption);
    } else if (event->type == QZDESK_CORE_EVENT_CHAT && event->text[0] != '\0') {
        /* 聊天记录统一由核心推过来（用户打字、语音识别、助手回复、系统提示），
         * 内容与顺序和网页控制台上看到的是同一份；去重也在核心做，这里不再判断。 */
        bool from_user = strcmp(event->role, "user") == 0;
        stop_typing();
        add_bubble(event->text, from_user);
        if (strcmp(event->role, "assistant") == 0) {
            set_caption("回复中", qz_face_state_text(QZ_FACE_SPEAKING));
            apply_face_state(QZ_FACE_SPEAKING);
            arm_state_timer(6000, QZ_FACE_IDLE);
            maybe_jump_pomodoro(event->text);
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
    const char *text = lv_textarea_get_text(message_input);
    if (!text || text[0] == '\0') return;
    char copy[256];
    snprintf(copy, sizeof(copy), "%s", text);
    lv_textarea_set_text(message_input, "");
    stop_typing();
    /* 不在本地先画气泡：核心会把这条记成用户消息再推回来，网页控制台与设备
     * 屏幕因此显示的是同一条记录，不会出现一边有、一边没有。 */
    if (!qzdesk_core_send_text(copy)) {
        /* Only a transport failure can land here now: the core owns the
         * "text -> speech" step and reports its own errors as toasts. */
        add_bubble("核心未连接，这条消息没有发出去。可以先用语音输入，或在核心启动后重试。", false);
        set_caption("未连接", "核心未运行");
        apply_face_state(QZ_FACE_IDLE);
    } else {
        /* The core synthesises this text locally and uploads it as a normal voice
         * turn, so the face follows the state messages it sends back. */
        set_caption("合成中", "正在把文字变成语音");
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
        send_message(event);
    }
}

/* ------------------------------------------------------------------------- *
 * Mode switch
 * ------------------------------------------------------------------------- */

static void update_mode_button(void)
{
    if (!mode_button_text) return;
    lv_label_set_text(mode_button_text, face_mode ? "聊天" : "表情");
    lv_obj_t *icon = lv_obj_get_child(mode_button, 0);
    if (icon) lv_label_set_text(icon, face_mode ? LV_SYMBOL_KEYBOARD : LV_SYMBOL_IMAGE);
}

static void show_mode(bool show_face)
{
    if (face_mode == show_face) return;
    face_mode = show_face;

    if (show_face) {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(chat_view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(face_view, LV_OBJ_FLAG_HIDDEN);
        qz_animate_pop_in(face_view);
        /* Greet with the happy face, then fall back to the real state. */
        qz_face_state_t previous = current_face_state;
        if (previous == QZ_FACE_HAPPY) previous = QZ_FACE_IDLE;
        apply_face_state(QZ_FACE_HAPPY);
        if (face_caption) lv_label_set_text(face_caption, qz_face_state_text(QZ_FACE_HAPPY));
        arm_state_timer(1800, previous);
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
    talking = true;
    if (face_caption) lv_label_set_text(face_caption, "我在听你说～");
    qzdesk_core_request_status();
}

static void talk_released(lv_event_t *event)
{
    (void)event;
    talking = false;
    if (face_caption) lv_label_set_text(face_caption, qz_face_state_text(settle_target));
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

static void build_toolbar(void)
{
    lv_obj_t *toolbar = lv_obj_create(screen);
    lv_obj_set_size(toolbar, QZ_DESIGN_W - 16, QZ_TOOLBAR_H - 4);
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 6);
    qz_style_toolbar(toolbar);

    lv_obj_t *back = qz_icon_button(toolbar, LV_SYMBOL_LEFT, 30);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_event_cb(back, back_to_desktop, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = qz_text(toolbar, "AI 助手", 15, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_CENTER, -50, 0);
    state_dot = lv_obj_create(toolbar);
    lv_obj_set_size(state_dot, 6, 6);
    lv_obj_align(state_dot, LV_ALIGN_CENTER, 4, 0);
    lv_obj_set_style_radius(state_dot, LV_RADIUS_CIRCLE, 0);
    qz_obj_set_bg_color(state_dot, QZ_ORANGE, 0);
    lv_obj_set_style_border_width(state_dot, 0, 0);
    lv_obj_clear_flag(state_dot, LV_OBJ_FLAG_CLICKABLE);
    state_label = qz_text(toolbar, "连接中", 11, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(state_label, LV_ALIGN_CENTER, 26, 0);

    mode_button = lv_button_create(toolbar);
    lv_obj_set_size(mode_button, 78, 26);
    lv_obj_align(mode_button, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_radius(mode_button, LV_RADIUS_CIRCLE, 0);
    /* iOS tinted capsule: soft blue fill, blue glyph + caption. */
    qz_obj_set_bg_color(mode_button, QZ_ACCENT_TINT, 0);
    qz_obj_set_bg_color(mode_button, QZ_FILL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(mode_button, 0, 0);
    lv_obj_set_style_pad_all(mode_button, 0, 0);
    qz_add_press_feedback(mode_button);
    qz_add_press_scale(mode_button, 96);
    lv_obj_t *icon = qz_symbol(mode_button, LV_SYMBOL_IMAGE, 12, qz_color(QZ_ACCENT_TEXT));
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 10, 0);
    mode_button_text = qz_text(mode_button, "表情", 12, qz_color(QZ_ACCENT_TEXT));
    lv_obj_align(mode_button_text, LV_ALIGN_LEFT_MID, 27, 0);
    lv_obj_add_event_cb(mode_button, toggle_mode, LV_EVENT_CLICKED, NULL);
}

static void build_chat_view(void)
{
    /* The list runs the full height and is padded clear of the bar and the
     * composer, so content passes *underneath* both: that is what turns the
     * translucent material into glass — a translucent fill over nothing reads as
     * flat grey. build_toolbar() runs first and is moved to the foreground in
     * qz_assistant_create() so the bar stays on top of it. */
    chat_view = lv_obj_create(screen);
    lv_obj_set_size(chat_view, lv_pct(100), QZ_DESIGN_H);
    lv_obj_align(chat_view, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(chat_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chat_view, 0, 0);
    lv_obj_set_style_pad_all(chat_view, 0, 0);
    lv_obj_clear_flag(chat_view, LV_OBJ_FLAG_SCROLLABLE);

    chat_list = lv_obj_create(chat_view);
    lv_obj_set_size(chat_list, lv_pct(100), QZ_DESIGN_H);
    lv_obj_align(chat_list, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(chat_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chat_list, 0, 0);
    lv_obj_set_style_pad_left(chat_list, 12, 0);
    lv_obj_set_style_pad_right(chat_list, 12, 0);
    lv_obj_set_style_pad_top(chat_list, QZ_TOOLBAR_H + 12, 0);
    lv_obj_set_style_pad_bottom(chat_list, COMPOSER_H + 12, 0);
    lv_obj_set_style_pad_row(chat_list, 8, 0);
    lv_obj_set_flex_flow(chat_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(chat_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_add_flag(chat_list, LV_OBJ_FLAG_SCROLLABLE);
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
    lv_obj_set_size(message_input, 358, 34);
    lv_obj_align(message_input, LV_ALIGN_LEFT_MID, 8, 0);
    lv_textarea_set_one_line(message_input, true);
    lv_textarea_set_placeholder_text(message_input, "输入消息…");
    qz_style_textarea(message_input);
    lv_obj_add_event_cb(message_input, input_event, LV_EVENT_ALL, NULL);

    lv_obj_t *mic = lv_button_create(bar);
    lv_obj_set_size(mic, 34, 34);
    lv_obj_align(mic, LV_ALIGN_RIGHT_MID, -50, 0);
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

    send_button = lv_button_create(bar);
    lv_obj_set_size(send_button, 34, 34);
    lv_obj_align(send_button, LV_ALIGN_RIGHT_MID, -8, 0);
    qz_style_primary_button(send_button);
    qz_add_press_scale(send_button, 94);
    lv_obj_t *arrow = qz_symbol(send_button, LV_SYMBOL_UP, 15, qz_color(QZ_TEXT_ON_ACCENT));
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
    lv_obj_set_size(face_view, lv_pct(100), CHAT_AREA_H);
    lv_obj_align(face_view, LV_ALIGN_TOP_MID, 0, QZ_TOOLBAR_H);
    lv_obj_set_style_bg_opa(face_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(face_view, 0, 0);
    lv_obj_set_style_pad_all(face_view, 0, 0);
    lv_obj_clear_flag(face_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(face_view, LV_OBJ_FLAG_HIDDEN);

    big_face = qz_face_create(face_view, 164);
    lv_obj_align(big_face, LV_ALIGN_LEFT_MID, 30, 0);

    face_caption = qz_text(face_view, qz_face_state_text(QZ_FACE_IDLE), 17, qz_color(QZ_TEXT));
    lv_obj_align(face_caption, LV_ALIGN_TOP_LEFT, 200, 88);

    face_hint = qz_text(face_view, "右上角可切回聊天", 11, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(face_hint, LV_ALIGN_TOP_LEFT, 200, 116);

    lv_obj_t *talk = lv_button_create(face_view);
    lv_obj_set_size(talk, 240, 48);
    lv_obj_align(talk, LV_ALIGN_TOP_LEFT, 200, 156);
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
    lv_obj_t *talk_text = qz_text(talk, "长按说话", 15, qz_color(QZ_TEXT_ON_ACCENT));
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

void qz_assistant_set_desktop(lv_obj_t *desktop) { desktop_screen = desktop; }
