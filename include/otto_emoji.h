/* 小智标准表情（GIF）资源接口 —— 生成物，见 tools/build_otto_emoji.py。
 *
 * 素材：txp666/otto-emoji-gif-component（MIT）。21 个表情里取用 10 个，
 * 按界面上的三处脸缩放到 60/88/164。
 */
#ifndef QZ_OTTO_EMOJI_H
#define QZ_OTTO_EMOJI_H

#include "lvgl.h"
#include "ai_face.h"

/** 尺寸档：跟界面上的三处脸一一对应。 */
typedef enum {
    QZ_OTTO_60 = 0,    /**< 关于卡片（60px） */
    QZ_OTTO_88 = 1,    /**< 主页大卡片（88px） */
    QZ_OTTO_164 = 2,   /**< 全屏表情（164px） */
    QZ_OTTO_SIZE_COUNT
} qz_otto_size_t;

/** 该档位的边长（物理像素）：容器按它定尺寸，图案 1:1 不再缩放。 */
int32_t qz_otto_px(qz_otto_size_t size);

/** 取某个状态在某个尺寸档下的 GIF 描述符（内存源，直接喂 lv_gif）。 */
const lv_image_dsc_t *qz_otto_emoji(qz_face_state_t state, qz_otto_size_t size);

/** 状态对应的表情名（neutral / happy / thinking …），调试与日志用。 */
const char *qz_otto_emoji_name(qz_face_state_t state);

#endif /* QZ_OTTO_EMOJI_H */
