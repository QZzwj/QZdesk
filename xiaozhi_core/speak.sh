#!/bin/sh
#
# MCP 工具：让设备用自己的扬声器念一段话（本地播报）。
#
# 交给本机核心的 HTTP 控制台（默认 127.0.0.1:8080，QZDESK_SKILL_WEB_PORT 可覆盖）：
#   POST /api/speak      {"text":"..."}  讯飞 TTS 合成后本地播放（首选，内容可控）
#   POST /api/chat/send  {"text":"..."}  该接口还不存在时退回：让云端 AI 回答
#                        （会慢一点，而且念出来的是 AI 的回答，不完全等于原文）
#
# 入参：{"text": "该喝水了"}     也接受 message / content
# 出参：{"status":"ok","text":"...","via":"/api/speak","http":200}
#
# 整个请求用 python3 发：中文要按字节原样送出去，shell 变量 + sed 在 LC_ALL=C 的
# 环境里会把 UTF-8 弄坏。入参 JSON 也从 stdin 读，避免走 argv 的字符集转换。
set -u

port=${QZDESK_SKILL_WEB_PORT:-${XIAOZHI_SKILL_WEB_PORT:-8080}}
command -v python3 >/dev/null 2>&1 || { printf '没有 python3（本工具用它发本地 HTTP 请求）\n' >&2; exit 1; }

body=$(cat)
export QZDESK_SPEAK_BODY="$body"

python3 - "$port" <<'PY'
import json
import os
import sys
import urllib.error
import urllib.request

port = sys.argv[1]
# 一定要按字节取环境变量：设备上核心的 locale 往往是 C，os.environ 会用 ascii 解码，
# 中文会变成代理字符，再 encode("utf-8") 直接抛 UnicodeEncodeError。
raw = os.environb.get(b"QZDESK_SPEAK_BODY", b"")
try:
    # json.loads 认 bytes，非 UTF-8 之外都能自己判编码
    params = json.loads(raw) if raw.strip() else {}
except ValueError as error:
    sys.stderr.write("入参不是合法 JSON：%s\n" % error)
    sys.exit(1)
if not isinstance(params, dict):
    sys.stderr.write("入参必须是 JSON 对象\n")
    sys.exit(1)

text = (params.get("text") or params.get("message") or params.get("content") or "").strip()
if not text:
    sys.stderr.write("缺少 text：要念的内容\n")
    sys.exit(1)

# 本地回环请求绝不能被代理：环境里残留的 http_proxy 会把 127.0.0.1 也代理走，
# 表现是核心明明在监听却报 "Connection refused"（这台开发机上就有这个坑）。
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

def post(path, payload):
    request = urllib.request.Request(
        "http://127.0.0.1:%s%s" % (port, path),
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    return opener.open(request, timeout=25).status

via = "/api/speak"
try:
    status = post(via, {"text": text})
except urllib.error.HTTPError as error:
    body = error.read().decode("utf-8", "replace")
    if error.code != 404:
        sys.stderr.write("核心返回 %s：%s\n" % (error.code, body))
        sys.exit(1)
    # /api/speak 还没有（旧核心）：退回文字聊天，至少能出声
    via = "/api/chat/send"
    try:
        status = post(via, {"text": text})
    except urllib.error.HTTPError as fallback:
        sys.stderr.write("核心返回 %s：%s\n"
                         % (fallback.code, fallback.read().decode("utf-8", "replace")))
        sys.exit(1)
    except Exception as error:
        sys.stderr.write("请求核心失败：%s\n" % error)
        sys.exit(1)
except Exception as error:
    sys.stderr.write("连不上核心控制台（127.0.0.1:%s）：%s\n" % (port, error))
    sys.exit(1)

# ensure_ascii 默认 True：输出保持纯 ASCII，不依赖设备的 locale
sys.stdout.write(json.dumps({"status": "ok", "text": text, "via": via, "http": status}))
sys.stdout.write("\n")
PY
