#!/usr/bin/env python3
"""Bake the mascot expression PNGs into LVGL RGB565A8 C arrays.

Usage (from the repository root):

    python3 QZdesk-Demo/tools/mk_mascot_assets.py [--preview]

Per expression in QZdesk-Demo/assets/mascot/ it runs:

1. decode with ffmpeg and upscale 4x (bilinear). Working at 4x is what makes the
   cut edge smooth after the final downsample.
2. flood fill the flat studio background from the borders with a *soft*
   threshold: pixels close to the backdrop colour fade out, pixels further away
   stay opaque, so the alpha ramp is a few source pixels wide instead of a hard
   0/255 step.
3. un-multiply the backdrop colour out of the semi transparent pixels, otherwise
   the white studio background leaves a pale fringe once the mascot is drawn on
   the blue card.
4. centre the subject on a square canvas and box-downsample with premultiplied
   alpha to the baked size.
5. rewrite QZdesk-Demo/app/mascot_assets.c + include/mascot_assets.h.

Assets are named after the assistant state they are shown for:
idle.png, happy.png, thinking.png, confused.png, speaking.png, blink.png.
Sources that already carry an alpha channel are used as they are; only opaque
sources go through the background removal below.

`blink` is not a state: it is the closed-eye frame the face swaps in for a
fraction of a second while idling (see ai_face.c). It is framed like the other
busts so the swap does not resize the character.

blink.png was taken from the reference video doubao_video_834x1112 (the closed-eye
frame at t=0.68s, cropped to `crop=834:630:0:50`), so it is the only source that
arrives without an alpha channel. It goes through `grabcut_matte()` below; the
hand-made renders keep their own alpha and skip the whole step.
"""
import os
import subprocess
import sys
from collections import deque

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC_DIR = os.path.join(ROOT, "assets", "mascot")
OUT_C = os.path.join(ROOT, "app", "mascot_assets.c")
OUT_H = os.path.join(ROOT, "include", "mascot_assets.h")
PREVIEW_DIR = os.path.join(HERE, "preview")
STATES = ["idle", "happy", "thinking", "confused", "speaking", "love", "blink"]

SUPERSAMPLE = 4      # working resolution multiplier for small sources
SUPERSAMPLE_MIN_SRC = 800  # sources bigger than this are already crisp enough
OUT_SIZE = 192       # baked square size (displayed up to 164px)
# The sources frame the character differently (some include the whole body, the
# one with the heart is a tighter bust), so after the content box normalisation
# each state gets a small manual correction: zoom multiplies the canvas (bigger
# canvas = smaller character), dy shifts the canvas centre down in percent.
FRAMING = {
    # idle 的新原图是**全身**站立（内容框 1626×2006，头带耳朵 1616 宽、约 1010 高）。
    # 按内容框归一化会让头只有别人的一半大（切表情"忽大忽小"），而按"头占画布 88%"
    # 收窗又会把耳朵横向切掉 —— 正确的判据是**屏幕上头的实际大小与各胸像一致**：
    # 五个胸像的头宽平均占画布 79.7%，据此反解出 zoom 0.952（画布 2029px）、
    # dy -19.5% 让头的中心落在画布 45% 高处，身体仍可见一部分。
    "idle": (0.952, -19.5),
    "happy": (1.30, 0.0),
    "love": (1.30, 0.0),   # 与 happy 同一张原图取景（抱心），跟着一起对齐
    "thinking": (1.06, 0.0),
    "confused": (1.06, 0.0),
    "speaking": (1.20, 0.0),
    # blink 来自参考视频（doubao_video_834x1112 的闭眼帧）：那只角色的耳朵比
    # 其它几张渲染更长更外张，按内容框归一化后会比 idle 的头大一圈。1.00 的
    # 缩放让脸尽量贴身、耳朵仍在画幅内；-13% 是把它对齐到 idle 的鼻子高度
    # （两张的 nose row 因此都是 126/192），眨眼时才不会看到画面往上跳。
    "blink": (1.00, -13.0),
}
KEY_TIGHT = 16       # colour distance that still counts as backdrop
FEATHER_PX = 6       # width of the soft edge at the 4x working resolution
MODEL_MARGIN_MIN = 5  # smallest border band used to read the backdrop colour
KEY_GROW = 7         # 与相邻背景像素的最大差：平缓的曲面渐变靠它翻过去
# 扩散时与背景模型的最大距离。这个上限是"渐变"和"角色"的分界线：
# 背景自身的曲面渐变离线性模型最多差 20 上下，而角色的白毛要比背景亮
# 30~50，所以 24 既够翻过渐变，又拦得住顺着柔和边爬进毛里。
KEY_GROW_MAX = 24
BLUR_RADIUS = 2      # 分类前先做的盒式平滑半径（压掉视频压缩噪点）
SHADOW_MIN_RATIO = 0.45  # 判定"同色被压暗"的最暗比例
SHADOW_CHROMA = 0.12     # 各通道比例的最大离散度：超过就不是同色，而是别的物体


