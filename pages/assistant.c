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
#define CHAT_TOP (QZ_HEADER_H + 6)
/* 输入面高度。紧凑版要给两个主键留出 40 实像素的触控高度，480×320 上
 * 则保持更薄的单行工作台。 */
static int32_t composer_height(void)
{
    return qz_compact() ? 80 : 64;
}

/* 输入面里的按钮尺寸：按 40 实像素反算回设计稿。 */
static int32_t composer_control_w(void)
{
    return qz_compact() ? qz_design_x(QZ_MIN_TOUCH_PX) : QZ_MIN_TOUCH_PX;
}

static int32_t composer_control_h(void)
{
    return qz_compact() ? qz_design_y(QZ_MIN_TOUCH_PX) : QZ_MIN_TOUCH_PX;
}

/* 空状态里的常用问法按行排列：紧凑屏两行，480×320 三行。 */
static int32_t preset_height(void)
{
    return qz_compact() ? qz_design_y(QZ_MIN_TOUCH_PX) : 40;
}

static int32_t empty_intro_height(void)
{
    return qz_compact() ? qz_design_px(44) : 60;
}

static int32_t menu_row_height(void)
{
    return qz_compact() ? qz_design_y(QZ_MIN_TOUCH_PX) : QZ_MIN_TOUCH_PX;
}

static lv_obj_t *keyboard;

static int32_t current_chat_top(void)
{
    return CHAT_TOP;
}

static int32_t chat_list_height(void)
{
    int32_t height = QZ_DESIGN_H - current_chat_top() - composer_height();
    if (keyboard && !lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN))
        height -= KEYBOARD_H;
    return height;
}

static int32_t face_height(void)
{
    /* QZ_DESIGN_H 是高度，不是最后一个 y 坐标；少减 1 会让内容根节点在
     * 320×240 上正好越过屏幕底边一个像素。 */
    return QZ_DESIGN_H - CHAT_TOP - 1;
}

/* 图标素材按物理像素烘焙。先反推设计稿尺寸，避免 320×240 上 24 设计像素
 * 最后只显示成 16 实像素。 */
static int32_t chat_icon_px(void)
{
    return qz_design_px(34);
}

static int32_t menu_icon_px(void)
{
    return qz_design_px(26);
}

/* 手机输入桥浮层（设计像素，见 include/scale.h）。 */
#define BRIDGE_PANEL_W 432
#define BRIDGE_PANEL_H 244
#define BRIDGE_TILE 174   /**< 白色扫码底板：二维码 + 四周静区 */
#define BRIDGE_QR 150     /**< 二维码本体；下面自己换算成实像素 */
#define BRIDGE_COLUMN_X 202
#define BRIDGE_COLUMN_W 216

static lv_obj_t *screen;
static lv_obj_t *chat_view;
static lv_obj_t *face_view;
static lv_obj_t *chat_list;
static lv_obj_t *chat_empty;
static lv_obj_t *composer;
static lv_obj_t *message_input;
static lv_obj_t *send_button;
static lv_obj_t *bridge_button;
static lv_obj_t *big_face;
static lv_obj_t *face_caption;
static lv_obj_t *talk_button_label;
static lv_obj_t *state_label;
static lv_obj_t *state_dot;
static lv_obj_t *session_notice;
static lv_obj_t *session_notice_label;
static lv_obj_t *menu_overlay;
static lv_obj_t *desktop_screen;
static lv_obj_t *typing_bubble;
static lv_timer_t *typing_timer;
static lv_obj_t *bridge_overlay;
static lv_obj_t *bridge_tile;
static lv_obj_t *bridge_qr;
static lv_obj_t *bridge_hint;
static lv_obj_t *bridge_url;

static void back_to_desktop(lv_event_t *event);
static void close_menu(void);
static void open_menu(lv_event_t *event);
static void bridge_show_error(const char *reason);
static void bridge_update(const qzdesk_core_event_t *event);
static qz_face_state_t current_face_state = QZ_FACE_IDLE;
/** 核心最近一次报的状态；番茄钟的表情联动要等它回到"在线"才接过去。 */
static int core_state = -1;
static char remote_emotion[8];
static bool face_mode;
static bool talking;
static uint32_t talking_started;
static int32_t typing_phase;
static uint32_t request_started_at;
static unsigned long long pending_text_id;
static uint32_t pending_text_at;
static char pending_draft[241];
static bool abort_pending;
static uint32_t abort_started_at;
static bool bridge_waiting;
static uint32_t bridge_requested_at;

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

/**
 * 按完整字符复制一串 UTF-8（放不下就停）。
 *
 * 识别文本按字节截断会留下半个汉字 —— LVGL 拿到非法 UTF-8 会画出替换符或直接
 * 丢掉尾巴，所以这里多一步边界判断。
 */
static void copy_utf8(char *out, size_t size, const char *text)
{
    size_t used = 0;
    size_t remaining = strlen(text);
    while (remaining > 0 && used + 1 < size) {
        unsigned char lead = (unsigned char)*text;
        size_t width = lead < 0xE0 ? (lead < 0xC0 ? 1 : 2) : (lead < 0xF0 ? 3 : 4);
        if (width > remaining || used + width + 1 > size) break;
        memcpy(out + used, text, width);
        used += width;
        text += width;
        remaining -= width;
    }
    out[used] = '\0';
}

static void set_caption(const char *status, const char *caption)
{
    if (state_label) lv_label_set_text(state_label, status);
    if (face_caption) lv_label_set_text(face_caption, caption);
    if (state_dot && state_label) {
        bool online = strcmp(status, "在线") == 0 || strcmp(status, "聆听中") == 0 ||
                      strcmp(status, "语音处理中") == 0 || strcmp(status, "回复中") == 0 ||
                      strcmp(status, "已接收") == 0 || strcmp(status, "已识别") == 0;
        bool failed = strcmp(status, "离线") == 0 || strcmp(status, "未连接") == 0 ||
                      strcmp(status, "未接收") == 0 || strcmp(status, "失败") == 0 ||
                      strcmp(status, "停止失败") == 0 || strcmp(status, "未确认") == 0;
        qz_color_token_t dot_color = online ? QZ_GREEN : failed ? QZ_RED : QZ_ORANGE;
        qz_obj_set_bg_color(state_dot, dot_color, 0);
        qz_obj_set_text_color(state_label, online ? QZ_TEXT_SECONDARY : QZ_TEXT_TERTIARY, 0);
        lv_obj_set_style_opa(state_label, LV_OPA_COVER, 0);
    }
}

static void apply_face_state(qz_face_state_t state)
{
    current_face_state = state;
    if (big_face) qz_face_set_state(big_face, state);
}

static void clear_remote_emotion(void)
{
    remote_emotion[0] = '\0';
}

static bool apply_remote_emotion_face(void)
{
    if (!remote_emotion[0] || !qz_face_emotion_valid(remote_emotion)) return false;
    if (big_face) qz_face_set_emotion(big_face, remote_emotion);
    current_face_state = qz_face_emotion_state(remote_emotion);
    return true;
}

static bool store_remote_emotion(const char *emotion)
{
    if (!qz_face_emotion_valid(emotion)) return false;
    snprintf(remote_emotion, sizeof(remote_emotion), "%s", emotion);
    return apply_remote_emotion_face();
}

/** 助手闲着的时候把脸借给番茄钟：专注=思考、休息=开心、暂停=困倦。
 *  没在跑番茄钟时这张脸还是待机，助手说话时照旧由对话状态接管。 */
static void apply_idle_face(void)
{
    if (apply_remote_emotion_face()) return;
    apply_face_state(qz_applets_focus_face());
}

/* 助手说话时，这颗键按下就是"打断这一轮、重新说"：文案跟着状态变，用户不用
 * 先去点标题栏里的「停止」。四个汉字，和"长按说话"同宽，不会把麦克风图标挤开。 */
