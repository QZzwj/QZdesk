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
3. box-downsample to the exact sizes derived from SIZES and keep the alpha channel;
4. rewrite `app/icon_assets.c` + `include/icon_assets.h` with one LV_COLOR_FORMAT_A8
   descriptor per (icon, size).

A8 + `image_recolor` is the point: one asset per size, tinted at runtime with the
active theme colour, so light and dark themes need no second copy of every icon.
Every size is rendered from the vector at its exact pixel size, so LVGL never has
to resample a glyph with its nearest-neighbour software transform. The glyphs are
also optically normalised (see OPTICAL_EXTENT) and drawn with a size-dependent
stroke weight (see stroke_width), so a chevron reads as heavy as a cloud and an
11px glyph stays legible on the 320×240 panel.

SIZES 里写的是**设计稿尺寸**（480×320 上的物理像素）。320×240 面板把设计稿按
2/3 缩放（见 include/scale.h），所以每个设计尺寸还会自动烘一份
`qz_scale_px()` 算出来的物理档位 —— 两块屏上取到的都是精确档位，一次缩放都不
触发。生成的 qz_icon() 会把"没有精确档位"的请求记进 qz_icon_size_misses()，
由 tools/check_layout.sh 钉死在 0：界面发虚的老毛病（11px 的图被 10px 和 13px
的调用来回拉伸）只要回来，检查就会红。
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

# 视觉重量归一。Tabler 的 24 单位画布里，多数图标的墨迹对角线约 25 单位，
# 但 chevron（7.25×13.25，对角线 15.1）和 x（13.25×13.25，18.7）天生小一圈：
# 单独看没问题，和齿轮、云并排时明显发虚。把墨迹对角线小于 MOST_EXTENT 的
# 图标等比放大到 OPTICAL_EXTENT，其余原样保留 —— 不做整体归一，免得把 Tabler
# 原本的构图改掉。
OPTICAL_EXTENT = 22.0
MIN_OPTICAL_EXTENT = 21.5
OPTICAL_MAX_SCALE = 1.45
OPTICAL_FRAME = 24.0  # 画布范围；放大后的外接盒必须留在里面，否则描边会被裁掉
MEASURE_PX = 96       # 用 96px 的一遍渲染量外接盒（按 viewBox 归一，与目标尺寸无关）
MEASURE_ALPHA = 32    # 外接盒阈值：低于它的都是抗锯齿羽化

# 线重曲线：先按**物理像素**定一条单调的粗细，再换算回 24 单位画布
# （stroke-width 是画布单位，缩放系数是 px/24）。
#
# 为什么小尺寸要给到 1.5px：1.25px 的线落在非整数像素上，抗锯齿会把它摊成两层
# 半灰的边，10~15px 的图标整体就是"发虚"的灰团（旧曲线在 17~23px 还掉到 1.04px，
# 比 11px 的还细）。1.5px 在 1x 下能保住一条实心线，又不至于把 12px 的齿轮糊死。
# 24px 往上沿用原来的比例：24px 2.0、48px 3.5、96px 5.0 物理像素。
def stroke_width(px):
    if px <= 16:
        physical = 1.5
    elif px <= 24:
        physical = 1.5 + 0.5 * (px - 16) / 8.0
    elif px <= 48:
        physical = 2.0 + 1.5 * (px - 24) / 24.0
    else:
        physical = 3.5 + 1.5 * (px - 48) / 48.0
    return physical * 24.0 / px

# 个别图标在不同版本的 Tabler 里改过名：按顺序试，第一个存在的就用
ALIASES = {
    "timer": ["timer", "alarm", "clock-play"],
    "alert-triangle": ["alert-triangle", "triangle-alert"],
    "info-circle": ["info-circle", "info-square"],
    "circle-check": ["circle-check", "circle-check"],
    "help-circle": ["help-circle", "circle-help"],
}

