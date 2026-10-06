#ifndef QZDESK_CORE_PROTOCOL_H
#define QZDESK_CORE_PROTOCOL_H

#include <stdbool.h>

typedef enum {
    QZDESK_CORE_EVENT_STATUS,
    /** 一条聊天记录：用户打字 / 语音识别、助手回复、核心提示，GUI 与 web 共用。 */
    QZDESK_CORE_EVENT_CHAT,
    QZDESK_CORE_EVENT_TOAST,
    QZDESK_CORE_EVENT_ACTIVATION,
    /** 天气快照：核心取好数、缓存好后推过来，网页控制台读的是同一份。 */
    QZDESK_CORE_EVENT_WEATHER,
    /** 性能监控快照：核心每 2 秒采一次，设备页与网页控制台读的是同一份。 */
    QZDESK_CORE_EVENT_PERFORMANCE,
} qzdesk_core_event_type_t;

/** 指标等级：核心已按阈值判好，界面只做「颜色 + 文字」映射，不各算一套。 */
typedef enum {
    QZDESK_LEVEL_UNKNOWN = 0, /**< 不可用 */
    QZDESK_LEVEL_OK,
    QZDESK_LEVEL_WARN,
    QZDESK_LEVEL_CRITICAL,
} qzdesk_level_t;

/** 服务 / 进程状态。 */
typedef enum {
    QZDESK_SERVICE_UNKNOWN = 0,
    QZDESK_SERVICE_RUNNING,
    QZDESK_SERVICE_STOPPED,
    QZDESK_SERVICE_DISABLED,
} qzdesk_service_t;

/** CPU 曲线保留的采样点数（与核心侧一致）。 */
#define QZDESK_PERFORMANCE_HISTORY 60
/** 最多接收几个 CPU 核心的使用率（页面按 5 列铺开，够 16 核）。 */
#define QZDESK_PERFORMANCE_CORES 16
/** 最多上报几条进程。 */
#define QZDESK_PERFORMANCE_PROCESSES 5
/** 每条进程说明的文本长度。 */
#define QZDESK_PERFORMANCE_PROCESS_TEXT 72

/**
 * 性能监控快照。
 *
 * 百分比统一用「×10 的整数」（例如 12.4% 存成 124）：界面层因此不必碰浮点，
 * 也不会因为地区设置出现小数点差异。数值缺失时对应的 `has_*` 为 false。
 */
typedef struct {
    unsigned long long timestamp;
    unsigned long long uptime_secs;

    int cpu_x10;
    qzdesk_level_t cpu_level;
    int core_count;
    int core_x10[QZDESK_PERFORMANCE_CORES];

    int memory_x10;
    qzdesk_level_t memory_level;
    unsigned long long memory_total_kb;
    unsigned long long memory_used_kb;
    unsigned long long memory_available_kb;
    unsigned long long swap_total_kb;
    unsigned long long swap_used_kb;

    int storage_x10;
    qzdesk_level_t storage_level;
    unsigned long long storage_total_kb;
    unsigned long long storage_used_kb;
    unsigned long long storage_available_kb;

    bool has_temperature;
    int temperature_x10;
    qzdesk_level_t temperature_level;

    bool has_wifi;
    int wifi_dbm;
    qzdesk_level_t wifi_level;

    bool has_latency;
    int latency_x10;
    qzdesk_level_t latency_level;

    /** 1 / 5 / 15 分钟负载，×100 的整数。 */
    int load_x100[3];

    qzdesk_service_t core_service;
    qzdesk_service_t ui_service;
    qzdesk_service_t audio_service;

    int history_count;
    int history_x10[QZDESK_PERFORMANCE_HISTORY];

    int process_count;
    /** 已经排版好的进程说明（角色 · 名称 · CPU · 内存）。 */
    char process_text[QZDESK_PERFORMANCE_PROCESSES][QZDESK_PERFORMANCE_PROCESS_TEXT];
} qzdesk_core_performance_t;

/**
 * 天气快照。
 *
 * 与网页控制台 `/api/weather` 返回的是同一份数据，字段由核心统一填好：
 * 网络、缓存、超时、重试全在核心侧，GUI 只管显示，从不等网络。
 */
