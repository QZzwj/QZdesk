/**
 * Performance monitor page (480x320) — everything on **one screen**.
 *
 * The page is deliberately not a scrolling list: the whole point is to glance at
 * the device and see its state. So the six areas share the height instead of
 * stacking past the bottom edge:
 *
 *   ┌ 概览带：CPU / 内存 / 磁盘 / 温度（各带进度条） ┐
 *   ├ CPU 使用率 + 负载 + 最近 60 点曲线            ┤
 *   ├ 各核心（5 列铺开，10 核两行）                 ┤
 *   ├ 内存（含 Swap）        │ 存储（根文件系统）   ┤
 *   └ 设备状态                │ 进程                ┘
 *
 * All numbers come from the core's 2-second sample (`QZDESK_CORE_EVENT_PERFORMANCE`),
 * so the device and the web console show the same data and nothing is sampled on
 * the LVGL thread. Widgets are created once; an update only writes text/values.
 */
#include "performance_page.h"
#include "qzdesk_core.h"
#include "theme.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CARD_W (QZ_SCREEN_W - 2 * QZ_GUTTER) /* 452 */
#define CONTENT_TOP (QZ_TOOLBAR_H + 8) /* 52 */
#define CORE_COLUMNS 5
#define MAX_CORE_ROWS 2
#define STATUS_LINES 4
#define PROCESS_LINES 4
#define CHART_POINTS QZDESK_PERFORMANCE_HISTORY

/* 各带的高度与纵向位置：加起来正好落在 320 以内，一屏看完，不需要滚动 */
#define ROW_GAP 6
#define ROW1_Y 0
#define ROW1_H 40 /* 概览 */
#define ROW2_Y (ROW1_Y + ROW1_H + ROW_GAP)
#define ROW2_H 52 /* CPU + 曲线 */
#define ROW3_Y (ROW2_Y + ROW2_H + ROW_GAP)
#define ROW3_H 44 /* 各核心 */
#define ROW4_Y (ROW3_Y + ROW3_H + ROW_GAP)
#define ROW4_H 48 /* 内存 | 存储 */
#define ROW5_Y (ROW4_Y + ROW4_H + ROW_GAP)
#define ROW5_H 54 /* 状态 | 进程 */
#define HALF_W ((CARD_W - 10) / 2)

/* 卡片不设内边距（那会把可用高度吃掉 24px，曲线会被裁掉），
 * 内部一律用这个内缩量自己留边，坐标就是卡片左上角起算，心里有数。 */
#define INSET 12

static lv_obj_t *screen;
static lv_obj_t *apps_screen_ref;
static lv_obj_t *update_label;
static bool visible;
/** 曲线是否已经用核心给的历史填满过（之后每次只追加一个点）。 */
static bool chart_seeded;

/* 概览：CPU / 内存 / 磁盘 / 温度 */
static lv_obj_t *overview_bar[4];
static lv_obj_t *overview_value[4];
static lv_obj_t *overview_note[4];
static const char *overview_title[4] = {"CPU", "内存", "磁盘", "温度"};

/* CPU */
static lv_obj_t *cpu_value;
static lv_obj_t *cpu_note;
static lv_obj_t *cpu_bar;
static lv_obj_t *cpu_chart;
static lv_chart_series_t *cpu_series;

/* 各核心 */
static lv_obj_t *core_bar[QZDESK_PERFORMANCE_CORES];
static lv_obj_t *core_label[QZDESK_PERFORMANCE_CORES];

/* 内存 / 存储 */
static lv_obj_t *memory_value;
static lv_obj_t *memory_detail;
static lv_obj_t *memory_bar;
static lv_obj_t *storage_value;
static lv_obj_t *storage_detail;
static lv_obj_t *storage_bar;

/* 状态 / 进程 */
static lv_obj_t *status_line[STATUS_LINES];
static lv_obj_t *process_line[PROCESS_LINES];

