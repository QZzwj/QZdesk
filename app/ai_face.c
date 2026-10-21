/* Native LVGL port of aora-bot's ball (light and dark).
 * Copyright (c) 2026 sam70361. See third_party/aora-bot/LICENSE and NOTICE.md.
 * Geometry and expression tables are generated from the pinned upstream data.
 */
#include "ai_face.h"
#include "scale.h"
#include "theme.h"
#include "src/draw/lv_draw_vector.h"
#include "src/misc/lv_area_private.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FACE_PI 3.14159265359f
#define FACE_C 114.2705f
#define FACE_TICK_MS 16
#define FACE_TEMPO 1.5f
#define FACE_POINTS 48

enum { B_X, B_Y, B_SCALE, B_ROTATE, B_BREATHE, B_RIBBONS,
       B_CONFETTI, B_ZZZ, B_ORBIT, E_LEFT, E_RIGHT = E_LEFT + 8, POSE_N = E_RIGHT + 8 };
enum { E_X, E_Y, E_SX, E_SY, E_ROTATE, E_OPEN, E_LOOK_X, E_LOOK_Y };
enum { AORA_SINE, AORA_PULSE, AORA_JITTER, AORA_SCAN, AORA_GLANCE, AORA_BLINK };

typedef struct { float v[POSE_N]; uint32_t color[3]; } aora_pose_t;
typedef struct {
    uint8_t target, prop, type;
    float amp;
    uint16_t period;
    float phase;
    uint16_t phase_ms;
    float speed;
    uint16_t decay, duration;
    float depth;
} aora_anim_t;
typedef struct { uint16_t at; aora_pose_t pose; } aora_frame_t;
typedef struct {
    const char *id;
    uint16_t transition;
    uint8_t pool[8], pool_count;
    uint16_t pool_ms[2], blink_ms[2];
    float openness;
    bool antics, gaze;
    aora_pose_t base;
    const aora_anim_t *anims;
    uint8_t anim_count;
    const aora_frame_t *frames;
    uint8_t frame_count, settle, next;
} aora_emotion_t;

#include "ai_face_data.h"

typedef struct {
    const char *id;
    qz_face_state_t state;
    const char *text;
} face_emotion_t;

typedef struct {
    lv_obj_t *root;
    lv_timer_t *timer;
    lv_vector_path_t *path;
    aora_pose_t pose, previous;
    lv_fpoint_t ring[2][FACE_POINTS], ring_from[2][FACE_POINTS];
    int emotion, ring_target;
    qz_face_state_t requested_state;
    bool requested_is_remote;
    char requested_id[8];
    uint32_t rng, last_tick;
    float animation_seed;
    float clock, started, ring_started, transition;
    float next_pool, next_blink, blink_started, next_antic, antic_started;
    uint8_t pool_pos, antic_kind;
} qz_face_t;

enum {
    EMO_SLEEP = 0,
    EMO_WAKE,
    EMO_IDLE,
    EMO_CURIOUS,
    EMO_DAZE,
    EMO_BOOT,
    EMO_HIBERNATE,
    EMO_SHIVER,
    EMO_HAPPY,
    EMO_CONFUSED,
    EMO_SAD,
    EMO_SURPRISED,
    EMO_SHY,
    EMO_TIRED,
    EMO_FOCUS,
    EMO_PANIC,
    EMO_RESIGNED,
    EMO_SATISFIED,
    EMO_PUZZLED,
    EMO_ANGRY,
    EMO_THINKING,
    EMO_RECEIVE,
    EMO_BUSY,
    EMO_COMPLETE,
    EMO_ERROR,
    EMO_LISTEN,
    EMO_NETWORK,
    EMO_RECALL,
    EMO_REFUSE,
    EMO_SPEAK,
    EMO_SEARCH,
    EMO_STOP,
    EMO_COUNT,
};

