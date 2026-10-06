/* Mascot face —— Echo-Mate 式"双眼表情"。
 *
 * 结构与编排移植自同门项目 Echo-Mate（DeskBot gui_app/pages/ui_ChatBotPage）：
 * 一条透明"眼睛面板"（210×80 基准）里两只白色圆角眼睛（80×80，@x∓60），下面一只
 * 嘴（60×60，平时透明，说话时出现并上下动），问号/思考/手三张 60px 装饰图。
 * 每种状态是一段**定时编排**（一串带延时的 lv_anim）：闪眼 = 把整条面板高度压到
 * 10px（子对象被裁剪，看起来就是眼皮合上）；看四周 = 面板平移；听/想/问 = 对应
 * 装饰图旋转淡入淡出。播完后由 250ms 调度器决定是否重播（idle 两种编排交替）。
 *
 * 所有尺寸按 face size / 210 等比换算（u()），一套编排伺候三处尺寸：
 * 60px（关于卡片）、88px（主页大卡片）、164px（全屏表情）。
 *
 * 与 Echo-Mate 的差异：编排链不用它的 lib_anim 包装，直接用 lv_anim 的 delay 串联；
 * 减少动态（qz_reduce_motion）时显示静态脸。
 */
#include "ai_face.h"
#include "emoji_imgs.h"
#include "theme.h"
#include <stdio.h>
#include <stdlib.h>

/* Echo-Mate 的设计基准（眼睛面板 210 宽），以下数值都按它写，运行时用 u() 缩放 */
#define EYE_PANEL_W 210
#define EYE_PANEL_H 80
#define EYE_SIZE 80
#define EYE_DX 60
#define EYE_PANEL_Y (-25)
#define MOUTH_PANEL_Y 95
#define MOUTH_SIZE 60
#define MOUTH_Y (-40)
#define BLINK_H 10
#define BLINK_MS 100

typedef struct {
    lv_obj_t *root;        /* size×size 的脸容器 */
    int32_t size;          /* 脸容器边长（设计像素，60/88/164） */
    lv_obj_t *eyes_panel;  /* 眼睛面板：闪眼时整体压扁，子对象随之被裁剪 */
    lv_obj_t *ver_panel;   /* 纵向漂移子面板 */
    lv_obj_t *eye[2];      /* 左右眼 */
    lv_obj_t *mouth_panel;
    lv_obj_t *mouth;
    lv_obj_t *question_img;
    lv_obj_t *think_img;
    lv_obj_t *hand_img;
    lv_timer_t *dispatch_timer;
    qz_face_state_t state;
    qz_face_state_t last_state;
    bool anim_complete;    /* 当前这段编排播完了吗 */
    uint32_t replay_at;    /* lv_tick：到点后重播当前编排 */
    int idle_index;        /* idle 的两种编排交替 */
} qz_face_t;

/* ------------------------------------------------------------------------- *
 * 动画小工具
 * ------------------------------------------------------------------------- */
static void anim_set_y(void *var, int32_t v) { lv_obj_set_y((lv_obj_t *)var, v); }

static void anim_set_x(void *var, int32_t v) { lv_obj_set_x((lv_obj_t *)var, v); }

static void anim_set_height(void *var, int32_t v) { lv_obj_set_height((lv_obj_t *)var, v); }

static void anim_set_width(void *var, int32_t v) { lv_obj_set_width((lv_obj_t *)var, v); }

static void anim_image_angle(void *var, int32_t v)
{
    lv_image_set_rotation((lv_obj_t *)var, (uint32_t)v);
}