# name -> **设计稿**尺寸（写在代码里的那个数，480×320 上就是物理像素）。
#
# 手写清单，不从调用点自动提取：每个数字都是一份独立蒙版，凭感觉加尺寸会让
# 资源翻倍。下面的数字是照着 ui 代码里的真实调用点抄的（括号里就是那些调用
# 点），新增图标或改调用点尺寸时照着补，再跑 tools/check_layout.sh 确认
# qz_icon_size_misses() 还是 0 —— 漏了哪一档，检查会直接把尺寸打出来。
#
# 每个设计尺寸会自动展开成实际面板上的物理尺寸：480×320（1x）、320×240
# （2/3）、480×240（3/4）与 320×320（2/3）。320 上 15 设计像素就是 10 物理
# 像素，当初 10px 的请求落到 11px 的烘焙档上被 LVGL 最近邻缩了一下 —— 铃铛
# 就是这么变成一排麻点的。矮而宽的 480×240 需要独立的 3/4 档。
SIZES = {
    # —— 天气 ——（首页卡 46、等待态占位云 34、气象站详情 32；认不出的天气也用这一族）
    "sun": [15, 32, 34, 46],
    "cloud": [26, 32, 34, 38, 42, 46],
    "cloud-rain": [15, 32, 34, 46],
    "cloud-snow": [32, 34, 46],
    "cloud-storm": [32, 34, 46],
    "cloud-fog": [32, 34, 46],
    "help-circle": [26, 32, 34, 38, 42, 46],

    # —— 状态栏 / 统计卡 ——（状态栏 26、系统状态卡三张统计卡 15、设置行 18）
    "wifi": [16, 18, 20, 26],
    "battery-4": [15, 26],
    "battery-3": [15, 26],
    "battery-2": [15, 21, 26],
    "battery-1": [15, 26],
    "battery-off": [15, 26],
    "bolt": [15],

    # —— 导航 / 通用 ——（qz_icon_button() 和 station_icon_button() 的图标是
    # size / 2：页头返回键 44→22、刷新 34→17 / 40→20 / 48→24、关闭 40→20 / 28→14）
    "chevron-left": [15, 22, 30],
    "chevron-right": [18, 20],
    "x": [14, 15, 16, 20, 22, 26],
    "refresh": [15, 17, 20, 24],
    "plus": [15, 16, 20, 22, 26],
    "check": [14, 21],
    "trash": [15],
    "download": [20],
    "eye": [16, 18, 19],
    "eye-off": [18, 20],
    "settings": [16, 18, 19, 20, 21, 22],
    "file-text": [16, 18, 20],
    "keyboard": [16, 17, 20, 22, 24, 30, 34, 39, 47],
    "device-mobile": [18, 22, 24, 30, 34, 39, 47],
    "pencil": [20],
    "list": [18, 20, 22, 24],
    "photo": [16, 20, 22, 26],
    "power": [14, 21],
    "droplet": [14, 15],
    "arrow-up": [20, 22, 24, 30, 34, 39, 47],
    "arrow-down": [20],

    # —— 图标瓷砖 ——（应用页 22、主页「接下来」20、设置行 18/16、技能空态 32）
    "bell": [15, 18, 19, 20, 21, 22],
    "timer": [19, 20, 21, 22],
    "clock": [19, 20, 21, 22],
    "activity": [19, 20, 21, 22],
    "volume": [16, 19, 20, 21, 22],
    "sparkles": [15, 20, 21, 22, 23, 30, 32],
    "plug": [32],

    # —— 记忆页与设置/设备类页面 ——
    "database": [16, 20, 22, 26],

    # —— 暂时没有调用点：先各烘一档，真用起来再按调用点补尺寸 ——
    "cpu": [20],
    "chart-bar": [20],
    "device-desktop": [20],
    "tool": [20],
    "bulb": [20],
    "home": [18, 20],
    "folder": [20],
    "shield": [20, 21],
    "user": [20],
    "palette": [20],
    "rotate": [20],
    "info-circle": [20],
    "alert-triangle": [20],
    "circle-check": [20],
    "layout-grid": [20],
    "microphone": [20, 30, 34],
    "player-play": [20],
    "repeat": [20, 21],
    "map-pin": [20],
    "wind": [15, 20],
    "thermometer": [15, 20],
}

# 面板 / 设计稿的比例。320×240 是最小基准（见 include/scale.h 的 qz_scale_minnum
# 与 qz_scale_ratio），这里只跟它对齐，不入库其它比例。
DESIGN_W, DESIGN_H = 480, 320
PANELS = ((DESIGN_W, DESIGN_H), (320, 240), (480, 240), (320, 320))