typedef struct {
    /** `ok`（刚取到）/ `loading`（正在刷新，下面仍是旧数据）/ `stale`（沿用上一次）
     *  / `error` / `no_location` / `bad_location`。 */
    char status[16];
    /** 城市名；没配置时核心填「未设置位置」。 */
    char city[32];
    /** 中文描述：晴、多云、毛毛雨…… */
    char text[16];
    /** 图标键：sun / cloud / fog / drizzle / rain / snow / shower / thunder /
     *  hail / unknown —— 界面据此画图标（LVGL 没有天气字形）。 */
    char icon[12];
    /** 数据观测时间，HH:MM。 */
    char updated[12];
    /** 状态说明或错误原因（如「天气暂不可用」「位置配置错误」）。 */
    char message[48];
    /** true 表示下面的数值是真实数据；false 时只显示 `status`/`message`。 */
    bool has_data;
    int temperature;    /* 当前温度（摄氏度，整数） */
    int apparent;       /* 体感温度 */
    int humidity;       /* 相对湿度（%） */
    int high;           /* 今日最高 */
    int low;            /* 今日最低 */
    int precipitation;  /* 今日降水概率上限（%） */
    int uv_x10;         /* 紫外线指数 ×10：界面层不碰浮点 */
    char sunrise[8];    /* 日出 HH:MM（拿不到为空串） */
    char sunset[8];     /* 日落 HH:MM */
    int wind_x10;       /* 风速 m/s ×10：界面层不必碰浮点 */
    int code;           /* Open-Meteo weather_code */
} qzdesk_core_weather_t;

typedef struct {
    qzdesk_core_event_type_t type;
    int state;
    /** 聊天记录的角色："user" / "assistant" / "system"（其他事件为空串）。 */
    char role[12];
    /**
     * Event text (chat message, toast, activation code).
     *
     * Sized for the longest message that can reach the GUI: the web console
     * accepts a few thousand characters, and the core forwards them verbatim.
     * Anything longer is truncated rather than dropped.
     */
    char text[1024];
    /** 仅 `QZDESK_CORE_EVENT_WEATHER` 有效。 */
    qzdesk_core_weather_t weather;
    /** 仅 `QZDESK_CORE_EVENT_PERFORMANCE` 有效。 */
    qzdesk_core_performance_t performance;
} qzdesk_core_event_t;

typedef void (*qzdesk_core_event_cb_t)(const qzdesk_core_event_t *event, void *user_data);

bool qzdesk_core_open(void);
bool qzdesk_core_request_status(void);
/**
 * Ask the core to replay the recent chat history as a series of `chat` events.
 *
 * The core keeps the one and only chat record (the web console reads the same
 * one), so a freshly started GUI can show the conversation that already
 * happened instead of an empty list. Send it once, after the status handshake
 * proves the core is listening.
 */
bool qzdesk_core_request_history(void);
/**
 * Hand a typed message to the core as `{"type":"chat_text","text":"..."}`.
 *
 * The cloud only accepts audio as user input, so the core synthesises this text
 * into speech (讯飞在线语音合成) and uploads it through the same Opus path the
 * microphone uses. Returns false only when the message could not be handed over
 * (the core process is not reachable) — a failed synthesis is reported back as a
 * toast, not by this return value.
 */
bool qzdesk_core_send_text(const char *text);
void qzdesk_core_poll(qzdesk_core_event_cb_t callback, void *user_data);
/**
 * Register an extra event handler.
 *
 * `qzdesk_core_poll` hands every datagram to one callback, but more than one
 * page cares about the stream: the assistant page owns the poll, while the home
 * screen wants the weather. Subscribers receive every event *in addition to*
 * the polling callback, so a page can react without draining datagrams that
 * another page still needs. Returns false when the table is full.
 */
bool qzdesk_core_subscribe(qzdesk_core_event_cb_t callback, void *user_data);
/**
 * Ask for the current weather snapshot: whatever the core already has (cache
 * included). No network request is made, so this is safe to call on startup.
 */
bool qzdesk_core_request_weather(void);
/**
 * Ask the core to refresh the weather now.
 *
 * Sends one datagram to the local core; the HTTP request itself happens in the
 * core's own task. A UI event handler therefore never waits on the network.
 */
bool qzdesk_core_refresh_weather(void);
/**
 * Ask for the current performance snapshot (last sample, no new sampling).
 *
 * The monitor samples every two seconds on its own; this only asks it to push
 * what it already has, so opening the page shows numbers immediately instead of
 * waiting for the next tick.
 */
bool qzdesk_core_request_performance(void);
void qzdesk_core_close(void);

#endif