static const face_emotion_t face_emotions[EMO_COUNT] = {
    {"00", QZ_FACE_SLEEPY, "睡着了，做个好梦"},
    {"01", QZ_FACE_THINKING, "正在醒来"},
    {"02", QZ_FACE_IDLE, "我在听你说"},
    {"03", QZ_FACE_WINK, "有点好奇"},
    {"04", QZ_FACE_CONFUSED, "放空中"},
    {"05", QZ_FACE_THINKING, "正在启动"},
    {"06", QZ_FACE_SLEEPY, "休眠模式"},
    {"07", QZ_FACE_SURPRISED, "醒醒，我在"},
    {"10", QZ_FACE_HAPPY, "好开心"},
    {"11", QZ_FACE_CONFUSED, "让我理一理"},
    {"12", QZ_FACE_SAD, "有点失落"},
    {"13", QZ_FACE_SURPRISED, "哇！"},
    {"14", QZ_FACE_LOVE, "被你夸到了"},
    {"15", QZ_FACE_SLEEPY, "有点累"},
    {"16", QZ_FACE_CONFIDENT, "专注中"},
    {"17", QZ_FACE_SURPRISED, "等等，慢一点"},
    {"18", QZ_FACE_SAD, "先这样吧"},
    {"19", QZ_FACE_RELAXED, "很满足"},
    {"20", QZ_FACE_CONFUSED, "嗯？"},
    {"21", QZ_FACE_EXCITED, "有点生气"},
    {"30", QZ_FACE_THINKING, "我在想"},
    {"31", QZ_FACE_THINKING, "收到"},
    {"32", QZ_FACE_THINKING, "正在处理"},
    {"33", QZ_FACE_EXCITED, "完成了"},
    {"34", QZ_FACE_SAD, "出了点问题"},
    {"35", QZ_FACE_IDLE, "我在听"},
    {"36", QZ_FACE_THINKING, "正在连接"},
    {"37", QZ_FACE_THINKING, "让我想想"},
    {"38", QZ_FACE_CONFUSED, "这个先不要"},
    {"39", QZ_FACE_SPEAKING, "正在回答"},
    {"40", QZ_FACE_THINKING, "正在找"},
    {"41", QZ_FACE_RELAXED, "已经停下"},
};

typedef struct {
    const char *alias;
    const char *id;
} face_alias_t;

static const face_alias_t face_aliases[] = {
    {"neutral", "02"}, {"idle", "02"}, {"calm", "02"}, {"online", "02"},
    {"sleep", "00"}, {"sleeping", "00"}, {"wake", "01"}, {"wakeup", "01"},
    {"waking", "01"}, {"curious", "03"}, {"daze", "04"}, {"loading", "36"},
    {"boot", "05"}, {"hibernate", "06"}, {"shiver", "07"},
    {"happy", "10"}, {"smile", "10"}, {"laugh", "10"}, {"confused", "20"},
    {"suspicious", "11"}, {"sad", "12"}, {"cry", "12"}, {"surprised", "13"},
    {"shock", "13"}, {"love", "14"}, {"heart", "14"}, {"shy", "14"},
    {"embarrassed", "14"}, {"tired", "15"}, {"sleepy", "15"},
    {"focus", "16"}, {"focused", "16"}, {"confident", "16"},
    {"panic", "17"}, {"helpless", "18"}, {"resigned", "18"},
    {"satisfied", "19"}, {"relaxed", "19"}, {"puzzled", "20"},
    {"angry", "21"}, {"think", "30"}, {"thinking", "30"},
    {"receive", "31"}, {"receiving", "31"}, {"busy", "32"},
    {"done", "33"}, {"complete", "33"}, {"success", "33"},
    {"error", "34"}, {"failed", "34"}, {"listen", "35"},
    {"listening", "35"}, {"network", "36"}, {"recall", "37"},
    {"reject", "38"}, {"refuse", "38"}, {"speak", "39"},
    {"speaking", "39"}, {"reply", "39"}, {"answering", "39"},
    {"search", "40"}, {"searching", "40"}, {"stop", "41"},
    {"wink", "03"}, {"playful", "03"}, {"excited", "33"},
};

static void normalize_key(const char *source, char *target, size_t size)
{
    size_t used = 0;
    if (!target || size == 0) return;
    target[0] = '\0';
    if (!source) return;

    while (*source && isspace((unsigned char)*source)) source++;
    while (*source && used + 1 < size) {
        unsigned char ch = (unsigned char)*source++;
        if (isspace(ch) || ch == '-' || ch == '_') {
            if (used == 0 || target[used - 1] == '_') continue;
            target[used++] = '_';
        } else {
            target[used++] = (char)tolower(ch);
        }
    }
    while (used > 0 && target[used - 1] == '_') used--;
    target[used] = '\0';
}

static int emotion_index(const char *id)
{
    char key[24];
    size_t i;

    if (!id || !id[0]) return -1;
    normalize_key(id, key, sizeof(key));
    if (!key[0]) return -1;

    for (i = 0; i < EMO_COUNT; i++) {
        if (strcmp(key, face_emotions[i].id) == 0) return (int)i;
    }
    for (i = 0; i < sizeof(face_aliases) / sizeof(face_aliases[0]); i++) {
        if (strcmp(key, face_aliases[i].alias) == 0) {
            for (size_t index = 0; index < EMO_COUNT; index++) {
                if (strcmp(face_aliases[i].id, face_emotions[index].id) == 0)
                    return (int)index;
            }
        }
    }
    return -1;
}

