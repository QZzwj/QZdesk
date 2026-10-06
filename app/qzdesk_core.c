#include "qzdesk_core.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define QZDESK_CORE_DEFAULT_HOST "127.0.0.1"
#define QZDESK_CORE_DEFAULT_CORE_PORT 5678
#define QZDESK_CORE_DEFAULT_GUI_PORT 5679
/* One datagram holds one chat record. The web console can send messages longer
 * than a line of dialog, and a truncated datagram would be dropped as invalid
 * JSON — so receive generously and let the text field do the trimming. */
#define QZDESK_CORE_MESSAGE_MAX 4096
/* 轮询回调之外还能挂几个订阅者：助手页占着轮询，首页也要天气。 */
#define QZDESK_CORE_MAX_SUBSCRIBERS 4

static int qzdesk_core_socket = -1;
static struct sockaddr_in core_address;
static qzdesk_core_event_cb_t subscriber_callback[QZDESK_CORE_MAX_SUBSCRIBERS];
static void *subscriber_data[QZDESK_CORE_MAX_SUBSCRIBERS];
static int subscriber_count;

static unsigned short env_port(const char *name, unsigned short default_value)
{
    const char *value = getenv(name);
    char *end = NULL;
    long port;
    if (!value || value[0] == '\0') return default_value;
    port = strtol(value, &end, 10);
    if (!end || *end != '\0' || port < 1 || port > 65535) return default_value;
    return (unsigned short)port;
}

static bool send_json(const char *json)
{
    if (qzdesk_core_socket < 0 || !json) return false;
    return sendto(qzdesk_core_socket, json, strlen(json), 0,
                  (const struct sockaddr *)&core_address, sizeof(core_address)) >= 0;
}

static const char *json_value(const char *json, const char *key)
{
    static char needle[48];
    const char *value;
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    value = strstr(json, needle);
    if (!value) return NULL;
    value += strlen(needle);
    while (*value && isspace((unsigned char)*value)) value++;
    if (*value != ':') return NULL;
    value++;
    while (*value && isspace((unsigned char)*value)) value++;
    return value;
}

static bool json_string(const char *json, const char *key, char *output, size_t output_size)
{
    const char *value = json_value(json, key);
    size_t length = 0;
    if (!value || *value != '\"' || output_size == 0) return false;
    value++;
    while (*value && *value != '\"' && length + 1 < output_size) {
        if (*value == '\\' && value[1]) {
            value++;
            if (*value == 'n') output[length++] = '\n';
            else if (*value == 'r') output[length++] = '\r';
            else if (*value == 't') output[length++] = '\t';
            else output[length++] = *value;
        } else {
            output[length++] = *value;
        }
        value++;
    }
    output[length] = '\0';
    return *value == '\"';
}

static bool json_int(const char *json, const char *key, int *output)
{
    const char *value = json_value(json, key);
    char *end = NULL;
    long number;
    if (!value) return false;
    number = strtol(value, &end, 10);
    if (end == value) return false;
    *output = (int)number;
    return true;
}

static bool json_double(const char *json, const char *key, double *output)
{
    const char *value = json_value(json, key);
    char *end = NULL;
    double number;
    if (!value) return false;
    number = strtod(value, &end);
    /* JSON 里的 null 取到的是 "null"，strtod 解析不了，正好当成"没有这个数" */
    if (end == value) return false;
    *output = number;
    return true;
}

/** 四舍五入到整数：界面显示温度不需要小数。 */
static int rounded(double value)
{
    return (int)(value < 0 ? value - 0.5 : value + 0.5);
}

/** 读一个可能很大的整数（字节数、秒数），int 装不下时用这个。 */
static bool json_llong(const char *json, const char *key, long long *output)
{
    const char *value = json_value(json, key);
    char *end = NULL;
    long long number;
    if (!value) return false;
    number = strtoll(value, &end, 10);
    if (end == value) return false;
    *output = number;
    return true;
}

/**
 * 把 cursor 处的 JSON 字符串读进 output，并把 cursor 移到字符串之后。
 *
 * 与 `json_string` 共用同一套转义处理：协议里的文本可能带引号或换行。
 */
