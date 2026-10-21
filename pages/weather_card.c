/**
 * Home-screen weather card.
 *
 * The split of responsibilities is deliberate: the Rust core fetches (Open-Meteo),
 * caches, times out and retries; this file only draws. The event callback does a
 * single `sendto` — the HTTP request happens in the core's own task, so tapping
 * the card can never stall LVGL, and a slow or dead weather API cannot affect
 * the AI conversation, Wi-Fi or skills.
 *
 * LVGL ships no weather glyphs and the project bundles no emoji font, so the
 * icon is drawn from primitives (circles, bars, polylines). That costs a few
 * lines of code, no flash and no new asset, and it follows the theme colours.
 */
#include "weather_card.h"
#include "weather_page.h"
#include "qzdesk_core.h"
#include "theme.h"
#include <stdio.h>
#include <string.h>

#define ICON_BOX 34      /* 图标画布尺寸 */
#define SPINNER 20       /* 刷新时的转圈尺寸 */
/* 刷新动画最多转这么久：核心单次请求上限 8 秒，超过就当这次没成，
 * 免得图标一直转下去。 */
#define REFRESH_TIMEOUT_MS 12000

static lv_obj_t *card;
static lv_obj_t *icon_box;
static lv_obj_t *spinner;
static lv_obj_t *temp_row;
static lv_obj_t *temp_label;
static lv_obj_t *desc_label;
static lv_obj_t *range_label;
static lv_obj_t *detail_label;
static lv_obj_t *place_label;
/** 已经向核心要过现成快照（只问一次，之后靠推送）。 */
static bool snapshot_requested;
/** 转圈开始的时刻（0 = 当前不在刷新），用于超时兜底。 */
static uint32_t loading_started;
/** 最近一次刷新是否已超时；用于把卡片明确保持在可重试状态。 */
static bool refresh_timed_out;

/* ------------------------------------------------------------------------- *
 * 绘图辅助
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

/** 一块实心色块（圆/圆角矩形），用来拼云、太阳、雪花…… */


/* ------------------------------------------------------------------------- *
 * 图标
 * ------------------------------------------------------------------------- */

/**
 * 天气图标：换成统一的 Tabler 线性图标（A8 蒙版 + 主题色染色），不再手绘。
 * 颜色按天气分：晴是橙色，雷雨给一点橙，雾用中性灰，其余走主题强调色。
 */
static qz_icon_t icon_kind(const char *key, qz_color_token_t *tint)
{
    *tint = QZ_ACCENT;
    if (!key || key[0] == '\0') {
        *tint = QZ_TEXT_TERTIARY;
        return QZ_ICON_HELP_CIRCLE;
    }
    if (strcmp(key, "sun") == 0) {
        *tint = QZ_ORANGE;
        return QZ_ICON_SUN;
    }
    if (strcmp(key, "cloud") == 0) return QZ_ICON_CLOUD;
    if (strcmp(key, "fog") == 0) {
        *tint = QZ_TEXT_SECONDARY;
        return QZ_ICON_CLOUD_FOG;
    }
    if (strcmp(key, "drizzle") == 0 || strcmp(key, "rain") == 0 || strcmp(key, "shower") == 0) {
        return QZ_ICON_CLOUD_RAIN;
    }
    if (strcmp(key, "snow") == 0) return QZ_ICON_CLOUD_SNOW;
    if (strcmp(key, "thunder") == 0 || strcmp(key, "hail") == 0) {
        *tint = QZ_ORANGE;
        return QZ_ICON_CLOUD_STORM;
    }
    *tint = QZ_TEXT_TERTIARY;
    return QZ_ICON_HELP_CIRCLE;
}

lv_obj_t *qz_weather_icon_create(lv_obj_t *parent, int size)
{
    lv_obj_t *box = lv_obj_create(parent);
    /* 首页与详情页各自决定图标框尺寸，render 会把素材完整放入该框。 */
    lv_obj_set_size(box, size > 0 ? size : ICON_BOX, size > 0 ? size : ICON_BOX);
    make_plain(box);
    return box;
}

