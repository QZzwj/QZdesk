#include "desktop.h"
#include "applets.h"
#include "ai_face.h"
#include "apps.h"
#include "assistant.h"
#include "config.h"
#include "settings.h"
#include "skills_page.h"
#include "theme.h"
#include "weather_card.h"
#include "weather_page.h"
#include "icon_assets.h"
#include "qzdesk_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Signal Workstation: one continuous scan path from system state to action.
 * The surfaces stay flush with the design grid instead of floating as cards. */
#define HOME_STATUS_H 44
#define HOME_GAP      7
#define HOME_HERO_Y   (HOME_STATUS_H + HOME_GAP)
#define HOME_HERO_H   144
#define HOME_NEXT_Y   (HOME_HERO_Y + HOME_HERO_H + HOME_GAP)
#define HOME_NEXT_H   43
#define HOME_ROW_Y    (HOME_NEXT_Y + HOME_NEXT_H + HOME_GAP)
#define HOME_ROW_H    60
#define HOME_WEATHER_W 294
#define HOME_TILE_GAP 10
#define HOME_TILE_W   (QZ_DESIGN_W - 2 * QZ_GUTTER - HOME_TILE_GAP - HOME_WEATHER_W)

static lv_obj_t *assistant_screen;
static lv_obj_t *apps_screen;
static lv_obj_t *settings_screen;
static lv_obj_t *desktop_screen;
static lv_obj_t *clock_label;
static lv_obj_t *address_label;
static lv_obj_t *wifi_icon;
static lv_obj_t *battery_icon;
static lv_obj_t *battery_label;
static lv_obj_t *hero_badge;
static lv_obj_t *hero_badge_dot;
static lv_obj_t *hero_badge_text;
static lv_obj_t *hero_face;
static lv_obj_t *hero_headline;
static lv_obj_t *hero_hint;
static lv_obj_t *next_value;
static lv_obj_t *next_tile;
static lv_obj_t *next_caption;
static qz_icon_t next_icon = QZ_ICON_COUNT;
static int core_state = -1;
static uint32_t core_seen_at;

static bool audio_is_muted(void)
{
    const char *value = getenv("QZDESK_AUDIO_DISABLED");
    return value && (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
                     strcmp(value, "TRUE") == 0);
}

static void core_status(const qzdesk_core_event_t *event, void *data)
{
    (void)data;
    if (event->type == QZDESK_CORE_EVENT_STATUS) {
        core_state = event->state;
        core_seen_at = lv_tick_get();
    } else if (event->type == QZDESK_CORE_EVENT_ACTIVATION) {
        core_state = 2;
        core_seen_at = lv_tick_get();
    }
}

static void wifi_clicked(lv_event_t *event)
{
    (void)event;
    qz_settings_open_wlan(desktop_screen);
}

/* ------------------------------------------------------------------------- *
 * Page transitions
 * ------------------------------------------------------------------------- */

static void show_assistant(lv_event_t *event)
{
    (void)event;
    qz_screen_load(assistant_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, QZ_DUR_SCREEN);
}

static void show_apps(lv_event_t *event)
{
    (void)event;
    qz_screen_load(apps_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, QZ_DUR_SCREEN);
}

/* ------------------------------------------------------------------------- *
 * Status bar
 * ------------------------------------------------------------------------- */

static qz_icon_t battery_icon_for(int level)
{
    if (level < 0 || level >= 90) return QZ_ICON_BATTERY_4;
    if (level >= 65) return QZ_ICON_BATTERY_3;
    if (level >= 40) return QZ_ICON_BATTERY_2;
    if (level >= 15) return QZ_ICON_BATTERY_1;
    return QZ_ICON_BATTERY_OFF;
}

