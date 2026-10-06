#include "desktop.h"
#include "ai_face.h"
#include "apps.h"
#include "assistant.h"
#include "config.h"
#include "settings.h"
#include "theme.h"
#include "weather_card.h"
#include "weather_page.h"
#include "icon_assets.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Home composition: one tall AI card on the left, weather/settings/apps stacked
 * on the right. Both columns run all the way to the bottom gutter — the device
 * address moved up into the status bar and the page indicator is gone, so the
 * cards take over that space. The weather card leads the right column (top of
 * the home screen, clear of the status bar) and the two entry cards fill the
 * rest.
 *
 * 高度都从设计稿推出来，不写死：两列必须正好收尾在 QZ_BOTTOM 这条线上。 */
#define HERO_Y 36
#define HERO_W 260
/* 底部只留一个 gutter 的留白，卡片一直长到底。 */
#define QZ_BOTTOM QZ_GUTTER
#define HERO_H (QZ_DESIGN_H - HERO_Y - QZ_BOTTOM)
#define SIDE_W 176
/* 天气卡高度保持固定：它的文字列是顶部对齐的（见 weather_card.c），变高只会多出
 * 一块空白、还把左侧图标挤到中间。剩余空间均分给两张入口卡。 */
#define WEATHER_H 86
#define SIDE_GAP 10
#define SIDE_H ((QZ_DESIGN_H - QZ_BOTTOM - HERO_Y - WEATHER_H - 2 * SIDE_GAP) / 2)
#define SIDE_X (QZ_DESIGN_W - QZ_GUTTER - SIDE_W)
#define SIDE_Y2 (HERO_Y + WEATHER_H + SIDE_GAP)
#define SIDE_Y3 (SIDE_Y2 + SIDE_H + SIDE_GAP)

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

static void show_settings(lv_event_t *event)
{
    (void)event;
    qz_screen_load(settings_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, QZ_DUR_SCREEN);
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
        lv_image_set_src(battery_icon, qz_icon(battery_icon_for(level), 16));
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
        lv_obj_set_style_bg_opa(hero_badge, connected ? LV_OPA_COVER : (lv_opa_t)190, 0);
        lv_label_set_text(hero_badge_text, connected ? "在线" : "离线");
        qz_obj_set_text_color(hero_badge_text, QZ_ACCENT_DARK, 0);
        if (hero_badge_dot) {
            lv_obj_set_style_bg_color(hero_badge_dot,
                                      qz_color(connected ? QZ_GREEN : QZ_ORANGE), 0);
        }
    }
}

/* ------------------------------------------------------------------------- *
 * Cards
 * ------------------------------------------------------------------------- */

static lv_obj_t *home_card(lv_obj_t *parent, int x, int y, int width, int height,
                           const char *symbol, const char *title, const char *detail,
                           uint32_t tile, uint32_t mark)
{
    lv_obj_t *card = qz_card_button(parent, width, height);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, x, y);
    /* 图标在左、文字在右：天气卡片占了右列顶部之后这两张卡变矮，
     * 横排比竖排更合适，也仍然是一眼看懂的一张卡。 */
    lv_obj_t *icon = qz_squircle(card, 32, tile);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_t *glyph = qz_symbol(icon, symbol, 16, qz_color(mark));
    lv_obj_center(glyph);

    lv_obj_t *name = qz_text(card, title, 14, qz_color(QZ_TEXT));
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 52, -9);

    lv_obj_t *caption = qz_text(card, detail, 10, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(caption, LV_ALIGN_LEFT_MID, 52, 9);
    lv_obj_set_width(caption, width - 52 - 10);
    lv_label_set_long_mode(caption, LV_LABEL_LONG_DOT);
    return card;
}

