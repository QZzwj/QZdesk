/* 摄像头与「有人靠近」检测。接口说明见 include/face_camera.h。
 *
 * 两个后端，走同一套状态机与回调：
 *
 *   1. **V4L2 + 帧差**（默认）：打开摄像头，把 Y 平面下采样成 32×24 的亮度网格，
 *      与上一帧逐格比较。变化的格子占比超过阈值 = 这一帧有人活动；连续若干帧活动
 *      判定「有人靠近」，持续一段时间安静判定「离开了」。不需要任何模型。
 *   2. **测试后端**（`QZDESK_PRESENCE_FAKE=1`）：没有摄像头的机器（开发机、SDL
 *      模拟器、CI）上周期性地制造「有人 / 离开」，用来验证整条链路 —— 亮屏、
 *      报给核心、助手打招呼，都不依赖真实摄像头。
 *
 * 环境变量：
 *   QZDESK_CAMERA_DISABLED=1     彻底禁用（老开关，保留）
 *   QZDESK_CAMERA_DEVICE=/dev/videoN  指定节点；模拟器下必须显式指定才会开摄像头
 *   QZDESK_CAMERA_FPS=5          分析帧率（默认 5，别拿 RV1106 的 CPU 去跑 30fps）
 *   QZDESK_PRESENCE_FAKE=1       用测试后端
 *   QZDESK_PRESENCE_FAKE_PERIOD=6  测试后端每这么多秒来一次「有人」 */

#include "face_camera.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CAMERA_MAX_BUFFERS 4

/* 亮度网格：32×24 在 480×320 的屏幕上够用，算力约等于零 */
#define GRID_W 32
#define GRID_H 24
/* 单格平均亮度差达到它就认为这格「动了」（0-255） */
#define CHANGE_LEVEL 16
/* 动了的格子占比超过它就认为这一帧有人活动（约 4% ≈ 30 格） */
#define CHANGE_RATIO 0.04
/* 连续这么多帧活动 → 有人靠近；连续这么多帧安静 → 离开了 */
#define ARRIVE_FRAMES 2
#define LEAVE_FRAMES 40

typedef enum {
    PIXEL_GREY,  /**< 每像素 1 字节 */
    PIXEL_NV12,  /**< Y 平面在前，1 字节/像素 */
    PIXEL_YUYV,  /**< 每像素 2 字节，Y 在前 */
} pixel_format_t;

typedef struct {
    int fd;
    void *buffers[CAMERA_MAX_BUFFERS];
    size_t lengths[CAMERA_MAX_BUFFERS];
    unsigned int count;
    unsigned int width;
    unsigned int height;
    unsigned int stride;      /**< 每行字节数 */
    pixel_format_t format;
    unsigned char *grid;      /**< GRID_W*GRID_H 的当前帧亮度 */
    unsigned char *previous;  /**< 上一帧亮度 */
    bool have_previous;       /**< 第一帧只做基准，不参与比较 */
} camera_t;

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static bool worker_started;
static bool stop_requested;
static qz_presence_cb_t presence_callback;
static void *presence_callback_data;

static bool present;
static int arrivals;
static unsigned long long last_seen;
static char backend_name[64] = "未启动";
static char status_text[96] = "摄像头未启动";

static bool env_flag(const char *name)
{
    const char *value = getenv(name);
    return value && (value[0] == '1' || value[0] == 'y' || value[0] == 'Y');
}

static int env_int(const char *name, int fallback)
{
    const char *value = getenv(name);
    int parsed;
    if (!value || !*value) return fallback;
    parsed = atoi(value);
    return parsed > 0 ? parsed : fallback;
}

static unsigned long long now_seconds(void)
{
    return (unsigned long long)time(NULL);
}

/** 单调毫秒，用于限帧（墙钟会被 NTP 拨动，不适合做间隔判断）。 */
static unsigned long long monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (unsigned long long)now.tv_sec * 1000ULL + (unsigned long long)(now.tv_nsec / 1000000);
}

/* ------------------------------------------------------------------------- *
 * 状态读写（工作线程与界面线程各写一半，统一走锁）
 * ------------------------------------------------------------------------- */

