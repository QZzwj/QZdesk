#ifndef QZDESK_SKILLS_H
#define QZDESK_SKILLS_H

#include <stdbool.h>

/* Client for the core's local Skill service. The core serves a small HTTP API
 * on QZDESK_SKILL_WEB_PORT (8080 by default, see xiaozhi_core/src/skill_web.rs)
 * and the device UI reads the same endpoints the web console uses:
 *
 *   GET  /api/skills                      -> skill list + summary
 *   PUT  /api/skills/<id>/selection       -> {"role":"primary"|"secondary"|"none"}
 */

#define QZ_SKILL_ID_MAX 48
#define QZ_SKILL_NAME_MAX 64
#define QZ_SKILL_DESC_MAX 160
#define QZ_SKILL_MAX 12

typedef enum {
    QZ_SKILL_ROLE_NONE,       /**< 关闭：保留在设备上但不参与对话 */
    QZ_SKILL_ROLE_PRIMARY,    /**< 主技能：会话开始即作为指令加载 */
    QZ_SKILL_ROLE_SECONDARY,  /**< 备用技能：按需检索 */
} qz_skill_role_t;

typedef struct {
    char id[QZ_SKILL_ID_MAX];        /**< 目录名，接口里的标识 */
    char name[QZ_SKILL_NAME_MAX];    /**< 展示名 */
    char description[QZ_SKILL_DESC_MAX];
    qz_skill_role_t role;
} qz_skill_t;

typedef struct {
    int primary;
    int secondary;
    int count;
} qz_skill_summary_t;

/**
 * Read the skill list through the local service.
 * Returns the number of entries written, or -1 when the service cannot be
 * reached (the core is not running, or it was started with the web UI off).
 */
int qz_skills_fetch(qz_skill_t *output, int max_count, qz_skill_summary_t *summary);

/** Change one skill's role. The core reconnects the session so it takes effect
 *  immediately. Returns false when the service rejected or could not be
 *  reached. */
bool qz_skills_set_role(const char *id, qz_skill_role_t role);

/** "primary" / "secondary" / "none" for a role. */
const char *qz_skills_role_key(qz_skill_role_t role);
/** 主技能 / 备用 / 关闭 */
const char *qz_skills_role_label(qz_skill_role_t role);

#endif
