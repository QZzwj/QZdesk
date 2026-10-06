/* 运动相机模式的取景数据源。接口说明见 include/camera_preview.h。
 *
 * 采集模板沿用 face_camera.c（同一套 V4L2 写法），差异有三处：
 *
 *   1. 格式要**彩色**：优先 NV12，退回 YUYV —— 帧差只要亮度，预览要颜色；
 *   2. 帧不丢弃：NV12/YUYV 逐像素转 RGB565、最近邻缩放到面板尺寸，写进
 *      三缓冲（写线程永远只写「显示中」之外的两块，界面取走的是稳定的）；
 *   3. 有快门：把最近一帧落成 24 位 BMP。
 *
 * 两个后端，走同一套缓冲与状态：
 *
 *   1. **V4L2**（默认）：与存在检测一致，显式 QZDESK_CAMERA_DEVICE 优先，
 *      否则探测 /dev/video0..3。同一颗摄像头同时只能被一个模块打开 ——
 *      运动相机页打开时先停存在检测，退出时恢复（applets.c 负责）。
 *   2. **测试后端**（QZDESK_PREVIEW_FAKE=1）：生成会动的彩条画面，没有
 *      摄像头的机器（开发机、SDL 模拟器、CI）上验证整条链路。
 *
 * 环境变量：
 *   QZDESK_CAMERA_DEVICE=/dev/videoN  指定节点
 *   QZDESK_PREVIEW_FPS=N              转换/上屏帧率（默认 12，RV1106 的
 *                                     CPU 要留给核心，别开 30）
 *   QZDESK_PREVIEW_FAKE=1             用测试后端
 *   QZDESK_CAM_DIR=/path              快门存图目录（默认 /tmp/qzdesk-cam） */

#include "camera_preview.h"
#include "scale.h"

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
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define PREVIEW_MAX_BUFFERS 4
#define PREVIEW_BUFFERS 3    /**< RGB565 三缓冲：1 块在显示、1 块在写、1 块富余 */
#define PREVIEW_CAP_W 640    /**< 向驱动申请的采集分辨率（驱动可能改小） */
#define PREVIEW_CAP_H 480

typedef enum {
    SOURCE_NONE,
    SOURCE_V4L2,
    SOURCE_FAKE,
} source_kind_t;

typedef enum {
    PIXEL_NONE,
    PIXEL_NV12,  /**< Y 平面在前，UV 半分辨率交错在后 */
    PIXEL_YUYV,  /**< 每像素 2 字节：Y0 U Y1 V */
} pixel_format_t;

typedef struct {
    int fd;
    void *buffers[PREVIEW_MAX_BUFFERS];
    size_t lengths[PREVIEW_MAX_BUFFERS];
    unsigned int count;
    unsigned int width;
    unsigned int height;
    unsigned int stride;
    pixel_format_t format;
} camera_t;

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static bool worker_started;
static bool stop_requested;
static source_kind_t source;

/* RGB565 三缓冲与帧序号。latest 是界面可取的那块；writing 是写线程手里的
 * 那块；swap 时两者互换。面板尺寸在启动时从 scale.h 取（运行时定）。 */
static uint8_t *frames[PREVIEW_BUFFERS];
static int latest_index;       /**< 最新完成的帧；界面取的就是这块 */
static int handed_index = -1;  /**< 界面上次取走的那块（渲染期间不能被写） */
static uint32_t frame_id;      /**< 完成的帧序号；界面拿它判断有没有新帧 */
static int frame_w;
static int frame_h;
static uint32_t delivered_id;   /**< 界面上次取走的序号（仅诊断用） */

static int photo_count;
static char backend_name[80] = "未启动";
static char status_text[96] = "取景未启动";

/* ------------------------------------------------------------------------- *
 * 小工具
 * ------------------------------------------------------------------------- */