static int emotion_index_for_state(qz_face_state_t state)
{
    switch (state) {
        case QZ_FACE_SLEEPY: return EMO_SLEEP;
        case QZ_FACE_SPEAKING: return EMO_SPEAK;
        case QZ_FACE_THINKING: return EMO_THINKING;
        case QZ_FACE_HAPPY: return EMO_HAPPY;
        case QZ_FACE_CONFUSED: return EMO_CONFUSED;
        case QZ_FACE_LOVE:
        case QZ_FACE_EMBARRASSED: return EMO_SHY;
        case QZ_FACE_SURPRISED: return EMO_SURPRISED;
        case QZ_FACE_WINK:
        case QZ_FACE_PLAYFUL: return EMO_CURIOUS;
        case QZ_FACE_EXCITED: return EMO_COMPLETE;
        case QZ_FACE_RELAXED: return EMO_SATISFIED;
        case QZ_FACE_CONFIDENT: return EMO_FOCUS;
        case QZ_FACE_SAD: return EMO_SAD;
        case QZ_FACE_IDLE:
        default: return EMO_IDLE;
    }
}


static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static float smooth(float v)
{
    v = clampf(v, 0, 1);
    return v * v * (3 - 2 * v);
}

static uint32_t face_rand(qz_face_t *face)
{
    uint32_t value = face->rng;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    face->rng = value;
    return value;
}

static float random_ms(qz_face_t *face, uint32_t lo, uint32_t hi)
{
    return (float)(lo + face_rand(face) % (hi - lo + 1));
}

static uint32_t blend_color(uint32_t a, uint32_t b, float t)
{
    uint32_t color = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
        float start = (float)((a >> shift) & 255);
        float end = (float)((b >> shift) & 255);
        color |= (uint32_t)(clampf(start + (end - start) * t, 0, 255) + 0.5f) << shift;
    }
    return color;
}

static aora_pose_t blend_pose(const aora_pose_t *a, const aora_pose_t *b, float t)
{
    aora_pose_t out;
    for (int i = 0; i < POSE_N; i++) out.v[i] = a->v[i] + (b->v[i] - a->v[i]) * t;
    for (int i = 0; i < 3; i++) out.color[i] = blend_color(a->color[i], b->color[i], t);
    return out;
}

static void target_ring(qz_face_t *face, int index)
{
    memcpy(face->ring_from, face->ring, sizeof(face->ring));
    face->ring_target = index;
    face->ring_started = face->clock;
}

static void start_emotion(qz_face_t *face, int index)
{
    const aora_emotion_t *def = &aora_emotions[index];
    face->previous = face->pose;
    face->emotion = index;
    face->started = face->clock;
    face->transition = fmaxf(def->transition, 420);
    face->pool_pos = 0;
    target_ring(face, def->pool[0]);
    /* Idle stays calm for longer than the original gallery demo. */
    float idle_hold = index == EMO_IDLE ? 1.35f : 1;
    face->next_pool = face->clock + random_ms(face, def->pool_ms[0], def->pool_ms[1]) * idle_hold;
    face->next_blink = face->clock + random_ms(face, def->blink_ms[0], def->blink_ms[1]);
    face->blink_started = -1000;
    face->antic_started = -10000;
    face->next_antic = face->clock + random_ms(face, 13000, 21000);
    if (def->base.v[B_RIBBONS] > 0) {
        face->antic_started = face->clock;
        face->antic_kind = 1;
    }
}

static float animation_value(const aora_anim_t *a, float t, float seed)
{
    float phase = 2 * FACE_PI * t / a->period + a->phase;
    switch (a->type) {
        case AORA_SINE: return a->amp * sinf(phase);
        case AORA_PULSE: return a->amp * 0.5f * (1 - cosf(phase));
        case AORA_GLANCE: return a->amp * tanhf(2.8f * sinf(phase));
        case AORA_SCAN: {
            float p = fmodf(t + a->phase_ms, a->period) / a->period;
            return a->amp * (p < 0.5f ? p * 4 - 1 : 3 - p * 4);
        }
        case AORA_BLINK: {
            float p = fmodf(t + a->phase_ms + seed * 97, a->period);
            return p < a->duration ? -a->depth * sinf(FACE_PI * p / a->duration) : 0;
        }
        case AORA_JITTER: {
            float s = t * a->speed / 1000;
            float value = (sinf(s * 3.1f + seed) + sinf(s * 5.7f + seed * 2.3f) +
                           sinf(s * 9.3f + seed * 4.1f)) * a->amp / 3;
            return value * (a->decay ? clampf(1 - t / a->decay, 0, 1) : 1);
        }
        default: return 0;
    }
}

