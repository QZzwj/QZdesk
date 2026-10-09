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

#define CARD_W (QZ_DESIGN_W - 2 * QZ_GUTTER) /* 452 */
#define HERO_Y 60
#define HERO_H 140
#define GRID_Y 208
#define GRID_H 104
#define INSET 12
#define ICON_SIZE 72
#define SPINNER_SIZE 30
#define TEXT_X 96
#define TEXT_W (CARD_W - TEXT_X - INSET)
#define TILE_COLS 5
#define TILE_ROWS 2
#define TILE_PAD_X 10
#define TILE_PAD_Y 8
#define TILE_W ((CARD_W - 2 * TILE_PAD_X) / TILE_COLS)
#define TILE_H ((GRID_H - 2 * TILE_PAD_Y) / TILE_ROWS)
#define TILES (TILE_COLS * TILE_ROWS)

static lv_obj_t *screen;
static lv_obj_t *back_ref;
static lv_obj_t *icon_box;
static lv_obj_t *spinner;
static lv_obj_t *city_label;
static lv_obj_t *temp_label;
static lv_obj_t *desc_label;
static lv_obj_t *updated_label;
static lv_obj_t *refresh_button;
static lv_obj_t *tile_caption[TILES];
static lv_obj_t *tile_value[TILES];

static const char *const TILE_CAPTION_TEXT[TILES] = {
    "湿度", "风速", "体感", "最高", "最低",
    "降水", "紫外线", "日出", "日落", "更新",
};

/* 两个处理器在工具栏里接线，定义在后面 */
static void on_back(lv_event_t *event);
static void on_refresh(lv_event_t *event);

/* ------------------------------------------------------------------------- *
 * 构建
 * ------------------------------------------------------------------------- */