/* ------------------------------------------------------------------------- *
 * 等级 → 颜色 / 文字：阈值在核心侧判，这里只映射，两边不会各算一套
 * ------------------------------------------------------------------------- */

static qz_color_token_t level_color(qzdesk_level_t level)
{
    switch (level) {
    case QZDESK_LEVEL_OK: return QZ_GREEN;
    case QZDESK_LEVEL_WARN: return QZ_ORANGE;
    case QZDESK_LEVEL_CRITICAL: return QZ_RED;
    default: return QZ_TEXT_TERTIARY;
    }
}

/** 文字说明：异常不能只靠颜色表达 */
static const char *level_caption(qzdesk_level_t level)
{
    switch (level) {
    case QZDESK_LEVEL_OK: return "正常";
    case QZDESK_LEVEL_WARN: return "偏高";
    case QZDESK_LEVEL_CRITICAL: return "危险";
    default: return "不可用";
    }
}

static const char *service_caption(qzdesk_service_t service)
{
    switch (service) {
    case QZDESK_SERVICE_RUNNING: return "运行中";
    case QZDESK_SERVICE_STOPPED: return "已停止";
    case QZDESK_SERVICE_DISABLED: return "已关闭";
    default: return "未知";
    }
}

/* ------------------------------------------------------------------------- *
 * 格式化
 * ------------------------------------------------------------------------- */

/** 把已经写好的内容后面接上后缀（不做格式化，也就没有截断告警） */
static void append_text(char *buffer, size_t size, const char *suffix)
{
    size_t length = strlen(buffer);
    size_t index = 0;
    if (!suffix) return;
    while (suffix[index] != '\0' && length + 1 < size) {
        buffer[length++] = suffix[index++];
    }
    buffer[length] = '\0';
}

/**
 * ×10 的整数写成 "12.4"。
 *
 * 手写十进制而不是 `snprintf("%d.%d")`：后者在编译期无法证明不会溢出目标缓冲，
 * 于是每个调用点都会收到一条 `-Wformat-truncation` 告警。这里的取值范围是
 * 百分比 0..1000、温度 0..1500，手写既准确又没有告警。
 */
static void write_tenths(char *buffer, size_t size, int value_x10)
{
    char digits[8];
    int count = 0;
    int value = value_x10 < 0 ? 0 : (value_x10 > 999999 ? 999999 : value_x10);
    int whole = value / 10;
    int fraction = value % 10;
    size_t index = 0;

    if (!buffer || size == 0) return;
    if (whole == 0) {
        digits[count++] = '0';
    } else {
        while (whole > 0 && count < (int)sizeof(digits)) {
            digits[count++] = (char)('0' + whole % 10);
            whole /= 10;
        }
    }
    while (count > 0 && index + 1 < size) {
        buffer[index++] = digits[--count];
    }
    if (index + 2 < size) {
        buffer[index++] = '.';
        buffer[index++] = (char)('0' + fraction);
    }
    buffer[index] = '\0';
}

/** ×10 的整数 → "12.4%"（或 "7.3 ms" 这样带单位的写法） */
static void format_x10(char *buffer, size_t size, int value_x10, const char *suffix)
{
    if (!buffer || size == 0) return;
    write_tenths(buffer, size, value_x10);
    append_text(buffer, size, suffix);
}

static void format_size(char *buffer, size_t size, unsigned long long kilobytes)
{
    if (kilobytes >= 1024ULL * 1024ULL) {
        snprintf(buffer, size, "%.1fG", (double)kilobytes / (1024.0 * 1024.0));
    } else if (kilobytes >= 1024ULL) {
        snprintf(buffer, size, "%lluM", kilobytes / 1024ULL);
    } else {
        snprintf(buffer, size, "%lluK", kilobytes);
    }
}