def qz_scale_px(design_px, panel_w, panel_h):
    """与 scale.h 的 qz_scale_px() 逐位一致：四舍五入（正数即 +半格再整除）。"""
    num = min(panel_w * DESIGN_H, panel_h * DESIGN_W)
    den = DESIGN_W * DESIGN_H
    return (design_px * num + den // 2) // den


def baked_sizes(design_sizes):
    """一个图标要烘的全部物理档位：每块面板上的精确取值，去重排序。"""
    sizes = set()
    for design_px in design_sizes:
        for panel_w, panel_h in PANELS:
            sizes.add(qz_scale_px(design_px, panel_w, panel_h))
    return sorted(sizes)


# 真正落盘的 (图标, 物理像素) 列表：设计尺寸 × 面板比例展开后的结果。
GLYPHS = {name: baked_sizes(sizes) for name, sizes in SIZES.items()}


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


def svg_markup(name, px, scale=1.0):
    """Inline the SVG at `px` px with a white stroke: the alpha channel is what we keep.

    `scale` 是这个图标的视觉重量归一系数，绕画布中心等比放大墨迹；线宽同时
    除以 scale，让放大后的笔画粗细仍按上面的曲线走。
    """
    with open(os.path.join(SVG_DIR, name + ".svg"), "r", encoding="utf-8") as handle:
        text = handle.read()
    text = re.sub(r"<!--.*?-->", "", text, flags=re.S)
    text = text.replace("currentColor", "#ffffff")
    text = re.sub(
        r'stroke-width="[0-9.]+"',
        'stroke-width="%.3f"' % (stroke_width(px) / scale),
        text,
    )
    group = '<g transform="translate(12 12) scale(%.5f) translate(-12 -12)">' % scale
    text = re.sub(
        r"(<svg\b[^>]*>)",
        lambda match: match.group(1) + group,
        text,
        count=1,
    )
    text = text.replace("</svg>", "</g></svg>")
    # 覆盖根节点尺寸，viewBox 会让整幅图（含笔画）等比缩放。
    # 注意不能写成 \bwidth= —— 那也会命中 stroke-width（横杠是词边界）。
    text = re.sub(r'(?<![-\w])width="[^"]*"', 'width="%d"' % (px * SUPERSAMPLE), text, count=1)
    text = re.sub(r'(?<![-\w])height="[^"]*"', 'height="%d"' % (px * SUPERSAMPLE), text, count=1)
    return text


def render(cells, cell_px, scales=None, out="sheet"):
    """One Chromium pass: every icon centred in its own cell, transparent background."""
    columns = 8
    rows = (len(cells) + columns - 1) // columns
    width = columns * cell_px
    height = rows * cell_px
    divs = "".join(
        '<div class="cell">%s</div>' % svg_markup(
            name, px, (scales or {}).get(name, 1.0))
        for name, px in cells
    )
    html = """<!doctype html><meta charset="utf-8"><style>
html,body{margin:0;padding:0;background:transparent}
.grid{display:grid;grid-template-columns:repeat(%d,%dpx);grid-auto-rows:%dpx}
.cell{display:flex;align-items:center;justify-content:center;overflow:hidden}
</style><div class="grid">%s</div>""" % (columns, cell_px, cell_px, divs)

    html_path = os.path.join(PNG_DIR, out + ".html")
    png_path = os.path.join(PNG_DIR, out + ".png")
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


def ink_bbox(image, columns, cell_px, index):
    """按 alpha 阈值量一个格子里墨迹的外接盒，返回值是 viewBox 单位。"""
    column = index % columns
    row = index // columns
    tile = image.crop((
        column * cell_px, row * cell_px,
        column * cell_px + cell_px, row * cell_px + cell_px,
    )).split()[3]
    box = tile.point(lambda value: 255 if value >= MEASURE_ALPHA else 0).getbbox()
    if not box:
        return 0.0, 0.0, 12.0, 12.0
    unit = 24.0 / cell_px
    return (
        (box[2] - box[0]) * unit,
        (box[3] - box[1]) * unit,
        (box[0] + box[2]) / 2.0 * unit,
        (box[1] + box[3]) / 2.0 * unit,
    )


def main():
    force = "--force-fetch" in sys.argv
    preview = "--preview" in sys.argv
    os.makedirs(PNG_DIR, exist_ok=True)
    from PIL import Image

    print("1) 取图标")
    missing = []
    for name in GLYPHS:
        before = os.path.exists(os.path.join(SVG_DIR, name + ".svg"))
        fetch(name, force=force)
        if not before:
            missing.append(name)
    print("   新下载 %d 个，共 %d 个图标" % (len(missing), len(GLYPHS)))

    names = sorted(GLYPHS)
    measure_cell = MEASURE_PX * SUPERSAMPLE
    measure_sheet, columns, _rows = render(
        [(name, MEASURE_PX) for name in names], measure_cell, out="measure")
    measure_image = Image.open(measure_sheet).convert("RGBA")

    print("2) 量墨迹外接盒，算视觉重量归一")
    scales = {}
    for index, name in enumerate(names):
        ink_w, ink_h, center_x, center_y = ink_bbox(
            measure_image, columns, measure_cell, index)
        # 用对角线而不是长边：细长的 chevron 该放大，近乎方形的 x 不该被拉爆
        extent = (ink_w * ink_w + ink_h * ink_h) ** 0.5
        if extent <= 0 or extent >= MIN_OPTICAL_EXTENT:
            scale = 1.0
        else:
            scale = OPTICAL_EXTENT / extent
        # 放大后外接盒不能顶出画布，否则描边会被 viewBox 裁掉
        frame = OPTICAL_FRAME
        allowed = OPTICAL_MAX_SCALE
        for half_ink, offset in ((ink_w / 2.0, center_x - 12.0),
                                 (ink_h / 2.0, center_y - 12.0)):
            if half_ink > 0:
                allowed = min(allowed, frame / 2.0 / (half_ink + abs(offset)))
        scales[name] = 1.0 if scale <= 1.0 else max(1.0, min(scale, allowed))
    scaled = [(name, scales[name]) for name in names if scales[name] > 1.01]
    print("   %d 个图标放大：%s" % (
        len(scaled), "  ".join("%s %.2f" % item for item in scaled)))

    cells = [(name, px) for name in names for px in GLYPHS[name]]
    cell_px = max(max(sizes) for sizes in GLYPHS.values()) * SUPERSAMPLE
    print("3) 渲染 %d 个（网格 %dpx/格）" % (len(cells), cell_px))
    sheet, columns, _rows = render(cells, cell_px, scales=scales)

    print("4) 降采样并烘成 A8")
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
    names = sorted(GLYPHS)
    enum_lines = []
    glyph_lines = []
    index_lines = []
    data_lines = []
    for name in names:
        enum_lines.append("    QZ_ICON_%s," % name.upper().replace("-", "_"))
    for name in names:
        for px in GLYPHS[name]:
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
 * Glyph for `icon` at the baked size closest to `px`, preferring the larger
 * entry on a tie. Callers get a glyph within a pixel or two of the request and
 * draw it 1:1, so LVGL never has to resample it. Returns NULL for an unknown
 * icon, so callers can fall back to text.
 */
const lv_image_dsc_t *qz_icon(qz_icon_t icon, int px);

/**
 * How many qz_icon() calls missed an exact baked size so far. Non-zero means
 * some screen asked for a size nobody baked: LVGL would then soften the glyph
 * with its nearest-neighbour transform. tools/check_layout.c asserts this
 * stays 0, so a new call site with an unknown size fails the layout check.
 */
int qz_icon_size_misses(void);

#endif
'''

C_TEMPLATE = '''/* Generated by tools/mk_icon_assets.py - do not edit by hand.
 * Tabler Icons (MIT), baked as A8 alpha masks: the colour comes from the theme
 * at runtime (see qz_icon_image in theme.c), so light and dark share one asset.
 */
#include "icon_assets.h"
#include <stdio.h>
#include <stdlib.h>

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

/* 没有精确档位的取图次数。见 qz_icon_size_misses()。 */
static int qz_icon_miss_count;

const lv_image_dsc_t *qz_icon(qz_icon_t icon, int px)
{
    int best = -1;
    size_t index;

    for (index = 0; index < sizeof(qz_icon_index) / sizeof(qz_icon_index[0]); index++) {
        const int candidate = qz_icon_index[index].px;
        if (qz_icon_index[index].icon != icon) continue;
        if (best < 0 || abs(candidate - px) < abs(qz_icon_index[best].px - px)) {
            best = (int)index;
            continue;
        }
        if (candidate - px == qz_icon_index[best].px - px &&
            candidate > qz_icon_index[best].px) {
            best = (int)index; /* 距离相同取大档：宁可略大，也不要把笔画压虚 */
        }
    }
    if (best < 0) return NULL;
    if (qz_icon_index[best].px != px) {
        qz_icon_miss_count++;
        if (getenv("QZDESK_ICON_DIAG")) {
            fprintf(stderr, "ICON_MISS icon=%%d requested=%%d used=%%d\\n",
                    (int)icon, px, qz_icon_index[best].px);
        }
    }
    return &qz_icon_glyphs[qz_icon_index[best].glyph];
}

int qz_icon_size_misses(void)
{
    return qz_icon_miss_count;
}
'''

if __name__ == "__main__":
    main()
