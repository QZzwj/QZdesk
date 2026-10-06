#ifndef QZDESK_WEB_CLIENT_H
#define QZDESK_WEB_CLIENT_H

#include <stdbool.h>
#include <stddef.h>

/* 本机核心的 HTTP 客户端。
 *
 * GUI 不直接碰网络协议：Skill 控制台、智能家居设备表都长在核心的本地服务
 * （默认 127.0.0.1:8080）上，界面只发几次很小的请求把 JSON 拿回来。请求集很小
 * （GET/POST/PUT + JSON 体），所以这里是手写实现而不是引依赖。
 */

/** 发一次请求，把响应体整段读进 `output`。返回 false 表示服务不可达或非 2xx。 */
bool qz_web_request(const char *method, const char *path, const char *body,
                    char *output, size_t output_size);

/** 读服务端口（QZDESK_SKILL_WEB_PORT / XIAOZHI_SKILL_WEB_PORT，默认 8080）。 */
unsigned short qz_web_port(void);

/* ------------------------------------------------------------------------- *
 * 极简 JSON 读取
 *
 * 响应都是小而扁平的结构（设备表、Skill 表），不需要通用解析器；一个按键找值
 * 的扫描器就够，也省得为它引入依赖。
 * ------------------------------------------------------------------------- */

/** 找到 `"key"` 之后的值起点（跳过冒号与空白），找不到返回 NULL。 */
const char *qz_json_value(const char *json, const char *key);
/** 读字符串值并反转义（\n \t \r \uXXXX）。 */
bool qz_json_string(const char *json, const char *key, char *output, size_t output_size);
bool qz_json_int(const char *json, const char *key, int *output);
/** 读布尔值：支持 true/false 与 0/1。 */
bool qz_json_bool(const char *json, const char *key, bool *output);

/** 取 `"key"` 数组里的第 index 个对象（大括号配对；字符串里的括号不参与）。
 *  返回该对象的起点或 NULL。 */
const char *qz_json_item(const char *json, const char *key, int index);

#endif
