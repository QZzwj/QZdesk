#ifndef QZDESK_SMARTHOME_H
#define QZDESK_SMARTHOME_H

#include <stdbool.h>

/* 界面侧的智能家居数据访问：设备表长在核心的本地服务上（/api/devices），
 * 界面只负责取回来显示、把点击变成一条指令。
 *
 * 核心才是唯一的事实来源 —— 云端 AI、网页控制台与这块屏幕看到的是同一份状态，
 * 所以这里不做缓存、不做本地推断，按下按钮就下发并重新取一次。
 */

#define QZ_DEVICE_MAX 12
#define QZ_DEVICE_ID_MAX 40
#define QZ_DEVICE_NAME_MAX 32

typedef struct {
    char id[QZ_DEVICE_ID_MAX];
    char name[QZ_DEVICE_NAME_MAX];
    bool is_light;
    bool online;
    /** 还没有上报过状态时为 false，此时界面上显示 "--"。 */
    bool has_state;
    bool state;
    /** 亮度百分比（0-100），仅灯有意义。 */
    int brightness;
} qz_device_t;

typedef struct {
    /** 中枢是否启用（未启用时设备列表只可能是手写配置里的那些）。 */
    bool enabled;
    /** MQTT 是否连上 broker。 */
    bool connected;
    /** 设备总数（可能多于取回来的条数）。 */
    int declared;
    char broker[48];
    char error[64];
} qz_hub_status_t;

/** 取设备表。返回设备条数，-1 表示本地服务不可达。 */
int qz_devices_fetch(qz_device_t *output, int max_count, qz_hub_status_t *status);

/** 下发一条指令：action 取 "on" / "off" / "toggle" / "brightness"。
 *  value 只在 brightness 时有意义（0-100）。 */
bool qz_device_command(const char *id, const char *action, int value);

/** 让核心重新向 zigbee2mqtt 要一次设备树（新配对设备后调用）。 */
bool qz_devices_discover(void);

#endif
