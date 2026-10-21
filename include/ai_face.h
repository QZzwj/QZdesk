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
    QZ_FACE_SURPRISED,/* 惊讶 */
    QZ_FACE_SLEEPY,   /* 困倦 */
    QZ_FACE_WINK,     /* 单眼眨 */
    QZ_FACE_EXCITED,  /* 兴奋 */
    QZ_FACE_RELAXED,  /* 放松 */
    QZ_FACE_CONFIDENT,/* 自信 */
    QZ_FACE_EMBARRASSED, /* 害羞 */
    QZ_FACE_PLAYFUL,  /* 调皮 */
    QZ_FACE_SAD,      /* 低落 */
    QZ_FACE_STATE_COUNT,
} qz_face_state_t;

/** Build the aora-bot ball in a square of `size` design pixels.
 *
 * The face is drawn by LVGL at the physical panel resolution. It does not load
 * GIF frames or image files, so every size stays crisp and the animation can
 * interpolate between expressions instead of swapping frames. */
lv_obj_t *qz_face_create(lv_obj_t *parent, int32_t size);
/** Switch to one of the built-in states. Repeated calls are ignored. */
void qz_face_set_state(lv_obj_t *face, qz_face_state_t state);
/** True if `emotion_id` is one of the supported protocol IDs or an alias. */
bool qz_face_emotion_valid(const char *emotion_id);
/**
 * Apply a protocol emotion (`00`-`07`, `10`-`21`, `30`-`41`).
 *
 * The cloud may also use names such as `happy` or `thinking`; common aliases
 * are accepted. Invalid IDs leave the current expression untouched.
 */
void qz_face_set_emotion(lv_obj_t *face, const char *emotion_id);
/** Stable legacy state used by callers that also maintain a caption. */
qz_face_state_t qz_face_emotion_state(const char *emotion_id);
/** Short caption for an emotion ID; empty string when the ID is invalid. */
const char *qz_face_emotion_text(const char *emotion_id);
/** Short Chinese caption describing the state. */
const char *qz_face_state_text(qz_face_state_t state);

#endif
