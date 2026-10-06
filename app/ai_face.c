/* Mascot face —— 矢量绘制 + 参数补间。
 *
 * 形象不是位图，而是用 LVGL 对象画出来的：身体、耳朵、头、眼睛、瞳孔、眉毛、嘴、
 * 腮红。每个表情是一组**参数**（眼睛开合、瞳孔朝向、眉毛高低、嘴形、腮红浓度、
 * 整体起伏），切换表情时把这些参数补间过去：过渡连续、不换帧、不跳变，也不需要
 * 烘焙流程与位图资源（原来的 6 张 192² RGB565A8 约 660KB 随之退役）。
 *
 * 只写十几个小对象的几何与不透明度，不用 transform_scale / 旋转，不申请图层缓冲
 * —— 软件渲染下比贴一张大图更便宜。
 *
 * 换配色或换造型：改下面 CHARACTER 那一节（配色 + 比例）与 k_state 参数表即可；
 * 尺寸仍然自动适配三处用法：60px（关于卡片）、88px（主页大卡片）、164px（全屏表情）。
 */
#include "ai_face.h"
#include "theme.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------- *
 * 节奏
 * ------------------------------------------------------------------------- */
#define QZ_BLINK_CLOSE_MS 90     /* 闭眼所需时间 */
#define QZ_BLINK_OPEN_MS 130     /* 睁开所需时间 */
#define QZ_BLINK_MIN_MS 3000     /* 眨眼间隔：随机，固定节拍会被一眼认出是循环 */
#define QZ_BLINK_MAX_MS 6200
#define QZ_BLINK_RETRY_MS 900    /* 非待机时不眨眼，过一会儿再看 */
#define QZ_LOOK_MIN_MS 2600      /* 待机时视线游移的间隔 */
#define QZ_LOOK_MAX_MS 5600
#define QZ_LOOK_MS 320           /* 视线移动时长 */
#define QZ_MOUTH_MIN_MS 200      /* 说话时嘴一开一合的周期 */
#define QZ_MOUTH_MAX_MS 320
#define QZ_BREATH_MS 2600

/* ------------------------------------------------------------------------- *
 * CHARACTER —— 配色与比例（比例是**面部尺寸的千分比**，与像素无关）
 * ------------------------------------------------------------------------- */
#define QZ_FUR 0xD9E2EC        /* 身体与头：浅灰蓝 */
#define QZ_FUR_LINE 0xB9C6D4   /* 描边：白卡片上也要看得出轮廓 */
#define QZ_FUR_INNER 0xF2C9CF  /* 耳内 */
#define QZ_INK 0x2F343B        /* 眼、眉、嘴 */
#define QZ_BLUSH 0xFF9AA6      /* 腮红 */

#define HEAD_W 600
#define HEAD_H 556
#define HEAD_Y 452
#define EYE_W 92
#define EYE_H 122
#define EYE_DX 165             /* 眼中距 330‰（与位图形象时代的锚点一致） */
#define EYE_Y 440              /* 眼睛行 44% 画布高 */
#define PUPIL 34
#define BROW_W 132
#define BROW_H 26
#define BROW_Y 350
#define MOUTH_Y 568
#define CHEEK_W 106
#define CHEEK_DX 206
#define CHEEK_Y 522
#define BODY_W 470
#define BODY_H 250
#define BODY_Y 744
#define EAR_W 150
#define EAR_H 250
#define EAR_DX 206
#define EAR_Y 244
#define DOT 150

/* 表情参数：几何量都是千分比，eye_open / cheek 是 0..1000 / 0..255。 */
typedef struct {
    int32_t eye_open;
    int32_t look_x;
    int32_t look_y;
    int32_t brow_l;
    int32_t brow_r;
    int32_t mouth_w;
    int32_t mouth_h;
    int32_t cheek;
    int32_t body_dy;
    int32_t body_dx;
    int32_t ear_dy;
} qz_face_params_t;

/* 参数通道：每个循环/一次性动效挂在一个唯一的 var 上，互不干扰。
 * 状态补间走 `face` 自己，通道各管一个参数。 */
