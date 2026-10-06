#include "face_camera.h"
#include <stdlib.h>

static bool camera_running;

bool qz_face_camera_available(void)
{
#if defined(QZDESK_SIMULATOR)
    return false;
#else
    const char *disabled = getenv("QZDESK_CAMERA_DISABLED");
    return !(disabled && (disabled[0] == '1' || disabled[0] == 'y' || disabled[0] == 'Y'));
#endif
}

bool qz_face_camera_start(void)
{
    if (!qz_face_camera_available()) return false;
    /* The RV1106 backend is linked by the board integration and owns VI/RKNN
     * threads. Keep this generic target side-effect free until that backend is
     * supplied, rather than opening a desktop webcam accidentally. */
    camera_running = false;
    return false;
}

void qz_face_camera_stop(void)
{
    camera_running = false;
}

const char *qz_face_camera_status(void)
{
    if (!qz_face_camera_available()) return "模拟器已禁用摄像头";
    return camera_running ? "人脸识别运行中" : "摄像头未启动";
}