def probe_size(path):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=width,height", "-of", "csv=p=0:s=x", path],
        capture_output=True, text=True).stdout.strip()
    w, h = out.split("x")
    return int(w), int(h)


def load_rgba(path, scale):
    w, h = probe_size(path)
    cmd = ["ffmpeg", "-loglevel", "error", "-i", path]
    if scale > 1:
        cmd += ["-vf", f"scale=iw*{scale}:ih*{scale}:flags=bilinear"]
    cmd += ["-f", "rawvideo", "-pix_fmt", "rgba", "-"]
    raw = subprocess.run(cmd, capture_output=True).stdout
    return w * scale, h * scale, bytearray(raw)


def box_blur_rgb(w, h, px, radius):
    """Return a smoothed RGB copy (3 bytes per pixel), used only for classification.

    视频帧带 H.264 压缩噪点，相邻像素能差 5~10：按"相邻几乎一样"扩散会卡在
    噪点上，而把阈值放大到能压住噪点，就会开始吃角色的羽化边。先把颜色平滑
    一遍，两个问题一起解决 —— 噪点没了，阈值仍旧可以很紧。

    输出的颜色仍然取自原图，平滑只影响"这里算不算背景"的判断。
    """
    horizontal = bytearray(w * h * 3)
    for y in range(h):
        base = y * w
        for x in range(w):
            x0 = x - radius if x >= radius else 0
            x1 = x + radius + 1 if x + radius < w else w
            span = x1 - x0
            r = g = b = 0
            for xx in range(x0, x1):
                i = (base + xx) * 4
                r += px[i]
                g += px[i + 1]
                b += px[i + 2]
            o = (base + x) * 3
            horizontal[o] = r // span
            horizontal[o + 1] = g // span
            horizontal[o + 2] = b // span

    flat = bytearray(w * h * 3)
    for y in range(h):
        y0 = y - radius if y >= radius else 0
        y1 = y + radius + 1 if y + radius < h else h
        span = y1 - y0
        base = y * w
        for x in range(w):
            r = g = b = 0
            for yy in range(y0, y1):
                o = (yy * w + x) * 3
                r += horizontal[o]
                g += horizontal[o + 1]
                b += horizontal[o + 2]
            o = (base + x) * 3
            flat[o] = r // span
            flat[o + 1] = g // span
            flat[o + 2] = b // span
    return flat


