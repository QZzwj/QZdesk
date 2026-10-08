#!/usr/bin/env python3
"""把小智标准表情 GIF 缩放到界面需要的尺寸，并生成可嵌入固件的 C 数组。

素材来源：txp666/otto-emoji-gif-component（MIT）—— 21 个「小智标准」表情，
240×240、33 帧、每帧 80ms。设备端用 LVGL 的 lv_gif 播放，GIF 字节直接嵌在
固件里（lv_gif 支持内存源），不需要在设备上放文件，也就不会出现"文件缺失 →
表情空白"。

用法：
    python3 tools/build_otto_emoji.py --src /path/to/otto-emoji-gif-component/gifs
输出：
    app/otto_emoji.c      生成的 C 数组与状态映射表（勿手改）
    include/otto_emoji.h  对外接口
    assets/mascot/otto/   缩放后的 GIF 原件（留档、便于查看与再生成）
"""

import argparse
import os
import sys
from PIL import Image

# 状态 -> 表情名。改这里就能换表情，重跑脚本即可（生成物会同步）。
STATE_MAP = [
    ("QZ_FACE_IDLE",      "neutral"),
    ("QZ_FACE_SPEAKING",  "happy"),
    ("QZ_FACE_THINKING",  "thinking"),
    ("QZ_FACE_HAPPY",     "laughing"),
    ("QZ_FACE_CONFUSED",  "confused"),
    ("QZ_FACE_LOVE",      "loving"),
    ("QZ_FACE_SURPRISED", "surprised"),
    ("QZ_FACE_SLEEPY",    "sleepy"),
    ("QZ_FACE_WINK",      "winking"),
    ("QZ_FACE_EXCITED",   "silly"),
]

# 尺寸档：名字 -> 像素（跟界面上的三处脸一一对应）
SIZES = [("60", 60), ("88", 88), ("164", 164)]
FRAME_MS = 80          # 原素材节奏，保持不变（12.5fps）
PALETTE_COLORS = 16    # 量化级数：压体积，同时保住边缘抗锯齿


def scale_gif(src_path, size, out_path):
    """裁掉画面四周的黑边 -> 按目标尺寸重采样 -> 重存成 GIF。

    原素材是 240×240 的黑底白脸，四周留了大量黑边；直接缩到 88px 时脸只占
    中间一小块，看着比原来的脸小一圈。这里按"所有帧的内容并集"裁到方形（留
    一点边距），脸就能撑满整块屏。背景顺手归一成纯黑，和我们画的黑底严丝合缝。
    """
    im = Image.open(src_path)
    n = getattr(im, "n_frames", 1)
    frames = []
    for i in range(n):
        im.seek(i)
        frames.append(im.convert("RGB"))

    # 内容并集 bbox（亮度 > 16 视为"脸上的东西"）
    box = None
    for f in frames:
        b = f.convert("L").point(lambda v: 255 if v > 16 else 0).getbbox()
        if b is None:
            continue
        box = b if box is None else (min(box[0], b[0]), min(box[1], b[1]),
                                     max(box[2], b[2]), max(box[3], b[3]))
    if box is None:
        box = (0, 0, im.width, im.height)

    # 扩成方形 + 8% 边距
    cx, cy = (box[0] + box[2]) / 2.0, (box[1] + box[3]) / 2.0
    side = max(box[2] - box[0], box[3] - box[1]) * 1.08
    side = min(side, min(im.width, im.height))
    half = side / 2.0
    crop = (max(0, int(round(cx - half))), max(0, int(round(cy - half))),
            min(im.width, int(round(cx + half))), min(im.height, int(round(cy + half))))

    out_frames = []
    for f in frames:
        g = f.crop(crop).resize((size, size), Image.LANCZOS)
        # 背景归一成纯黑（原素材是 1,1,1 这种近黑）
        g = g.point(lambda v: 0 if v < 20 else v)
        # 白脸黑底本来就没什么颜色，量化到 16 级灰能把体积压掉约一半，
        # 边缘的抗锯齿仍够平滑（8 级开始能在边缘看到台阶）。
        q = g.quantize(colors=PALETTE_COLORS, method=Image.MEDIANCUT)
        # 源 GIF 的 transparency 字段会跟着 palette 一起被继承，重存多帧时
        # PIL 会拿它去 convert(RGBA) 而报错；这里显式丢掉。
        q.info.pop("transparency", None)
        out_frames.append(q)
    out_frames[0].save(out_path, save_all=True, append_images=out_frames[1:],
                       duration=FRAME_MS, loop=0, optimize=True, disposal=2)
    return n, os.path.getsize(out_path)