enum {
    QZ_CH_EYE = 0,
    QZ_CH_MOUTH,
    QZ_CH_BODY_Y,
    QZ_CH_BODY_X,
    QZ_CH_LOOK_X,
    QZ_CH_LOOK_Y,
    QZ_CH_COUNT
};

typedef struct {
    void *face;
    int32_t kind;
    int32_t value;
} qz_face_channel_t;

typedef struct {
    lv_obj_t *root;
    lv_obj_t *body;
    lv_obj_t *ear[2];
    lv_obj_t *head;
    lv_obj_t *eye[2];
    lv_obj_t *pupil[2];
    lv_obj_t *brow[2];
    lv_obj_t *mouth;
    lv_obj_t *cheek[2];
    lv_obj_t *dots[3];
    lv_timer_t *blink_timer;
    lv_timer_t *look_timer;
    int32_t size;
    qz_face_state_t state;
    bool started;
    qz_face_params_t p;        /* 当前（补间中的）参数 */
    qz_face_params_t from;     /* 本次补间的起点 */
    qz_face_params_t to;       /* 本次补间的终点 */
    qz_face_channel_t ch[QZ_CH_COUNT];
} qz_face_t;

/* 各表情的参数。数值都能在屏幕上解释：eye_open 1000=全开、640=眯眼笑，
 * brow_* 负数=上扬，mouth_w/h 一起决定"闭嘴 / 咧开 / 张开 / o 形"。 */
static const qz_face_params_t k_state[QZ_FACE_STATE_COUNT] = {
    /*               eye_open look_x look_y brow_l brow_r mouth_w mouth_h cheek body_dy body_dx ear_dy */
    [QZ_FACE_IDLE]     = { 1000,     6,     4,    -6,    -6,    118,     32,     0,      0,      0,     0 },
    [QZ_FACE_SPEAKING] = { 1000,     3,     0,   -14,   -14,    134,    260,     0,      0,      0,     0 },
    [QZ_FACE_THINKING] = {  860,    26,   -18,   -46,    -8,     78,     34,     0,      0,      0,     6 },
    [QZ_FACE_HAPPY]    = {  640,     0,    -4,   -52,   -52,    210,    120,   130,      0,      0,   -48 },
    [QZ_FACE_CONFUSED] = { 1000,   -24,     8,    10,   -64,     70,     60,    24,      0,    -18,     8 },
};

/* ------------------------------------------------------------------------- *
 * 参数 → 几何
 * ------------------------------------------------------------------------- */
/** 千分比 → 设计稿像素（`size` 是设计尺寸，坐标宏会再按面板缩放）。 */
static int32_t pm(const qz_face_t *face, int32_t permille)
{
    return face->size * permille / 1000;
}

/** 以中心点定位：补间时改尺寸不会让元件跑偏。 */
static void place(lv_obj_t *obj, int32_t cx, int32_t cy, int32_t w, int32_t h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    lv_obj_set_size(obj, w, h);
    lv_obj_set_pos(obj, cx - w / 2, cy - h / 2);
}

static const qz_face_params_t *state_params(qz_face_state_t state)
{
    if (state >= QZ_FACE_STATE_COUNT) state = QZ_FACE_IDLE;
    return &k_state[state];
}

static bool can_animate(void);
static void stop_anims(qz_face_t *face);
static void start_tween(qz_face_t *face, const qz_face_params_t *target, uint32_t duration);
static void begin_loops(qz_face_t *face);
static void anim_opa(void *var, int32_t value);

