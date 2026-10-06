
#ifndef QZDESK_CAMERA_PREVIEW_H
#define QZDESK_CAMERA_PREVIEW_H

#include <stdbool.h>
#include <stdint.h>

/* 运动相机模式的取景数据源。
 *
 * 与 face_camera（存在检测）走的是同一类 V4L2 采集，但目的不同：那边只要
 * 亮度网格做帧差，这边要**彩色预览帧** —— 驱动给出的 NV12/YUYV 逐像素转成
 * RGB565、缩放到面板尺寸，放进三缓冲供界面（LVGL image 控件）轮流取用。
 *
 * 两边共享 /dev/videoN，不能同时开。运动相机页打开时会先停掉存在检测、
 * 退出时再恢复（applets.c 的相机页负责这个交接）。
 *
 * 没有摄像头（开发机、CI）时可用测试后端：QZDESK_PREVIEW_FAKE=1 会生成一个
 * 会动的彩条画面，整条「取帧 → 上屏 → 快门存图」链路都能在模拟器里验证。
 *
 * 回调与帧都在**工作线程**上产生；界面侧通过 qz_cam_preview_frame() 在
 * LVGL 定时器里取，两边用锁交接，缓冲在下次取帧前保持稳定。 */

/** 有没有可用的取景来源：真实摄像头，或显式打开的测试后端。 */
bool qz_cam_preview_available(void);

/** 启动取景。已在跑时返回 false。失败原因看 qz_cam_preview_status()。 */
bool qz_cam_preview_start(void);

/** 停止取景并释放摄像头。没在跑时是空操作。 */
void qz_cam_preview_stop(void);

/** 正在运行吗。 */
bool qz_cam_preview_running(void);

/** 后端名字（`V4L2 /dev/video0 640x480 NV12`、`测试后端`、`未启动`…）。 */
const char *qz_cam_preview_backend(void);

/** 一行状态说明：为什么不可用，或当前跑到哪一步。 */
const char *qz_cam_preview_status(void);

/**
 * 取最新一帧（面板尺寸 RGB565，`qz_panel_w × qz_panel_h`）。
 *
 * `*id` 返回帧序号；与上次相同表示这帧已经取过、没有新内容。返回的缓冲
 * 在下一次调用前不会被改写，可以放心交给 lv_image 渲染。没有可用帧时
 * 返回 NULL（还没出第一帧 / 没在跑）。
 */
const uint8_t *qz_cam_preview_frame(uint32_t *id);

/** 把最近一帧存成 24 位 BMP。失败原因写进 qz_cam_preview_status()。 */
bool qz_cam_preview_snapshot(char *path, unsigned int path_size);

/** 本次启动以来拍下的照片数（重启清零；只计数，不扫描目录）。 */
int qz_cam_preview_photos(void);

#endif
