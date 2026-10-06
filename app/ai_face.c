/* Mascot face —— 位图表情 + 矢量装饰层 + 逐表情动作配方。
 *
 * 形象本身是烘焙好的位图（见 mascot_assets.c，由 tools/mk_mascot_assets.py 从
 * assets/mascot/*.png 生成），用 lv_image 显示，一份资源伺候三处尺寸：60px（关于
 * 卡片）、88px（主页大卡片）、164px（全屏表情）。
 *
 * 表情"活"起来的做法参考了 0015/lvgl_kawaii_face 的思路，但按本工程的约束换了实现：
 *
 *   它                          这里
 *   lv_canvas 全量重绘 + 33fps 常驻定时器   位图 + 十几个小对象的 lv_anim 循环（静止时不耗）
 *   逐表情的 sine/方波配方        qz_face_recipe_t 表：起伏/摇摆/抖动/缩放呼吸 + 装饰强度
 *   星光、汗滴、眼泪、zzz         同一套思路，用矢量小对象叠在图上（不改美术）
 *   face_set_eye_openness(l, r)  两条"眼皮"覆盖层的开合度（眨眼、困倦、单眼闭共用）
 *
 * 不采用它的 canvas 路线：那需要三块画布缓冲并每帧全量重画，在 RV1106 这种软件渲染
 * 的板子上是纯浪费；这里只用 translate / size / opa，不申请图层缓冲。
 *
 * 面部锚点（QZ_FACE_EYE_*）仍然是从当前形象上量出来的：换形象后要重量一遍，否则
 * 眼皮会盖在脸颊上。mascot_assets.c 里那张闭眼帧（qz_mascot_blink）没有代码引用，
 * 眨眼走的是这里的矢量眼皮。
 */
#include "ai_face.h"
#include "mascot_assets.h"
#include "theme.h"
#include <stdio.h>
#include <stdlib.h>

/* 眨眼节奏：待机时每 3~6 秒闭一次眼，闭合来回约 200ms（眼皮高度补间，不是隐藏/显示，
 * 所以看起来是"压下来"而不是"啪一下不见"）。间隔随机：固定节拍会被一眼认出是循环。 */
#define QZ_BLINK_DOWN_MS 80
#define QZ_BLINK_UP_MS 120
#define QZ_BLINK_MIN_MS 3000
#define QZ_BLINK_MAX_MS 6200
#define QZ_BLINK_RETRY_MS 900

/* ------------------------------------------------------------------------- *
 * 形象相关：换宠物形象时，这一节 + 重新烘焙素材就是全部要动的地方
 *
 * 眼球位置取自当前形象（小狗），所以换形象后要按新形象的眼睛重新量这四个
 * 百分比和眼皮颜色 —— 否则眼皮的两条深色胶囊会盖在脸颊上、颜色也不对。
 * 都是画布百分比，与烘焙尺寸无关（192px 或以后改尺寸都不受影响）。
 *
 * 量法：在表情原图（正方形、主体居中）上量两眼外缘总宽、眼睛所在行与画布中
 * 心的偏移，除以画布边长即可。
 * ------------------------------------------------------------------------- */
#define QZ_FACE_EYE_SPAN_PCT 42    /* 两眼外缘的总宽（画布宽的百分比） */
#define QZ_FACE_EYE_W_PCT 9        /* 单只眼睛的宽 */
#define QZ_FACE_EYE_H_PCT 3        /* 单只眼睛的高：全闭时眼皮的厚度 */
#define QZ_FACE_EYE_ROW_PCT (-6)   /* 眼睛那行相对画布中心的高度偏移，正数向下 */
#define QZ_FACE_EYE_COLOR 0x75442f /* 眼皮颜色：取形象眼/眉的深色（小狗是棕） */

/* 装饰用色（参考项目里是黄星光、蓝汗滴；这里只用在叠加的小对象上，不属于美术） */
#define QZ_SPARKLE_COLOR 0xFFD24A
#define QZ_SWEAT_COLOR 0x8FD3FF