static void format_duration(char *buffer, size_t size, unsigned long long seconds)
{
    unsigned long long days = seconds / 86400ULL;
    unsigned long long hours = (seconds % 86400ULL) / 3600ULL;
    unsigned long long minutes = (seconds % 3600ULL) / 60ULL;
    if (days > 0) {
        snprintf(buffer, size, "%llu 天 %llu 小时", days, hours);
    } else if (hours > 0) {
        snprintf(buffer, size, "%llu 小时 %llu 分", hours, minutes);
    } else {
        snprintf(buffer, size, "%llu 分 %llu 秒", minutes, seconds % 60ULL);
    }
}

/* ------------------------------------------------------------------------- *
 * 控件
 * ------------------------------------------------------------------------- */

/** 一张分区卡片：固定尺寸，内部用左上角绝对定位，位置可预期 */
static lv_obj_t *card(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *plate = lv_obj_create(parent);
    lv_obj_set_size(plate, width, height);
    lv_obj_set_pos(plate, x, y);
    qz_style_plate(plate);
    lv_obj_set_style_radius(plate, QZ_RADIUS_CARD, 0);
    lv_obj_set_style_pad_all(plate, 0, 0);
    lv_obj_clear_flag(plate, LV_OBJ_FLAG_SCROLLABLE);
    return plate;
}

static lv_obj_t *caption(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = qz_text(parent, text, 9, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, INSET, 2);
    return label;
}

/** 单行文本：固定宽度 + 省略号，装不下就点掉，绝不换行把卡片撑高 */
static lv_obj_t *line(lv_obj_t *parent, int x, int y, int width, qz_color_token_t token)
{
    lv_obj_t *label = qz_text(parent, "", 9, qz_color(token));
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    return label;
}

/** 进度条：范围 0..1000，直接把 ×10 的百分比塞进去 */
static lv_obj_t *metric_bar(lv_obj_t *parent, int x, int y, int width, int height)
{
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, width, height);
    lv_obj_set_pos(bar, x, y);
    lv_bar_set_range(bar, 0, 1000);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, qz_color(QZ_FILL), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, qz_color(QZ_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    return bar;
}

static void bar_set(lv_obj_t *bar, int value_x10, qz_color_token_t color, bool available)
{
    if (!bar) return;
    int clamped = available ? value_x10 : 0;
    if (clamped < 0) clamped = 0;
    if (clamped > 1000) clamped = 1000;
    lv_bar_set_value(bar, clamped, LV_ANIM_OFF);
    qz_obj_set_bg_color(bar, available ? color : QZ_FILL, LV_PART_INDICATOR);
}

/* ------------------------------------------------------------------------- *
 * 各带
 * ------------------------------------------------------------------------- */

static void build_overview(lv_obj_t *content)
{
    lv_obj_t *plate = card(content, QZ_GUTTER, ROW1_Y, CARD_W, ROW1_H);
    int cell = (CARD_W - 2 * INSET) / 4;
    for (int index = 0; index < 4; index++) {
        int x = INSET + index * cell;
        lv_obj_t *name = qz_text(plate, overview_title[index], 9, qz_color(QZ_TEXT_SECONDARY));
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, x, 3);
        overview_value[index] = qz_text(plate, "--", 15, qz_color(QZ_TEXT));
        lv_obj_align(overview_value[index], LV_ALIGN_TOP_LEFT, x, 12);
        overview_note[index] = qz_text(plate, "", 9, qz_color(QZ_TEXT_TERTIARY));
        lv_obj_align(overview_note[index], LV_ALIGN_TOP_LEFT, x + 62, 15);
        overview_bar[index] = metric_bar(plate, x, 32, cell - 10, 5);
    }
}