static bool take_json_string(const char **cursor, char *output, size_t output_size)
{
    const char *value = *cursor;
    size_t length = 0;
    if (!value || *value != '\"' || output_size == 0) return false;
    value++;
    while (*value && *value != '\"' && length + 1 < output_size) {
        if (*value == '\\' && value[1]) {
            value++;
            if (*value == 'n') output[length++] = '\n';
            else if (*value == 'r') output[length++] = '\r';
            else if (*value == 't') output[length++] = '\t';
            else output[length++] = *value;
        } else {
            output[length++] = *value;
        }
        value++;
    }
    output[length] = '\0';
    if (*value != '\"') return false;
    *cursor = value + 1;
    return true;
}

/** 读一个整数数组，例如 `"cores_x10":[120,30]`。读到非数字就停，绝不空转。 */
static int json_int_array(const char *json, const char *key, int *output, int max)
{
    const char *cursor = json_value(json, key);
    int count = 0;
    if (!cursor || *cursor != '[') return 0;
    cursor++;
    while (*cursor && *cursor != ']' && count < max) {
        char *end = NULL;
        long value;
        while (*cursor == ' ' || *cursor == ',' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') {
            cursor++;
            if (*cursor == ']') break;
        }
        if (*cursor == ']' || *cursor == '\0') break;
        value = strtol(cursor, &end, 10);
        if (end == cursor) break; /* null 或格式不符：宁可少读，不要死循环 */
        output[count++] = (int)value;
        cursor = end;
    }
    return count;
}

/** 字符串数组的起点（跳过 '['），没有这个字段时返回 NULL。 */
static const char *json_string_array_start(const char *json, const char *key)
{
    const char *cursor = json_value(json, key);
    if (!cursor || *cursor != '[') return NULL;
    return cursor + 1;
}

/** 依次取字符串数组的下一个元素；取完（或格式不符）返回 false。 */
static bool json_string_array_next(const char **cursor, char *output, size_t output_size)
{
    const char *value = *cursor;
    while (*value == ' ' || *value == ',' || *value == '\t' || *value == '\n' || *value == '\r') value++;
    if (*value == ']' || *value == '\0') return false;
    if (*value != '\"') return false;
    if (!take_json_string(&value, output, output_size)) return false;
    *cursor = value;
    return true;
}

/** 指标等级：核心已判好阈值，界面只管按它选颜色与文字。 */
static qzdesk_level_t level_from(const char *text)
{
    if (!text) return QZDESK_LEVEL_UNKNOWN;
    if (strcmp(text, "ok") == 0) return QZDESK_LEVEL_OK;
    if (strcmp(text, "warn") == 0) return QZDESK_LEVEL_WARN;
    if (strcmp(text, "critical") == 0) return QZDESK_LEVEL_CRITICAL;
    return QZDESK_LEVEL_UNKNOWN;
}

static qzdesk_service_t service_from(const char *text)
{
    if (!text) return QZDESK_SERVICE_UNKNOWN;
    if (strcmp(text, "running") == 0) return QZDESK_SERVICE_RUNNING;
    if (strcmp(text, "stopped") == 0) return QZDESK_SERVICE_STOPPED;
    if (strcmp(text, "disabled") == 0) return QZDESK_SERVICE_DISABLED;
    return QZDESK_SERVICE_UNKNOWN;
}

/** 读一个短字符串并交给枚举映射用（读不到返回 NULL）。 */
static const char *short_field(const char *json, const char *key, char *buffer, size_t size)
{
    return json_string(json, key, buffer, size) ? buffer : NULL;
}