static aora_pose_t sequence_pose(const aora_emotion_t *def, float t)
{
    if (!def->frame_count) return def->base;
    const aora_frame_t *first = &def->frames[0];
    const aora_frame_t *last = &def->frames[def->frame_count - 1];
    if (t <= first->at) return first->pose;
    if (t >= last->at) {
        if (def->settle != 0) return last->pose;
        return blend_pose(&last->pose, &def->base, smooth((t - last->at) / fmaxf(def->transition, 420)));
    }
    for (int i = 1; i < def->frame_count; i++) {
        const aora_frame_t *a = &def->frames[i - 1], *b = &def->frames[i];
        if (t < b->at) return blend_pose(&a->pose, &b->pose, smooth((t - a->at) / (b->at - a->at)));
    }
    return def->base;
}

static void compose_face(qz_face_t *face, bool motion)
{
    const aora_emotion_t *def = &aora_emotions[face->emotion];
    float t = face->clock - face->started;
    if (def->settle == 2 && def->frame_count &&
        (!motion || t >= def->frames[def->frame_count - 1].at)) {
        start_emotion(face, def->next);
        def = &aora_emotions[face->emotion];
        t = 0;
    }

    aora_pose_t pose = !motion
        ? (def->settle == 1 && def->frame_count ? def->frames[def->frame_count - 1].pose : def->base)
        : sequence_pose(def, t);

    if (motion) {
        float br = pose.v[B_BREATHE];
        float ph = 2 * FACE_PI * face->clock / 3600;
        pose.v[B_SCALE] += br * sinf(ph);
        pose.v[B_Y] += br * 55 * sinf(ph + 0.6f);
        for (int i = 0; i < def->anim_count; i++) {
            const aora_anim_t *a = &def->anims[i];
            float value = animation_value(a, t, face->animation_seed);
            if (a->target == 0) pose.v[a->prop] += value;
            else {
                int first = a->target == 3 ? E_RIGHT : E_LEFT;
                int last = a->target == 2 ? E_LEFT : E_RIGHT;
                for (int eye = first; eye <= last; eye += 8) {
                    if (a->prop == 8) {
                        pose.v[eye + E_SX] += value;
                        pose.v[eye + E_SY] += value;
                    } else pose.v[eye + a->prop] += value;
                }
            }
        }
        if (def->gaze) {
            float w = face->clock / 1000;
            for (int k = 0; k < 2; k++) {
                int eye = k ? E_RIGHT : E_LEFT;
                pose.v[eye + E_LOOK_X] += 1.4f * sinf(0.42f * w + k) + 0.5f * sinf(w + k * 2);
                pose.v[eye + E_LOOK_Y] += 0.9f * sinf(0.58f * w + k);
            }
        }
        if (face->clock >= face->next_pool && def->pool_count > 1) {
            face->pool_pos = (face->pool_pos + 1) % def->pool_count;
            target_ring(face, def->pool[face->pool_pos]);
            face->next_pool = face->clock + random_ms(face, def->pool_ms[0], def->pool_ms[1]);
        }
        if (def->blink_ms[0] && face->clock >= face->next_blink) {
            face->blink_started = face->clock;
            face->next_blink = face->clock + random_ms(face, def->blink_ms[0], def->blink_ms[1]);
        }
        if (def->antics && face->clock >= face->next_antic) {
            face->antic_started = face->clock;
            face->antic_kind = face_rand(face) % 3;
            face->next_antic = face->clock + random_ms(face, 13000, 21000);
        }
        float blink_t = (face->clock - face->blink_started) / 180;
        float open = def->openness * (1 - (blink_t >= 0 && blink_t < 1 ? sinf(FACE_PI * blink_t) : 0));
        pose.v[E_LEFT + E_OPEN] *= open;
        pose.v[E_RIGHT + E_OPEN] *= open;
        float antic = face->clock - face->antic_started;
        if (antic >= 0 && antic < 1400) {
            float p = antic / 1400;
            if (face->antic_kind == 0) pose.v[B_Y] -= 12 * sinf(FACE_PI * p) * sinf(FACE_PI * p);
            if (face->antic_kind == 2) pose.v[E_RIGHT + E_OPEN] *= 1 - sinf(FACE_PI * p);
        }
        if (t < face->transition) pose = blend_pose(&face->previous, &pose, smooth(t / face->transition));
    } else {
        pose.v[E_LEFT + E_OPEN] *= def->openness;
        pose.v[E_RIGHT + E_OPEN] *= def->openness;
    }

    float rt = fmaxf(0, (face->clock - face->ring_started) / 1000);
    float progress = motion ? 1 - (1 + 7 * rt) * expf(-7 * rt) : 1;
    for (int k = 0; k < 2; k++) for (int p = 0; p < FACE_POINTS; p++) {
        const lv_fpoint_t *a = &face->ring_from[k][p], *b = &aora_eyes[face->ring_target][k][p];
        face->ring[k][p] = (lv_fpoint_t){a->x + (b->x - a->x) * progress, a->y + (b->y - a->y) * progress};
    }
    face->pose = pose;
}

