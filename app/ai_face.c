/* Mascot face.
 *
 * The character art is pre-baked (see mascot_assets.c, generated from the
 * expression sheet by tools/mk_mascot_assets.py) and shown through an lv_image
 * with `LV_IMAGE_ALIGN_CONTAIN`, so the same 160px asset serves every size the
 * UI uses: 60px in the about card, 88px on the home card and 164px full screen.
 *
 * Expressions are swapped on state changes with a short fade + rise, and a
 * couple of cheap transform free animations keep the character alive
 * (breathing, bobbing, swaying) — no scaled/rotated layers are used anywhere,
 * which keeps the embedded heap happy.
 *
 * Idling also blinks: a timer hides the character's eyes behind two small dark
 * capsules for a fraction of a second every few seconds. 静帧看久了像贴纸，眨眼
 * 是最便宜的一点"活着"的信号。眼皮是矢量覆盖层而不是第二张位图：换帧会让轮廓
 * 抖一下（见 QZ_FACE_EYE_* 那一节）。
 *
 * mascot_assets.c 里烘焙着一张闭眼帧（qz_mascot_blink，来自 assets/mascot/
 * blink.png），目前没有代码引用它 —— 现在的眨眼走矢量眼皮那条路。
 */
#include "ai_face.h"
#include "mascot_assets.h"
#include "theme.h"
#include <stdlib.h>

/* 眨眼节奏：待机时每 3~6 秒闭一次眼，闭住 130ms。
 * 间隔随机而不是固定节拍 —— 固定节拍看两眼就会被认出是循环。 */
#define QZ_BLINK_HOLD_MS 130
#define QZ_BLINK_MIN_MS 3000
#define QZ_BLINK_MAX_MS 6200
#define QZ_BLINK_RETRY_MS 900

/* ------------------------------------------------------------------------- *
 * 形象相关：换宠物形象时，这一节 + 重新烘焙素材就是全部要动的地方
 *
 * 眼球位置取自当前形象（小狗），所以换形象后要按新形象的眼睛重新量这四个
 * 百分比和眼皮颜色 —— 否则眨眼的两条深色胶囊会盖在脸颊上、颜色也不对。
 * 都是画布百分比，与烘焙尺寸无关（192px 或以后改尺寸都不受影响）。
 *
 * 量法：在表情原图（正方形、主体居中）上量两眼外缘总宽、眼睛所在行与画布中
 * 心的偏移，除以画布边长即可。
 * ------------------------------------------------------------------------- */
#define QZ_FACE_EYE_SPAN_PCT 42    /* 两眼外缘的总宽（画布宽的百分比） */
#define QZ_FACE_EYE_W_PCT 9        /* 单只眼睛的宽 */
#define QZ_FACE_EYE_H_PCT 3        /* 单只眼睛的高：闭眼时眼皮的厚度 */
#define QZ_FACE_EYE_ROW_PCT (-6)   /* 眼睛那行相对画布中心的高度偏移，正数向下 */
#define QZ_FACE_EYE_COLOR 0x75442f /* 眼皮颜色：取形象眼/眉的深色（小狗是棕） */

typedef struct {
    lv_obj_t *root;
    lv_obj_t *image;
    lv_obj_t *blink_eyes;
    lv_obj_t *dots[3];
    lv_timer_t *blink_timer;
    int32_t size;
    qz_face_state_t state;
    bool blinking;
} qz_face_t;

/* ------------------------------------------------------------------------- *
 * Animation helpers
 * ------------------------------------------------------------------------- */

static void anim_opa(void *var, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)value, 0);
}

static void anim_translate_y(void *var, int32_t value)
{
    lv_obj_set_style_translate_y((lv_obj_t *)var, value, 0);
}

static void anim_translate_x(void *var, int32_t value)
{
    lv_obj_set_style_translate_x((lv_obj_t *)var, value, 0);
}

static void stop_anims(qz_face_t *face)
{
    lv_anim_delete(face->root, NULL);
    lv_anim_delete(face->image, NULL);
    for (int i = 0; i < 3; i++) lv_anim_delete(face->dots[i], NULL);
}

