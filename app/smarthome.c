/* 界面侧的智能家居数据访问：取设备表、下发指令。
 *
 * 核心持有唯一的事实来源（它才是接 MQTT 的那一端），这里只做翻译：
 * JSON → qz_device_t，点击 → 一条 HTTP 指令。所以界面不缓存、不推断状态，
 * 每次操作后重新取一次，屏幕上显示的就是核心里的真实状态。 */
#include "smarthome.h"
#include "web_client.h"
#include <stdio.h>
#include <string.h>

#define RESPONSE_MAX 16384
#define COMMAND_BODY_MAX 256

/** 把字符串按 JSON 规则转义（只处理必须转义的字符，UTF-8 原样通过）。 */
static bool json_escape(const char *input, char *output, size_t output_size)
{
    size_t used = 0;
    for (const char *cursor = input; *cursor; cursor++) {
        unsigned char ch = (unsigned char)*cursor;
        char escape[8];

        if (ch == '"') {
            snprintf(escape, sizeof(escape), "\\\"");
        } else if (ch == '\\') {
            snprintf(escape, sizeof(escape), "\\\\");
        } else if (ch < 0x20) {
            snprintf(escape, sizeof(escape), "\\u%04x", ch);
        } else {
            if (used + 1 >= output_size) return false;
            output[used++] = (char)ch;
            continue;
        }
        size_t length = strlen(escape);
        if (used + length >= output_size) return false;
        memcpy(output + used, escape, length);
        used += length;
    }
    output[used] = '\0';
    return true;
}

int qz_devices_fetch(qz_device_t *output, int max_count, qz_hub_status_t *status)
{
    static char body[RESPONSE_MAX];
    const char *hub;
    int count = 0;
    int declared = 0;

    if (status) memset(status, 0, sizeof(*status));
    if (!output || max_count <= 0) return -1;
    if (!qz_web_request("GET", "/api/devices", NULL, body, sizeof(body))) return -1;

    hub = qz_json_value(body, "hub");
    if (status && hub) {
        bool flag = false;
        status->enabled = qz_json_bool(hub, "enabled", &flag) && flag;
        status->connected = qz_json_bool(hub, "connected", &flag) && flag;
        status->broker[0] = '\0';
        qz_json_string(hub, "broker", status->broker, sizeof(status->broker));
        status->error[0] = '\0';
        qz_json_string(hub, "error", status->error, sizeof(status->error));
    }

    qz_json_int(body, "count", &declared);
    for (int i = 0; i < max_count; i++) {
        const char *item = qz_json_item(body, "devices", i);
        qz_device_t *device;
        char kind[12] = "";
        bool flag = false;
        int brightness = 0;

        if (!item) break;
        device = &output[count];
        memset(device, 0, sizeof(*device));
        if (!qz_json_string(item, "id", device->id, sizeof(device->id))) continue;
        if (!qz_json_string(item, "name", device->name, sizeof(device->name)) ||
            device->name[0] == '\0') {
            snprintf(device->name, sizeof(device->name), "%.*s",
                     (int)sizeof(device->name) - 1, device->id);
        }
        qz_json_string(item, "kind", kind, sizeof(kind));
        device->is_light = strcmp(kind, "light") == 0;
        device->online = qz_json_bool(item, "online", &flag) && flag;
        /* 状态可能还是 null（还没上报过），那时界面显示 "--" */
        if (qz_json_bool(item, "state", &flag)) {
            device->has_state = true;
            device->state = flag;
        }
        if (qz_json_int(item, "brightness", &brightness)) {
            device->brightness = brightness;
        }
        count++;
    }

    if (status) status->declared = declared > 0 ? declared : count;
    return count;
}

bool qz_device_command(const char *id, const char *action, int value)
{
    char escaped[QZ_DEVICE_ID_MAX * 2];
    char body[COMMAND_BODY_MAX];
    static char response[256];

    if (!id || id[0] == '\0' || !action) return false;
    /* 指令是我们自己给的固定几种，先白名单化再拼进 JSON，省掉一层注入担心 */
    if (strcmp(action, "on") != 0 && strcmp(action, "off") != 0 &&
        strcmp(action, "toggle") != 0 && strcmp(action, "brightness") != 0) {
        return false;
    }
    if (!json_escape(id, escaped, sizeof(escaped))) return false;

    if (strcmp(action, "brightness") == 0) {
        int percent = value < 0 ? 0 : (value > 100 ? 100 : value);
        snprintf(body, sizeof(body),
                 "{\"id\":\"%s\",\"action\":\"brightness\",\"value\":%d}",
                 escaped, percent);
    } else {
        snprintf(body, sizeof(body), "{\"id\":\"%s\",\"action\":\"%s\"}", escaped, action);
    }
    return qz_web_request("POST", "/api/devices/command", body, response, sizeof(response));
}

bool qz_devices_discover(void)
{
    static char response[256];
    return qz_web_request("POST", "/api/devices/discover", "{}", response, sizeof(response));
}