static void update_talk_label(bool speaking)
{
    if (talk_button_label)
        lv_label_set_text(talk_button_label, speaking ? "打断重说" : "长按说话");
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
    lv_obj_set_style_pad_column(row, qz_design_px(8), 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, from_user ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    /* 助手每轮回复前放一个很小的身份标记。它让左右两列一眼分得清，同时比
     * 给每条消息再加头像卡轻得多。 */
    if (!from_user) {
        lv_obj_t *mark = lv_obj_create(row);
        int32_t size = qz_design_px(30);
        lv_obj_set_size(mark, size, size);
        lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, 0);
        qz_obj_set_bg_color(mark, QZ_ACCENT_TINT, 0);
        lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(mark, 0, 0);
        lv_obj_set_style_pad_all(mark, 0, 0);
        lv_obj_set_style_margin_top(mark, qz_design_px(3), 0);
        lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(mark, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *glyph = qz_icon_image(mark, QZ_ICON_SPARKLES,
                                        qz_design_px(15),
                                        qz_color(QZ_ACCENT_TEXT));
        if (glyph) lv_obj_center(glyph);
    }
    return row;
}

static void set_button_icon(lv_obj_t *button, qz_icon_t icon, int32_t physical_px,
                            qz_color_token_t color)
{
    if (!button) return;
    lv_obj_t *old = lv_obj_get_child(button, 0);
    if (old) lv_obj_delete(old);
    lv_obj_t *glyph = qz_icon_image(button, icon, qz_design_px(physical_px),
                                    qz_color(color));
    if (glyph) lv_obj_center(glyph);
}

static lv_obj_t *add_bubble(const char *text, bool from_user)
{
    if (!chat_list || !text || text[0] == '\0') return NULL;

    if (chat_empty) {
        lv_obj_delete(chat_empty);
        chat_empty = NULL;
    }
    bool follow = lv_obj_get_scroll_bottom(chat_list) < 24;
    while (lv_obj_get_child_count(chat_list) >= 100) {
        lv_obj_delete(lv_obj_get_child(chat_list, 0));
    }
    lv_obj_t *row = bubble_row(from_user);
    lv_obj_t *bubble = lv_obj_create(row);
    lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(bubble, lv_pct(84), 0);
    if (from_user) {
        lv_obj_set_style_pad_left(bubble, 12, 0);
        lv_obj_set_style_pad_right(bubble, 12, 0);
        lv_obj_set_style_pad_top(bubble, 9, 0);
        lv_obj_set_style_pad_bottom(bubble, 9, 0);
        lv_obj_set_style_radius(bubble, 5, 0);
        lv_obj_set_style_border_width(bubble, 0, 0);
        qz_obj_set_bg_color(bubble, QZ_ACCENT, 0);
        lv_obj_set_style_bg_grad_dir(bubble, LV_GRAD_DIR_NONE, 0);
        lv_obj_set_style_shadow_width(bubble, 0, 0);
    } else {
        /* Replies use the same quiet white material as the composer. This is a
         * message bubble, not another feature card: one fill, one hairline and
         * no shadow. */
        qz_obj_set_bg_color(bubble, QZ_CARD, 0);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bubble, 1, 0);
        qz_obj_set_border_color(bubble, QZ_SEPARATOR, 0);
        lv_obj_set_style_border_opa(bubble, (lv_opa_t)110, 0);
        lv_obj_set_style_pad_left(bubble, 12, 0);
        lv_obj_set_style_pad_right(bubble, 12, 0);
        lv_obj_set_style_pad_top(bubble, 9, 0);
        lv_obj_set_style_pad_bottom(bubble, 9, 0);
        lv_obj_set_style_radius(bubble, 5, 0);
        lv_obj_set_style_shadow_width(bubble, 0, 0);
    }
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = qz_text(bubble, text, 17, qz_color(from_user ? QZ_TEXT_ON_ACCENT : QZ_TEXT));
    lv_obj_set_width(label, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(label, qz_compact() ? 300 : 360, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(label, 5, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_update_layout(chat_list);
    if (follow || from_user) lv_obj_scroll_to_view(row, LV_ANIM_ON);
    return bubble;
}

static void typing_pulse(lv_timer_t *timer)
{
    lv_obj_t *row = (lv_obj_t *)lv_timer_get_user_data(timer);
    if (!row) return;
    lv_obj_t *bubble = lv_obj_get_child(row, lv_obj_get_child_count(row) - 1);
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
    lv_obj_set_size(bubble, LV_SIZE_CONTENT, qz_design_px(34));
    qz_obj_set_bg_color(bubble, QZ_CARD, 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(bubble, 11, 0);
    lv_obj_set_style_pad_right(bubble, 11, 0);
    lv_obj_set_style_pad_column(bubble, 5, 0);
    lv_obj_set_style_radius(bubble, 5, 0);
    lv_obj_set_style_border_width(bubble, 1, 0);
    qz_obj_set_border_color(bubble, QZ_SEPARATOR, 0);
    lv_obj_set_style_border_opa(bubble, (lv_opa_t)110, 0);
    lv_obj_set_style_shadow_width(bubble, 0, 0);
    lv_obj_set_flex_flow(bubble, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bubble, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_CLICKABLE);

    qz_text(bubble, "思考中", 13, qz_color(QZ_TEXT_SECONDARY));
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

static bool is_session_progress(const char *text)
{
    return text && (strstr(text, "正在协商") || strstr(text, "正在同步") ||
                    strstr(text, "正在连接云端") || strstr(text, "正在重建会话"));
}

static void show_session_notice(const char *text)
{
    if (!chat_view) return;
    if (!session_notice) {
        session_notice = lv_obj_create(chat_view);
        lv_obj_set_size(session_notice, QZ_DESIGN_W - 32, 44);
        lv_obj_align(session_notice, LV_ALIGN_TOP_MID, 0, current_chat_top() + 4);
        qz_obj_set_bg_color(session_notice, QZ_ACCENT_TINT, 0);
        lv_obj_set_style_bg_opa(session_notice, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(session_notice, 1, 0);
        qz_obj_set_border_color(session_notice, QZ_SEPARATOR, 0);
        lv_obj_set_style_border_opa(session_notice, (lv_opa_t)120, 0);
        lv_obj_set_style_shadow_width(session_notice, 0, 0);
        lv_obj_set_style_radius(session_notice, 4, 0);
        lv_obj_set_style_pad_all(session_notice, 8, 0);
        lv_obj_set_style_pad_column(session_notice, 10, 0);
        lv_obj_set_flex_flow(session_notice, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(session_notice, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(session_notice, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *spinner = lv_spinner_create(session_notice);
        lv_obj_set_size(spinner, 22, 22);
        lv_spinner_set_anim_params(spinner, 1000, 90);
        session_notice_label = qz_text(session_notice, "", 14, qz_color(QZ_TEXT_SECONDARY));
        lv_obj_set_width(session_notice_label, 0);
        lv_obj_set_flex_grow(session_notice_label, 1);
        lv_label_set_long_mode(session_notice_label, LV_LABEL_LONG_DOT);
    }
    lv_label_set_text(session_notice_label, text);
    lv_obj_remove_flag(session_notice, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(session_notice);
}

static void hide_session_notice(void)
{
    if (session_notice) lv_obj_add_flag(session_notice, LV_OBJ_FLAG_HIDDEN);
}

static void qzdesk_core_event(const qzdesk_core_event_t *event, void *user_data)
{
    (void)user_data;
    if (!event) return;
    if (event->type == QZDESK_CORE_EVENT_ACK && strcmp(event->command, "abort_turn") == 0) {
        abort_pending = false;
        if (event->ok) {
            pending_text_id = 0;
            talking = false;
            clear_remote_emotion();
            lv_obj_remove_state(send_button, LV_STATE_DISABLED);
            stop_typing();
            hide_session_notice();
            apply_face_state(QZ_FACE_IDLE);
            set_caption("已停止", "聊天记录与输入已保留");
        } else {
            add_bubble(event->text[0] ? event->text : "停止失败，请重试", false);
            set_caption("停止失败", "可以再次点击停止");
        }
        qzdesk_core_request_status();
        return;
    }
    if (event->type == QZDESK_CORE_EVENT_ACK && strcmp(event->command, "new_chat") == 0) {
        clear_remote_emotion();
        stop_typing();
        if (event->ok) show_session_notice(event->text);
        else {
            hide_session_notice();
            add_bubble(event->text, false);
        }
        return;
    }
    if (event->type == QZDESK_CORE_EVENT_ACK && pending_text_id &&
        strcmp(event->command, "chat_text") == 0 && event->request_id == pending_text_id) {
        pending_text_id = 0;
        lv_obj_remove_state(send_button, LV_STATE_DISABLED);
        if (event->ok) {
            if (!abort_pending && strcmp(lv_textarea_get_text(message_input), pending_draft) == 0)
                lv_textarea_set_text(message_input, "");
            set_caption("已发送", "等待云端回复");
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
        core_state = event->state;
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
            hide_session_notice();
            clear_remote_emotion();
            status = "在线";
            caption = qz_face_state_text(qz_applets_focus_face());
            if (!talking) apply_idle_face();
        } else if (event->state == 5) {
            clear_remote_emotion();
            status = talking ? "聆听中" : "语音处理中";
            caption = talking ? "我在听你说，松开结束" : "等待语音处理结果";
            if (!talking) apply_idle_face();
        } else if (event->state == 6) {
            status = "回复中";
            if (apply_remote_emotion_face()) {
                caption = qz_face_emotion_text(remote_emotion);
            } else {
                caption = qz_face_state_text(QZ_FACE_SPEAKING);
                apply_face_state(QZ_FACE_SPEAKING);
            }

        } else {
            clear_remote_emotion();
            status = event->state == 2 ? "待激活" : event->state == 4 ? "重连中" : "启动中";
            caption = event->state == 2 ? "请先完成设备激活" : "正在连接云端服务";
            apply_face_state(QZ_FACE_THINKING);
        }
        set_caption(status, caption);
        /* 回复中按下语音键 = 打断重说，键面文案跟着换。 */
        update_talk_label(event->state == 6);
    } else if (event->type == QZDESK_CORE_EVENT_CHAT && event->text[0] != '\0') {
        /* 聊天记录统一由核心推过来（用户打字、语音识别、助手回复、系统提示），
         * 内容与顺序和网页控制台上看到的是同一份；去重也在核心做，这里不再判断。 */
        bool from_user = strcmp(event->role, "user") == 0;
        stop_typing();
        if (strcmp(event->role, "divider") == 0) {
            hide_session_notice();
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
            if (is_session_progress(event->text)) return;
            char notice[sizeof(event->text) + 32];
            snprintf(notice, sizeof(notice), "系统提示：%s", event->text);
            add_bubble(notice, false);
        } else add_bubble(event->text, from_user);
        if (strcmp(event->role, "assistant") == 0) {
            hide_session_notice();
            if (apply_remote_emotion_face()) {
                set_caption("回复中", qz_face_emotion_text(remote_emotion));
            } else {
                set_caption("回复中", qz_face_state_text(QZ_FACE_SPEAKING));
                apply_face_state(QZ_FACE_SPEAKING);
            }
        }
    } else if (event->type == QZDESK_CORE_EVENT_ASR && event->text[0] != '\0') {
        /* 刚听清的一句用户话。聊天模式下气泡里已经有了，表情模式下没有列表，
         * 字幕就是唯一能核对"它听成了什么"的地方 —— 听错了按住语音键就能
         * 打断重说，不必先去点「停止」。 */
        char heard[192];
        char spoken[168];
        copy_utf8(spoken, sizeof(spoken), event->text);
        snprintf(heard, sizeof(heard), "我听到：%s", spoken);
        clear_remote_emotion();
        set_caption("已识别", "正在思考如何回答");
        apply_face_state(QZ_FACE_THINKING);
        if (face_caption) lv_label_set_text(face_caption, heard);
    } else if (event->type == QZDESK_CORE_EVENT_EMOTION && event->emotion[0] != '\0') {
        if (store_remote_emotion(event->emotion)) {
            set_caption(core_state == 6 ? "回复中" : "已接收",
                        qz_face_emotion_text(remote_emotion));
        }
    } else if (event->type == QZDESK_CORE_EVENT_TOAST && event->text[0] != '\0') {
        stop_typing();
        if (is_session_progress(event->text)) show_session_notice(event->text);
        else {
            hide_session_notice();
            add_bubble(event->text, false);
        }
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
    } else if (event->type == QZDESK_CORE_EVENT_BRIDGE) {
        bridge_update(event);
    }
}

static void poll_qzdesk_core(lv_timer_t *timer)
{
    (void)timer;
    static bool status_requested;
    static uint32_t last_retry_at;
    const uint32_t now = lv_tick_get();
    if (abort_pending && lv_tick_elaps(abort_started_at) >= 4000) {
        abort_pending = false;
        add_bubble("停止结果未确认，输入与聊天记录已保留，可再次点击停止。", false);
        set_caption("未确认", "请重试停止");
        qzdesk_core_request_status();
    }
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
    if (bridge_waiting && lv_tick_elaps(bridge_requested_at) >= 4000)
        bridge_show_error("核心没有回应，请稍后重新打开");
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
    /* 番茄钟换阶段（专注↔休息、开始/暂停）不会经过核心事件，所以每拍对一次表：
     * 助手空闲时把脸交给它，正忙（在听、在等回复）时不打扰。 */
    if (core_state == 3 && !talking && !pending_text_id && !abort_pending) apply_idle_face();
}

/* ------------------------------------------------------------------------- *
 * Input
 * ------------------------------------------------------------------------- */

static void abort_turn_clicked(lv_event_t *event)
{
    (void)event;
    if (abort_pending) return;
    clear_remote_emotion();
    if (!qzdesk_core_abort_turn()) {
        add_bubble("停止指令未发送，本地服务不可达，请重试。", false);
        set_caption("停止失败", "输入与聊天记录已保留");
        return;
    }
    abort_pending = true;
    abort_started_at = lv_tick_get();
    set_caption("停止中", "等待核心确认停止");
}

static void send_message(lv_event_t *event)
{
    (void)event;
    if (pending_text_id || abort_pending) return;
    const char *text = lv_textarea_get_text(message_input);
    if (!text || text[0] == '\0') return;
    if (strlen(text) > 240) {
        add_bubble("消息最多 240 字节，请缩短后发送；草稿已保留。", false);
        return;
    }
    clear_remote_emotion();
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

/* 三个"点一下就发"的常用问法。设备上没有中文输入法，中文指令要么说，要么
 * 从这里选；chip 直接用预设文案做 user_data，静态存储所以不用拷贝。 */
static const char *const preset_texts[] = {
    "今天天气怎么样？",
    "看看设备现在的状况",
    "25 分钟后提醒我",
};

static void preset_clicked(lv_event_t *event)
{
    if (pending_text_id || abort_pending) return;
    if (lv_textarea_get_text(message_input)[0]) {
        set_caption("有草稿未发送", "先发送或清空输入框");
        return;
    }
    clear_remote_emotion();
    const char *text = lv_event_get_user_data(event);
    if (!qzdesk_core_send_text(text)) {
        set_caption("未连接", "本地服务未运行，这句话没有发出去");
        return;
    }
    snprintf(pending_draft, sizeof(pending_draft), "%s", text);
    pending_text_id = qzdesk_core_text_request_id();
    pending_text_at = lv_tick_get();
    lv_obj_add_state(send_button, LV_STATE_DISABLED);
    set_caption("发送中", "等待核心确认接收");
    apply_face_state(QZ_FACE_THINKING);
}

/** 把一条建议排成可扫读的整行，而不是另一个漂浮胶囊。 */
static void preset_row(lv_obj_t *parent, int index, bool last)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_size(row, lv_pct(100), preset_height());
    qz_touch_target(row);
    lv_obj_set_style_radius(row, 9, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    qz_obj_set_bg_color(row, QZ_FILL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(row, last ? 0 : 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    qz_obj_set_border_color(row, QZ_SEPARATOR, 0);
    lv_obj_set_style_border_opa(row, last ? LV_OPA_TRANSP : (lv_opa_t)180, 0);
    lv_obj_set_style_pad_left(row, 8, 0);
    lv_obj_set_style_pad_right(row, 8, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    qz_add_press_feedback(row);

    lv_obj_t *label = qz_text(row, preset_texts[index], 16, qz_color(QZ_TEXT));
    lv_obj_set_width(label, 0);
    lv_obj_set_flex_grow(label, 1);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);

    qz_icon_image(row, QZ_ICON_CHEVRON_RIGHT, qz_design_px(18),
                  qz_color(QZ_TEXT_TERTIARY));
    lv_obj_add_event_cb(row, preset_clicked, LV_EVENT_CLICKED, (void *)preset_texts[index]);
}

/** 紧凑屏只留两条最常用的问法，并排放下，不再让建议区吃掉半屏。 */
static void preset_chip(lv_obj_t *parent, int index)
{
    lv_obj_t *chip = lv_button_create(parent);
    lv_obj_set_size(chip, 0, preset_height());
    lv_obj_set_flex_grow(chip, 1);
    qz_touch_target(chip);
    lv_obj_set_style_radius(chip, 10, 0);
    qz_obj_set_bg_color(chip, QZ_ACCENT_TINT, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    qz_obj_set_bg_color(chip, QZ_FILL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(chip, 0, 0);
    lv_obj_set_style_shadow_width(chip, 0, 0);
    lv_obj_set_style_pad_left(chip, 8, 0);
    lv_obj_set_style_pad_right(chip, 7, 0);
    lv_obj_set_style_pad_column(chip, qz_design_px(5), 0);
    lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    qz_add_press_feedback(chip);

    lv_obj_t *label = qz_text(chip, preset_texts[index], 14,
                              qz_color(QZ_ACCENT_TEXT));
    lv_obj_set_width(label, 0);
    lv_obj_set_flex_grow(label, 1);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    qz_icon_image(chip, QZ_ICON_SPARKLES, qz_design_px(15),
                  qz_color(QZ_ACCENT_TEXT));
    lv_obj_add_event_cb(chip, preset_clicked, LV_EVENT_CLICKED,
                        (void *)preset_texts[index]);
}

static void set_keyboard_visible(bool visible)
{
    if (!keyboard || !message_input) return;
    if (visible) {
        lv_obj_add_state(message_input, LV_STATE_FOCUSED);
    } else {
        lv_obj_remove_state(message_input, LV_STATE_FOCUSED);
    }

    if (visible) {
        lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(composer, LV_ALIGN_BOTTOM_MID, 0, -KEYBOARD_H);
        lv_obj_set_height(chat_list, chat_list_height());
    } else {
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(composer, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_height(chat_list, chat_list_height());
    }
    lv_obj_align(chat_list, LV_ALIGN_TOP_MID, 0, current_chat_top());
    if (session_notice)
        lv_obj_align(session_notice, LV_ALIGN_TOP_MID, 0, current_chat_top() + 4);
    scroll_to_latest();
}

static void toggle_keyboard(lv_event_t *event)
{
    (void)event;
    bool open = keyboard && !lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    set_keyboard_visible(!open);
}

static void input_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_FOCUSED) {
        set_keyboard_visible(true);
    } else if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_CANCEL) {
        set_keyboard_visible(false);
    } else if (code == LV_EVENT_READY) {
        set_keyboard_visible(false);
        send_message(event);
        scroll_to_latest();
    }
}

/* ------------------------------------------------------------------------- *
 * 手机输入桥
 *
 * 设备上只有拉丁键盘，中文打不出来。点输入栏里的手机键弹一张码：手机扫码打开
 * 网页控制台的对话页，在手机上打字，核心把文字直发云端，不兼容时再回退语音。
 *
 * 地址只有核心知道（局域网 IP + 网页端口），所以先请求、拿到回包再画码；没连网
 * 时核心回一条原因，界面照原样说出来，不画半个空码。
 * ------------------------------------------------------------------------- */

static void close_bridge(void)
{
    if (bridge_overlay) lv_obj_delete(bridge_overlay);
    bridge_overlay = NULL;
    bridge_tile = NULL;
    bridge_qr = NULL;
    bridge_hint = NULL;
    bridge_url = NULL;
    bridge_waiting = false;
}

static void bridge_scrim_clicked(lv_event_t *event)
{
    if (lv_event_get_target(event) != lv_event_get_current_target(event)) return;
    close_bridge();
}

static void bridge_close_clicked(lv_event_t *event)
{
    (void)event;
    close_bridge();
}

/** 白底板正在等地址：转圈。 */
static void bridge_tile_spinner(void)
{
    lv_obj_clean(bridge_tile);
    bridge_qr = NULL;
    lv_obj_t *spinner = lv_spinner_create(bridge_tile);
    lv_obj_set_size(spinner, 34, 34);
    lv_spinner_set_anim_params(spinner, 1000, 90);
    lv_obj_center(spinner);
}

/** 白底板放二维码：静区来自底板留白，所以码本身只占中间那 150 设计像素。 */
static bool bridge_tile_qr(const char *url)
{
    lv_obj_clean(bridge_tile);
    bridge_qr = lv_qrcode_create(bridge_tile);
    if (!bridge_qr) return false;
    /* 二维码是位图格子：画布被设计缩放拉过之后格子大小会不均匀，扫不出来。
     * `lv_qrcode_set_size` 不吃 scale.h 的缩放，这里先换算成实像素再建画布。 */
    lv_qrcode_set_size(bridge_qr, qz_scale_px(BRIDGE_QR));
    lv_qrcode_set_dark_color(bridge_qr, lv_color_black());
    lv_qrcode_set_light_color(bridge_qr, lv_color_white());
    if (lv_qrcode_update(bridge_qr, url, (uint32_t)strlen(url)) != LV_RESULT_OK) {
        lv_obj_delete(bridge_qr);
        bridge_qr = NULL;
        return false;
    }
    lv_obj_center(bridge_qr);
    return true;
}

static void bridge_show_url(const char *url)
{
    if (!bridge_tile_qr(url)) {
        bridge_show_error("二维码没能生成，请重新打开一次");
        return;
    }
    bridge_waiting = false;
    lv_label_set_text(bridge_hint, "手机扫码，打开对话页直接在手机上打字");
    lv_label_set_text(bridge_url, url);
}

static void bridge_show_error(const char *reason)
{
    bridge_waiting = false;
    lv_obj_clean(bridge_tile);
    bridge_qr = NULL;
    /* 白底板没内容可放了，收起来让原因占满整块面板：这一句要能一眼读完，
     * 挤在右半栏里读起来像脚注。 */
    lv_obj_add_flag(bridge_tile, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(bridge_hint, BRIDGE_PANEL_W - 56);
    lv_obj_set_height(bridge_hint, 110);
    lv_obj_set_style_text_align(bridge_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(bridge_hint, LV_ALIGN_CENTER, 0, 16);
    lv_obj_set_style_text_color(bridge_hint, qz_color(QZ_TEXT), 0);
    lv_label_set_text(bridge_hint, reason && reason[0] ? reason
                                                       : "暂时拿不到扫码地址，请稍后重试");
    lv_label_set_text(bridge_url, "");
}

static void bridge_update(const qzdesk_core_event_t *event)
{
    if (!bridge_overlay) return;
    if (event->ok) bridge_show_url(event->text);
    else bridge_show_error(event->text);
}

static void open_bridge(lv_event_t *event)
{
    (void)event;
    if (bridge_overlay) return;
    /* 屏幕键盘和手机扫码是两条路：扫码时先把键盘收回去，别让它压在浮层底下。 */
    set_keyboard_visible(false);

    bridge_overlay = qz_overlay_create(lv_layer_top());
    lv_obj_add_event_cb(bridge_overlay, bridge_scrim_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(bridge_overlay);
    lv_obj_set_size(panel, BRIDGE_PANEL_W, BRIDGE_PANEL_H);
    lv_obj_center(panel);
    qz_style_sheet(panel);
    lv_obj_set_style_pad_all(panel, 0, 0);

    bridge_tile = lv_obj_create(panel);
    lv_obj_set_size(bridge_tile, BRIDGE_TILE, BRIDGE_TILE);
    lv_obj_align(bridge_tile, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_set_style_radius(bridge_tile, 12, 0);
    lv_obj_set_style_bg_color(bridge_tile, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(bridge_tile, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bridge_tile, 0, 0);
    lv_obj_set_style_pad_all(bridge_tile, 0, 0);
    lv_obj_clear_flag(bridge_tile, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = qz_text(panel, "手机输入", 18, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, BRIDGE_COLUMN_X, 26);

    bridge_hint = qz_text(panel, "正在获取扫码地址", 13, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_set_size(bridge_hint, BRIDGE_COLUMN_W, 56);
    lv_label_set_long_mode(bridge_hint, LV_LABEL_LONG_WRAP);
    lv_obj_align(bridge_hint, LV_ALIGN_TOP_LEFT, BRIDGE_COLUMN_X, 58);

    bridge_url = qz_text(panel, "", 12, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_set_size(bridge_url, BRIDGE_COLUMN_W, 62);
    lv_label_set_long_mode(bridge_url, LV_LABEL_LONG_WRAP);
    lv_obj_align(bridge_url, LV_ALIGN_TOP_LEFT, BRIDGE_COLUMN_X, 116);

    lv_obj_t *close = qz_button(panel, "关闭", 92, 36);
    lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, -16, -16);
    lv_obj_add_event_cb(close, bridge_close_clicked, LV_EVENT_CLICKED, NULL);

    bridge_tile_spinner();
    bridge_waiting = true;
    bridge_requested_at = lv_tick_get();
    if (!qzdesk_core_request_bridge())
        bridge_show_error("本地核心没在运行，扫码地址拿不到");
}

/* ------------------------------------------------------------------------- *
 * Mode switch
 * ------------------------------------------------------------------------- */

static void show_mode(bool show_face)
{
    if (face_mode == show_face) return;
    face_mode = show_face;

    if (show_face) {
        set_keyboard_visible(false);
        lv_obj_align(chat_list, LV_ALIGN_TOP_MID, 0, current_chat_top());
        lv_obj_set_height(chat_list, chat_list_height());
        lv_obj_add_flag(chat_view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(face_view, LV_OBJ_FLAG_HIDDEN);
        qz_animate_pop_in(face_view);
        apply_face_state(current_face_state);
    } else {
        lv_obj_add_flag(face_view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(chat_view, LV_OBJ_FLAG_HIDDEN);
        qz_animate_pop_in(chat_view);
    }
}

/* ------------------------------------------------------------------------- *
 * Voice button
 * ------------------------------------------------------------------------- */

static void talk_pressed(lv_event_t *event)
{
    (void)event;
    if (talking) return;
    clear_remote_emotion();
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
    /* `size` is a design-space value. A glyph must stay square in physical
     * pixels, so calculate the final side once and use the raw LVGL entry
     * points for every geometry value. Mixing the scale macros here makes
     * 320x320 stretch the box by 2/3 horizontally and 1x vertically. */
    int32_t side = qz_scale_px(size);
    lv_obj_t *box = lv_obj_create(parent);
    (lv_obj_set_size)(box, side, side);
    (lv_obj_set_style_bg_opa)(box, LV_OPA_TRANSP, 0);
    (lv_obj_set_style_border_width)(box, 0, 0);
    (lv_obj_set_style_pad_all)(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    int32_t thickness = side * 8 / 100;
    int32_t capsule_w = side * 32 / 100;
    int32_t capsule_h = side * 50 / 100;
    int32_t cradle = size * 60 / 100;

    lv_obj_t *stem = lv_obj_create(box);
    (lv_obj_set_size)(stem, thickness, side * 16 / 100);
    (lv_obj_align)(stem, LV_ALIGN_TOP_MID, 0, side * 72 / 100);
    (lv_obj_set_style_radius)(stem, LV_RADIUS_CIRCLE, 0);
    (lv_obj_set_style_bg_color)(stem, color, 0);
    (lv_obj_set_style_border_width)(stem, 0, 0);
    lv_obj_clear_flag(stem, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *base = lv_obj_create(box);
    (lv_obj_set_size)(base, side * 40 / 100, thickness);
    (lv_obj_align)(base, LV_ALIGN_TOP_MID, 0, side * 88 / 100);
    (lv_obj_set_style_radius)(base, LV_RADIUS_CIRCLE, 0);
    (lv_obj_set_style_bg_color)(base, color, 0);
    (lv_obj_set_style_border_width)(base, 0, 0);
    lv_obj_clear_flag(base, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *arc = qz_arc_piece(box, cradle, cradle, 0, 180, thickness, color);
    (lv_obj_align)(arc, LV_ALIGN_TOP_MID, 0, side * 16 / 100);

    lv_obj_t *capsule = lv_obj_create(box);
    (lv_obj_set_size)(capsule, capsule_w, capsule_h);
    (lv_obj_align)(capsule, LV_ALIGN_TOP_MID, 0, side * 8 / 100);
    (lv_obj_set_style_radius)(capsule, LV_RADIUS_CIRCLE, 0);
    (lv_obj_set_style_bg_color)(capsule, color, 0);
    (lv_obj_set_style_border_width)(capsule, 0, 0);
    lv_obj_clear_flag(capsule, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(capsule, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

static lv_obj_t *memory_screen;
static lv_obj_t *memory_list;
static lv_obj_t *memory_status;
static lv_obj_t *memory_save;
static lv_obj_t *memory_delete;
static unsigned long long memory_selected;
static bool memory_busy;

static void memory_request(const char *body);

static void memory_back(lv_event_t *event)
{
    (void)event;
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
    (void)user_data;
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
    if (!count) qz_text(memory_list, "还没有记忆。回到对话页说一句要记住的事即可。", 16,
                        qz_color(QZ_TEXT_SECONDARY));
    char status[96];
    snprintf(status, sizeof(status), "仅保存在本机，点一条再删除 · 共 %d 条", count);
    lv_label_set_text(memory_status, status);
}

static void memory_request(const char *body)
{
    if (memory_busy) return;
    memory_busy = true;
    lv_obj_add_state(memory_save, LV_STATE_DISABLED);
    lv_obj_add_state(memory_delete, LV_STATE_DISABLED);
    lv_label_set_text(memory_status, "正在处理本地记忆…");
    if (!qz_web_request_async("POST", "/api/companion/records", body, memory_done, NULL))
        memory_done(false, "", NULL);
}

static void memory_save_clicked(lv_event_t *event)
{
    (void)event;
    /* 设备上没有中文输入法，写记忆只能回对话页说一句。这里只把落点指过去，
     * 不弹键盘 —— 弹出来也打不出中文字。 */
    qz_screen_load(screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, QZ_DUR_SCREEN);
    add_bubble("新增记忆：回到对话页，长按话筒说一句要记住的事即可。", false);
    set_caption("对话中添加", "说一句要记住的事");
}

static void memory_delete_clicked(lv_event_t *event)
{
    (void)event;
    if (!memory_selected) return;
    char body[160];
    snprintf(body, sizeof(body), "{\"action\":\"delete\",\"kind\":\"memory\",\"id\":%llu}", memory_selected);
    memory_request(body);
}

static void open_memory(lv_event_t *event)
{
    (void)event;
    if (!memory_screen) {
        memory_screen = lv_obj_create(NULL);
        qz_style_screen(memory_screen);
        lv_obj_t *header = qz_page_header(memory_screen, "本地记忆");
        lv_obj_t *back = qz_back_button(header);
        lv_obj_align(back, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_add_event_cb(back, memory_back, LV_EVENT_CLICKED, NULL);
        /* 状态行放在列表上沿：它多数时候是列表的说明（"仅保存在本机…"），
         * 出错时又要在按钮旁边立刻可见。留在页面底部时字会被屏幕边缘切掉。 */
        memory_status = qz_text(memory_screen, "正在读取…", 14, qz_color(QZ_TEXT_SECONDARY));
        lv_obj_set_width(memory_status, QZ_DESIGN_W - 28);
        lv_obj_set_height(memory_status, 38);
        lv_label_set_long_mode(memory_status, LV_LABEL_LONG_WRAP);
        lv_obj_align(memory_status, LV_ALIGN_TOP_LEFT, 14, 58);

        memory_list = lv_obj_create(memory_screen);
        lv_obj_set_size(memory_list, QZ_DESIGN_W - 20, 94);
        lv_obj_align(memory_list, LV_ALIGN_TOP_MID, 0, 102);
        lv_obj_set_style_bg_opa(memory_list, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(memory_list, 0, 0);
        lv_obj_set_style_pad_all(memory_list, 6, 0);
        lv_obj_set_style_pad_row(memory_list, 8, 0);
        lv_obj_set_flex_flow(memory_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(memory_list, LV_DIR_VER);
        /* 这里原来是"填写偏好 → 确认保存"的输入框。设备上没有中文输入法，
         * 那是个打不出字的死路，改成一条说明 + 一个回对话页的入口：新增靠说，
         * 这一页只负责查看和忘记。 */
        lv_obj_t *memory_input = qz_card_button(memory_screen, QZ_DESIGN_W - 28, 56);
        lv_obj_align(memory_input, LV_ALIGN_TOP_MID, 0, 202);
        lv_obj_t *input_hint = qz_text(memory_input,
                                       "新增记忆：回对话页长按话筒说一句要记住的事", 15,
                                       qz_color(QZ_TEXT_SECONDARY));
        lv_obj_set_width(input_hint, lv_pct(100));
        lv_obj_set_style_text_align(input_hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(input_hint, LV_LABEL_LONG_DOT);
        lv_obj_center(input_hint);
        memory_save = qz_button(memory_screen, "去对话页说", (QZ_DESIGN_W - 36) / 2, 48);
        lv_obj_align(memory_save, LV_ALIGN_TOP_LEFT, 14, 266);
        qz_obj_set_text_color(lv_obj_get_child(memory_save, 0), QZ_ACCENT_TEXT, 0);
        lv_obj_add_event_cb(memory_save, memory_save_clicked, LV_EVENT_CLICKED, NULL);
        memory_delete = qz_card_button(memory_screen, (QZ_DESIGN_W - 36) / 2, 48);
        lv_obj_align(memory_delete, LV_ALIGN_TOP_RIGHT, -14, 266);
        lv_obj_t *delete_text = qz_text(memory_delete, "删除选中记忆", 17, qz_color(QZ_RED));
        lv_obj_center(delete_text);
        lv_obj_add_state(memory_delete, LV_STATE_DISABLED);
        lv_obj_add_event_cb(memory_delete, memory_delete_clicked, LV_EVENT_CLICKED, NULL);
    }
    qz_screen_load(memory_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, QZ_DUR_SCREEN);
    memory_request("{\"action\":\"list\",\"kind\":\"memory\"}");
}

static void new_chat_clicked(lv_event_t *event)
{
    (void)event;
    if (talking || pending_text_id || abort_pending) {
        add_bubble("请等待当前消息处理完成后再新建聊天", false);
        return;
    }
    if (!qzdesk_core_new_chat()) add_bubble("本地核心不可达，请稍后重试", false);
    else set_caption("请求中", "等待核心受理新会话");
}

typedef enum {
    MENU_NEW_CHAT,
    MENU_MEMORY,
    MENU_STOP,
    MENU_MODE,
} menu_action_t;

static void menu_scrim_clicked(lv_event_t *event)
{
    if (lv_event_get_target(event) != lv_event_get_current_target(event)) return;
    close_menu();
}

static void close_menu(void)
{
    if (menu_overlay) lv_obj_delete(menu_overlay);
    menu_overlay = NULL;
}

static void menu_action(lv_event_t *event)
{
    menu_action_t action = (menu_action_t)(uintptr_t)lv_event_get_user_data(event);
    close_menu();
    if (action == MENU_NEW_CHAT) new_chat_clicked(event);
    else if (action == MENU_MEMORY) open_memory(event);
    else if (action == MENU_STOP) abort_turn_clicked(event);
    else show_mode(!face_mode);
}

static lv_obj_t *menu_cell(lv_obj_t *panel, qz_icon_t icon, const char *caption,
                           qz_color_token_t color, menu_action_t action,
                           bool right_edge, bool bottom_edge)
{
    lv_obj_t *cell = lv_button_create(panel);
    lv_obj_set_size(cell, lv_pct(50), menu_row_height());
    qz_touch_target(cell);
    lv_obj_set_style_radius(cell, 0, 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
    qz_obj_set_bg_color(cell, QZ_FILL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(cell, 1, 0);
    qz_obj_set_border_color(cell, QZ_SEPARATOR, 0);
    lv_obj_set_style_border_opa(cell, (lv_opa_t)120, 0);
    lv_obj_set_style_border_side(
        cell, (lv_border_side_t)((right_edge ? LV_BORDER_SIDE_RIGHT : 0) |
                                 (bottom_edge ? LV_BORDER_SIDE_BOTTOM : 0)), 0);
    lv_obj_set_style_shadow_width(cell, 0, 0);
    lv_obj_set_style_pad_left(cell, 10, 0);
    lv_obj_set_style_pad_right(cell, 6, 0);
    lv_obj_set_style_pad_column(cell, qz_design_px(7), 0);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    qz_add_press_feedback(cell);

    lv_obj_t *glyph = qz_icon_image(cell, icon, menu_icon_px(), qz_color(color));
    if (glyph) lv_obj_clear_flag(glyph, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *label = qz_text(cell, caption, 15, qz_color(color));
    lv_obj_set_width(label, 0);
    lv_obj_set_flex_grow(label, 1);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_add_event_cb(cell, menu_action, LV_EVENT_CLICKED, (void *)(uintptr_t)action);
    return cell;
}

static void open_menu(lv_event_t *event)
{
    (void)event;
    if (menu_overlay) close_menu();
    menu_overlay = qz_overlay_create(lv_layer_top());
    lv_obj_set_style_bg_opa(menu_overlay, (lv_opa_t)26, 0);
    lv_obj_add_event_cb(menu_overlay, menu_scrim_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(menu_overlay);
    /* 两列菜单每格都要同时放下 26px 图标和完整动作名。252 设计像素在
     * 320×240 面板上只剩约 81 实像素/格，四项文案都会被裁掉；340 仍能贴右
     * 展开，同时在 480 面板上留出完整的文字宽度。 */
    lv_obj_set_size(panel, 340, LV_SIZE_CONTENT);
    lv_obj_align(panel, LV_ALIGN_TOP_RIGHT, -6, QZ_HEADER_H + 4);
    qz_style_sheet(panel);
    qz_obj_set_bg_color(panel, QZ_CARD, 0);
    lv_obj_set_style_bg_grad_dir(panel, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_radius(panel, 5, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    qz_obj_set_border_color(panel, QZ_SEPARATOR, 0);
    lv_obj_set_style_border_opa(panel, (lv_opa_t)150, 0);
    lv_obj_set_style_shadow_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 4, 0);
    lv_obj_set_style_pad_row(panel, 0, 0);
    lv_obj_set_style_pad_column(panel, 0, 0);
    lv_obj_set_style_clip_corner(panel, true, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    menu_cell(panel, QZ_ICON_PLUS, "新建聊天", QZ_TEXT, MENU_NEW_CHAT, true, true);
    menu_cell(panel, QZ_ICON_DATABASE, "本地记忆", QZ_TEXT, MENU_MEMORY, false, true);
    menu_cell(panel, QZ_ICON_X, "停止回复", QZ_RED, MENU_STOP, true, false);
    menu_cell(panel, face_mode ? QZ_ICON_KEYBOARD : QZ_ICON_PHOTO,
              face_mode ? "聊天模式" : "表情模式", QZ_ACCENT_TEXT, MENU_MODE,
              false, false);
    qz_animate_pop_in(panel);
}

static void build_toolbar(void)
{
    lv_obj_t *header = qz_page_header(screen, "AI 助手");
    lv_obj_t *title = lv_obj_get_child(header, 0);
    if (title) lv_obj_align(title, LV_ALIGN_LEFT_MID, 58, -1);

    lv_obj_t *back = qz_back_button(header);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 4, 0);
    set_button_icon(back, QZ_ICON_CHEVRON_LEFT, 22, QZ_TEXT);
    lv_obj_add_event_cb(back, back_to_desktop, LV_EVENT_CLICKED, NULL);

    lv_obj_t *more = qz_icon_button(header, LV_SYMBOL_LIST, 44);
    lv_obj_align(more, LV_ALIGN_RIGHT_MID, -6, 0);
    set_button_icon(more, QZ_ICON_LIST, 24, QZ_TEXT);
    lv_obj_add_event_cb(more, open_menu, LV_EVENT_CLICKED, NULL);

    /* 状态直接贴在标题右侧，只用一颗圆点分隔层级。原来那块 132px 的实心
     * 胶囊在 320 屏上抢走了近半条页头，也让标题看起来像工具栏。 */
    lv_obj_t *state_line = lv_obj_create(header);
    lv_obj_set_size(state_line, 226, 24);
    lv_obj_align(state_line, LV_ALIGN_LEFT_MID, 162, 0);
    lv_obj_set_style_bg_opa(state_line, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(state_line, 0, 0);
    lv_obj_set_style_pad_all(state_line, 0, 0);
    lv_obj_set_style_pad_column(state_line, qz_design_px(6), 0);
    lv_obj_set_flex_flow(state_line, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(state_line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(state_line, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(state_line, LV_OBJ_FLAG_SCROLLABLE);

    state_dot = lv_obj_create(state_line);
    int32_t dot = qz_design_px(6);
    lv_obj_set_size(state_dot, dot, dot);
    lv_obj_set_style_radius(state_dot, LV_RADIUS_CIRCLE, 0);
    qz_obj_set_bg_color(state_dot, QZ_ORANGE, 0);
    lv_obj_set_style_border_width(state_dot, 0, 0);
    lv_obj_clear_flag(state_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(state_dot, LV_OBJ_FLAG_SCROLLABLE);
    state_label = qz_text(state_line, "连接中", 13, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_set_width(state_label, 0);
    lv_obj_set_flex_grow(state_label, 1);
    lv_label_set_long_mode(state_label, LV_LABEL_LONG_DOT);
}

static void build_chat_view(void)
{
    const int32_t composer_h = composer_height();
    const int32_t control_w = composer_control_w();
    const int32_t control_h = composer_control_h();

    /* 聊天记录在固定的内容窗口内滚动；键盘打开时只缩短这块窗口。 */
    chat_view = lv_obj_create(screen);
    lv_obj_set_size(chat_view, lv_pct(100), QZ_DESIGN_H);
    lv_obj_align(chat_view, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(chat_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chat_view, 0, 0);
    lv_obj_set_style_pad_all(chat_view, 0, 0);
    lv_obj_clear_flag(chat_view, LV_OBJ_FLAG_SCROLLABLE);

    chat_list = lv_obj_create(chat_view);
    lv_obj_set_size(chat_list, lv_pct(100), chat_list_height());
    lv_obj_align(chat_list, LV_ALIGN_TOP_MID, 0, current_chat_top());
    lv_obj_set_style_bg_opa(chat_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chat_list, 0, 0);
    lv_obj_set_style_pad_left(chat_list, 12, 0);
    lv_obj_set_style_pad_right(chat_list, 12, 0);
    lv_obj_set_style_pad_top(chat_list, 8, 0);
    lv_obj_set_style_pad_bottom(chat_list, 8, 0);
    lv_obj_set_style_pad_row(chat_list, 10, 0);
    lv_obj_set_flex_flow(chat_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(chat_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_add_flag(chat_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(chat_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(chat_list, LV_SCROLLBAR_MODE_OFF);

    /* 第一次打开时只留一句问候和可直接发送的问法。左侧对齐、无卡片，让空白
     * 本身成为页面的安静部分。 */
    const bool compact = qz_compact();
    chat_empty = lv_obj_create(chat_list);
    lv_obj_set_size(chat_empty, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(chat_empty, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chat_empty, 0, 0);
    lv_obj_set_style_pad_left(chat_empty, 14, 0);
    lv_obj_set_style_pad_right(chat_empty, 14, 0);
    lv_obj_set_style_pad_top(chat_empty, 0, 0);
    lv_obj_set_style_pad_bottom(chat_empty, 0, 0);
    lv_obj_set_style_pad_row(chat_empty, compact ? 2 : 0, 0);
    lv_obj_set_flex_flow(chat_empty, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(chat_empty, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(chat_empty, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *intro = lv_obj_create(chat_empty);
    lv_obj_set_size(intro, lv_pct(100), empty_intro_height());
    lv_obj_set_style_bg_opa(intro, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(intro, 0, 0);
    lv_obj_set_style_pad_all(intro, 0, 0);
    lv_obj_set_style_pad_column(intro, compact ? 10 : 14, 0);
    lv_obj_set_flex_flow(intro, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(intro, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(intro, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(intro, LV_OBJ_FLAG_CLICKABLE);

    qz_face_create(intro, 60);
    lv_obj_t *copy = lv_obj_create(intro);
    lv_obj_set_size(copy, 0, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(copy, 1);
    lv_obj_set_style_bg_opa(copy, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(copy, 0, 0);
    lv_obj_set_style_pad_all(copy, 0, 0);
    lv_obj_set_style_pad_row(copy, 2, 0);
    lv_obj_set_flex_flow(copy, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(copy, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(copy, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(copy, LV_OBJ_FLAG_CLICKABLE);
    qz_text(copy, "今天想做什么？", compact ? 18 : 20, qz_color(QZ_TEXT));
    lv_obj_t *empty_hint = qz_text(copy, "我在，慢慢说。",
                                   compact ? 13 : 14, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_set_width(empty_hint, lv_pct(100));
    lv_label_set_long_mode(empty_hint, LV_LABEL_LONG_CLIP);

    if (compact) {
        lv_obj_t *hint = qz_text(chat_empty, "可以这样问", 12,
                                 qz_color(QZ_TEXT_TERTIARY));
        lv_obj_set_width(hint, lv_pct(100));
        lv_obj_set_style_pad_top(hint, 4, 0);
        lv_obj_t *row = lv_obj_create(chat_empty);
        lv_obj_set_size(row, lv_pct(100), preset_height());
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_column(row, qz_design_px(8), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        preset_chip(row, 0);
        preset_chip(row, 1);
    } else {
        preset_row(chat_empty, 0, false);
        preset_row(chat_empty, 1, false);
        preset_row(chat_empty, 2, true);
    }

    composer = lv_obj_create(chat_view);
    lv_obj_set_size(composer, lv_pct(100), composer_h);
    lv_obj_align(composer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(composer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(composer, 0, 0);
    lv_obj_set_style_pad_all(composer, 0, 0);
    lv_obj_clear_flag(composer, LV_OBJ_FLAG_SCROLLABLE);

    /* 一块安静、实心的输入面。所有动作共用同一套近方形工作按钮，
     * 让输入区像工具台而不是漂浮的胶囊。 */
    lv_obj_t *bar = lv_obj_create(composer);
    lv_obj_set_size(bar, QZ_DESIGN_W - 16, composer_h - 6);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, -3);
    lv_obj_set_style_radius(bar, 6, 0);
    qz_obj_set_bg_color(bar, QZ_CARD, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    qz_obj_set_border_color(bar, QZ_SEPARATOR, 0);
    lv_obj_set_style_border_opa(bar, (lv_opa_t)120, 0);
    lv_obj_set_style_shadow_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, qz_design_px(4), 0);
    lv_obj_set_style_pad_column(bar, qz_design_px(5), 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    message_input = lv_textarea_create(bar);
    lv_obj_set_size(message_input, 0, control_h);
    lv_obj_set_flex_grow(message_input, 1);
    lv_textarea_set_one_line(message_input, true);
    /* one_line 把高度收回内容高度：那样输入框的命中区只剩一行字，小屏上点
     * 进去只能靠运气。高度按触控下限写死，这里补回来再上样式。 */
    lv_obj_set_height(message_input, control_h);
    lv_textarea_set_placeholder_text(message_input, "问点什么…");
    qz_style_textarea(message_input);
    lv_obj_set_style_bg_opa(message_input, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(message_input, LV_OPA_TRANSP, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(message_input, 0, 0);
    lv_obj_set_style_radius(message_input, 0, 0);
    lv_obj_set_style_pad_left(message_input, 8, 0);
    lv_obj_set_style_pad_right(message_input, 4, 0);
    lv_obj_add_event_cb(message_input, input_event, LV_EVENT_ALL, NULL);

    lv_obj_t *utility_group = lv_obj_create(bar);
    lv_obj_set_size(utility_group, control_w * 2, control_h);
    lv_obj_set_style_bg_opa(utility_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(utility_group, 0, 0);
    lv_obj_set_style_pad_all(utility_group, 0, 0);
    lv_obj_set_style_pad_column(utility_group, 0, 0);
    lv_obj_set_flex_flow(utility_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(utility_group, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(utility_group, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *keyboard_button = lv_button_create(utility_group);
    lv_obj_set_size(keyboard_button, control_w, control_h);
    lv_obj_set_style_radius(keyboard_button, 5, 0);
    qz_obj_set_bg_color(keyboard_button, QZ_FILL, 0);
    lv_obj_set_style_bg_opa(keyboard_button, LV_OPA_COVER, 0);
    qz_obj_set_bg_color(keyboard_button, QZ_FILL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(keyboard_button, 0, 0);
    lv_obj_set_style_shadow_width(keyboard_button, 0, 0);
    lv_obj_set_style_pad_all(keyboard_button, 0, 0);
    qz_add_press_feedback(keyboard_button);
    lv_obj_t *keyboard_glyph = qz_icon_image(keyboard_button, QZ_ICON_KEYBOARD,
                                             chat_icon_px(),
                                             qz_color(QZ_TEXT));
    lv_obj_center(keyboard_glyph);
    lv_obj_add_event_cb(keyboard_button, toggle_keyboard, LV_EVENT_CLICKED, NULL);

    /* 手机是设备上唯一能自然输入中文的通道：入口放在输入栏，和键盘并排，
     * 不占标题栏那四个已经排满的动作位。 */
    bridge_button = lv_button_create(utility_group);
    lv_obj_set_size(bridge_button, control_w, control_h);
    lv_obj_set_style_radius(bridge_button, 5, 0);
    qz_obj_set_bg_color(bridge_button, QZ_FILL, 0);
    lv_obj_set_style_bg_opa(bridge_button, LV_OPA_COVER, 0);
    qz_obj_set_bg_color(bridge_button, QZ_FILL_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(bridge_button, 0, 0);
    lv_obj_set_style_shadow_width(bridge_button, 0, 0);
    lv_obj_set_style_pad_all(bridge_button, 0, 0);
    qz_add_press_feedback(bridge_button);
    lv_obj_t *bridge_glyph = qz_icon_image(bridge_button, QZ_ICON_DEVICE_MOBILE,
                                           chat_icon_px(),
                                           qz_color(QZ_TEXT));
    if (bridge_glyph) lv_obj_center(bridge_glyph);
    lv_obj_add_event_cb(bridge_button, open_bridge, LV_EVENT_CLICKED, NULL);

    lv_obj_t *mic = lv_button_create(bar);
    lv_obj_set_size(mic, control_w, control_h);
    lv_obj_set_style_radius(mic, 5, 0);
    qz_obj_set_bg_color(mic, QZ_ACCENT_TINT, 0);
    qz_obj_set_bg_color(mic, QZ_ACCENT_DARK, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(mic, 0, 0);
    lv_obj_set_style_shadow_width(mic, 0, 0);
    lv_obj_set_style_pad_all(mic, 0, 0);
    qz_add_press_feedback(mic);
    qz_add_press_scale(mic, 94);
    lv_obj_t *mic_box = mic_glyph(mic, chat_icon_px(), qz_color(QZ_ACCENT_TEXT));
    lv_obj_center(mic_box);
    lv_obj_add_event_cb(mic, talk_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(mic, talk_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(mic, talk_released, LV_EVENT_PRESS_LOST, NULL);

    send_button = lv_button_create(bar);
    lv_obj_set_size(send_button, control_w, control_h);
    qz_style_primary_button(send_button);
    lv_obj_set_style_radius(send_button, 5, 0);
    lv_obj_set_style_pad_all(send_button, 0, 0);
    qz_add_press_scale(send_button, 94);
    lv_obj_t *arrow = qz_icon_image(send_button, QZ_ICON_ARROW_UP, chat_icon_px(),
                                    qz_color(QZ_TEXT_ON_ACCENT));
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
    lv_obj_set_size(face_view, lv_pct(100), face_height());
    lv_obj_align(face_view, LV_ALIGN_TOP_MID, 0, current_chat_top());
    lv_obj_set_style_bg_opa(face_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(face_view, 0, 0);
    lv_obj_set_style_pad_all(face_view, 0, 0);
    lv_obj_clear_flag(face_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(face_view, LV_OBJ_FLAG_HIDDEN);

    big_face = qz_face_create(face_view, 160);
    lv_obj_align(big_face, LV_ALIGN_LEFT_MID, 24, -12);

    face_caption = qz_text(face_view, qz_face_state_text(QZ_FACE_IDLE), 20, qz_color(QZ_TEXT));
    lv_obj_align(face_caption, LV_ALIGN_TOP_LEFT, 210, 76);
    /* 两行封顶：识别结果长短不一，字幕自己撑高会压到下面的语音键上。 */
    lv_obj_set_size(face_caption, 240, 54);
    lv_label_set_long_mode(face_caption, LV_LABEL_LONG_DOT);

    lv_obj_t *talk = lv_button_create(face_view);
    lv_obj_set_size(talk, 240, 52);
    qz_touch_target(talk);
    lv_obj_align(talk, LV_ALIGN_TOP_LEFT, 210, 140);
    lv_obj_set_style_radius(talk, 5, 0);
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
    talk_button_label = qz_text(talk, "长按说话", 17, qz_color(QZ_TEXT_ON_ACCENT));
    lv_obj_align(talk_button_label, LV_ALIGN_CENTER, 14, 0);
    lv_obj_t *mic_box = lv_obj_get_child(talk, 0);
    lv_obj_align(mic_box, LV_ALIGN_CENTER, -46, 0);
    lv_obj_add_event_cb(talk, talk_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(talk, talk_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(talk, talk_released, LV_EVENT_PRESS_LOST, NULL);
}

/** 返回键和侧滑返回共用的一段：键盘开着就先收键盘，录音中就停下，再退主页。 */
static void assistant_back(void)
{
    if (menu_overlay) {
        close_menu();
        return;
    }
    if (keyboard && !lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN)) {
        set_keyboard_visible(false);
        return;
    }
    if (talking) {
        talking = false;
        qzdesk_core_listen(false);
    }
    stop_typing();
    qz_screen_load(desktop_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, QZ_DUR_SCREEN);
}

static void back_to_desktop(lv_event_t *event)
{
    (void)event;
    assistant_back();
}

/* ------------------------------------------------------------------------- *
 * Screen
 * ------------------------------------------------------------------------- */

lv_obj_t *qz_assistant_create(void)
{
    screen = lv_obj_create(NULL);
    session_notice = NULL;
    session_notice_label = NULL;
    qz_style_screen(screen);

    build_toolbar();
    build_chat_view();
    build_face_view();
    /* The bar is built first, so the list would paint over it: lift it back on
     * top now that everything behind it exists. */
    lv_obj_move_foreground(lv_obj_get_child(screen, 0));
    face_mode = false;

    set_caption("连接中", qz_face_state_text(QZ_FACE_IDLE));
    qz_screen_set_back_handler(screen, assistant_back);
    lv_timer_create(poll_qzdesk_core, 100, NULL);
    return screen;
}

bool qz_assistant_prepare_text(const char *text)
{
    if (!screen || pending_text_id || talking || !text) return false;
    if (lv_textarea_get_text(message_input)[0]) return false;
    lv_textarea_set_text(message_input, text);
    set_keyboard_visible(false);
    show_mode(false);
    qz_screen_load(screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, QZ_DUR_SCREEN);
    return true;
}

void qz_assistant_set_desktop(lv_obj_t *desktop) { desktop_screen = desktop; }