static void path_ring(lv_vector_path_t *path, const lv_fpoint_t *points, int count)
{
    lv_vector_path_clear(path);
    lv_vector_path_move_to(path, &points[0]);
    for (int i = 1; i < count; i++) lv_vector_path_line_to(path, &points[i]);
    lv_vector_path_close(path);
}

static void fill_color(lv_vector_dsc_t *dsc, uint32_t color, lv_opa_t opa)
{
    lv_vector_dsc_set_fill_color(dsc, lv_color_hex(color));
    lv_vector_dsc_set_fill_opa(dsc, opa);
    lv_vector_dsc_set_stroke_opa(dsc, LV_OPA_TRANSP);
}

static void circle_path(lv_vector_path_t *path, float x, float y, float rx, float ry)
{
    lv_vector_path_clear(path);
    lv_fpoint_t center = { x, y };
    lv_vector_path_append_circle(path, &center, rx, ry);
}

static void draw_ribbons(qz_face_t *face, lv_vector_dsc_t *dsc, bool front, float strength)
{
    if (strength <= 0.01f) return;
    static const uint32_t colors[] = {0x5B95F0, 0x9A72EE, 0x35C3BD, 0xF5B13F};
    for (int ribbon = 0; ribbon < 2; ribbon++) {
        float phase = face->clock / 880 + ribbon * FACE_PI;
        for (int i = 0; i < 26; i++) {
            float angle = phase - (25 - i) * 0.07f;
            if ((cosf(angle) > 0) != front) continue;
            lv_fpoint_t p = {FACE_C + sinf(angle) * (125 + ribbon * 6),
                            88 - cosf(angle) * 30 + sinf(angle) * (ribbon ? 10 : -10)};
            lv_fpoint_t q = {FACE_C + sinf(angle + 0.08f) * (125 + ribbon * 6),
                            88 - cosf(angle + 0.08f) * 30 + sinf(angle + 0.08f) * (ribbon ? 10 : -10)};
            lv_vector_path_clear(face->path);
            lv_vector_path_move_to(face->path, &p);
            lv_vector_path_line_to(face->path, &q);
            lv_vector_dsc_set_fill_opa(dsc, LV_OPA_TRANSP);
            lv_vector_dsc_set_stroke_color(dsc, lv_color_hex(colors[(ribbon + i / 7) % 4]));
            lv_vector_dsc_set_stroke_opa(dsc, (lv_opa_t)(strength * (80 + i * 5)));
            lv_vector_dsc_set_stroke_width(dsc, 0.5f + i * 0.12f);
            lv_vector_dsc_set_stroke_cap(dsc, LV_VECTOR_STROKE_CAP_ROUND);
            lv_vector_dsc_add_path(dsc, face->path);
        }
    }
}

static void draw_confetti(qz_face_t *face, lv_vector_dsc_t *dsc)
{
    float age = (face->clock - face->started) / 1000;
    float amount = face->pose.v[B_CONFETTI];
    if (age < 0 || age > 2.8f || amount <= 0) return;
    static const uint32_t colors[] = {0xF9705C, 0x5B95F0, 0x3FBE86, 0xF5B13F, 0x9A72EE, 0x35C3BD};
    float fade = smooth(age / 0.15f) * powf(1 - age / 2.8f, 1.7f) * amount;
    for (int i = 0; i < 14; i++) {
        float angle = 2 * FACE_PI * i / 14;
        float travel = (1 - expf(-2.5f * age)) * 70;
        float x = FACE_C + cosf(angle) * (72 + travel);
        float y = 105 + sinf(angle) * (65 + travel * 0.6f) + age * age * 8;
        circle_path(face->path, x, y, 2.3f, 1.4f);
        fill_color(dsc, colors[i % 6], (lv_opa_t)(clampf(fade, 0, 1) * 255));
        lv_vector_dsc_add_path(dsc, face->path);
    }
}