/* Idle loops are deliberately slow: they are the character breathing, not UI
 * feedback. They are dropped entirely when the user asked for reduced motion. */
static void loop_anim(lv_obj_t *obj, lv_anim_exec_xcb_t cb, int32_t from, int32_t to,
                      uint32_t duration)
{
    if (qz_reduce_motion()) {
        lv_obj_set_style_translate_x(obj, 0, 0);
        lv_obj_set_style_translate_y(obj, 0, 0);
        return;
    }
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_exec_cb(&anim, cb);
    lv_anim_set_values(&anim, from, to);
    lv_anim_set_duration(&anim, duration);
    lv_anim_set_playback_duration(&anim, duration);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    qz_anim_ease_in_out(&anim);
    lv_anim_start(&anim);
}

static void pulse_anim(lv_obj_t *obj, int32_t from, int32_t to, uint32_t duration,
                       uint32_t delay)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_exec_cb(&anim, anim_opa);
    lv_anim_set_values(&anim, from, to);
    lv_anim_set_duration(&anim, duration);
    lv_anim_set_delay(&anim, delay);
    lv_anim_set_playback_duration(&anim, duration);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_start(&anim);
}

/* ------------------------------------------------------------------------- *
 * Construction
 * ------------------------------------------------------------------------- */

static const lv_image_dsc_t *state_image(qz_face_state_t state)
{
    switch (state) {
        case QZ_FACE_HAPPY: return &qz_mascot_happy;
        case QZ_FACE_THINKING: return &qz_mascot_thinking;
        case QZ_FACE_CONFUSED: return &qz_mascot_confused;
        case QZ_FACE_LOVE: return &qz_mascot_love;
        case QZ_FACE_SPEAKING: return &qz_mascot_speaking;
        case QZ_FACE_IDLE:
        default: return &qz_mascot_idle;
    }
}

static int32_t blink_interval_ms(void)
{
    int32_t span = QZ_BLINK_MAX_MS - QZ_BLINK_MIN_MS;
    return QZ_BLINK_MIN_MS + (int32_t)(rand() % (span + 1));
}

/* 眨眼只换图，不参与状态机：定时器每跳一次在"睁/闭"之间翻一下，节奏靠自身
 * period 调整，不需要第二只定时器。 */
static void blink_tick(lv_timer_t *timer)
{
    qz_face_t *face = (qz_face_t *)lv_timer_get_user_data(timer);
    if (!face) return;

    if (face->blinking) {
        face->blinking = false;
        lv_obj_add_flag(face->blink_eyes, LV_OBJ_FLAG_HIDDEN);
        lv_timer_set_period(timer, blink_interval_ms());
        return;
    }

    /* 说话 / 思考 / 开心各有自己的动作，眨眼叠上去会打架；减少动态时干脆不眨 */
    if (qz_reduce_motion() || face->state != QZ_FACE_IDLE) {
        lv_timer_set_period(timer, QZ_BLINK_RETRY_MS);
        return;
    }
    face->blinking = true;
    lv_obj_clear_flag(face->blink_eyes, LV_OBJ_FLAG_HIDDEN);
    lv_timer_set_period(timer, QZ_BLINK_HOLD_MS);
}

static void face_delete(lv_event_t *event)
{
    qz_face_t *face = (qz_face_t *)lv_event_get_user_data(event);
    /* 定时器挂在 face 上，必须先停掉再释放，否则回调会踩到已释放的内存 */
    if (face->blink_timer) lv_timer_delete(face->blink_timer);
    free(face);
}