static void anim_opa(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

/** 编排的一步：延时 delay 后把属性从 from 补到 to。 */
static void seq(lv_obj_t *obj, uint32_t delay, uint32_t dur, int32_t from, int32_t to,
                lv_anim_path_cb_t path, lv_anim_exec_xcb_t cb)
{
    lv_anim_t anim;

    if (dur == 0) return;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_exec_cb(&anim, cb);
    lv_anim_set_values(&anim, from, to);
    lv_anim_set_duration(&anim, dur);
    lv_anim_set_delay(&anim, delay);
    lv_anim_set_path_cb(&anim, path);
    lv_anim_start(&anim);
}

/* ------------------------------------------------------------------------- *
 * 编排（数值照抄 Echo-Mate，运行时经 u() 等比缩放）
 * ------------------------------------------------------------------------- */
/** Echo-Mate 设计值 -> 我们的尺寸（它们的基准是 210 宽的眼睛面板）。 */
static int32_t uw(qz_face_t *face, int32_t v)
{
    return face->size * v / 210;
}

/**
 * 把所有对象摆回基准位，并隐藏装饰。
 *
 * 一律用 lv_obj_align(CENTER)（和创建时一致）：面板是居中摆放的，若在这里直接
 * lv_obj_set_y(面板, -25) 会把"居中坐标"写成原始设计值，整个脸跳到容器左上角，
 * 右眼超出被裁 —— 就是"显示不全"的来源。编排动画的起点全部取当前实际坐标。
 */
static void reinit(qz_face_t *face)
{
    int32_t s = face->size;

    lv_anim_delete(face->eyes_panel, NULL);
    lv_anim_delete(face->ver_panel, NULL);
    for (int i = 0; i < 2; i++) lv_anim_delete(face->eye[i], NULL);
    lv_anim_delete(face->mouth_panel, NULL);
    lv_anim_delete(face->mouth, NULL);
    lv_anim_delete(face->question_img, NULL);
    lv_anim_delete(face->think_img, NULL);
    lv_anim_delete(face->hand_img, NULL);

    lv_obj_set_width(face->eyes_panel, s * EYE_PANEL_W / 210);
    lv_obj_set_height(face->eyes_panel, s * EYE_PANEL_H / 210);
    lv_obj_align(face->eyes_panel, LV_ALIGN_CENTER, 0, s * EYE_PANEL_Y / 210);
    lv_obj_set_width(face->ver_panel, s * EYE_PANEL_W / 210);
    lv_obj_set_height(face->ver_panel, s * EYE_PANEL_H / 210);
    lv_obj_center(face->ver_panel);
    for (int i = 0; i < 2; i++) {
        int32_t side = (i == 0) ? -1 : 1;
        lv_obj_set_width(face->eye[i], s * EYE_SIZE / 210);
        lv_obj_set_height(face->eye[i], s * EYE_SIZE / 210);
        lv_obj_align(face->eye[i], LV_ALIGN_CENTER, side * s * EYE_DX / 210, 0);
    }
    lv_obj_set_width(face->mouth, s * MOUTH_SIZE / 210);
    lv_obj_set_height(face->mouth, s * MOUTH_SIZE / 210);
    lv_obj_align(face->mouth, LV_ALIGN_CENTER, 0, s * MOUTH_Y / 210);
    lv_obj_align(face->mouth_panel, LV_ALIGN_CENTER, 0, s * MOUTH_PANEL_Y / 210);
    lv_obj_set_style_bg_opa(face->mouth, LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(face->question_img, LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(face->think_img, LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(face->hand_img, LV_OPA_TRANSP, 0);
}

/** 装饰图：淡入 + 旋转一圈（Echo-Mate 的问号/思考图入场方式）。 */
static void image_spin_in(qz_face_t *face, lv_obj_t *img, uint32_t delay)
{
    int32_t s = face->size;

    seq(img, delay, 500, 0, 255, lv_anim_path_linear, anim_opa);
    seq(img, delay + s * 750 / 210, 500, -100, 100, lv_anim_path_ease_in_out, anim_image_angle);
}

static void image_fade_out(qz_face_t *face, lv_obj_t *img, uint32_t delay)
{
    (void)face;
    seq(img, delay, 500, 255, 0, lv_anim_path_linear, anim_opa);
}

/** 待机编排一：看左、看右、看右下（沿途眨三次眼），5.5s。 */
static void idle1(qz_face_t *face)
{
    lv_obj_t *panel = face->eyes_panel;
    int32_t y0, x0, h0;
    int32_t blink_h = uw(face, BLINK_H);

    reinit(face);
    y0 = lv_obj_get_y(panel);      /* reinit 之后的真实位置，编排从这里出发 */
    x0 = lv_obj_get_x(panel);
    h0 = lv_obj_get_height(panel);
    seq(panel, 0, 500, y0, y0 - uw(face, 20), lv_anim_path_ease_in_out, anim_set_y);
    seq(panel, 0, 500, x0, x0 - uw(face, 20), lv_anim_path_ease_in_out, anim_set_x);
    seq(panel, 1000, uw(face, BLINK_MS), h0, blink_h, lv_anim_path_ease_out, anim_set_height);
    seq(panel, 1000 + uw(face, BLINK_MS), uw(face, BLINK_MS), blink_h, h0,
        lv_anim_path_ease_out, anim_set_height);
    seq(panel, 1500, 500, x0 - uw(face, 20), x0 + uw(face, 20), lv_anim_path_ease_in_out, anim_set_x);
    seq(panel, 2000, uw(face, BLINK_MS), h0, blink_h, lv_anim_path_ease_out, anim_set_height);
    seq(panel, 2000 + uw(face, BLINK_MS), uw(face, BLINK_MS), blink_h, h0,
        lv_anim_path_ease_out, anim_set_height);
    seq(panel, 3000, 500, x0 + uw(face, 20), x0 + uw(face, 40), lv_anim_path_ease_in_out, anim_set_x);
    seq(panel, 3000, 500, y0 - uw(face, 20), y0 + uw(face, 20), lv_anim_path_ease_in_out, anim_set_y);
    seq(panel, 4000, uw(face, BLINK_MS), h0, blink_h, lv_anim_path_ease_out, anim_set_height);
    seq(panel, 4000 + uw(face, BLINK_MS), uw(face, BLINK_MS), blink_h, h0,
        lv_anim_path_ease_out, anim_set_height);
    seq(panel, 5000, 500, x0 + uw(face, 40), x0, lv_anim_path_ease_in_out, anim_set_x);
    seq(panel, 5000, 500, y0 + uw(face, 20), y0, lv_anim_path_ease_in_out, anim_set_y);
    face->replay_at = lv_tick_get() + 5600;
}

/** 待机编排二：上下两层对向漂移（呼吸），思考图旋转进出，3.5s。 */
static void idle2(qz_face_t *face)
{
    lv_obj_t *panel = face->eyes_panel;
    lv_obj_t *ver = face->ver_panel;
    int32_t vy0, y0;

    reinit(face);
    vy0 = lv_obj_get_y(ver);
    y0 = lv_obj_get_y(panel);
    seq(ver, 0, 500, vy0, vy0 - uw(face, 20), lv_anim_path_ease_out, anim_set_y);
    seq(ver, 2500, 500, vy0 - uw(face, 20), vy0, lv_anim_path_ease_out, anim_set_y);
    seq(panel, 0, 500, y0, y0 + uw(face, 20), lv_anim_path_ease_out, anim_set_y);
    seq(panel, 2500, 500, y0 + uw(face, 20), y0, lv_anim_path_ease_out, anim_set_y);

    image_spin_in(face, face->think_img, uw(face, 750));
    image_fade_out(face, face->think_img, 3000);
    face->replay_at = lv_tick_get() + 3600;
}

/** 聆听/惊讶：眼睛收窄后上下脉冲两次，问号旋转进出，4s。 */
static void listening(qz_face_t *face)
{
    int32_t w0, h0;
    int32_t w_squash = uw(face, EYE_SIZE - 30);
    int32_t blink_h = uw(face, BLINK_H);
    int32_t blink_ms = uw(face, BLINK_MS);

    reinit(face);
    w0 = lv_obj_get_width(face->eye[0]);
    h0 = lv_obj_get_height(face->eye[0]);
    for (int i = 0; i < 2; i++) {
        seq(face->eye[i], 0, 100, w0, w_squash, lv_anim_path_ease_out, anim_set_width);
        seq(face->eye[i], 1000, 100, h0, blink_h, lv_anim_path_ease_out, anim_set_height);
        seq(face->eye[i], 1000 + blink_ms, blink_ms, blink_h, h0, lv_anim_path_ease_out,
            anim_set_height);
        seq(face->eye[i], 2000, 100, h0, blink_h, lv_anim_path_ease_out, anim_set_height);
        seq(face->eye[i], 2000 + blink_ms, blink_ms, blink_h, h0, lv_anim_path_ease_out,
            anim_set_height);
        seq(face->eye[i], 3000, 100, w_squash, w0, lv_anim_path_ease_out, anim_set_width);
    }
    image_spin_in(face, face->question_img, 0);
    image_fade_out(face, face->question_img, 3500);
    face->replay_at = lv_tick_get() + 4100;
}

/** 思考：手图旋转进来，问号退场，眼睛脉冲，3.5s。 */
static void thinking(qz_face_t *face)
{
    int32_t w0, h0;
    int32_t w_squash = uw(face, EYE_SIZE - 30);
    int32_t blink_h = uw(face, BLINK_H);
    int32_t blink_ms = uw(face, BLINK_MS);

    reinit(face);
    w0 = lv_obj_get_width(face->eye[0]);
    h0 = lv_obj_get_height(face->eye[0]);
    seq(face->hand_img, 0, 500, 0, 255, lv_anim_path_linear, anim_opa);
    seq(face->hand_img, uw(face, 750), 500, -250, -150, lv_anim_path_ease_in_out,
        anim_image_angle);
    image_fade_out(face, face->question_img, 1500);
    image_fade_out(face, face->hand_img, 1500);
    for (int i = 0; i < 2; i++) {
        seq(face->eye[i], 0, 100, w0, w_squash, lv_anim_path_ease_out, anim_set_width);
        seq(face->eye[i], 1000, 100, h0, blink_h, lv_anim_path_ease_out, anim_set_height);
        seq(face->eye[i], 1000 + blink_ms, blink_ms, blink_h, h0, lv_anim_path_ease_out,
            anim_set_height);
        seq(face->eye[i], 2000, 100, h0, blink_h, lv_anim_path_ease_out, anim_set_height);
        seq(face->eye[i], 2000 + blink_ms, blink_ms, blink_h, h0, lv_anim_path_ease_out,
            anim_set_height);
    }
    face->replay_at = lv_tick_get() + 3600;
}

/** 说话：嘴出现并一开一合两轮（嘴板反向移动），中途整条眼板眨一次，2.6s。 */
static void speaking(qz_face_t *face)
{
    lv_obj_t *mouth = face->mouth;
    lv_obj_t *mouth_panel = face->mouth_panel;
    lv_obj_t *panel = face->eyes_panel;
    int32_t my0, py0, h0;
    int32_t blink_h = uw(face, BLINK_H);

    reinit(face);
    lv_obj_set_style_bg_opa(mouth, LV_OPA_COVER, 0);
    my0 = lv_obj_get_y(mouth);
    py0 = lv_obj_get_y(mouth_panel);
    h0 = lv_obj_get_height(panel);
    /* 一开一合：嘴上抬、嘴板下迎，再各自回去 */
    seq(mouth, 0, 150, my0, my0 - uw(face, 10), lv_anim_path_ease_out, anim_set_y);
    seq(mouth, 150, 150, my0 - uw(face, 10), my0, lv_anim_path_ease_out, anim_set_y);
    seq(mouth_panel, 0, 150, py0, py0 + uw(face, 10), lv_anim_path_ease_out, anim_set_y);
    seq(mouth_panel, 150, 150, py0 + uw(face, 10), py0, lv_anim_path_ease_out, anim_set_y);
    seq(panel, 500, 200, h0, blink_h, lv_anim_path_ease_in_out, anim_set_height);
    seq(panel, 700, 200, blink_h, h0, lv_anim_path_ease_in_out, anim_set_height);
    seq(mouth, 1500, 150, my0, my0 - uw(face, 10), lv_anim_path_ease_out, anim_set_y);
    seq(mouth, 1650, 150, my0 - uw(face, 10), my0, lv_anim_path_ease_out, anim_set_y);
    seq(mouth_panel, 1500, 150, py0, py0 + uw(face, 10), lv_anim_path_ease_out, anim_set_y);
    seq(mouth_panel, 1650, 150, py0 + uw(face, 10), py0, lv_anim_path_ease_out, anim_set_y);
    seq(panel, 2000, 200, h0, blink_h, lv_anim_path_ease_in_out, anim_set_height);
    seq(panel, 2200, 200, blink_h, h0, lv_anim_path_ease_in_out, anim_set_height);
    face->replay_at = lv_tick_get() + 2600;
}

/** 开心：蹦一下带两次眨眼，2.7s（我们的附加状态，词汇与 Echo-Mate 一致）。 */
static void happy(qz_face_t *face)
{
    lv_obj_t *panel = face->eyes_panel;
    int32_t y0, h0;
    int32_t blink_h = uw(face, BLINK_H);
    int32_t blink_ms = uw(face, BLINK_MS);

    reinit(face);
    y0 = lv_obj_get_y(panel);
    h0 = lv_obj_get_height(panel);
    seq(panel, 0, 300, y0, y0 - uw(face, 26), lv_anim_path_ease_out, anim_set_y);
    seq(panel, 300, 300, y0 - uw(face, 26), y0, lv_anim_path_ease_in_out, anim_set_y);
    seq(panel, 400, blink_ms, h0, blink_h, lv_anim_path_ease_out, anim_set_height);
    seq(panel, 400 + blink_ms, blink_ms, blink_h, h0, lv_anim_path_ease_out, anim_set_height);
    seq(panel, 1200, blink_ms, h0, blink_h, lv_anim_path_ease_out, anim_set_height);
    seq(panel, 1200 + blink_ms, blink_ms, blink_h, h0, lv_anim_path_ease_out, anim_set_height);
    seq(panel, 1500, 300, y0, y0 - uw(face, 14), lv_anim_path_ease_in_out, anim_set_y);
    seq(panel, 1800, 300, y0 - uw(face, 14), y0, lv_anim_path_ease_in_out, anim_set_y);
    face->replay_at = lv_tick_get() + 2700;
}

/** 困倦：整条眼板往下沉 + 三次慢眨，3.7s。 */
static void sleepy(qz_face_t *face)
{
    lv_obj_t *panel = face->eyes_panel;
    int32_t y0, h0;
    int32_t blink_h = uw(face, BLINK_H);

    reinit(face);
    y0 = lv_obj_get_y(panel);
    h0 = lv_obj_get_height(panel);
    seq(panel, 0, 800, y0, y0 + uw(face, 10), lv_anim_path_ease_out, anim_set_y);
    seq(panel, 100, 500, h0, blink_h, lv_anim_path_ease_in_out, anim_set_height);
    seq(panel, 600, 500, blink_h, h0, lv_anim_path_ease_in_out, anim_set_height);
    seq(panel, 1400, 500, h0, blink_h, lv_anim_path_ease_in_out, anim_set_height);
    seq(panel, 1900, 500, blink_h, h0, lv_anim_path_ease_in_out, anim_set_height);
    seq(panel, 2200, 500, h0, blink_h, lv_anim_path_ease_in_out, anim_set_height);
    seq(panel, 2700, 500, blink_h, h0, lv_anim_path_ease_in_out, anim_set_height);
    face->replay_at = lv_tick_get() + 3700;
}

/** 单眼眨：右眼压扁再弹回，1.3s。 */
static void wink(qz_face_t *face)
{
    lv_obj_t *right = face->eye[1];
    int32_t x0, h0;

    reinit(face);
    h0 = lv_obj_get_height(right);
    x0 = lv_obj_get_x(face->eyes_panel);
    seq(right, 0, 150, h0, uw(face, BLINK_H), lv_anim_path_ease_out, anim_set_height);
    seq(right, 400, 250, uw(face, BLINK_H), h0, lv_anim_path_ease_out, anim_set_height);
    seq(face->eyes_panel, 0, 200, x0, x0 + uw(face, 10), lv_anim_path_ease_out, anim_set_x);
    seq(face->eyes_panel, 600, 200, x0 + uw(face, 10), x0, lv_anim_path_ease_out, anim_set_x);
    face->replay_at = lv_tick_get() + 1300;
}

/* ------------------------------------------------------------------------- *
 * 调度：状态变化 -> 重新初始化并播新编排；播完 -> 重播（idle 两种交替）
 * ------------------------------------------------------------------------- */
static void dispatch_tick(lv_timer_t *timer)
{
    qz_face_t *face = (qz_face_t *)lv_timer_get_user_data(timer);

    if (!face) return;
    if (face->state != face->last_state) {
        face->last_state = face->state;
        face->anim_complete = true;
        reinit(face);
    }
    if (qz_reduce_motion()) return;            /* 静态脸 */
    if (!face->anim_complete || lv_tick_get() < face->replay_at) return;

    switch (face->state) {
        case QZ_FACE_SPEAKING: speaking(face); break;
        case QZ_FACE_THINKING: thinking(face); break;
        case QZ_FACE_CONFUSED: listening(face); break;   /* 问号图正好是"没听懂" */
        case QZ_FACE_SURPRISED: listening(face); break;
        case QZ_FACE_HAPPY: happy(face); break;
        case QZ_FACE_LOVE: happy(face); break;
        case QZ_FACE_EXCITED: happy(face); break;
        case QZ_FACE_SLEEPY: sleepy(face); break;
        case QZ_FACE_WINK: wink(face); break;
        case QZ_FACE_IDLE:
        default:
            if (face->idle_index == 1) {
                idle1(face);
                face->idle_index = 2;
            } else {
                idle2(face);
                face->idle_index = 1;
            }
            break;
    }
}

/** 开发用：QZDESK_FACE_CYCLE[=毫秒] 逐个轮播状态，状态号打到 stderr。 */
static void cycle_tick(lv_timer_t *timer)
{
    qz_face_t *face = (qz_face_t *)lv_timer_get_user_data(timer);
    qz_face_state_t next;

    if (!face) return;
    next = (qz_face_state_t)((face->state + 1) % QZ_FACE_STATE_COUNT);
    qz_face_set_state(face->root, next);
    fprintf(stderr, "face cycle -> %d\n", (int)next);
}

static void face_delete(lv_event_t *event)
{
    qz_face_t *face = (qz_face_t *)lv_event_get_user_data(event);

    if (!face) return;
    if (face->dispatch_timer) lv_timer_delete(face->dispatch_timer);
    free(face);
}

/* 一只圆角矩形眼（Echo-Mate 用 lv_button + radius=边长，这里等价实现） */
static lv_obj_t *eye_blob(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *obj = lv_obj_create(parent);

    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, h, 0);          /* h 即半径 = 圆 */
    lv_obj_set_style_bg_color(obj, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

/** 透明容器（面板/装饰底座）。 */
static lv_obj_t *plain_panel(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *obj = lv_obj_create(parent);

    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

lv_obj_t *qz_face_create(lv_obj_t *parent, int32_t size)
{
    qz_face_t *face = (qz_face_t *)calloc(1, sizeof(qz_face_t));
    int32_t s = size;

    if (!face) return NULL;
    face->size = size;
    face->state = QZ_FACE_STATE_COUNT;   /* 非法值起步：让第一次 dispatch 真正跑起来 */
    face->last_state = QZ_FACE_STATE_COUNT;
    face->anim_complete = true;
    face->idle_index = 1;

    face->root = lv_obj_create(parent);
    lv_obj_set_size(face->root, size, size);
    lv_obj_set_style_bg_opa(face->root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(face->root, 0, 0);
    lv_obj_set_style_pad_all(face->root, 0, 0);
    lv_obj_set_style_radius(face->root, 0, 0);
    lv_obj_clear_flag(face->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(face->root, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_user_data(face->root, face);
    lv_obj_add_event_cb(face->root, face_delete, LV_EVENT_DELETE, face);

    /* 眼睛面板（裁剪子对象 -> 闪眼），里面一层纵向漂移面板，再放两只眼 */
    face->eyes_panel = plain_panel(face->root, s * EYE_PANEL_W / 210, s * EYE_PANEL_H / 210);
    lv_obj_set_style_radius(face->eyes_panel, s * EYE_PANEL_H / 420, 0);
    lv_obj_align(face->eyes_panel, LV_ALIGN_CENTER, 0, s * EYE_PANEL_Y / 210);
    face->ver_panel = plain_panel(face->eyes_panel, s * EYE_PANEL_W / 210, s * EYE_PANEL_H / 210);
    lv_obj_center(face->ver_panel);
    for (int i = 0; i < 2; i++) {
        int32_t side = (i == 0) ? -1 : 1;
        face->eye[i] = eye_blob(face->ver_panel, s * EYE_SIZE / 210, s * EYE_SIZE / 210);
        lv_obj_align(face->eye[i], LV_ALIGN_CENTER, side * s * EYE_DX / 210, 0);
        /* Echo-Mate 是深底白眼；我们的卡片是白的，眼睛用主题文字色。必须走
         * qz_obj_set_bg_color 而不是裸 set：这样切换深色模式时眼睛会跟着反白，
         * 否则黑眼落在黑底上，整个表情就“看不见”了。 */
        qz_obj_set_bg_color(face->eye[i], QZ_TEXT, 0);
    }

    /* 嘴（平时透明，说话时出现） */
    face->mouth_panel = plain_panel(face->root, s * 80 / 210, s * 80 / 210);
    lv_obj_align(face->mouth_panel, LV_ALIGN_CENTER, 0, s * MOUTH_PANEL_Y / 210);
    face->mouth = eye_blob(face->mouth_panel, s * MOUTH_SIZE / 210, s * MOUTH_SIZE / 210);
    lv_obj_align(face->mouth, LV_ALIGN_CENTER, 0, s * MOUTH_Y / 210);
    qz_obj_set_bg_color(face->mouth, QZ_TEXT, 0);
    lv_obj_set_style_bg_opa(face->mouth, LV_OPA_TRANSP, 0);

    /* 三张装饰图（来自 Echo-Mate 的 assets） */
    face->question_img = lv_image_create(face->root);
    lv_image_set_src(face->question_img, &ui_img_question60_png);
    lv_obj_align(face->question_img, LV_ALIGN_CENTER, s * 125 / 210, -s * 80 / 210);
    lv_obj_set_style_opa(face->question_img, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(face->question_img, LV_OBJ_FLAG_CLICKABLE);

    face->think_img = lv_image_create(face->root);
    lv_image_set_src(face->think_img, &ui_img_think60_png);
    lv_obj_align(face->think_img, LV_ALIGN_CENTER, s * 120 / 210, -s * 80 / 210);
    lv_obj_set_style_opa(face->think_img, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(face->think_img, LV_OBJ_FLAG_CLICKABLE);

    face->hand_img = lv_image_create(face->root);
    lv_image_set_src(face->hand_img, &ui_img_hand60_png);
    lv_obj_align(face->hand_img, LV_ALIGN_CENTER, 0, s * 55 / 210);
    lv_image_set_rotation(face->hand_img, 2710);   /* 原项目 -350（0.1°），等价角 */
    lv_obj_set_style_opa(face->hand_img, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(face->hand_img, LV_OBJ_FLAG_CLICKABLE);

    face->dispatch_timer = lv_timer_create(dispatch_tick, 250, face);

    /* 开发用：逐个轮播状态 */
    {
        const char *cycle = getenv("QZDESK_FACE_CYCLE");
        if (cycle && cycle[0] != '\0') {
            int32_t dwell = atoi(cycle);
            if (dwell <= 0) dwell = 2500;
            lv_timer_create(cycle_tick, (uint32_t)dwell, face);
        }
    }
    return face->root;
}

/* ------------------------------------------------------------------------- *
 * 状态与 API
 * ------------------------------------------------------------------------- */
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
        case QZ_FACE_IDLE:
        default: return "我在听你说～";
    }
}

void qz_face_trigger_blink(lv_obj_t *root)
{
    qz_face_t *face;
    int32_t h0;

    if (!root) return;
    face = (qz_face_t *)lv_obj_get_user_data(root);
    if (!face) return;
    h0 = lv_obj_get_height(face->eyes_panel);   /* 从当前高度出发，别打断正在播的编排 */
    seq(face->eyes_panel, 0, 100, h0, uw(face, BLINK_H), lv_anim_path_ease_in_out,
        anim_set_height);
    seq(face->eyes_panel, 150, 150, uw(face, BLINK_H), h0, lv_anim_path_ease_in_out,
        anim_set_height);
}

void qz_face_set_state(lv_obj_t *root, qz_face_state_t state)
{
    qz_face_t *face;

    if (!root) return;
    face = (qz_face_t *)lv_obj_get_user_data(root);
    if (!face) return;
    if (state >= QZ_FACE_STATE_COUNT) state = QZ_FACE_IDLE;
    /* 调度器每 250ms 检查一次：这里只记状态，切换动作（reinit + 新编排）在那里做，
     * 和 Echo-Mate 一致 —— 避免在回调里嵌套一长串动画创建。 */
    face->state = state;
}