void qz_weather_icon_render(lv_obj_t *box, const char *key)
{
    qz_color_token_t tint;
    qz_icon_t icon = icon_kind(key, &tint);
    if (!box) return;
    /* 只清图标画布：转圈是它的兄弟对象，不受影响。 */
    lv_obj_clean(box);
    /* 让图标内容跟随自己的框：详情页与首页卡片的尺寸不同。 */
    lv_obj_update_layout(box);
    int physical = lv_obj_get_width(box) < lv_obj_get_height(box)
                       ? lv_obj_get_width(box) : lv_obj_get_height(box);
    int design_size = (int)((int64_t)physical * QZ_SCALE_DEN / qz_scale_minnum());
    lv_obj_t *image = qz_icon_image(box, icon, design_size, qz_color(tint));
    if (image) {
        const lv_image_dsc_t *source = lv_image_get_src(image);
        /* 素材是"最接近设计尺寸的那一档"，可能比框略大。显式 fit 到框内，
         * 再留一像素边缘，避免居中取整把描边推到框外；素材与框同尺寸时
         * 这一步会退化成 1:1，正好不触发 LVGL 的软件缩图。 */
        int target = physical > 2 ? physical - 2 : physical;
        int source_size = source->header.w > source->header.h
                              ? source->header.w : source->header.h;
        if (source_size > target) {
            lv_image_set_scale(image, (uint32_t)(256 * target / source_size));
        }
        lv_obj_center(image);
    }
}

/* ------------------------------------------------------------------------- *
 * 刷新动画
 * ------------------------------------------------------------------------- */

static void show_spinner(void)
{
    if (!spinner) return;
    refresh_timed_out = false;
    loading_started = lv_tick_get();
    lv_obj_add_flag(icon_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(spinner, LV_OBJ_FLAG_HIDDEN);
}

static void show_icon(void)
{
    loading_started = 0;
    if (!spinner) return;
    lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_HIDDEN);
}

/** 兜底：核心一直没回话时别让图标永远转下去。 */
static void loading_watch(lv_timer_t *timer)
{
    (void)timer;
    if (loading_started == 0) return;
    if (lv_tick_elaps(loading_started) < REFRESH_TIMEOUT_MS) return;
    show_icon();
    refresh_timed_out = true;
    if (place_label) lv_label_set_text(place_label, "刷新超时，点进去重试");
}

/**
 * 天气描述那一行写什么。
 *
 * 临近降水优先：未来两小时里要下雨时，小卡片先回答"要不要带伞"，把
 * "多云转晴"这种全天描述留给详情页 —— 屏幕上只有一行位置，问得最多的
 * 问题该先答。没有临近降水就照常写天气描述。0 分钟 = 正在下。
 */