def row_backdrop_model(w, h, flat):
    """逐行估计背景色：左右边缘带取中位数，行内线性插值。

    视频里的背景是渐变（上浅下深、中间亮边缘暗），一个全局颜色抠不干净；
    但它在水平方向平缓，逐行插值足够准。取中位数而不是均值，是因为边缘
    偶尔会混进别的东西（柔影、噪点）。返回每行的 (左色, 右色)。
    """
    margin = max(MODEL_MARGIN_MIN, w // 70)
    rows = []
    for y in range(h):
        base = y * w
        sides = []
        for x0 in (0, w - margin):
            values = ([], [], [])
            for x in range(x0, x0 + margin):
                o = (base + x) * 3
                for channel in range(3):
                    values[channel].append(flat[o + channel])
            sides.append(tuple(sorted(v)[len(v) // 2] if v else 200 for v in values))
        rows.append(sides)
    return rows


def grabcut_matte(w, h, px, rows):
    """GrabCut 抠图，返回每像素 0/255 的 bytearray；cv2 不可用时返回 None。

    视频帧的背景是渐变加一圈环境光遮蔽，纯颜色阈值只能在"留下脏边"和
    "顺着柔和边界吃进白毛"之间来回挪 —— 阈值再怎么调也只是挪边界。GrabCut
    用颜色统计 + 连通性求割，两个问题一起解决。种子来自上面的背景模型：
    贴着模型的算"大概率背景"，离得远的算"大概率角色"。
    """
    try:
        import numpy as np
        import cv2
    except Exception:
        return None

    rgb = np.frombuffer(bytes(px), np.uint8).reshape(h, w, 4)[:, :, :3].astype(np.float32)
    left = np.array([row[0] for row in rows], np.float32)
    right = np.array([row[1] for row in rows], np.float32)
    ramp = np.linspace(0.0, 1.0, w, dtype=np.float32)[None, :, None]
    model = left[:, None, :] * (1.0 - ramp) + right[:, None, :] * ramp

    # 先验只有两句话：贴边且贴着背景模型的算背景，其余（画面内部）算角色。
    # 反过来标会把角色的毛色喂给背景模型，头会被从中间切开。
    # 这里刻意不按"同色压暗"去标背景：角色下巴压着脖子，那圈阴影同样满足
    # "三通道比例一致"，标下去会把脖子判成背景、身体被割成独立一块。
    diff = np.abs(rgb - model).max(axis=2)
    mask = np.full((h, w), cv2.GC_PR_FGD, np.uint8)
    mask[diff <= KEY_TIGHT] = cv2.GC_PR_BGD
    mask[:2, :] = cv2.GC_BGD
    mask[-2:, :] = cv2.GC_BGD
    mask[:, :2] = cv2.GC_BGD
    mask[:, -2:] = cv2.GC_BGD

    bgr = np.ascontiguousarray(rgb[:, :, ::-1]).astype(np.uint8)
    background_model = np.zeros((1, 65), np.float64)
    foreground_model = np.zeros((1, 65), np.float64)
    cv2.grabCut(bgr, mask, None, background_model, foreground_model, 5,
                cv2.GC_INIT_WITH_MASK)

    foreground = np.where((mask == cv2.GC_FGD) | (mask == cv2.GC_PR_FGD), 255, 0).astype(np.uint8)
    foreground = cv2.morphologyEx(foreground, cv2.MORPH_CLOSE, np.ones((3, 3), np.uint8))
    # 只留最大的一块：零散的小斑点会被羽化放大成一圈脏点
    count, labels, stats, _ = cv2.connectedComponentsWithStats(foreground, 8)
    if count > 2:
        keep = 1 + int(np.argmax(stats[1:, cv2.CC_STAT_AREA]))
        foreground = np.where(labels == keep, 255, 0).astype(np.uint8)
    return bytearray(foreground.reshape(-1).tobytes())


def key_background(w, h, px):
    """Remove the backdrop and feather the silhouette.

    The backdrop is modelled per row from the left/right border bands instead of
    being treated as one flat colour: renders taken from a video carry a graded
    backdrop (lighter above, darker at the edges), and a single global colour
    would either leave a visible halo where the grade drifts or start eating the
    cream fur where it happens to match.

    The fill itself still uses a *tight* colour threshold against that local
    model, so it cannot leak into the fur; the soft edge comes from feathered
    alpha around the subject rather than from widening the threshold.

    Returns the number of pixels that were made (partly) transparent.
    """

    def sample(x, y):
        i = (y * w + x) * 4
        return px[i], px[i + 1], px[i + 2], px[i + 3]

    corners = [sample(0, 0), sample(w - 1, 0), sample(0, h - 1), sample(w - 1, h - 1)]
    if all(c[3] < 8 for c in corners):
        return 0  # already keyed (alpha channel present)

    flat = box_blur_rgb(w, h, px, BLUR_RADIUS)

    def flat_at(x, y):
        o = (y * w + x) * 3
        return flat[o], flat[o + 1], flat[o + 2]

    rows = row_backdrop_model(w, h, flat)

    def backdrop_at(x, y):
        left, right = rows[y]
        t = x / (w - 1) if w > 1 else 0.0
        return tuple(int(left[c] + (right[c] - left[c]) * t + 0.5) for c in range(3))

    def is_backdrop(x, y, near=None):
        if sample(x, y)[3] < 8:
            return True
        r, g, b = flat_at(x, y)
        mr, mg, mb = backdrop_at(x, y)
        if (abs(r - mr) <= KEY_TIGHT and abs(g - mg) <= KEY_TIGHT
                and abs(b - mb) <= KEY_TIGHT):
            return True
        # 影棚里角色周围有一圈环境光遮蔽，脚下还有一道接地投影。它们不是
        # "另一种颜色"，而是同一块背景被压暗：三个通道与背景模型成同一个
        # 比例。按比例判断既能清掉这圈柔影（否则成品会带一圈脏边），
        # 又不会误吃角色 —— 毛色与背景色相不同，比例必然散得很开。
        ratios = []
        for value, model in ((r, mr), (g, mg), (b, mb)):
            if model <= 0:
                ratios = []
                break
            ratios.append(value / model)
        if ratios:
            lo, hi = min(ratios), max(ratios)
            if lo >= SHADOW_MIN_RATIO and hi <= 1.02 and (hi - lo) <= SHADOW_CHROMA:
                return True
        # 背景是曲面渐变（中间亮、边缘暗），线性插值在中间会偏一个十几二十，
        # 单靠上面那条判定会让填充停在半路；允许沿"相邻几乎一样"继续走，
        # 但整体仍要贴着背景模型，防止顺着角色的羽化边爬进去。
        if near is None:
            return False
        nr, ng, nb = near
        if max(abs(r - nr), abs(g - ng), abs(b - nb)) > KEY_GROW:
            return False
        return max(abs(r - mr), abs(g - mg), abs(b - mb)) <= KEY_GROW_MAX

    matte = grabcut_matte(w, h, px, rows)
    if matte is not None:
        print("  (GrabCut)")
        outside = bytearray(0 if value else 1 for value in matte)
    else:
        print("  (colour key)")
        outside = bytearray(w * h)
        queue = deque()

        def push(x, y, near):
            idx = y * w + x
            if outside[idx] or not is_backdrop(x, y, near):
                return
            outside[idx] = 1
            queue.append((x, y))

        for x in range(w):
            push(x, 0, None)
            push(x, h - 1, None)
        for y in range(h):
            push(0, y, None)
            push(w - 1, y, None)
        while queue:
            x, y = queue.popleft()
            here = flat_at(x, y)
            if x > 0:
                push(x - 1, y, here)
            if x + 1 < w:
                push(x + 1, y, here)
            if y > 0:
                push(x, y - 1, here)
            if y + 1 < h:
                push(x, y + 1, here)

    # Distance from every backdrop pixel to the subject, capped at FEATHER_PX.
    dist = bytearray(w * h)
    frontier = deque()
    for idx in range(w * h):
        if outside[idx]:
            dist[idx] = FEATHER_PX
        else:
            frontier.append(idx)
    while frontier:
        idx = frontier.popleft()
        d = dist[idx] + 1
        if d > FEATHER_PX:
            continue
        x = idx % w
        y = idx // w
        for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
            if nx < 0 or ny < 0 or nx >= w or ny >= h:
                continue
            nidx = ny * w + nx
            if dist[nidx] > d:
                dist[nidx] = d
                frontier.append(nidx)

    touched = 0
    for idx in range(w * h):
        if not outside[idx]:
            continue
        d = dist[idx]
        a = 0 if d >= FEATHER_PX else (FEATHER_PX - d) * 255 // FEATHER_PX
        i = idx * 4
        if a < 255:
            touched += 1
            if a > 0:
                # Un-multiply the backdrop: c = (c - bg * (1 - a)) / a, so no
                # pale fringe is left when drawn on a coloured background.
                # 用的是这一行这一列插值出来的本地背景色，不是某个全局平均色。
                inv = 255 - a
                model = backdrop_at(idx % w, idx // w)
                for c_i in range(3):
                    v = (px[i + c_i] * 255 - model[c_i] * inv) // a
                    px[i + c_i] = 0 if v < 0 else (255 if v > 255 else v)
        px[i + 3] = a
    return touched


def content_box(w, h, px):
    x0, y0, x1, y1 = w, h, -1, -1
    for y in range(h):
        base = y * w
        for x in range(w):
            if px[(base + x) * 4 + 3] > 6:
                if x < x0:
                    x0 = x
                if x > x1:
                    x1 = x
                if y < y0:
                    y0 = y
                if y > y1:
                    y1 = y
    if x1 < 0:
        return 0, 0, w - 1, h - 1
    return x0, y0, x1, y1


def downsample_square(w, h, px, box, size, framing=(1.0, 0.0)):
    """Fit `box` on a square canvas (plus per state framing) and box-filter it."""
    x0, y0, x1, y1 = box
    bw = x1 - x0 + 1
    bh = y1 - y0 + 1
    zoom, dy_pct = framing
    side = max(bw, bh) * 100.0 / 94 * zoom
    ox = x0 + bw / 2.0 - side / 2.0
    oy = y0 + bh / 2.0 - side / 2.0 + side * dy_pct / 100.0
    step = side / size

    color = bytearray(size * size * 2)
    alpha = bytearray(size * size)
    for row in range(size):
        sy0 = int(oy + row * step)
        sy1 = max(sy0 + 1, int(oy + (row + 1) * step))
        for col in range(size):
            sx0 = int(ox + col * step)
            sx1 = max(sx0 + 1, int(ox + (col + 1) * step))
            ar = ag = ab = aa = 0
            n = 0
            for y in range(sy0, sy1):
                if y < 0 or y >= h:
                    n += sx1 - sx0
                    continue
                base = y * w
                for x in range(sx0, sx1):
                    n += 1
                    if x < 0 or x >= w:
                        continue
                    i = (base + x) * 4
                    a = px[i + 3]
                    ar += px[i] * a
                    ag += px[i + 1] * a
                    ab += px[i + 2] * a
                    aa += a
            a = aa // n if n else 0
            if aa:
                r = ar // aa
                g = ag // aa
                b = ab // aa
            else:
                r = g = b = 0
            o = row * size + col
            value = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            color[o * 2] = value & 0xFF
            color[o * 2 + 1] = (value >> 8) & 0xFF
            alpha[o] = a
    return color, alpha


def write_preview(name, size, color, alpha, backdrop):
    os.makedirs(PREVIEW_DIR, exist_ok=True)
    px = bytearray(size * size * 4)
    for i in range(size * size):
        value = color[i * 2] | (color[i * 2 + 1] << 8)
        r = (value >> 11) & 0x1F
        g = (value >> 5) & 0x3F
        b = value & 0x1F
        px[i * 4] = (r * 255 + 15) // 31
        px[i * 4 + 1] = (g * 255 + 31) // 63
        px[i * 4 + 2] = (b * 255 + 15) // 31
        px[i * 4 + 3] = alpha[i]
    raw = os.path.join(PREVIEW_DIR, f"{name}_{backdrop}.rgba")
    with open(raw, "wb") as fh:
        fh.write(px)
    colour = {"grey": (214, 216, 222), "blue": (76, 104, 230),
              "white": (255, 255, 255)}[backdrop]
    geq = f"geq=r={colour[0]}:g={colour[1]}:b={colour[2]}:a=255"
    subprocess.run(
        ["ffmpeg", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgba",
         "-s", f"{size}x{size}", "-i", raw, "-vf",
         f"format=rgba,split[a][b];[b]{geq}[bg];[bg][a]overlay",
         "-y", os.path.join(PREVIEW_DIR, f"{name}_{backdrop}.png")], check=False)


def main():
    entries = []
    for state in STATES:
        path = os.path.join(SRC_DIR, state + ".png")
        src_w, src_h = probe_size(path)
        supersample = 1 if max(src_w, src_h) >= SUPERSAMPLE_MIN_SRC else SUPERSAMPLE
        w, h, px = load_rgba(path, supersample)
        touched = key_background(w, h, px)
        box = content_box(w, h, px)
        framing = FRAMING.get(state, (1.0, 0.0))
        color, alpha = downsample_square(w, h, px, box, OUT_SIZE, framing)
        entries.append((state, color, alpha))
        if "--preview" in sys.argv:
            for backdrop in ("grey", "blue", "white"):
                write_preview(state, OUT_SIZE, color, alpha, backdrop)
        print(f"{state:9s} {src_w}x{src_h} box={box} framing={framing}")

    with open(OUT_C, "w", encoding="utf-8") as fh:
        fh.write("/* Generated by tools/mk_mascot_assets.py - do not edit by hand.\n"
                 " * LVGL RGB565A8 mascot faces: colour plane then alpha plane.\n"
                 " */\n")
        fh.write('#include "mascot_assets.h"\n\n')
        for state, color, alpha in entries:
            blob = bytes(color) + bytes(alpha)
            fh.write(f"static const uint8_t mascot_{state}_map[] = {{\n")
            for i in range(0, len(blob), 16):
                chunk = blob[i:i + 16]
                fh.write("    " + " ".join(f"0x{b:02x}," for b in chunk) + "\n")
            fh.write("};\n\n")
            fh.write(f"const lv_image_dsc_t qz_mascot_{state} = {{\n"
                     f"    .header.cf = LV_COLOR_FORMAT_RGB565A8,\n"
                     f"    .header.w = {OUT_SIZE},\n"
                     f"    .header.h = {OUT_SIZE},\n"
                     f"    .data_size = {len(blob)},\n"
                     f"    .data = mascot_{state}_map,\n"
                     f"}};\n\n")

    with open(OUT_H, "w", encoding="utf-8") as fh:
        fh.write("/* Generated by tools/mk_mascot_assets.py - do not edit by hand. */\n"
                 "#ifndef QZDESK_MASCOT_ASSETS_H\n#define QZDESK_MASCOT_ASSETS_H\n\n"
                 '#include "lvgl/lvgl.h"\n\n'
                 "/* Square size of every baked mascot image. */\n"
                 f"#define QZ_MASCOT_SIZE {OUT_SIZE}\n\n")
        for state, _, _ in entries:
            fh.write(f"extern const lv_image_dsc_t qz_mascot_{state};\n")
        fh.write("\n#endif\n")
    print("wrote", OUT_C, "and", OUT_H)


if __name__ == "__main__":
    main()
