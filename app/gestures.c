#include "gestures.h"
#include "notifications.h"
#include "theme.h"

/* ------------------------------------------------------------------------- *
 * 全局手势
 *
 * 钩在输入设备上，而不是逐个控件挂回调：不管手指从哪个控件上开始滑都算数，
 * 而且能在 RELEASED 时用 lv_indev_stop_processing() 把这一下从控件手里拿走 ——
 * 否则"边缘右滑"对控件来说还是一次点击，返回之后立刻又点进了下一页。
 * ------------------------------------------------------------------------- */
#define GESTURE_EDGE_X 24      /**< 从左边多少设计像素之内起手才算返回（iOS 是 20pt） */
#define GESTURE_MIN_TRAVEL 36  /**< 至少要走这么远，短划不算手势 */
#define GESTURE_RATIO 2        /**< 主方向得比次方向明显：斜着滑不算 */

static lv_point_t start_point;
static bool tracking;
static bool consumed;

static lv_dir_t swipe_dir(lv_indev_t *indev)
{
    lv_point_t end;
    lv_indev_get_point(indev, &end);
    int32_t dx = end.x - start_point.x;
    int32_t dy = end.y - start_point.y;
    if (dx >= qz_scale_x(GESTURE_MIN_TRAVEL) && dx >= GESTURE_RATIO * LV_ABS(dy))
        return LV_DIR_RIGHT;
    if (dy >= qz_scale_y(GESTURE_MIN_TRAVEL) && dy >= GESTURE_RATIO * LV_ABS(dx))
        return LV_DIR_BOTTOM;
    return LV_DIR_NONE;
}

/** 手势已经完成，这一下不再交给下面的控件。先撤掉按压态，再让 LVGL 等到
 *  抬手；等待期间和抬手后的 CLICKED 都不会落到控件上。 */
static void swallow(lv_indev_t *indev)
{
    lv_obj_t *act = lv_indev_get_active_obj();
    lv_obj_t *scrolled = lv_indev_get_scroll_obj(indev);
    if (act) lv_obj_send_event(act, LV_EVENT_PRESS_LOST, indev);
    if (scrolled && scrolled != act) lv_obj_send_event(scrolled, LV_EVENT_PRESS_LOST, indev);
    lv_indev_wait_release(indev);
    lv_indev_stop_processing(indev);
}

static void gesture_return(lv_indev_t *indev)
{
    swallow(indev);
    if (qz_notifications_panel_open()) {
        qz_notifications_close_panel();
    } else if (!qz_overlay_close_top()) {
        qz_screen_back();
    }
}

static void gesture_notifications(lv_indev_t *indev)
{
    if (qz_notifications_panel_open() || qz_overlay_open()) return;
    swallow(indev);
    qz_notifications_open_panel();
}

static void gesture_event(lv_event_t *event)
{
    lv_indev_t *indev = lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &start_point);
        tracking = true;
        consumed = false;
        return;
    }
    if (code == LV_EVENT_PRESS_LOST || code == LV_EVENT_CANCEL ||
        code == LV_EVENT_INDEV_RESET) {
        tracking = false;
        return;
    }
    if (!tracking || consumed) return;
    if (code != LV_EVENT_PRESSING) return;

    lv_dir_t dir = swipe_dir(indev);
    if (dir == LV_DIR_RIGHT && start_point.x <= qz_scale_x(GESTURE_EDGE_X)) {
        consumed = true;
        tracking = false;
        gesture_return(indev);
    } else if (dir == LV_DIR_BOTTOM && start_point.y <= qz_scale_y(QZ_STATUS_H)) {
        consumed = true;
        tracking = false;
        gesture_notifications(indev);
    }
}

void qz_global_gestures_init(void)
{
    for (lv_indev_t *indev = lv_indev_get_next(NULL); indev;
         indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) continue;
        lv_indev_add_event_cb(indev, gesture_event, LV_EVENT_ALL, indev);
    }
}