/* ------------------------------------------------------------------------- *
 * 表情配方：一张表决定"这个表情怎么动、亮什么装饰"
 *
 * 数值都按面部尺寸的千分比给，换尺寸不用改。参考项目的做法是每个表情一组
 * 正弦/方波参数，这里一样，只是把它落到 translate / image zoom / 覆盖层上。
 * ------------------------------------------------------------------------- */
typedef struct {
    int32_t bounce;     /* 纵向起伏幅度（‰），0 = 不上下动 */
    int32_t bounce_ms;  /* 单程时长：越小越急 */
    int32_t sway;       /* 横向摇摆幅度（‰），>0 时用摇摆代替起伏（困惑那种） */
    int32_t sway_ms;
    int32_t jitter;     /* 方波抖动幅度（‰）：惊讶/兴奋那种"抖一下" */
    int32_t jitter_ms;  /* 方波半周期 */
    int32_t breathe;    /* 图片缩放的呼吸幅度（256 基准的千分比），0 = 不缩放 */
    int32_t lid_l;      /* 常态眼皮闭合度（‰ of 眼睛高） */
    int32_t lid_r;      /* 右眼单独再闭（单眼眨） */
    int32_t sparkle;    /* 星光不透明度 0..255 */
    int32_t sweat;      /* 汗滴不透明度 0..255 */
    int32_t zzz;        /* zzz 不透明度 0..255 */
    bool dots;          /* 思考气泡 */
} qz_face_recipe_t;

static const qz_face_recipe_t k_recipe[QZ_FACE_STATE_COUNT] = {
    /*                  bounce b_ms sway s_ms jit j_ms brth lid_l lid_r spark sweat zzz dots */
    [QZ_FACE_IDLE]     = {   20, 2000,   0,   0,   0,   0,    0,    0,    0,    0,    0,   0, false },
    [QZ_FACE_SPEAKING] = {   14,  420,   0,   0,   0,   0,    8,    0,    0,    0,    0,   0, false },
    [QZ_FACE_THINKING] = {   18, 1800,   0,   0,   0,   0,    0,    0,    0,    0,    0,   0, true  },
    [QZ_FACE_HAPPY]    = {   30,  460,   0,   0,   0,   0,   16,    0,    0,  200,    0,   0, false },
    [QZ_FACE_CONFUSED] = {    0,    0,  18,1200,   0,   0,    0,    0,    0,    0,  160,   0, false },
    [QZ_FACE_LOVE]     = {   20,  900,   0,   0,   0,   0,   12,    0,    0,  220,    0,   0, false },
    [QZ_FACE_SURPRISED]= {    0,    0,   0,   0,  16,  90,   24,    0,    0,  120,    0,   0, false },
    [QZ_FACE_SLEEPY]   = {   30, 3400,   0,   0,   0,   0,    0,  620,  620,    0,    0, 200, false },
    [QZ_FACE_WINK]     = {   15,  620,   0,   0,   0,   0,    0,    0,  900,  180,    0,   0, false },
    [QZ_FACE_EXCITED]  = {   34,  380,   0,   0,   0,   0,   20,    0,    0,  255,    0,   0, false },
};

