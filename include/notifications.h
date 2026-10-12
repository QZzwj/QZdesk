#ifndef QZDESK_NOTIFICATIONS_H
#define QZDESK_NOTIFICATIONS_H
#include <stdbool.h>
void qz_notifications_init(void);
bool qz_notifications_dnd(void);
bool qz_notifications_set_dnd(bool enabled);
#endif
