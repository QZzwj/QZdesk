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
/* 主页只有三块，按"两行"排：
 *     状态栏 / 上半 AI 助手 / 下半 左天气 右应用
 * 设置不再占首页格子，改为应用页里的一个入口（pages/apps.c）。
 *
 * 尺寸按"真机 320×240 上想要的实际像素 ×4/3"写：缩放层会把设计稿缩 2/3
 * （实机 = 设计 × 2/3），所以设计 30 的标题在真机上是 20px、设计 132 的高是 88px。
 * 竖向预算：状态栏 35 / 主卡 43..175 / 下半 183..304 / 底部留白 16。 */
#define HOME_STATUS_H 35
#define HOME_HERO_Y   43
#define HOME_HERO_H   132
#define HOME_ROW_Y    183
#define HOME_ROW_H    121
#define HOME_TILE_GAP 14
#define HOME_TILE_W   ((QZ_DESIGN_W - 2 * QZ_GUTTER - HOME_TILE_GAP) / 2)

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

/** 主页主卡：上半部分整块 —— 脸 + 标题 + 一句提示 + 在线胶囊。 */
static void hero_card(lv_obj_t *parent)
{
    lv_obj_t *card = qz_card_button(parent, QZ_DESIGN_W - 2 * QZ_GUTTER, HOME_HERO_H);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, QZ_GUTTER, HOME_HERO_Y);

    /* 脸外面套一圈浅色圆盘，白卡上一块黑屏太生硬 */
    lv_obj_t *halo = lv_obj_create(card);
    lv_obj_set_size(halo, 114, 114);
    lv_obj_align(halo, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_radius(halo, LV_RADIUS_CIRCLE, 0);
    qz_obj_set_bg_color(halo, QZ_FILL, 0);
    lv_obj_set_style_bg_opa(halo, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(halo, 0, 0);
    lv_obj_set_style_pad_all(halo, 0, 0);
    lv_obj_clear_flag(halo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(halo, LV_OBJ_FLAG_SCROLLABLE);

    /* 设计 102 -> 实机 68：落在 60px 那一档资源上，1:1 播、不缩放不裁切 */
    lv_obj_t *face = qz_face_create(card, 102);
    lv_obj_align(face, LV_ALIGN_LEFT_MID, 18, 0);
    qz_face_set_state(face, QZ_FACE_IDLE);

    lv_obj_t *title = qz_text(card, "AI 助手", 30, qz_color(QZ_TEXT));       /* 实机 20 */
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 152, -22);
    lv_obj_t *hint = qz_text(card, "点击开始对话", 20, qz_color(QZ_TEXT_SECONDARY));  /* 实机 15 */
    lv_obj_align(hint, LV_ALIGN_LEFT_MID, 152, 18);

    /* 唯一的一处强调色，同时暗示"点进去" */
    lv_obj_t *go = qz_chevron(card, qz_color(QZ_ACCENT), 22);
    lv_obj_align(go, LV_ALIGN_RIGHT_MID, -18, 0);

    hero_badge = lv_obj_create(card);
    lv_obj_set_size(hero_badge, 78, 30);
    lv_obj_align(hero_badge, LV_ALIGN_TOP_RIGHT, -14, 12);
    lv_obj_set_style_radius(hero_badge, LV_RADIUS_CIRCLE, 0);
    qz_obj_set_bg_color(hero_badge, QZ_ACCENT_TINT, 0);
    lv_obj_set_style_bg_opa(hero_badge, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hero_badge, 0, 0);
    lv_obj_set_style_pad_all(hero_badge, 0, 0);
    lv_obj_clear_flag(hero_badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(hero_badge, LV_OBJ_FLAG_SCROLLABLE);

    hero_badge_dot = lv_obj_create(hero_badge);
    lv_obj_set_size(hero_badge_dot, 10, 10);
    lv_obj_align(hero_badge_dot, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_radius(hero_badge_dot, LV_RADIUS_CIRCLE, 0);
    qz_obj_set_bg_color(hero_badge_dot, QZ_GREEN, 0);
    lv_obj_set_style_border_width(hero_badge_dot, 0, 0);
    lv_obj_clear_flag(hero_badge_dot, LV_OBJ_FLAG_CLICKABLE);
    hero_badge_text = qz_text(hero_badge, "在线", 18, qz_color(QZ_ACCENT_DARK));
    lv_obj_align(hero_badge_text, LV_ALIGN_LEFT_MID, 28, 0);

    lv_obj_add_event_cb(card, show_assistant, LV_EVENT_CLICKED, NULL);
    qz_animate_entrance(card, 0);
}

/** 下半部分的方块：图标在上、标题在下（天气那一格由天气卡自己画）。 */
static lv_obj_t *home_tile(lv_obj_t *parent, int x, const char *symbol, const char *title,
                           uint32_t tile, uint32_t mark)
{
    lv_obj_t *card = qz_card_button(parent, HOME_TILE_W, HOME_ROW_H);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, x, HOME_ROW_Y);

    lv_obj_t *icon = qz_squircle(card, 60, tile);            /* 实机 40 */
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_t *glyph = qz_symbol(icon, symbol, 34, qz_color(mark));   /* 实机 23 */
    lv_obj_center(glyph);

    lv_obj_t *name = qz_text(card, title, 30, qz_color(QZ_TEXT));    /* 实机 20 */
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 82);
    return card;
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

    /* Status bar：尺寸按真机（320×240）的目标像素定，大面板上同比例放大 */
    lv_obj_t *status = lv_obj_create(desktop_screen);
    lv_obj_set_size(status, lv_pct(100), HOME_STATUS_H);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(status, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status, 0, 0);
    lv_obj_set_style_pad_all(status, 0, 0);
    lv_obj_clear_flag(status, LV_OBJ_FLAG_SCROLLABLE);

    clock_label = qz_text(status, "--:--", 20, qz_color(QZ_TEXT));
    lv_obj_align(clock_label, LV_ALIGN_LEFT_MID, QZ_GUTTER, 0);

    battery_icon = qz_icon_image(status, QZ_ICON_BATTERY_4, 26, qz_color(QZ_TEXT));
    lv_obj_align(battery_icon, LV_ALIGN_RIGHT_MID, -QZ_GUTTER, 0);
    battery_label = qz_text(status, "100%", 18, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(battery_label, LV_ALIGN_RIGHT_MID, -QZ_GUTTER - 36, 0);
    wifi_icon = qz_icon_image(status, QZ_ICON_WIFI, 26, qz_color(QZ_TEXT));
    lv_obj_align(wifi_icon, LV_ALIGN_RIGHT_MID, -QZ_GUTTER - 100, 0);

    /* 设备地址：原来单独占底部一行（连那条指示条一起删掉了），现在放在状态栏
     * 中间 —— 左边是时间、右边是无线与电量，中间正好空着。 */
    char address[32];
    qz_device_ip(address, sizeof(address));
    address_label = qz_text(status, address, 16, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(address_label, LV_ALIGN_CENTER, 0, 0);

    /* 上半：AI 助手（整宽） */
    hero_card(desktop_screen);

    /* 下半左：天气。数据由核心异步取好、缓存好后推过来，这里只负责显示；点一下
     * 进天气详情页（刷新按钮在详情页里），网络请求都不在 UI 线程里。 */
    lv_obj_t *weather = qz_weather_card_create(desktop_screen, QZ_GUTTER, HOME_ROW_Y,
                                               HOME_TILE_W, HOME_ROW_H);
    qz_weather_page_init(desktop_screen);

    /* 下半右：应用（设置已经收进应用页里）。 */
    lv_obj_t *apps = home_tile(desktop_screen, QZ_GUTTER + HOME_TILE_W + HOME_TILE_GAP,
                               LV_SYMBOL_LIST, "应用", QZ_ACCENT_TINT, QZ_ACCENT_DARK);
    lv_obj_add_event_cb(apps, show_apps, LV_EVENT_CLICKED, NULL);

    qz_animate_entrance(weather, 70);
    qz_animate_entrance(apps, 130);

    update_status();
    lv_timer_create(status_timer, 1000, NULL);
    return desktop_screen;
}

void qz_desktop_set_assistant(lv_obj_t *assistant) { assistant_screen = assistant; }
void qz_desktop_set_apps(lv_obj_t *apps) { apps_screen = apps; }
void qz_desktop_set_settings(lv_obj_t *settings) { settings_screen = settings; }
