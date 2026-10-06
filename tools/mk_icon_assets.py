#!/usr/bin/env python3
"""Bake the UI icon set into LVGL A8 glyph arrays.

Usage (from the repository root):

    python3 QZdesk-Demo/tools/mk_icon_assets.py [--force-fetch] [--preview]

What it does per icon in `assets/icons/*.svg` (Tabler Icons, MIT):

1. fetch the SVG from the Tabler mirror if it is missing (through gh-proxy,
   because raw.githubusercontent.com is not reachable directly here);
2. render it with headless Chromium at 4x the target size on a transparent
   background — 4x then a box downsample is what keeps a 2px stroke smooth at
   16px, the same trick the mascot baker uses;
3. box-downsample to the exact sizes listed in SIZES and keep the alpha channel;
4. rewrite `app/icon_assets.c` + `include/icon_assets.h` with one LV_COLOR_FORMAT_A8
   descriptor per (icon, size).

A8 + `image_recolor` is the point: one asset per size, tinted at runtime with the
active theme colour, so light and dark themes need no second copy of every icon.
The icons keep their original SVG geometry — no scaling artefacts from growing a
small bitmap, because every size is rendered from the vector.
"""
import os
import re
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SVG_DIR = os.path.join(ROOT, "assets", "icons")
PNG_DIR = os.path.join(HERE, "icon-png")
OUT_C = os.path.join(ROOT, "app", "icon_assets.c")
OUT_H = os.path.join(ROOT, "include", "icon_assets.h")

CHROME = os.path.expanduser(
    "~/.cache/ms-playwright/chromium-1243/chrome-linux64/chrome"
)
MIRRORS = [
    "https://gh-proxy.com/https://raw.githubusercontent.com/tabler/tabler-icons/main/icons/outline/{name}.svg",
    "https://cdn.jsdelivr.net/npm/@tabler/icons@3.31.0/icons/outline/{name}.svg",
]

SUPERSAMPLE = 4  # render at 4x, then box-downsample
# 小尺寸图标笔画偏细：16px 那一档把 stroke-width 抬一点，视觉重量才和 24px 一致
SMALL_BOOST = {16: 2.35}

# 个别图标在不同版本的 Tabler 里改过名：按顺序试，第一个存在的就用
ALIASES = {
    "timer": ["timer", "alarm", "clock-play"],
    "alert-triangle": ["alert-triangle", "triangle-alert"],
    "info-circle": ["info-circle", "info-square"],
    "circle-check": ["circle-check", "circle-check"],
    "help-circle": ["help-circle", "circle-help"],
}

# name -> 需要烘焙的像素尺寸（只烘真正用到的，避免资源膨胀）
# 天气图标多两档：卡片 40px、详情页 72px。
SIZES = {
    # —— 天气 ——（卡片 40、详情页 96，各档供小图标复用）
    "sun": [16, 24, 40, 96],
    "cloud": [16, 24, 40, 96],
    "cloud-rain": [16, 24, 40, 96],
    "cloud-snow": [16, 24, 40, 96],
    "cloud-storm": [16, 24, 40, 96],
    "cloud-fog": [16, 24, 40, 96],
    "help-circle": [16, 24, 40],
    "droplet": [16, 24],
    "wind": [16, 24],
    "thermometer": [16, 24],
    "arrow-up": [14, 20],
    "arrow-down": [14, 20],
    "clock": [14, 20],
    "map-pin": [14, 20],

    # —— 状态栏 ——
    "wifi": [16, 20],
    "battery-4": [16, 20],
    "battery-3": [16, 20],
    "battery-2": [16, 20],
    "battery-1": [16, 20],
    "battery-off": [16, 20],
    "bolt": [16, 20],

    # —— 导航 / 通用 ——
    "chevron-left": [18, 22],
    "chevron-right": [14, 18],
    "x": [18, 22],
    "refresh": [18, 22],
    "plus": [18, 22],
    "check": [16, 20],
    "settings": [20, 26],
    "layout-grid": [20, 26],
    "list": [20, 26],
    "bell": [16, 20],
    "volume": [16, 20],
    "microphone": [20, 26],
    "player-play": [18, 22],
    "repeat": [18, 22],
    "eye": [16, 20],
    "eye-off": [16, 20],
    "keyboard": [16, 20],
    "file-text": [16, 20],
    "pencil": [16, 20],
    "download": [16, 20],
    "photo": [16, 20],
    "power": [16, 20],
    "trash": [16, 20],
    "activity": [16, 20],
    "cpu": [16, 20],
    "chart-bar": [16, 20],
    "database": [16, 20],
    "device-desktop": [16, 20],
    "sparkles": [16, 20],
    "tool": [16, 20],
    "bulb": [16, 20],
    "plug": [16, 20],
    "timer": [16, 20],
    "home": [16, 20],
    "folder": [16, 20],
    "shield": [16, 20],
    "user": [16, 20],
    "palette": [16, 20],
    "rotate": [16, 20],
    "info-circle": [16, 20],
    "alert-triangle": [16, 20],
    "circle-check": [16, 20],
}