static void draw_sleep(qz_face_t *face, lv_vector_dsc_t *dsc, bool motion)
{
    float amount = face->pose.v[B_ZZZ];
    if (amount <= 0.01f) return;
    for (int i = 0; i < 3; i++) {
        float p = motion ? fmodf(face->clock * 0.00033f + i / 3.0f, 1) : i / 3.0f;
        float fade = motion ? (p < 0.18f ? p / 0.18f : 1 - (p - 0.18f) / 0.82f) : 0.6f;
        float x = 180 + p * 34 + 4 * sinf(p * 9), y = 48 - p * 42, size = 6 + p * 6;
        lv_vector_path_clear(face->path);
        lv_fpoint_t points[] = {{x, y}, {x + size, y}, {x, y + size}, {x + size, y + size}};
        lv_vector_path_move_to(face->path, &points[0]);
        for (int j = 1; j < 4; j++) lv_vector_path_line_to(face->path, &points[j]);
        lv_vector_dsc_set_fill_opa(dsc, LV_OPA_TRANSP);
        lv_vector_dsc_set_stroke_color(dsc, lv_color_hex(qz_theme_is_dark() ? 0xB8B4AC : 0x898379));
        lv_vector_dsc_set_stroke_opa(dsc, (lv_opa_t)(clampf(fade * amount, 0, 1) * 190));
        lv_vector_dsc_set_stroke_width(dsc, 1.6f);
        lv_vector_dsc_set_stroke_cap(dsc, LV_VECTOR_STROKE_CAP_ROUND);
        lv_vector_dsc_add_path(dsc, face->path);
    }
}

