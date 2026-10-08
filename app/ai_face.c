/* Mascot face —— 小智标准表情（GIF）。
 *
 * 表情素材改用 txp666/otto-emoji-gif-component 的「小智标准」21 个表情：240×240、
 * 33 帧、每帧 80ms 的黑白风格动画，就是真机上小智设备的那张脸。我们取其中 10 个
 * 缩放到界面需要的三档尺寸（60 / 88 / 164），GIF 字节直接嵌在固件里（见
 * tools/build_otto_emoji.py），运行时由 LVGL 的 lv_gif 解码播放 —— 设备上不需要
 * 放任何素材文件，也就不会出现"素材缺失 → 表情空白"。
 *
 * 为什么换掉上一版自己画的双眼脸：那套是手写几何 + lv_anim 链的动画，出现过
 * "闭眼那一拍没接上睁眼 → 眼板卡在 4px、眼睛被裁剪窗整个裁掉"的空白（实测待机
 * 3.5s 后持续空白 3.8s）。把动效交给成熟素材和解码器，我们只负责选片与省 CPU。
 *
 * 对外 API 与旧实现一致（create / set_state / state_text），三处调用点
 * （主页 88、关于卡片 60、全屏表情 164）无需改动。
 */
#include "ai_face.h"
#include "otto_emoji.h"
#include "theme.h"
#include <stdlib.h>

typedef struct {
    lv_obj_t *root;        /* 深色圆角"屏幕"：表情是黑底白脸，需有黑底承着 */
    lv_obj_t *gif;
    qz_face_state_t state;
    qz_otto_size_t size;   /* 用哪一档尺寸的资源 */
    lv_timer_t *vis_timer;
    bool paused;
} qz_face_t;

/** 物理边长 -> 资源档位：取最接近的一档。
 *
 * 用"最接近"而不是"够用"是因为缩放面板（320×240）下 88 设计像素只剩 59 物理
 * 像素：拿 88px 的图塞进 59px 的框要再缩一遍，既糊又白费 CPU，不如直接播 60px
 * 的那一档。阈值取两档的中点。 */
static qz_otto_size_t size_class(int32_t px)
{
    if (px <= 74) return QZ_OTTO_60;    /* 60 与 88 的中点 */
    if (px <= 126) return QZ_OTTO_88;   /* 88 与 164 的中点 */
    return QZ_OTTO_164;
}

static void set_paused(qz_face_t *face, bool pause)
{
    if (pause == face->paused) return;
    face->paused = pause;
    if (pause) lv_gif_pause(face->gif);
    else lv_gif_resume(face->gif);
}

/**
 * 只解码"当前屏上"的那张脸。
 *
 * 三处脸分别在主页、关于页、对话页三块屏幕上，同时解码三份等于白白占 CPU ——
 * RV1106 的算力要留给核心。这里每 500ms 看一眼自己的屏幕是不是当前屏，不是就
 * 暂停 lv_gif 内部的解码定时器（恢复时接着播，不重头开始）。顺带把"减少动态"
 * 也当成长时间暂停处理。
 */
static void vis_tick(lv_timer_t *timer)
{
    qz_face_t *face = (qz_face_t *)lv_timer_get_user_data(timer);

    if (!face || !face->root || !face->gif) return;
    set_paused(face, qz_reduce_motion() ||
                     lv_obj_get_screen(face->root) != lv_screen_active());
}

static void face_delete(lv_event_t *event)
{
    qz_face_t *face = (qz_face_t *)lv_event_get_user_data(event);

    if (!face) return;
    if (face->vis_timer) lv_timer_delete(face->vis_timer);
    free(face);
}

lv_obj_t *qz_face_create(lv_obj_t *parent, int32_t design_px)
{
    qz_face_t *face;
    int32_t box;    /* 面板上的物理边长 */
    int32_t art;    /* 图案的物理边长 */

    if (!parent || design_px <= 0) return NULL;

    /* 页面写的是设计像素、由 scale.h 的宏统一缩放到实际面板；脸内部要按物理
     * 像素自己算（容器正方形、图案 1:1 不缩放），所以这里换算一次。之后的写入
     * 一律用带括号的旁路调用，免得被那些宏再缩一遍。
     * 取较小的比例（qz_scale_px）是为了两个方向都不溢出：320×240 下 88 设计
     * 像素 -> 59 物理像素。 */
    box = qz_scale_px(design_px);

    face = (qz_face_t *)calloc(1, sizeof(qz_face_t));
    if (!face) return NULL;
    face->size = size_class(box);
    face->state = QZ_FACE_IDLE;
    art = qz_otto_px(face->size);

    /* 深色圆角屏：表情自带黑底，露在白卡片上就是一块"机器人的脸屏" */
    face->root = lv_obj_create(parent);
    (lv_obj_set_size)(face->root, box, box);
    (lv_obj_set_style_radius)(face->root, box / 5, 0);
    /* 圆角不靠 clip_corner：那会让 LVGL 把子对象渲进 ARGB8888 图层再做遮罩，
     * 而且只在重绘区域与"上/中/下三条带"相交时才生效（lv_refr.c:199-248）——
     * 表情每帧只重绘脸区，圆角实测会时灵时不灵、甚至整个变直角。素材已把四角
     * 做成透明（见 tools/build_otto_emoji.py），这里给一层黑底圆角，透过透明角
     * 看到的就是它，边缘由 LVGL 的圆角抗锯齿保证。 */
    lv_obj_set_style_bg_color(face->root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(face->root, LV_OPA_COVER, 0);
    (lv_obj_set_style_border_width)(face->root, 0, 0);
    (lv_obj_set_style_pad_all)(face->root, 0, 0);
    lv_obj_clear_flag(face->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(face->root, face);
    lv_obj_add_event_cb(face->root, face_delete, LV_EVENT_DELETE, face);

    face->gif = lv_gif_create(face->root);
    /* 图案与容器都是正方形：居中对齐（档位比容器大时由容器裁掉多出的边） */
    (lv_obj_set_pos)(face->gif, (box - art) / 2, (box - art) / 2);
    lv_obj_clear_flag(face->gif, LV_OBJ_FLAG_CLICKABLE);
    lv_gif_set_src(face->gif, qz_otto_emoji(face->state, face->size));

    face->vis_timer = lv_timer_create(vis_tick, 500, face);
    vis_tick(face->vis_timer);           /* 初始若不在当前屏就立刻停住 */

    return face->root;
}

void qz_face_set_state(lv_obj_t *root, qz_face_state_t state)
{
    qz_face_t *face;
    const lv_image_dsc_t *src;

    if (!root) return;
    face = (qz_face_t *)lv_obj_get_user_data(root);
    if (!face || !face->gif) return;
    if (state >= QZ_FACE_STATE_COUNT) state = QZ_FACE_IDLE;
    if (state == face->state) return;    /* 同状态不重载，避免无谓地重启解码 */
    face->state = state;
    src = qz_otto_emoji(state, face->size);
    if (src) lv_gif_set_src(face->gif, src);
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
        case QZ_FACE_IDLE:
        default: return "我在听你说～";
    }
}