static void set_text(char *target, size_t size, const char *text)
{
    /* 用 "%.*s" 显式限长：内容会安全截断，也免得编译器对可能超长的 %s 报
     * -Wformat-truncation（来源字符串的静态长度它算得出来，但悲观）。 */
    int limit = size > 0 ? (int)size - 1 : 0;
    pthread_mutex_lock(&state_lock);
    snprintf(target, size, "%.*s", limit, text ? text : "");
    pthread_mutex_unlock(&state_lock);
}

static void mark_present(bool value)
{
    pthread_mutex_lock(&state_lock);
    present = value;
    if (value) {
        arrivals++;
        last_seen = now_seconds();
    }
    pthread_mutex_unlock(&state_lock);
}

/** 判定结果对外播报。回调在工作线程上调用，界面侧只该做标记。 */
static void emit(qz_presence_event_t event)
{
    qz_presence_cb_t callback;
    void *user_data;

    pthread_mutex_lock(&state_lock);
    callback = presence_callback;
    user_data = presence_callback_data;
    pthread_mutex_unlock(&state_lock);
    if (callback) callback(event, user_data);
}

static bool should_stop(void)
{
    bool value;
    pthread_mutex_lock(&state_lock);
    value = stop_requested;
    pthread_mutex_unlock(&state_lock);
    return value;
}

/* ------------------------------------------------------------------------- *
 * V4L2 后端
 * ------------------------------------------------------------------------- */

static const char *format_name(pixel_format_t format)
{
    switch (format) {
        case PIXEL_GREY: return "GREY";
        case PIXEL_NV12: return "NV12";
        default: return "YUYV";
    }
}

/** 打开一个能出 YUV 的采集节点。成功返回 fd，失败返回 -1。 */
static int open_capture_node(const char *path)
{
    struct v4l2_capability capability;
    int fd = open(path, O_RDWR | O_NONBLOCK);

    if (fd < 0) return -1;
    memset(&capability, 0, sizeof(capability));
    if (ioctl(fd, VIDIOC_QUERYCAP, &capability) < 0) {
        close(fd);
        return -1;
    }
    if (!(capability.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
        !(capability.capabilities & V4L2_CAP_STREAMING)) {
        close(fd);
        return -1;
    }
    return fd;
}

/** 挑一个节点：显式指定的优先；否则在模拟器里不开（免得抓走桌面摄像头）。 */
static int open_camera_device(char *name, size_t name_size)
{
    const char *explicit_device = getenv("QZDESK_CAMERA_DEVICE");

    if (explicit_device && *explicit_device) {
        int fd = open_capture_node(explicit_device);
        if (fd >= 0) {
            snprintf(name, name_size, "V4L2 %s", explicit_device);
            return fd;
        }
        return -1;
    }

    for (int index = 0; index < 4; index++) {
        char path[32];
        int fd;
        snprintf(path, sizeof(path), "/dev/video%d", index);
        fd = open_capture_node(path);
        if (fd >= 0) {
            snprintf(name, name_size, "V4L2 %s", path);
            return fd;
        }
    }
    return -1;
}

/** 申请采集格式。优先单平面亮度格式，驱动不认才退回 NV12 / YUYV。 */
static bool set_format(camera_t *camera)
{
    struct v4l2_format format;
    struct v4l2_fmtdesc description;
    bool has_grey = false;
    bool has_nv12 = false;
    bool has_yuyv = false;

    memset(&description, 0, sizeof(description));
    description.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for (description.index = 0;; description.index++) {
        if (ioctl(camera->fd, VIDIOC_ENUM_FMT, &description) < 0) break;
        if (description.pixelformat == V4L2_PIX_FMT_GREY) has_grey = true;
        if (description.pixelformat == V4L2_PIX_FMT_NV12) has_nv12 = true;
        if (description.pixelformat == V4L2_PIX_FMT_YUYV) has_yuyv = true;
    }

    /* 640×480 足够看出"有人进来了"，又不至于让 ISP 与 CPU 忙起来 */
    for (int attempt = 0; attempt < 3; attempt++) {
        unsigned int wanted;
        if (attempt == 0 && has_grey) wanted = V4L2_PIX_FMT_GREY;
        else if (attempt == (has_grey ? 1 : 0) && has_nv12) wanted = V4L2_PIX_FMT_NV12;
        else if (has_yuyv) wanted = V4L2_PIX_FMT_YUYV;
        else continue;

        memset(&format, 0, sizeof(format));
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width = 640;
        format.fmt.pix.height = 480;
        format.fmt.pix.pixelformat = wanted;
        format.fmt.pix.field = V4L2_FIELD_NONE;
        if (ioctl(camera->fd, VIDIOC_S_FMT, &format) < 0) continue;
        /* 驱动可能改小了分辨率或换了格式，按它实际给的来 */
        if (format.fmt.pix.pixelformat == V4L2_PIX_FMT_GREY) {
            camera->format = PIXEL_GREY;
        } else if (format.fmt.pix.pixelformat == V4L2_PIX_FMT_NV12 ||
                   format.fmt.pix.pixelformat == V4L2_PIX_FMT_YUV420) {
            camera->format = PIXEL_NV12;
        } else if (format.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV) {
            camera->format = PIXEL_YUYV;
        } else {
            continue;
        }
        camera->width = format.fmt.pix.width;
        camera->height = format.fmt.pix.height;
        camera->stride = format.fmt.pix.bytesperline;
        if (camera->width == 0 || camera->height == 0) continue;
        if (camera->stride == 0) {
            camera->stride = camera->width * (camera->format == PIXEL_YUYV ? 2 : 1);
        }
        return true;
    }
    return false;
}

static bool start_streaming(camera_t *camera)
{
    struct v4l2_requestbuffers request;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    memset(&request, 0, sizeof(request));
    request.count = CAMERA_MAX_BUFFERS;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (ioctl(camera->fd, VIDIOC_REQBUFS, &request) < 0 || request.count == 0) return false;
    camera->count = request.count;

    for (unsigned int index = 0; index < camera->count; index++) {
        struct v4l2_buffer buffer;
        memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        if (ioctl(camera->fd, VIDIOC_QUERYBUF, &buffer) < 0) return false;
        camera->lengths[index] = buffer.length;
        camera->buffers[index] =
            mmap(NULL, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, camera->fd, buffer.m.offset);
        if (camera->buffers[index] == MAP_FAILED) return false;
        memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        if (ioctl(camera->fd, VIDIOC_QBUF, &buffer) < 0) return false;
    }
    return ioctl(camera->fd, VIDIOC_STREAMON, &type) == 0;
}

static void close_camera(camera_t *camera)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (camera->fd >= 0) {
        ioctl(camera->fd, VIDIOC_STREAMOFF, &type);
        for (unsigned int index = 0; index < camera->count; index++) {
            if (camera->buffers[index] && camera->buffers[index] != MAP_FAILED) {
                munmap(camera->buffers[index], camera->lengths[index]);
            }
        }
        close(camera->fd);
    }
    free(camera->grid);
    free(camera->previous);
    memset(camera, 0, sizeof(*camera));
    camera->fd = -1;
}