static void apply_params(qz_face_t *face)
{
    const qz_face_params_t *p = &face->p;
    int32_t eye_h = pm(face, EYE_H) * p->eye_open / 1000;

    if (eye_h < 2) eye_h = 2;

    lv_obj_set_style_translate_x(face->root, pm(face, p->body_dx), 0);
    lv_obj_set_style_translate_y(face->root, pm(face, p->body_dy), 0);

    place(face->body, pm(face, 500), pm(face, BODY_Y), pm(face, BODY_W), pm(face, BODY_H));
    for (int i = 0; i < 2; i++) {
        int32_t side = (i == 0) ? -1 : 1;
        place(face->ear[i], pm(face, 500 + side * EAR_DX), pm(face, EAR_Y + p->ear_dy),
              pm(face, EAR_W), pm(face, EAR_H));
    }
    place(face->head, pm(face, 500), pm(face, HEAD_Y), pm(face, HEAD_W), pm(face, HEAD_H));

    for (int i = 0; i < 2; i++) {
        int32_t side = (i == 0) ? -1 : 1;
        int32_t eye_cx = pm(face, 500 + side * EYE_DX);
        int32_t eye_cy = pm(face, EYE_Y);

        place(face->eye[i], eye_cx, eye_cy, pm(face, EYE_W), eye_h);
        /* 瞳孔：眼睛快合上时先消失，避免留一条白缝 */
        /* 瞳孔行程只在眼睛内（look_* 的幅度就是这么定的），并略微偏上像高光 */
        place(face->pupil[i], eye_cx + pm(face, p->look_x), eye_cy + pm(face, p->look_y) - pm(face, 20),
              pm(face, PUPIL), pm(face, PUPIL));
        lv_obj_set_style_opa(face->pupil[i], p->eye_open < 420 ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
        /* 眉毛跟着视线走一点，看起来是"长在脸上"的 */
        place(face->brow[i], eye_cx + pm(face, p->look_x * 2 / 3),
              pm(face, BROW_Y + (i == 0 ? p->brow_l : p->brow_r)),
              pm(face, BROW_W), pm(face, BROW_H));
    }

    place(face->mouth, pm(face, 500), pm(face, MOUTH_Y), pm(face, p->mouth_w), pm(face, p->mouth_h));
    for (int i = 0; i < 2; i++) {
        int32_t side = (i == 0) ? -1 : 1;
        place(face->cheek[i], pm(face, 500 + side * CHEEK_DX), pm(face, CHEEK_Y),
              pm(face, CHEEK_W), pm(face, CHEEK_W));
        lv_obj_set_style_opa(face->cheek[i], (lv_opa_t)p->cheek, 0);
    }
}

/* ------------------------------------------------------------------------- *
 * 补间与通道
 * ------------------------------------------------------------------------- */
static void mix_params(void *var, int32_t value)
{
    qz_face_t *face = (qz_face_t *)var;
    const int32_t *from = (const int32_t *)&face->from;
    const int32_t *to = (const int32_t *)&face->to;
    int32_t *cur = (int32_t *)&face->p;
    const int32_t count = (int32_t)(sizeof(qz_face_params_t) / sizeof(int32_t));

    for (int32_t i = 0; i < count; i++) {
        cur[i] = from[i] + (to[i] - from[i]) * value / 256;
    }
    apply_params(face);
}

static void start_tween(qz_face_t *face, const qz_face_params_t *target, uint32_t duration)
{
    if (!can_animate() || duration == 0) {
        face->p = *target;
        apply_params(face);
        return;
    }
    face->from = face->p;
    face->to = *target;

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, face);
    lv_anim_set_exec_cb(&anim, mix_params);
    lv_anim_set_values(&anim, 0, 256);
    lv_anim_set_duration(&anim, duration);
    qz_anim_ease_out(&anim);
    lv_anim_start(&anim);
}

/** 通道对应的当前参数值：动画起点必须从这里取，否则第一条命令会跳一下。 */
static int32_t param_ref(const qz_face_t *face, int32_t kind)
{
    switch (kind) {
        case QZ_CH_EYE: return face->p.eye_open;
        case QZ_CH_MOUTH: return face->p.mouth_h;
        case QZ_CH_BODY_Y: return face->p.body_dy;
        case QZ_CH_BODY_X: return face->p.body_dx;
        case QZ_CH_LOOK_X: return face->p.look_x;
        case QZ_CH_LOOK_Y: return face->p.look_y;
        default: return 0;
    }
}