lv_obj_t *qz_face_create(lv_obj_t *parent, int32_t size)
{
    qz_face_t *face = (qz_face_t *)calloc(1, sizeof(qz_face_t));
    if (!face) return NULL;
    face->size = size;
    face->state = QZ_FACE_IDLE;

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

    face->image = lv_image_create(face->root);
    lv_image_set_src(face->image, state_image(QZ_FACE_IDLE));
    lv_image_set_inner_align(face->image, LV_IMAGE_ALIGN_CENTER);
    lv_obj_set_size(face->image, size, size);
    lv_obj_center(face->image);
    /* The baked art is square, so one uniform zoom fits every usage (60/88/164).
     * Image scaling is done inline by the renderer — unlike lv_obj transforms it
     * does not allocate a layer buffer.
     *
     * 无条件设置：面板比设计稿小时，外框已经缩了（见 include/scale.h），内容若
     * 停在 1:1 就会被居中裁掉 —— 那正是"图片显示不全"。zoom 按设计像素算，
     * 再由 scale.h 缩到面板。 */
    lv_image_set_scale(face->image, (uint32_t)(256 * size / QZ_MASCOT_SIZE));
    lv_obj_clear_flag(face->image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(face->image, LV_OBJ_FLAG_SCROLLABLE);

    /* Keep the artwork fixed during a blink. The reference UI animates separate
     * eye objects; swapping a second full mascot frame shifts the silhouette.
     * 尺寸与位置都来自 QZ_FACE_EYE_*（换形象时改那里）。 */
    face->blink_eyes = lv_obj_create(face->root);
    lv_obj_set_size(face->blink_eyes, size * QZ_FACE_EYE_SPAN_PCT / 100,
                    size * QZ_FACE_EYE_H_PCT / 100);
    lv_obj_align(face->blink_eyes, LV_ALIGN_CENTER, 0, size * QZ_FACE_EYE_ROW_PCT / 100);
    lv_obj_set_style_bg_opa(face->blink_eyes, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(face->blink_eyes, 0, 0);
    lv_obj_set_style_pad_all(face->blink_eyes, 0, 0);
    lv_obj_clear_flag(face->blink_eyes, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(face->blink_eyes, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(face->blink_eyes, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 2; i++) {
        int32_t eye_w = size * QZ_FACE_EYE_W_PCT / 100;
        int32_t eye_h = size * QZ_FACE_EYE_H_PCT / 100;
        lv_obj_t *eye = lv_obj_create(face->blink_eyes);
        lv_obj_set_size(eye, eye_w, eye_h);
        lv_obj_align(eye, i == 0 ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(eye, lv_color_hex(QZ_FACE_EYE_COLOR), 0);
        lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(eye, 0, 0);
        lv_obj_clear_flag(eye, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(eye, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* Small vector "thought dots" on top of the art, used while thinking. */
    for (int i = 0; i < 3; i++) {
        int32_t dot = size * 8 / 100;
        if (dot < 3) dot = 3;
        lv_obj_t *bubble = lv_obj_create(face->root);
        lv_obj_set_size(bubble, dot, dot);
        lv_obj_align(bubble, LV_ALIGN_TOP_RIGHT,
                     -(int32_t)(size * 7 / 100) - i * (dot * 3 / 2),
                     (int32_t)(size * 6 / 100));
        lv_obj_set_style_radius(bubble, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(bubble, qz_color(QZ_FACE_BUBBLE), 0);
        lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bubble, 0, 0);
        lv_obj_clear_flag(bubble, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_opa(bubble, LV_OPA_TRANSP, 0);
        face->dots[i] = bubble;
    }

    /* 眨眼：换的是图片本身，和上面的状态动画互不干扰 */
    face->blink_timer = lv_timer_create(blink_tick, blink_interval_ms(), face);

    qz_face_set_state(face->root, QZ_FACE_IDLE);
    return face->root;
}

/* ------------------------------------------------------------------------- *
 * States
 * ------------------------------------------------------------------------- */

const char *qz_face_state_text(qz_face_state_t state)
{
    switch (state) {
        case QZ_FACE_SPEAKING: return "我在认真回答～";
        case QZ_FACE_THINKING: return "让我想一想…";
        case QZ_FACE_HAPPY: return "好开心见到你！";
        case QZ_FACE_CONFUSED: return "我好像没听懂…";
        case QZ_FACE_LOVE: return "好喜欢你呀！";
        case QZ_FACE_IDLE:
        default: return "我在听你说～";
    }
}

void qz_face_set_state(lv_obj_t *root, qz_face_state_t state)
{
    if (!root) return;
    qz_face_t *face = (qz_face_t *)lv_obj_get_user_data(root);
    if (!face) return;

    int32_t size = face->size;
    stop_anims(face);
    face->state = state;
    /* 状态切换自己会换图，进行中的那一次眨眼就此作废 */
    face->blinking = false;
    lv_obj_add_flag(face->blink_eyes, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_opa(face->dots[i], LV_OPA_TRANSP, 0);
    }
    /* Settle the sway of the previous state instead of snapping it away: a new
     * state can arrive mid movement. */
    int32_t sway_x = lv_obj_get_style_translate_x(face->root, 0);
    int32_t sway_y = lv_obj_get_style_translate_y(face->root, 0);
    if ((sway_x != 0 || sway_y != 0) && state != QZ_FACE_CONFUSED) {
        lv_anim_t settle_x;
        lv_anim_init(&settle_x);
        lv_anim_set_var(&settle_x, face->root);
        lv_anim_set_exec_cb(&settle_x, anim_translate_x);
        lv_anim_set_values(&settle_x, sway_x, 0);
        lv_anim_set_duration(&settle_x, QZ_DUR_SMALL);
        qz_anim_ease_out(&settle_x);
        lv_anim_start(&settle_x);

        lv_anim_t settle_y;
        lv_anim_init(&settle_y);
        lv_anim_set_var(&settle_y, face->root);
        lv_anim_set_exec_cb(&settle_y, anim_translate_y);
        lv_anim_set_values(&settle_y, sway_y, 0);
        lv_anim_set_duration(&settle_y, QZ_DUR_SMALL);
        qz_anim_ease_out(&settle_y);
        lv_anim_start(&settle_y);
    }

    /* Swap the expression, then fade + rise it in so the change reads as a
     * gesture. Both start from the live value, so an interrupted transition
     * continues instead of jumping. */
    lv_image_set_src(face->image, state_image(state));
    lv_anim_t fade;
    lv_anim_init(&fade);
    lv_anim_set_var(&fade, face->image);
    lv_anim_set_exec_cb(&fade, anim_opa);
    lv_anim_set_values(&fade, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&fade, QZ_DUR_SMALL);
    qz_anim_ease_out(&fade);
    lv_anim_start(&fade);

    int32_t rise_distance = size * 5 / 100;
    lv_anim_t rise;
    lv_anim_init(&rise);
    lv_anim_set_var(&rise, face->image);
    lv_anim_set_exec_cb(&rise, anim_translate_y);
    lv_anim_set_values(&rise, lv_obj_get_style_translate_y(face->image, 0) + rise_distance, 0);
    lv_anim_set_duration(&rise, QZ_DUR_PANEL);
    qz_anim_ease_out(&rise);
    lv_anim_start(&rise);

    switch (state) {
        case QZ_FACE_SPEAKING:
            /* Quick bob, as if the character is talking. */
            loop_anim(face->image, anim_translate_y, 0, -size * 2 / 100, 420);
            break;

        case QZ_FACE_THINKING:
            /* Slow breathing plus floating thought dots. */
            loop_anim(face->image, anim_translate_y, 0, -size * 2 / 100, 1800);
            for (int i = 0; i < 3; i++) {
                pulse_anim(face->dots[i], LV_OPA_20, LV_OPA_COVER, 520, (uint32_t)(i * 180));
            }
            break;

        case QZ_FACE_HAPPY:
            /* Little hop on top of the entrance. */
            loop_anim(face->image, anim_translate_y, 0, -size * 3 / 100, 520);
            break;

        case QZ_FACE_CONFUSED:
            /* Sideways sway of a puzzled head. */
            loop_anim(face->root, anim_translate_x, -size * 2 / 100, size * 2 / 100, 1200);
            break;

        case QZ_FACE_IDLE:
        default:
            loop_anim(face->root, anim_translate_y, 0, -size * 2 / 100, 2000);
            break;
    }
}