def c_bytes_literal(data, indent="    ", per_line=32):
    """把字节串写成 C 字符串字面量（每字节 \\xHH，按行拼接）。"""
    lines = []
    for i in range(0, len(data), per_line):
        chunk = data[i:i + per_line]
        lines.append(indent + '"' + "".join("\\x%02x" % b for b in chunk) + '"')
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="otto-emoji-gif-component 的 gifs/ 目录")
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    args = ap.parse_args()

    root = args.root
    src_dir = args.src
    asset_dir = os.path.join(root, "assets", "mascot", "otto")
    os.makedirs(asset_dir, exist_ok=True)

    names = sorted({name for _, name in STATE_MAP})
    missing = [n for n in names if not os.path.exists(os.path.join(src_dir, n + ".gif"))]
    if missing:
        sys.exit("缺少素材: %s（在 %s 下找不到）" % (", ".join(missing), src_dir))

    # 1) 缩放并留档
    scaled = {}
    total = 0
    for name in names:
        for label, px in SIZES:
            out = os.path.join(asset_dir, "%s-%s.gif" % (name, label))
            n, size = scale_gif(os.path.join(src_dir, name + ".gif"), px, out)
            scaled[(name, label)] = open(out, "rb").read()
            total += size
            print("  %-11s %3spx  %2d 帧  %6.1f KB" % (name, label, n, size / 1024.0))
    print("  ── 三档合计 %.1f KB（嵌入固件的字节数）" % (total / 1024.0))

    # 2) 生成 C
    out_c = ['''/* 由 tools/build_otto_emoji.py 生成 —— 请勿手改。
 *
 * 素材：txp666/otto-emoji-gif-component（MIT License）的「小智标准」表情，
 * 原图 240×240、33 帧、每帧 80ms；这里按界面需要缩放到 60/88/164 三档并把
 * GIF 原始字节嵌进固件。lv_gif 直接吃内存里的 GIF 流，设备上不需要放文件。
 *
 * 版权与署名见仓库根目录 NOTICE；换表情改 STATE_MAP 后重跑脚本即可。
 */
#include "otto_emoji.h"

#ifndef LV_ATTRIBUTE_MEM_ALIGN
    #define LV_ATTRIBUTE_MEM_ALIGN
#endif

/* ---------------- GIF 字节 ---------------- */
''']

    for name in names:
        for label, px in SIZES:
            data = scaled[(name, label)]
            out_c.append("/* %s.gif -> %spx，%d 字节 */\nstatic const char gif_%s_%s[] =\n%s;\n\n"
                         % (name, label, len(data), name, label,
                            c_bytes_literal(data)))

    out_c.append("/* ---------------- 描述符与映射表 ---------------- */\n")
    for name in names:
        for label, px in SIZES:
            out_c.append("""static const lv_image_dsc_t dsc_%s_%s = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_RAW,   /* lv_gif 载入后会换成 ARGB8888 */
    .header.w = %d,
    .header.h = %d,
    .data_size = sizeof(gif_%s_%s) - 1, /* 去掉字符串结尾的 NUL */
    .data = (const uint8_t *)gif_%s_%s
};\n\n""" % (name, label, px, px, name, label, name, label))

    out_c.append("static const lv_image_dsc_t *const table[QZ_FACE_STATE_COUNT][QZ_OTTO_SIZE_COUNT] = {\n")
    for state, name in STATE_MAP:
        row = ", ".join("&dsc_%s_%s" % (name, label) for label, _ in SIZES)
        out_c.append("    [%s] = { %s },\n" % (state, row))
    out_c.append("};\n\n")

    px_list = [px for _, px in SIZES]
    out_c.append('''int32_t qz_otto_px(qz_otto_size_t size)
{
    switch (size) {
        case QZ_OTTO_60: return %d;
        case QZ_OTTO_88: return %d;
        case QZ_OTTO_164: return %d;
        default: return %d;
    }
}

''' % (px_list[0], px_list[1], px_list[2], px_list[1]))

    out_c.append('''const char *qz_otto_emoji_name(qz_face_state_t state)
{
    switch (state) {
''')
    for state, name in STATE_MAP:
        out_c.append('        case %s: return "%s";\n' % (state, name))
    out_c.append('''        default: return "neutral";
    }
}

const lv_image_dsc_t *qz_otto_emoji(qz_face_state_t state, qz_otto_size_t size)
{
    if (state >= QZ_FACE_STATE_COUNT) state = QZ_FACE_IDLE;
    if (size >= QZ_OTTO_SIZE_COUNT) size = QZ_OTTO_88;
    return table[state][size];
}
''')

    with open(os.path.join(root, "app", "otto_emoji.c"), "w", encoding="utf-8") as fh:
        fh.write("".join(out_c))

    header = '''/* 小智标准表情（GIF）资源接口 —— 生成物，见 tools/build_otto_emoji.py。
 *
 * 素材：txp666/otto-emoji-gif-component（MIT）。21 个表情里取用 10 个，
 * 按界面上的三处脸缩放到 60/88/164。
 */
#ifndef QZ_OTTO_EMOJI_H
#define QZ_OTTO_EMOJI_H

#include "lvgl.h"
#include "ai_face.h"

/** 尺寸档：跟界面上的三处脸一一对应。 */
typedef enum {
    QZ_OTTO_60 = 0,    /**< 关于卡片（60px） */
    QZ_OTTO_88 = 1,    /**< 主页大卡片（88px） */
    QZ_OTTO_164 = 2,   /**< 全屏表情（164px） */
    QZ_OTTO_SIZE_COUNT
} qz_otto_size_t;

/** 该档位的边长（物理像素）：容器按它定尺寸，图案 1:1 不再缩放。 */
int32_t qz_otto_px(qz_otto_size_t size);

/** 取某个状态在某个尺寸档下的 GIF 描述符（内存源，直接喂 lv_gif）。 */
const lv_image_dsc_t *qz_otto_emoji(qz_face_state_t state, qz_otto_size_t size);

/** 状态对应的表情名（neutral / happy / thinking …），调试与日志用。 */
const char *qz_otto_emoji_name(qz_face_state_t state);

#endif /* QZ_OTTO_EMOJI_H */
'''
    with open(os.path.join(root, "include", "otto_emoji.h"), "w", encoding="utf-8") as fh:
        fh.write(header)

    c_size = os.path.getsize(os.path.join(root, "app", "otto_emoji.c"))
    print("已生成 app/otto_emoji.c（%.1f MB 源码）、include/otto_emoji.h、assets/mascot/otto/"
          % (c_size / 1024.0 / 1024.0))
    print("状态映射：")
    for state, name in STATE_MAP:
        print("  %-18s -> %s" % (state, name))


if __name__ == "__main__":
    main()
