/**
 * Weather detail page — see include/weather_page.h for the layout and the split
 * of responsibilities with the core.
 */
#include "weather_page.h"
#include "weather_card.h"
#include "qzdesk_core.h"
#include "theme.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CARD_W (QZ_DESIGN_W - 2 * QZ_GUTTER) /* 452 */
#define HERO_Y 58
#define HERO_H 128
#define GRID_Y 194
#define GRID_H 118
#define INSET 12
#define ICON_WELL 68
#define ICON_SIZE 42
#define SPINNER_SIZE 24
#define CONTENT_X 100
#define CONTENT_W 204
#define SPLIT_X 316
#define CLOCK_X 328
#define CLOCK_W 108
#define TILE_COLS 4
#define TILE_ROWS 2
#define TILE_W (CARD_W / TILE_COLS)
#define TILE_H (GRID_H / TILE_ROWS)
#define TILES (TILE_COLS * TILE_ROWS)

static lv_obj_t *screen;
static lv_obj_t *back_ref;
static lv_obj_t *hero_card;
static lv_obj_t *metric_panel;
static lv_obj_t *icon_box;
static lv_obj_t *spinner;
static lv_obj_t *city_label;
static lv_obj_t *temp_label;
static lv_obj_t *desc_label;
static lv_obj_t *updated_label;
static lv_obj_t *refresh_button;
static lv_obj_t *station_time;
static lv_obj_t *station_date;
static bool refreshing;
static uint32_t refresh_started;
/* 页面开出来到第一份快照之间是空屏：那段时间必须有解释，不能只留一个 --° */
static bool got_snapshot;
static bool no_data_warned;
static uint32_t page_opened_at;
static char last_icon[24];

static void clear_tiles(void);
static void show_no_data(const char *message, const char *hint, bool warn);

static void update_station_clock(void)
{
    if (!station_time || !station_date) return;
    time_t now = time(NULL);
    struct tm value;
    if (!localtime_r(&now, &value)) return;
    char text[64];
    strftime(text, sizeof(text), "%H:%M", &value);
    lv_label_set_text(station_time, text);
    static const char *days[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    snprintf(text, sizeof(text), "%02d.%02d  %s", value.tm_mon + 1,
             value.tm_mday, days[value.tm_wday]);
    lv_label_set_text(station_date, text);
}

static void refresh_watch(lv_timer_t *timer)
{
    (void)timer;
    update_station_clock();
    if (!got_snapshot && !no_data_warned && lv_tick_elaps(page_opened_at) >= 4000) {
        no_data_warned = true;
        /* 核心在跑只是慢，和核心根本没起来，是两件事，说清楚是哪一件 */
        if (qzdesk_core_open()) show_no_data("正在等待天气数据…", "也可以点右上角刷新", false);
        else show_no_data("本地服务未连接", "点右上角刷新重试", true);
        return;
    }
    if (!refreshing || lv_tick_elaps(refresh_started) < 20000) return;
    refreshing = false;
    if (spinner) lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
    if (icon_box) lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_HIDDEN);
    if (refresh_button) {
        lv_obj_remove_state(refresh_button, LV_STATE_DISABLED);
        lv_obj_set_style_opa(refresh_button, LV_OPA_COVER, 0);
    }
    if (updated_label) lv_label_set_text(updated_label, "刷新超时，请重试");
}
static lv_obj_t *tile_caption[TILES];
static lv_obj_t *tile_value[TILES];

static const char *const TILE_CAPTION_TEXT[TILES] = {
    "湿度", "风速", "体感", "降水",
    "紫外线", "高 / 低", "日出", "日落",
};

static const qz_icon_t TILE_ICON[TILES] = {
    QZ_ICON_DROPLET, QZ_ICON_WIND, QZ_ICON_THERMOMETER, QZ_ICON_CLOUD_RAIN,
    QZ_ICON_SUN, QZ_ICON_THERMOMETER, QZ_ICON_SUN, QZ_ICON_SUN,
};

static const qz_color_token_t TILE_TINT[TILES] = {
    QZ_ACCENT_DARK, QZ_ACCENT_DARK, QZ_ORANGE, QZ_ACCENT,
    QZ_ORANGE, QZ_TEXT_SECONDARY, QZ_ORANGE, QZ_TEXT_SECONDARY,
};

/* 两个处理器在工具栏里接线，定义在后面 */
static void on_back(lv_event_t *event);
static void on_refresh(lv_event_t *event);

/* ------------------------------------------------------------------------- *
 * 构建
 * ------------------------------------------------------------------------- */