static void hero_card(lv_obj_t *parent)
{
    lv_obj_t *card = qz_card_button(parent, HERO_W, HERO_H);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, QZ_GUTTER, HERO_Y);
    /* A white plate, not a colour field. The reference builds its hierarchy out
     * of type (grey caption over black value) and spends the accent only on what
     * can be acted on, so the hero stays a plate and the accent shows up in the
     * status pill and the chevron. */
    lv_obj_t *halo = lv_obj_create(card);
    lv_obj_set_size(halo, 96, 96);
    lv_obj_align(halo, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_radius(halo, LV_RADIUS_CIRCLE, 0);
    qz_obj_set_bg_color(halo, QZ_FILL, 0);
    lv_obj_set_style_bg_opa(halo, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(halo, 0, 0);
    lv_obj_set_style_pad_all(halo, 0, 0);
    lv_obj_clear_flag(halo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(halo, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *face = qz_face_create(card, 88);
    lv_obj_align(face, LV_ALIGN_LEFT_MID, 12, 0);
    qz_face_set_state(face, QZ_FACE_IDLE);

    lv_obj_t *title = qz_text(card, "AI 助手", 18, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 130, -44);

    lv_obj_t *hint = qz_text(card, "点击开始对话", 12, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(hint, LV_ALIGN_LEFT_MID, 130, -12);

    lv_obj_t *tag = qz_text(card, "语音 · 文字 · 表情", 10, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(tag, LV_ALIGN_LEFT_MID, 130, 16);

    /* The one accent mark on the plate, and the cue that it opens something. */
    lv_obj_t *go = qz_chevron(card, qz_color(QZ_ACCENT), 15);
    lv_obj_align(go, LV_ALIGN_RIGHT_MID, -16, 6);

    hero_badge = lv_obj_create(card);
    lv_obj_set_size(hero_badge, 54, 20);
    lv_obj_align(hero_badge, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_radius(hero_badge, LV_RADIUS_CIRCLE, 0);
    /* Tinted pill: the accent marks the state, the dot says which one it is,
     * and the caption stays dark enough to read at 11px. */
    qz_obj_set_bg_color(hero_badge, QZ_ACCENT_TINT, 0);
    lv_obj_set_style_bg_opa(hero_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hero_badge, 0, 0);
    lv_obj_set_style_pad_all(hero_badge, 0, 0);
    lv_obj_clear_flag(hero_badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(hero_badge, LV_OBJ_FLAG_SCROLLABLE);

    hero_badge_dot = lv_obj_create(hero_badge);
    lv_obj_set_size(hero_badge_dot, 5, 5);
    lv_obj_align(hero_badge_dot, LV_ALIGN_LEFT_MID, 9, 0);
    lv_obj_set_style_radius(hero_badge_dot, LV_RADIUS_CIRCLE, 0);
    qz_obj_set_bg_color(hero_badge_dot, QZ_GREEN, 0);
    lv_obj_set_style_border_width(hero_badge_dot, 0, 0);
    lv_obj_clear_flag(hero_badge_dot, LV_OBJ_FLAG_CLICKABLE);
    hero_badge_text = qz_text(hero_badge, "在线", 11, qz_color(QZ_ACCENT_DARK));
    lv_obj_align(hero_badge_text, LV_ALIGN_LEFT_MID, 19, 0);

    lv_obj_add_event_cb(card, show_assistant, LV_EVENT_CLICKED, NULL);
    qz_animate_entrance(card, 0);
}

/* ------------------------------------------------------------------------- *
 * Screen
 * ------------------------------------------------------------------------- */

static void status_timer(lv_timer_t *timer)
{
    (void)timer;
    update_status();
}

lv_obj_t *qz_desktop_create(const char *server)
{
    (void)server;
    desktop_screen = lv_obj_create(NULL);
    qz_style_screen(desktop_screen);

    /* Status bar */
    lv_obj_t *status = lv_obj_create(desktop_screen);
    lv_obj_set_size(status, lv_pct(100), QZ_STATUS_H);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(status, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status, 0, 0);
    lv_obj_set_style_pad_all(status, 0, 0);
    lv_obj_clear_flag(status, LV_OBJ_FLAG_SCROLLABLE);

    clock_label = qz_text(status, "--:--", 13, qz_color(QZ_TEXT));
    lv_obj_align(clock_label, LV_ALIGN_LEFT_MID, QZ_GUTTER, 0);

    battery_icon = qz_icon_image(status, QZ_ICON_BATTERY_4, 16, qz_color(QZ_TEXT));
    lv_obj_align(battery_icon, LV_ALIGN_RIGHT_MID, -QZ_GUTTER, 0);
    battery_label = qz_text(status, "100%", 10, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(battery_label, LV_ALIGN_RIGHT_MID, -QZ_GUTTER - 24, 0);
    wifi_icon = qz_icon_image(status, QZ_ICON_WIFI, 16, qz_color(QZ_TEXT));
    lv_obj_align(wifi_icon, LV_ALIGN_RIGHT_MID, -QZ_GUTTER - 70, 0);

    /* 设备地址：原来单独占底部一行（连那条指示条一起删掉了），现在放在状态栏
     * 中间 —— 左边是时间、右边是无线与电量，中间正好空着。 */
    char address[32];
    qz_device_ip(address, sizeof(address));
    address_label = qz_text(status, address, 11, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(address_label, LV_ALIGN_CENTER, 0, 0);

    /* Hero card */
    hero_card(desktop_screen);

    /* 天气卡片：数据由核心异步取好、缓存好后推过来，这里只负责显示；
     * 点一下进天气详情页（刷新按钮在详情页里），网络请求都不在 UI 线程里。 */
    lv_obj_t *weather = qz_weather_card_create(desktop_screen, SIDE_X, HERO_Y, SIDE_W, WEATHER_H);
    qz_weather_page_init(desktop_screen);

    /* Secondary cards */
    lv_obj_t *settings = home_card(desktop_screen, SIDE_X, SIDE_Y2, SIDE_W, SIDE_H,
                                   LV_SYMBOL_SETTINGS, "设置", "WLAN · 声音 · 显示",
                                   QZ_ACCENT, QZ_TEXT_ON_ACCENT);
    lv_obj_add_event_cb(settings, show_settings, LV_EVENT_CLICKED, NULL);
    lv_obj_t *apps = home_card(desktop_screen, SIDE_X, SIDE_Y3, SIDE_W, SIDE_H,
                               LV_SYMBOL_LIST, "应用", "设备工具与技能",
                               QZ_ACCENT_TINT, QZ_ACCENT_DARK);
    lv_obj_add_event_cb(apps, show_apps, LV_EVENT_CLICKED, NULL);
    qz_animate_entrance(weather, 70);
    qz_animate_entrance(settings, 110);
    qz_animate_entrance(apps, 150);

    update_status();
    lv_timer_create(status_timer, 1000, NULL);
    return desktop_screen;
}

void qz_desktop_set_assistant(lv_obj_t *assistant) { assistant_screen = assistant; }
void qz_desktop_set_apps(lv_obj_t *apps) { apps_screen = apps; }
void qz_desktop_set_settings(lv_obj_t *settings) { settings_screen = settings; }
