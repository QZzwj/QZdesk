#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_DEVICE "/dev/input/event0"
#define DEFAULT_SCREEN_W 320
#define DEFAULT_SCREEN_H 240

typedef struct {
    int min;
    int max;
    int valid;
} axis_range_t;

typedef struct {
    int fd;
    int screen_w;
    int screen_h;
    axis_range_t x_raw;
    axis_range_t y_raw;
    axis_range_t mt_x;
    axis_range_t mt_y;
} touch_t;

static void usage(const char *name)
{
    fprintf(stderr,
            "usage:\n"
            "  %s info\n"
            "  %s tap X Y [HOLD_MS]\n"
            "  %s hold X Y HOLD_MS\n"
            "  %s swipe X1 Y1 X2 Y2 [DURATION_MS] [STEPS]\n"
            "\n"
            "Coordinates use the %dx%d UI screen. Override with\n"
            "QZDESK_TOUCH_SCREEN=WIDTHxHEIGHT and QZDESK_TOUCH_DEVICE=path.\n",
            name, name, name, name, DEFAULT_SCREEN_W, DEFAULT_SCREEN_H);
}

static int parse_int(const char *text, int *out)
{
    char *end = NULL;
    long value;
    if (!text || !*text) return 0;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno || !end || *end || value < INT32_MIN || value > INT32_MAX) return 0;
    *out = (int)value;
    return 1;
}

static void parse_screen(touch_t *touch)
{
    const char *value = getenv("QZDESK_TOUCH_SCREEN");
    int width = DEFAULT_SCREEN_W;
    int height = DEFAULT_SCREEN_H;
    touch->screen_w = width;
    touch->screen_h = height;
    if (value && *value) {
        char tail = 0;
        if (sscanf(value, "%dx%d%c", &width, &height, &tail) == 2 &&
            width > 1 && height > 1) {
            touch->screen_w = width;
            touch->screen_h = height;
        }
    }
}

static axis_range_t query_axis(int fd, unsigned long request)
{
    struct input_absinfo info;
    axis_range_t range = { 0, 0, 0 };
    memset(&info, 0, sizeof(info));
    if (ioctl(fd, request, &info) == 0 && info.maximum > info.minimum) {
        range.min = info.minimum;
        range.max = info.maximum;
        range.valid = 1;
    }
    return range;
}

