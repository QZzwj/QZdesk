#ifndef QZDESK_FACE_CAMERA_H
#define QZDESK_FACE_CAMERA_H

#include <stdbool.h>

/* Camera access is deliberately isolated from the UI. The SDL simulator
 * reports unavailable and never probes /dev/video* or Rockchip devices. */
bool qz_face_camera_available(void);
bool qz_face_camera_start(void);
void qz_face_camera_stop(void);
const char *qz_face_camera_status(void);

#endif