typedef struct {
    lv_obj_t *root;
    lv_obj_t *image;
    lv_obj_t *lid_box;      /* 眼皮容器（定位用，本身透明） */
    lv_obj_t *lid[2];       /* 左右眼皮胶囊：高度即开合度 */
    lv_obj_t *dots[3];
    lv_obj_t *sparkle[3];   /* 星光：两处绕着头上转，一晃一晃 */
    lv_obj_t *sweat;
    lv_obj_t *zzz[2];
    lv_timer_t *blink_timer;
    lv_timer_t *jitter_timer;
    int32_t size;
    qz_face_state_t state;
    bool blinking;
    int32_t jitter_sign;
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

/** 图片缩放的呼吸/弹跳：zoom 走内联采样，不申请图层缓冲。 */
static void anim_image_zoom(void *var, int32_t value)
{
    lv_obj_t *image = (lv_obj_t *)var;
    int32_t base = (int32_t)(intptr_t)lv_obj_get_user_data(image);
    lv_image_set_scale(image, (uint32_t)(base + value));
}

/** 眼皮高度：0 = 睁开，满 = 闭合。高度为 0 时整条透明，免得留下一条线。 */
static void anim_lid(void *var, int32_t value)
{
    lv_obj_t *lid = (lv_obj_t *)var;
    lv_obj_set_height(lid, value);
    lv_obj_set_style_opa(lid, value < 2 ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
}

static void stop_anims(qz_face_t *face)
{
    lv_anim_delete(face->root, NULL);
    lv_anim_delete(face->image, NULL);
    for (int i = 0; i < 2; i++) lv_anim_delete(face->lid[i], NULL);
    for (int i = 0; i < 3; i++) lv_anim_delete(face->dots[i], NULL);
    for (int i = 0; i < 3; i++) lv_anim_delete(face->sparkle[i], NULL);
    lv_anim_delete(face->sweat, NULL);
    for (int i = 0; i < 2; i++) lv_anim_delete(face->zzz[i], NULL);
    if (face->jitter_timer) {
        lv_timer_delete(face->jitter_timer);
        face->jitter_timer = NULL;
    }
}

/* Idle loops are deliberately slow: they are the character breathing, not UI
 * feedback. They are dropped entirely when the user asked for reduced motion. */
static void loop_anim(lv_obj_t *obj, lv_anim_exec_xcb_t cb, int32_t from, int32_t to,
                      uint32_t duration, uint32_t delay)
{
    lv_anim_t anim;

    if (qz_reduce_motion() || duration == 0) return;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_exec_cb(&anim, cb);
    lv_anim_set_values(&anim, from, to);
    lv_anim_set_duration(&anim, duration);
    if (delay) lv_anim_set_delay(&anim, delay);
    lv_anim_set_playback_duration(&anim, duration);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    qz_anim_ease_in_out(&anim);
    lv_anim_start(&anim);
}

static void pulse_anim(lv_obj_t *obj, int32_t from, int32_t to, uint32_t duration,
                       uint32_t delay)
{
    lv_anim_t anim;

    if (qz_reduce_motion()) return;
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

static const qz_face_recipe_t *recipe_of(qz_face_state_t state)
{
    return (state < QZ_FACE_STATE_COUNT) ? &k_recipe[state] : &k_recipe[QZ_FACE_IDLE];
}

static const lv_image_dsc_t *state_image(qz_face_state_t state)
{
    switch (state) {
        case QZ_FACE_HAPPY: return &qz_mascot_happy;
        case QZ_FACE_THINKING: return &qz_mascot_thinking;
        case QZ_FACE_CONFUSED: return &qz_mascot_confused;
        case QZ_FACE_LOVE: return &qz_mascot_love;
        case QZ_FACE_SPEAKING: return &qz_mascot_speaking;
        case QZ_FACE_SURPRISED: return &qz_mascot_happy;   /* 眯眼笑那张最适合当"惊讶" */
        case QZ_FACE_SLEEPY: return &qz_mascot_idle;
        case QZ_FACE_WINK: return &qz_mascot_happy;
        case QZ_FACE_EXCITED: return &qz_mascot_speaking;
        case QZ_FACE_IDLE:
        default: return &qz_mascot_idle;
    }
}

static int32_t blink_interval_ms(void)
{
    int32_t span = QZ_BLINK_MAX_MS - QZ_BLINK_MIN_MS;
    return QZ_BLINK_MIN_MS + (int32_t)(rand() % (span + 1));
}

/** 眼皮的"常态开合度"：表情给的基线（困倦半闭、单眼闭），眨眼在此之上再压。 */
static void apply_lids(qz_face_t *face)
{
    const qz_face_recipe_t *recipe = recipe_of(face->state);
    int32_t full = face->size * QZ_FACE_EYE_H_PCT / 100;

    lv_obj_set_height(face->lid[0], full * recipe->lid_l / 1000);
    lv_obj_set_height(face->lid[1], full * (recipe->lid_r ? recipe->lid_r : recipe->lid_l) / 1000);
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_opa(face->lid[i],
                             lv_obj_get_height(face->lid[i]) < 2 ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    }
}

static void blink_tick(lv_timer_t *timer)
{
    qz_face_t *face = (qz_face_t *)lv_timer_get_user_data(timer);
    const qz_face_recipe_t *recipe;
    int32_t full;

    if (!face) return;

    if (face->blinking) {
        /* 睁开：回到这个表情自己的常态开合度 */
        face->blinking = false;
        apply_lids(face);
        lv_timer_set_period(timer, blink_interval_ms());
        return;
    }

    /* 说话 / 思考 / 开心各有自己的动作，眨眼叠上去会打架；困倦本来就不该睁着。
     * 减少动态时干脆不眨。 */
    if (qz_reduce_motion() || face->state != QZ_FACE_IDLE) {
        lv_timer_set_period(timer, QZ_BLINK_RETRY_MS);
        return;
    }

    face->blinking = true;
    recipe = recipe_of(face->state);
    full = face->size * QZ_FACE_EYE_H_PCT / 100;
    for (int i = 0; i < 2; i++) {
        lv_anim_t anim;
        lv_anim_init(&anim);
        lv_anim_set_var(&anim, face->lid[i]);
        lv_anim_set_exec_cb(&anim, anim_lid);
        lv_anim_set_values(&anim, lv_obj_get_height(face->lid[i]), full);
        lv_anim_set_duration(&anim, QZ_BLINK_DOWN_MS);
        lv_anim_set_playback_duration(&anim, QZ_BLINK_UP_MS);
        qz_anim_ease_in_out(&anim);
        lv_anim_start(&anim);
    }
    lv_timer_set_period(timer, QZ_BLINK_DOWN_MS + QZ_BLINK_UP_MS + 60);
    (void)recipe;
}

/** 方波抖动：正弦做不出"受惊/使劲"那种一卡一卡的感觉（参考项目也用方波）。 */
static void jitter_tick(lv_timer_t *timer)
{
    qz_face_t *face = (qz_face_t *)lv_timer_get_user_data(timer);
    const qz_face_recipe_t *recipe;

    if (!face) return;
    recipe = recipe_of(face->state);
    if (recipe->jitter == 0) return;
    face->jitter_sign = -face->jitter_sign;
    lv_obj_set_style_translate_x(face->root, face->jitter_sign * face->size * recipe->jitter / 1000, 0);
}

static void face_delete(lv_event_t *event)
{
    qz_face_t *face = (qz_face_t *)lv_event_get_user_data(event);
    /* 定时器挂在 face 上，必须先停掉再释放，否则回调会踩到已释放的内存 */
    if (face->blink_timer) lv_timer_delete(face->blink_timer);
    if (face->jitter_timer) lv_timer_delete(face->jitter_timer);
    free(face);
}

/** 开发用：QZDESK_FACE_CYCLE[=毫秒] 逐个轮播表情，并把状态号打到 stderr。 */
static void cycle_tick(lv_timer_t *timer)
{
    qz_face_t *face = (qz_face_t *)lv_timer_get_user_data(timer);
    qz_face_state_t next;

    if (!face) return;
    next = (qz_face_state_t)((face->state + 1) % QZ_FACE_STATE_COUNT);
    qz_face_set_state(face->root, next);
    fprintf(stderr, "face cycle -> %d\n", (int)next);
}

static lv_obj_t *blob(lv_obj_t *parent, uint32_t color, lv_opa_t opa)
{
    lv_obj_t *obj = lv_obj_create(parent);

    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_opa(obj, LV_OPA_TRANSP, 0);
    return obj;
}

lv_obj_t *qz_face_create(lv_obj_t *parent, int32_t size)
{
    qz_face_t *face = (qz_face_t *)calloc(1, sizeof(qz_face_t));

    if (!face) return NULL;
    face->size = size;
    /* 用一个非法值起步：qz_face_set_state 会忽略"同状态"的重复调用，若这里先写成
     * IDLE，下面那次真正进入 idle 的调用会被挡掉，呼吸与装饰就都不会启动。 */
    face->state = QZ_FACE_STATE_COUNT;
    face->jitter_sign = 1;

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
     * 再由 scale.h 缩到面板。基准值留在 user_data 里，呼吸动画在它之上加减。 */
    lv_obj_set_user_data(face->image, (void *)(intptr_t)(256 * size / QZ_MASCOT_SIZE));
    lv_image_set_scale(face->image, (uint32_t)(256 * size / QZ_MASCOT_SIZE));
    lv_obj_clear_flag(face->image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(face->image, LV_OBJ_FLAG_SCROLLABLE);

    /* 眼皮：一条容器定位在"眼睛那行"，里面两条胶囊的**高度**就是开合度 ——
     * 眨眼、困倦半闭、单眼眨都用这一套（对应参考项目的 face_set_eye_openness）。 */
    face->lid_box = lv_obj_create(face->root);
    lv_obj_set_size(face->lid_box, size * QZ_FACE_EYE_SPAN_PCT / 100,
                    size * QZ_FACE_EYE_H_PCT / 100);
    lv_obj_align(face->lid_box, LV_ALIGN_CENTER, 0, size * QZ_FACE_EYE_ROW_PCT / 100);
    lv_obj_set_style_bg_opa(face->lid_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(face->lid_box, 0, 0);
    lv_obj_set_style_pad_all(face->lid_box, 0, 0);
    lv_obj_clear_flag(face->lid_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(face->lid_box, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 2; i++) {
        int32_t eye_w = size * QZ_FACE_EYE_W_PCT / 100;
        int32_t eye_h = size * QZ_FACE_EYE_H_PCT / 100;
        lv_obj_t *lid = lv_obj_create(face->lid_box);
        lv_obj_set_size(lid, eye_w, eye_h);
        lv_obj_align(lid, i == 0 ? LV_ALIGN_LEFT_MID : LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_radius(lid, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(lid, lv_color_hex(QZ_FACE_EYE_COLOR), 0);
        lv_obj_set_style_bg_opa(lid, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(lid, 0, 0);
        lv_obj_clear_flag(lid, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(lid, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_opa(lid, LV_OPA_TRANSP, 0);
        face->lid[i] = lid;
    }

    /* 思考气泡：右上角三点，只在 thinking 时脉冲 */
    for (int i = 0; i < 3; i++) {
        int32_t dot = size * 8 / 100;
        if (dot < 3) dot = 3;
        lv_obj_t *bubble = blob(face->root, 0xE4EFFF, LV_OPA_COVER);
        lv_obj_set_size(bubble, dot, dot);
        lv_obj_align(bubble, LV_ALIGN_TOP_RIGHT,
                     -(int32_t)(size * 7 / 100) - i * (dot * 3 / 2),
                     (int32_t)(size * 6 / 100));
        face->dots[i] = bubble;
    }

    /* 星光：三点绕着头部转（两点在上、一点在右上），轨迹用两条错相 90° 的
     * translate 循环拼出来 —— 和参考项目的星光环绕同一思路，但不占定时器。 */
    {
        static const int32_t sx[3] = { 130, 860, 620 };
        static const int32_t sy[3] = { 170, 230, 60 };
        for (int i = 0; i < 3; i++) {
            int32_t d = size * (i == 0 ? 9 : 7) / 100;
            lv_obj_t *star = blob(face->root, QZ_SPARKLE_COLOR, LV_OPA_COVER);
            lv_obj_set_size(star, d, d);
            lv_obj_set_style_radius(star, d / 3, 0);
            lv_obj_align(star, LV_ALIGN_TOP_LEFT, size * sx[i] / 1000, size * sy[i] / 1000);
            face->sparkle[i] = star;
        }
    }

    /* 汗滴（右上）与 zzz（右上角两点，困倦时飘） */
    face->sweat = blob(face->root, QZ_SWEAT_COLOR, LV_OPA_COVER);
    lv_obj_set_size(face->sweat, size * 7 / 100, size * 10 / 100);
    lv_obj_set_style_radius(face->sweat, size * 4 / 100, 0);
    lv_obj_align(face->sweat, LV_ALIGN_TOP_RIGHT, -(int32_t)(size * 12 / 100), size * 12 / 100);

    for (int i = 0; i < 2; i++) {
        lv_obj_t *z = qz_text(face->root, "z", i == 0 ? 12 : 9, qz_color(QZ_TEXT_SECONDARY));
        lv_obj_align(z, LV_ALIGN_TOP_RIGHT, -(int32_t)(size * (i == 0 ? 10 : 22) / 100),
                     size * (i == 0 ? 10 : 0) / 100);
        face->zzz[i] = z;
    }

    face->blink_timer = lv_timer_create(blink_tick, blink_interval_ms(), face);
    face->jitter_timer = lv_timer_create(jitter_tick, 90, face);

    qz_face_set_state(face->root, QZ_FACE_IDLE);

    /* 开发用：QZDESK_FACE_CYCLE[=毫秒] 逐个轮播表情，并把状态号打到 stderr ——
     * 抓帧脚本据此按状态存图，核对每个表情的配方。 */
    {
        const char *cycle = getenv("QZDESK_FACE_CYCLE");
        if (cycle && cycle[0] != '\0') {
            int32_t dwell = atoi(cycle);
            if (dwell <= 0) dwell = 1800;
            lv_timer_create(cycle_tick, (uint32_t)dwell, face);
        }
    }
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
        case QZ_FACE_SURPRISED: return "咦？";
        case QZ_FACE_SLEEPY: return "我有点困了…";
        case QZ_FACE_WINK: return "嘿，我在这儿～";
        case QZ_FACE_EXCITED: return "太棒了！";
        case QZ_FACE_IDLE:
        default: return "我在听你说～";
    }
}

/** 装饰层：按配方把星光/汗滴/zzz/思考气泡开起来或收掉。 */
static void apply_decorations(qz_face_t *face)
{
    const qz_face_recipe_t *recipe = recipe_of(face->state);
    int32_t size = face->size;

    for (int i = 0; i < 3; i++) {
        lv_obj_t *star = face->sparkle[i];

        if (recipe->sparkle == 0) {
            lv_obj_set_style_opa(star, LV_OPA_TRANSP, 0);
            continue;
        }
        /* 两个方向周期不同 -> 走 Lissajous 轨迹，比圆周更像"飘" */
        loop_anim(star, anim_translate_x, -size * 3 / 100, size * 3 / 100, 1400 + (uint32_t)i * 160, 0);
        loop_anim(star, anim_translate_y, -size * 3 / 100, size * 3 / 100, 1900 + (uint32_t)i * 120, 200);
        pulse_anim(star, 70, recipe->sparkle, 600 + (uint32_t)i * 90, (uint32_t)i * 120);
    }

    if (recipe->sweat) {
        /* 汗滴：往下滚一段再回到眉上，方波式地"滴" */
        lv_obj_set_style_opa(face->sweat, (lv_opa_t)recipe->sweat, 0);
        loop_anim(face->sweat, anim_translate_y, 0, size * 14 / 100, 1100, 0);
        loop_anim(face->sweat, anim_translate_x, 0, size * 2 / 100, 1700, 0);
    } else {
        lv_obj_set_style_opa(face->sweat, LV_OPA_TRANSP, 0);
    }

    for (int i = 0; i < 2; i++) {
        lv_obj_t *z = face->zzz[i];

        if (recipe->zzz == 0) {
            lv_obj_set_style_opa(z, LV_OPA_TRANSP, 0);
            continue;
        }
        loop_anim(z, anim_translate_y, 0, -size * 8 / 100, 1600 + (uint32_t)i * 300, (uint32_t)i * 400);
        pulse_anim(z, 40, (int32_t)recipe->zzz, 900 + (uint32_t)i * 200, (uint32_t)i * 400);
    }

    for (int i = 0; i < 3; i++) {
        lv_obj_t *dot = face->dots[i];

        if (recipe->dots) {
            pulse_anim(dot, LV_OPA_20, LV_OPA_COVER, 520, (uint32_t)i * 180);
        } else {
            lv_obj_set_style_opa(dot, LV_OPA_TRANSP, 0);
        }
    }
}

void qz_face_set_state(lv_obj_t *root, qz_face_state_t state)
{
    qz_face_t *face;
    const qz_face_recipe_t *recipe;
    int32_t size;
    int32_t sway_x;
    int32_t sway_y;

    if (!root) return;
    face = (qz_face_t *)lv_obj_get_user_data(root);
    if (!face) return;
    /* 同一个状态重复设置直接忽略：状态报文每秒都来，重放一次淡入会闪。 */
    if (face->state == state) return;

    size = face->size;
    stop_anims(face);
    face->state = state;
    recipe = recipe_of(state);

    /* 状态切换自己会换图，进行中的那一次眨眼就此作废；眼皮回到本表情的常态开合度 */
    face->blinking = false;
    apply_lids(face);
    lv_image_set_scale(face->image, (uint32_t)(uintptr_t)lv_obj_get_user_data(face->image));

    /* Settle the sway of the previous state instead of snapping it away: a new
     * state can arrive mid movement. */
    sway_x = lv_obj_get_style_translate_x(face->root, 0);
    sway_y = lv_obj_get_style_translate_y(face->root, 0);
    if (sway_x != 0 || sway_y != 0) {
        lv_anim_t settle;

        lv_anim_init(&settle);
        lv_anim_set_var(&settle, face->root);
        lv_anim_set_exec_cb(&settle, anim_translate_x);
        lv_anim_set_values(&settle, sway_x, 0);
        lv_anim_set_duration(&settle, QZ_DUR_SMALL);
        qz_anim_ease_out(&settle);
        lv_anim_start(&settle);

        lv_anim_init(&settle);
        lv_anim_set_var(&settle, face->root);
        lv_anim_set_exec_cb(&settle, anim_translate_y);
        lv_anim_set_values(&settle, sway_y, 0);
        lv_anim_set_duration(&settle, QZ_DUR_SMALL);
        qz_anim_ease_out(&settle);
        lv_anim_start(&settle);
    }

    /* Swap the expression, then fade + rise it in so the change reads as a
     * gesture. Both start from the live value, so an interrupted transition
     * continues instead of jumping. */
    lv_image_set_src(face->image, state_image(state));
    {
        lv_anim_t fade;

        lv_anim_init(&fade);
        lv_anim_set_var(&fade, face->image);
        lv_anim_set_exec_cb(&fade, anim_opa);
        lv_anim_set_values(&fade, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_duration(&fade, QZ_DUR_SMALL);
        qz_anim_ease_out(&fade);
        lv_anim_start(&fade);
    }
    {
        lv_anim_t rise;
        int32_t rise_distance = size * 5 / 100;

        lv_anim_init(&rise);
        lv_anim_set_var(&rise, face->image);
        lv_anim_set_exec_cb(&rise, anim_translate_y);
        lv_anim_set_values(&rise, lv_obj_get_style_translate_y(face->image, 0) + rise_distance, 0);
        lv_anim_set_duration(&rise, QZ_DUR_PANEL);
        qz_anim_ease_out(&rise);
        lv_anim_start(&rise);
    }

    /* 逐表情动作：一个表情要么上下起伏、要么左右摇摆（困惑那种），
     * 另可按配方叠一个图片缩放的呼吸；"受惊/使劲"用方波抖动（见 jitter_tick）。 */
    if (recipe->sway) {
        loop_anim(face->root, anim_translate_x, -size * recipe->sway / 1000,
                  size * recipe->sway / 1000, recipe->sway_ms, 0);
    } else if (recipe->bounce) {
        loop_anim(face->image, anim_translate_y, 0, -size * recipe->bounce / 1000,
                  recipe->bounce_ms, 0);
    }
    if (recipe->breathe) {
        int32_t base = (int32_t)(uintptr_t)lv_obj_get_user_data(face->image);
        loop_anim(face->image, anim_image_zoom, 0, base * recipe->breathe / 1000, 700, 0);
    }
    if (face->jitter_timer) {
        lv_timer_set_period(face->jitter_timer, recipe->jitter ? (uint32_t)recipe->jitter_ms : 1000);
        if (recipe->jitter == 0) {
            face->jitter_sign = 1;
            lv_obj_set_style_translate_x(face->root, 0, 0);
        }
    }

    apply_decorations(face);
}