static void update_status(void)
{
    time_t now = time(NULL);
    struct tm local_time;
    char text[16];
    localtime_r(&now, &local_time);
    strftime(text, sizeof(text), "%H:%M", &local_time);
    /* Only write when the value really changed: a no-op set still invalidates
     * the label, and these two tick every second. */
    if (clock_label && strcmp(lv_label_get_text(clock_label), text) != 0) {
        lv_label_set_text(clock_label, text);
    }

    char address[32];
    qz_device_ip(address, sizeof(address));
    if (address_label && strcmp(lv_label_get_text(address_label), address) != 0) {
        lv_label_set_text(address_label, address);
    }
    int connected = strcmp(address, "未连接") != 0;
    if (wifi_icon) {
        lv_obj_set_style_image_recolor(wifi_icon,
                                       qz_color(connected ? QZ_TEXT : QZ_TEXT_TERTIARY), 0);
    }

    int level = qz_battery_level();
    if (battery_icon) {
        /* 同一个设计尺寸取档，别在这里写物理像素：图标是照着设计稿 26px 烘的，
         * 写死 16 会在 320×240 上跳到 17px 的那一档，比旁边的 wifi 还大一圈。 */
        lv_image_set_src(battery_icon,
                         qz_icon(battery_icon_for(level), qz_scale_px(26)));
        if (level < 0) {
            /* 没有电池信息（比如桌面调试）：图标降灰，别让人以为满电 */
            lv_obj_set_style_image_recolor(battery_icon, qz_color(QZ_TEXT_TERTIARY), 0);
        }
    }
    if (battery_label) {
        if (level < 0) {
            lv_obj_add_flag(battery_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            char percent[16];
            snprintf(percent, sizeof(percent), "%d%%", level);
            lv_label_set_text(battery_label, percent);
            lv_obj_clear_flag(battery_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (hero_badge && hero_badge_text) {
        bool fresh = core_state >= 0 && lv_tick_elaps(core_seen_at) < 15000;
        bool ready = fresh && (core_state == 3 || core_state == 5 || core_state == 6);
        const char *label = !fresh ? "离线" : core_state == 2 ? "连接中" :
                            core_state == 5 ? "聆听" : core_state == 6 ? "播报" :
                            ready ? "在线" : "待命";
        const char *headline = !fresh ? "需要重新连接" : core_state == 2 ? "正在唤醒助手" :
                               core_state == 5 ? "我在听" : core_state == 6 ? "正在回答" :
                               ready ? "你好，我在" : "准备就绪";
        const char *hint = !fresh ? "点按查看连接状态" : core_state == 5 ? "直接说出你的问题" :
                           core_state == 6 ? "可随时点按打断" : "点按开始对话";
        lv_obj_set_style_bg_opa(hero_badge, ready ? LV_OPA_COVER : (lv_opa_t)190, 0);
        lv_label_set_text(hero_badge_text, label);
        if (hero_headline) lv_label_set_text(hero_headline, headline);
        if (hero_hint) lv_label_set_text(hero_hint, hint);
        qz_obj_set_text_color(hero_badge_text, QZ_ACCENT_DARK, 0);
        if (hero_badge_dot) {
            lv_obj_set_style_bg_color(hero_badge_dot,
                                      qz_color(ready ? QZ_GREEN : QZ_ORANGE), 0);
        }
    }
}

/* ------------------------------------------------------------------------- *
 * Cards
 * ------------------------------------------------------------------------- */

/** The one dominant surface: a live assistant face, a changing headline, and a
 * precise status readout. The whole surface is one large target. */
static void hero_card(lv_obj_t *parent)
{
    lv_obj_t *card = qz_card_button(parent, QZ_DESIGN_W - 2 * QZ_GUTTER, HOME_HERO_H);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, QZ_GUTTER, HOME_HERO_Y);
    lv_obj_set_style_radius(card, QZ_RADIUS_CARD, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_COVER, 0);

    lv_obj_t *signal_rail = lv_obj_create(card);
    lv_obj_set_size(signal_rail, 5, HOME_HERO_H - 20);
    lv_obj_align(signal_rail, LV_ALIGN_LEFT_MID, 0, 0);
    qz_obj_set_bg_color(signal_rail, QZ_ACCENT, 0);
    lv_obj_set_style_bg_opa(signal_rail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(signal_rail, 0, 0);
    lv_obj_set_style_radius(signal_rail, 0, 0);
    lv_obj_clear_flag(signal_rail, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(signal_rail, LV_OBJ_FLAG_SCROLLABLE);

    hero_face = qz_face_create(card, 96);
    lv_obj_align(hero_face, LV_ALIGN_LEFT_MID, 24, -4);
    qz_face_set_state(hero_face, QZ_FACE_IDLE);

    lv_obj_t *divider = qz_separator(card, HOME_HERO_H - 20, true);
    lv_obj_align(divider, LV_ALIGN_TOP_LEFT, 140, 10);

    lv_obj_t *caption = qz_text(card, "QZdesk 助手", 13, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(caption, LV_ALIGN_TOP_LEFT, 160, 15);
    hero_headline = qz_text(card, "你好，我在", 31, qz_color(QZ_TEXT));
    lv_obj_align(hero_headline, LV_ALIGN_TOP_LEFT, 160, 38);
    hero_hint = qz_text(card, "点按开始对话", 15, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(hero_hint, LV_ALIGN_TOP_LEFT, 160, 79);

    lv_obj_t *go = qz_chevron(card, qz_color(QZ_ACCENT), 20);
    lv_obj_align(go, LV_ALIGN_RIGHT_MID, -16, 0);

    hero_badge = lv_obj_create(card);
    lv_obj_set_size(hero_badge, 88, 26);
    lv_obj_align(hero_badge, LV_ALIGN_TOP_RIGHT, -16, 14);
    lv_obj_set_style_radius(hero_badge, 4, 0);
    qz_obj_set_bg_color(hero_badge, QZ_ACCENT_TINT, 0);
    lv_obj_set_style_bg_opa(hero_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hero_badge, 1, 0);
    qz_obj_set_border_color(hero_badge, QZ_ACCENT, 0);
    lv_obj_set_style_border_opa(hero_badge, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(hero_badge, 0, 0);
    lv_obj_clear_flag(hero_badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(hero_badge, LV_OBJ_FLAG_SCROLLABLE);

    hero_badge_dot = lv_obj_create(hero_badge);
    lv_obj_set_size(hero_badge_dot, 8, 8);
    lv_obj_align(hero_badge_dot, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_radius(hero_badge_dot, LV_RADIUS_CIRCLE, 0);
    qz_obj_set_bg_color(hero_badge_dot, QZ_GREEN, 0);
    lv_obj_set_style_border_width(hero_badge_dot, 0, 0);
    lv_obj_clear_flag(hero_badge_dot, LV_OBJ_FLAG_CLICKABLE);
    hero_badge_text = qz_text(hero_badge, "在线", 14, qz_color(QZ_ACCENT_DARK));
    lv_obj_align(hero_badge_text, LV_ALIGN_LEFT_MID, 26, 0);

    lv_obj_t *signal_line = qz_separator(card, QZ_DESIGN_W - 2 * QZ_GUTTER - 174, false);
    lv_obj_align(signal_line, LV_ALIGN_BOTTOM_LEFT, 160, -32);
    const char *labels[] = {
        "核心 AUTO", audio_is_muted() ? "语音 静音" : "语音 ON", "READY"
    };
    for (int index = 0; index < 3; index++) {
        int x = 160 + index * 95;
        lv_obj_t *name = qz_text(card, labels[index], 11, qz_color(QZ_TEXT_SECONDARY));
        lv_obj_set_width(name, 88);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_align(name, LV_ALIGN_BOTTOM_LEFT, x, -9);
    }

    lv_obj_add_event_cb(card, show_assistant, LV_EVENT_CLICKED, NULL);
}

/** 「接下来」卡：番茄钟在跑就先报它，否则报最近的一条提醒，都没有就说暂无安排。
 *  点一下就进对应的页面 —— 主页只负责回答"现在有什么事、要往哪去"。 */
static void next_card_tapped(lv_event_t *event)
{
    (void)event;
    qz_applets_glance_t glance;
    qz_applets_glance(&glance);
    lv_obj_t *target = qz_applets_screen(glance.focus_active ? QZ_APPLET_POMODORO
                                                             : QZ_APPLET_REMINDER);
    if (!target) return;
    /* 从主页直接进小程序，返回键要回主页（默认回的是应用页）。 */
    qz_applets_set_back(desktop_screen);
    qz_screen_load(target, LV_SCR_LOAD_ANIM_MOVE_LEFT, QZ_DUR_SCREEN);
}

static lv_obj_t *next_card(lv_obj_t *parent)
{
    lv_obj_t *card = qz_card_button(parent, QZ_DESIGN_W - 2 * QZ_GUTTER, HOME_NEXT_H);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, QZ_GUTTER, HOME_NEXT_Y);
    lv_obj_set_style_radius(card, QZ_RADIUS_CARD, 0);

    lv_obj_t *rail = lv_obj_create(card);
    lv_obj_set_size(rail, 4, HOME_NEXT_H);
    lv_obj_align(rail, LV_ALIGN_LEFT_MID, 0, 0);
    qz_obj_set_bg_color(rail, QZ_ACCENT, 0);
    lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rail, 0, 0);
    lv_obj_set_style_radius(rail, 0, 0);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_SCROLLABLE);

    next_tile = qz_squircle(card, 28, QZ_ACCENT_TINT);
    lv_obj_align(next_tile, LV_ALIGN_LEFT_MID, 12, 0);

    next_caption = qz_text(card, "接下来", 11, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(next_caption, LV_ALIGN_TOP_LEFT, 50, 6);
    next_value = qz_text(card, "暂无安排", 17, qz_color(QZ_TEXT));
    /* Long reminder text must remain inside the fixed strip. Set the label
     * policy before its final geometry so LVGL never reflows it to content
     * height and pushes the baseline through the card edge. */
    lv_label_set_long_mode(next_value, LV_LABEL_LONG_DOT);
    lv_obj_set_width(next_value, QZ_DESIGN_W - 2 * QZ_GUTTER - 50 - 40);
    lv_obj_set_height(next_value, 20);
    if (qz_compact()) {
        lv_obj_add_flag(next_caption, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(next_value, LV_ALIGN_LEFT_MID, 50, 0);
    } else {
        lv_obj_align(next_value, LV_ALIGN_TOP_LEFT, 50, 21);
    }

    lv_obj_t *go = qz_chevron(card, qz_color(QZ_TEXT_TERTIARY), 14);
    lv_obj_align(go, LV_ALIGN_RIGHT_MID, -14, 0);

    lv_obj_add_event_cb(card, next_card_tapped, LV_EVENT_CLICKED, NULL);
    return card;
}

static void update_next_card(void)
{
    qz_applets_glance_t glance;
    char text[64];
    qz_icon_t icon = QZ_ICON_BELL;
    qz_color_token_t tint = QZ_TEXT_TERTIARY;

    if (!next_value) return;
    qz_applets_glance(&glance);
    if (glance.focus_active) {
        int remaining = glance.focus_remaining > 0 ? glance.focus_remaining : 0;
        snprintf(text, sizeof(text), "%s · %02d:%02d",
                 glance.focus_paused ? "已暂停" : glance.focus_break ? "休息中" : "专注中",
                 remaining / 60, remaining % 60);
        icon = QZ_ICON_TIMER;
        tint = QZ_ACCENT_DARK;
    } else if (glance.has_reminder) {
        snprintf(text, sizeof(text), "%s%02d:%02d %s",
                 glance.reminder_in_minutes >= 24 * 60 ? "明日 " : "",
                 glance.reminder_hour, glance.reminder_minute, glance.reminder_label);
        tint = QZ_ACCENT_DARK;
    } else {
        snprintf(text, sizeof(text), "暂无安排");
    }
    if (strcmp(lv_label_get_text(next_value), text) != 0) lv_label_set_text(next_value, text);

    if (icon != next_icon) {
        next_icon = icon;
        lv_obj_clean(next_tile);
        lv_obj_t *glyph = qz_icon_image(next_tile, icon, 20, qz_color(tint));
        if (glyph) lv_obj_center(glyph);
    }
}

/** 陪伴表情跟着番茄钟走：专注=思考、休息=开心、暂停=困倦、没在跑=待机。 */
static void update_hero_face(void)
{
    if (!hero_face) return;
    qz_face_set_state(hero_face, qz_applets_focus_face());
}

/** 天气卡右边的一级入口：与天气卡同高、在剩余宽度里居中。
 *  主页只留这一个去处，技能、设置、性能这些二级页都在应用页里，不重复占位。 */
static lv_obj_t *home_apps_tile(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *card = qz_card_button(parent, w, h);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_radius(card, QZ_RADIUS_CARD, 0);

    lv_obj_t *icon = qz_squircle(card, 32, QZ_ACCENT_TINT);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_t *glyph = qz_symbol(icon, LV_SYMBOL_LIST, 18, qz_color(QZ_ACCENT_DARK));
    lv_obj_center(glyph);

    lv_obj_t *name = qz_text(card, "应用", 18, qz_color(QZ_TEXT));
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 52, qz_compact() ? 0 : -7);
    lv_obj_t *meta = qz_text(card, "全部工具", 12, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(meta, LV_ALIGN_LEFT_MID, 52, 11);
    if (qz_compact()) lv_obj_add_flag(meta, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *go = qz_chevron(card, qz_color(QZ_TEXT_TERTIARY), 14);
    lv_obj_align(go, LV_ALIGN_RIGHT_MID, -12, 0);
    return card;
}

/* ------------------------------------------------------------------------- *
 * Screen
 * ------------------------------------------------------------------------- */

static void status_timer(lv_timer_t *timer)
{
    (void)timer;
    update_status();
    update_next_card();
    update_hero_face();
}

lv_obj_t *qz_desktop_create(const char *server)
{
    (void)server;
    desktop_screen = lv_obj_create(NULL);
    qz_style_screen(desktop_screen);

    lv_obj_t *status = lv_obj_create(desktop_screen);
    lv_obj_set_size(status, lv_pct(100), HOME_STATUS_H);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 0);
    qz_style_header(status);

    clock_label = qz_text(status, "--:--", 23, qz_color(QZ_TEXT));
    lv_obj_align(clock_label, LV_ALIGN_LEFT_MID, QZ_GUTTER, 0);
    lv_obj_t *brand = qz_text(status, "QZDESK", 12, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(brand, LV_ALIGN_LEFT_MID, 84, -7);
    lv_obj_t *mode = qz_text(status, "本地工作台", 13, qz_color(QZ_TEXT_SECONDARY));
    if (qz_compact()) {
        lv_obj_add_flag(brand, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(mode, LV_ALIGN_LEFT_MID, 84, 0);
    } else {
        lv_obj_align(mode, LV_ALIGN_LEFT_MID, 84, 9);
    }

    battery_icon = qz_icon_image(status, QZ_ICON_BATTERY_4, 26, qz_color(QZ_TEXT));
    lv_obj_align(battery_icon, LV_ALIGN_RIGHT_MID, -QZ_GUTTER, 0);
    battery_label = qz_text(status, "100%", 16, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(battery_label, LV_ALIGN_RIGHT_MID, -QZ_GUTTER - 36, 0);
    wifi_icon = qz_icon_image(status, QZ_ICON_WIFI, 26, qz_color(QZ_TEXT));
    lv_obj_align(wifi_icon, LV_ALIGN_RIGHT_MID, -QZ_GUTTER - 100, 0);
    lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(wifi_icon, 10);
    lv_obj_add_event_cb(wifi_icon, wifi_clicked, LV_EVENT_CLICKED, NULL);

    char address[32];
    qz_device_ip(address, sizeof(address));
    if (qz_compact()) {
        address_label = NULL;
    } else {
        address_label = qz_text(status, address, 14, qz_color(QZ_TEXT_TERTIARY));
        lv_obj_align(address_label, LV_ALIGN_RIGHT_MID, -176, 0);
    }

    hero_card(desktop_screen);
    lv_obj_t *next = next_card(desktop_screen);

    lv_obj_t *weather = qz_weather_card_create(desktop_screen, QZ_GUTTER, HOME_ROW_Y,
                                               HOME_WEATHER_W, HOME_ROW_H);
    qz_weather_page_init(desktop_screen);

    lv_obj_t *apps = home_apps_tile(desktop_screen,
                                    QZ_GUTTER + HOME_WEATHER_W + HOME_TILE_GAP,
                                    HOME_ROW_Y, HOME_TILE_W, HOME_ROW_H);
    lv_obj_add_event_cb(apps, show_apps, LV_EVENT_CLICKED, NULL);

    qzdesk_core_subscribe(core_status, NULL);
    update_status();
    update_next_card();
    update_hero_face();
    lv_timer_create(status_timer, 1000, NULL);
    return desktop_screen;
}

void qz_desktop_set_assistant(lv_obj_t *assistant) { assistant_screen = assistant; }
void qz_desktop_set_apps(lv_obj_t *apps) { apps_screen = apps; }
void qz_desktop_set_settings(lv_obj_t *settings) { settings_screen = settings; }