static void anim_channel(void *var, int32_t value)
{
    qz_face_channel_t *channel = (qz_face_channel_t *)var;
    qz_face_t *face = (qz_face_t *)channel->face;

    channel->value = value;
    switch (channel->kind) {
        case QZ_CH_EYE: face->p.eye_open = value; break;
        case QZ_CH_MOUTH: face->p.mouth_h = value; break;
        case QZ_CH_BODY_Y: face->p.body_dy = value; break;
        case QZ_CH_BODY_X: face->p.body_dx = value; break;
        case QZ_CH_LOOK_X: face->p.look_x = value; break;
        case QZ_CH_LOOK_Y: face->p.look_y = value; break;
        default: break;
    }
    apply_params(face);
}

static void channel_set(qz_face_t *face, int32_t kind, int32_t to, uint32_t duration,
                        uint32_t delay, uint32_t playback, bool loop)
{
    lv_anim_t anim;
    qz_face_channel_t *channel = &face->ch[kind];

    channel->value = param_ref(face, kind);
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, channel);
    lv_anim_set_exec_cb(&anim, anim_channel);
    lv_anim_set_values(&anim, channel->value, to);
    lv_anim_set_duration(&anim, duration);
    if (delay) lv_anim_set_delay(&anim, delay);
    if (playback) lv_anim_set_playback_duration(&anim, playback);
    if (loop) lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    qz_anim_ease_in_out(&anim);
    lv_anim_start(&anim);
}

/** 从当前值插到一个绝对值（不循环）。 */
static void channel_to(qz_face_t *face, int32_t kind, int32_t to, uint32_t duration)
{
    channel_set(face, kind, to, duration, 0, 0, false);
}

/** 来回循环（呼吸、摇摆、说话时的嘴）。 */
static void channel_loop(qz_face_t *face, int32_t kind, int32_t to, uint32_t duration,
                         uint32_t delay)
{
    channel_set(face, kind, to, duration, delay, duration, true);
}

/** 去-回一次（眨眼）：闭合后自动睁开。 */
static void channel_blink(qz_face_t *face, int32_t to, uint32_t close_ms, uint32_t open_ms)
{
    channel_set(face, QZ_CH_EYE, to, close_ms, 0, open_ms, false);
}

static void stop_anims(qz_face_t *face)
{
    lv_anim_delete(face, NULL);
    lv_anim_delete(face->root, NULL);
    for (int i = 0; i < QZ_CH_COUNT; i++) {
        lv_anim_delete(&face->ch[i], NULL);
    }
    for (int i = 0; i < 3; i++) {
        lv_anim_delete(face->dots[i], NULL);
        lv_obj_set_style_opa(face->dots[i], LV_OPA_TRANSP, 0);
    }
}

static bool can_animate(void)
{
    return !qz_reduce_motion();
}

/* ------------------------------------------------------------------------- *
 * 待机时的"活着"：眨眼与视线游移
 * ------------------------------------------------------------------------- */
static int32_t rand_between(int32_t lo, int32_t hi)
{
    int32_t span = hi - lo;
    return lo + (int32_t)(rand() % (span + 1));
}

static void blink_tick(lv_timer_t *timer)
{
    qz_face_t *face = (qz_face_t *)lv_timer_get_user_data(timer);

    /* 只在待机时眨：闭眼和"说话/思考"语义冲突 */
    if (!can_animate() || face->state != QZ_FACE_IDLE) {
        lv_timer_set_period(timer, QZ_BLINK_RETRY_MS);
        return;
    }
    channel_blink(face, 60, QZ_BLINK_CLOSE_MS, QZ_BLINK_OPEN_MS);
    lv_timer_set_period(timer, rand_between(QZ_BLINK_MIN_MS, QZ_BLINK_MAX_MS));
}

/** 开发用：QZDESK_FACE_CYCLE=1 时逐个轮播表情，便于在屏幕上核对参数。 */
static void cycle_tick(lv_timer_t *timer)
{
    qz_face_t *face = (qz_face_t *)lv_timer_get_user_data(timer);

    qz_face_set_state(face->root, (qz_face_state_t)((face->state + 1) % QZ_FACE_STATE_COUNT));
}