/** 把一帧的亮度下采样成 GRID_W×GRID_H：每格取几个采样点求平均。 */
static void sample_grid(const camera_t *camera, const unsigned char *frame, unsigned char *grid)
{
    unsigned int step_x = camera->width / GRID_W;
    unsigned int step_y = camera->height / GRID_H;
    unsigned int pixel_step = camera->format == PIXEL_YUYV ? 2u : 1u;
    if (step_x == 0) step_x = 1;
    if (step_y == 0) step_y = 1;

    for (unsigned int row = 0; row < GRID_H; row++) {
        for (unsigned int column = 0; column < GRID_W; column++) {
            unsigned int sum = 0;
            unsigned int samples = 0;
            /* 每格采 4 个点（2×2）：够稳，又不至于把整帧都读一遍 */
            for (unsigned int dy = 0; dy < 2; dy++) {
                for (unsigned int dx = 0; dx < 2; dx++) {
                    unsigned int x = column * step_x + dx * (step_x / 2);
                    unsigned int y = row * step_y + dy * (step_y / 2);
                    size_t offset;
                    if (x >= camera->width || y >= camera->height) continue;
                    offset = (size_t)y * camera->stride + (size_t)x * pixel_step;
                    sum += frame[offset];
                    samples++;
                }
            }
            grid[row * GRID_W + column] = samples ? (unsigned char)(sum / samples) : 0;
        }
    }
}

/** 变化的格子数。 */
static int changed_cells(const unsigned char *grid, const unsigned char *previous)
{
    int changed = 0;
    for (int index = 0; index < GRID_W * GRID_H; index++) {
        int difference = (int)grid[index] - (int)previous[index];
        if (difference < 0) difference = -difference;
        if (difference >= CHANGE_LEVEL) changed++;
    }
    return changed;
}