def fetch(name, force=False):
    path = os.path.join(SVG_DIR, name + ".svg")
    if os.path.exists(path) and not force:
        return path
    os.makedirs(SVG_DIR, exist_ok=True)
    last = None
    for candidate in ALIASES.get(name, [name]):
        for pattern in MIRRORS:
            url = pattern.format(name=candidate)
            try:
                with urllib.request.urlopen(url, timeout=30) as response:
                    body = response.read()
                if b"<svg" not in body:
                    raise ValueError("not an SVG")
                with open(path, "wb") as handle:
                    handle.write(body)
                print("  fetched %-18s %5d bytes (%s)" % (name, len(body), candidate))
                return path
            except Exception as error:  # 换镜像 / 换候选名重试
                last = error
    raise SystemExit("无法下载 %s：%s" % (name, last))


def svg_markup(name, px):
    """Inline the SVG at `px` px with a white stroke: the alpha channel is what we keep."""
    with open(os.path.join(SVG_DIR, name + ".svg"), "r", encoding="utf-8") as handle:
        text = handle.read()
    text = re.sub(r"<!--.*?-->", "", text, flags=re.S)
    text = text.replace("currentColor", "#ffffff")
    if px in SMALL_BOOST:
        text = re.sub(r'stroke-width="[0-9.]+"', 'stroke-width="%s"' % SMALL_BOOST[px], text)
    # 覆盖根节点尺寸，viewBox 会让整幅图（含笔画）等比缩放。
    # 注意不能写成 \bwidth= —— 那也会命中 stroke-width（横杠是词边界）。
    text = re.sub(r'(?<![-\w])width="[^"]*"', 'width="%d"' % (px * SUPERSAMPLE), text, count=1)
    text = re.sub(r'(?<![-\w])height="[^"]*"', 'height="%d"' % (px * SUPERSAMPLE), text, count=1)
    return text


def render(cells, cell_px):
    """One Chromium pass: every icon centred in its own cell, transparent background."""
    columns = 8
    rows = (len(cells) + columns - 1) // columns
    width = columns * cell_px
    height = rows * cell_px
    divs = "".join(
        '<div class="cell">%s</div>' % svg_markup(name, px) for name, px in cells
    )
    html = """<!doctype html><meta charset="utf-8"><style>
html,body{margin:0;padding:0;background:transparent}
.grid{display:grid;grid-template-columns:repeat(%d,%dpx);grid-auto-rows:%dpx}
.cell{display:flex;align-items:center;justify-content:center;overflow:hidden}
</style><div class="grid">%s</div>""" % (columns, cell_px, cell_px, divs)

    html_path = os.path.join(PNG_DIR, "sheet.html")
    png_path = os.path.join(PNG_DIR, "sheet.png")
    with open(html_path, "w", encoding="utf-8") as handle:
        handle.write(html)

    command = [
        CHROME, "--headless=new", "--no-sandbox", "--disable-gpu",
        "--hide-scrollbars", "--default-background-color=00000000",
        "--force-device-scale-factor=1",
        "--virtual-time-budget=4000",
        "--window-size=%d,%d" % (width, height),
        "--screenshot=" + png_path,
        "file://" + html_path,
    ]
    result = subprocess.run(command, capture_output=True, text=True)
    if not os.path.exists(png_path):
        raise SystemExit("Chromium 没出图：\n%s\n%s" % (result.stdout, result.stderr))
    return png_path, columns, rows


def main():
    force = "--force-fetch" in sys.argv
    preview = "--preview" in sys.argv
    os.makedirs(PNG_DIR, exist_ok=True)

    print("1) 取图标")
    missing = []
    for name in SIZES:
        before = os.path.exists(os.path.join(SVG_DIR, name + ".svg"))
        fetch(name, force=force)
        if not before:
            missing.append(name)
    print("   新下载 %d 个，共 %d 个图标" % (len(missing), len(SIZES)))

    cells = [(name, px) for name in sorted(SIZES) for px in SIZES[name]]
    cell_px = max(max(sizes) for sizes in SIZES.values()) * SUPERSAMPLE
    print("2) 渲染 %d 个（网格 %dpx/格）" % (len(cells), cell_px))
    sheet, columns, _rows = render(cells, cell_px)

    print("3) 降采样并烘成 A8")
    from PIL import Image

    image = Image.open(sheet).convert("RGBA")
    baked = {}
    for index, (name, px) in enumerate(cells):
        column = index % columns
        row = index // columns
        size = px * SUPERSAMPLE
        left = column * cell_px + (cell_px - size) // 2
        top = row * cell_px + (cell_px - size) // 2
        tile = image.crop((left, top, left + size, top + size))
        tile = tile.resize((px, px), Image.LANCZOS)
        alpha = tile.split()[3]  # 只用 alpha：颜色运行时按主题染
        if preview:
            tile.save(os.path.join(PNG_DIR, "%s-%d.png" % (name, px)))
        baked[(name, px)] = alpha

    write_assets(baked)
    print("   写出 %s 与 %s" % (os.path.relpath(OUT_C, ROOT), os.path.relpath(OUT_H, ROOT)))
    print("   共 %d 个资源，%.1f KB 数据" % (
        len(baked), sum(len(v.tobytes()) for v in baked.values()) / 1024.0))