static void build_cpu(lv_obj_t *content)
{
    lv_obj_t *plate = card(content, QZ_GUTTER, ROW2_Y, CARD_W, ROW2_H);
    caption(plate, "CPU");
    cpu_value = qz_text(plate, "--", 16, qz_color(QZ_TEXT));
    lv_obj_align(cpu_value, LV_ALIGN_TOP_LEFT, INSET, 11);
    cpu_bar = metric_bar(plate, INSET, 31, 150, 6);
    cpu_note = line(plate, INSET, 40, 180, QZ_TEXT_SECONDARY);

    cpu_chart = lv_chart_create(plate);
    lv_obj_set_size(cpu_chart, 236, 44);
    lv_obj_align(cpu_chart, LV_ALIGN_TOP_RIGHT, -INSET, 4);
    /* 固定 60 个点：曲线只滚动显示最近两分钟，不随采样增长 */
    lv_chart_set_type(cpu_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(cpu_chart, CHART_POINTS);
    lv_chart_set_range(cpu_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(cpu_chart, 2, 0);
    lv_obj_set_style_bg_opa(cpu_chart, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(cpu_chart, 0, 0);
    lv_obj_set_style_line_width(cpu_chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(cpu_chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_clear_flag(cpu_chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(cpu_chart, LV_OBJ_FLAG_SCROLLABLE);
    cpu_series = lv_chart_add_series(cpu_chart, qz_color(QZ_ACCENT), LV_CHART_AXIS_PRIMARY_Y);
}

static void build_cores(lv_obj_t *content)
{
    lv_obj_t *plate = card(content, QZ_GUTTER, ROW3_Y, CARD_W, ROW3_H);
    caption(plate, "各核心");

    int gap = 6;
    int cell_w = (CARD_W - 2 * INSET - (CORE_COLUMNS - 1) * gap) / CORE_COLUMNS;
    for (int index = 0; index < QZDESK_PERFORMANCE_CORES; index++) {
        int column = index % CORE_COLUMNS;
        int row = index / CORE_COLUMNS;
        int x = INSET + column * (cell_w + gap);
        int y = 14 + row * 14;
        core_label[index] = qz_text(plate, "", 9, qz_color(QZ_TEXT_SECONDARY));
        lv_obj_align(core_label[index], LV_ALIGN_TOP_LEFT, x, y);
        core_bar[index] = metric_bar(plate, x + 32, y + 3, cell_w - 32, 4);
        /* 默认隐藏：核心数少时不要留一串空条 */
        lv_obj_add_flag(core_label[index], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(core_bar[index], LV_OBJ_FLAG_HIDDEN);
    }
}

/** 内存、存储共用的一格：大值 + 明细 + 进度条 */
static void build_half(lv_obj_t *content, int x, int y, const char *title, lv_obj_t **value,
                       lv_obj_t **detail, lv_obj_t **bar)
{
    lv_obj_t *plate = card(content, x, y, HALF_W, ROW4_H);
    caption(plate, title);
    *value = qz_text(plate, "--", 13, qz_color(QZ_TEXT));
    lv_obj_align(*value, LV_ALIGN_TOP_LEFT, INSET, 14);
    *detail = line(plate, INSET, 32, HALF_W - 2 * INSET, QZ_TEXT_SECONDARY);
    *bar = metric_bar(plate, INSET, 43, HALF_W - 2 * INSET, 4);
}

static void build_status(lv_obj_t *content)
{
    lv_obj_t *plate = card(content, QZ_GUTTER, ROW5_Y, HALF_W, ROW5_H);
    caption(plate, "设备状态");
    for (int index = 0; index < STATUS_LINES; index++) {
        status_line[index] = line(plate, INSET, 14 + index * 10, HALF_W - 2 * INSET,
                                  QZ_TEXT_SECONDARY);
    }
}

static void build_processes(lv_obj_t *content)
{
    lv_obj_t *plate = card(content, QZ_GUTTER + HALF_W + 10, ROW5_Y, HALF_W, ROW5_H);
    caption(plate, "进程");
    for (int index = 0; index < PROCESS_LINES; index++) {
        process_line[index] = line(plate, INSET, 14 + index * 10, HALF_W - 2 * INSET,
                                   QZ_TEXT_SECONDARY);
    }
}

/* ------------------------------------------------------------------------- *
 * 渲染
 * ------------------------------------------------------------------------- */

static void apply(const qzdesk_core_performance_t *data)
{
    /* 中间结果各用容量明确的小缓冲，组合行才用大一点的 text：
     * 这样每个 snprintf 的容量上限都能被编译器算清楚，不会有截断告警。 */
    char percent[12];
    char used[16];
    char total[16];
    char swap[16];
    char uptime[32];
    char temperature[16];
    char latency[16];
    char text[96];

    /* —— 概览四指标 —— */
    int values[4] = {data->cpu_x10, data->memory_x10, data->storage_x10,
                     data->has_temperature ? data->temperature_x10 : 0};
    qzdesk_level_t levels[4] = {data->cpu_level, data->memory_level, data->storage_level,
                                data->temperature_level};
    bool available[4] = {true, true, true, data->has_temperature};

    for (int index = 0; index < 4; index++) {
        if (!available[index]) {
            lv_label_set_text(overview_value[index], "不可用");
            lv_obj_set_style_text_color(overview_value[index], qz_color(QZ_TEXT_TERTIARY), 0);
            lv_label_set_text(overview_note[index], "");
        } else {
            format_x10(percent, sizeof(percent), values[index], index == 3 ? "°" : "%");
            lv_label_set_text(overview_value[index], percent);
            lv_obj_set_style_text_color(overview_value[index],
                                        qz_color(level_color(levels[index])), 0);
            lv_label_set_text(overview_note[index], level_caption(levels[index]));
        }
        bar_set(overview_bar[index], values[index], level_color(levels[index]), available[index]);
    }

    /* —— CPU：使用率 + 负载 + 曲线 —— */
    format_x10(percent, sizeof(percent), data->cpu_x10, "%");
    lv_label_set_text(cpu_value, percent);
    lv_obj_set_style_text_color(cpu_value, qz_color(level_color(data->cpu_level)), 0);
    bar_set(cpu_bar, data->cpu_x10, level_color(data->cpu_level), true);
    snprintf(text, sizeof(text), "%s · 负载 %d.%02d / %d.%02d / %d.%02d",
             level_caption(data->cpu_level),
             data->load_x100[0] / 100, data->load_x100[0] % 100,
             data->load_x100[1] / 100, data->load_x100[1] % 100,
             data->load_x100[2] / 100, data->load_x100[2] % 100);
    lv_label_set_text(cpu_note, text);

    /* 曲线：第一次用核心给的历史填满，之后每次只追加最新一点 */
    if (data->history_count > 0) {
        if (!chart_seeded) {
            chart_seeded = true;
            for (int index = 0; index < data->history_count; index++) {
                lv_chart_set_next_value(cpu_chart, cpu_series, data->history_x10[index] / 10);
            }
        } else {
            lv_chart_set_next_value(cpu_chart, cpu_series,
                                    data->history_x10[data->history_count - 1] / 10);
        }
    }

    /* —— 各核心：5 列铺开，最多两行 —— */
    for (int index = 0; index < QZDESK_PERFORMANCE_CORES; index++) {
        bool show = index < data->core_count && (index / CORE_COLUMNS) < MAX_CORE_ROWS;
        if (!show) {
            lv_obj_add_flag(core_label[index], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(core_bar[index], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(core_label[index], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(core_bar[index], LV_OBJ_FLAG_HIDDEN);
        snprintf(text, sizeof(text), "%d %d%%", index, data->core_x10[index] / 10);
        lv_label_set_text(core_label[index], text);
        bar_set(core_bar[index], data->core_x10[index], QZ_ACCENT, true);
    }

    /* —— 内存 / 存储 —— */
    if (data->memory_total_kb > 0) {
        format_size(used, sizeof(used), data->memory_used_kb);
        format_size(total, sizeof(total), data->memory_total_kb);
        format_x10(percent, sizeof(percent), data->memory_x10, "%");
        snprintf(text, sizeof(text), "内存 %s", percent);
        lv_label_set_text(memory_value, text);
        lv_obj_set_style_text_color(memory_value, qz_color(level_color(data->memory_level)), 0);
        if (data->swap_total_kb > 0) {
            format_size(swap, sizeof(swap), data->swap_used_kb);
            snprintf(text, sizeof(text), "已用 %s / %s · Swap %s", used, total, swap);
        } else {
            snprintf(text, sizeof(text), "已用 %s / %s · Swap 未启用", used, total);
        }
        lv_label_set_text(memory_detail, text);
        bar_set(memory_bar, data->memory_x10, level_color(data->memory_level), true);
    } else {
        lv_label_set_text(memory_value, "内存 不可用");
        lv_obj_set_style_text_color(memory_value, qz_color(QZ_TEXT_TERTIARY), 0);
        lv_label_set_text(memory_detail, "");
        bar_set(memory_bar, 0, QZ_TEXT_TERTIARY, false);
    }

    if (data->storage_total_kb > 0) {
        format_size(used, sizeof(used), data->storage_used_kb);
        format_size(total, sizeof(total), data->storage_total_kb);
        format_x10(percent, sizeof(percent), data->storage_x10, "%");
        snprintf(text, sizeof(text), "磁盘 %s", percent);
        lv_label_set_text(storage_value, text);
        lv_obj_set_style_text_color(storage_value, qz_color(level_color(data->storage_level)), 0);
        snprintf(text, sizeof(text), "已用 %s / %s · %s", used, total,
                 level_caption(data->storage_level));
        lv_label_set_text(storage_detail, text);
        bar_set(storage_bar, data->storage_x10, level_color(data->storage_level), true);
    } else {
        lv_label_set_text(storage_value, "磁盘 不可用");
        lv_obj_set_style_text_color(storage_value, qz_color(QZ_TEXT_TERTIARY), 0);
        lv_label_set_text(storage_detail, "");
        bar_set(storage_bar, 0, QZ_TEXT_TERTIARY, false);
    }

    /* —— 设备状态：四行，一行一件事 —— */
    format_duration(uptime, sizeof(uptime), data->uptime_secs);
    snprintf(text, sizeof(text), "运行 %s", uptime);
    lv_label_set_text(status_line[0], text);

    /* 温度与 Wi-Fi 常用一行：都不可用时就直接写文案，省得两行空占位置 */
    if (data->has_temperature) {
        format_x10(temperature, sizeof(temperature), data->temperature_x10, "°C");
        if (data->has_wifi) {
            snprintf(text, sizeof(text), "温度 %s · Wi-Fi %d", temperature, data->wifi_dbm);
        } else {
            snprintf(text, sizeof(text), "温度 %s · Wi-Fi 不可用", temperature);
        }
    } else if (data->has_wifi) {
        snprintf(text, sizeof(text), "温度 不可用 · Wi-Fi %d", data->wifi_dbm);
    } else {
        snprintf(text, sizeof(text), "温度 不可用 · Wi-Fi 不可用");
    }
    lv_label_set_text(status_line[1], text);

    if (data->has_latency) {
        format_x10(latency, sizeof(latency), data->latency_x10, " ms");
        snprintf(text, sizeof(text), "延迟 %s", latency);
    } else {
        snprintf(text, sizeof(text), "延迟 不可用");
    }
    lv_label_set_text(status_line[2], text);

    snprintf(text, sizeof(text), "服务 %s · %s · 音频 %s",
             service_caption(data->core_service), service_caption(data->ui_service),
             service_caption(data->audio_service));
    lv_label_set_text(status_line[3], text);

    /* —— 进程：核心 / 界面 / 占用最高 —— */
    for (int index = 0; index < PROCESS_LINES; index++) {
        if (index < data->process_count && data->process_text[index][0] != '\0') {
            lv_label_set_text(process_line[index], data->process_text[index]);
        } else {
            lv_label_set_text(process_line[index], index == 0 ? "暂无数据" : "");
        }
    }

    /* —— 顶部：最近更新时间 —— */
    if (data->timestamp > 0) {
        time_t seconds = (time_t)data->timestamp;
        struct tm local_time;
        localtime_r(&seconds, &local_time);
        snprintf(text, sizeof(text), "实时 · %02d:%02d:%02d",
                 local_time.tm_hour, local_time.tm_min, local_time.tm_sec);
    } else {
        snprintf(text, sizeof(text), "等待核心数据");
    }
    lv_label_set_text(update_label, text);
}

/* ------------------------------------------------------------------------- *
 * 事件
 * ------------------------------------------------------------------------- */

static void core_event(const qzdesk_core_event_t *event, void *user_data)
{
    (void)user_data;
    /* 不在最前面显示时完全不碰控件：隐藏的页面不该有任何刷新开销 */
    if (!visible || !screen) return;
    if (event->type != QZDESK_CORE_EVENT_PERFORMANCE) return;
    apply(&event->performance);
}

static void on_screen_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_SCREEN_LOADED) {
        visible = true;
        /* 进页面就要一份现成的：核心 2 秒一采，不必等下一拍 */
        qzdesk_core_request_performance();
    } else if (code == LV_EVENT_SCREEN_UNLOADED) {
        visible = false;
    }
}

static void back_to_apps(lv_event_t *event)
{
    (void)event;
    if (apps_screen_ref) {
        qz_screen_load(apps_screen_ref, LV_SCR_LOAD_ANIM_MOVE_RIGHT, QZ_DUR_SCREEN);
    }
}

/* ------------------------------------------------------------------------- *
 * 创建
 * ------------------------------------------------------------------------- */

lv_obj_t *qz_performance_create(lv_obj_t *apps_screen)
{
    apps_screen_ref = apps_screen;
    screen = lv_obj_create(NULL);
    qz_style_screen(screen);
    lv_obj_add_event_cb(screen, on_screen_event, LV_EVENT_SCREEN_LOADED, NULL);
    lv_obj_add_event_cb(screen, on_screen_event, LV_EVENT_SCREEN_UNLOADED, NULL);

    /* 顶部：返回 + 标题 + 刷新状态与最近更新时间 */
    lv_obj_t *toolbar = lv_obj_create(screen);
    lv_obj_set_size(toolbar, QZ_SCREEN_W - 16, QZ_TOOLBAR_H - 4);
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 6);
    qz_style_toolbar(toolbar);

    lv_obj_t *back = qz_icon_button(toolbar, LV_SYMBOL_LEFT, 30);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_event_cb(back, back_to_apps, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = qz_text(toolbar, "性能监控", 15, qz_color(QZ_TEXT));
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    update_label = qz_text(toolbar, "等待核心数据", 9, qz_color(QZ_TEXT_SECONDARY));
    lv_obj_align(update_label, LV_ALIGN_RIGHT_MID, -10, 0);

    /* 内容：固定位置的一条条带，一屏装完（可滚动只是兜底：字体不同也不会被切掉） */
    lv_obj_t *content = lv_obj_create(screen);
    lv_obj_set_size(content, QZ_SCREEN_W, QZ_SCREEN_H - CONTENT_TOP - 4);
    lv_obj_align(content, LV_ALIGN_TOP_LEFT, 0, CONTENT_TOP);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);

    build_overview(content);
    build_cpu(content);
    build_cores(content);
    build_half(content, QZ_GUTTER, ROW4_Y, "内存", &memory_value, &memory_detail, &memory_bar);
    build_half(content, QZ_GUTTER + HALF_W + 10, ROW4_Y, "存储（根文件系统）", &storage_value,
               &storage_detail, &storage_bar);
    build_status(content);
    build_processes(content);

    /* 订阅核心事件：助手页拿着轮询，这里只加一条订阅 */
    qzdesk_core_subscribe(core_event, NULL);
    return screen;
}
