#!/usr/bin/env python3
"""把小智标准表情 GIF 缩放到界面需要的尺寸，并生成可嵌入固件的 C 数组。

素材来源：txp666/otto-emoji-gif-component（MIT）—— 21 个「小智标准」表情，
240×240、33 帧、原素材每帧 80ms。设备端用 LVGL 的 lv_gif 播放，GIF 字节直接
嵌在固件里（lv_gif 支持内存源），不需要在设备上放文件，也就不会出现"文件缺失
→ 表情空白"。默认重烘为每帧 160ms，让表情动作慢下来。

尺寸按**面板与调用点**预烘（见 DESIGNS）：三处脸在 480×320 与 320×240 上的
物理边长都各有一份 1:1 的 GIF，运行时一个像素都不缩放。以前只有 60/88/164
三档、由 lv_gif 现缩，边缘会被重采样糊出一圈毛边（320 上 76 设计像素的
脸 = 51 物理像素要缩掉 15%）。

素材本身是**不透明**的深蓝方块：LVGL 的 GIF 解码器不支持透明（把透明像素填
成背景色索引的颜色、alpha 给 0xff），把圆角烘进 GIF 没有任何效果，圆角由容器
的 clip_corner 负责（见 app/ai_face.c）。

用法：
    python3 tools/build_otto_emoji.py --src /path/to/otto-emoji-gif-component/gifs
    python3 tools/build_otto_emoji.py --src 界面比例/.../PNG          # 有 PNG 源
    python3 tools/build_otto_emoji.py --from-assets [--root 仓库根]  # 原始 GIF 不在时
    python3 tools/build_otto_emoji.py --from-assets --emit-only      # 只改映射表时

--from-assets 用 assets/mascot/otto 下已有的 1:1 GIF 反推尺寸（先放大再裁切，
只对"白圆点"这种简单图形成立），用于原素材丢失时重建，正常换表情请用 --src。

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
    ("QZ_FACE_RELAXED",   "relaxed"),
    ("QZ_FACE_CONFIDENT", "cool"),
    ("QZ_FACE_EMBARRASSED","embarrassed"),
    ("QZ_FACE_PLAYFUL",   "funny"),
    ("QZ_FACE_SAD",       "sad"),
]

# 设计尺寸 -> 档位名（枚举名在各面板尺寸前缀下唯一）。全部来自真实调用点：
# 关于卡（40 / 60）、主页 hero 头像（76）、对话页表情（160）。
DESIGNS = [
    (40, "40"),
    (60, "60"),
    (76, "76"),
    (160, "160"),
]
# 面板尺寸 -> 像素缩放分子（与 scale.h 的 qz_scale_px 一致：四舍五入）。
PANELS = [
    (320, 2, 3),   # 320×240 / 320×320 的宽度比
    (480, 1, 1),
]

# 反推源：--from-assets 时优先使用这些真实存在的历史/当前档位。
# 旧素材并不保证每个表情都有 88px（cool 等只有 60/107/160），所以不能再
# 假设固定三档；实际选择时会跳过不存在的文件。
SOURCE_TIERS = (60, 88, 107, 160, 164)
# 旧命名只用于清理时保留可继续作为底稿的文件。
LEGACY_TIERS = {"60", "88", "164"}

FRAME_MS = 160         # 原素材 80ms 的两倍，动作更从容（6.25fps）
PALETTE_COLORS = 16    # 量化级数：压体积，同时保住边缘抗锯齿
CROP_MARGIN = 1.04     # 内容外留 4%：脸撑满屏，眼睛就大，小尺寸才立得住
NAVY = (0x14, 0x27, 0x3C)   # 主题深蓝（QZ_TEXT 浅色值），替掉纯黑


def read_frames(path):
    """读所有帧（RGB）。GIF 多帧会保留透明角，这里只取画面内容。"""
    im = Image.open(path)
    frames = []
    for i in range(getattr(im, "n_frames", 1)):
        im.seek(i)
        frames.append(im.convert("RGB"))
    return frames


def retime_gif(path, out_path, frame_ms):
    """保持像素不变，只把已有 GIF 的每帧时长改成目标值。"""
    im = Image.open(path)
    frames = []
    for i in range(getattr(im, "n_frames", 1)):
        im.seek(i)
        # copy() 会把当前帧从延迟解码状态中取出来；源文件与输出相同时也能安全
        # 等到所有帧都读完之后再覆盖。
        frames.append(im.copy())
    frames[0].save(out_path, save_all=True, append_images=frames[1:],
                   duration=frame_ms, loop=0, optimize=True, disposal=2)
    return len(frames), os.path.getsize(out_path)


def rebuild_source(asset_dir, name, target_px):
    """从该表情实际存在的素材中挑一个最接近目标的源档。"""
    candidates = []
    for tier in SOURCE_TIERS:
        path = os.path.join(asset_dir, "%s-%d.gif" % (name, tier))
        if os.path.exists(path):
            candidates.append((abs(tier - target_px), tier, path))
    if candidates:
        candidates.sort()
        return candidates[0][1], candidates[0][2]

    # 兼容未来的素材命名：扫描该表情的所有 GIF，仍按尺寸距离选最近档。
    prefix = name + "-"
    for filename in os.listdir(asset_dir):
        if not filename.startswith(prefix) or not filename.endswith(".gif"):
            continue
        try:
            tier = int(filename[len(prefix):-4])
        except ValueError:
            continue
        path = os.path.join(asset_dir, filename)
        candidates.append((abs(tier - target_px), tier, path))
    if candidates:
        candidates.sort()
        return candidates[0][1], candidates[0][2]
    return None, None


def palette_from_navy(frame):
    """纯黑 -> 主题深蓝；其余灰度按亮度插值到白。"""
    out = frame.copy()
    pix = out.load()
    for y in range(out.height):
        for x in range(out.width):
            r, g, b = pix[x, y]
            if r < 20 and g < 20 and b < 20:
                pix[x, y] = NAVY
            else:
                t = max(r, g, b)
                pix[x, y] = tuple(int(NAVY[i] + (255 - NAVY[i]) * t / 255)
                                  for i in range(3))
    return out


def scale_frames(frames, size, out_path, frame_ms=FRAME_MS):
    """裁掉画面四周的黑边 -> 按目标尺寸重采样 -> 重存成 GIF。

    原素材是 240×240 的黑底白脸，四周留了大量黑边；直接缩到 88px 时脸只占
    中间一小块，看着比原来的脸小一圈。这里按"所有帧的内容并集"裁到方形（留
    CROP_MARGIN 的边距），脸就能撑满整块屏。背景归一成主题深蓝，和容器的
    底色严丝合缝。
    """
    n = len(frames)

    # 内容并集 bbox（亮度 > 16 视为"脸上的东西"）
    box = None
    for f in frames:
        b = f.convert("L").point(lambda v: 255 if v > 16 else 0).getbbox()
        if b is None:
            continue
        box = b if box is None else (min(box[0], b[0]), min(box[1], b[1]),
                                     max(box[2], b[2]), max(box[3], b[3]))
    if box is None:
        box = (0, 0, frames[0].width, frames[0].height)

    # 扩成方形 + 4% 边距
    cx, cy = (box[0] + box[2]) / 2.0, (box[1] + box[3]) / 2.0
    side = max(box[2] - box[0], box[3] - box[1]) * CROP_MARGIN
    side = min(side, min(frames[0].width, frames[0].height))
    half = side / 2.0
    crop = (max(0, int(round(cx - half))), max(0, int(round(cy - half))),
            min(frames[0].width, int(round(cx + half))),
            min(frames[0].height, int(round(cy + half))))

    out_frames = []
    for f in frames:
        g = f.crop(crop).resize((size, size), Image.LANCZOS)
        # 背景归一成深蓝（原素材是 1,1,1 这种近黑）
        g = g.point(lambda v: 0 if v < 20 else v)
        g = palette_from_navy(g)
        # 白脸黑底本来就没什么颜色，量化到 16 级灰能把体积压掉约一半，
        # 边缘的抗锯齿仍够平滑（8 级开始能在边缘看到台阶）。
        q = g.quantize(colors=PALETTE_COLORS, method=Image.MEDIANCUT)
        # 源 GIF 的 transparency 字段会跟着 palette 一起被继承，重存多帧时
        # PIL 会拿它去 convert(RGBA) 而报错；这里显式丢掉。
        q.info.pop("transparency", None)
        out_frames.append(q)

    # 这里**不做**圆角：素材存成整块深蓝，圆角交给容器的 clip_corner（app/ai_face.c）。
    # 烘透明角在设备上等于没做 —— LVGL 的 GIF 解码器不支持透明，它把"透明"像素填成
    # GIF 背景色索引的颜色、alpha 给 0xff（third_party/lvgl/src/libs/gif/gifdec.c
    # 初始化 canvas 那段），画出来的还是方块，角上的颜色还取决于量化后 0 号调色板
    # 是什么色。所以这里只保证背景是深蓝，形状的事一处都不掺和。
    out_frames[0].save(out_path, save_all=True, append_images=out_frames[1:],
                       duration=frame_ms, loop=0, optimize=True, disposal=2)
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
    ap.add_argument("--src", help="otto-emoji-gif-component 的 gifs/ 目录，或 PNG 源目录")
    ap.add_argument("--from-assets", action="store_true",
                    help="原素材不在时，用 assets/mascot/otto 下已有的 GIF 重建")
    ap.add_argument("--emit-only", action="store_true",
                    help="素材已是最新，只按当前映射重生成 C/头文件（不重烘、不改素材）")
    ap.add_argument("--frame-ms", type=int, default=FRAME_MS,
                    help="重烘后的 GIF 每帧时长，默认 %dms" % FRAME_MS)
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    args = ap.parse_args()

    if args.emit_only and args.src:
        sys.exit("--emit-only 只和 --from-assets 搭配使用")
    if not args.emit_only and bool(args.src) == bool(args.from_assets):
        sys.exit("--src 与 --from-assets 必须二选一")
    if not 20 <= args.frame_ms <= 1000:
        sys.exit("--frame-ms 必须在 20 到 1000 之间")

    root = args.root
    asset_dir = os.path.join(root, "assets", "mascot", "otto")
    os.makedirs(asset_dir, exist_ok=True)

    names = sorted({name for _, name in STATE_MAP})
    source_frames = {}     # name -> 原始帧；--from-assets 时下面按档读
    if not args.from_assets:
        missing = [n for n in names if not os.path.exists(os.path.join(args.src, n + ".gif"))]
        if missing:
            sys.exit("缺少素材: %s（在 %s 下找不到）" % (", ".join(missing), args.src))
        for name in names:
            source_frames[name] = read_frames(os.path.join(args.src, name + ".gif"))

    # 每个面板 × 每个设计尺寸的物理档位（去重：320 上 50/60 可能相同）。
    baked = {}             # (panel_w, name, label) -> px
    for panel_w, num, den in PANELS:
        for design, label in DESIGNS:
            px = (design * num + den // 2) // den
            baked[(panel_w, label)] = px
    px_set = sorted({v for v in baked.values()})
    print("物理档位 %d 个：%s" % (len(px_set), " ".join(str(v) for v in px_set)))

    # 1) 缩放并留档
    scaled = {}
    stale = []
    total = 0
    for name in names:
        for px in px_set:
            out = os.path.join(asset_dir, "%s-%d.gif" % (name, px))
            if args.emit_only:
                # 素材已经是按当前映射烘好的，直接读回来；重烘会从"已处理过的
                # 图"再缩一次，边距和抗锯齿会一轮轮漂掉。
                if not os.path.exists(out):
                    sys.exit("--emit-only 缺少 %s" % out)
                scaled[(name, px)] = open(out, "rb").read()
                total += os.path.getsize(out)
                continue
            if args.from_assets:
                # 取最近的一个真实存在的档位作底：小了会丢墨迹，大了只是多花
                # 一次重采样。同尺寸时只改帧时长，避免反复裁切让画面慢慢漂移。
                source_px, src_path = rebuild_source(asset_dir, name, px)
                if not src_path:
                    sys.exit("--from-assets 找不到 %s 的可用源图" % name)
                if source_px == px:
                    n, size = retime_gif(src_path, out, args.frame_ms)
                else:
                    frames = read_frames(src_path)
                    n, size = scale_frames(frames, px, out, args.frame_ms)
            else:
                frames = source_frames[name]
                n, size = scale_frames(frames, px, out, args.frame_ms)
            stale.append(out)
            scaled[(name, px)] = open(out, "rb").read()
            total += size
        if not args.emit_only:
            print("  %-11s %2d 档" % (name, len(px_set)))
    if args.emit_only:
        print("  复用 assets/mascot/otto 下已烘好的素材，未重烘")
    print("  ── %d 档 × %d 表情 = %.1f KB（嵌入固件的字节数）"
          % (len(px_set), len(names), total / 1024.0))
    if not args.emit_only:
        print("  播放节奏：每帧 %dms（约 %.1ffps）" %
              (args.frame_ms, 1000.0 / args.frame_ms))

    # 清掉上一轮跑出来的、这一轮不再需要的档位文件；旧命名（*-60/88/164.gif）
    # 是 --from-assets 的源底稿，保留。
    removed = 0
    if not args.emit_only:
        for fn in sorted(os.listdir(asset_dir)):
            if not fn.endswith(".gif"):
                continue
            if not any(fn.startswith(n + "-") for n in names):
                continue
            if os.path.join(asset_dir, fn) in stale:
                continue
            tail = fn[:-4].rsplit("-", 1)[-1]
            if tail in LEGACY_TIERS:
                continue
            os.remove(os.path.join(asset_dir, fn))
            removed += 1
    if removed:
        print("  清理过期档位 %d 个" % removed)

    # 2) 生成 C
    out_c = ['''/* 由 tools/build_otto_emoji.py 生成 —— 请勿手改。
 *
 * 素材：txp666/otto-emoji-gif-component（MIT License）的「小智标准」表情，
 * 原图 240×240、33 帧、每帧 80ms；这里按面板与调用点的真实尺寸各烘一份，
 * 并把播放节奏放慢到每帧 %dms。GIF 原始字节直接嵌进固件，运行时 1:1 显示、
 * 不再让 lv_gif 现缩。lv_gif 直接吃内存里的 GIF 流，设备上不需要放文件。
 *
 * 版权与署名见仓库根目录 NOTICE；换表情改 STATE_MAP 后重跑脚本即可。
 */
#include "otto_emoji.h"

#ifndef LV_ATTRIBUTE_MEM_ALIGN
    #define LV_ATTRIBUTE_MEM_ALIGN
#endif

/* ---------------- GIF 字节 ---------------- */
''' % args.frame_ms]

    for name in names:
        for px in px_set:
            data = scaled[(name, px)]
            out_c.append("/* %s.gif -> %dpx，%d 字节 */\nstatic const char gif_%s_%d[] =\n%s;\n\n"
                         % (name, px, len(data), name, px,
                            c_bytes_literal(data)))

    out_c.append("/* ---------------- 描述符与映射表 ---------------- */\n")
    for name in names:
        for px in px_set:
            out_c.append("""static const lv_image_dsc_t dsc_%s_%d = {
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.cf = LV_COLOR_FORMAT_RAW,   /* lv_gif 载入后会换成 ARGB8888 */
    .header.w = %d,
    .header.h = %d,
    .data_size = sizeof(gif_%s_%d) - 1, /* 去掉字符串结尾的 NUL */
    .data = (const uint8_t *)gif_%s_%d
};\n\n""" % (name, px, px, px, name, px, name, px))

    size_ids = []   # 按枚举顺序：(panel_w, label, px, enum 名)
    for panel_w, _, _ in PANELS:
        for _, label in DESIGNS:
            size_ids.append((panel_w, label, baked[(panel_w, label)],
                             "QZ_OTTO_%d_%s" % (panel_w, label)))

    out_c.append("static const lv_image_dsc_t *const table[QZ_FACE_STATE_COUNT]"
                 "[QZ_OTTO_SIZE_COUNT] = {\n")
    for state, name in STATE_MAP:
        row = ", ".join("&dsc_%s_%d" % (name, px) for _, _, px, _ in size_ids)
        out_c.append("    [%s] = { %s },\n" % (state, row))
    out_c.append("};\n\n")

    out_c.append('''int32_t qz_otto_px(qz_otto_size_t size)
{
    switch (size) {
''')
    for _, _, px, enum in size_ids:
        out_c.append("        case %s: return %d;\n" % (enum, px))
    out_c.append("        default: return %d;\n" % size_ids[-1][2])
    out_c.append('''    }
}

qz_otto_size_t qz_otto_size_for(int32_t panel_w, int32_t design_px)
{
    switch (panel_w) {
''')
    # 入参是设计像素，所以命中边界也必须是设计值的中点（50 / 68 / 118）；
    # 拿物理像素当边界会错档（320 面板上 76 设计像素烘成 51，60 设计像素烘成
    # 40，边界若写成 40/50/68 会把 76 逼到 160 档，脸一下大出一倍）。
    designs = [d for d, _ in DESIGNS]
    bounds = [(designs[i] + designs[i + 1]) // 2 for i in range(len(designs) - 1)]
    for panel_w, _, _ in PANELS:
        out_c.append("        case %d:\n" % panel_w)
        for i, bound in enumerate(bounds):
            out_c.append("            if (design_px <= %d) return QZ_OTTO_%d_%s;\n"
                         % (bound, panel_w, DESIGNS[i][1]))
        out_c.append("            return QZ_OTTO_%d_%s;\n" % (panel_w, DESIGNS[-1][1]))
    out_c.append("        default: break;\n")
    out_c.append("    }\n")
    # 未登记的面板宽度：按设计尺寸给 480 档兜底（宁可大一点，别让 lv_gif 现缩）。
    out_c.append("    switch (design_px) {\n")
    for design, label in DESIGNS[:-1]:
        out_c.append("        case %d: return QZ_OTTO_480_%s;\n" % (design, label))
    out_c.append("        default: return QZ_OTTO_480_%s;\n" % DESIGNS[-1][1])
    out_c.append("    }\n}\n\n")

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
    if (size >= QZ_OTTO_SIZE_COUNT) size = QZ_OTTO_480_160;
    return table[state][size];
}
''')

    with open(os.path.join(root, "app", "otto_emoji.c"), "w", encoding="utf-8") as fh:
        fh.write("".join(out_c))

    header = '''/* 小智标准表情（GIF）资源接口 —— 生成物，见 tools/build_otto_emoji.py。
 *
 * 素材：txp666/otto-emoji-gif-component（MIT）。21 个表情里取用 15 个，按
 * 面板（320 / 480）与界面调用点的设计尺寸各烘一份 1:1 资源。
 */
#ifndef QZ_OTTO_EMOJI_H
#define QZ_OTTO_EMOJI_H

#include "lvgl.h"
#include "ai_face.h"

/** 尺寸档：面板宽度 + 设计尺寸（480×320 设计稿里的像素）。 */
typedef enum {
'''
    for _, _, _, enum in size_ids:
        header += "    %s,\n" % enum
    header += '''    QZ_OTTO_SIZE_COUNT
} qz_otto_size_t;

/** 该档位的边长（物理像素）：容器按它定尺寸，图案 1:1 不再缩放。 */
int32_t qz_otto_px(qz_otto_size_t size);

/** 面板宽度 + 设计像素 -> 尺寸档（与 scale.h 的 qz_scale_px 同一套四舍五入）。 */
qz_otto_size_t qz_otto_size_for(int32_t panel_w, int32_t design_px);

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