def write_assets(baked):
    names = sorted(SIZES)
    enum_lines = []
    glyph_lines = []
    index_lines = []
    data_lines = []
    for name in names:
        enum_lines.append("    QZ_ICON_%s," % name.upper().replace("-", "_"))
    for name in names:
        for px in sorted(SIZES[name]):
            symbol = "icon_%s_%d" % (name.replace("-", "_"), px)
            data = baked[(name, px)].tobytes()
            data_lines.append("/* %s %dpx */\nstatic const uint8_t %s_map[] = {" % (name, px, symbol))
            for offset in range(0, len(data), 16):
                chunk = data[offset:offset + 16]
                data_lines.append("    " + " ".join("0x%02x," % byte for byte in chunk))
            data_lines.append("};\n")
            # 每个 (图标, 尺寸) 一份描述符：调用方把指针交给 LVGL，不能共用一份
            glyph_lines.append(
                "    { .header = { .magic = LV_IMAGE_HEADER_MAGIC,"
                " .cf = LV_COLOR_FORMAT_A8, .w = %d, .h = %d, .stride = %d },"
                " .data_size = %d, .data = %s_map }," % (px, px, px, px * px, symbol)
            )
            index_lines.append(
                "    { QZ_ICON_%s, %d, %s_index }," % (
                    name.upper().replace("-", "_"), px, symbol)
            )
            # 索引里只存名字，真正的下标用数组初始化里的位置
            index_lines[-1] = index_lines[-1].replace(
                "%s_index" % symbol, str(len(index_lines) - 1))

    with open(OUT_H, "w", encoding="utf-8") as handle:
        handle.write(HEADER_TEMPLATE % "\n".join(enum_lines))
    with open(OUT_C, "w", encoding="utf-8") as handle:
        handle.write(
            C_TEMPLATE % ("\n".join(data_lines), "\n".join(glyph_lines), "\n".join(index_lines))
        )


HEADER_TEMPLATE = '''/* Generated by tools/mk_icon_assets.py - do not edit by hand.
 * Tabler Icons (MIT) baked as LV_COLOR_FORMAT_A8 glyphs and tinted at runtime.
 */
#ifndef QZDESK_ICON_ASSETS_H
#define QZDESK_ICON_ASSETS_H

#include "lvgl/lvgl.h"

typedef enum {
%s
    QZ_ICON_COUNT,
} qz_icon_t;

/**
 * Glyph for `icon` at the baked size closest to `px` (never smaller than asked
 * for unless it is the largest baked size). Returns NULL for an unknown icon, so
 * callers can fall back to text.
 */
const lv_image_dsc_t *qz_icon(qz_icon_t icon, int px);

#endif
'''

C_TEMPLATE = '''/* Generated by tools/mk_icon_assets.py - do not edit by hand.
 * Tabler Icons (MIT), baked as A8 alpha masks: the colour comes from the theme
 * at runtime (see qz_icon_image in theme.c), so light and dark share one asset.
 */
#include "icon_assets.h"

%s
/* 每个 (图标, 尺寸) 一份描述符：LVGL 会长期持有这个指针，不能共用一份 */
static const lv_image_dsc_t qz_icon_glyphs[] = {
%s
};

static const struct {
    qz_icon_t icon;
    int px;
    unsigned glyph;
} qz_icon_index[] = {
%s
};

const lv_image_dsc_t *qz_icon(qz_icon_t icon, int px)
{
    int best = -1;
    size_t index;

    for (index = 0; index < sizeof(qz_icon_index) / sizeof(qz_icon_index[0]); index++) {
        const int candidate = qz_icon_index[index].px;
        if (qz_icon_index[index].icon != icon) continue;
        if (best < 0) {
            best = (int)index;
        } else if (candidate >= px && (qz_icon_index[best].px < px || candidate < qz_icon_index[best].px)) {
            best = (int)index; /* 不小于要求尺寸里最小的那一档 */
        } else if (qz_icon_index[best].px < px && candidate > qz_icon_index[best].px) {
            best = (int)index; /* 都比要求小：取最大的 */
        }
    }
    if (best < 0) return NULL;
    return &qz_icon_glyphs[qz_icon_index[best].glyph];
}
'''

if __name__ == "__main__":
    main()
