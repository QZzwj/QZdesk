#ifndef QZDESK_AI_FACE_H
#define QZDESK_AI_FACE_H

#include "lvgl/lvgl.h"

/** Expression states required by the assistant page. */
typedef enum {
    QZ_FACE_IDLE = 0, /* 待机 */
    QZ_FACE_SPEAKING, /* 说话 */
    QZ_FACE_THINKING, /* 思考 */
    QZ_FACE_HAPPY,    /* 开心 */
    QZ_FACE_CONFUSED, /* 困惑 */
    QZ_FACE_LOVE,     /* 抱心（喜欢 / 被夸） */
    QZ_FACE_SURPRISED,/* 惊讶（方波抖动 + 呼吸放大） */
    QZ_FACE_SLEEPY,   /* 困倦（眼皮半闭 + zzz） */
    QZ_FACE_WINK,     /* 单眼眨 + 星光 */
    QZ_FACE_EXCITED,  /* 兴奋（快弹跳 + 满星光） */
    QZ_FACE_STATE_COUNT,
} qz_face_state_t;

/** Build a vector (LVGL object) face that fills a square of `size` pixels. */
lv_obj_t *qz_face_create(lv_obj_t *parent, int32_t size);
/** Switch the expression, restarting the state animation. */
void qz_face_set_state(lv_obj_t *face, qz_face_state_t state);
/** Short Chinese caption describing the state. */
const char *qz_face_state_text(qz_face_state_t state);

#endif