static int open_touch(touch_t *touch)
{
    const char *path = getenv("QZDESK_TOUCH_DEVICE");
    int probe;
    memset(touch, 0, sizeof(*touch));
    parse_screen(touch);
    if (!path || !*path) path = DEFAULT_DEVICE;

    probe = open(path, O_RDONLY | O_NOCTTY | O_CLOEXEC);
    if (probe < 0) {
        fprintf(stderr, "open %s for probe failed: %s\n", path, strerror(errno));
        return -1;
    }
    touch->x_raw = query_axis(probe, EVIOCGABS(ABS_X));
    touch->y_raw = query_axis(probe, EVIOCGABS(ABS_Y));
    touch->mt_x = query_axis(probe, EVIOCGABS(ABS_MT_POSITION_X));
    touch->mt_y = query_axis(probe, EVIOCGABS(ABS_MT_POSITION_Y));
    close(probe);

    touch->fd = open(path, O_WRONLY | O_NOCTTY | O_CLOEXEC);
    if (touch->fd < 0) {
        fprintf(stderr, "open %s for injection failed: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

static void print_range(const char *name, axis_range_t range)
{
    if (range.valid) {
        printf("%s=%d..%d\n", name, range.min, range.max);
    } else {
        printf("%s=unavailable\n", name);
    }
}

static int emit(touch_t *touch, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event event;
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.code = code;
    event.value = value;
    if (write(touch->fd, &event, sizeof(event)) != (ssize_t)sizeof(event)) {
        fprintf(stderr, "write event type=%u code=%u failed: %s\n",
                type, code, strerror(errno));
        return -1;
    }
    return 0;
}

static int sync_report(touch_t *touch)
{
    return emit(touch, EV_SYN, SYN_REPORT, 0);
}

static int clamp_int(int value, int low, int high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static int map_axis(int screen_value, int screen_size, axis_range_t calibrated,
                    axis_range_t multitouch)
{
    /*
     * lv_evdev calibrates ABS_X/ABS_Y itself. If those axes are absent, it
     * forwards the multitouch value unchanged, so feed it display pixels.
     */
    if (calibrated.valid) {
        long scaled = calibrated.min +
                      (long)screen_value * (calibrated.max - calibrated.min) /
                          (screen_size - 1);
        return (int)scaled;
    }
    if (multitouch.valid && multitouch.max <= screen_size * 2) {
        long scaled = multitouch.min +
                      (long)screen_value * (multitouch.max - multitouch.min) /
                          (screen_size - 1);
        return (int)scaled;
    }
    return screen_value;
}

static int point_x(touch_t *touch, int x)
{
    x = clamp_int(x, 0, touch->screen_w - 1);
    return map_axis(x, touch->screen_w, touch->x_raw, touch->mt_x);
}

static int point_y(touch_t *touch, int y)
{
    y = clamp_int(y, 0, touch->screen_h - 1);
    return map_axis(y, touch->screen_h, touch->y_raw, touch->mt_y);
}

static int send_position(touch_t *touch, int x, int y)
{
    int raw_x = point_x(touch, x);
    int raw_y = point_y(touch, y);
    if (emit(touch, EV_ABS, ABS_X, raw_x) < 0 ||
        emit(touch, EV_ABS, ABS_Y, raw_y) < 0 ||
        emit(touch, EV_ABS, ABS_MT_POSITION_X, raw_x) < 0 ||
        emit(touch, EV_ABS, ABS_MT_POSITION_Y, raw_y) < 0) {
        return -1;
    }
    return 0;
}

static int touch_down(touch_t *touch, int x, int y)
{
    if (emit(touch, EV_ABS, ABS_MT_SLOT, 0) < 0 ||
        emit(touch, EV_ABS, ABS_MT_TRACKING_ID, 0) < 0 ||
        send_position(touch, x, y) < 0 ||
        emit(touch, EV_KEY, BTN_TOUCH, 1) < 0) {
        return -1;
    }
    return sync_report(touch);
}

static int touch_move(touch_t *touch, int x, int y)
{
    if (send_position(touch, x, y) < 0) return -1;
    return sync_report(touch);
}

static int touch_up(touch_t *touch)
{
    if (emit(touch, EV_ABS, ABS_MT_SLOT, 0) < 0 ||
        emit(touch, EV_ABS, ABS_MT_TRACKING_ID, -1) < 0 ||
        emit(touch, EV_KEY, BTN_TOUCH, 0) < 0) {
        return -1;
    }
    return sync_report(touch);
}

static void sleep_ms(int milliseconds)
{
    if (milliseconds <= 0) return;
    usleep((useconds_t)milliseconds * 1000U);
}

static int command_tap(touch_t *touch, int x, int y, int hold_ms)
{
    if (touch_down(touch, x, y) < 0) return 1;
    sleep_ms(hold_ms);
    if (touch_up(touch) < 0) return 1;
    return 0;
}

static int command_swipe(touch_t *touch, int x1, int y1, int x2, int y2,
                         int duration_ms, int steps)
{
    int step;
    if (steps < 2) steps = 2;
    if (duration_ms < steps) duration_ms = steps;
    if (touch_down(touch, x1, y1) < 0) return 1;
    for (step = 1; step <= steps; step++) {
        int x = x1 + (x2 - x1) * step / steps;
        int y = y1 + (y2 - y1) * step / steps;
        sleep_ms(duration_ms / steps);
        if (touch_move(touch, x, y) < 0) return 1;
    }
    if (touch_up(touch) < 0) return 1;
    return 0;
}

int main(int argc, char **argv)
{
    touch_t touch;
    const char *command;
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    if (open_touch(&touch) < 0) return 1;

    command = argv[1];
    if (strcmp(command, "info") == 0) {
        printf("device=%s\n", getenv("QZDESK_TOUCH_DEVICE") ?
               getenv("QZDESK_TOUCH_DEVICE") : DEFAULT_DEVICE);
        printf("screen=%dx%d\n", touch.screen_w, touch.screen_h);
        print_range("abs_x", touch.x_raw);
        print_range("abs_y", touch.y_raw);
        print_range("mt_x", touch.mt_x);
        print_range("mt_y", touch.mt_y);
        close(touch.fd);
        return 0;
    }

    if (strcmp(command, "tap") == 0) {
        int x, y, hold_ms = 90;
        if (argc < 4 || !parse_int(argv[2], &x) || !parse_int(argv[3], &y) ||
            (argc >= 5 && !parse_int(argv[4], &hold_ms))) {
            usage(argv[0]);
            close(touch.fd);
            return 2;
        }
        return command_tap(&touch, x, y, hold_ms);
    }

    if (strcmp(command, "hold") == 0) {
        int x, y, hold_ms;
        if (argc != 5 || !parse_int(argv[2], &x) || !parse_int(argv[3], &y) ||
            !parse_int(argv[4], &hold_ms) || hold_ms < 0) {
            usage(argv[0]);
            close(touch.fd);
            return 2;
        }
        return command_tap(&touch, x, y, hold_ms);
    }

    if (strcmp(command, "swipe") == 0) {
        int x1, y1, x2, y2, duration_ms = 280, steps = 18;
        if (argc < 6 || !parse_int(argv[2], &x1) || !parse_int(argv[3], &y1) ||
            !parse_int(argv[4], &x2) || !parse_int(argv[5], &y2) ||
            (argc >= 7 && !parse_int(argv[6], &duration_ms)) ||
            (argc >= 8 && !parse_int(argv[7], &steps))) {
            usage(argv[0]);
            close(touch.fd);
            return 2;
        }
        return command_swipe(&touch, x1, y1, x2, y2, duration_ms, steps);
    }

    usage(argv[0]);
    close(touch.fd);
    return 2;
}
