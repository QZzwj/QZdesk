#include "skills.h"
#include "web_client.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* 与核心的本地 Skill 服务说话。
 *
 * HTTP 与扁平 JSON 的实现在 app/web_client.c 里共用 —— 智能家居的设备表要走
 * 同一个本地服务、同一套扫描逻辑，与其抄一份不如共一份。这里只负责把响应翻译
 * 成界面要的结构。 */

#define RESPONSE_MAX 16384

static qz_skill_role_t role_from_key(const char *key)
{
    if (key && strcmp(key, "primary") == 0) return QZ_SKILL_ROLE_PRIMARY;
    if (key && strcmp(key, "secondary") == 0) return QZ_SKILL_ROLE_SECONDARY;
    return QZ_SKILL_ROLE_NONE;
}

int qz_skills_fetch(qz_skill_t *output, int max_count, qz_skill_summary_t *summary)
{
    static char body[RESPONSE_MAX];
    char role[24];
    int count = 0;
    int declared = 0;

    if (summary) memset(summary, 0, sizeof(*summary));
    if (!output || max_count <= 0) return -1;
    if (!qz_web_request("GET", "/api/skills", NULL, body, sizeof(body))) return -1;

    qz_json_int(body, "count", &declared);
    for (int i = 0; i < max_count; i++) {
        const char *object = qz_json_item(body, "skills", i);
        qz_skill_t *skill;
        if (!object) break;
        skill = &output[count];
        memset(skill, 0, sizeof(*skill));
        if (!qz_json_string(object, "id", skill->id, sizeof(skill->id))) {
            continue;
        }
        if (!qz_json_string(object, "name", skill->name, sizeof(skill->name)) ||
            skill->name[0] == '\0') {
            snprintf(skill->name, sizeof(skill->name), "%s", skill->id);
        }
        qz_json_string(object, "description", skill->description, sizeof(skill->description));
        role[0] = '\0';
        qz_json_string(object, "role", role, sizeof(role));
        skill->role = role_from_key(role);

        if (summary) {
            if (skill->role == QZ_SKILL_ROLE_PRIMARY) summary->primary++;
            else if (skill->role == QZ_SKILL_ROLE_SECONDARY) summary->secondary++;
        }
        count++;
    }

    if (summary) summary->count = declared > 0 ? declared : count;
    return count;
}

bool qz_skills_set_role(const char *id, qz_skill_role_t role)
{
    char path[160];
    char body[64];
    static char response[512];

    if (!id || id[0] == '\0') return false;
    /* The identifier is a directory name; refuse anything that could break the
     * request line. */
    if (strlen(id) >= QZ_SKILL_ID_MAX) return false;
    for (const char *p = id; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_' && *p != '.') return false;
    }

    snprintf(path, sizeof(path), "/api/skills/%s/selection", id);
    snprintf(body, sizeof(body), "{\"role\":\"%s\"}", qz_skills_role_key(role));
    return qz_web_request("PUT", path, body, response, sizeof(response));
}

const char *qz_skills_role_key(qz_skill_role_t role)
{
    switch (role) {
        case QZ_SKILL_ROLE_PRIMARY: return "primary";
        case QZ_SKILL_ROLE_SECONDARY: return "secondary";
        default: return "none";
    }
}

const char *qz_skills_role_label(qz_skill_role_t role)
{
    switch (role) {
        case QZ_SKILL_ROLE_PRIMARY: return "主技能";
        case QZ_SKILL_ROLE_SECONDARY: return "备用";
        default: return "关闭";
    }
}