/** 逐帧分析 + 「有人 / 离开」状态机。有事件时填 `event` 并返回 true。 */
static bool analyze(camera_t *camera, const unsigned char *frame, int *active_streak,
                    int *quiet_streak, qz_presence_event_t *event)
{
    int changed;
    bool active;

    if (!camera->grid) camera->grid = malloc(GRID_W * GRID_H);
    if (!camera->previous) camera->previous = malloc(GRID_W * GRID_H);
    if (!camera->grid || !camera->previous) return false;

    sample_grid(camera, frame, camera->grid);
    changed = camera->have_previous ? changed_cells(camera->grid, camera->previous) : 0;
    active = camera->have_previous && changed >= (int)(GRID_W * GRID_H * CHANGE_RATIO);

    memcpy(camera->previous, camera->grid, GRID_W * GRID_H);
    camera->have_previous = true;

    *active_streak = active ? *active_streak + 1 : 0;
    *quiet_streak = active ? 0 : *quiet_streak + 1;

    if (!present && *active_streak >= ARRIVE_FRAMES) {
        mark_present(true);
        *quiet_streak = 0;
        *event = QZ_PRESENCE_ARRIVE;
        return true;
    }
    if (present && *quiet_streak >= LEAVE_FRAMES) {
        mark_present(false);
        *active_streak = 0;
        *event = QZ_PRESENCE_LEAVE;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------------- *
 * 工作线程
 * ------------------------------------------------------------------------- */

/** 测试后端：没有摄像头也把整条链路跑起来（亮屏、报核心、打招呼）。 */
static void run_fake_backend(int period)
{
    int waited;

    while (!should_stop()) {
        for (waited = 0; waited < period * 10 && !should_stop(); waited++) {
            usleep(100 * 1000);
        }
        if (should_stop()) break;
        mark_present(true);
        set_text(status_text, sizeof(status_text), "测试后端：有人靠近");
        emit(QZ_PRESENCE_ARRIVE);

        for (waited = 0; waited < (period * 10) / 2 && !should_stop(); waited++) {
            usleep(100 * 1000);
        }
        if (should_stop()) break;
        mark_present(false);
        set_text(status_text, sizeof(status_text), "测试后端：无人");
        emit(QZ_PRESENCE_LEAVE);
    }
}

static void run_v4l2_backend(camera_t *camera)
{
    int active_streak = 0;
    int quiet_streak = 0;
    int frame_interval_ms = 1000 / env_int("QZDESK_CAMERA_FPS", 5);
    unsigned long long last_frame_ms = 0;

    if (frame_interval_ms < 20) frame_interval_ms = 20;

    while (!should_stop()) {
        struct pollfd descriptor;
        struct v4l2_buffer buffer;
        unsigned long long now_ms;

        descriptor.fd = camera->fd;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        if (poll(&descriptor, 1, 300) <= 0) continue;

        memset(&buffer, 0, sizeof(buffer));
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (ioctl(camera->fd, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN) continue;
            break;
        }

        now_ms = monotonic_ms();
        /* 限帧：RV1106 上没必要按摄像头给的全速做帧差 */
        if (now_ms - last_frame_ms >= (unsigned long long)frame_interval_ms &&
            buffer.index < camera->count) {
            qz_presence_event_t event;
            last_frame_ms = now_ms;
            if (analyze(camera, camera->buffers[buffer.index], &active_streak,
                        &quiet_streak, &event)) {
                emit(event);
            }
        }

        if (ioctl(camera->fd, VIDIOC_QBUF, &buffer) < 0) break;
    }
}

static void *worker_main(void *argument)
{
    camera_t *camera = argument;
    int period = env_int("QZDESK_PRESENCE_FAKE_PERIOD", 6);

    if (env_flag("QZDESK_PRESENCE_FAKE")) {
        run_fake_backend(period);
    } else {
        run_v4l2_backend(camera);
    }

    if (camera) close_camera(camera);
    free(camera);
    set_text(status_text, sizeof(status_text), "摄像头已停止");
    return NULL;
}

/* ------------------------------------------------------------------------- *
 * 对外接口
 * ------------------------------------------------------------------------- */

bool qz_face_camera_available(void)
{
    if (env_flag("QZDESK_CAMERA_DISABLED")) return false;
    if (env_flag("QZDESK_PRESENCE_FAKE")) return true;
#if defined(QZDESK_SIMULATOR)
    /* 模拟器不主动抓桌面摄像头；要试就显式指一个节点 */
    return getenv("QZDESK_CAMERA_DEVICE") != NULL;
#else
    {
        char name[48];
        int fd = open_camera_device(name, sizeof(name));
        if (fd < 0) return false;
        close(fd);
        return true;
    }
#endif
}

bool qz_face_camera_start(qz_presence_cb_t callback, void *user_data)
{
    camera_t *camera;

    pthread_mutex_lock(&state_lock);
    if (worker_started) {
        pthread_mutex_unlock(&state_lock);
        return false;
    }
    stop_requested = false;
    presence_callback = callback;
    presence_callback_data = user_data;
    pthread_mutex_unlock(&state_lock);

    if (env_flag("QZDESK_CAMERA_DISABLED")) {
        set_text(status_text, sizeof(status_text), "摄像头已被配置禁用");
        return false;
    }

    camera = calloc(1, sizeof(*camera));
    if (!camera) return false;
    camera->fd = -1;

    if (env_flag("QZDESK_PRESENCE_FAKE")) {
        set_text(backend_name, sizeof(backend_name), "测试后端");
        set_text(status_text, sizeof(status_text), "测试后端：运行中");
    } else {
        char name[48];
        int fd = open_camera_device(name, sizeof(name));
        if (fd < 0) {
            free(camera);
            set_text(status_text, sizeof(status_text), "没有可用的摄像头（/dev/video*）");
            return false;
        }
        camera->fd = fd;
        if (!set_format(camera) || !start_streaming(camera)) {
            close_camera(camera);
            free(camera);
            set_text(status_text, sizeof(status_text), "摄像头不支持所需的 YUV 格式");
            return false;
        }
        {
            /* 名字里带上协商到的格式与分辨率：换板子/换摄像头时，这两项最能
             * 说明"到底有没有真的拿到画面"。 */
            char label[96];
            snprintf(label, sizeof(label), "%s %s %ux%u", name, format_name(camera->format),
                     camera->width, camera->height);
            set_text(backend_name, sizeof(backend_name), label);
        }
        set_text(status_text, sizeof(status_text), "运行中，正在检测是否有人");
    }

    if (pthread_create(&worker, NULL, worker_main, camera) != 0) {
        if (camera->fd >= 0) close_camera(camera);
        free(camera);
        set_text(status_text, sizeof(status_text), "无法启动检测线程");
        return false;
    }
    pthread_mutex_lock(&state_lock);
    worker_started = true;
    pthread_mutex_unlock(&state_lock);

    {
        char message[96];
        snprintf(message, sizeof(message), "%s", qz_face_camera_backend());
        fprintf(stderr, "[presence] 检测已启动：%s\n", message);
    }
    return true;
}

void qz_face_camera_stop(void)
{
    bool started;

    pthread_mutex_lock(&state_lock);
    started = worker_started;
    stop_requested = true;
    pthread_mutex_unlock(&state_lock);
    if (!started) return;

    pthread_join(worker, NULL);
    pthread_mutex_lock(&state_lock);
    worker_started = false;
    stop_requested = false;
    present = false;
    presence_callback = NULL;
    presence_callback_data = NULL;
    pthread_mutex_unlock(&state_lock);
    set_text(backend_name, sizeof(backend_name), "未启动");
}

const char *qz_face_camera_backend(void)
{
    return backend_name;
}

const char *qz_face_camera_status(void)
{
    return status_text;
}

bool qz_face_camera_running(void)
{
    bool value;
    pthread_mutex_lock(&state_lock);
    value = worker_started;
    pthread_mutex_unlock(&state_lock);
    return value;
}

bool qz_face_camera_present(void)
{
    bool value;
    pthread_mutex_lock(&state_lock);
    value = present;
    pthread_mutex_unlock(&state_lock);
    return value;
}

int qz_face_camera_arrivals(void)
{
    int value;
    pthread_mutex_lock(&state_lock);
    value = arrivals;
    pthread_mutex_unlock(&state_lock);
    return value;
}

unsigned long long qz_face_camera_last_seen(void)
{
    unsigned long long value;
    pthread_mutex_lock(&state_lock);
    value = last_seen;
    pthread_mutex_unlock(&state_lock);
    return value;
}