static void show_summary(const qzdesk_core_weather_t *weather)
{
    char text[48];
    int minutes = weather->near_precipitation_minutes;

    if (minutes < 0) {
        lv_label_set_text(desc_label, weather->text[0] ? weather->text : "—");
        qz_obj_set_text_color(desc_label, QZ_TEXT_SECONDARY, 0);
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
    /* 和图标同一族的强调色：这一行说的是"接下来"，不是当前天气。 */
    qz_obj_set_text_color(desc_label, QZ_ACCENT, 0);
}

/* ------------------------------------------------------------------------- *
 * 渲染
 * ------------------------------------------------------------------------- */

void qz_weather_card_apply(const qzdesk_core_weather_t *weather)
{
    char text[96];
    bool loading;
    bool stale;
    const char *message;

    if (!weather || !card) return;
    refresh_timed_out = false;
    loading = strcmp(weather->status, "loading") == 0;
    stale = strcmp(weather->status, "stale") == 0;

    /* Refresh only occupies the icon slot; the reading and freshness stay
     * legible while the next sample is in flight. */
    if (loading) {
        show_spinner();
    } else {
        show_icon();
        lv_obj_align(icon_box, LV_ALIGN_LEFT_MID, 10, 0);
        qz_weather_icon_render(icon_box, weather->icon);
    }

    if (!weather->has_data) {
        message = loading ? "正在刷新…" : "天气暂不可用";
        lv_obj_add_flag(temp_label, LV_OBJ_FLAG_HIDDEN);
        if (!loading) {
            lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clean(icon_box);
            lv_obj_t *cloud = qz_icon_image(icon_box, QZ_ICON_CLOUD, 26,
                                            qz_color(QZ_TEXT_TERTIARY));
            if (cloud) lv_obj_center(cloud);
        }
        lv_obj_set_style_text_font(desc_label, qz_font_size(16), 0);
        lv_obj_align(desc_label, LV_ALIGN_TOP_LEFT, 76, 4);
        lv_obj_set_width(desc_label, 130);
        lv_obj_set_height(desc_label, 30);
        lv_label_set_text(desc_label, message);
        qz_obj_set_text_color(desc_label, QZ_TEXT_SECONDARY, 0);
        lv_obj_add_flag(range_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(detail_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(place_label, loading ? "刷新中" : "重试");
        qz_obj_set_text_color(place_label, loading ? QZ_ACCENT_DARK : QZ_ORANGE, 0);
        return;
    }

    lv_obj_clear_flag(temp_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(range_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(detail_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(temp_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_width(temp_label, 72);
    lv_obj_set_height(temp_label, 38);
    lv_obj_align(desc_label, LV_ALIGN_TOP_LEFT, 76, 4);
    lv_obj_set_width(desc_label, 130);
    lv_obj_set_height(desc_label, 30);
    lv_obj_set_style_text_font(desc_label, qz_font_size(11), 0);
    lv_label_set_long_mode(desc_label, LV_LABEL_LONG_DOT);

    snprintf(text, sizeof(text), "%d°", weather->temperature);
    lv_label_set_text(temp_label, text);
    show_summary(weather);

    if (qz_compact()) {
        snprintf(text, sizeof(text), "%d°~%d°", weather->low, weather->high);
    } else {
        snprintf(text, sizeof(text), "%d°~%d° · 体感 %d°",
                 weather->low, weather->high, weather->apparent);
    }
    lv_label_set_text(range_label, text);

    if (weather->wind_x10 != 0) {
        if (qz_compact()) {
            snprintf(text, sizeof(text), "湿%d%% · 风%d", weather->humidity,
                     (weather->wind_x10 + 5) / 10);
        } else {
            snprintf(text, sizeof(text), "湿%d%% · 风%d.%dm/s", weather->humidity,
                     weather->wind_x10 / 10, weather->wind_x10 % 10);
        }
    } else {
        snprintf(text, sizeof(text), "湿%d%%", weather->humidity);
    }
    lv_label_set_text(detail_label, text);

    if (strcmp(weather->status, "offline") == 0) {
        if (qz_compact()) {
            snprintf(text, sizeof(text), "%s", weather->updated[0] ? weather->updated : "离线");
        } else {
            snprintf(text, sizeof(text), "未联网%s%s",
                     weather->updated[0] && strcmp(weather->updated, "--:--") != 0 ? " · " : "",
                     weather->updated[0] && strcmp(weather->updated, "--:--") != 0 ? weather->updated : "");
        }
    } else if (weather->updated[0] != '\0' && strcmp(weather->updated, "--:--") != 0) {
        if (qz_compact()) {
            snprintf(text, sizeof(text), "%s", weather->updated);
        } else {
            snprintf(text, sizeof(text), stale ? "上次 · %s" : "更新 %s", weather->updated);
        }
    } else {
        snprintf(text, sizeof(text), "点击查看详情");
    }
    lv_label_set_text(place_label, text);
    qz_obj_set_text_color(place_label,
                          stale || strcmp(weather->status, "offline") == 0
                              ? QZ_ORANGE : QZ_TEXT_TERTIARY,
                          0);
}

/* ------------------------------------------------------------------------- *
 * 事件
 * ------------------------------------------------------------------------- */

static void weather_tapped(lv_event_t *event)
{
    (void)event;
    /* 首页卡片是入口：点进去看详情（刷新按钮在详情页里）。
     * 打开页面只是切屏 + 一次本地快照请求，仍然没有任何网络 I/O。 */
    qz_weather_page_open();
}

static void core_event(const qzdesk_core_event_t *event, void *user_data)
{
    (void)user_data;
    if (event->type == QZDESK_CORE_EVENT_WEATHER) {
        qz_weather_card_apply(&event->weather);
        return;
    }
    /* 首次状态握手说明核心在监听：这时要一次现成快照（含本地缓存），
     * 不触发网络请求，所以界面一出现就有数据可看。 */
    if (event->type == QZDESK_CORE_EVENT_STATUS && !snapshot_requested) {
        snapshot_requested = true;
        qzdesk_core_request_weather();
    }
}

/* ------------------------------------------------------------------------- *
 * 创建
 * ------------------------------------------------------------------------- */

lv_obj_t *qz_weather_card_create(lv_obj_t *parent, int x, int y, int width, int height)
{
    card = qz_card_button(parent, width, height);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_add_event_cb(card, weather_tapped, LV_EVENT_CLICKED, NULL);

    lv_obj_t *rail = lv_obj_create(card);
    lv_obj_set_size(rail, 3, height - 20);
    lv_obj_align(rail, LV_ALIGN_LEFT_MID, 0, 0);
    qz_obj_set_bg_color(rail, QZ_ACCENT, 0);
    lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rail, 0, 0);
    lv_obj_set_style_radius(rail, 0, 0);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(rail, LV_OBJ_FLAG_SCROLLABLE);

    icon_box = lv_obj_create(card);
    lv_obj_set_size(icon_box, ICON_BOX, ICON_BOX);
    lv_obj_align(icon_box, LV_ALIGN_LEFT_MID, 8, 0);
    make_plain(icon_box);

    spinner = lv_spinner_create(card);
    lv_obj_set_size(spinner, SPINNER, SPINNER);
    lv_obj_align(spinner, LV_ALIGN_LEFT_MID, 8 + (ICON_BOX - SPINNER) / 2, 0);
    /* 只留那一段转动的弧，底圈不画，小尺寸下更干净。 */
    lv_obj_set_style_arc_opa(spinner, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 3, LV_PART_INDICATOR);
    qz_obj_set_arc_color(spinner, QZ_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(spinner, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(spinner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);

    temp_row = lv_obj_create(card);
    lv_obj_set_size(temp_row, 240, 38);
    lv_obj_align(temp_row, LV_ALIGN_TOP_LEFT, 44, 0);
    make_plain(temp_row);
    temp_label = qz_text(temp_row, "--°", qz_compact() ? 23 : 28, qz_color(QZ_TEXT));
    lv_obj_align(temp_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_width(temp_label, 72);
    lv_obj_set_height(temp_label, 38);
    lv_label_set_long_mode(temp_label, LV_LABEL_LONG_CLIP);
    desc_label = qz_text(temp_row, "", 11, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_set_width(desc_label, 130);
    lv_obj_set_height(desc_label, 30);
    lv_obj_align(desc_label, LV_ALIGN_TOP_LEFT, 76, 4);
    lv_label_set_long_mode(desc_label, LV_LABEL_LONG_DOT);

    range_label = qz_text(card, "", 10, qz_color(QZ_TEXT_TERTIARY));
    detail_label = qz_text(card, "", 10, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_set_size(range_label, 124, 14);
    lv_obj_align(range_label, LV_ALIGN_TOP_LEFT, 44, 39);
    lv_label_set_long_mode(range_label, LV_LABEL_LONG_DOT);
    lv_obj_set_size(detail_label, 106, 14);
    lv_obj_align(detail_label, LV_ALIGN_TOP_RIGHT, -8, 39);
    lv_label_set_long_mode(detail_label, LV_LABEL_LONG_DOT);
    place_label = qz_text(card, "", 10, qz_color(QZ_TEXT_TERTIARY));
    lv_obj_set_width(place_label, 66);
    lv_obj_set_height(place_label, 16);
    lv_obj_set_style_text_align(place_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(place_label, LV_ALIGN_TOP_RIGHT, -8, 7);
    lv_label_set_long_mode(place_label, LV_LABEL_LONG_DOT);

    /* First frame names the wait instead of showing a dead "--°". */
    lv_obj_add_flag(temp_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(desc_label, LV_ALIGN_TOP_LEFT, 76, 4);
    lv_label_set_text(desc_label, "天气加载中…");
    lv_label_set_text(place_label, "正在读取");
    qz_obj_set_text_color(place_label, QZ_ACCENT_DARK, 0);
    lv_obj_align(icon_box, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_clean(icon_box);
    lv_obj_t *cloud = qz_icon_image(icon_box, QZ_ICON_CLOUD, 34, qz_color(QZ_TEXT_TERTIARY));
    if (cloud) lv_obj_center(cloud);

    /* 订阅核心事件：助手页拿着轮询，这里只加一条订阅，不会抢走它的包。 */
    qzdesk_core_subscribe(core_event, NULL);
    lv_timer_create(loading_watch, 1000, NULL);
    return card;
}