static void make_plain(lv_obj_t *obj)
{
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

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

static void build_toolbar(void)
{
    lv_obj_t *toolbar = lv_obj_create(screen);
    lv_obj_set_size(toolbar, QZ_DESIGN_W - 16, 46);
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 6);
    qz_style_toolbar(toolbar);

    lv_obj_t *back = qz_back_button(toolbar);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_event_cb(back, on_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = qz_text(toolbar, "天气", 18, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    refresh_button = qz_icon_button(toolbar, LV_SYMBOL_REFRESH, 40);
    lv_obj_align(refresh_button, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_add_event_cb(refresh_button, on_refresh, LV_EVENT_CLICKED, NULL);
}

static void build_hero(void)
{
    lv_obj_t *hero = make_card(screen, QZ_GUTTER, HERO_Y, CARD_W, HERO_H);

    icon_box = qz_weather_icon_create(hero, ICON_SIZE);
    lv_obj_align(icon_box, LV_ALIGN_LEFT_MID, INSET, 0);

    spinner = lv_spinner_create(hero);
    lv_obj_set_size(spinner, SPINNER_SIZE, SPINNER_SIZE);
    lv_obj_align(spinner, LV_ALIGN_LEFT_MID, INSET + (ICON_SIZE - SPINNER_SIZE) / 2, 0);
    /* 只留转动的那一段弧，底圈不画 */
    lv_obj_set_style_arc_opa(spinner, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(spinner, qz_color(QZ_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(spinner, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(spinner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);

    city_label = qz_text(hero, "未设置位置", 16, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(city_label, LV_ALIGN_TOP_LEFT, TEXT_X, 8);
    lv_obj_set_width(city_label, TEXT_W);
    lv_label_set_long_mode(city_label, LV_LABEL_LONG_WRAP);

    /* 温度与描述同排：温度大、描述小，字宽变化时自动贴在一起 */
    lv_obj_t *temp_row = lv_obj_create(hero);
    lv_obj_set_size(temp_row, TEXT_W, LV_SIZE_CONTENT);
    lv_obj_align(temp_row, LV_ALIGN_TOP_LEFT, TEXT_X, 52);
    make_plain(temp_row);
    lv_obj_set_flex_flow(temp_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(temp_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(temp_row, 7, 0);

    temp_label = qz_text(temp_row, "--°", 36, qz_color(QZ_TEXT));
    desc_label = qz_text(temp_row, "", 17, qz_color(QZ_TEXT_SECONDARY));

    lv_obj_set_flex_grow(desc_label, 1);
    lv_label_set_long_mode(desc_label, LV_LABEL_LONG_WRAP);

    updated_label = qz_text(hero, "", 16, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_align(updated_label, LV_ALIGN_TOP_LEFT, TEXT_X, 114);
    lv_obj_set_width(updated_label, TEXT_W);
    lv_label_set_long_mode(updated_label, LV_LABEL_LONG_WRAP);

}

static void build_grid(void)
{
    lv_obj_t *grid = make_card(screen, QZ_GUTTER, GRID_Y, CARD_W, GRID_H);
    int index;

    for (index = 0; index < TILES; index++) {
        int column = index % TILE_COLS;
        int row = index / TILE_COLS;
        /* 每列占卡片三分之一，文字在列内水平居中：三列才不会一左一右不齐 */
        int center = TILE_PAD_X + column * TILE_W + TILE_W / 2 - CARD_W / 2;
        int y = TILE_PAD_Y + row * TILE_H;

        tile_caption[index] = qz_text(grid, TILE_CAPTION_TEXT[index], 16,
                                      qz_color(QZ_TEXT_TERTIARY));
        lv_obj_align(tile_caption[index], LV_ALIGN_TOP_MID, center, y);

        tile_value[index] = qz_text(grid, "—", 17, qz_color(QZ_TEXT));
        lv_obj_align(tile_value[index], LV_ALIGN_TOP_MID, center, y + 22);
    }
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
            snprintf(text, sizeof(text), "%d°", weather->high);
            break;
        case 4:
            snprintf(text, sizeof(text), "%d°", weather->low);
            break;
        case 5:
            if (weather->precipitation >= 0) snprintf(text, sizeof(text), "%d%%", weather->precipitation);
            else snprintf(text, sizeof(text), "—");
            break;
        case 6:
            if (weather->uv_x10 >= 0) snprintf(text, sizeof(text), "%d.%d", weather->uv_x10 / 10,
                                              weather->uv_x10 % 10);
            else snprintf(text, sizeof(text), "—");
            break;
        case 7:
            snprintf(text, sizeof(text), "%s", weather->sunrise[0] ? weather->sunrise : "—");
            break;
        case 8:
            snprintf(text, sizeof(text), "%s", weather->sunset[0] ? weather->sunset : "—");
            break;
        default:
            if (weather->updated[0] && strcmp(weather->updated, "--:--") != 0) {
                snprintf(text, sizeof(text), "%s", weather->updated);
            } else {
                snprintf(text, sizeof(text), "—");
            }
            break;
        }
        lv_label_set_text(tile_value[index], text);
    }
}

static void clear_tiles(void)
{
    int index;
    for (index = 0; index < TILES; index++) lv_label_set_text(tile_value[index], "—");
}

static void apply(const qzdesk_core_weather_t *weather)
{
    char text[64];
    bool loading;
    bool stale;

    if (!screen || !weather) return;
    loading = strcmp(weather->status, "loading") == 0;
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
        }
    }
    if (refresh_button) {
        lv_obj_set_style_opa(refresh_button, loading ? LV_OPA_40 : LV_OPA_COVER, 0);
    }

    if (!weather->has_data) {
        /* 没有数据：把原因写在最显眼的位置，并给一句能做什么 */
        const char *message = weather->message[0] ? weather->message : "天气暂不可用";
        lv_label_set_text(city_label, weather->city[0] ? weather->city : "未设置位置");
        lv_label_set_text(temp_label, "");
        lv_obj_add_flag(temp_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_font(desc_label, qz_font_size(16), 0);
        qz_obj_set_text_color(desc_label, QZ_ORANGE, 0);
        lv_label_set_text(desc_label, message);
        qz_obj_set_text_color(updated_label, QZ_TEXT_TERTIARY, 0);
        lv_label_set_text(updated_label, loading ? "正在刷新…" : "点右上角刷新重试");
        clear_tiles();
        return;
    }

    lv_obj_clear_flag(temp_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(desc_label, qz_font_size(17), 0);
    qz_obj_set_text_color(desc_label, QZ_TEXT_SECONDARY, 0);
    lv_label_set_text(city_label, weather->city);

    snprintf(text, sizeof(text), "%d°", weather->temperature);
    lv_label_set_text(temp_label, text);
    lv_label_set_text(desc_label, weather->text[0] ? weather->text : "—");

    if (strcmp(weather->status, "offline") == 0) {
        /* 未联网：核心已停止请求（不会再重试），这里如实说明 */
        snprintf(text, sizeof(text), "未联网 · %s", weather->updated);
    } else if (weather->updated[0] != '\0' && strcmp(weather->updated, "--:--") != 0) {
        snprintf(text, sizeof(text), stale ? "上次数据 · %s" : "更新于 %s", weather->updated);
    } else {
        snprintf(text, sizeof(text), "%s", stale ? "上次数据" : "");
    }
    lv_label_set_text(updated_label, text);
    /* 沿用上一次的数据时时间行转橙：不吵，但看得出这不是刚取的 */
    qz_obj_set_text_color(updated_label, (stale || strcmp(weather->status, "offline") == 0)
                                             ? QZ_ORANGE
                                             : QZ_TEXT_TERTIARY, 0);

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
    /* 只发一个 UDP 包给本机核心；HTTP 请求在核心自己的任务里，UI 不等网络 */
    if (!qzdesk_core_refresh_weather()) {
        if (updated_label) lv_label_set_text(updated_label, "核心未连接");
        return;
    }
    if (icon_box) lv_obj_add_flag(icon_box, LV_OBJ_FLAG_HIDDEN);
    if (spinner) lv_obj_clear_flag(spinner, LV_OBJ_FLAG_HIDDEN);
    if (updated_label) lv_label_set_text(updated_label, "正在刷新…");
}

static void core_event(const qzdesk_core_event_t *event, void *user_data)
{
    (void)user_data;
    if (event->type == QZDESK_CORE_EVENT_WEATHER) apply(&event->weather);
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

    build_toolbar();
    build_hero();
    build_grid();

    /* 首页卡片拿着一个订阅名额，这里再加一个：各自都能收到天气推送 */
    qzdesk_core_subscribe(core_event, NULL);
    /* 页面一建好就要一次现成快照（含缓存，不触发网络），首屏不留空 */
    qzdesk_core_request_weather();
}

void qz_weather_page_open(void)
{
    if (!screen) return;
    qz_screen_load(screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, QZ_DUR_SCREEN);
    /* 再要一次：首页那次可能是几分钟前，快照是最新的本地数据 */
    qzdesk_core_request_weather();
}