static void look_tick(lv_timer_t *timer)
{
    qz_face_t *face = (qz_face_t *)lv_timer_get_user_data(timer);

    if (can_animate() && face->state == QZ_FACE_IDLE) {
        /* 幅度以瞳孔不跑出眼睛为准（眼睛宽 92‰、瞳孔 34‰） */
        channel_to(face, QZ_CH_LOOK_X, rand_between(-28, 28), QZ_LOOK_MS);
        channel_to(face, QZ_CH_LOOK_Y, rand_between(-16, 20), QZ_LOOK_MS + 40);
    }
    lv_timer_set_period(timer, rand_between(QZ_LOOK_MIN_MS, QZ_LOOK_MAX_MS));
}

/* ------------------------------------------------------------------------- *
 * 循环动效（按状态）
 * ------------------------------------------------------------------------- */
static void begin_loops(qz_face_t *face)
{
    /* 循环动效等表情补间走完再接管同一个参数，否则两边各写一帧会抖。 */
    const uint32_t delay = QZ_DUR_PANEL;

    if (!can_animate()) return;

    switch (face->state) {
        case QZ_FACE_SPEAKING:
            /* 嘴一开一合 + 轻微起伏：周期取随机，避免像节拍器 */
            channel_loop(face, QZ_CH_MOUTH, 900,
                         (uint32_t)rand_between(QZ_MOUTH_MIN_MS, QZ_MOUTH_MAX_MS), delay);
            channel_loop(face, QZ_CH_BODY_Y, -14, 420, delay);
            break;

        case QZ_FACE_THINKING:
            channel_loop(face, QZ_CH_BODY_Y, -18, 1800, delay);
            for (int i = 0; i < 3; i++) {
                lv_anim_t dot;
                lv_anim_init(&dot);
                lv_anim_set_var(&dot, face->dots[i]);
                lv_anim_set_exec_cb(&dot, anim_opa);
                lv_anim_set_values(&dot, LV_OPA_20, LV_OPA_COVER);
                lv_anim_set_duration(&dot, 520);
                lv_anim_set_delay(&dot, (uint32_t)i * 180);
                lv_anim_set_playback_duration(&dot, 520);
                lv_anim_set_repeat_count(&dot, LV_ANIM_REPEAT_INFINITE);
                qz_anim_ease_in_out(&dot);
                lv_anim_start(&dot);
            }
            break;

        case QZ_FACE_HAPPY:
            channel_loop(face, QZ_CH_BODY_Y, -40, 460, delay);
            break;

        case QZ_FACE_CONFUSED:
            channel_loop(face, QZ_CH_BODY_X, 18, 1200, delay);
            break;

        case QZ_FACE_IDLE:
        default:
            channel_loop(face, QZ_CH_BODY_Y, -22, QZ_BREATH_MS, delay);
            break;
    }
}

/* ------------------------------------------------------------------------- *
 * 创建
 * ------------------------------------------------------------------------- */
static void plain(lv_obj_t *obj, uint32_t bg, lv_opa_t opa)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

/** 圆角统一用 LV_RADIUS_CIRCLE：半径随尺寸自适应，眨眼把眼睛压扁时会自然变成一条线。 */
static lv_obj_t *shape(lv_obj_t *parent, uint32_t bg, lv_opa_t opa)
{
    lv_obj_t *obj = lv_obj_create(parent);

    plain(obj, bg, opa);
    return obj;
}

