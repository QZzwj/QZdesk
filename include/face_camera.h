#ifndef QZDESK_FACE_CAMERA_H
#define QZDESK_FACE_CAMERA_H

#include <stdbool.h>

/* 摄像头与「有人靠近」检测。
 *
 * 它只为两件自动化行为服务：有人靠近时把屏幕点亮、并让助手打个招呼（界面侧见
 * pages/applets.c 的人脸页，核心侧见 controller 里对 presence 报文的处理）。
 *
 * 判定「有没有人」不需要识别是谁，所以默认后端是 **V4L2 抓帧 + 帧间差分**：不要
 * 模型、不要额外运行库，任何能出 YUV 的 Linux 摄像头都能跑。设备侧若接上 RKNN
 * 人脸模型（SDK 里已有 librknnmrt），把结果接到同一个回调即可，界面与核心都不用
 * 改。
 *
 * 回调在**工作线程**上触发：只做标记（写一个 volatile / 队列），不要在其中碰
 * LVGL 对象，也不要在里面做会阻塞的事。界面侧的用法见 pages/applets.c。 */

typedef enum {
    QZ_PRESENCE_ARRIVE, /**< 从「没人」变成「有人」 */
    QZ_PRESENCE_LEAVE,  /**< 持续一段时间没人 */
} qz_presence_event_t;

typedef void (*qz_presence_cb_t)(qz_presence_event_t event, void *user_data);

/** 有没有可用的采集来源：真实摄像头，或显式打开的测试后端。 */
bool qz_face_camera_available(void);

/** 启动检测。`callback` 可以为 NULL（只更新内部状态）。已在跑时返回 false。 */
bool qz_face_camera_start(qz_presence_cb_t callback, void *user_data);

/** 停止检测并释放摄像头。没在跑时是空操作。 */
void qz_face_camera_stop(void);

/** 后端名字（`V4L2 /dev/video0`、`测试后端`、`未启动`…），页面直接显示。 */
const char *qz_face_camera_backend(void);

/** 一行状态说明：为什么不可用、或者当前有没有人。 */
const char *qz_face_camera_status(void);

/** 正在运行吗。 */
bool qz_face_camera_running(void);

/** 当前是否检测到有人。 */
bool qz_face_camera_present(void);

/** 本次启动以来「有人靠近」的次数。 */
int qz_face_camera_arrivals(void);

/** 最近一次检测到人的时刻（Unix 秒）；从没检测到为 0。 */
unsigned long long qz_face_camera_last_seen(void);

#endif