static void face_draw_event(lv_event_t *event)
{
    qz_face_t *face = lv_event_get_user_data(event);
    lv_layer_t *layer = lv_event_get_layer(event);
    if (!face || !layer) return;
    lv_area_t area;
    lv_obj_get_coords(face->root, &area);
    lv_layer_t *destination = layer;
    lv_area_t composite_area = area;
    bool composite = layer->color_format != LV_COLOR_FORMAT_ARGB8888 &&
                     layer->color_format != LV_COLOR_FORMAT_XRGB8888;
    if (composite) {
        /* ThorVG requires 32-bit pixels; framebuffer panels can be RGB565/888. */
        if (!lv_area_intersect(&composite_area, &area, &layer->_clip_area)) return;
        layer = lv_draw_layer_create(destination, LV_COLOR_FORMAT_ARGB8888, &composite_area);
        if (!layer) return;
    }
    float box = (float)LV_MIN(lv_area_get_width(&area), lv_area_get_height(&area));
    bool motion = !qz_reduce_motion();
    aora_pose_t *pose = &face->pose;
    lv_vector_dsc_t *dsc = lv_vector_dsc_create(layer);
    float unit = box / 285;
    lv_vector_dsc_translate(dsc, area.x1 + box / 2, area.y1 + box / 2);
    lv_vector_dsc_scale(dsc, unit, unit);
    lv_vector_dsc_translate(dsc, -FACE_C, -FACE_C);

    circle_path(face->path, FACE_C + pose->v[B_X] * 0.7f, 233, 68, 6);
    fill_color(dsc, 0x000000, qz_theme_is_dark() ? 26 : 18);
    lv_vector_dsc_add_path(dsc, face->path);

    float antic = face->clock - face->antic_started;
    float spin = motion && face->antic_kind == 1 && antic >= 0 && antic < 2200
        ? 2 * FACE_PI * smooth(antic / 2200) : 0;
    float ribbon_strength = motion ? fmaxf(pose->v[B_ORBIT], spin > 0 ? sinf(FACE_PI * antic / 2200) : 0) : 0;
    draw_ribbons(face, dsc, false, ribbon_strength);

    lv_matrix_t world;
    lv_matrix_identity(&world);
    lv_matrix_translate(&world, area.x1 + box / 2, area.y1 + box / 2);
    lv_matrix_scale(&world, unit, unit);
    lv_matrix_translate(&world, -FACE_C, -FACE_C);
    lv_vector_dsc_translate(dsc, FACE_C + pose->v[B_X], FACE_C + pose->v[B_Y]);
    lv_vector_dsc_rotate(dsc, pose->v[B_ROTATE]);
    float squash = spin ? 0.88f + 0.12f * fabsf(cosf(spin)) : 1;
    lv_vector_dsc_scale(dsc, pose->v[B_SCALE] * squash, pose->v[B_SCALE]);
    lv_vector_dsc_translate(dsc, -FACE_C, -FACE_C);

    path_ring(face->path, aora_body, 96);
    uint32_t color = pose->color[0];
    lv_gradient_stop_t stops[] = {
        {.color = lv_color_hex(blend_color(color, 0xFFFFFF, 0.22f)), .opa = 255, .frac = 0},
        {.color = lv_color_hex(color), .opa = 255, .frac = 158},
        {.color = lv_color_hex(blend_color(color, 0, 0.12f)), .opa = 255, .frac = 255}
    };
    lv_vector_dsc_set_fill_opa(dsc, LV_OPA_COVER);
    lv_vector_dsc_set_stroke_opa(dsc, LV_OPA_TRANSP);
    lv_vector_dsc_set_fill_radial_gradient(dsc, 228.54f * 0.38f, 228.44f * 0.32f, 228.54f * 0.75f);
    lv_vector_dsc_set_fill_gradient_color_stops(dsc, stops, 3);
    lv_vector_dsc_add_path(dsc, face->path);

    for (int k = 0; k < 2; k++) {
        int eye = k ? E_RIGHT : E_LEFT;
        lv_fpoint_t center = {0, 0}, points[FACE_POINTS];
        for (int i = 0; i < FACE_POINTS; i++) {
            center.x += face->ring[k][i].x / FACE_POINTS;
            center.y += face->ring[k][i].y / FACE_POINTS;
        }
        float sy = clampf(pose->v[eye + E_SY] * clampf(pose->v[eye + E_OPEN], 0.02f, 2.4f), 0.02f, 2.4f);
        float half_h = 21 * sy + 2;
        float ey = clampf(center.y + pose->v[eye + E_Y] + pose->v[eye + E_LOOK_Y], half_h, 228.44f - half_h);
        float half_w = fmaxf(sqrtf(fmaxf(0, FACE_C * FACE_C - (ey - FACE_C) * (ey - FACE_C))), 12);
        float theta = clampf((center.x - FACE_C + pose->v[eye + E_X] + pose->v[eye + E_LOOK_X]) / half_w, -1.15f, 1.15f) + spin;
        float compression = cosf(theta);
        if (compression <= 0.02f) continue;
        float ex = FACE_C + half_w * sinf(theta) * 0.985f;
        float fy = sqrtf(1 - powf((ey - FACE_C) / 130, 2) * 0.22f);
        float rotation = pose->v[eye + E_ROTATE] * FACE_PI / 180;
        float cs = cosf(rotation), sn = sinf(rotation);
        float eye_scale = box < 50 ? 1.12f : 1;
        for (int i = 0; i < FACE_POINTS; i++) {
            float x = (face->ring[k][i].x - center.x) * pose->v[eye + E_SX] * compression * eye_scale;
            float y = (face->ring[k][i].y - center.y) * sy * fy * eye_scale;
            points[i] = (lv_fpoint_t){ex + x * cs - y * sn, ey + x * sn + y * cs};
        }
        path_ring(face->path, points, FACE_POINTS);
        fill_color(dsc, pose->color[k + 1], LV_OPA_COVER);
        lv_vector_dsc_add_path(dsc, face->path);
    }

    lv_vector_dsc_set_transform(dsc, &world);
    draw_ribbons(face, dsc, true, ribbon_strength);
    if (motion) draw_confetti(face, dsc);
    if (box >= 48) draw_sleep(face, dsc, motion);
    lv_draw_vector(dsc);
    lv_vector_dsc_delete(dsc);
    if (composite) {
        lv_draw_image_dsc_t image_dsc;
        lv_draw_image_dsc_init(&image_dsc);
        image_dsc.src = layer;
        lv_draw_layer(destination, &image_dsc, &composite_area);
    }
}

static void face_tick(lv_timer_t *timer)
{
    qz_face_t *face = lv_timer_get_user_data(timer);
    uint32_t now = lv_tick_get();
    uint32_t elapsed = now - face->last_tick;
    face->last_tick = now;
    if (!lv_obj_is_visible(face->root) || lv_obj_get_screen(face->root) != lv_screen_active()) return;
    if (qz_reduce_motion()) return;
    face->clock += (float)LV_MIN(elapsed, 80) / FACE_TEMPO;
    compose_face(face, true);
    lv_obj_invalidate(face->root);
}

static void face_delete(lv_event_t *event)
{
    qz_face_t *face = lv_event_get_user_data(event);
    if (face->timer) lv_timer_delete(face->timer);
    lv_vector_path_delete(face->path);
    free(face);
}

