#!/usr/bin/env python3
"""从一个镜像/分区文件里找出所有内嵌的图片，并输出预览。

为什么需要它：开机 logo 存在镜像里时并不总是 BMP。Rockchip 的链路里可能是
BMP（resource.img 里的 logo.bmp / logo_kernel.bmp），也可能是裸像素
（.rgb/.rgba/.bgra，ffmpeg 之类工具转出来的那类），还可能在 u-boot 里。
与其猜格式，不如把文件里所有"像图片"的东西都挖出来看一眼。

用法：
    tools/extract-images.py <镜像文件> [输出目录] [--panel WxH]

    --raw             再把"裸像素"也猜一遍（BMP/PNG/GIF/JPEG 之外的
                      .rgb/.rgba 那类；误报多，默认不开）
    --panel 320x240   配合 --raw 使用的面板分辨率

输出：
    控制台列出每张图的偏移、尺寸、格式；
    输出目录里是抠出来的原始文件；
    如果装了 Pillow，再生成一张 preview.png 便于直接看。
"""

import os
import struct
import sys

MAGICS = [
    (b"\x89PNG\r\n\x1a\n", "PNG"),
    (b"GIF87a", "GIF"),
    (b"GIF89a", "GIF"),
    (b"\xff\xd8\xff", "JPEG"),
]

DEFAULT_PANELS = [(320, 240), (480, 320), (720, 720)]


def find_bmps(data):
    found = []
    i = 0
    while True:
        i = data.find(b"BM", i + 1)
        if i < 0 or i + 30 >= len(data):
            break
        (size,) = struct.unpack_from("<I", data, i + 2)
        (off,) = struct.unpack_from("<I", data, i + 10)
        (header,) = struct.unpack_from("<I", data, i + 14)
        if header not in (12, 40, 108, 124):
            continue
        w, h = struct.unpack_from("<ii", data, i + 18)
        (bpp,) = struct.unpack_from("<H", data, i + 28)
        if not (8 <= w <= 8192 and 8 <= abs(h) <= 8192 and bpp in (1, 4, 8, 16, 24, 32)):
            continue
        if not (30 < size <= 30 * 1024 * 1024) or i + size > len(data):
            continue
        found.append((i, size, "BMP %dx%d %dbpp" % (w, abs(h), bpp)))
    return found


def find_magics(data):
    found = []
    for magic, name in MAGICS:
        start = 0
        while True:
            i = data.find(magic, start)
            if i < 0:
                break
            found.append((i, name))
            start = i + 1
    return sorted(found)


def guess_raw(data, panels):
    """裸像素猜不出来内容，只能按"长度刚好等于 宽×高×字节"来认。"""
    found = []
    for w, h in panels:
        for bpp, label in ((2, "RGB565"), (3, "RGB888"), (4, "RGBA8888")):
            size = w * h * bpp
            if size > len(data):
                continue
            for off in (0, len(data) - size):
                found.append((off, size, "裸像素 %s %dx%d（%d B）" % (label, w, h, size)))
    return found


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    outdir = sys.argv[2] if len(sys.argv) > 2 and not sys.argv[2].startswith("-") else "images_out"
    panels = list(DEFAULT_PANELS)
    if "--panel" in sys.argv:
        spec = sys.argv[sys.argv.index("--panel") + 1]
        w, h = spec.lower().split("x")
        panels = [(int(w), int(h))] + panels

    data = open(path, "rb").read()
    print("文件 %s：%.2f MB" % (path, len(data) / 1048576))

    os.makedirs(outdir, exist_ok=True)
    items = []
    for off, size, label in find_bmps(data):
        name = "%08d_%s.bmp" % (off, label.replace(" ", "_").replace("/", "-"))
        dest = os.path.join(outdir, name)
        open(dest, "wb").write(data[off:off + size])
        items.append((off, size, label, dest))
    for off, label in find_magics(data):
        size = 0
        dest = os.path.join(outdir, "%08d.%s" % (off, label.lower()))
        open(dest, "wb").write(data[off:off + 4096])
        items.append((off, size, label + "（头部已存，需再解析长度）", dest))
    if "--raw" in sys.argv:
        # 裸像素没有任何头，只能按"长度刚好等于 宽×高×字节"猜，误报很多，
        # 所以默认不跑；确认要赌一把时再加 --raw。
        for off, size, label in guess_raw(data, panels):
            dest = os.path.join(outdir, "%08d_%s.bin" % (off, label.replace(" ", "_")))
            open(dest, "wb").write(data[off:off + size])
            items.append((off, size, label, dest))

    if not items:
        print("没找到图片。可能这个分区里确实没有，或格式很特殊 —— 把文件发我看看。")
        return 1
    for off, size, label, dest in sorted(items, key=lambda x: x[0]):
        print("  偏移 %9d  %-34s -> %s" % (off, label, dest))

    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return 0
    shots = []
    for off, size, label, dest in sorted(items, key=lambda x: x[0]):
        try:
            im = Image.open(dest)
            im.load()
            im = im.convert("RGB")
        except Exception:
            continue
        thumb = im.copy()
        thumb.thumbnail((320, 200))
        shots.append((label, thumb))
        if len(shots) >= 8:
            break
    if not shots:
        return 0
    width = sum(s.width + 10 for _, s in shots)
    height = max(s.height for _, s in shots) + 22
    sheet = Image.new("RGB", (width, height), (60, 60, 64))
    draw = ImageDraw.Draw(sheet)
    x = 5
    for label, thumb in shots:
        sheet.paste(thumb, (x, 20))
        draw.text((x, 5), label[:34], fill=(255, 255, 255))
        x += thumb.width + 10
    preview = os.path.join(outdir, "preview.png")
    sheet.save(preview)
    print("预览: %s" % preview)
    return 0


if __name__ == "__main__":
    sys.exit(main())
