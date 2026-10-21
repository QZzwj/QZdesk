/* 小智标准表情（GIF）资源接口 —— 生成物，见 tools/build_otto_emoji.py。
 *
 * 素材：txp666/otto-emoji-gif-component（MIT）。21 个表情里取用 15 个，按
 * 面板（320 / 480）与界面调用点的设计尺寸各烘一份 1:1 资源。
 */
#ifndef QZ_OTTO_EMOJI_H
#define QZ_OTTO_EMOJI_H

#include "lvgl.h"
#include "ai_face.h"

/** 尺寸档：面板宽度 + 设计尺寸（480×320 设计稿里的像素）。 */
typedef enum {
    QZ_OTTO_320_40,
    QZ_OTTO_320_60,
    QZ_OTTO_320_76,
    QZ_OTTO_320_160,
    QZ_OTTO_480_40,
    QZ_OTTO_480_60,
    QZ_OTTO_480_76,
    QZ_OTTO_480_160,
    QZ_OTTO_SIZE_COUNT
} qz_otto_size_t;

/** 该档位的边长（物理像素）：容器按它定尺寸，图案 1:1 不再缩放。 */
int32_t qz_otto_px(qz_otto_size_t size);

/** 面板宽度 + 设计像素 -> 尺寸档（与 scale.h 的 qz_scale_px 同一套四舍五入）。 */
qz_otto_size_t qz_otto_size_for(int32_t panel_w, int32_t design_px);

/** 取某个状态在某个尺寸档下的 GIF 描述符（内存源，直接喂 lv_gif）。 */
const lv_image_dsc_t *qz_otto_emoji(qz_face_state_t state, qz_otto_size_t size);

/** 状态对应的表情名（neutral / happy / thinking …），调试与日志用。 */
const char *qz_otto_emoji_name(qz_face_state_t state);

#endif /* QZ_OTTO_EMOJI_H */
