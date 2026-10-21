/* Headless pixel checks of the production renderer in each panel format. */
#include "ai_face.h"
#include "scale.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIDE 240
static bool dark, reduced;
static unsigned char *pixels;
static int pixel_size;
static const char *output_dir;
static lv_obj_t *face;

bool qz_theme_is_dark(void) { return dark; }
bool qz_reduce_motion(void) { return reduced; }

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *map)
{
    int width = lv_area_get_width(area);
    for (int y = area->y1; y <= area->y2; y++) {
        memcpy(pixels + (y * SIDE + area->x1) * pixel_size,
               map + (y - area->y1) * width * pixel_size, width * pixel_size);
    }
    lv_display_flush_ready(display);
}

static void advance(int ms)
{
    for (int i = 0; i < ms; i += 16) {
        lv_tick_inc(16);
        lv_timer_handler();
    }
}

static uint32_t capture(const char *name)
{
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    uint32_t hash = 2166136261u;
    unsigned char rgb[SIDE * SIDE * 3];
    int body_pixels = 0, eye_pixels = 0;
    for (int i = 0; i < SIDE * SIDE; i++) {
        unsigned char *p = pixels + i * pixel_size;
        if (pixel_size == 2) {
            uint16_t c;
            memcpy(&c, p, sizeof(c));
            rgb[i * 3] = ((c >> 11) & 31) * 255 / 31;
            rgb[i * 3 + 1] = ((c >> 5) & 63) * 255 / 63;
            rgb[i * 3 + 2] = (c & 31) * 255 / 31;
        } else {
            rgb[i * 3] = p[2]; rgb[i * 3 + 1] = p[1]; rgb[i * 3 + 2] = p[0];
        }
        int r = rgb[i * 3], g = rgb[i * 3 + 1], b = rgb[i * 3 + 2];
        int bg_r = dark ? 24 : 246, bg_g = dark ? 24 : 247, bg_b = dark ? 27 : 249;
        if (abs(r - bg_r) + abs(g - bg_g) + abs(b - bg_b) > 35) body_pixels++;
        /* Count eyes within the central body, excluding the dark background. */
        int x = i % SIDE - SIDE / 2, y = i / SIDE - SIDE / 2;
        if (x * x + y * y < 70 * 70 && r < 50 && g < 50 && b < 50) eye_pixels++;
        for (int k = 0; k < 3; k++) hash = (hash ^ rgb[i * 3 + k]) * 16777619u;
    }
    if (name) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.ppm", output_dir, name);
        FILE *file = fopen(path, "wb");
        if (!file) { perror(path); exit(2); }
        fprintf(file, "P6\n%d %d\n255\n", SIDE, SIDE);
        fwrite(rgb, sizeof(rgb), 1, file);
        fclose(file);
        printf("FRAME %s hash=%08x body=%d eyes=%d\n", name, hash, body_pixels, eye_pixels);
    }
    if (body_pixels < 400) { fprintf(stderr, "Blank ball frame: %s\n", name); exit(1); }
    return hash;
}

static void require(bool condition, const char *message)
{
    if (!condition) { fprintf(stderr, "FAIL %s\n", message); exit(1); }
}

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    output_dir = argv[1];
    reduced = strcmp(argv[2], "reduced") == 0;
    int bits = atoi(argv[3]);
    pixel_size = bits / 8;
    require(bits == 16 || bits == 24 || bits == 32, "supported pixel format");
    qz_scale_init(480, 320);
    lv_init();
    lv_display_t *display = lv_display_create(SIDE, SIDE);
    lv_color_format_t format = bits == 16 ? LV_COLOR_FORMAT_RGB565 :
                               bits == 24 ? LV_COLOR_FORMAT_RGB888 : LV_COLOR_FORMAT_XRGB8888;
    lv_display_set_color_format(display, format);
    pixels = calloc(SIDE * SIDE, pixel_size);
    void *buffer = calloc(SIDE * SIDE, pixel_size);
    if (!pixels || !buffer) return 2;
    lv_display_set_buffers(display, buffer, NULL, SIDE * SIDE * pixel_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(display, flush);
    lv_timer_pause(lv_display_get_refr_timer(display));
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    face = qz_face_create(screen, 230);
    lv_obj_center(face);
    static const char *ids[] = {
        "00", "01", "02", "03", "04", "05", "06", "07",
        "10", "11", "12", "13", "14", "15", "16", "17", "18", "19", "20", "21",
        "30", "31", "32", "33", "34", "35", "36", "37", "38", "39", "40", "41"
    };
    for (int theme = 0; theme < 2; theme++) {
        dark = theme != 0;
        lv_obj_set_style_bg_color(screen, lv_color_hex(dark ? 0x18181B : 0xF6F7F9), 0);
        for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
            require(qz_face_emotion_valid(ids[i]), "numeric expression accepted");
            require(strlen(qz_face_emotion_text(ids[i])) > 0, "expression caption");
            qz_face_set_emotion(face, ids[i]);
            advance(2000);
            char name[64];
            snprintf(name, sizeof(name), "%s-%s", dark ? "dark" : "light", ids[i]);
            uint32_t settled = capture(name);
            qz_face_set_emotion(face, ids[i]);
            require(capture(NULL) == settled, "repeated expression does not restart transition");
        }
    }
    require(qz_face_emotion_valid(" THINKING ") && qz_face_emotion_valid("happy"), "protocol aliases");
    require(!qz_face_emotion_valid("cloud") && !qz_face_emotion_valid("99"), "invalid expression rejected");
    uint32_t before = capture(NULL);
    qz_face_set_emotion(face, "invalid");
    require(capture(NULL) == before, "invalid input leaves expression unchanged");
    qz_face_set_emotion(face, "02");
    advance(3000);
    before = capture("idle-a");
    advance(1200);
    uint32_t after = capture("idle-b");
    require(reduced ? before == after : before != after, "animation / reduced motion pixels");
    lv_obj_add_flag(face, LV_OBJ_FLAG_HIDDEN);
    advance(10000);
    lv_obj_remove_flag(face, LV_OBJ_FLAG_HIDDEN);
    require(capture(NULL) == after, "hidden face pauses animation");
    lv_obj_t *other = lv_obj_create(NULL);
    lv_screen_load(other);
    advance(5000);
    lv_screen_load(screen);
    require(capture(NULL) == after, "inactive screen pauses animation");
    /* The same renderer is used by the small desktop avatar. */
    lv_obj_delete(face);
    face = qz_face_create(screen, 40);
    lv_obj_center(face);
    qz_face_set_emotion(face, "10");
    advance(2000);
    capture("small");
    lv_obj_delete(face);
    advance(100);
    lv_display_delete(display);
    lv_deinit();
    free(buffer); free(pixels);
    printf("PASS 32 expressions, light/dark, %dbpp, %s, pause, aliases, lifecycle\n", bits, argv[2]);
    return 0;
}