static int env_int(const char *name, int fallback)
{
    const char *value = getenv(name);
    int parsed;
    if (!value || !*value) return fallback;
    parsed = atoi(value);
    return parsed > 0 ? parsed : fallback;
}

static bool env_flag(const char *name)
{
    const char *value = getenv(name);
    return value && (value[0] == '1' || value[0] == 'y' || value[0] == 'Y');
}

static void set_text(char *target, size_t size, const char *text)
{
    int limit = size > 0 ? (int)size - 1 : 0;
    pthread_mutex_lock(&state_lock);
    snprintf(target, size, "%.*s", limit, text ? text : "");
    pthread_mutex_unlock(&state_lock);
}

static unsigned long long monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (unsigned long long)now.tv_sec * 1000ULL + (unsigned long long)(now.tv_nsec / 1000000);
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
 * V4L2 采集（写法与 face_camera.c 一致，只换格式偏好与分辨率）
 * ------------------------------------------------------------------------- */

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

/** 申请采集格式：NV12 优先（RV1106 的 ISP 原生输出），退回 YUYV。 */
static bool set_format(camera_t *camera)
{
    struct v4l2_format format;
    struct v4l2_fmtdesc description;
    bool has_nv12 = false;
    bool has_yuyv = false;

    memset(&description, 0, sizeof(description));
    description.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    for (description.index = 0;; description.index++) {
        if (ioctl(camera->fd, VIDIOC_ENUM_FMT, &description) < 0) break;
        if (description.pixelformat == V4L2_PIX_FMT_NV12 ||
            description.pixelformat == V4L2_PIX_FMT_YUV420) has_nv12 = true;
        if (description.pixelformat == V4L2_PIX_FMT_YUYV) has_yuyv = true;
    }

    for (int attempt = 0; attempt < 2; attempt++) {
        unsigned int wanted;
        if (attempt == 0 && has_nv12) wanted = V4L2_PIX_FMT_NV12;
        else if (has_yuyv) wanted = V4L2_PIX_FMT_YUYV;
        else continue;

        memset(&format, 0, sizeof(format));
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width = PREVIEW_CAP_W;
        format.fmt.pix.height = PREVIEW_CAP_H;
        format.fmt.pix.pixelformat = wanted;
        format.fmt.pix.field = V4L2_FIELD_NONE;
        if (ioctl(camera->fd, VIDIOC_S_FMT, &format) < 0) continue;
        if (format.fmt.pix.pixelformat == V4L2_PIX_FMT_NV12 ||
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
    request.count = PREVIEW_MAX_BUFFERS;
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
    memset(camera, 0, sizeof(*camera));
    camera->fd = -1;
}

/* ------------------------------------------------------------------------- *
 * 像素转换：NV12 / YUYV → RGB565，最近邻缩放到面板尺寸
 * ------------------------------------------------------------------------- */

static inline uint16_t yuv_to_rgb565(int y, int u, int v)
{
    int r = y + ((1432 * (v - 128)) >> 10);            /* ≈ 1.402 */
    int g = y - ((351 * (u - 128) + 731 * (v - 128)) >> 10);
    int b = y + ((1815 * (u - 128)) >> 10);            /* ≈ 1.772 */
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/** 把一帧 YUV 写进 RGB565 缓冲（面板尺寸，最近邻）。 */
static void convert_frame(const camera_t *camera, const unsigned char *data, uint8_t *out)
{
    int dst_w = frame_w;
    int dst_h = frame_h;
    int src_w = (int)camera->width;
    int src_h = (int)camera->height;
    const unsigned char *y_plane = data;
    const unsigned char *uv_plane = data + (size_t)camera->stride * camera->height;

    for (int dy = 0; dy < dst_h; dy++) {
        int sy = dy * src_h / dst_h;
        uint16_t *row = (uint16_t *)(out + (size_t)dy * dst_w * 2);
        if (camera->format == PIXEL_NV12) {
            const unsigned char *y_row = y_plane + (size_t)sy * camera->stride;
            const unsigned char *uv_row = uv_plane + (size_t)(sy / 2) * camera->stride;
            for (int dx = 0; dx < dst_w; dx++) {
                int sx = dx * src_w / dst_w;
                int y = y_row[sx];
                int u = uv_row[(sx & ~1)] - 128;
                int v = uv_row[(sx & ~1) + 1] - 128;
                row[dx] = yuv_to_rgb565(y, u, v);
            }
        } else {  /* YUYV：一像素一字节 Y，U/V 两像素共用 */
            const unsigned char *y_row = y_plane + (size_t)sy * camera->stride;
            for (int dx = 0; dx < dst_w; dx++) {
                int sx = dx * src_w / dst_w;
                int pair = sx & ~1;
                int y = y_row[sx * 2];
                int u = y_row[pair * 2 + 1] - 128;
                int v = y_row[pair * 2 + 3] - 128;
                row[dx] = yuv_to_rgb565(y, u, v);
            }
        }
    }
}

/* ------------------------------------------------------------------------- *
 * 测试后端：会动的彩条 + 移动的斜块，链路验证用
 * ------------------------------------------------------------------------- */

static void fake_frame(uint8_t *out, unsigned long long tick)
{
    static const uint16_t bars[8] = {
        0xF800, 0xFC00, 0xFFE0, 0x07E0, 0x07FF, 0x00FF, 0xF81F, 0xFFFF,
    };
    int dst_w = frame_w;
    int dst_h = frame_h;
    int block = dst_w / 4 > 0 ? dst_w / 4 : 40;
    int bx = (int)((tick * 3) % (unsigned long long)(dst_w + block)) - block;
    int by = (int)((tick * 2) % (unsigned long long)(dst_h + block)) - block;

    for (int y = 0; y < dst_h; y++) {
        uint16_t *row = (uint16_t *)(out + (size_t)y * dst_w * 2);
        for (int x = 0; x < dst_w; x++) {
            uint16_t color = bars[(x * 8) / dst_w];
            if (x >= bx && x < bx + block && y >= by && y < by + block) {
                color = 0x0000;                      /* 移动的黑块证明帧在刷新 */
            } else if (y >= dst_h - 18) {
                color = 0x0000;                      /* 底部黑条：放帧号用的 */
            }
            row[x] = color;
        }
    }
    /* 底条上按帧号点亮刻度：动没动、帧率多少，一眼可辨 */
    int marks = (int)(tick % 20);
    uint16_t *bottom = (uint16_t *)(out + (size_t)(dst_h - 16) * dst_w * 2);
    for (int x = 0; x < dst_w; x += 8) {
        int index = x / 8;
        if (index <= marks) bottom[x] = 0xFFFF;
    }
}

/* ------------------------------------------------------------------------- *
 * 工作线程
 * ------------------------------------------------------------------------- */

/** 挑一块既不是「最新」也不是「界面上次取走」的缓冲来写：两块都在渲染
 * 生命周期里（latest 可能正被 LVGL 扫描，handed 是上一次取走还没换掉的），
 * 三缓冲正好剩一块。 */
static int pick_writable(void)
{
    for (int i = 0; i < PREVIEW_BUFFERS; i++) {
        if (i != latest_index && i != handed_index) return i;
    }
    return -1;
}

/** 写线程主体：起后端、循环出帧，直到 stop_requested。 */
static void *run_preview(void *argument)
{
    (void)argument;
    char backend[80] = "";
    camera_t camera;
    int fps = env_int("QZDESK_PREVIEW_FPS", 12);
    unsigned long long tick = 0;
    unsigned long long last_ms = 0;
    int interval_ms = 1000 / (fps > 0 && fps <= 30 ? fps : 12);

    memset(&camera, 0, sizeof(camera));
    camera.fd = -1;

    if (env_flag("QZDESK_PREVIEW_FAKE")) {
        source = SOURCE_FAKE;
        snprintf(backend, sizeof(backend), "测试后端");
    } else {
        camera.fd = open_camera_device(backend, sizeof(backend));
        if (camera.fd < 0 || !set_format(&camera) || !start_streaming(&camera)) {
            set_text(status_text, sizeof(status_text),
                     camera.fd < 0 ? "打不开摄像头（QZDESK_CAMERA_DEVICE 或 /dev/video0..3）"
                                   : "驱动不认 NV12/YUYV 或申请缓冲失败");
            if (camera.fd >= 0) close_camera(&camera);
            source = SOURCE_NONE;
            set_text(backend_name, sizeof(backend_name), "未启动");
            return NULL;
        }
        source = SOURCE_V4L2;
        {
            char detail[48];
            snprintf(detail, sizeof(detail), " %ux%u %s", camera.width, camera.height,
                     camera.format == PIXEL_NV12 ? "NV12" : "YUYV");
            strncat(backend, detail, sizeof(backend) - strlen(backend) - 1);
        }
    }
    set_text(backend_name, sizeof(backend_name), backend);
    set_text(status_text, sizeof(status_text), "取景中");

    last_ms = monotonic_ms();
    while (!should_stop()) {
        unsigned long long now = monotonic_ms();
        uint8_t *target;

        if (now - last_ms < (unsigned long long)interval_ms) {
            if (source == SOURCE_V4L2) {
                /* 有帧就先收走，避免驱动缓冲堆积；限速在转换这一侧做 */
                struct pollfd fds;
                int ready;
                fds.fd = camera.fd;
                fds.events = POLLIN;
                ready = poll(&fds, 1, 4);
                if (ready > 0) {
                    struct v4l2_buffer buffer;
                    memset(&buffer, 0, sizeof(buffer));
                    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                    buffer.memory = V4L2_MEMORY_MMAP;
                    if (ioctl(camera.fd, VIDIOC_DQBUF, &buffer) == 0) {
                        ioctl(camera.fd, VIDIOC_QBUF, &buffer);
                    }
                }
            } else {
                struct timespec pause;
                pause.tv_sec = 0;
                pause.tv_nsec = 4 * 1000 * 1000;
                nanosleep(&pause, NULL);
            }
            continue;
        }
        last_ms = now;

        pthread_mutex_lock(&state_lock);
        int slot = pick_writable();
        target = slot >= 0 ? frames[slot] : NULL;
        pthread_mutex_unlock(&state_lock);
        if (!target) continue;

        if (source == SOURCE_V4L2) {
            struct pollfd fds;
            struct v4l2_buffer buffer;
            int ready;

            fds.fd = camera.fd;
            fds.events = POLLIN;
            ready = poll(&fds, 1, 200);
            if (ready <= 0) continue;
            memset(&buffer, 0, sizeof(buffer));
            buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            if (ioctl(camera.fd, VIDIOC_DQBUF, &buffer) < 0) continue;
            convert_frame(&camera, camera.buffers[buffer.index], target);
            ioctl(camera.fd, VIDIOC_QBUF, &buffer);
        } else {
            fake_frame(target, tick);
        }

        /* 发布：写好的缓冲成为 latest。之前说过，刚写完的这块此刻没有任何
         * 读者 —— 界面要么还拿着上一块 handed，要么下一次取帧才换过来。 */
        pthread_mutex_lock(&state_lock);
        latest_index = slot;
        frame_id++;
        pthread_mutex_unlock(&state_lock);

        tick++;
    }

    if (source == SOURCE_V4L2) close_camera(&camera);
    return NULL;
}

/* ------------------------------------------------------------------------- *
 * Public API
 * ------------------------------------------------------------------------- */

bool qz_cam_preview_available(void)
{
    if (env_flag("QZDESK_PREVIEW_FAKE")) return true;
    if (getenv("QZDESK_CAMERA_DEVICE") && *getenv("QZDESK_CAMERA_DEVICE")) return true;
    for (int index = 0; index < 4; index++) {
        char path[32];
        int fd;
        snprintf(path, sizeof(path), "/dev/video%d", index);
        fd = open_capture_node(path);
        if (fd >= 0) {
            close(fd);
            return true;
        }
    }
    return false;
}

bool qz_cam_preview_start(void)
{
    pthread_mutex_lock(&state_lock);
    if (worker_started) {
        pthread_mutex_unlock(&state_lock);
        return false;
    }
    stop_requested = false;
    frame_w = (int)qz_panel_w;
    frame_h = (int)qz_panel_h;
    for (int i = 0; i < PREVIEW_BUFFERS; i++) {
        free(frames[i]);
        frames[i] = malloc((size_t)frame_w * frame_h * 2);
        if (!frames[i]) {
            for (int j = 0; j <= i; j++) {
                free(frames[j]);
                frames[j] = NULL;
            }
            pthread_mutex_unlock(&state_lock);
            set_text(status_text, sizeof(status_text), "申请帧缓冲失败（内存不足）");
            return false;
        }
    }
    latest_index = 0;
    handed_index = -1;
    frame_id = 0;
    delivered_id = 0;
    pthread_mutex_unlock(&state_lock);

    if (pthread_create(&worker, NULL, run_preview, NULL) != 0) {
        set_text(status_text, sizeof(status_text), "起不来取景线程");
        pthread_mutex_lock(&state_lock);
        for (int i = 0; i < PREVIEW_BUFFERS; i++) {
            free(frames[i]);
            frames[i] = NULL;
        }
        pthread_mutex_unlock(&state_lock);
        return false;
    }
    worker_started = true;
    return true;
}

void qz_cam_preview_stop(void)
{
    pthread_mutex_lock(&state_lock);
    if (!worker_started) {
        pthread_mutex_unlock(&state_lock);
        return;
    }
    stop_requested = true;
    pthread_mutex_unlock(&state_lock);

    pthread_join(worker, NULL);
    worker_started = false;
    source = SOURCE_NONE;
    set_text(backend_name, sizeof(backend_name), "未启动");
    set_text(status_text, sizeof(status_text), "取景未启动");
    pthread_mutex_lock(&state_lock);
    for (int i = 0; i < PREVIEW_BUFFERS; i++) {
        free(frames[i]);
        frames[i] = NULL;
    }
    frame_id = 0;
    pthread_mutex_unlock(&state_lock);
}

bool qz_cam_preview_running(void)
{
    bool value;
    pthread_mutex_lock(&state_lock);
    value = worker_started;
    pthread_mutex_unlock(&state_lock);
    return value;
}

const char *qz_cam_preview_backend(void)
{
    return backend_name;
}

const char *qz_cam_preview_status(void)
{
    return status_text;
}

const uint8_t *qz_cam_preview_frame(uint32_t *id)
{
    const uint8_t *buffer = NULL;

    pthread_mutex_lock(&state_lock);
    if (frames[latest_index] && frame_id != delivered_id) {
        buffer = frames[latest_index];
        delivered_id = frame_id;
        handed_index = latest_index;   /* 这块进入渲染生命周期，写线程避开它 */
        if (id) *id = frame_id;
    }
    pthread_mutex_unlock(&state_lock);
    return buffer;
}

bool qz_cam_preview_snapshot(char *path, unsigned int path_size)
{
    const char *dir = getenv("QZDESK_CAM_DIR");
    char directory[160];
    char file[224];
    uint8_t *copy;
    unsigned char header[54];
    unsigned int w;
    unsigned int h;
    unsigned int row_bytes;
    FILE *out;
    bool ok = true;

    if (!worker_started || frame_id == 0) {
        set_text(status_text, sizeof(status_text), "还没有画面，等出帧再拍");
        return false;
    }
    snprintf(directory, sizeof(directory), "%.*s", (int)sizeof(directory) - 1,
             dir && *dir ? dir : "/tmp/qzdesk-cam");
    mkdir(directory, 0755);   /* 已存在时 EEXIST，忽略 */

    pthread_mutex_lock(&state_lock);
    w = (unsigned int)frame_w;
    h = (unsigned int)frame_h;
    copy = malloc((size_t)w * h * 2);
    if (copy && frames[latest_index]) memcpy(copy, frames[latest_index], (size_t)w * h * 2);
    photo_count++;
    snprintf(file, sizeof(file), "%s/cam_%03d.bmp", directory, photo_count);
    pthread_mutex_unlock(&state_lock);

    if (!copy) {
        set_text(status_text, sizeof(status_text), "拍不了：内存不足");
        return false;
    }

    row_bytes = w * 3;
    row_bytes = (row_bytes + 3) & ~3u;
    memset(header, 0, sizeof(header));
    header[0] = 'B'; header[1] = 'M';
    {
        unsigned int image_size = row_bytes * h;
        unsigned int file_size = 54 + image_size;
        header[2] = (unsigned char)(file_size);
        header[3] = (unsigned char)(file_size >> 8);
        header[4] = (unsigned char)(file_size >> 16);
        header[5] = (unsigned char)(file_size >> 24);
        header[10] = 54;                                   /* 像素数据偏移 */
        header[14] = 40;                                   /* BITMAPINFOHEADER */
        header[18] = (unsigned char)(w);
        header[19] = (unsigned char)(w >> 8);
        header[20] = (unsigned char)(w >> 16);
        header[21] = (unsigned char)(w >> 24);
        header[22] = (unsigned char)(h);
        header[23] = (unsigned char)(h >> 8);
        header[24] = (unsigned char)(h >> 16);
        header[25] = (unsigned char)(h >> 24);
        header[26] = 1;                                    /* planes */
        header[28] = 24;                                   /* bpp */
        header[34] = (unsigned char)(image_size);
        header[35] = (unsigned char)(image_size >> 8);
        header[36] = (unsigned char)(image_size >> 16);
        header[37] = (unsigned char)(image_size >> 24);
    }

    out = fopen(file, "wb");
    if (!out) {
        set_text(status_text, sizeof(status_text), "存不了图：目录写不进去");
        free(copy);
        return false;
    }
    ok = fwrite(header, 1, sizeof(header), out) == sizeof(header);
    for (unsigned int y = 0; ok && y < h; y++) {
        /* BMP 自底向上、BGR 序；RGB565 行直接翻回去 */
        unsigned int src_y = h - 1 - y;
        const uint16_t *line = (const uint16_t *)(copy + (size_t)src_y * w * 2);
        unsigned char rgb[3];
        for (unsigned int x = 0; x < w; x++) {
            uint16_t pixel = line[x];
            rgb[0] = (unsigned char)(((pixel & 0x001F) << 3) | ((pixel >> 2) & 0x07));
            rgb[1] = (unsigned char)(((pixel & 0x07E0) >> 3) | ((pixel >> 9) & 0x03));
            rgb[2] = (unsigned char)((pixel >> 8) & 0xF8);
            if (fwrite(rgb, 1, 3, out) != 3) { ok = false; break; }
        }
        for (unsigned int pad = w * 3; pad < row_bytes; pad++) {
            if (fputc(0, out) == EOF) { ok = false; break; }
        }
    }
    if (fclose(out) != 0) ok = false;
    free(copy);
    if (!ok) {
        set_text(status_text, sizeof(status_text), "写图失败（磁盘？）");
        remove(file);
        return false;
    }
    set_text(status_text, sizeof(status_text), "已保存");
    if (path && path_size) snprintf(path, path_size, "%.*s", (int)path_size - 1, file);
    return true;
}

int qz_cam_preview_photos(void)
{
    int value;
    pthread_mutex_lock(&state_lock);
    value = photo_count;
    pthread_mutex_unlock(&state_lock);
    return value;
}
