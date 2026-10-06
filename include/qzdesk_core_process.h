#ifndef QZDESK_CORE_PROCESS_API_H
#define QZDESK_CORE_PROCESS_API_H

#include <stdbool.h>

/*
 * Start the bundled core when it is available next to qzdesk_screen.
 * Set QZDESK_CORE_AUTOSTART=0 to keep the core as a separately managed process,
 * or QZDESK_CORE_BIN to point at a different executable.
 */
bool qzdesk_core_start(void);

/* Return true after the process receives SIGINT or SIGTERM. */
bool qzdesk_core_should_stop(void);

/* Return true when the simulator supervisor requested a UI restart. */
bool qzdesk_core_restart_requested(void);

/* Stop and reap a core process started by qzdesk_core_start(). */
void qzdesk_core_stop(void);

#endif
