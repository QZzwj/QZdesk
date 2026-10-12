#!/bin/sh
#
# MCP 工具：给设备屏幕拍一张快照。
#
# 直接读 framebuffer（QZDESK_FB，默认 /dev/fb0），转成 24 位 BMP 存到持久目录，
# 返回文件路径（用 adb pull / scp / samba 取走）。分辨率与像素格式都是运行时从
# /sys/class/graphics/fb0 读的，所以换屏、换色深都不用改脚本。
#
# 入参：{"name": "shot"}      可选，只允许 [A-Za-z0-9._-]，中文等会被折成 "screen"
# 出参：{"status":"ok","path":"/userdata/qzdesk/screenshots/screen-20261009-130501.bmp",...}
#
# 为什么用 python3：fb0 是 RGB565（LV_COLOR_DEPTH 16），要按 BGR 重排成 BMP，
# 用 shell 逐像素不现实；rootfs 里已经带了 python3（见 buildroot qzdesk_defconfig）。
set -u

json_str() {
	printf '%s' "$1" | sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" | head -1
}

body=$(cat)
name=$(json_str "$body" name)
# 只留 ASCII 安全字符：文件名要跨 shell / adb / samba，中文名容易在不同环节被转义
name=$(printf '%s' "$name" | tr -cd 'A-Za-z0-9._-')
name=$(printf '%s' "$name" | sed 's/^[.-]*//')     # 去掉开头的 . 与 -（避免隐藏文件、"./../" 这类前缀）
[ -n "$name" ] || name=screen

fb=${QZDESK_FB:-/dev/fb0}
[ -r "$fb" ] || { printf '读不到 framebuffer：%s（可用 QZDESK_FB 指定）\n' "$fb" >&2; exit 1; }

sysdir=/sys/class/graphics/$(basename "$fb")
bpp=$(cat "$sysdir/bits_per_pixel" 2>/dev/null)
size=$(cat "$sysdir/virtual_size" 2>/dev/null)
stride=$(cat "$sysdir/stride" 2>/dev/null)

# 有的驱动不暴露（或写错）这几个属性，允许显式覆盖：
#   QZDESK_FB_BPP=16  QZDESK_FB_SIZE=320x240  QZDESK_FB_STRIDE=640
bpp=${QZDESK_FB_BPP:-$bpp}
size=${QZDESK_FB_SIZE:-$size}
stride=${QZDESK_FB_STRIDE:-$stride}
size=$(printf '%s' "$size" | tr 'xX' ',')   # 覆盖值习惯写 320x240，sysfs 是 320,240

case "$size" in
	*,*) width=${size%%,*}; height=${size#*,} ;;
	*) printf '读不到 %s/virtual_size，无法确定分辨率（可用 QZDESK_FB_SIZE=320x240 指定）\n' "$sysdir" >&2; exit 1 ;;
esac
case "$width" in ''|*[!0-9]*) printf '分辨率不合法：%s\n' "$size" >&2; exit 1 ;; esac
case "$height" in ''|*[!0-9]*) printf '分辨率不合法：%s\n' "$size" >&2; exit 1 ;; esac
case "$bpp" in ''|*[!0-9]*) bpp=16 ;; esac
case "$stride" in ''|*[!0-9]*) stride=$(( width * bpp / 8 )) ;; esac

# 数据目录：与核心的 user_data.rs 同一约定 —— 设备上 /userdata/qzdesk（跨固件升级保留）
data_dir=${QZDESK_DATA_DIR:-}
if [ -z "$data_dir" ]; then
	if mkdir -p /userdata/qzdesk 2>/dev/null; then
		data_dir=/userdata/qzdesk
	elif mkdir -p "${HOME:-/tmp}/.local/share/qzdesk" 2>/dev/null; then
		data_dir="${HOME:-/tmp}/.local/share/qzdesk"
	else
		printf '找不到可写的数据目录（可用 QZDESK_DATA_DIR 指定）\n' >&2
		exit 1
	fi
fi
outdir=$data_dir/screenshots
mkdir -p "$outdir" || { printf '建不了目录：%s\n' "$outdir" >&2; exit 1; }
out=$outdir/$name-$(date +%Y%m%d-%H%M%S).bmp

command -v python3 >/dev/null 2>&1 || { printf '没有 python3（本工具用它做像素格式转换）\n' >&2; exit 1; }

python3 - "$fb" "$out" "$width" "$height" "$bpp" "$stride" <<'PY'
import struct
import sys

fb, out, width, height, bpp, stride = (
    sys.argv[1], sys.argv[2],
    int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6]),
)
row_bytes = ((width * 3 + 3) // 4) * 4          # BMP 每行 4 字节对齐
image_size = row_bytes * height
pad = b"\x00" * (row_bytes - width * 3)
pixel_bytes = width * bpp // 8

with open(fb, "rb", buffering=0) as source, open(out, "wb") as target:
    target.write(b"BM")
    target.write(struct.pack("<IHHI", 14 + 40 + image_size, 0, 0, 54))
    target.write(struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0,
                             image_size, 2835, 2835, 0, 0))
    for y in range(height - 1, -1, -1):          # BMP 是自下而上存的
        source.seek(y * stride)
        row = source.read(pixel_bytes)
        if len(row) < pixel_bytes:               # 读到短行（fb 尾部）用黑补齐
            row += b"\x00" * (pixel_bytes - len(row))
        if bpp == 16:
            # RGB565（小端）：R 高 5 位、G 中 6 位、B 低 5 位；展开到 8 位时把高位补进低位
            line = bytearray()
            for i in range(0, pixel_bytes, 2):
                value = row[i] | (row[i + 1] << 8)
                r = (value >> 11) & 0x1F
                g = (value >> 5) & 0x3F
                b = value & 0x1F
                line += bytes(((b << 3) | (b >> 2),
                               (g << 2) | (g >> 4),
                               (r << 3) | (r >> 2)))
        elif bpp == 32:
            # XRGB8888：内存顺序就是 B、G、R、X
            line = bytearray()
            for i in range(0, pixel_bytes, 4):
                line += row[i:i + 3]
        else:
            sys.stderr.write("不支持的 bits_per_pixel：%d\n" % bpp)
            sys.exit(1)
        target.write(bytes(line))
        target.write(pad)
PY

[ -s "$out" ] || { printf '截图失败：%s 没写出内容\n' "$out" >&2; exit 1; }

bytes=$(wc -c < "$out" 2>/dev/null || printf 0)
printf '{"status":"ok","path":"%s","width":%s,"height":%s,"bpp":%s,"bytes":%s}\n' \
	"$out" "$width" "$height" "$bpp" "$bytes"