static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, width, height);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_radius(card, QZ_RADIUS_CARD, 0);
    qz_style_plate(card);
    /* 卡片内部一律用 INSET 自己留边：坐标从卡片左上角起算，不受主题内边距影响 */
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static void make_plain(lv_obj_t *obj)
{
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void show_placeholder_icon(void)
{
    if (!icon_box) return;
    lv_obj_clean(icon_box);
    lv_obj_t *cloud = qz_icon_image(icon_box, QZ_ICON_CLOUD, 34,
                                    qz_color(QZ_TEXT_TERTIARY));
    if (cloud) lv_obj_center(cloud);
}

static void build_header(void)
{
    lv_obj_t *header = qz_page_header(screen, "桌面气象站");
    lv_obj_t *back = qz_back_button(header);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);

    refresh_button = qz_icon_button(header, LV_SYMBOL_REFRESH, 40);
    lv_obj_align(refresh_button, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_add_event_cb(refresh_button, on_refresh, LV_EVENT_CLICKED, NULL);
}

static void build_hero(void)
{
    lv_obj_t *hero = make_card(screen, QZ_GUTTER, HERO_Y, CARD_W, HERO_H);
    hero_card = hero;

    lv_obj_t *rail = lv_obj_create(hero);
    lv_obj_set_size(rail, 4, HERO_H - 20);
    lv_obj_align(rail, LV_ALIGN_LEFT_MID, 0, 0);
    qz_obj_set_bg_color(rail, QZ_ACCENT, 0);
    lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rail, 0, 0);
    lv_obj_set_style_radius(rail, 0, 0);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon_well = qz_squircle(hero, ICON_WELL, QZ_ACCENT_TINT);
    lv_obj_align(icon_well, LV_ALIGN_TOP_LEFT, 16, 34);

    icon_box = qz_weather_icon_create(icon_well, ICON_SIZE);
    lv_obj_center(icon_box);

    spinner = lv_spinner_create(icon_well);
    lv_obj_set_size(spinner, SPINNER_SIZE, SPINNER_SIZE);
    lv_obj_center(spinner);
    /* 只留转动的那一段弧，底圈不画 */
    lv_obj_set_style_arc_opa(spinner, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 3, LV_PART_INDICATOR);
    qz_obj_set_arc_color(spinner, QZ_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(spinner, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(spinner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);

    city_label = qz_text(hero, "未设置位置", 16, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(city_label, LV_ALIGN_TOP_LEFT, CONTENT_X, 11);
    lv_obj_set_width(city_label, CONTENT_W);
    lv_obj_set_height(city_label, 20);
    lv_label_set_long_mode(city_label, LV_LABEL_LONG_DOT);

    temp_label = qz_text(hero, "--°", 50, qz_color(QZ_ACCENT));
    lv_obj_align(temp_label, LV_ALIGN_TOP_LEFT, CONTENT_X, 32);
    lv_obj_set_width(temp_label, 138);
    lv_obj_set_height(temp_label, 70);
    lv_label_set_long_mode(temp_label, LV_LABEL_LONG_CLIP);

    desc_label = qz_text(hero, "", 15, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(desc_label, LV_ALIGN_TOP_LEFT, CONTENT_X, 104);
    lv_obj_set_width(desc_label, CONTENT_W);
    lv_obj_set_height(desc_label, 17);
    lv_label_set_long_mode(desc_label, LV_LABEL_LONG_DOT);

    lv_obj_t *split = qz_separator(hero, HERO_H - 28, true);
    lv_obj_align(split, LV_ALIGN_TOP_LEFT, SPLIT_X, 14);

    lv_obj_t *time_caption = qz_text(hero, "站内时间", 11, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(time_caption, LV_ALIGN_TOP_LEFT, CLOCK_X, 13);
    station_time = qz_text(hero, "00:00", 27, qz_color(QZ_TEXT));
    lv_obj_align(station_time, LV_ALIGN_TOP_LEFT, CLOCK_X, 33);
    lv_obj_set_width(station_time, CLOCK_W);
    lv_obj_set_height(station_time, 36);
    station_date = qz_text(hero, "", 11, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_set_width(station_date, CLOCK_W);
    lv_label_set_long_mode(station_date, LV_LABEL_LONG_DOT);
    lv_obj_align(station_date, LV_ALIGN_TOP_LEFT, CLOCK_X, 70);

    updated_label = qz_text(hero, "", 11, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(updated_label, LV_ALIGN_TOP_LEFT, CLOCK_X, 92);
    lv_obj_set_width(updated_label, CLOCK_W);
    lv_obj_set_height(updated_label, 22);
    lv_label_set_long_mode(updated_label, LV_LABEL_LONG_DOT);

    update_station_clock();
}

static void build_grid(void)
{
    lv_obj_t *grid = make_card(screen, QZ_GUTTER, GRID_Y, CARD_W, GRID_H);
    metric_panel = grid;

    for (int index = 0; index < TILES; index++) {
        int column = index % TILE_COLS;
        int row = index / TILE_COLS;
        lv_obj_t *band = lv_obj_get_child(grid, row == 0 ? 0 : 1);
        if (!band) {
            band = lv_obj_create(grid);
            lv_obj_set_size(band, lv_pct(100), lv_pct(50));
            lv_obj_align(band, LV_ALIGN_TOP_LEFT, 0, lv_pct(row * 50));
            make_plain(band);
            if (row == 0) {
                qz_obj_set_bg_color(band, QZ_FILL, 0);
                lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
            }
        }

        lv_obj_t *cell = lv_obj_create(band);
        lv_obj_set_size(cell, lv_pct(100 / TILE_COLS), lv_pct(100));
        lv_obj_align(cell, LV_ALIGN_TOP_LEFT, lv_pct(column * 100 / TILE_COLS), 0);
        make_plain(cell);

        lv_obj_t *glyph = qz_icon_image(cell, TILE_ICON[index], 15,
                                        qz_color(TILE_TINT[index]));
        if (glyph) lv_obj_align(glyph, LV_ALIGN_TOP_RIGHT, -12, 11);

        tile_caption[index] = qz_text(cell, TILE_CAPTION_TEXT[index], 11,
                                      qz_color(QZ_TEXT_TERTIARY));
        lv_obj_align(tile_caption[index], LV_ALIGN_TOP_LEFT, 12, 10);
        lv_obj_set_width(tile_caption[index], TILE_W - 44);
        lv_obj_set_height(tile_caption[index], 16);
        lv_label_set_long_mode(tile_caption[index], LV_LABEL_LONG_DOT);

        tile_value[index] = qz_text(cell, "—", row == 0 ? 19 : 17,
                                    qz_color(QZ_TEXT));
        lv_obj_align(tile_value[index], LV_ALIGN_TOP_LEFT, 12, 28);
        lv_obj_set_width(tile_value[index], TILE_W - 24);
        lv_obj_set_height(tile_value[index], 23);
        lv_label_set_long_mode(tile_value[index], LV_LABEL_LONG_DOT);

        if (column > 0) {
            lv_obj_t *rule = qz_separator(cell, TILE_H - 12, true);
            lv_obj_align(rule, LV_ALIGN_LEFT_MID, 0, 0);
            lv_obj_set_height(rule, lv_pct(88));
        }
    }

    lv_obj_t *mid_rule = qz_separator(grid, CARD_W, false);
    lv_obj_align(mid_rule, LV_ALIGN_TOP_LEFT, 0, TILE_H);
    lv_obj_set_width(mid_rule, lv_pct(100));
}

/* ------------------------------------------------------------------------- *
 * 渲染
 * ------------------------------------------------------------------------- */

static void set_tiles(const qzdesk_core_weather_t *weather)
{
    char text[48];
    int index;

    for (index = 0; index < TILES; index++) {
        switch (index) {
        case 0:
            snprintf(text, sizeof(text), "%d%%", weather->humidity);
            break;
        case 1:
            if (weather->wind_x10 != 0) {
                snprintf(text, sizeof(text), "%d.%dm/s", weather->wind_x10 / 10,
                         weather->wind_x10 % 10);
            } else {
                snprintf(text, sizeof(text), "—");
            }
            break;
        case 2:
            snprintf(text, sizeof(text), "%d°", weather->apparent);
            break;
        case 3:
            if (weather->precipitation >= 0) snprintf(text, sizeof(text), "%d%%", weather->precipitation);
            else snprintf(text, sizeof(text), "—");
            break;
        case 4:
            if (weather->uv_x10 >= 0) snprintf(text, sizeof(text), "%d.%d", weather->uv_x10 / 10,
                                              weather->uv_x10 % 10);
            else snprintf(text, sizeof(text), "—");
            break;
        case 5:
            snprintf(text, sizeof(text), "%d°/%d°", weather->high, weather->low);
            break;
        case 6:
            snprintf(text, sizeof(text), "%s", weather->sunrise[0] ? weather->sunrise : "—");
            break;
        default:
            snprintf(text, sizeof(text), "%s", weather->sunset[0] ? weather->sunset : "—");
            break;
        }
        lv_label_set_text(tile_value[index], text);
    }
}

static void clear_tiles(void)
{
    int index;
    for (index = 0; index < TILES; index++) {
        if (!tile_value[index]) continue;
        lv_label_set_text(tile_value[index], "—");
    }
}

static void show_condition_summary(const qzdesk_core_weather_t *weather)
{
    char text[64];
    int minutes = weather->near_precipitation_minutes;
    bool stale = strcmp(weather->status, "stale") == 0 ||
                 strcmp(weather->status, "offline") == 0;

    if (minutes < 0) {
        lv_label_set_text(desc_label, weather->text[0] ? weather->text : "—");
        qz_obj_set_text_color(desc_label,
                              stale ? QZ_ORANGE : QZ_TEXT_SECONDARY, 0);
        return;
    }

    if (minutes == 0) snprintf(text, sizeof(text), "正在下雨");
    else if (minutes < 60) snprintf(text, sizeof(text), "%d分钟后有雨", minutes);
    else snprintf(text, sizeof(text), "%d小时后有雨", minutes / 60);
    if (weather->near_precipitation_probability >= 0) {
        size_t used = strlen(text);
        snprintf(text + used, sizeof(text) - used, " · %d%%",
                 weather->near_precipitation_probability);
    }
    lv_label_set_text(desc_label, text);
    qz_obj_set_text_color(desc_label,
                          stale ? QZ_ORANGE : QZ_ACCENT_TEXT, 0);
}

/* 没有数据可画时的统一样子：温度让位给一句话，格子清回占位符。warn 为真表示
 * 这不是"还没到"而是"没等到"，字色转橙，语气跟着变。 */
static void show_no_data(const char *message, const char *hint, bool warn)
{
    if (!desc_label) return;
    if (!got_snapshot) lv_label_set_text(city_label, "未设置位置");
    lv_obj_add_flag(temp_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(desc_label, qz_font_size(17), 0);
    lv_label_set_long_mode(desc_label, LV_LABEL_LONG_DOT);
    qz_obj_set_text_color(desc_label, warn ? QZ_ORANGE : QZ_TEXT_SECONDARY, 0);
    lv_label_set_text(desc_label, message);
    lv_obj_set_width(desc_label, CONTENT_W);
    lv_obj_set_height(desc_label, 22);
    lv_obj_align(desc_label, LV_ALIGN_TOP_LEFT, CONTENT_X, 54);
    qz_obj_set_text_color(updated_label, QZ_TEXT_TERTIARY, 0);
    lv_label_set_text(updated_label, hint);
    lv_obj_set_width(updated_label, CLOCK_W);
    lv_obj_set_height(updated_label, 22);
    lv_obj_align(updated_label, LV_ALIGN_TOP_LEFT, CLOCK_X, 92);
    clear_tiles();
}

static void apply(const qzdesk_core_weather_t *weather)
{
    char text[64];
    bool loading;
    bool stale;

    if (!screen || !weather) return;
    got_snapshot = true;
    loading = strcmp(weather->status, "loading") == 0;
    if (loading && !refreshing) refresh_started = lv_tick_get();
    refreshing = loading;
    stale = strcmp(weather->status, "stale") == 0;

    /* 刷新期间保留旧数据，只在图标位置转圈 */
    if (loading) {
        if (icon_box) lv_obj_add_flag(icon_box, LV_OBJ_FLAG_HIDDEN);
        if (spinner) lv_obj_clear_flag(spinner, LV_OBJ_FLAG_HIDDEN);
    } else {
        if (spinner) lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
        if (icon_box) {
            lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_HIDDEN);
            qz_weather_icon_render(icon_box, weather->icon);
            if (last_icon[0] && strcmp(last_icon, weather->icon) != 0)
                qz_animate_pop_in(icon_box);
            snprintf(last_icon, sizeof(last_icon), "%s", weather->icon);
        }
    }
    if (refresh_button) {
        if (loading) lv_obj_add_state(refresh_button, LV_STATE_DISABLED);
        else lv_obj_remove_state(refresh_button, LV_STATE_DISABLED);
        lv_obj_set_style_opa(refresh_button, loading ? LV_OPA_40 : LV_OPA_COVER, 0);
    }

    if (!weather->has_data) {
        /* 没有数据：把原因写在最显眼的位置，并给一句能做什么 */
        lv_label_set_text(city_label, weather->city[0] ? weather->city : "未设置位置");
        show_no_data(weather->message[0] ? weather->message : "天气暂不可用",
                     loading ? "正在刷新…" : "点右上角刷新重试", true);
        return;
    }

    lv_obj_clear_flag(temp_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(desc_label, qz_font_size(15), 0);
    lv_label_set_long_mode(desc_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(desc_label, CONTENT_W);
    lv_obj_set_height(desc_label, 17);
    lv_obj_align(desc_label, LV_ALIGN_TOP_LEFT, CONTENT_X, 104);
    lv_obj_set_width(updated_label, CLOCK_W);
    lv_obj_set_height(updated_label, 17);
    lv_obj_align(updated_label, LV_ALIGN_TOP_LEFT, CLOCK_X, 91);
    qz_obj_set_text_color(desc_label, QZ_TEXT_SECONDARY, 0);
    lv_label_set_text(city_label, weather->city);

    snprintf(text, sizeof(text), "%d°", weather->temperature);
    lv_label_set_text(temp_label, text);
    show_condition_summary(weather);

    if (strcmp(weather->status, "offline") == 0) {
        /* 未联网：核心已停止请求（不会再重试），这里如实说明 */
        snprintf(text, sizeof(text), "离线 %s", weather->updated);
    } else if (weather->updated[0] != '\0' && strcmp(weather->updated, "--:--") != 0) {
        snprintf(text, sizeof(text), stale ? "缓存 %s" : "更新 %s", weather->updated);
    } else {
        snprintf(text, sizeof(text), "%s", stale ? "缓存数据" : "");
    }
    lv_label_set_text(updated_label, text);
    /* 沿用上一次的数据时时间行转橙：不吵，但看得出这不是刚取的 */
    qz_obj_set_text_color(updated_label,
                          (stale || strcmp(weather->status, "offline") == 0)
                              ? QZ_ORANGE
                              : QZ_TEXT_TERTIARY,
                          0);

    set_tiles(weather);
}

/* ------------------------------------------------------------------------- *
 * 事件
 * ------------------------------------------------------------------------- */

static void on_back(lv_event_t *event)
{
    (void)event;
    qz_screen_load(back_ref, LV_SCR_LOAD_ANIM_MOVE_RIGHT, QZ_DUR_SCREEN);
}

static void on_refresh(lv_event_t *event)
{
    (void)event;
    if (refreshing) return;
    /* 只发一个 UDP 包给本机核心；HTTP 请求在核心自己的任务里，UI 不等网络 */
    if (!qzdesk_core_refresh_weather()) {
        if (updated_label) lv_label_set_text(updated_label, "核心未连接");
        return;
    }
    if (icon_box) lv_obj_add_flag(icon_box, LV_OBJ_FLAG_HIDDEN);
    if (spinner) lv_obj_clear_flag(spinner, LV_OBJ_FLAG_HIDDEN);
    refreshing = true;
    refresh_started = lv_tick_get();
    if (refresh_button) lv_obj_add_state(refresh_button, LV_STATE_DISABLED);
    if (updated_label) lv_label_set_text(updated_label, "正在刷新…");
}

static void core_event(const qzdesk_core_event_t *event, void *user_data)
{
    (void)user_data;
    if (event->type == QZDESK_CORE_EVENT_WEATHER) apply(&event->weather);
}

static void weather_screen_loaded(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_SCREEN_LOADED || qz_reduce_motion()) return;
    qz_animate_entrance(hero_card, 0);
    qz_animate_entrance(metric_panel, 42);
}

/* ------------------------------------------------------------------------- *
 * 对外
 * ------------------------------------------------------------------------- */

void qz_weather_page_init(lv_obj_t *back_screen)
{
    if (screen) return;
    back_ref = back_screen ? back_screen : lv_screen_active();

    screen = lv_obj_create(NULL);
    qz_style_screen(screen);

    build_header();
    build_hero();
    build_grid();
    lv_obj_add_event_cb(screen, weather_screen_loaded, LV_EVENT_SCREEN_LOADED, NULL);
    /* 网格建完才谈得上清空：首屏的占位态放在三块都齐了之后再摆 */
    show_no_data("正在读取天气…", "", false);
    show_placeholder_icon();
    page_opened_at = lv_tick_get();
    lv_timer_create(refresh_watch, 1000, NULL);

    /* 首页卡片拿着一个订阅名额，这里再加一个：各自都能收到天气推送 */
    qzdesk_core_subscribe(core_event, NULL);
    /* 页面一建好就要一次现成快照（含缓存，不触发网络），首屏不留空 */
    qzdesk_core_request_weather();
}

void qz_weather_page_open(void)
{
    if (!screen) return;
    back_ref = lv_screen_active();
    qz_screen_load(screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, QZ_DUR_SCREEN);
    /* 再要一次：首页那次可能是几分钟前，快照是最新的本地数据 */
    qzdesk_core_request_weather();
}