static void anim_opa(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

static void face_delete(lv_event_t *event)
{
    qz_face_t *face = (qz_face_t *)lv_event_get_user_data(event);

    if (!face) return;
    stop_anims(face);
    if (face->blink_timer) lv_timer_delete(face->blink_timer);
    if (face->look_timer) lv_timer_delete(face->look_timer);
    free(face);
}

lv_obj_t *qz_face_create(lv_obj_t *parent, int32_t size)
{
    qz_face_t *face = (qz_face_t *)calloc(1, sizeof(qz_face_t));

    if (!face) return NULL;
    face->size = size;
    face->root = lv_obj_create(parent);
    lv_obj_set_size(face->root, size, size);
    lv_obj_set_style_bg_opa(face->root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(face->root, 0, 0);
    lv_obj_set_style_pad_all(face->root, 0, 0);
    lv_obj_clear_flag(face->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(face->root, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_user_data(face->root, face);
    lv_obj_add_event_cb(face->root, face_delete, LV_EVENT_DELETE, face);

    for (int i = 0; i < QZ_CH_COUNT; i++) {
        face->ch[i].face = face;
        face->ch[i].kind = i;
    }

    /* 先画身体与耳朵（在头后面），再画头，最后是五官 */
    face->body = shape(face->root, QZ_FUR, LV_OPA_COVER);
    for (int i = 0; i < 2; i++) {
        face->ear[i] = shape(face->root, QZ_FUR, LV_OPA_COVER);
        lv_obj_t *inner = shape(face->ear[i], QZ_FUR_INNER, LV_OPA_COVER);
        lv_obj_set_size(inner, size * EAR_W * 55 / 100000, size * EAR_H * 58 / 100000);
        lv_obj_center(inner);
    }
    face->head = shape(face->root, QZ_FUR, LV_OPA_COVER);
    /* 头做成方圆形：圆角取头宽的 46%，比正圆更像卡通头 */
    lv_obj_set_style_radius(face->head, pm(face, HEAD_W) * 46 / 100, 0);
    lv_obj_set_style_border_width(face->head, 1, 0);
    lv_obj_set_style_border_color(face->head, lv_color_hex(QZ_FUR_LINE), 0);

    for (int i = 0; i < 2; i++) {
        face->eye[i] = shape(face->root, QZ_INK, LV_OPA_COVER);
        face->pupil[i] = shape(face->root, 0xFFFFFF, LV_OPA_COVER);
        face->brow[i] = shape(face->root, QZ_INK, LV_OPA_COVER);
        face->cheek[i] = shape(face->root, QZ_BLUSH, LV_OPA_TRANSP);
    }
    face->mouth = shape(face->root, QZ_INK, LV_OPA_COVER);

    /* 思考气泡：画在右上角，只在 thinking 时脉冲 */
    for (int i = 0; i < 3; i++) {
        lv_obj_t *dot = shape(face->root, QZ_FACE_BUBBLE, LV_OPA_COVER);
        int32_t d = pm(face, DOT);
        lv_obj_set_size(dot, d, d);
        lv_obj_set_pos(dot, pm(face, 500) + pm(face, 250) + (int32_t)i * (d * 3 / 2),
                       pm(face, 120));
        lv_obj_set_style_opa(dot, LV_OPA_TRANSP, 0);
        face->dots[i] = dot;
    }

    face->p = k_state[QZ_FACE_IDLE];
    face->from = face->p;
    face->to = face->p;
    apply_params(face);

    face->blink_timer = lv_timer_create(blink_tick, rand_between(QZ_BLINK_MIN_MS, QZ_BLINK_MAX_MS), face);
    face->look_timer = lv_timer_create(look_tick, rand_between(QZ_LOOK_MIN_MS, QZ_LOOK_MAX_MS), face);
    if (getenv("QZDESK_FACE_CYCLE")) {
        lv_timer_create(cycle_tick, 1800, face);
    }
    begin_loops(face);
    return face->root;
}

/* ------------------------------------------------------------------------- *
 * 状态
 * ------------------------------------------------------------------------- */
const char *qz_face_state_text(qz_face_state_t state)
{
    switch (state) {
        case QZ_FACE_SPEAKING: return "我在认真回答～";
        case QZ_FACE_THINKING: return "让我想一想…";
        case QZ_FACE_HAPPY: return "好开心见到你！";
        case QZ_FACE_CONFUSED: return "我好像没听懂…";
        case QZ_FACE_IDLE:
        default: return "我在听你说～";
    }
}

void qz_face_set_state(lv_obj_t *root, qz_face_state_t state)
{
    qz_face_t *face;

    if (!root) return;
    face = (qz_face_t *)lv_obj_get_user_data(root);
    if (!face) return;
    if (face->started && face->state == state) return; /* 同状态不重启动画，免得抖 */

    face->started = true;
    face->state = state;
    stop_anims(face);
    /* 表情之间只补间参数：眼睛、眉毛、嘴、腮红、耳朵一起走，所以没有换帧的跳变 */
    start_tween(face, state_params(state), qz_reduce_motion() ? 0 : QZ_DUR_PANEL);
    begin_loops(face);
}
