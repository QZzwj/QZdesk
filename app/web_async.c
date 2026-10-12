#include "web_client.h"
#include "lvgl.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define REQUEST_MAX 12288
#define RESPONSE_MAX 49152

typedef struct {
    char method[8];
    char path[256];
    char body[REQUEST_MAX];
    char response[RESPONSE_MAX];
    bool done;
    bool ok;
    pthread_mutex_t mutex;
    qz_web_done_cb_t callback;
    void *user_data;
} web_job_t;

static void *web_worker(void *data)
{
    web_job_t *job = data;
    bool ok = qz_web_request(job->method, job->path, job->body,
                             job->response, sizeof(job->response));
    pthread_mutex_lock(&job->mutex);
    job->ok = ok;
    job->done = true;
    pthread_mutex_unlock(&job->mutex);
    return NULL;
}

static void web_completed(lv_timer_t *timer)
{
    web_job_t *job = lv_timer_get_user_data(timer);
    pthread_mutex_lock(&job->mutex);
    bool done = job->done;
    pthread_mutex_unlock(&job->mutex);
    if (!done) return;
    lv_timer_delete(timer);
    job->callback(job->ok, job->response, job->user_data);
    pthread_mutex_destroy(&job->mutex);
    free(job);
}

bool qz_web_request_async(const char *method, const char *path, const char *body,
                          qz_web_done_cb_t callback, void *user_data)
{
    if (!method || !path || !callback || strlen(method) >= 8 || strlen(path) >= 256 ||
        (body && strlen(body) >= REQUEST_MAX)) return false;
    web_job_t *job = calloc(1, sizeof(*job));
    if (!job) return false;
    snprintf(job->method, sizeof(job->method), "%s", method);
    snprintf(job->path, sizeof(job->path), "%s", path);
    snprintf(job->body, sizeof(job->body), "%s", body ? body : "");
    job->callback = callback;
    job->user_data = user_data;
    if (pthread_mutex_init(&job->mutex, NULL) != 0) { free(job); return false; }
    qz_web_port();
    pthread_t worker;
    if (pthread_create(&worker, NULL, web_worker, job) != 0) {
        pthread_mutex_destroy(&job->mutex);
        free(job);
        return false;
    }
    pthread_detach(worker);
    lv_timer_create(web_completed, 20, job);
    return true;
}