lv_obj_t *qz_face_create(lv_obj_t *parent, int32_t design_px)
{
    if (!parent || design_px <= 0) return NULL;
    qz_face_t *face = calloc(1, sizeof(*face));
    if (!face) return NULL;
    face->rng = 0xA341316Cu ^ (uint32_t)(uintptr_t)face ^ lv_tick_get();
    face->animation_seed = (float)(face->rng % 1000) / 100;
    face->last_tick = lv_tick_get();
    face->path = lv_vector_path_create(LV_VECTOR_PATH_QUALITY_MEDIUM);
    face->pose = aora_emotions[EMO_IDLE].base;
    memcpy(face->ring, aora_eyes[0], sizeof(face->ring));
    start_emotion(face, EMO_IDLE);
    face->requested_state = QZ_FACE_IDLE;
    snprintf(face->requested_id, sizeof(face->requested_id), "02");
    compose_face(face, !qz_reduce_motion());
    face->root = lv_obj_create(parent);
    int box = LV_MAX(qz_scale_px(design_px), 24);
    (lv_obj_set_size)(face->root, box, box);
    (lv_obj_set_style_bg_opa)(face->root, LV_OPA_TRANSP, 0);
    (lv_obj_set_style_border_width)(face->root, 0, 0);
    (lv_obj_set_style_shadow_width)(face->root, 0, 0);
    (lv_obj_set_style_pad_all)(face->root, 0, 0);
    (lv_obj_set_style_radius)(face->root, 0, 0);
    lv_obj_clear_flag(face->root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(face->root, face);
    lv_obj_add_event_cb(face->root, face_draw_event, LV_EVENT_DRAW_MAIN_END, face);
    lv_obj_add_event_cb(face->root, face_delete, LV_EVENT_DELETE, face);
    face->timer = lv_timer_create(face_tick, FACE_TICK_MS, face);
    return face->root;
}

void qz_face_set_state(lv_obj_t *root, qz_face_state_t state)
{
    if (!root) return;
    qz_face_t *face = lv_obj_get_user_data(root);
    if (!face) return;
    if (state < 0 || state >= QZ_FACE_STATE_COUNT) state = QZ_FACE_IDLE;
    if (!face->requested_is_remote && face->requested_state == state) return;
    start_emotion(face, emotion_index_for_state(state));
    face->requested_is_remote = false;
    face->requested_state = state;
    snprintf(face->requested_id, sizeof(face->requested_id), "%s", aora_emotions[face->emotion].id);
    compose_face(face, !qz_reduce_motion());
    lv_obj_invalidate(root);
}

void qz_face_set_emotion(lv_obj_t *root, const char *emotion_id)
{
    if (!root || !emotion_id) return;
    qz_face_t *face = lv_obj_get_user_data(root);
    int index = emotion_index(emotion_id);
    if (!face || index < 0) return;
    if (face->requested_is_remote && strcmp(face->requested_id, face_emotions[index].id) == 0) return;
    start_emotion(face, index);
    face->requested_is_remote = true;
    face->requested_state = face_emotions[index].state;
    snprintf(face->requested_id, sizeof(face->requested_id), "%s", face_emotions[index].id);
    compose_face(face, !qz_reduce_motion());
    lv_obj_invalidate(root);
}

bool qz_face_emotion_valid(const char *emotion_id)
{
    return emotion_index(emotion_id) >= 0;
}

qz_face_state_t qz_face_emotion_state(const char *emotion_id)
{
    int index = emotion_index(emotion_id);
    return index >= 0 ? face_emotions[index].state : QZ_FACE_IDLE;
}

const char *qz_face_emotion_text(const char *emotion_id)
{
    int index = emotion_index(emotion_id);
    return index >= 0 ? face_emotions[index].text : "";
}

const char *qz_face_state_text(qz_face_state_t state)
{
    switch (state) {
        case QZ_FACE_SPEAKING: return "我在认真回答～";
        case QZ_FACE_THINKING: return "让我想一想…";
        case QZ_FACE_HAPPY: return "好开心见到你！";
        case QZ_FACE_CONFUSED: return "我好像没听懂…";
        case QZ_FACE_LOVE: return "好喜欢你呀！";
        case QZ_FACE_SURPRISED: return "咦？";
        case QZ_FACE_SLEEPY: return "我有点困了…";
        case QZ_FACE_WINK: return "嘿，我在这儿～";
        case QZ_FACE_EXCITED: return "太棒了！";
        case QZ_FACE_RELAXED: return "放轻松，我陪着你～";
        case QZ_FACE_CONFIDENT: return "交给我吧！";
        case QZ_FACE_EMBARRASSED: return "被夸得有点不好意思…";
        case QZ_FACE_PLAYFUL: return "嘿嘿，逗你一下～";
        case QZ_FACE_SAD: return "我有一点难过…";
        case QZ_FACE_IDLE:
        default: return "我在听你说～";
    }
}