bool qzdesk_core_open(void)
{
    const char *host = getenv("QZDESK_CORE_HOST");
    struct sockaddr_in gui_address;
    int flags;
    if (qzdesk_core_socket >= 0) return true;
    if (!host || host[0] == '\0') host = QZDESK_CORE_DEFAULT_HOST;

    qzdesk_core_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (qzdesk_core_socket < 0) return false;
    memset(&gui_address, 0, sizeof(gui_address));
    gui_address.sin_family = AF_INET;
    gui_address.sin_addr.s_addr = htonl(INADDR_ANY);
    gui_address.sin_port = htons(env_port("QZDESK_CORE_GUI_PORT", QZDESK_CORE_DEFAULT_GUI_PORT));
    if (bind(qzdesk_core_socket, (const struct sockaddr *)&gui_address, sizeof(gui_address)) < 0) {
        close(qzdesk_core_socket);
        qzdesk_core_socket = -1;
        return false;
    }
    flags = fcntl(qzdesk_core_socket, F_GETFL, 0);
    if (flags < 0 || fcntl(qzdesk_core_socket, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(qzdesk_core_socket);
        qzdesk_core_socket = -1;
        return false;
    }
    memset(&core_address, 0, sizeof(core_address));
    core_address.sin_family = AF_INET;
    core_address.sin_port = htons(env_port("QZDESK_CORE_PORT", QZDESK_CORE_DEFAULT_CORE_PORT));
    if (inet_pton(AF_INET, host, &core_address.sin_addr) != 1) {
        close(qzdesk_core_socket);
        qzdesk_core_socket = -1;
        return false;
    }
    return true;
}

bool qzdesk_core_request_status(void)
{
    return send_json("{\"type\":\"status_request\"}");
}

bool qzdesk_core_request_history(void)
{
    return send_json("{\"type\":\"chat_history_request\"}");
}

bool qzdesk_core_subscribe(qzdesk_core_event_cb_t callback, void *user_data)
{
    int index;
    if (!callback) return false;
    /* 同一个回调重复注册就直接认作已注册（界面重建不会把表撑满） */
    for (index = 0; index < subscriber_count; index++) {
        if (subscriber_callback[index] == callback && subscriber_data[index] == user_data) {
            return true;
        }
    }
    if (subscriber_count >= QZDESK_CORE_MAX_SUBSCRIBERS) return false;
    subscriber_callback[subscriber_count] = callback;
    subscriber_data[subscriber_count] = user_data;
    subscriber_count++;
    return true;
}

bool qzdesk_core_request_weather(void)
{
    return send_json("{\"type\":\"weather_request\"}");
}

bool qzdesk_core_request_performance(void)
{
    return send_json("{\"type\":\"performance_request\"}");
}

bool qzdesk_core_refresh_weather(void)
{
    return send_json("{\"type\":\"weather_refresh\"}");
}

/** 一条事件送给轮询回调与所有订阅者：谁都能看到，但谁也不会把别人的包吃掉。 */
static void deliver(const qzdesk_core_event_t *event, qzdesk_core_event_cb_t callback,
                    void *user_data)
{
    int index;
    if (callback) callback(event, user_data);
    for (index = 0; index < subscriber_count; index++) {
        subscriber_callback[index](event, subscriber_data[index]);
    }
}

bool qzdesk_core_send_text(const char *text)
{
    char json[512];
    int written;
    if (!text || text[0] == '\0') return false;

    /* The cloud protocol only accepts audio as user input, so a typed message
     * cannot go out as-is: the core synthesises it locally (sherpa-onnx) and
     * sends the resulting speech through the same Opus path the microphone
     * uses. The GUI's job is to hand the text over — `chat_text` is that
     * hand-off, and the core answers with state messages while it works. */
    written = snprintf(json, sizeof(json),
                       "{\"type\":\"chat_text\",\"text\":\"");
    if (written < 0 || (size_t)written >= sizeof(json)) return false;

    for (const char *cursor = text; *cursor; cursor++) {
        unsigned char ch = (unsigned char)*cursor;
        char escape[8];
        const char *piece = NULL;

        switch (ch) {
        case '"': piece = "\\\""; break;
        case '\\': piece = "\\\\"; break;
        case '\n': piece = "\\n"; break;
        case '\r': piece = "\\r"; break;
        case '\t': piece = "\\t"; break;
        default: break;
        }
        if (!piece) {
            if (ch < 0x20) {
                /* Control characters are not legal in a JSON string; the
                 * keyboard cannot produce them, but a paste could. */
                snprintf(escape, sizeof(escape), "\\u%04x", ch);
                piece = escape;
            } else {
                /* Multi-byte UTF-8 passes through untouched: the text is already
                 * valid UTF-8, and JSON strings are UTF-8. */
                if ((size_t)written + 1 >= sizeof(json)) return false;
                json[written++] = (char)ch;
                continue;
            }
        }
        if ((size_t)written + strlen(piece) >= sizeof(json) - 3) return false;
        written += (int)strlen(piece);
        memcpy(json + written - strlen(piece), piece, strlen(piece));
    }

    if ((size_t)written + 3 >= sizeof(json)) return false;
    json[written++] = '"';
    json[written++] = '}';
    json[written] = '\0';
    return send_json(json);
}

bool qzdesk_core_notify_presence(bool present)
{
    /* 有人靠近 / 离开。界面不自己播报，交给核心决定要不要让助手打个招呼 ——
     * 这样问候语会和其他消息一样进聊天记录，网页控制台也看得到。 */
    return send_json(present ? "{\"type\":\"presence\",\"present\":true}"
                             : "{\"type\":\"presence\",\"present\":false}");
}

static void dispatch_message(const char *json, qzdesk_core_event_cb_t callback, void *user_data)
{
    char type[24] = "";
    qzdesk_core_event_t event = {0};
    int state;
    if (json_int(json, "state", &state)) {
        event.type = QZDESK_CORE_EVENT_STATUS;
        event.state = state;
        deliver(&event, callback, user_data);
    }
    if (!json_string(json, "type", type, sizeof(type))) return;
    if (strcmp(type, "chat") == 0) {
        event.type = QZDESK_CORE_EVENT_CHAT;
        /* user 是用户自己说的（气泡贴右侧），assistant / system 贴左侧。 */
        json_string(json, "role", event.role, sizeof(event.role));
    } else if (strcmp(type, "toast") == 0) {
        event.type = QZDESK_CORE_EVENT_TOAST;
    } else if (strcmp(type, "activation") == 0) {
        event.type = QZDESK_CORE_EVENT_ACTIVATION;
    } else if (strcmp(type, "weather") == 0) {
        /* 天气：核心已经把数值取整、把单位统一好，这里只做搬运。 */
        double number;
        qzdesk_core_weather_t *weather = &event.weather;
        event.type = QZDESK_CORE_EVENT_WEATHER;
        json_string(json, "status", weather->status, sizeof(weather->status));
        json_string(json, "city", weather->city, sizeof(weather->city));
        json_string(json, "text", weather->text, sizeof(weather->text));
        json_string(json, "icon", weather->icon, sizeof(weather->icon));
        json_string(json, "updated", weather->updated, sizeof(weather->updated));
        json_string(json, "message", weather->message, sizeof(weather->message));
        /* 有温度才代表这份快照带真实数据；其余字段缺了就保持 0。 */
        /* 0 是合法值（0% 降水、夜里的紫外线就是 0）：用 -1 表示「字段没来」 */
        weather->precipitation = -1;
        weather->uv_x10 = -1;
        if (json_double(json, "temperature", &number)) {
            weather->has_data = true;
            weather->temperature = rounded(number);
        }
        if (json_double(json, "apparent", &number)) weather->apparent = rounded(number);
        if (json_double(json, "humidity", &number)) weather->humidity = rounded(number);
        if (json_double(json, "high", &number)) weather->high = rounded(number);
        if (json_double(json, "low", &number)) weather->low = rounded(number);
        if (json_double(json, "precipitation", &number)) weather->precipitation = rounded(number);
        if (json_double(json, "uv", &number)) weather->uv_x10 = rounded(number * 10.0);
        json_string(json, "sunrise", weather->sunrise, sizeof(weather->sunrise));
        json_string(json, "sunset", weather->sunset, sizeof(weather->sunset));
        if (json_double(json, "wind", &number)) weather->wind_x10 = rounded(number * 10.0);
        if (json_double(json, "code", &number)) weather->code = rounded(number);
    } else if (strcmp(type, "performance") == 0) {
        /* 性能：核心已经把等级判好、百分比 ×10 整型化，而且字段名就是为设备
         * 准备的扁平结构（键名唯一、缺的指标给 null），这里只做搬运。 */
        qzdesk_core_performance_t *performance = &event.performance;
        char level[16];
        long long wide;
        int value;

        event.type = QZDESK_CORE_EVENT_PERFORMANCE;
        if (json_llong(json, "timestamp", &wide)) performance->timestamp = (unsigned long long)wide;
        if (json_llong(json, "uptime_secs", &wide)) performance->uptime_secs = (unsigned long long)wide;

        if (json_int(json, "cpu_x10", &value)) performance->cpu_x10 = value;
        performance->cpu_level = level_from(short_field(json, "cpu_level", level, sizeof(level)));
        performance->core_count = json_int_array(json, "cores_x10", performance->core_x10,
                                                 QZDESK_PERFORMANCE_CORES);

        if (json_int(json, "memory_x10", &value)) performance->memory_x10 = value;
        performance->memory_level = level_from(short_field(json, "memory_level", level, sizeof(level)));
        if (json_llong(json, "memory_total_kb", &wide)) performance->memory_total_kb = (unsigned long long)wide;
        if (json_llong(json, "memory_used_kb", &wide)) performance->memory_used_kb = (unsigned long long)wide;
        if (json_llong(json, "memory_available_kb", &wide)) performance->memory_available_kb = (unsigned long long)wide;
        if (json_llong(json, "swap_total_kb", &wide)) performance->swap_total_kb = (unsigned long long)wide;
        if (json_llong(json, "swap_used_kb", &wide)) performance->swap_used_kb = (unsigned long long)wide;

        if (json_int(json, "storage_x10", &value)) performance->storage_x10 = value;
        performance->storage_level = level_from(short_field(json, "storage_level", level, sizeof(level)));
        if (json_llong(json, "storage_total_kb", &wide)) performance->storage_total_kb = (unsigned long long)wide;
        if (json_llong(json, "storage_used_kb", &wide)) performance->storage_used_kb = (unsigned long long)wide;
        if (json_llong(json, "storage_available_kb", &wide)) performance->storage_available_kb = (unsigned long long)wide;

        /* 不可用的指标在协议里是 null：读不到就保持 has_* = false，界面据此
         * 显示「不可用」，与网页同一种表达。 */
        performance->has_temperature = json_int(json, "temperature_x10", &value);
        if (performance->has_temperature) performance->temperature_x10 = value;
        performance->temperature_level = level_from(short_field(json, "temperature_level", level, sizeof(level)));

        performance->has_wifi = json_int(json, "wifi_dbm", &value);
        if (performance->has_wifi) performance->wifi_dbm = value;
        performance->wifi_level = level_from(short_field(json, "wifi_level", level, sizeof(level)));

        performance->has_latency = json_int(json, "latency_x10", &value);
        if (performance->has_latency) performance->latency_x10 = value;
        performance->latency_level = level_from(short_field(json, "latency_level", level, sizeof(level)));

        json_int_array(json, "load_x100", performance->load_x100, 3);
        performance->core_service = service_from(short_field(json, "core_service", level, sizeof(level)));
        performance->ui_service = service_from(short_field(json, "ui_service", level, sizeof(level)));
        performance->audio_service = service_from(short_field(json, "audio_service", level, sizeof(level)));

        performance->history_count = json_int_array(json, "history_x10", performance->history_x10,
                                                    QZDESK_PERFORMANCE_HISTORY);
        {
            const char *cursor = json_string_array_start(json, "process_text");
            while (cursor && performance->process_count < QZDESK_PERFORMANCE_PROCESSES &&
                   json_string_array_next(&cursor,
                                          performance->process_text[performance->process_count],
                                          QZDESK_PERFORMANCE_PROCESS_TEXT)) {
                performance->process_count++;
            }
        }
    } else {
        return;
    }
    if (event.type == QZDESK_CORE_EVENT_ACTIVATION) json_string(json, "code", event.text, sizeof(event.text));
    else if (event.type == QZDESK_CORE_EVENT_CHAT || event.type == QZDESK_CORE_EVENT_TOAST) {
        json_string(json, "text", event.text, sizeof(event.text));
    }
    /* 天气与性能自己没有 text 字段（或为空），不能按"空文本"丢掉。 */
    if (event.type == QZDESK_CORE_EVENT_WEATHER || event.type == QZDESK_CORE_EVENT_PERFORMANCE ||
        event.text[0] != '\0') {
        deliver(&event, callback, user_data);
    }
}

void qzdesk_core_poll(qzdesk_core_event_cb_t callback, void *user_data)
{
    char message[QZDESK_CORE_MESSAGE_MAX];
    int received;
    int count = 0;
    if (qzdesk_core_socket < 0) return;
    while (count++ < 8) {
        received = (int)recvfrom(qzdesk_core_socket, message, sizeof(message) - 1, 0, NULL, NULL);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            return;
        }
        message[received] = '\0';
        dispatch_message(message, callback, user_data);
    }
}

void qzdesk_core_close(void)
{
    if (qzdesk_core_socket >= 0) close(qzdesk_core_socket);
    qzdesk_core_socket = -1;
}
