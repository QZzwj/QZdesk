/* 本机核心 HTTP 服务的极小客户端 + 扁平 JSON 读取。
 *
 * 这份实现原本长在 app/skills.c 里（Skill 控制台要读 /api/skills）。智能家居的
 * 设备表要走同一个服务、同一套扫描逻辑，于是抽出来共用，而不是再抄一遍 ——
 * 抄一遍的代价是两个地方各修一遍 bug。 */
#include "web_client.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define REQUEST_MAX 4096
#define RESPONSE_MAX 16384

unsigned short qz_web_port(void)
{
    static unsigned short cached;
    const char *value;
    char *end = NULL;
    long port;

    if (cached) return cached;
    value = getenv("QZDESK_SKILL_WEB_PORT");
    if (!value || value[0] == '\0') value = getenv("XIAOZHI_SKILL_WEB_PORT");
    port = value && value[0] != '\0' ? strtol(value, &end, 10) : 0;
    if (!end || *end != '\0' || port < 1 || port > 65535) port = 8080;
    cached = (unsigned short)port;
    return cached;
}

bool qz_web_request(const char *method, const char *path, const char *body,
                    char *output, size_t output_size)
{
    struct sockaddr_in address;
    int socket_fd;
    char request[REQUEST_MAX];
    char response[RESPONSE_MAX];
    size_t used = 0;
    ssize_t received;
    char *header_end;
    int status;
    int length;

    if (output_size == 0) return false;
    output[0] = '\0';

    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) return false;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(qz_web_port());
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    /* 服务就在本机，短超时足够区分"没在跑"和"慢"；界面不能卡在这里等。 */
    struct timeval timeout = { 2, 0 };
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if (connect(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(socket_fd);
        return false;
    }

    length = snprintf(request, sizeof(request),
                      "%s %s HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                      "Content-Type: application/json; charset=utf-8\r\n"
                      "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                      method, path, body ? strlen(body) : 0, body ? body : "");
    if (length <= 0 || (size_t)length >= sizeof(request)) {
        close(socket_fd);
        return false;
    }
    if (send(socket_fd, request, (size_t)length, 0) != length) {
        close(socket_fd);
        return false;
    }

    while (used + 1 < sizeof(response) &&
           (received = recv(socket_fd, response + used, sizeof(response) - 1 - used, 0)) > 0) {
        used += (size_t)received;
    }
    close(socket_fd);
    response[used] = '\0';

    header_end = strstr(response, "\r\n\r\n");
    if (!header_end) return false;
    if (strncmp(response, "HTTP/1.", 7) != 0) return false;
    status = atoi(response + 9);
    if (status < 200 || status >= 300) return false;

    /* 只要响应体；列表被截断也比什么都没有强 */
    snprintf(output, output_size, "%s", header_end + 4);
    return true;
}

const char *qz_json_value(const char *json, const char *key)
{
    static char needle[64];
    const char *value;
    if (!json || !key) return NULL;
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

bool qz_json_string(const char *json, const char *key, char *output, size_t output_size)
{
    const char *value = qz_json_value(json, key);
    size_t length = 0;
    if (!value || *value != '"' || output_size == 0) return false;
    value++;
    while (*value && *value != '"' && length + 1 < output_size) {
        if (*value == '\\' && value[1]) {
            value++;
            if (*value == 'n') output[length++] = '\n';
            else if (*value == 't') output[length++] = '\t';
            else if (*value == 'r') output[length++] = '\r';
            else if (*value == 'u') {
                /* \uXXXX：界面上一行说明够用，不为此实现完整转码 */
                output[length++] = '?';
                for (int skip = 0; skip < 4 && value[1]; skip++) value++;
            } else {
                output[length++] = *value;
            }
        } else {
            output[length++] = *value;
        }
        value++;
    }
    output[length] = '\0';
    return *value == '"';
}

bool qz_json_int(const char *json, const char *key, int *output)
{
    const char *value = qz_json_value(json, key);
    char *end = NULL;
    long number;
    if (!value) return false;
    number = strtol(value, &end, 10);
    if (end == value) return false;
    *output = (int)number;
    return true;
}

bool qz_json_bool(const char *json, const char *key, bool *output)
{
    const char *value = qz_json_value(json, key);
    int number = 0;
    if (!value) return false;
    if (strncmp(value, "true", 4) == 0) {
        *output = true;
        return true;
    }
    if (strncmp(value, "false", 5) == 0) {
        *output = false;
        return true;
    }
    if (qz_json_int(json, key, &number)) {
        *output = number != 0;
        return true;
    }
    return false;
}

const char *qz_json_item(const char *json, const char *key, int index)
{
    const char *array = qz_json_value(json, key);
    const char *cursor;
    int depth = 0;
    int found = -1;
    bool in_string = false;

    if (!array) return NULL;
    cursor = strchr(array, '[');
    if (!cursor) return NULL;
    cursor++;
    for (; *cursor; cursor++) {
        if (in_string) {
            if (*cursor == '\\' && cursor[1]) cursor++;
            else if (*cursor == '"') in_string = false;
            continue;
        }
        if (*cursor == '"') {
            in_string = true;
        } else if (*cursor == '{') {
            if (depth == 0 && ++found == index) return cursor;
            depth++;
        } else if (*cursor == '}') {
            if (depth > 0) depth--;
        } else if (*cursor == ']' && depth == 0) {
            break;
        }
    }
    return NULL;
}
